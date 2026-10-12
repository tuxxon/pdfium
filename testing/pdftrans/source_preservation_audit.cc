// Copyright 2026 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Host audit for CPDF_RemoveOwnedTextPreservingStreams().
//
//   list:  --list --input in.pdf
//          Prints every top-level page object of every page as JSON.
//   audit: --input in.pdf --page N (--select-index I | --select-text S)...
//          --output out.pdf [--scale 2]
//
// The audit loads the original from memory, removes the selected top-level
// text objects with the source-stream remover, saves a full copy, closes and
// reopens it, and compares it with independent oracles computed from fresh
// loads of the unchanged original:
//   * every page's effective MediaBox/CropBox/Rotate/UserUnit and size,
//   * every page's top-level object signature sequence (minus the selection),
//   * every non-target page's raster (exact),
//   * the target page raster outside the selected objects' actual paint mask,
//     where the mask is the original page rendered with ONLY the selected
//     objects active on a transparent bitmap (alpha != 0),
//   * the target page raster against the original rendered with the selected
//     objects deactivated (exact).
// There are no tolerances and no bounding-box masks. A rejected removal must
// leave the page dictionary unchanged and its save/reopen identical.

#include <stdint.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "core/fdrm/fx_crypt_sha.h"
#include "core/fpdfapi/edit/cpdf_sourcecontentremover.h"
#include "core/fpdfapi/font/cpdf_font.h"
#include "core/fpdfapi/page/cpdf_page.h"
#include "core/fpdfapi/page/cpdf_pageobject.h"
#include "core/fpdfapi/page/cpdf_textobject.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_object.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfapi/parser/cpdf_stream_acc.h"
#include "core/fxcrt/compiler_specific.h"
#include "core/fxcrt/span.h"
#include "fpdfsdk/cpdfsdk_helpers.h"
#include "public/cpp/fpdf_scopers.h"
#include "public/fpdf_edit.h"
#include "public/fpdf_save.h"
#include "public/fpdfview.h"

namespace {

using Bytes = std::vector<uint8_t>;

const char* ResultName(CPDF_SourceTextRemovalResult result) {
  switch (result) {
    case CPDF_SourceTextRemovalResult::kSuccess:
      return "SUCCESS";
    case CPDF_SourceTextRemovalResult::kInvalidSelection:
      return "kInvalidSelection";
    case CPDF_SourceTextRemovalResult::kNoSourceProvenance:
      return "kNoSourceProvenance";
    case CPDF_SourceTextRemovalResult::kPageModified:
      return "kPageModified";
    case CPDF_SourceTextRemovalResult::kUnsupportedContent:
      return "kUnsupportedContent";
    case CPDF_SourceTextRemovalResult::kUnsupportedText:
      return "kUnsupportedText";
    case CPDF_SourceTextRemovalResult::kCrossStreamOperator:
      return "kCrossStreamOperator";
    case CPDF_SourceTextRemovalResult::kTextPositionDependency:
      return "kTextPositionDependency";
    case CPDF_SourceTextRemovalResult::kSourceChanged:
      return "kSourceChanged";
  }
  return "UNKNOWN";
}

std::string Hex(pdfium::span<const uint8_t> bytes) {
  static constexpr std::string_view kDigits = "0123456789abcdef";
  std::string out;
  for (uint8_t b : bytes) {
    out += kDigits[b >> 4];
    out += kDigits[b & 15];
  }
  return out;
}

std::string Sha256(pdfium::span<const uint8_t> bytes) {
  return Hex(CryptSha256Generate(bytes));
}

std::string Json(const std::string& s) {
  std::string out = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += static_cast<char>(c);
    } else if (c < 0x20) {
      static constexpr std::string_view kDigits = "0123456789abcdef";
      out += "\\u00";
      out += kDigits[c >> 4];
      out += kDigits[c & 15];
    } else {
      out += static_cast<char>(c);
    }
  }
  return out + "\"";
}

