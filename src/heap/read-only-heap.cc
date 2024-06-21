// Copyright 2019 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/heap/read-only-heap.h"

#include <cstddef>
#include <cstring>

#include "src/common/globals.h"
#include "src/common/ptr-compr-inl.h"
#include "src/heap/heap-write-barrier-inl.h"
#include "src/heap/memory-chunk-metadata.h"
#include "src/heap/mutable-page-metadata.h"
#include "src/heap/read-only-spaces.h"
#include "src/heap/third-party/heap-api.h"
#include "src/init/isolate-group.h"
#include "src/objects/heap-object-inl.h"
#include "src/objects/objects-inl.h"
#include "src/objects/smi.h"
#include "src/snapshot/read-only-deserializer.h"
#include "src/utils/allocation.h"

namespace v8 {
namespace internal {

#ifndef V8_COMPRESS_POINTERS_IN_MULTIPLE_CAGES
// This ReadOnlyHeap instance will only be accessed by Isolates that are already
// set up. As such it doesn't need to be guarded by a mutex.
SharedReadOnlyHeap* SharedReadOnlyHeap::shared_ro_heap_ = nullptr;
#endif  // !V8_COMPRESS_POINTERS_IN_MULTIPLE_CAGES

ReadOnlyHeap::~ReadOnlyHeap() {
#ifdef V8_ENABLE_SANDBOX
  GetProcessWideCodePointerTable()->TearDownSpace(&code_pointer_space_);
#endif
}

// static
void ReadOnlyHeap::SetUp(Isolate* isolate,
                         SnapshotData* read_only_snapshot_data,
                         bool can_rehash) {
  DCHECK_NOT_NULL(isolate);

  if (IsReadOnlySpaceShared()) {
    ReadOnlyHeap* ro_heap;
    auto* group = isolate->isolate_group();
    if (read_only_snapshot_data != nullptr) {
      bool read_only_heap_created = false;
      base::MutexGuard guard(group->read_only_heap_creation_mutex());
      auto* artifacts = group->read_only_artifacts();
#ifndef V8_COMPRESS_POINTERS_IN_MULTIPLE_CAGES
      ReadOnlyArtifacts::read_only_artifacts_ = artifacts;
#endif  // !V8_COMPRESS_POINTERS_IN_MULTIPLE_CAGES
      if (!artifacts) {
        artifacts = group->InitializeReadOnlyArtifacts();
        artifacts->InitializeChecksum(read_only_snapshot_data);
        ro_heap = CreateInitialHeapForBootstrapping(isolate, artifacts);
        ro_heap->DeserializeIntoIsolate(isolate, read_only_snapshot_data,
                                        can_rehash);
        artifacts->set_initial_next_unique_sfi_id(
            isolate->next_unique_sfi_id());
        read_only_heap_created = true;
      } else {
        ro_heap = artifacts->read_only_heap();
        isolate->SetUpFromReadOnlyArtifacts(artifacts, ro_heap);
#ifdef V8_COMPRESS_POINTERS
        isolate->external_pointer_table().SetUpFromReadOnlyArtifacts(
            isolate->heap()->read_only_external_pointer_space(), artifacts);
#endif  // V8_COMPRESS_POINTERS
      }
      artifacts->VerifyChecksum(read_only_snapshot_data,
                                read_only_heap_created);
      ro_heap->InitializeIsolateRoots(isolate);
    } else {
      // This path should only be taken in mksnapshot, should only be run once
      // before tearing down the Isolate that holds this ReadOnlyArtifacts and
      // is not thread-safe.
      auto* artifacts = group->read_only_artifacts();
      CHECK(!artifacts);
      artifacts = group->InitializeReadOnlyArtifacts();
      ro_heap = CreateInitialHeapForBootstrapping(isolate, artifacts);

      // Ensure the first read-only page ends up first in the cage.
      ro_heap->read_only_space()->EnsurePage();
      artifacts->VerifyChecksum(read_only_snapshot_data, true);
    }
  } else {
    auto* ro_heap = new ReadOnlyHeap(new ReadOnlySpace(isolate->heap()));
    isolate->SetUpFromReadOnlyArtifacts(nullptr, ro_heap);
    if (read_only_snapshot_data != nullptr) {
      ro_heap->DeserializeIntoIsolate(isolate, read_only_snapshot_data,
                                      can_rehash);
    }
  }
}

void ReadOnlyHeap::DeserializeIntoIsolate(Isolate* isolate,
                                          SnapshotData* read_only_snapshot_data,
                                          bool can_rehash) {
  DCHECK_NOT_NULL(read_only_snapshot_data);

  ReadOnlyDeserializer des(isolate, read_only_snapshot_data, can_rehash);
  des.DeserializeIntoIsolate();
  OnCreateRootsComplete(isolate);

#ifdef V8_ENABLE_EXTENSIBLE_RO_SNAPSHOT
  if (isolate->serializer_enabled()) {
    // If this isolate will be serialized, leave RO space unfinalized and
    // allocatable s.t. it can be extended (e.g. by future Context::New calls).
    // We reach this scenario when creating custom snapshots - these initially
    // create the isolate from the default V8 snapshot, create new customized
    // contexts, and finally reserialize.
  } else {
    InitFromIsolate(isolate);
  }
#else
  InitFromIsolate(isolate);
#endif  // V8_ENABLE_EXTENSIBLE_RO_SNAPSHOT
}

void ReadOnlyHeap::OnCreateRootsComplete(Isolate* isolate) {
  DCHECK_NOT_NULL(isolate);
  DCHECK(!roots_init_complete_);
  if (IsReadOnlySpaceShared()) InitializeFromIsolateRoots(isolate);
  roots_init_complete_ = true;
}

void ReadOnlyHeap::OnCreateHeapObjectsComplete(Isolate* isolate) {
  DCHECK_NOT_NULL(isolate);

  // InitFromIsolate mutates MemoryChunk flags which would race with any
  // concurrently-running sweeper tasks. Ensure that sweeping has been
  // completed, i.e. no sweeper tasks are currently running.
  isolate->heap()->EnsureSweepingCompleted(
      Heap::SweepingForcedFinalizationMode::kV8Only);

  InitFromIsolate(isolate);

#ifdef VERIFY_HEAP
  if (v8_flags.verify_heap) {
    HeapVerifier::VerifyReadOnlyHeap(isolate->heap());
    HeapVerifier::VerifyHeap(isolate->heap());
  }
#endif
}

// static
ReadOnlyHeap* ReadOnlyHeap::CreateInitialHeapForBootstrapping(
    Isolate* isolate, ReadOnlyArtifacts* artifacts) {
  DCHECK(IsReadOnlySpaceShared());

  std::unique_ptr<SharedReadOnlyHeap> ro_heap;
  auto* ro_space = new ReadOnlySpace(isolate->heap());
  std::unique_ptr<SharedReadOnlyHeap> shared_ro_heap(
      new SharedReadOnlyHeap(ro_space));
#ifdef V8_COMPRESS_POINTERS_IN_MULTIPLE_CAGES
  isolate->isolate_group()->set_shared_ro_heap(shared_ro_heap.get());
#else
  SharedReadOnlyHeap::shared_ro_heap_ = shared_ro_heap.get();
#endif  // V8_COMPRESS_POINTERS_IN_MULTIPLE_CAGES
  ro_heap = std::move(shared_ro_heap);
  artifacts->set_read_only_heap(std::move(ro_heap));
  isolate->SetUpFromReadOnlyArtifacts(artifacts, artifacts->read_only_heap());
  return artifacts->read_only_heap();
}

void SharedReadOnlyHeap::InitializeIsolateRoots(Isolate* isolate) {
  void* const isolate_ro_roots =
      isolate->roots_table().read_only_roots_begin().location();
  std::memcpy(isolate_ro_roots, read_only_roots_,
              kEntriesCount * sizeof(Address));
}

void SharedReadOnlyHeap::InitializeFromIsolateRoots(Isolate* isolate) {
  void* const isolate_ro_roots =
      isolate->roots_table().read_only_roots_begin().location();
  std::memcpy(read_only_roots_, isolate_ro_roots,
              kEntriesCount * sizeof(Address));
}

void ReadOnlyHeap::InitFromIsolate(Isolate* isolate) {
  DCHECK(roots_init_complete_);
  read_only_space_->ShrinkPages();
  if (IsReadOnlySpaceShared()) {
    auto* artifacts = isolate->isolate_group()->read_only_artifacts();
    read_only_space()->DetachPagesAndAddToArtifacts(artifacts);
    artifacts->ReinstallReadOnlySpace(isolate);

    read_only_space_ = artifacts->shared_read_only_space();

#ifdef DEBUG
    artifacts->VerifyHeapAndSpaceRelationships(isolate);
#endif
  } else {
    read_only_space_->Seal(ReadOnlySpace::SealMode::kDoNotDetachFromHeap);
  }
}

ReadOnlyHeap::ReadOnlyHeap(ReadOnlySpace* ro_space)
    : read_only_space_(ro_space) {
#ifdef V8_ENABLE_SANDBOX
  GetProcessWideCodePointerTable()->InitializeSpace(&code_pointer_space_);
#endif
}

void ReadOnlyHeap::OnHeapTearDown(Heap* heap) {
  read_only_space_->TearDown(heap->memory_allocator());
  delete read_only_space_;
}

void SharedReadOnlyHeap::OnHeapTearDown(Heap* heap) {
  // Do nothing as ReadOnlyHeap is shared between all Isolates.
}

// static
void ReadOnlyHeap::PopulateReadOnlySpaceStatistics(
    SharedMemoryStatistics* statistics) {
  statistics->read_only_space_size_ = 0;
  statistics->read_only_space_used_size_ = 0;
  statistics->read_only_space_physical_size_ = 0;
  if (IsReadOnlySpaceShared()) {
#ifndef V8_COMPRESS_POINTERS_IN_MULTIPLE_CAGES
    auto* artifacts = ReadOnlyArtifacts::read_only_artifacts_;
#else
    auto* artifacts =
        Isolate::Current()->isolate_group()->read_only_artifacts();
#endif  // !V8_COMPRESS_POINTERS_IN_MULTIPLE_CAGES
    if (artifacts) {
      auto* ro_space = artifacts->shared_read_only_space();
      statistics->read_only_space_size_ = ro_space->CommittedMemory();
      statistics->read_only_space_used_size_ = ro_space->Size();
      statistics->read_only_space_physical_size_ =
          ro_space->CommittedPhysicalMemory();
    }
  }
}

// static
bool ReadOnlyHeap::Contains(Address address) {
  if (V8_ENABLE_THIRD_PARTY_HEAP_BOOL) {
    return third_party_heap::Heap::InReadOnlySpace(address);
  } else {
    return MemoryChunk::FromAddress(address)->InReadOnlySpace();
  }
}

// static
bool ReadOnlyHeap::Contains(Tagged<HeapObject> object) {
  if (V8_ENABLE_THIRD_PARTY_HEAP_BOOL) {
    return third_party_heap::Heap::InReadOnlySpace(object.address());
  } else {
    return MemoryChunk::FromHeapObject(object)->InReadOnlySpace();
  }
}

ReadOnlyHeapObjectIterator::ReadOnlyHeapObjectIterator(
    const ReadOnlyHeap* ro_heap)
    : ReadOnlyHeapObjectIterator(ro_heap->read_only_space()) {}

ReadOnlyHeapObjectIterator::ReadOnlyHeapObjectIterator(
    const ReadOnlySpace* ro_space)
    : ro_space_(ro_space),
      current_page_(ro_space->pages().begin()),
      page_iterator_(
          current_page_ == ro_space->pages().end() ? nullptr : *current_page_) {
  DCHECK(!V8_ENABLE_THIRD_PARTY_HEAP_BOOL);
}

Tagged<HeapObject> ReadOnlyHeapObjectIterator::Next() {
  while (current_page_ != ro_space_->pages().end()) {
    Tagged<HeapObject> obj = page_iterator_.Next();
    if (!obj.is_null()) return obj;

    ++current_page_;
    if (current_page_ == ro_space_->pages().end()) return Tagged<HeapObject>();
    page_iterator_.Reset(*current_page_);
  }

  DCHECK_EQ(current_page_, ro_space_->pages().end());
  return Tagged<HeapObject>();
}

ReadOnlyPageObjectIterator::ReadOnlyPageObjectIterator(
    const ReadOnlyPageMetadata* page,
    SkipFreeSpaceOrFiller skip_free_space_or_filler)
    : ReadOnlyPageObjectIterator(
          page, page == nullptr ? kNullAddress : page->GetAreaStart(),
          skip_free_space_or_filler) {}

ReadOnlyPageObjectIterator::ReadOnlyPageObjectIterator(
    const ReadOnlyPageMetadata* page, Address current_addr,
    SkipFreeSpaceOrFiller skip_free_space_or_filler)
    : page_(page),
      current_addr_(current_addr),
      skip_free_space_or_filler_(skip_free_space_or_filler) {
  DCHECK(!V8_ENABLE_THIRD_PARTY_HEAP_BOOL);
  DCHECK_GE(current_addr, page->GetAreaStart());
  DCHECK_LT(current_addr, page->GetAreaStart() + page->area_size());
}

Tagged<HeapObject> ReadOnlyPageObjectIterator::Next() {
  if (page_ == nullptr) return HeapObject();

  Address end = page_->GetAreaStart() + page_->area_size();
  for (;;) {
    DCHECK_LE(current_addr_, end);
    if (current_addr_ == end) return HeapObject();

    Tagged<HeapObject> object = HeapObject::FromAddress(current_addr_);
    const int object_size = object->Size();
    current_addr_ += ALIGN_TO_ALLOCATION_ALIGNMENT(object_size);

    if (skip_free_space_or_filler_ == SkipFreeSpaceOrFiller::kYes &&
        IsFreeSpaceOrFiller(object)) {
      continue;
    }

    DCHECK_OBJECT_SIZE(object_size);
    return object;
  }
}

void ReadOnlyPageObjectIterator::Reset(const ReadOnlyPageMetadata* page) {
  page_ = page;
  current_addr_ = page->GetAreaStart();
}

}  // namespace internal
}  // namespace v8
