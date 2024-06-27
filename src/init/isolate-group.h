// Copyright 2024 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_INIT_ISOLATE_GROUP_H_
#define V8_INIT_ISOLATE_GROUP_H_

#include <memory>

#include "src/base/once.h"
#include "src/base/page-allocator.h"
#include "src/base/platform/mutex.h"
#include "src/codegen/external-reference-table.h"
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
class MemoryChunk;
#endif
class CodeRange;
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

  static void InitializeIsolateIndependentExternalRefTable();

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

  CodeRange* EnsureCodeRange(size_t requested_size);
  CodeRange* GetCodeRange() const { return code_range_.get(); }

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

  Address* external_ref_table() { return external_ref_table_; }

 private:
  friend class ::v8::base::LeakyObject<IsolateGroup>;
  friend class PoolTest;
  friend class MemoryChunk;

  IsolateGroup();
  ~IsolateGroup();
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

  ::v8::base::OnceType init_code_range_ = V8_ONCE_INIT;
  std::unique_ptr<CodeRange> code_range_;

  // Mutex used to ensure that ReadOnlyArtifacts creation is only done once.
  base::Mutex read_only_heap_creation_mutex_;

  std::unique_ptr<ReadOnlyArtifacts> read_only_artifacts_;
  SharedReadOnlyHeap* shared_ro_heap_ = nullptr;
  Isolate* shared_space_isolate_ = nullptr;

  Address external_ref_table_[ExternalReferenceTable::kSizeIsolateIndependent] = {0};
};

}  // namespace internal
}  // namespace v8

#endif  // V8_INIT_ISOLATE_GROUP_H_