// Bit-exact float text: %.9g round-trips float.
std::string Num(float value) {
  std::ostringstream out;
  out << std::setprecision(9) << value;
  return out.str();
}

std::string Utf16ToUtf8(const std::vector<uint16_t>& units) {
  std::string out;
  for (size_t i = 0; i < units.size(); ++i) {
    uint32_t c = units[i];
    if (c >= 0xD800 && c < 0xDC00 && i + 1 < units.size() &&
        units[i + 1] >= 0xDC00 && units[i + 1] < 0xE000) {
      c = 0x10000 + ((c - 0xD800) << 10) + (units[++i] - 0xDC00);
    }
    if (c < 0x80) {
      out += static_cast<char>(c);
    } else if (c < 0x800) {
      out += static_cast<char>(0xC0 | (c >> 6));
      out += static_cast<char>(0x80 | (c & 63));
    } else if (c < 0x10000) {
      out += static_cast<char>(0xE0 | (c >> 12));
      out += static_cast<char>(0x80 | ((c >> 6) & 63));
      out += static_cast<char>(0x80 | (c & 63));
    } else {
      out += static_cast<char>(0xF0 | (c >> 18));
      out += static_cast<char>(0x80 | ((c >> 12) & 63));
      out += static_cast<char>(0x80 | ((c >> 6) & 63));
      out += static_cast<char>(0x80 | (c & 63));
    }
  }
  return out;
}

bool ReadFile(const std::string& path, Bytes* out) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return false;
  }
  out->assign(std::istreambuf_iterator<char>(file),
              std::istreambuf_iterator<char>());
  return !file.bad();
}

bool WriteFile(const std::string& path, const Bytes& bytes) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  std::copy(bytes.begin(), bytes.end(), std::ostreambuf_iterator<char>(file));
  file.close();
  return !file.fail();
}

struct MemoryWriter : FPDF_FILEWRITE {
  Bytes bytes;
  MemoryWriter() {
    version = 1;
    WriteBlock = [](FPDF_FILEWRITE* self, const void* data,
                    unsigned long size) -> int {
      auto* writer = static_cast<MemoryWriter*>(self);
      // SAFETY: FPDF_FILEWRITE guarantees `size` readable bytes at `data`.
      auto block =
          UNSAFE_BUFFERS(pdfium::span(static_cast<const uint8_t*>(data), size));
      writer->bytes.insert(writer->bytes.end(), block.begin(), block.end());
      return 1;
    };
  }
};

ScopedFPDFDocument Load(const Bytes& bytes) {
  return ScopedFPDFDocument(
      FPDF_LoadMemDocument64(bytes.data(), bytes.size(), nullptr));
}

// Effective page attributes, resolving inheritance through /Parent.
std::string Attr(const CPDF_Dictionary* page, const char* key, bool inherit) {
  RetainPtr<const CPDF_Dictionary> dict(page);
  std::set<const CPDF_Dictionary*> seen;
  while (dict && seen.insert(dict.Get()).second) {
    RetainPtr<const CPDF_Object> value = dict->GetDirectObjectFor(key);
    if (value) {
      if (const CPDF_Array* array = value->AsArray()) {
        std::string out = "[";
        for (size_t i = 0; i < array->size(); ++i) {
          out += (i ? "," : "") + Num(array->GetFloatAt(i));
        }
        return out + "]";
      }
      if (value->IsNumber()) {
        return Num(value->GetNumber());
      }
      return Json("non-numeric");
    }
    if (!inherit) {
      break;
    }
    dict = dict->GetDictFor("Parent");
  }
  return "null";
}

std::string PageSettings(FPDF_PAGE fpdf_page) {
  const CPDF_Page* page = CPDFPageFromFPDFPage(fpdf_page);
  const CPDF_Dictionary* dict = page->GetDict().Get();
  return "{\"MediaBox\":" + Attr(dict, "MediaBox", true) +
         ",\"CropBox\":" + Attr(dict, "CropBox", true) +
         ",\"Rotate\":" + Attr(dict, "Rotate", true) +
         ",\"UserUnit\":" + Attr(dict, "UserUnit", false) +
         ",\"width\":" + Num(FPDF_GetPageWidthF(fpdf_page)) +
         ",\"height\":" + Num(FPDF_GetPageHeightF(fpdf_page)) +
         ",\"rotation\":" + std::to_string(FPDFPage_GetRotation(fpdf_page)) +
         "}";
}

