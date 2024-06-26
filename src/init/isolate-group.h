// Copyright 2024 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_INIT_ISOLATE_GROUP_H_
#define V8_INIT_ISOLATE_GROUP_H_

#include <memory>

#include "src/base/page-allocator.h"
#include "src/common/globals.h"
#include "src/flags/flags.h"
#include "src/utils/allocation.h"

namespace v8 {

namespace base {
template <typename T>
class LeakyObject;
}  // namespace base

namespace internal {

#ifdef V8_ENABLE_SANDBOX
class Sandbox;
#endif

// An IsolateGroup allows an API user to control which isolates get allocated
// together in a shared pointer cage.
//
// The standard configuration of V8 is to enable pointer compression and to
// allocate all isolates in a single shared pointer cage
// (V8_COMPRESS_POINTERS_IN_SHARED_CAGE).  This also enables the sandbox
// (V8_ENABLE_SANDBOX), of which there can currently be only one per process, as
// it requires a large part of the virtual address space.
//
// The standard configuration comes with a limitation, in that the total size of
// the compressed pointer cage is limited to 4 GB.  Some API users would like
// pointer compression but also want to avoid the 4 GB limit of the shared
// pointer cage.  Isolate groups allow users to declare which isolates should be
// co-located in a single pointer cage.
//
// Isolate groups are useful only if pointer compression is enabled.  Otherwise,
// the isolate could just allocate pages from the global system allocator;
// there's no need to stay within any particular address range.  If pointer
// compression is disabled, there is just one global isolate group.
//
// Note that JavaScript objects can only be passed between isolates of the same
// group.  Ensuring this invariant is the responsibility of the API user.
class V8_EXPORT_PRIVATE IsolateGroup final {
 public:
  // InitializeOncePerProcess should be called early on to initialize the
  // process-wide group.
  static IsolateGroup* AcquireDefault() { return GetDefault()->Acquire(); }

  // Return true if we can create additional isolate groups: only the case if
  // multiple pointer cages were configured in at build-time.
  static constexpr bool CanCreateNewGroups() {
    return COMPRESS_POINTERS_IN_MULTIPLE_CAGES_BOOL;
  }

  // Create a new isolate group, allocating a fresh pointer cage if pointer
  // compression is enabled.  If new groups cannot be created in this build
  // configuration, abort.
  //
  // The pointer cage for isolates in this group will be released when the
  // group's refcount drops to zero.  The group's initial refcount is 1.
  static IsolateGroup* New();

  static void InitializeOncePerProcess();

  // Obtain a fresh reference on the isolate group.
  IsolateGroup* Acquire() {
    DCHECK_LT(0, reference_count_.load());
    reference_count_++;
    return this;
  }

  // Release a reference on an isolate group, possibly freeing any shared memory
  // resources.
  void Release() {
    DCHECK_LT(0, reference_count_.load());
    if (--reference_count_ == 0) delete this;
  }

  v8::PageAllocator* page_allocator() const { return page_allocator_; }

  VirtualMemoryCage* GetPtrComprCage() const {
    return pointer_compression_cage_;
  }
  VirtualMemoryCage* GetTrustedPtrComprCage() const {
    return trusted_pointer_compression_cage_;
  }

  Address GetPtrComprCageBase() const { return GetPtrComprCage()->base(); }
  Address GetTrustedPtrComprCageBase() const {
    return GetTrustedPtrComprCage()->base();
  }

 private:
  friend class ::v8::base::LeakyObject<IsolateGroup>;
  friend class PoolTest;

  IsolateGroup() {}
  ~IsolateGroup() { DCHECK_EQ(reference_count_.load(), 0); }
  IsolateGroup(const IsolateGroup&) = delete;
  IsolateGroup& operator=(const IsolateGroup&) = delete;

  static IsolateGroup* GetDefault();
  // Only used for testing.
  static void ReleaseDefault();

#ifdef V8_ENABLE_SANDBOX
  void Initialize(Sandbox* sandbox);
#else   // V8_ENABLE_SANDBOX
  void Initialize();
#endif  // V8_ENABLE_SANDBOX

  std::atomic<int> reference_count_{1};
  v8::PageAllocator* page_allocator_ = nullptr;
  VirtualMemoryCage* trusted_pointer_compression_cage_ = nullptr;
  VirtualMemoryCage* pointer_compression_cage_ = nullptr;
  VirtualMemoryCage reservation_;
};

}  // namespace internal
}  // namespace v8

#endif  // V8_INIT_ISOLATE_GROUP_H_
