// Copyright 2024 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_INIT_ISOLATE_GROUP_H_
#define V8_INIT_ISOLATE_GROUP_H_

#include <memory>

#include "src/base/page-allocator.h"
#include "src/base/platform/mutex.h"
#include "src/common/globals.h"
#include "src/flags/flags.h"
#include "src/utils/allocation.h"

namespace v8 {

namespace base {
template <typename T>
class LeakyObject;
}  // namespace base

namespace internal {

class Isolate;
class SharedReadOnlyHeap;
class ReadOnlyArtifacts;

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
// compression is disabled, isolate groups are a no-op.
//
// Note that JavaScript objects can only be passed between isolates of the same
// group.  Ensuring this invariant is the responsibility of the API user.
class V8_EXPORT_PRIVATE IsolateGroup final {
 public:
  // Create a new isolate group, allocating a fresh pointer cage if pointer
  // compression is enabled.
  //
  // The pointer cage for isolates in this group will be released when the
  // group's refcount drops to zero.  The group's initial refcount is 1.
  //
  // Note that if pointer compression is disabled, isolates are not grouped and
  // no memory is associated with the isolate group.
  static IsolateGroup* New();

  // Some configurations (e.g. V8_ENABLE_SANDBOX) put all isolates into a single
  // group.  InitializeOncePerProcess should be called early on to initialize
  // the process-wide group.  If this configuration has no process-wide isolate
  // group, the result is nullptr.
  static IsolateGroup* AcquireGlobal();

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

  bool has_shared_space_isolate() const {
    return shared_space_isolate_ != nullptr;
  }

  Isolate* shared_space_isolate() const {
    DCHECK(has_shared_space_isolate());
    return shared_space_isolate_;
  }

  void init_shared_space_isolate(Isolate* isolate) {
    DCHECK(!has_shared_space_isolate());
    shared_space_isolate_ = isolate;
  }

  void ClearSharedSpaceIsolate();

  Address read_only_heap_addr();
  void set_shared_ro_heap(SharedReadOnlyHeap* heap) { shared_ro_heap_ = heap; }

  // Remove RO artifacts if there is only one isolate left.
  void MaybeRemoveReadOnlyArtifacts();

  base::Mutex* read_only_heap_creation_mutex() {
    return &read_only_heap_creation_mutex_;
  }

  ReadOnlyArtifacts* read_only_artifacts() {
    return read_only_artifacts_.get();
  }

  ReadOnlyArtifacts* InitializeReadOnlyArtifacts();

 private:
  friend class ::v8::base::LeakyObject<IsolateGroup>;
  static IsolateGroup* GetProcessWideIsolateGroup();

  IsolateGroup();
  ~IsolateGroup();
  IsolateGroup(const IsolateGroup&) = delete;
  IsolateGroup& operator=(const IsolateGroup&) = delete;

  friend class PoolTest;
  // Only used for testing.
  static void ReleaseGlobal();

  std::atomic<int> reference_count_{1};
  v8::PageAllocator* page_allocator_ = nullptr;
  VirtualMemoryCage* trusted_pointer_compression_cage_ = nullptr;
  VirtualMemoryCage* pointer_compression_cage_ = nullptr;
  VirtualMemoryCage reservation_;

  // Mutex used to ensure that ReadOnlyArtifacts creation is only done once.
  base::Mutex read_only_heap_creation_mutex_;

  std::unique_ptr<ReadOnlyArtifacts> read_only_artifacts_;
  SharedReadOnlyHeap* shared_ro_heap_ = nullptr;
  Isolate* shared_space_isolate_ = nullptr;
};

}  // namespace internal
}  // namespace v8

#endif  // V8_INIT_ISOLATE_GROUP_H_
