// Copyright 2023 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_SANDBOX_CODE_POINTER_TABLE_INL_H_
#define V8_SANDBOX_CODE_POINTER_TABLE_INL_H_

#include "src/sandbox/code-pointer-table.h"
// Include the non-inl header before the rest of the headers.

#include "src/common/code-memory-access-inl.h"
#include "src/sandbox/external-entity-table-inl.h"

#ifdef V8_COMPRESS_POINTERS

namespace v8 {
namespace internal {

void CodePointerTableEntry::MakeCodePointerEntry(Address code,
                                                 Address entrypoint,
                                                 CodeEntrypointTag tag,
                                                 bool mark_as_alive) {
  DCHECK_EQ(code & kMarkingBit, 0);
  DCHECK_EQ(entrypoint >> kCodeEntrypointTagShift, 0);
  DCHECK_NE(tag, kFreeCodePointerTableEntryTag);

  if (mark_as_alive) code |= kMarkingBit;
  entrypoint_.store(entrypoint ^ tag, std::memory_order_relaxed);
  code_.store(code, std::memory_order_relaxed);
}

Address CodePointerTableEntry::GetEntrypoint(CodeEntrypointTag tag) const {
  DCHECK(!IsFreelistEntry());
  return entrypoint_.load(std::memory_order_relaxed) ^ tag;
}

void CodePointerTableEntry::SetEntrypoint(Address value,
                                          CodeEntrypointTag tag) {
  DCHECK(!IsFreelistEntry());
  DCHECK_EQ(value >> kCodeEntrypointTagShift, 0);
  DCHECK_NE(tag, kFreeCodePointerTableEntryTag);

  entrypoint_.store(value ^ tag, std::memory_order_relaxed);
}

Address CodePointerTableEntry::GetCodeObject() const {
  DCHECK(!IsFreelistEntry());
  // We reuse the heap object tag bit as marking bit, so we need to explicitly
  // set it here when accessing the pointer.
  return code_.load(std::memory_order_relaxed) | kMarkingBit;
}

void CodePointerTableEntry::SetCodeObject(Address new_value) {
  DCHECK(!IsFreelistEntry());
  // SetContent shouldn't change the marking state of the entry. Currently this
  // is always automatically the case, but if this ever fails, we might need to
  // manually copy the marking bit.
  DCHECK_EQ(code_ & kMarkingBit, new_value & kMarkingBit);
  code_.store(new_value, std::memory_order_relaxed);
}

void CodePointerTableEntry::MakeFreelistEntry(uint32_t next_entry_index) {
  Address value = kFreeEntryTag | next_entry_index;
  entrypoint_.store(value, std::memory_order_relaxed);
  code_.store(kNullAddress, std::memory_order_relaxed);
}

bool CodePointerTableEntry::IsFreelistEntry() const {
  auto entrypoint = entrypoint_.load(std::memory_order_relaxed);
  return (entrypoint & kFreeEntryTag) == kFreeEntryTag;
}

uint32_t CodePointerTableEntry::GetNextFreelistEntryIndex() const {
  return static_cast<uint32_t>(entrypoint_.load(std::memory_order_relaxed));
}

void CodePointerTableEntry::Mark() {
  Address old_value = code_.load(std::memory_order_relaxed);
  Address new_value = old_value | kMarkingBit;

  // We don't need to perform the CAS in a loop since it can only fail if a new
  // value has been written into the entry. This, however, will also have set
  // the marking bit.
  bool success = code_.compare_exchange_strong(old_value, new_value,
                                               std::memory_order_relaxed);
  DCHECK(success || (old_value & kMarkingBit) == kMarkingBit);
  USE(success);
}

void CodePointerTableEntry::Unmark() {
  Address value = code_.load(std::memory_order_relaxed);
  value &= ~kMarkingBit;
  code_.store(value, std::memory_order_relaxed);
}

bool CodePointerTableEntry::IsMarked() const {
  Address value = code_.load(std::memory_order_relaxed);
  return value & kMarkingBit;
}

template <typename EntrypointMappingFunction,
          typename CodeObjectMappingFunction>
void CodePointerTableEntry::Remap(const CodePointerTableEntry& original,
                                  EntrypointMappingFunction entrypoint_mapping,
                                  CodeObjectMappingFunction code_mapping) {
  CFIMetadataWriteScope write_scope("CodePointerTable write");
  const Address original_code_object = original.code_.load();
  const CodeEntrypointTag tag = base::bit_cast<CodeEntrypointTag>(
      original.entrypoint_.load() & kFreeCodePointerTableEntryTag);
  const Address original_entrypoint = original.GetEntrypoint(tag);
  const Address remapped_code_object = code_mapping(original_code_object);
  const Address remapped_entrypoint = entrypoint_mapping(original_entrypoint);
  MakeCodePointerEntry(remapped_code_object, remapped_entrypoint, tag,
                       original.IsMarked());
}

Address CodePointerTable::GetEntrypoint(CodePointerHandle handle,
                                        CodeEntrypointTag tag) const {
  uint32_t index = HandleToIndex(handle);
  return at(index).GetEntrypoint(tag);
}

Address CodePointerTable::GetCodeObject(CodePointerHandle handle) const {
  uint32_t index = HandleToIndex(handle);
  // Due to the fact that we use the heap object tag as marking bit, this table
  // (in contrast to the trusted pointer table) does not return Smi::zero() for
  // the 0th entry. That entry must therefore not be accessed here.
  DCHECK_NE(index, 0);
  return at(index).GetCodeObject();
}

void CodePointerTable::SetEntrypoint(CodePointerHandle handle, Address value,
                                     CodeEntrypointTag tag) {
  DCHECK_NE(kNullCodePointerHandle, handle);
  uint32_t index = HandleToIndex(handle);
  CFIMetadataWriteScope write_scope("CodePointerTable write");
  at(index).SetEntrypoint(value, tag);
}

void CodePointerTable::SetCodeObject(CodePointerHandle handle, Address value) {
  DCHECK_NE(kNullCodePointerHandle, handle);
  uint32_t index = HandleToIndex(handle);
  CFIMetadataWriteScope write_scope("CodePointerTable write");
  at(index).SetCodeObject(value);
}

CodePointerHandle CodePointerTable::AllocateAndInitializeEntry(
    Space* space, Address code, Address entrypoint, CodeEntrypointTag tag) {
  DCHECK(space->BelongsTo(this));
  uint32_t index = AllocateEntry(space);
  CFIMetadataWriteScope write_scope("CodePointerTable write");
  at(index).MakeCodePointerEntry(code, entrypoint, tag,
                                 space->allocate_black());
  return IndexToHandle(index);
}

void CodePointerTable::Mark(Space* space, CodePointerHandle handle) {
  DCHECK(space->BelongsTo(this));
  // The null entry is immortal and immutable, so no need to mark it as alive.
  if (handle == kNullCodePointerHandle) return;

  uint32_t index = HandleToIndex(handle);
  DCHECK(space->Contains(index));

  CFIMetadataWriteScope write_scope("CodePointerTable write");
  at(index).Mark();
}

template <typename Callback>
void CodePointerTable::IterateActiveEntriesIn(Space* space, Callback callback) {
  IterateEntriesIn(space, [&](uint32_t index) {
    if (!at(index).IsFreelistEntry()) {
      callback(IndexToHandle(index), at(index).GetCodeObject());
    }
  });
}

uint32_t CodePointerTable::HandleToIndex(CodePointerHandle handle) const {
  uint32_t index = handle >> kCodePointerHandleShift;
  DCHECK_EQ(handle,
            (index << kCodePointerHandleShift) | kCodePointerHandleMarker);
  return index;
}

CodePointerHandle CodePointerTable::IndexToHandle(uint32_t index) const {
  CodePointerHandle handle = index << kCodePointerHandleShift;
  DCHECK_EQ(index, handle >> kCodePointerHandleShift);
  return handle | kCodePointerHandleMarker;
}

template <typename EntrypointMappingFunction,
          typename CodeObjectMappingFunction>
void CodePointerTable::CloneSpaceFrom(
    CodePointerTable* original, Space* original_space, Space* destination_space,
    EntrypointMappingFunction entrypoint_mapping,
    CodeObjectMappingFunction code_mapping) {
  CloneSegmentsData(original, original_space, destination_space);
  original->IterateEntriesIn(
      original_space,
      [this, original, entrypoint_mapping, code_mapping](uint32_t index) {
        const CodePointerTableEntry& original_entry = original->at(index);
        if (original_entry.IsFreelistEntry()) {
          return;
        }
        at(index).Remap(original_entry, entrypoint_mapping, code_mapping);
        DCHECK_EQ(original_entry.IsMarked(), at(index).IsMarked());
      });
}

}  // namespace internal
}  // namespace v8

#endif  // V8_COMPRESS_POINTERS

#endif  // V8_SANDBOX_CODE_POINTER_TABLE_INL_H_
