// Copyright 2024 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_HEAP_MEMORY_CHUNK_INL_H_
#define V8_HEAP_MEMORY_CHUNK_INL_H_

#include "src/common/ptr-compr-inl.h"
#include "src/heap/memory-chunk-metadata.h"
#include "src/heap/memory-chunk.h"

namespace v8 {
namespace internal {

MemoryChunkMetadata* MemoryChunk::Metadata() {
  // If this changes, we also need to update
  // CodeStubAssembler::PageMetadataFromMemoryChunk
#ifdef V8_ENABLE_SANDBOX
  DCHECK_LT(metadata_index_, kMetadataPointerTableSizeMask);

#ifdef V8_COMPRESS_POINTERS_IN_MULTIPLE_CAGES
  MemoryChunkMetadata** metadata_pointer_table = reinterpret_cast<MemoryChunkMetadata**>(MemoryChunkMetadataAddrAccess::metadata_pointer_table());
  MemoryChunkMetadata* metadata =
      metadata_pointer_table[metadata_index_ & kMetadataPointerTableSizeMask];
#else
  MemoryChunkMetadata* metadata =
      metadata_pointer_table_[metadata_index_ & kMetadataPointerTableSizeMask];
#endif  // V8_COMPRESS_POINTERS_IN_MULTIPLE_CAGES

  // Check that the Metadata belongs to this Chunk, since an attacker with write
  // inside the sandbox could've swapped the index.
  SBXCHECK_EQ(metadata->Chunk(), this);
  return metadata;
#else
  return metadata_;
#endif
}

const MemoryChunkMetadata* MemoryChunk::Metadata() const {
  return const_cast<MemoryChunk*>(this)->Metadata();
}

Heap* MemoryChunk::GetHeap() { return Metadata()->heap(); }

}  // namespace internal
}  // namespace v8

#endif  // V8_HEAP_MEMORY_CHUNK_INL_H_
