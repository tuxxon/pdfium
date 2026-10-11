// Copyright 2026 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/fpdfapi/edit/cpdf_sourcecontentremover.h"

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "core/fpdfapi/page/cpdf_form.h"
#include "core/fpdfapi/page/cpdf_formobject.h"
#include "core/fpdfapi/page/cpdf_page.h"
#include "core/fpdfapi/page/cpdf_textobject.h"
#include "core/fpdfapi/page/test_with_page_module.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfapi/parser/cpdf_stream_acc.h"
#include "core/fpdfapi/parser/cpdf_test_document.h"
#include "core/fxcrt/span.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace {
using Result = CPDF_SourceTextRemovalResult;

std::string Decode(const CPDF_Stream* stream) {
  auto acc = pdfium::MakeRetain<CPDF_StreamAcc>(pdfium::WrapRetain(stream));
  acc->LoadAllDataFiltered();
  auto bytes = acc->GetSpan();
  return std::string(bytes.begin(), bytes.end());
}

std::string Blank(std::string original, const std::string& selected) {
  size_t pos = original.find(selected);
  EXPECT_NE(std::string::npos, pos);
  if (pos != std::string::npos) {
    original.replace(pos, selected.size(), selected.size(), ' ');
  }
  return original;
}
}  // namespace

class CPDFSourceContentRemoverTest : public TestWithPageModule {
 protected:
  void SetUp() override {
    TestWithPageModule::SetUp();
    doc_ = std::make_unique<CPDF_TestDocument>();
    doc_->CreateNewDoc();
    auto font = doc_->NewIndirect<CPDF_Dictionary>();
    font->SetNewFor<CPDF_Name>("Type", "Font");
    font->SetNewFor<CPDF_Name>("Subtype", "Type1");
    font->SetNewFor<CPDF_Name>("BaseFont", "Helvetica");
    resources_ = pdfium::MakeRetain<CPDF_Dictionary>();
    auto fonts = resources_->SetNewFor<CPDF_Dictionary>("Font");
    fonts->SetNewFor<CPDF_Reference>("F1", doc_.get(), font->GetObjNum());
  }

  void TearDown() override {
    resources_.Reset();
    doc_.reset();
    TestWithPageModule::TearDown();
  }

  RetainPtr<CPDF_Stream> InstallForm(const std::string& content) {
    auto form = doc_->NewIndirect<CPDF_Stream>(pdfium::as_byte_span(content));
    auto dict = form->GetMutableDict();
    dict->SetNewFor<CPDF_Name>("Type", "XObject");
    dict->SetNewFor<CPDF_Name>("Subtype", "Form");
    auto box = dict->SetNewFor<CPDF_Array>("BBox");
    for (int value : {0, 0, 300, 300}) {
      box->AppendNew<CPDF_Number>(value);
    }
    dict->SetFor("Resources", resources_->Clone());
    resources_->SetNewFor<CPDF_Dictionary>("XObject")
        ->SetNewFor<CPDF_Reference>("Fm", doc_.get(), form->GetObjNum());
    return form;
  }

  RetainPtr<CPDF_Page> MakePage(const std::vector<std::string>& contents) {
    auto dict = doc_->CreateNewPage(page_count_++);
    dict->SetFor("Resources", resources_);
    auto array = doc_->NewIndirect<CPDF_Array>();
    for (const auto& content : contents) {
      auto stream = doc_->NewIndirect<CPDF_Stream>(
          pdfium::as_byte_span(content));
      array->AppendNew<CPDF_Reference>(doc_.get(), stream->GetObjNum());
    }
    dict->SetNewFor<CPDF_Reference>("Contents", doc_.get(), array->GetObjNum());
    auto page = pdfium::MakeRetain<CPDF_Page>(doc_.get(), dict);
    page->ParseContent();
    return page;
  }

  CPDF_TextObject* Text(CPDF_Page* page, size_t ordinal = 0) {
    for (const auto& object : *page) {
      if (object->IsText() && ordinal-- == 0) {
        return object->AsText();
      }
    }
    return nullptr;
  }

  Result Remove(CPDF_Page* page, size_t ordinal = 0) {
    std::array<CPDF_TextObject*, 1> owned = {Text(page, ordinal)};
    return CPDF_RemoveOwnedTextPreservingStreams(page, owned);
  }

