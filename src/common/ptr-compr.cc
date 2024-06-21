// Copyright 2022 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/common/ptr-compr-inl.h"

namespace v8::internal {

#ifdef V8_COMPRESS_POINTERS

#ifdef V8_COMPRESS_POINTERS_IN_SHARED_CAGE
#define THREAD_LOCAL_IF_MULTICAGE
#else
#define THREAD_LOCAL_IF_MULTICAGE thread_local
#endif  // V8_COMPRESS_POINTERS_IN_SHARED_CAGE

THREAD_LOCAL_IF_MULTICAGE uintptr_t MainCage::base_ = kNullAddress;

// static
Address MainCage::base_non_inlined() { return base_; }

// static
void MainCage::set_base_non_inlined(Address base) { base_ = base; }

#ifdef V8_ENABLE_SANDBOX
uintptr_t TrustedCage::base_ = kNullAddress;
#endif  // V8_ENABLE_SANDBOX

#ifdef V8_EXTERNAL_CODE_SPACE
THREAD_LOCAL_IF_MULTICAGE uintptr_t ExternalCodeCompressionScheme::base_ =
    kNullAddress;

// static
Address ExternalCodeCompressionScheme::base_non_inlined() { return base_; }

// static
void ExternalCodeCompressionScheme::set_base_non_inlined(Address base) {
  base_ = base;
}
#endif  // V8_EXTERNAL_CODE_SPACE

#ifdef V8_COMPRESS_POINTERS_IN_MULTIPLE_CAGES
thread_local Address ReadOnlyHeapAddrAccess::ro_heap_addr_ = kNullAddress;
// static
Address ReadOnlyHeapAddrAccess::heap_addr() { return ro_heap_addr_; }
// static
void ReadOnlyHeapAddrAccess::set_heap_addr(Address heap_addr) {
  ro_heap_addr_ = heap_addr;
}
#endif  // V8_COMPRESS_POINTERS_IN_MULTIPLE_CAGES

#undef THREAD_LOCAL_IF_MULTICAGE

#endif  // V8_COMPRESS_POINTERS

}  // namespace v8::internal
