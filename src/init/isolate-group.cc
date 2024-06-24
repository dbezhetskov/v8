// Copyright 2018 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/init/isolate-group.h"

#include "src/base/bounded-page-allocator.h"
#include "src/base/platform/memory.h"
#include "src/common/ptr-compr-inl.h"
#include "src/execution/isolate.h"
#include "src/heap/code-range.h"
#include "src/heap/read-only-spaces.h"
#include "src/heap/trusted-range.h"
#include "src/sandbox/sandbox.h"
#include "src/utils/memcopy.h"
#include "src/utils/utils.h"

namespace v8 {
namespace internal {

#ifdef V8_COMPRESS_POINTERS
struct PtrComprCageReservationParams
    : public VirtualMemoryCage::ReservationParams {
  PtrComprCageReservationParams() {
    page_allocator = GetPlatformPageAllocator();

    reservation_size = kPtrComprCageReservationSize;
    base_alignment = kPtrComprCageBaseAlignment;

    // Simplify BoundedPageAllocator's life by configuring it to use same page
    // size as the Heap will use (MemoryChunk::kPageSize).
    page_size =
        RoundUp(size_t{1} << kPageSizeBits, page_allocator->AllocatePageSize());
    requested_start_hint = RoundDown(
        reinterpret_cast<Address>(page_allocator->GetRandomMmapAddr()),
        base_alignment);

#if V8_OS_FUCHSIA && !V8_EXTERNAL_CODE_SPACE
    // If external code space is not enabled then executable pages (e.g. copied
    // builtins, and JIT pages) will fall under the pointer compression range.
    // Under Fuchsia that means the entire range must be allocated as JITtable.
    permissions = PageAllocator::Permission::kNoAccessWillJitLater;
#else
    permissions = PageAllocator::Permission::kNoAccess;
#endif
    page_initialization_mode =
        base::PageInitializationMode::kAllocatedPagesCanBeUninitialized;
    page_freeing_mode = base::PageFreeingMode::kMakeInaccessible;
  }
};
#endif  // V8_COMPRESS_POINTERS

#ifndef V8_COMPRESS_POINTERS_IN_MULTIPLE_CAGES
// static
IsolateGroup* IsolateGroup::GetProcessWideIsolateGroup() {
  static ::v8::base::LeakyObject<IsolateGroup> global_isolate_group_;
  return global_isolate_group_.get();
}
#endif  // !V8_COMPRESS_POINTERS_IN_MULTIPLE_CAGES

IsolateGroup::IsolateGroup() {
  metadata_pointer_table_ = std::make_unique<Address[]>(MemoryChunk::MetadataPointerTableSize());
}

IsolateGroup::~IsolateGroup() { DCHECK_EQ(reference_count_.load(), 0); }

// static
void IsolateGroup::InitializeOncePerProcess() {
#ifndef V8_COMPRESS_POINTERS_IN_MULTIPLE_CAGES
  IsolateGroup* group = GetProcessWideIsolateGroup();
  DCHECK(!group->reservation_.IsReserved());

#ifdef V8_COMPRESS_POINTERS_IN_SHARED_CAGE
  PtrComprCageReservationParams params;
  base::AddressRegion existing_reservation;
#ifdef V8_ENABLE_SANDBOX
  // The pointer compression cage must be placed at the start of the sandbox.
  auto sandbox = GetProcessWideSandbox();
  CHECK(sandbox->is_initialized());
  Address base = sandbox->address_space()->AllocatePages(
    sandbox->base(), params.reservation_size, params.base_alignment,
    PagePermissions::kNoAccess);
  CHECK_EQ(sandbox->base(), base);
  existing_reservation = base::AddressRegion(base, params.reservation_size);
  params.page_allocator = sandbox->page_allocator();
#endif  // V8_ENABLE_SANDBOX
  if (!group->reservation_.InitReservation(params, existing_reservation)) {
    V8::FatalProcessOutOfMemory(
      nullptr,
      "Failed to reserve virtual memory for process-wide V8 "
      "pointer compression cage");
  }
  group->page_allocator_ = group->reservation_.page_allocator();
  group->pointer_compression_cage_ = &group->reservation_;
  V8HeapCompressionScheme::InitBase(group->GetPtrComprCageBase());
#ifdef V8_EXTERNAL_CODE_SPACE
  // Speculatively set the code cage base to the same value in case jitless
  // mode will be used. Once the process-wide CodeRange instance is created
  // the code cage base will be set accordingly.
  ExternalCodeCompressionScheme::InitBase(V8HeapCompressionScheme::base());
#endif  // V8_EXTERNAL_CODE_SPACE

#ifdef V8_ENABLE_SANDBOX
  TrustedRange::EnsureProcessWideTrustedRange(kMaximalTrustedRangeSize);
  group->trusted_pointer_compression_cage_ =
    TrustedRange::GetProcessWideTrustedRange();
#else
  group->trusted_pointer_compression_cage_ = &group->reservation_;
#endif  // V8_ENABLE_SANDBOX
#endif  // V8_COMPRESS_POINTERS_IN_SHARED_CAGE

#if !COMPRESS_POINTERS_BOOL
  group->pointer_compression_cage_ = &group->reservation_;
  group->trusted_pointer_compression_cage_ = &group->reservation_;
  group->page_allocator_ = GetPlatformPageAllocator();
#endif  // !COMPRESS_POINTERS_BOOL

#endif  // !V8_COMPRESS_POINTERS_IN_MULTIPLE_CAGES

#if defined(V8_ENABLE_SANDBOX) && defined(V8_COMPRESS_POINTERS_IN_MULTIPLE_CAGES)
  TrustedRange::EnsureProcessWideTrustedRange(kMaximalTrustedRangeSize);
#endif
}

void IsolateGroup::ClearSharedSpaceIsolate() {
  // Can only clear shared space isolate when no other isolates are live.
#ifdef V8_COMPRESS_POINTERS_IN_MULTIPLE_CAGES
  // The only reference to the group is from the shared space isolate.
  CHECK_EQ(1, reference_count_.load());
#else
  // For shared-cage mode and for mode without pointer compression
  // the process wide isolate group holds a link too.
  CHECK_EQ(2, reference_count_.load());
#endif
  DCHECK(has_shared_space_isolate());
  shared_space_isolate_ = nullptr;
}

Address IsolateGroup::read_only_heap_addr() {
  return reinterpret_cast<Address>(&shared_ro_heap_);
}

void IsolateGroup::MaybeRemoveReadOnlyArtifacts() {
  // RC == 2 because isolate group also holds a link.
  if (reference_count_.load() == 2) {
    read_only_artifacts_.reset();
  }
}

ReadOnlyArtifacts* IsolateGroup::InitializeReadOnlyArtifacts() {
  DCHECK(!read_only_artifacts_);
  read_only_artifacts_ = std::make_unique<ReadOnlyArtifacts>();
  return read_only_artifacts_.get();
}

// static
IsolateGroup* IsolateGroup::New() {
  IsolateGroup* group = new IsolateGroup;

#if defined(V8_COMPRESS_POINTERS_IN_MULTIPLE_CAGES)
  PtrComprCageReservationParams params;
  if (!group->reservation_.InitReservation(params)) {
    V8::FatalProcessOutOfMemory(
        nullptr,
        "Failed to reserve memory for new isolate group");
  }
  group->pointer_compression_cage_ = &group->reservation_;
  group->trusted_pointer_compression_cage_ = &group->reservation_;
  group->page_allocator_ = group->pointer_compression_cage_->page_allocator();
#elif defined(V8_COMPRESS_POINTERS_IN_SHARED_CAGE)
  FATAL("Can't create new isolate groups with shared pointer compression cage");
#else
  group->pointer_compression_cage_ = &group->reservation_;
  group->trusted_pointer_compression_cage_ = &group->reservation_;
  group->page_allocator_ = GetPlatformPageAllocator();
#endif

  CHECK_NOT_NULL(group->page_allocator_);
  return group;
}

// static
IsolateGroup* IsolateGroup::AcquireGlobal() {
#ifndef V8_COMPRESS_POINTERS_IN_MULTIPLE_CAGES
  return GetProcessWideIsolateGroup()->Acquire();
#else
  return nullptr;
#endif  // !V8_COMPRESS_POINTERS_IN_MULTIPLE_CAGES
}

// static
void IsolateGroup::ReleaseGlobal() {
#ifndef V8_COMPRESS_POINTERS_IN_MULTIPLE_CAGES
  if (CodeRange* code_range = CodeRange::GetProcessWideCodeRange()) {
    code_range->Free();
  }

  IsolateGroup* group = GetProcessWideIsolateGroup();
  CHECK_EQ(group->reference_count_.load(), 1);
  CHECK(!group->has_shared_space_isolate());
  group->page_allocator_ = nullptr;
  group->trusted_pointer_compression_cage_ = nullptr;
  group->pointer_compression_cage_ = nullptr;
  group->reservation_.Free();
#endif  // !V8_COMPRESS_POINTERS_IN_MULTIPLE_CAGES
}

}  // namespace internal
}  // namespace v8
