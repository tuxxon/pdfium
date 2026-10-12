// Copyright 2026 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/fpdfapi/edit/cpdf_sourcecontentremover.h"

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <utility>
#include <vector>

#include "core/fpdfapi/font/cpdf_font.h"
#include "core/fpdfapi/page/cpdf_form.h"
#include "core/fpdfapi/page/cpdf_formobject.h"
#include "core/fpdfapi/page/cpdf_page.h"
#include "core/fpdfapi/page/cpdf_sourcecontent.h"
#include "core/fpdfapi/page/cpdf_streamparser.h"
#include "core/fpdfapi/page/cpdf_textobject.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfapi/parser/cpdf_stream_acc.h"
#include "core/fpdfapi/parser/fpdf_parser_decode.h"
#include "core/fpdfapi/parser/fpdf_parser_utility.h"
#include "core/fxcrt/data_vector.h"
#include "core/fxcrt/span.h"

namespace {
using Result = CPDF_SourceTextRemovalResult;

bool HasDirtyObjects(const CPDF_PageObjectHolder* holder, size_t depth = 0) {
  if (depth > 64 || holder->HasDirtyStreams()) {
    return true;
  }
  for (const auto& object : *holder) {
    if (object->IsDirty() || !object->IsActive()) {
      return true;
    }
    const auto* form = object->AsForm();
    if (form && HasDirtyObjects(form->form(), depth + 1)) {
      return true;
    }
  }
  return false;
}

std::vector<RetainPtr<const CPDF_Stream>> GetContentStreams(CPDF_Page* page) {
  std::vector<RetainPtr<const CPDF_Stream>> result;
  auto contents = page->GetDict()->GetDirectObjectFor("Contents");
  if (!contents) {
    return result;
  }
  if (contents->IsStream()) {
    result.push_back(pdfium::WrapRetain(contents->AsStream()));
    return result;
  }
  const auto* array = contents->AsArray();
  if (!array) {
    return {};
  }
  for (size_t i = 0; i < array->size(); ++i) {
    auto stream = array->GetStreamAt(i);
    if (!stream) {
      return {};
    }
    result.push_back(std::move(stream));
  }
  return result;
}

// The offset space is exactly the one used by CPDF_ContentParser. Reject a
// show whose operands/operator cross streams, including artificial separators.
std::optional<size_t> FindStream(const CPDF_SourceContent& source,
                                 const CPDF_SourceContent::Operation& op) {
  if (op.begin >= op.end) {
    return std::nullopt;
  }
  for (size_t i = 0; i < source.streams.size(); ++i) {
    const auto& stream = source.streams[i];
    if (op.begin >= stream.offset &&
        static_cast<uint64_t>(op.end) <=
            static_cast<uint64_t>(stream.offset) + stream.decoded.size()) {
      return i;
    }
  }
  return std::nullopt;
}

constexpr size_t kMaxXObjectDepth = 16;

bool IsTextShow(ByteStringView op) {
  return op == "Tj" || op == "TJ" || op == "'" || op == "\"";
}

// ISO 32000-1 Table A.1. Other viewers split or alias unknown keywords, e.g.
// pdf.js reads "Tjq" as Tj followed by q.
bool IsKnownOperator(ByteStringView op) {
  static constexpr auto kOperators = std::to_array<const char*>({
      "b",  "B",  "b*", "B*", "BDC", "BI",  "BMC", "BT",  "BX", "c",  "cm",
      "CS", "cs", "d",  "d0", "d1",  "Do",  "DP",  "EI",  "EMC", "ET", "EX",
      "f",  "F",  "f*", "G",  "g",   "gs",  "h",   "i",   "ID", "j",  "J",
      "K",  "k",  "l",  "m",  "M",   "MP",  "n",   "q",   "Q",  "re", "RG",
      "rg", "ri", "s",  "S",  "SC",  "sc",  "SCN", "scn", "sh", "T*", "Tc",
      "Td", "TD", "Tf", "Tj", "TJ",  "TL",  "Tm",  "Tr",  "Ts", "Tw", "Tz",
      "v",  "w",  "W",  "W*", "y",   "'",   "\""});
  return std::ranges::any_of(kOperators,
                             [op](const char* known) { return op == known; });
}

enum class CursorStep { kOk, kReadsTaint, kMalformed };

// Whether the text position may still depend on a removed show (or, inside a
// Form, on the caller), under two models that must both be clean:
//  - PDFium, pdf.js and poppler save the position with q, restore it with Q,
//    keep it after ET and pass it to Forms.
//  - ISO 32000 keeps the text matrices out of the graphics state: Q does not
//    restore them and they end at ET (task 28, tuxxon/pdfium 1ae4821).
class CursorTaint {
 public:
  explicit CursorTaint(bool initially_tainted) : tainted_(initially_tainted) {}

