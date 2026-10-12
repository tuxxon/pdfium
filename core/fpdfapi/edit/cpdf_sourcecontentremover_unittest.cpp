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

  // Adds a named XObject beside existing ones. A Form gets a snapshot of the
  // current resources, so XObjects added earlier are visible inside it.
  RetainPtr<CPDF_Stream> AddXObject(const std::string& name,
                                    const std::string& content,
                                    const char* subtype,
                                    bool own_resources = true) {
    auto xobject =
        doc_->NewIndirect<CPDF_Stream>(pdfium::as_byte_span(content));
    auto dict = xobject->GetMutableDict();
    dict->SetNewFor<CPDF_Name>("Type", "XObject");
    dict->SetNewFor<CPDF_Name>("Subtype", subtype);
    if (std::string(subtype) == "Form") {
      auto box = dict->SetNewFor<CPDF_Array>("BBox");
      for (int value : {0, 0, 300, 300}) {
        box->AppendNew<CPDF_Number>(value);
      }
      if (own_resources) {
        dict->SetFor("Resources", resources_->Clone());
      }
    } else if (std::string(subtype) == "Image") {
      dict->SetNewFor<CPDF_Number>("Width", 1);
      dict->SetNewFor<CPDF_Number>("Height", 1);
      dict->SetNewFor<CPDF_Number>("BitsPerComponent", 8);
      dict->SetNewFor<CPDF_Name>("ColorSpace", "DeviceGray");
    }
    resources_->GetOrCreateDictFor("XObject")->SetNewFor<CPDF_Reference>(
        ByteString(name.c_str()), doc_.get(), xobject->GetObjNum());
    return xobject;
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

// From task 28 (tuxxon/pdfium 1ae4821): ISO 32000 keeps the text matrices out
// of the graphics state, so Q does not restore them in a spec viewer.
TEST_F(CPDFSourceContentRemoverTest, QDoesNotRestoreTaintedCursor) {
  // PDFium restores the text position on Q; spec viewers do not.
  auto page = MakePage({"BT /F1 12 Tf q (ERASE) Tj Q (KEEP) Tj ET"});
  auto contents = page->GetDict()->GetObjectFor("Contents");
  EXPECT_EQ(Result::kTextPositionDependency, Remove(page.Get()));
  EXPECT_EQ(contents.Get(), page->GetDict()->GetObjectFor("Contents").Get());
}

TEST_F(CPDFSourceContentRemoverTest, CursorResetInsideOrAfterQClearsTaint) {
  for (const char* body : {"q (ERASE) Tj 0 -14 Td Q (KEEP) Tj",
                           "q (ERASE) Tj Q 0 -14 Td (KEEP) Tj"}) {
    auto page = MakePage({std::string("BT /F1 12 Tf ") + body + " ET"});
    EXPECT_EQ(Result::kSuccess, Remove(page.Get())) << body;
  }
}

TEST_F(CPDFSourceContentRemoverTest, SpecTaintEndsAtETAndQRestoresTheRest) {
  // Q restores PDFium's untainted position and ET ends the spec matrices, so
  // even a Form that reads the inherited position is unaffected.
  AddXObject("Fm", "(KEEP) Tj", "Form");
  auto page = MakePage({"BT /F1 12 Tf q (ERASE) Tj Q ET /Fm Do"});
  EXPECT_EQ(Result::kSuccess, Remove(page.Get()));
}

TEST_F(CPDFSourceContentRemoverTest, RetainedQuoteResetsBothModels) {
  for (const char* show : {"(KEEP) '", "1 2 (KEEP) \""}) {
    auto page = MakePage({std::string("BT /F1 12 Tf 14 TL (ERASE) Tj ") +
                          show + " ET"});
    EXPECT_EQ(Result::kSuccess, Remove(page.Get())) << show;
  }
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

// pdfTeX figures: the last line before an included PDF figure is removed, then
// ET ... /Im1 Do. A Form whose text shows all sit in its own BT/ET cannot read
// the caller's cursor (BT resets it), so the deletion must be accepted.
TEST_F(CPDFSourceContentRemoverTest, FormWithOwnTextObjectsIgnoresDeletedCursor) {
  const std::string figure = "q BT /F1 12 Tf 5 5 Td (KEEP) Tj ET Q";
  auto form = AddXObject("Im1", figure, "Form");
  const std::string content =
      "BT /F1 12 Tf 10 10 Td (ERASE) Tj ET q 1 0 0 1 0 0 cm /Im1 Do Q";
  auto page = MakePage({content});
  EXPECT_EQ(Result::kSuccess, Remove(page.Get()));
  EXPECT_EQ(Blank(content, "(ERASE) Tj"), Decode(Stream(page.Get()).Get()));
  EXPECT_EQ(figure, Decode(form.Get()));
}

TEST_F(CPDFSourceContentRemoverTest, ImageXObjectIgnoresDeletedCursor) {
  AddXObject("Im", std::string(1, '\x80'), "Image");
  const std::string content = "BT /F1 12 Tf (ERASE) Tj ET /Im Do";
  auto page = MakePage({content});
  EXPECT_EQ(Result::kSuccess, Remove(page.Get()));
  EXPECT_EQ(Blank(content, "(ERASE) Tj"), Decode(Stream(page.Get()).Get()));
}

TEST_F(CPDFSourceContentRemoverTest, FormShowOutsideItsTextObjectRejects) {
  for (const char* figure :
       {"BT ET (KEEP) Tj", "q BT ET Q [(KEEP)] TJ", "BT ET (KEEP) '",
        "BT ET 1 2 (KEEP) \""}) {
    AddXObject("Im1", figure, "Form");
    auto page = MakePage({"BT /F1 12 Tf (ERASE) Tj ET /Im1 Do"});
    EXPECT_EQ(Result::kTextPositionDependency, Remove(page.Get())) << figure;
  }
}

TEST_F(CPDFSourceContentRemoverTest, NestedFormsAreCheckedRecursively) {
  AddXObject("Inner", "(KEEP) Tj", "Form");
  AddXObject("Outer", "q /Inner Do Q", "Form");
  auto rejected = MakePage({"BT /F1 12 Tf (ERASE) Tj ET /Outer Do"});
  EXPECT_EQ(Result::kTextPositionDependency, Remove(rejected.Get()));

  AddXObject("Inner", "BT /F1 12 Tf (KEEP) Tj ET", "Form");
  AddXObject("Outer", "q /Inner Do Q", "Form");
  auto accepted = MakePage({"BT /F1 12 Tf (ERASE) Tj ET /Outer Do"});
  EXPECT_EQ(Result::kSuccess, Remove(accepted.Get()));
}

TEST_F(CPDFSourceContentRemoverTest, FormDoesNotBorrowThePagesXObjects) {
  // Here both resolve the page's /Im; nested deeper, PDFium falls back to the
  // page and poppler to the caller chain, so never fall back.
  AddXObject("Outer", "q /Im Do Q", "Form");
  AddXObject("Im", std::string(1, '\x80'), "Image");
  auto page = MakePage({"BT /F1 12 Tf (ERASE) Tj ET /Outer Do"});
  EXPECT_EQ(Result::kTextPositionDependency, Remove(page.Get()));
}

TEST_F(CPDFSourceContentRemoverTest, FormKeywordMustBeAKnownOperator) {
  // pdf.js splits "Tjq" into Tj and q, and reads a stray byte as its own token.
  for (const char* figure : {"(KEEP) Tjq", "(KEEP) \x80Tj"}) {
    AddXObject("Im1", figure, "Form");
    auto page = MakePage({"BT /F1 12 Tf (ERASE) Tj ET /Im1 Do"});
    EXPECT_EQ(Result::kTextPositionDependency, Remove(page.Get())) << figure;
  }
}

TEST_F(CPDFSourceContentRemoverTest, FormQCanRestoreTheCallersCursor) {
  // q/Q save the text position in PDFium, pdf.js and poppler alike.
  for (const char* figure : {"q BT Q (KEEP) Tj ET", "BT Q (KEEP) Tj ET"}) {
    AddXObject("Im1", figure, "Form");
    auto page = MakePage({"BT /F1 12 Tf (ERASE) Tj ET /Im1 Do"});
    EXPECT_EQ(Result::kTextPositionDependency, Remove(page.Get())) << figure;
  }
}

TEST_F(CPDFSourceContentRemoverTest, FormDoInsideRestoredTextObjectRejects) {
  // Q brings back the caller's position inside BT; Do is not allowed there.
  AddXObject("Im", std::string(1, '\x80'), "Image");
  AddXObject("Im1", "q BT Q /Im Do ET", "Form");
  auto page = MakePage({"BT /F1 12 Tf (ERASE) Tj ET /Im1 Do"});
  EXPECT_EQ(Result::kTextPositionDependency, Remove(page.Get()));
}

TEST_F(CPDFSourceContentRemoverTest, FormWithMalformedDictionaryRejects) {
  // CPDF_StreamParser re-lexes BT after a non-name key; pdf.js swallows it.
  AddXObject("Im1", "<< 1 BT >> (KEEP) Tj ET", "Form");
  auto page = MakePage({"BT /F1 12 Tf (ERASE) Tj ET /Im1 Do"});
  EXPECT_EQ(Result::kTextPositionDependency, Remove(page.Get()));
}

TEST_F(CPDFSourceContentRemoverTest, FormBytesOtherViewersMayReadRejects) {
  // An external /F stream, or a filter that fails and leaves PDFium the raw
  // bytes, can paint something other than what the scan saw.
  auto external = AddXObject("Ext", "BT /F1 12 Tf (KEEP) Tj ET", "Form");
  external->GetMutableDict()->SetNewFor<CPDF_Name>("F", "figure.pdf");
  auto broken = AddXObject("Bad", "BT /F1 12 Tf (KEEP) Tj ET", "Form");
  broken->GetMutableDict()->SetNewFor<CPDF_Name>("Filter", "FlateDecode");
  for (const char* name : {"/Ext", "/Bad"}) {
    auto page = MakePage(
        {std::string("BT /F1 12 Tf (ERASE) Tj ET ") + name + " Do"});
    EXPECT_EQ(Result::kTextPositionDependency, Remove(page.Get())) << name;
  }
}

TEST_F(CPDFSourceContentRemoverTest, DoWithSeveralOperandsRejects) {
  // PDFium paints the last operand (/Fm, unsafe), not the first (/Im, safe).
  AddXObject("Im", std::string(1, '\x80'), "Image");
  AddXObject("Fm", "(KEEP) Tj", "Form");
  auto page = MakePage({"BT /F1 12 Tf (ERASE) Tj ET /Im /Fm Do"});
  // The parser marks such a Do invalid, so the operand check rejects first.
  EXPECT_EQ(Result::kUnsupportedContent, Remove(page.Get()));
}

TEST_F(CPDFSourceContentRemoverTest, DoSplitAcrossStreamsRejects) {
  AddXObject("Im", std::string(1, '\x80'), "Image");
  auto page = MakePage({"BT /F1 12 Tf (ERASE) Tj ET /Im", "Do"});
  EXPECT_EQ(Result::kTextPositionDependency, Remove(page.Get()));
}

TEST_F(CPDFSourceContentRemoverTest, FormChainIsBoundedAtDepthSixteen) {
  // Nesting is bounded so a long chain cannot exhaust the stack. The top-level
  // Form is depth 0; a safe Form at depth 16 passes, one at depth 17 does not.
  for (int last : {16, 17}) {
    const std::string prefix = "C" + std::to_string(last) + "_";
    AddXObject(prefix + std::to_string(last), "BT /F1 12 Tf (KEEP) Tj ET",
               "Form");
    for (int i = last - 1; i >= 0; --i) {
      AddXObject(prefix + std::to_string(i),
                 "/" + prefix + std::to_string(i + 1) + " Do", "Form");
    }
    auto page = MakePage({"BT /F1 12 Tf (ERASE) Tj ET /" + prefix + "0 Do"});
    EXPECT_EQ(last == 16 ? Result::kSuccess : Result::kTextPositionDependency,
              Remove(page.Get()))
        << last;
  }
}

TEST_F(CPDFSourceContentRemoverTest, NestedDoPaintsOnlyItsLoneNameOperand) {
  // PDFium paints the last operand, here the string (Fm), not /Im.
  AddXObject("Im", std::string(1, '\x80'), "Image");
  AddXObject("Fm", "(KEEP) Tj", "Form");
  AddXObject("Outer", "/Im (Fm) Do", "Form");
  auto page = MakePage({"BT /F1 12 Tf (ERASE) Tj ET /Outer Do"});
  EXPECT_EQ(Result::kTextPositionDependency, Remove(page.Get()));
}

TEST_F(CPDFSourceContentRemoverTest, NestedNamesResolveInTheFormsResources) {
  // Outer's own /X is unsafe while the page's /X is an Image.
  AddXObject("X", "(KEEP) Tj", "Form");
  AddXObject("Outer", "/X Do", "Form");
  AddXObject("X", std::string(1, '\x80'), "Image");
  auto page = MakePage({"BT /F1 12 Tf (ERASE) Tj ET /Outer Do"});
  EXPECT_EQ(Result::kTextPositionDependency, Remove(page.Get()));
}

TEST_F(CPDFSourceContentRemoverTest, FormWithoutResourcesUsesTheCallers) {
  AddXObject("Im", std::string(1, '\x80'), "Image");
  AddXObject("Bare", "/Im Do", "Form", /*own_resources=*/false);
  auto page = MakePage({"BT /F1 12 Tf (ERASE) Tj ET /Bare Do"});
  EXPECT_EQ(Result::kSuccess, Remove(page.Get()));
}

TEST_F(CPDFSourceContentRemoverTest, DoInsideTextObjectAfterDeletedShowRejects) {
  // ISO 32000 does not allow Do inside BT/ET; keep rejecting it when tainted.
  AddXObject("Im", std::string(1, '\x80'), "Image");
  auto page = MakePage({"BT /F1 12 Tf (ERASE) Tj /Im Do ET"});
  EXPECT_EQ(Result::kTextPositionDependency, Remove(page.Get()));
}

TEST_F(CPDFSourceContentRemoverTest, SelfReferencingFormRejects) {
  // Without its own resources the Form uses the page's, which name it again;
  // the scan recurses until the depth limit.
  AddXObject("Loop", "/Loop Do", "Form", /*own_resources=*/false);
  auto page = MakePage({"BT /F1 12 Tf (ERASE) Tj ET /Loop Do"});
  EXPECT_EQ(Result::kTextPositionDependency, Remove(page.Get()));
}

TEST_F(CPDFSourceContentRemoverTest, UnprovenXObjectsAfterDeletedCursorReject) {
  // Inline image data cannot be tokenized safely; PostScript XObjects and
  // missing names are unknown.
  AddXObject("Inline", "BI /W 1 /H 1 /BPC 8 /CS /G ID \x80 EI", "Form");
  AddXObject("Ps", "BT /F1 12 Tf (KEEP) Tj ET", "PS");
  for (const char* name : {"/Inline", "/Ps", "/Missing"}) {
    auto page = MakePage(
        {std::string("BT /F1 12 Tf (ERASE) Tj ET ") + name + " Do"});
    EXPECT_EQ(Result::kTextPositionDependency, Remove(page.Get())) << name;
  }
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