const char* TypeName(int type) {
  switch (type) {
    case FPDF_PAGEOBJ_TEXT:
      return "text";
    case FPDF_PAGEOBJ_PATH:
      return "path";
    case FPDF_PAGEOBJ_IMAGE:
      return "image";
    case FPDF_PAGEOBJ_SHADING:
      return "shading";
    case FPDF_PAGEOBJ_FORM:
      return "form";
  }
  return "unknown";
}

struct ObjectInfo {
  int type = 0;
  std::array<float, 4> bounds = {};  // left, bottom, right, top
  std::string text;                  // UTF-8 raw object text.
  std::string signature;
  std::string json;
};

std::vector<ObjectInfo> DescribeObjects(FPDF_PAGE page) {
  std::vector<ObjectInfo> result;
  const int count = FPDFPage_CountObjects(page);
  for (int i = 0; i < count; ++i) {
    FPDF_PAGEOBJECT object = FPDFPage_GetObject(page, i);
    ObjectInfo info;
    info.type = FPDFPageObj_GetType(object);
    float l = 0, b = 0, r = 0, t = 0;
    FPDFPageObj_GetBounds(object, &l, &b, &r, &t);
    info.bounds = {l, b, r, t};
    FS_MATRIX m = {};
    FPDFPageObj_GetMatrix(object, &m);
    std::string geometry = "\"bounds\":[" + Num(l) + "," + Num(b) + "," +
                           Num(r) + "," + Num(t) + "],\"matrix\":[" + Num(m.a) +
                           "," + Num(m.b) + "," + Num(m.c) + "," + Num(m.d) +
                           "," + Num(m.e) + "," + Num(m.f) + "]";
    std::string extra;
    if (info.type == FPDF_PAGEOBJ_TEXT) {
      // Raw object text: the font's Unicode for each shown char code. Unlike
      // FPDFTextObj_GetText(), it does not depend on neighbouring objects.
      const auto* text = CPDFPageObjectFromFPDFPageObject(object)->AsText();
      RetainPtr<CPDF_Font> font = text->GetFont();
      std::vector<uint16_t> units;
      for (uint32_t code : text->GetCharCodes()) {
        if (code == CPDF_Font::kInvalidCharCode || !font) {
          continue;
        }
        WideString unicode = font->UnicodeFromCharCode(code);
        if (unicode.IsEmpty()) {
          units.push_back(0xFFFD);
        }
        for (wchar_t c : unicode) {
          if (c >= 0x10000) {
            units.push_back(0xD800 + ((c - 0x10000) >> 10));
            units.push_back(0xDC00 + ((c - 0x10000) & 0x3FF));
          } else {
            units.push_back(static_cast<uint16_t>(c));
          }
        }
      }
      info.text = Utf16ToUtf8(units);
      float size = 0;
      FPDFTextObj_GetFontSize(object, &size);
      extra = ",\"text\":" + Json(info.text) + ",\"utf16\":\"" +
              Hex(pdfium::as_bytes(pdfium::span(units))) + "\",\"mode\":" +
              std::to_string(FPDFTextObj_GetTextRenderMode(object)) +
              ",\"fontSize\":" + Num(size) +
              ",\"font\":" + Json(font ? font->GetBaseFontName().c_str() : "") +
              ",\"type3\":" + (font && font->IsType3Font() ? "true" : "false") +
              ",\"chars\":" + std::to_string(text->CountItems());
    }
    info.signature = std::string("\"type\":\"") + TypeName(info.type) + "\"," +
                     geometry + extra;
    info.json =
        "{\"index\":" + std::to_string(i) + "," + info.signature +
        ",\"contentStream\":" +
        std::to_string(
            CPDFPageObjectFromFPDFPageObject(object)->GetContentStream()) +
        "}";
    result.push_back(std::move(info));
  }
  return result;
}

