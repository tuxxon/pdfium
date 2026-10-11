// Copyright 2026 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/fpdfapi/edit/cpdf_sourcecontentremover.h"

#include <algorithm>
#include <map>
#include <set>
#include <utility>
#include <vector>

#include "core/fpdfapi/font/cpdf_font.h"
#include "core/fpdfapi/page/cpdf_form.h"
#include "core/fpdfapi/page/cpdf_formobject.h"
#include "core/fpdfapi/page/cpdf_page.h"
#include "core/fpdfapi/page/cpdf_sourcecontent.h"
#include "core/fpdfapi/page/cpdf_textobject.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfapi/parser/cpdf_stream_acc.h"
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

Result ValidateCursorDependencies(const CPDF_SourceContent& source,
                                   const std::set<size_t>& selected) {
  bool tainted = false;
  bool in_text = false;
  std::vector<bool> saved_taint;
  for (const auto& op : source.operations) {
    const ByteString& name = op.name;
    const bool removed = op.text_object_index &&
                         selected.contains(*op.text_object_index);
    if (!op.valid_operands) {
      return Result::kUnsupportedContent;
    }
    if (name == "q") {
      saved_taint.push_back(tainted);
    } else if (name == "Q") {
      if (saved_taint.empty()) {
        return Result::kUnsupportedContent;
      }
      tainted = saved_taint.back();
      saved_taint.pop_back();
    } else if (name == "BT") {
      if (in_text) {
        return Result::kUnsupportedContent;
      }
      in_text = true;
      tainted = false;
    } else if (name == "ET") {
      if (!in_text) {
        return Result::kUnsupportedContent;
      }
      in_text = false;
      // ET finalizes text clipping; PDFium does not reset text position here.
    } else if (name == "Tm" || name == "Td" || name == "TD" || name == "T*") {
      tainted = false;
    } else if (name == "Tj" || name == "TJ" || name == "'" || name == "\"") {
      if (!in_text) {
        return Result::kUnsupportedContent;
      }
      if (removed) {
        // Only plain Tj/TJ pass selection validation. Neither changes the line
        // position; mark their current-text-position effect as unknown.
        tainted = true;
      } else {
        // Retained quote operators first reset position from the text line.
        if (name == "'" || name == "\"") {
          tainted = false;
        }
        if (tainted) {
          return Result::kTextPositionDependency;
        }
      }
    } else if (name == "Do" && tainted) {
      // Form parsing copies the parent's whole CPDF_AllStates. Do not assume
      // a Form starts with BT. Conservatively reject image Do as well.
      return Result::kTextPositionDependency;
    }
  }
  return in_text || !saved_taint.empty() ? Result::kUnsupportedContent
                                        : Result::kSuccess;
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
  const Result dependencies = ValidateCursorDependencies(*source, selected);
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