  RetainPtr<const CPDF_Stream> Stream(CPDF_Page* page, size_t index = 0) {
    auto contents = page->GetDict()->GetDirectObjectFor("Contents");
    if (contents->IsStream()) {
      return pdfium::WrapRetain(contents->AsStream());
    }
    return contents->AsArray()->GetStreamAt(index);
  }

  std::unique_ptr<CPDF_TestDocument> doc_;
  RetainPtr<CPDF_Dictionary> resources_;
  int page_count_ = 0;
};

TEST_F(CPDFSourceContentRemoverTest, BlanksOnlyOwnedShowAndRetainsGeometry) {
  const std::string source =
      "q .1 .2 .3 .4 k 10 20 30 40 re f Q\n"
      "BT /F1 12 Tf 1 0 0 1 30 200 Tm % fake (KEEP) Tj\n"
      "(ERASE\\(x\\)) Tj 0 -20 Td (KEEP) Tj ET";
  auto page = MakePage({source});
  auto original_stream = Stream(page.Get());
  auto original_resources = page->GetDict()->GetDirectObjectFor("Resources");
  auto dict = page->GetMutableDict();
  dict->SetNewFor<CPDF_Number>("Rotate", 90);
  dict->SetNewFor<CPDF_Number>("UserUnit", 2);
  auto media_box = dict->SetNewFor<CPDF_Array>("MediaBox");
  auto crop_box = dict->SetNewFor<CPDF_Array>("CropBox");
  for (int value : {10, 20, 310, 420}) {
    media_box->AppendNew<CPDF_Number>(value);
  }
  for (int value : {15, 25, 305, 415}) {
    crop_box->AppendNew<CPDF_Number>(value);
  }
  const auto keep_pos = Text(page.Get(), 1)->GetPos();
  ASSERT_EQ(Result::kSuccess, Remove(page.Get()));
  EXPECT_EQ(source, Decode(original_stream.Get()));
  EXPECT_EQ(Blank(source, "(ERASE\\(x\\)) Tj"),
            Decode(Stream(page.Get()).Get()));
  EXPECT_EQ(original_resources.Get(),
            page->GetDict()->GetDirectObjectFor("Resources").Get());
  EXPECT_EQ(90, dict->GetIntegerFor("Rotate"));
  EXPECT_EQ(2, dict->GetIntegerFor("UserUnit"));
  EXPECT_EQ(media_box.Get(), dict->GetArrayFor("MediaBox").Get());
  EXPECT_EQ(crop_box.Get(), dict->GetArrayFor("CropBox").Get());
  auto reopened = pdfium::MakeRetain<CPDF_Page>(doc_.get(), dict);
  reopened->ParseContent();
  ASSERT_NE(nullptr, Text(reopened.Get()));
  EXPECT_EQ(keep_pos, Text(reopened.Get())->GetPos());
  EXPECT_EQ(nullptr, Text(reopened.Get(), 1));
  EXPECT_EQ(Result::kPageModified, Remove(page.Get()));
}

TEST_F(CPDFSourceContentRemoverTest, TJArrayHexAndCommentsHaveExactSpan) {
  const std::string selected = "[<4142> -35 % ) Tj is a comment\n (CD)] TJ";
  const std::string source = "BT /F1 12 Tf " + selected +
                             " 1 0 0 1 25 50 Tm (KEEP) Tj ET";
  auto page = MakePage({source});
  ASSERT_EQ(Result::kSuccess, Remove(page.Get()));
  EXPECT_EQ(Blank(source, selected), Decode(Stream(page.Get()).Get()));
}

TEST_F(CPDFSourceContentRemoverTest, RejectsAdvanceDependencyWithoutMutation) {
  auto page = MakePage({"BT /F1 12 Tf (ERASE) Tj (KEEP) Tj ET"});
  auto contents = page->GetDict()->GetObjectFor("Contents");
  EXPECT_EQ(Result::kTextPositionDependency, Remove(page.Get()));
  EXPECT_EQ(contents.Get(), page->GetDict()->GetObjectFor("Contents").Get());
}