struct Raster {
  int width = 0;
  int height = 0;
  Bytes bgra;
};

// White-background BGRA render without annotations, same as the page frame
// the app uses; `transparent` gives an alpha paint mask instead.
Raster Render(FPDF_PAGE page, float scale, bool transparent) {
  Raster raster;
  raster.width =
      static_cast<int>(std::lround(FPDF_GetPageWidthF(page) * scale));
  raster.height =
      static_cast<int>(std::lround(FPDF_GetPageHeightF(page) * scale));
  ScopedFPDFBitmap bitmap(FPDFBitmap_Create(raster.width, raster.height, 1));
  FPDFBitmap_FillRect(bitmap.get(), 0, 0, raster.width, raster.height,
                      transparent ? 0x00000000 : 0xFFFFFFFF);
  FPDF_RenderPageBitmap(bitmap.get(), page, 0, 0, raster.width, raster.height,
                        0, 0);
  const int stride = FPDFBitmap_GetStride(bitmap.get());
  // SAFETY: an FPDF_BITMAP buffer holds `stride * height` bytes.
  auto data = UNSAFE_BUFFERS(pdfium::span(
      static_cast<const uint8_t*>(FPDFBitmap_GetBuffer(bitmap.get())),
      static_cast<size_t>(stride) * raster.height));
  for (int y = 0; y < raster.height; ++y) {
    auto row = data.subspan(static_cast<size_t>(y) * stride,
                            static_cast<size_t>(raster.width) * 4);
    raster.bgra.insert(raster.bgra.end(), row.begin(), row.end());
  }
  return raster;
}

int CountDiff(const Raster& a,
              const Raster& b,
              const std::vector<bool>* mask,
              bool inside) {
  if (a.width != b.width || a.height != b.height) {
    return -1;
  }
  int count = 0;
  for (size_t p = 0; p < a.bgra.size() / 4; ++p) {
    if (mask && (*mask)[p] != inside) {
      continue;
    }
    if (!std::equal(a.bgra.begin() + p * 4, a.bgra.begin() + p * 4 + 4,
                    b.bgra.begin() + p * 4)) {
      ++count;
    }
  }
  return count;
}

int PageCount(const Bytes& bytes) {
  ScopedFPDFDocument doc = Load(bytes);
  return doc ? FPDF_GetPageCount(doc.get()) : -1;
}

struct PageSnapshot {
  std::string settings;
  std::vector<ObjectInfo> objects;
  Raster raster;
};

std::vector<PageSnapshot> Snapshot(const Bytes& bytes, float scale) {
  std::vector<PageSnapshot> pages;
  ScopedFPDFDocument doc = Load(bytes);
  if (!doc) {
    return pages;
  }
  for (int i = 0; i < FPDF_GetPageCount(doc.get()); ++i) {
    ScopedFPDFPage page(FPDF_LoadPage(doc.get(), i));
    PageSnapshot snapshot;
    snapshot.settings = PageSettings(page.get());
    snapshot.objects = DescribeObjects(page.get());
    snapshot.raster = Render(page.get(), scale, false);
    pages.push_back(std::move(snapshot));
  }
  return pages;
}

// Renders the original target page with `selected` objects either the ONLY
// active objects (paint oracle) or the only INACTIVE ones (expected result).
Raster RenderSelection(const Bytes& bytes,
                       int page_index,
                       const std::set<int>& selected,
                       bool only_selected,
                       float scale) {
  ScopedFPDFDocument doc = Load(bytes);
  ScopedFPDFPage page(FPDF_LoadPage(doc.get(), page_index));
  const int count = FPDFPage_CountObjects(page.get());
  for (int i = 0; i < count; ++i) {
    const bool is_selected = selected.contains(i);
    FPDFPageObj_SetIsActive(FPDFPage_GetObject(page.get(), i),
                            only_selected ? is_selected : !is_selected);
  }
  return Render(page.get(), scale, only_selected);
}