  // `removed` marks a show whose bytes are blanked.
  CursorStep Apply(ByteStringView op, bool removed) {
    if (op == "q") {
      saved_.push_back(tainted_);
    } else if (op == "Q") {
      if (saved_.empty()) {
        return CursorStep::kMalformed;
      }
      tainted_ = saved_.back();
      saved_.pop_back();
    } else if (op == "BT") {
      if (in_text_) {
        return CursorStep::kMalformed;
      }
      in_text_ = true;
      tainted_ = false;
    } else if (op == "ET") {
      if (!in_text_) {
        return CursorStep::kMalformed;
      }
      in_text_ = false;
      // The spec text matrices end here; PDFium keeps the position.
      spec_tainted_ = false;
    } else if (op == "Tm" || op == "Td" || op == "TD" || op == "T*") {
      tainted_ = false;
      spec_tainted_ = false;
    } else if (IsTextShow(op)) {
      if (!in_text_) {
        return CursorStep::kMalformed;
      }
      if (removed) {
        // Only plain Tj/TJ pass selection validation. Neither changes the line
        // position; mark their current-text-position effect as unknown.
        tainted_ = true;
        spec_tainted_ = true;
      } else {
        // Retained quote operators first reset position from the text line.
        if (op == "'" || op == "\"") {
          tainted_ = false;
          spec_tainted_ = false;
        }
        if (tainted()) {
          return CursorStep::kReadsTaint;
        }
      }
    }
    return CursorStep::kOk;
  }

  bool Balanced() const { return !in_text_ && saved_.empty(); }
  bool tainted() const { return tainted_ || spec_tainted_; }
  bool in_text() const { return in_text_; }

