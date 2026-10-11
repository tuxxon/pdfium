// Copyright 2026 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CORE_FPDFAPI_PAGE_CPDF_SOURCECONTENT_H_
#define CORE_FPDFAPI_PAGE_CPDF_SOURCECONTENT_H_

#include <stddef.h>
#include <stdint.h>

#include <optional>
#include <vector>

#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fxcrt/bytestring.h"
#include "core/fxcrt/data_vector.h"
#include "core/fxcrt/retain_ptr.h"

// Provenance of an unedited top-level page parse. Forms deliberately do not
// record provenance. Offsets address PDFium's concatenated decoded Contents,
// including the one-byte separator inserted between array entries.
struct CPDF_SourceContent {
  struct Stream {
    RetainPtr<const CPDF_Stream> identity;
    DataVector<uint8_t> decoded;
    uint32_t offset = 0;
  };

  struct Operation {
    ByteString name;
    uint32_t begin = 0;
    uint32_t end = 0;
    bool valid_operands = false;
    std::optional<size_t> text_object_index;
  };

  // Bound memory overhead; an ordinary page can still parse when capture is
  // unsupported. Removal then fails closed.
  static constexpr size_t kMaxDecodedBytes = 64 * 1024 * 1024;
  static constexpr size_t kMaxOperations = 1000000;

  std::vector<Stream> streams;
  std::vector<Operation> operations;
  // Numeric identities are never dereferenced. They detect removal/reordering
  // of objects without leaving dangling observer pointers after ordinary edits.
  std::vector<uintptr_t> object_identities;
  bool valid = true;
  bool complete = false;
  bool consumed = false;
};

#endif  // CORE_FPDFAPI_PAGE_CPDF_SOURCECONTENT_H_