// Union of the selected objects' bounds, snapped outward to whole pixels.
std::vector<bool> BoundsMask(const Bytes& bytes,
                             int page_index,
                             const std::vector<ObjectInfo>& objects,
                             const std::set<int>& selected,
                             const Raster& frame) {
  std::vector<bool> mask(frame.bgra.size() / 4);
  ScopedFPDFDocument doc = Load(bytes);
  ScopedFPDFPage page(FPDF_LoadPage(doc.get(), page_index));
  const CFX_Matrix display =
      CPDFPageFromFPDFPage(page.get())
          ->GetDisplayMatrixForRect(FX_RECT(0, 0, frame.width, frame.height),
                                    0);
  for (int index : selected) {
    const auto& box = objects[index].bounds;
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (int corner = 0; corner < 4; ++corner) {
      const CFX_PointF point = display.Transform(CFX_PointF(
          corner & 1 ? box[2] : box[0], corner & 2 ? box[3] : box[1]));
      x0 = std::min(x0, point.x);
      y0 = std::min(y0, point.y);
      x1 = std::max(x1, point.x);
      y1 = std::max(y1, point.y);
    }
    const int left = std::max(0, static_cast<int>(std::floor(x0)));
    const int top = std::max(0, static_cast<int>(std::floor(y0)));
    const int right = std::min(frame.width, static_cast<int>(std::ceil(x1)));
    const int bottom = std::min(frame.height, static_cast<int>(std::ceil(y1)));
    for (int y = top; y < bottom; ++y) {
      for (int x = left; x < right; ++x) {
        mask[static_cast<size_t>(y) * frame.width + x] = true;
      }
    }
  }
  return mask;
}

// Decoded /Contents streams of a page (direct stream or array).
std::vector<Bytes> DecodedContents(FPDF_PAGE fpdf_page,
                                   std::vector<uint32_t>* object_numbers) {
  std::vector<Bytes> result;
  const CPDF_Page* page = CPDFPageFromFPDFPage(fpdf_page);
  RetainPtr<const CPDF_Object> contents =
      page->GetDict()->GetDirectObjectFor("Contents");
  std::vector<RetainPtr<const CPDF_Stream>> streams;
  if (contents && contents->IsStream()) {
    streams.push_back(pdfium::WrapRetain(contents->AsStream()));
  } else if (contents && contents->IsArray()) {
    for (size_t i = 0; i < contents->AsArray()->size(); ++i) {
      streams.push_back(contents->AsArray()->GetStreamAt(i));
    }
  }
  for (const auto& stream : streams) {
    if (!stream) {
      result.emplace_back();
      object_numbers->push_back(0);
      continue;
    }
    auto acc = pdfium::MakeRetain<CPDF_StreamAcc>(stream);
    acc->LoadAllDataFiltered();
    result.emplace_back(acc->GetSpan().begin(), acc->GetSpan().end());
    object_numbers->push_back(stream->GetObjNum());
  }
  return result;
}

std::string StreamComparison(const Bytes& source,
                             const Bytes& output,
                             int page_index,
                             bool* ok) {
  ScopedFPDFDocument a = Load(source);
  ScopedFPDFDocument b = Load(output);
  ScopedFPDFPage pa(FPDF_LoadPage(a.get(), page_index));
  ScopedFPDFPage pb(FPDF_LoadPage(b.get(), page_index));
  std::vector<uint32_t> na, nb;
  std::vector<Bytes> sa = DecodedContents(pa.get(), &na);
  std::vector<Bytes> sb = DecodedContents(pb.get(), &nb);
  std::string json = "[";
  *ok = sa.size() == sb.size();
  for (size_t i = 0; i < sa.size() && i < sb.size(); ++i) {
    size_t changed = 0;
    bool only_spaces = sa[i].size() == sb[i].size();
    for (size_t j = 0; only_spaces && j < sa[i].size(); ++j) {
      if (sa[i][j] != sb[i][j]) {
        ++changed;
        only_spaces = sb[i][j] == ' ';
      }
    }
    *ok = *ok && only_spaces;
    json += std::string(i ? "," : "") +
            "{\"sourceBytes\":" + std::to_string(sa[i].size()) +
            ",\"outputBytes\":" + std::to_string(sb[i].size()) +
            ",\"blankedBytes\":" + std::to_string(changed) +
            ",\"onlyBlanked\":" + (only_spaces ? "true" : "false") + "}";
  }
  return json + "]";
}