 private:
  bool tainted_;
  bool spec_tainted_ = false;
  bool in_text_ = false;
  std::vector<bool> saved_;
};

bool XObjectIgnoresTextPosition(const CPDF_Dictionary* resources,
                                const ByteString& name,
                                size_t depth);

// A Do while the position is tainted is safe only outside a text object, where
// ISO 32000 allows it, and only for an XObject that ignores the position.
bool TaintedDoIsSafe(const CursorTaint& cursor,
                     const CPDF_Dictionary* resources,
                     const std::optional<ByteString>& name,
                     size_t depth) {
  return !cursor.in_text() && name &&
         XObjectIgnoresTextPosition(resources, *name, depth);
}

// Whether painting XObject `name` from `resources` cannot read the caller's
// text position. PDFium starts a Form with a fresh text position (AddForm
// copies only the general, graph, color and text states), but pdf.js and
// poppler pass it on and the saved PDF is opened by them too. So accept only
// an Image, or a Form whose shows never read the inherited position and whose
// nested XObjects pass the same test. Cycles end at the depth limit.
bool XObjectIgnoresTextPosition(const CPDF_Dictionary* resources,
                                const ByteString& name,
                                size_t depth) {
  if (depth > kMaxXObjectDepth || !resources) {
    return false;
  }
  // No fallback to the page's XObjects: PDFium and poppler fall back
  // differently.
  RetainPtr<const CPDF_Dictionary> xobjects = resources->GetDictFor("XObject");
  RetainPtr<const CPDF_Stream> xobject =
      xobjects ? ToStream(xobjects->GetDirectObjectFor(name.AsStringView()))
               : nullptr;
  if (!xobject) {
    return false;
  }
  RetainPtr<const CPDF_Dictionary> dict = xobject->GetDict();
  const ByteString subtype = dict->GetByteStringFor("Subtype");
  if (subtype == "Image") {
    return true;
  }
  // Other viewers read an external /F stream instead of these bytes.
  if (subtype != "Form" || dict->KeyExist("F")) {
    return false;
  }
  const std::optional<DecoderArray> decoders = GetDecoderArray(dict);
  if (!decoders) {
    return false;
  }
  auto acc = pdfium::MakeRetain<CPDF_StreamAcc>(xobject);
  acc->LoadAllDataFiltered();
  if (!decoders->empty()) {
    // A failed filter leaves PDFium the raw bytes; other viewers may decode
    // more.
    auto raw = pdfium::MakeRetain<CPDF_StreamAcc>(xobject);
    raw->LoadAllDataRaw();
    if (std::ranges::equal(acc->GetSpan(), raw->GetSpan())) {
      return false;
    }
  }
  RetainPtr<const CPDF_Dictionary> form_resources = dict->GetDictFor("Resources");
  if (!form_resources) {
    form_resources = pdfium::WrapRetain(resources);
  }
  CPDF_StreamParser parser(acc->GetSpan());
  CursorTaint cursor(/*initially_tainted=*/true);
  // PDFium paints a Do's last operand; check it only when it is a name.
  std::optional<ByteString> operand;
  while (true) {
    const auto type = parser.ParseNextElement();
    if (type == CPDF_StreamParser::kEndOfData) {
      break;
    }
    if (type != CPDF_StreamParser::kKeyword) {
      // A null object is a malformed dictionary or array, which other parsers
      // resynchronize differently.
      if (type == CPDF_StreamParser::kOther && !parser.GetObject()) {
        return false;
      }
      operand.reset();
      if (type == CPDF_StreamParser::kName) {
        operand = PDF_NameDecode(parser.GetWord().Substr(1));
      }
      continue;
    }
    const ByteStringView word = parser.GetWord();
    // Inline image data cannot be skipped without the image parser.
    if (word == "BI" || !IsKnownOperator(word) ||
        cursor.Apply(word, /*removed=*/false) != CursorStep::kOk) {
      return false;
    }
    if (word == "Do" && cursor.tainted() &&
        !TaintedDoIsSafe(cursor, form_resources.Get(), operand, depth + 1)) {
      return false;
    }
    operand.reset();
  }
  // Painting wraps the Form in q/Q, so leftover q or BT cannot reach the caller.
  return true;
}

// The XObject name of a top-level Do. `valid_operands` already guarantees a
// lone name operand, so this check and PDFium agree on the painted XObject.
std::optional<ByteString> DoOperand(const CPDF_SourceContent& source,
                                    const CPDF_SourceContent::Operation& op) {
  const auto stream_index = FindStream(source, op);
  if (!stream_index) {
    return std::nullopt;
  }
  const auto& stream = source.streams[*stream_index];
  CPDF_StreamParser parser(pdfium::span(stream.decoded)
                               .subspan(op.begin - stream.offset,
                                        op.end - op.begin));
  if (parser.ParseNextElement() != CPDF_StreamParser::kName) {
    return std::nullopt;
  }
  return PDF_NameDecode(parser.GetWord().Substr(1));
}

Result ValidateCursorDependencies(const CPDF_Page& page,
                                  const CPDF_SourceContent& source,
                                  const std::set<size_t>& selected) {
  CursorTaint cursor(/*initially_tainted=*/false);
  for (const auto& op : source.operations) {
    const bool removed = op.text_object_index &&
                         selected.contains(*op.text_object_index);
    if (!op.valid_operands) {
      return Result::kUnsupportedContent;
    }
    const CursorStep step = cursor.Apply(op.name.AsStringView(), removed);
    if (step == CursorStep::kMalformed) {
      return Result::kUnsupportedContent;
    }
    if (step == CursorStep::kReadsTaint) {
      return Result::kTextPositionDependency;
    }
    if (op.name == "Do" && cursor.tainted() &&
        !TaintedDoIsSafe(cursor, page.GetResources().Get(),
                         DoOperand(source, op), /*depth=*/0)) {
      return Result::kTextPositionDependency;
    }
  }
  return cursor.Balanced() ? Result::kSuccess : Result::kUnsupportedContent;
}
}  // namespace