TEST_F(CPDFSourceContentRemoverTest, ConsecutiveOwnedShowsCanBeRemovedTogether) {
  auto page = MakePage({"BT /F1 12 Tf (ONE) Tj (TWO) Tj ET"});
  std::array<CPDF_TextObject*, 2> owned = {Text(page.Get()), Text(page.Get(), 1)};
  EXPECT_EQ(Result::kSuccess,
            CPDF_RemoveOwnedTextPreservingStreams(page.Get(), owned));
}

TEST_F(CPDFSourceContentRemoverTest, ResetsReestablishIndependentCursor) {
  for (const char* reset : {"1 0 0 1 25 50 Tm", "0 -14 Td", "0 -14 TD",
                            "T*", "ET BT"}) {
    auto page = MakePage({std::string("BT /F1 12 Tf (ERASE) Tj ") +
                          reset + " (KEEP) Tj ET"});
    EXPECT_EQ(Result::kSuccess, Remove(page.Get())) << reset;
  }
}

TEST_F(CPDFSourceContentRemoverTest, ETAloneDoesNotResetCursor) {
  auto page = MakePage({"BT /F1 12 Tf (ERASE) Tj ET (KEEP) Tj"});
  EXPECT_NE(Result::kSuccess, Remove(page.Get()));
}

TEST_F(CPDFSourceContentRemoverTest, QCanRestoreTaintedCursorAfterTm) {
  auto page = MakePage({"BT /F1 12 Tf (ERASE) Tj q "
                        "1 0 0 1 25 50 Tm Q (KEEP) Tj ET"});
  EXPECT_EQ(Result::kTextPositionDependency, Remove(page.Get()));
}

TEST_F(CPDFSourceContentRemoverTest, QRestoresUntaintedSavedCursor) {
  auto page = MakePage({"BT /F1 12 Tf q (ERASE) Tj Q (KEEP) Tj ET"});
  EXPECT_EQ(Result::kSuccess, Remove(page.Get()));
}

TEST_F(CPDFSourceContentRemoverTest, RejectsDoBeforeCursorReset) {
  auto page = MakePage({"BT /F1 12 Tf (ERASE) Tj ET /Missing Do"});
  EXPECT_EQ(Result::kTextPositionDependency, Remove(page.Get()));
}

TEST_F(CPDFSourceContentRemoverTest, RejectsQuotedAndClippingOwnedText) {
  for (const char* show : {"(ERASE) '", "1 2 (ERASE) \"",
                           "4 Tr (ERASE) Tj", "3 Tr (ERASE) Tj"}) {
    auto page = MakePage({std::string("BT /F1 12 Tf ") + show + " ET"});
    EXPECT_EQ(Result::kUnsupportedText, Remove(page.Get())) << show;
  }
}

TEST_F(CPDFSourceContentRemoverTest, RejectsCrossStreamShowOperands) {
  auto page = MakePage({"BT /F1 12 Tf (ERASE)", "Tj ET"});
  EXPECT_EQ(Result::kCrossStreamOperator, Remove(page.Get()));
}

TEST_F(CPDFSourceContentRemoverTest, TracksCursorAcrossStreamBoundary) {
  auto page = MakePage({"BT /F1 12 Tf (ERASE) Tj", "(KEEP) Tj ET"});
  EXPECT_EQ(Result::kTextPositionDependency, Remove(page.Get()));
}

TEST_F(CPDFSourceContentRemoverTest, CopiesSharedArrayAndStreamOnlyForTargetPage) {
  const std::string source = "BT /F1 12 Tf (ERASE) Tj ET";
  auto page = MakePage({source, "q 1 2 3 4 re f Q"});
  auto shared_contents = page->GetDict()->GetObjectFor("Contents");
  auto original_second = Stream(page.Get(), 1);
  auto other_dict = doc_->CreateNewPage(page_count_++);
  other_dict->SetFor("Resources", resources_);
  other_dict->SetFor("Contents", shared_contents->Clone());
  auto other = pdfium::MakeRetain<CPDF_Page>(doc_.get(), other_dict);
  other->ParseContent();
  ASSERT_EQ(Result::kSuccess, Remove(page.Get()));
  EXPECT_EQ(source, Decode(Stream(other.Get()).Get()));
  EXPECT_NE(Stream(other.Get()).Get(), Stream(page.Get()).Get());
  EXPECT_EQ(original_second.Get(), Stream(page.Get(), 1).Get());
  EXPECT_EQ(shared_contents->GetDirect().Get(),
            other->GetDict()->GetDirectObjectFor("Contents").Get());
}