int Fail(const std::string& message) {
  std::cerr << "error: " << message << "\n";
  return 2;
}

int List(const Bytes& input) {
  ScopedFPDFDocument doc = Load(input);
  if (!doc) {
    return Fail("cannot load input");
  }
  std::cout << "{\"inputSha256\":\"" << Sha256(input) << "\",\"pages\":[";
  for (int i = 0; i < FPDF_GetPageCount(doc.get()); ++i) {
    ScopedFPDFPage page(FPDF_LoadPage(doc.get(), i));
    std::cout << (i ? "," : "") << "{\"page\":" << i
              << ",\"settings\":" << PageSettings(page.get())
              << ",\"objects\":[";
    std::vector<ObjectInfo> objects = DescribeObjects(page.get());
    for (size_t j = 0; j < objects.size(); ++j) {
      std::cout << (j ? "," : "") << objects[j].json;
    }
    std::cout << "]}";
  }
  std::cout << "]}\n";
  return 0;
}

int Audit(const Bytes& input,
          int page_index,
          std::vector<int> select_indices,
          const std::vector<std::string>& select_texts,
          const std::string& output_path,
          float scale) {
  const std::string input_sha = Sha256(input);
  std::vector<PageSnapshot> before = Snapshot(input, scale);
  if (before.empty() || page_index < 0 ||
      page_index >= static_cast<int>(before.size())) {
    return Fail("bad input or page");
  }
  const std::vector<ObjectInfo>& objects = before[page_index].objects;
  std::string selection_error;
  for (const std::string& wanted : select_texts) {
    int match = -1;
    int matches = 0;
    for (size_t i = 0; i < objects.size(); ++i) {
      if (objects[i].type == FPDF_PAGEOBJ_TEXT && objects[i].text == wanted) {
        match = static_cast<int>(i);
        ++matches;
      }
    }
    if (matches != 1) {
      selection_error = matches ? "AMBIGUOUS_SELECTION" : "NOT_SELECTABLE";
      break;
    }
    select_indices.push_back(match);
  }
  std::set<int> selected(select_indices.begin(), select_indices.end());
  if (selection_error.empty() &&
      (selected.empty() || selected.size() != select_indices.size())) {
    selection_error = "AMBIGUOUS_SELECTION";
  }
  for (int index : selected) {
    if (!selection_error.empty()) {
      break;
    }
    if (index < 0 || index >= static_cast<int>(objects.size()) ||
        objects[index].type != FPDF_PAGEOBJ_TEXT) {
      selection_error = "NOT_SELECTABLE";
    }
  }
  std::string selection_json = "[";
  for (int index : selected) {
    if (index >= 0 && index < static_cast<int>(objects.size())) {
      selection_json += std::string(selection_json.size() > 1 ? "," : "") +
                        objects[index].json;
    }
  }
  selection_json += "]";
  if (!selection_error.empty()) {
    std::cout << "{\"inputSha256\":\"" << input_sha
              << "\",\"page\":" << page_index << ",\"result\":\""
              << selection_error << "\",\"selection\":" << selection_json
              << ",\"passed\":true}\n";
    return 0;
  }

  // Removal on a fresh load. No GenerateContent/RemoveObject is ever called.
  CPDF_SourceTextRemovalResult result;
  bool contents_unchanged_on_reject = true;
  MemoryWriter writer;
  {
    ScopedFPDFDocument doc = Load(input);
    ScopedFPDFPage page(FPDF_LoadPage(doc.get(), page_index));
    CPDF_Page* cpdf_page = CPDFPageFromFPDFPage(page.get());
    std::vector<CPDF_TextObject*> owned;
    for (int index : selected) {
      owned.push_back(CPDFPageObjectFromFPDFPageObject(
                          FPDFPage_GetObject(page.get(), index))
                          ->AsText());
    }
    RetainPtr<const CPDF_Object> contents_before =
        cpdf_page->GetDict()->GetObjectFor("Contents");
    result = CPDF_RemoveOwnedTextPreservingStreams(cpdf_page, owned);
    if (result != CPDF_SourceTextRemovalResult::kSuccess) {
      contents_unchanged_on_reject =
          contents_before.Get() ==
          cpdf_page->GetDict()->GetObjectFor("Contents").Get();
    }
    page.reset();
    if (!FPDF_SaveAsCopy(doc.get(), &writer, 0)) {
      return Fail("save failed");
    }
  }
  const bool removed = result == CPDF_SourceTextRemovalResult::kSuccess;
  if (!output_path.empty() && !WriteFile(output_path, writer.bytes)) {
    return Fail("cannot write output");
  }
  std::vector<PageSnapshot> after = Snapshot(writer.bytes, scale);
  bool passed = after.size() == before.size() && contents_unchanged_on_reject;
  std::string pages_json = "[";
  const Raster paint = RenderSelection(input, page_index, selected,
                                       /*only_selected=*/true, scale);
  const Raster expected = RenderSelection(input, page_index, selected,
                                          /*only_selected=*/false, scale);
  for (size_t i = 0; i < before.size() && i < after.size(); ++i) {
    const bool target = static_cast<int>(i) == page_index && removed;
    std::vector<std::string> want, got;
    for (size_t j = 0; j < before[i].objects.size(); ++j) {
      if (!target || !selected.contains(static_cast<int>(j))) {
        want.push_back(before[i].objects[j].signature);
      }
    }
    for (const auto& object : after[i].objects) {
      got.push_back(object.signature);
    }
    const bool settings_equal = before[i].settings == after[i].settings;
    const bool objects_equal = want == got;
    std::string page_json =
        "{\"page\":" + std::to_string(i) +
        ",\"settingsEqual\":" + (settings_equal ? "true" : "false") +
        ",\"objectsEqual\":" + (objects_equal ? "true" : "false") +
        ",\"sourceObjects\":" + std::to_string(before[i].objects.size()) +
        ",\"outputObjects\":" + std::to_string(after[i].objects.size()) +
        ",\"sourceRasterSha256\":\"" + Sha256(before[i].raster.bgra) +
        "\",\"outputRasterSha256\":\"" + Sha256(after[i].raster.bgra) + "\"";
    passed = passed && settings_equal && objects_equal;
    if (!target) {
      const int diff = CountDiff(before[i].raster, after[i].raster, nullptr, 0);
      page_json += ",\"changedPixels\":" + std::to_string(diff);
      passed = passed && diff == 0;
    } else {
      std::vector<bool> mask(paint.bgra.size() / 4);
      int mask_pixels = 0;
      for (size_t p = 0; p < mask.size(); ++p) {
        mask[p] = paint.bgra[p * 4 + 3] != 0;
        mask_pixels += mask[p];
      }
      const int outside = CountDiff(before[i].raster, after[i].raster, &mask,
                                    /*inside=*/false);
      const int inside = CountDiff(before[i].raster, after[i].raster, &mask,
                                   /*inside=*/true);
      const int vs_expected =
          CountDiff(expected, after[i].raster, nullptr, false);
      // Diagnostic only (never used for PASS): the historical audit allowed
      // changes inside the selected objects' pixel-snapped bounding boxes.
      const std::vector<bool> boxes =
          BoundsMask(input, page_index, objects, selected, paint);
      int outside_boxes = 0;
      int outside_boxes_in_paint = 0;
      for (size_t p = 0; p < mask.size(); ++p) {
        if (!boxes[p] && !std::equal(before[i].raster.bgra.begin() + p * 4,
                                     before[i].raster.bgra.begin() + p * 4 + 4,
                                     after[i].raster.bgra.begin() + p * 4)) {
          ++outside_boxes;
          outside_boxes_in_paint += mask[p];
        }
      }
      bool streams_ok = false;
      const std::string streams =
          StreamComparison(input, writer.bytes, page_index, &streams_ok);
      page_json +=
          ",\"paintMaskPixels\":" + std::to_string(mask_pixels) +
          ",\"changedInsidePaint\":" + std::to_string(inside) +
          ",\"changedOutsidePaint\":" + std::to_string(outside) +
          ",\"changedOutsideBounds\":" + std::to_string(outside_boxes) +
          ",\"changedOutsideBoundsInsidePaint\":" +
          std::to_string(outside_boxes_in_paint) +
          ",\"diffVsDeactivatedOriginal\":" + std::to_string(vs_expected) +
          ",\"paintMaskSha256\":\"" + Sha256(paint.bgra) +
          "\",\"streams\":" + streams;
      passed = passed && outside == 0 && vs_expected == 0 && streams_ok;
    }
    pages_json += std::string(i ? "," : "") + page_json + "}";
  }
  pages_json += "]";
  std::cout << "{\"inputSha256\":\"" << input_sha
            << "\",\"page\":" << page_index << ",\"result\":\""
            << ResultName(result) << "\",\"selection\":" << selection_json
            << ",\"outputSha256\":\"" << Sha256(writer.bytes)
            << "\",\"outputBytes\":" << writer.bytes.size()
            << ",\"sourcePageCount\":" << before.size()
            << ",\"outputPageCount\":" << PageCount(writer.bytes)
            << ",\"contentsUnchangedOnReject\":"
            << (contents_unchanged_on_reject ? "true" : "false")
            << ",\"scale\":" << Num(scale) << ",\"pages\":" << pages_json
            << ",\"passed\":" << (passed ? "true" : "false") << "}\n";
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  // SAFETY: argv holds argc entries.
  const std::vector<std::string> args =
      UNSAFE_BUFFERS(std::vector<std::string>(argv, argv + argc));
  std::string input_path, output_path;
  int page = -1;
  float scale = 2.0f;
  bool list = false;
  std::vector<int> indices;
  std::vector<std::string> texts;
  for (int i = 1; i < argc; ++i) {
    const std::string& arg = args[i];
    const bool has_value = i + 1 < argc;
    if (arg == "--list") {
      list = true;
    } else if (arg == "--input" && has_value) {
      input_path = args[++i];
    } else if (arg == "--output" && has_value) {
      output_path = args[++i];
    } else if (arg == "--page" && has_value) {
      page = std::stoi(args[++i]);
    } else if (arg == "--scale" && has_value) {
      scale = std::stof(args[++i]);
    } else if (arg == "--select-index" && has_value) {
      indices.push_back(std::stoi(args[++i]));
    } else if (arg == "--select-text" && has_value) {
      texts.push_back(args[++i]);
    } else {
      return Fail("unknown argument " + arg);
    }
  }
  Bytes input;
  if (input_path.empty() || !ReadFile(input_path, &input)) {
    return Fail("cannot read --input");
  }
  const std::string sha_before = Sha256(input);
  FPDF_LIBRARY_CONFIG config = {};
  config.version = 2;
  FPDF_InitLibraryWithConfig(&config);
  int status = list ? List(input)
                    : Audit(input, page, indices, texts, output_path, scale);
  FPDF_DestroyLibrary();
  Bytes reread;
  if (!ReadFile(input_path, &reread) || Sha256(reread) != sha_before) {
    return Fail("input file changed during audit");
  }
  return status;
}