Result CPDF_RemoveOwnedTextPreservingStreams(
    CPDF_Page* page,
    pdfium::span<CPDF_TextObject* const> owned_text) {
  if (!page || !page->GetDocument() || owned_text.empty() ||
      owned_text.size() > 4096) {
    return Result::kInvalidSelection;
  }
  auto* source = page->GetSourceContent();
  if (!source || !source->complete || !source->valid ||
      page->GetParseState() != CPDF_PageObjectHolder::ParseState::kParsed) {
    return Result::kNoSourceProvenance;
  }
  if (source->consumed || HasDirtyObjects(page) ||
      page->GetPageObjectCount() != source->object_identities.size()) {
    return Result::kPageModified;
  }

  std::map<const CPDF_TextObject*, size_t> native_text_indices;
  for (size_t i = 0; i < page->GetPageObjectCount(); ++i) {
    const auto* object = page->GetPageObjectByIndex(i);
    if (object->IsDirty() || !object->IsActive() ||
        reinterpret_cast<uintptr_t>(object) != source->object_identities[i]) {
      return Result::kPageModified;
    }
    if (object->IsText()) {
      native_text_indices.emplace(object->AsText(), i);
    }
  }

  std::set<size_t> selected;
  for (const auto* text : owned_text) {
    auto found = native_text_indices.find(text);
    if (found == native_text_indices.end() ||
        !selected.insert(found->second).second) {
      return Result::kInvalidSelection;
    }
    if (text->GetTextRenderMode() != TextRenderingMode::MODE_FILL ||
        !text->GetFont() || text->GetFont()->IsType3Font()) {
      return Result::kUnsupportedText;
    }
  }

  auto current_streams = GetContentStreams(page);
  if (current_streams.size() != source->streams.size() ||
      current_streams.empty()) {
    return Result::kSourceChanged;
  }
  for (size_t i = 0; i < current_streams.size(); ++i) {
    const auto& original = source->streams[i];
    if (current_streams[i].Get() != original.identity.Get() ||
        current_streams[i]->GetObjNum() == 0) {
      return Result::kSourceChanged;
    }
    // External stream data and semantic stream dictionaries are not handled.
    // Ordinary Contents dictionaries may retain arbitrary unrelated entries.
    if (current_streams[i]->GetDict()->KeyExist("F")) {
      return Result::kUnsupportedContent;
    }
    auto acc = pdfium::MakeRetain<CPDF_StreamAcc>(current_streams[i]);
    acc->LoadAllDataFiltered();
    if (!std::ranges::equal(acc->GetSpan(), original.decoded)) {
      return Result::kSourceChanged;
    }
  }

  std::map<size_t, DataVector<uint8_t>> replacements;
  std::set<size_t> matched;
  uint32_t last_end = 0;
  for (const auto& op : source->operations) {
    if (!op.text_object_index || !selected.contains(*op.text_object_index)) {
      continue;
    }
    if ((op.name != "Tj" && op.name != "TJ") || !op.valid_operands) {
      return Result::kUnsupportedText;
    }
    if (!matched.insert(*op.text_object_index).second || op.begin < last_end) {
      return Result::kNoSourceProvenance;
    }
    const auto stream_index = FindStream(*source, op);
    if (!stream_index) {
      return Result::kCrossStreamOperator;
    }
    const auto& original = source->streams[*stream_index];
    const auto* object = page->GetPageObjectByIndex(*op.text_object_index);
    // PDFium's GetCurrentStreamIndex uses the cursor after the operator. A
    // final operator ending at EOF still belongs to this original stream.
    if (object->GetContentStream() != static_cast<int32_t>(*stream_index)) {
      return Result::kNoSourceProvenance;
    }
    auto it = replacements.try_emplace(*stream_index, original.decoded).first;
    auto bytes = pdfium::span(it->second).subspan(
        op.begin - original.offset, op.end - op.begin);
    std::ranges::fill(bytes, static_cast<uint8_t>(' '));
    last_end = op.end;
  }
  if (matched != selected) {
    return Result::kNoSourceProvenance;
  }
  const Result dependencies =
      ValidateCursorDependencies(*page, *source, selected);
  if (dependencies != Result::kSuccess) {
    return dependencies;
  }

  // Commit only after every selected object, source stream, and cursor effect
  // has been validated. No existing indirect stream or array is mutated.
  CPDF_Document* document = page->GetDocument();
  auto contents = pdfium::MakeRetain<CPDF_Array>();
  for (size_t i = 0; i < current_streams.size(); ++i) {
    uint32_t object_number = current_streams[i]->GetObjNum();
    auto replacement = replacements.find(i);
    if (replacement != replacements.end()) {
      auto dictionary = ToDictionary(current_streams[i]->GetDict()->Clone());
      auto stream = pdfium::MakeRetain<CPDF_Stream>(DataVector<uint8_t>(),
                                                   std::move(dictionary));
      stream->SetDataAndRemoveFilter(replacement->second);
      object_number = document->AddIndirectObject(std::move(stream));
    }
    contents->AppendNew<CPDF_Reference>(document, object_number);
  }
  page->GetMutableDict()->SetFor("Contents", std::move(contents));
  source->consumed = true;
  return Result::kSuccess;
}
