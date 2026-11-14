#pragma once

#include <memory>
#include <span>

#include <arrow/result.h>
#include <arrow/type_fwd.h>

namespace antb1::exec {

enum class BinaryViewRetention {
  kBounded,  // exact distinct-live-byte retention bound
  kShare,    // cheap transport; a retaining consumer decides when to copy
  kCopy,     // fresh compact payload blocks, including repeated output values
  kHybrid,   // share a backing allocation when selected copy volume covers its capacity
};

// The pilot's owned payload helpers (ADR 0027). Sources must be valid, immutable Arrow arrays;
// their allocating pools must outlive all results. Gather checks row references and owns every
// referenced buffer. kBounded compacts when distinct backing capacity exceeds the larger of
// 64 KiB and four times distinct live out-of-line payload; duplicate references do not inflate
// that allowance. kShare avoids range accounting; kCopy copies each selected long value into
// fresh owned blocks. kHybrid compares each distinct backing allocation's full capacity with
// selected long-value bytes, including duplicates, and shares on equality or greater copy volume.
// Inline values retain no source payload. Every result has BINARY_VIEW type.
// Ref is JoinRowRef or SortBuffer::RowRef. A ref with chunk == kNoChunk produces a NULL.
template <class Ref>
arrow::Result<std::shared_ptr<arrow::Array>> GatherBinaryViews(
    std::span<const arrow::Array* const> arrays, std::span<const Ref> refs, arrow::MemoryPool* pool,
    BinaryViewRetention retention = BinaryViewRetention::kBounded);

bool HasBinaryViews(const arrow::Schema& schema);

// Checked Take for INT64 or UINT32 indices; NULL indices produce NULL rows. Non-view columns use
// Arrow's kernel. Filter drops false/NULL mask entries and requires the batch's length.
arrow::Result<std::shared_ptr<arrow::RecordBatch>> TakeViewBatch(
    const std::shared_ptr<arrow::RecordBatch>& batch, const std::shared_ptr<arrow::Array>& indices,
    arrow::MemoryPool* pool, BinaryViewRetention retention = BinaryViewRetention::kBounded);
arrow::Result<std::shared_ptr<arrow::RecordBatch>> FilterViewBatch(
    const std::shared_ptr<arrow::RecordBatch>& batch,
    const std::shared_ptr<arrow::BooleanArray>& selection, arrow::MemoryPool* pool,
    BinaryViewRetention retention = BinaryViewRetention::kBounded);

// Reapply the gather retention bound to view columns without changing row order or physical
// schema. Ordinary columns are shared unchanged.
arrow::Result<std::shared_ptr<arrow::RecordBatch>> CompactBinaryViews(
    const std::shared_ptr<arrow::RecordBatch>& batch, arrow::MemoryPool* pool);

// Copy all selected view payload to fresh owned blocks, preserving the physical schema. Unlike
// CompactBinaryViews this also copies an already compact input, allowing a retaining consumer to
// replace old blocks after dropping rows. Ordinary columns are shared unchanged.
arrow::Result<std::shared_ptr<arrow::RecordBatch>> CopyBinaryViewPayload(
    const std::shared_ptr<arrow::RecordBatch>& batch, arrow::MemoryPool* pool);

// Apply the hybrid policy independently to each view column's selected payload. Source aliases
// with the same backing allocation share one decision. Ordinary columns are shared unchanged;
// the result does not certify the exact distinct-live-byte bound of CompactBinaryViews.
arrow::Result<std::shared_ptr<arrow::RecordBatch>> RetainBinaryViewPayload(
    const std::shared_ptr<arrow::RecordBatch>& batch, arrow::MemoryPool* pool);

// The explicit final boundary: restore BINARY fields and copy selected view payload once.
// Ordinary columns are shared unchanged. Fails if one output Binary array exceeds its offset
// range; it never changes SQL's VARCHAR representation or byte semantics.
arrow::Result<std::shared_ptr<arrow::RecordBatch>> MaterializeBinaryViews(
    const std::shared_ptr<arrow::RecordBatch>& batch, arrow::MemoryPool* pool);

}  // namespace antb1::exec