TEST_F(CPDFSourceContentRemoverTest, RejectsChangedSourceBytes) {
  auto page = MakePage({"BT /F1 12 Tf (ERASE) Tj ET"});
  auto stream = page->GetMutableDict()->GetMutableArrayFor("Contents")
                    ->GetMutableStreamAt(0);
  stream->SetDataAndRemoveFilter(pdfium::as_byte_span("BT ET"));
  EXPECT_EQ(Result::kSourceChanged, Remove(page.Get()));
}

TEST_F(CPDFSourceContentRemoverTest, RejectsDirtyAndDuplicateSelections) {
  auto page = MakePage({"BT /F1 12 Tf (ERASE) Tj ET"});
  std::array<CPDF_TextObject*, 2> duplicate = {Text(page.Get()), Text(page.Get())};
  EXPECT_EQ(Result::kInvalidSelection,
            CPDF_RemoveOwnedTextPreservingStreams(page.Get(), duplicate));
  Text(page.Get())->SetDirty(true);
  EXPECT_EQ(Result::kPageModified, Remove(page.Get()));
}

TEST_F(CPDFSourceContentRemoverTest, ParserPauseAndPathLookaheadKeepCorrectOffsets) {
  std::string source = "0 0 m 10 10 l S BT /F1 12 Tf ";
  for (int i = 0; i < 110; ++i) {
    source += "1 0 0 1 20 20 Tm (KEEP) Tj ";
  }
  source += "1 0 0 1 30 30 Tm (ERASE) Tj ET";
  auto page = MakePage({source});
  ASSERT_EQ(Result::kSuccess, Remove(page.Get(), 110));
  EXPECT_EQ(Blank(source, "(ERASE) Tj"), Decode(Stream(page.Get()).Get()));
}

TEST_F(CPDFSourceContentRemoverTest, PreservesInlineImageBytesIncludingFakeTextTokens) {
  const std::string source =
      "q BI /W 4 /H 1 /BPC 8 /CS /RGB ID (FAKE)TjABCD EI Q\n"
      "BT /F1 12 Tf (ERASE) Tj ET";
  auto page = MakePage({source});
  ASSERT_EQ(Result::kSuccess, Remove(page.Get()));
  EXPECT_EQ(Blank(source, "(ERASE) Tj"), Decode(Stream(page.Get()).Get()));
}

TEST_F(CPDFSourceContentRemoverTest, KeepsNonselectedTextClippingCommandsExactly) {
  const std::string source =
      "BT /F1 12 Tf 4 Tr (CLIP) Tj ET q 1 2 30 40 re f Q "
      "BT 0 Tr (ERASE) Tj ET";
  auto page = MakePage({source});
  ASSERT_EQ(Result::kSuccess, Remove(page.Get(), 1));
  EXPECT_EQ(Blank(source, "(ERASE) Tj"), Decode(Stream(page.Get()).Get()));
}

TEST_F(CPDFSourceContentRemoverTest, SourceStreamIdentityCannotBeSubstituted) {
  const std::string source = "BT /F1 12 Tf (ERASE) Tj ET";
  auto page = MakePage({source});
  auto different_stream = doc_->NewIndirect<CPDF_Stream>(
      pdfium::as_byte_span(source));
  page->GetMutableDict()->GetMutableArrayFor("Contents")
      ->SetNewAt<CPDF_Reference>(0, doc_.get(), different_stream->GetObjNum());
  EXPECT_EQ(Result::kSourceChanged, Remove(page.Get()));
}

TEST_F(CPDFSourceContentRemoverTest, RejectsPartialBatchBeforeAnyContentsMutation) {
  auto page = MakePage({"BT /F1 12 Tf (FIRST) Tj ET",
                        "BT /F1 12 Tf (DEPENDENT) Tj (KEEP) Tj ET"});
  auto contents = page->GetDict()->GetObjectFor("Contents");
  std::array<CPDF_TextObject*, 2> selected = {Text(page.Get()), Text(page.Get(), 1)};
  EXPECT_EQ(Result::kTextPositionDependency,
            CPDF_RemoveOwnedTextPreservingStreams(page.Get(), selected));
  EXPECT_EQ(contents.Get(), page->GetDict()->GetObjectFor("Contents").Get());
}

