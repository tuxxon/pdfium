// Copyright 2026 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CORE_FPDFAPI_EDIT_CPDF_SOURCECONTENTREMOVER_H_
#define CORE_FPDFAPI_EDIT_CPDF_SOURCECONTENTREMOVER_H_

#include "core/fxcrt/span.h"

class CPDF_Page;
class CPDF_TextObject;

enum class CPDF_SourceTextRemovalResult {
  kSuccess,
  kInvalidSelection,
  kNoSourceProvenance,
  kPageModified,
  kUnsupportedContent,
  kUnsupportedText,
  kCrossStreamOperator,
  kTextPositionDependency,
  kSourceChanged,
};

// Use ONLY on a freshly loaded, fully parsed original page. The caller must
// validate ownership (exact native index/text/bounds) before passing pointers.
// Supports whole top-level mode-0 non-Type3 Tj/TJ objects whose deleted cursor
// advances cannot affect any retained text or XObject invocation.
//
// All validation precedes page mutation. Failure leaves the page dictionary
// unchanged. Success replaces /Contents by page-local copy-on-write streams,
// preserving every decoded byte outside the selected operand/operator ranges,
// all resources, and the rest of the page dictionary.
//
// Success deliberately does NOT update the loaded page-object cache. Immediately
// SaveAsCopy WITHOUT GenerateContent, then close/reload before rendering or any
// further editing. Never combine with RemoveObject or GenerateContent.
CPDF_SourceTextRemovalResult CPDF_RemoveOwnedTextPreservingStreams(
    CPDF_Page* page,
    pdfium::span<CPDF_TextObject* const> owned_text);

#endif  // CORE_FPDFAPI_EDIT_CPDF_SOURCECONTENTREMOVER_H_