TEST_F(CPDFSourceContentRemoverTest, FormWithoutBTCanReadDeletedCursor) {
  auto form = InstallForm("(KEEP) Tj");
  auto page = MakePage({"BT /F1 12 Tf (ERASE) Tj ET /Fm Do"});
  EXPECT_EQ(Result::kTextPositionDependency, Remove(page.Get()));
  EXPECT_EQ("(KEEP) Tj", Decode(form.Get()));
}

TEST_F(CPDFSourceContentRemoverTest, KeepsFormStreamAndResourceIdentityAfterQ) {
  const std::string graphics = "q .1 .2 .3 .4 k 10 20 30 40 re f Q";
  auto form = InstallForm(graphics);
  auto page = MakePage({"q BT /F1 12 Tf (ERASE) Tj ET Q /Fm Do"});
  auto resources = page->GetDict()->GetDirectObjectFor("Resources");
  EXPECT_EQ(Result::kSuccess, Remove(page.Get()));
  EXPECT_EQ(graphics, Decode(form.Get()));
  EXPECT_EQ(resources.Get(),
            page->GetDict()->GetDirectObjectFor("Resources").Get());
  EXPECT_EQ(form.Get(), resources_->GetDictFor("XObject")
                            ->GetStreamFor("Fm").Get());
}

TEST_F(CPDFSourceContentRemoverTest, RejectsDirtyDescendantInUnselectedForm) {
  InstallForm("BT /F1 12 Tf (KEEP) Tj ET");
  auto page = MakePage({"/Fm Do BT /F1 12 Tf (ERASE) Tj ET"});
  auto* form = page->GetPageObjectByIndex(0)->AsForm();
  ASSERT_NE(nullptr, form);
  ASSERT_GT(form->form()->GetPageObjectCount(), 0u);
  form->form()->GetPageObjectByIndex(0)->SetDirty(true);
  EXPECT_EQ(Result::kPageModified, Remove(page.Get()));
}

TEST_F(CPDFSourceContentRemoverTest, RejectsType3OwnedText) {
  auto font = doc_->NewIndirect<CPDF_Dictionary>();
  font->SetNewFor<CPDF_Name>("Type", "Font");
  font->SetNewFor<CPDF_Name>("Subtype", "Type3");
  auto bbox = font->SetNewFor<CPDF_Array>("FontBBox");
  for (int value : {0, 0, 600, 700}) {
    bbox->AppendNew<CPDF_Number>(value);
  }
  auto matrix = font->SetNewFor<CPDF_Array>("FontMatrix");
  for (float value : {0.001f, 0.0f, 0.0f, 0.001f, 0.0f, 0.0f}) {
    matrix->AppendNew<CPDF_Number>(value);
  }
  font->SetNewFor<CPDF_Number>("FirstChar", 65);
  font->SetNewFor<CPDF_Number>("LastChar", 65);
  font->SetNewFor<CPDF_Array>("Widths")->AppendNew<CPDF_Number>(600);
  auto encoding = font->SetNewFor<CPDF_Dictionary>("Encoding");
  auto differences = encoding->SetNewFor<CPDF_Array>("Differences");
  differences->AppendNew<CPDF_Number>(65);
  differences->AppendNew<CPDF_Name>("A");
  const std::string glyph = "600 0 0 0 600 700 d1 0 0 600 700 re f";
  auto charproc = doc_->NewIndirect<CPDF_Stream>(pdfium::as_byte_span(glyph));
  font->SetNewFor<CPDF_Dictionary>("CharProcs")
      ->SetNewFor<CPDF_Reference>("A", doc_.get(), charproc->GetObjNum());
  resources_->GetMutableDictFor("Font")
      ->SetNewFor<CPDF_Reference>("F3", doc_.get(), font->GetObjNum());
  auto page = MakePage({"BT /F3 12 Tf (A) Tj ET"});
  ASSERT_NE(nullptr, Text(page.Get()));
  EXPECT_EQ(Result::kUnsupportedText, Remove(page.Get()));
}
