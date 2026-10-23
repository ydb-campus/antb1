#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <vector>

#include <arrow/array/array_base.h>
#include <arrow/array/builder_base.h>
#include <arrow/record_batch.h>
#include <arrow/status.h>

// Gathering rows of several batches into one array: the sort buffer's output (SortBuffer::Slice)
// and a hash join's build columns (HashJoinOperator, PartRightJoinOperator).

namespace antb1::exec {

// The chunk of a row reference that names no row: GatherRows<true> appends NULL for it.
inline constexpr std::uint32_t kNoChunk = std::numeric_limits<std::uint32_t>::max();

// The arrays of every column of a set of chunks (batches of one schema), taken once and kept
// column by column, for GatherRows over the same chunks again and again: asking a batch for a
// column (arrow::RecordBatch::column) locks the batch and counts a reference, and a gather over
// chunks asks every chunk once per call, so a caller that gathers from many chunks many times -- a
// hash join's build, gathered for every batch of rows its join emits -- would walk all of them each
// time. The arrays are the ones each chunk keeps (arrow::RecordBatch::columns), so the chunks must
// outlive it.
class ChunkColumns {
 public:
  ChunkColumns() = default;
  // Columns 0 .. num_columns - 1 of every chunk of `chunks`, which have at least that many.
  ChunkColumns(const std::vector<std::shared_ptr<arrow::RecordBatch>>& chunks, int num_columns);

  // The bytes of the array pointers that a ChunkColumns of `num_columns` columns of `num_chunks`
  // chunks holds, one per chunk and column (to reserve before making one).
  [[nodiscard]] static int64_t MemoryUsage(std::size_t num_chunks, int num_columns);

  // Column `column`'s array of every chunk, in chunk order. `column` must be one it took.
  [[nodiscard]] std::span<const arrow::Array* const> column(int column) const;

 private:
  std::size_t chunks_ = 0;
  std::vector<const arrow::Array*> arrays_;  // column c of chunk k at c * chunks_ + k
};

// The rows `refs` of one column, whose array in chunk k is arrays[k] (row `row` of arrays[chunk]
// for a ref {chunk, row}), in order, appended to `builder`, a builder of the column's type. Typed
// loops for SMALLINT, INTEGER, BIGINT, USMALLINT, DATE, TIMESTAMP, DOUBLE, DECIMAL and VARCHAR
// (whose bytes are reserved once); one slice per run of consecutive rows of a chunk for any other
// type (BOOLEAN). With kWithNulls, a ref whose chunk is kNoChunk appends NULL. Ref is
// SortBuffer::RowRef or JoinRowRef (instantiated in gather.cc for <false, SortBuffer::RowRef>,
// <false, JoinRowRef> and <true, JoinRowRef>); the template arguments are given explicitly, as a
// span is not deduced from a vector.
template <bool kWithNulls, class Ref>
arrow::Status GatherRows(std::span<const arrow::Array* const> arrays, std::span<const Ref> refs,
                         arrow::ArrayBuilder& builder);

// The same over column `column` of `chunks`, asking every chunk for it on every call (the sort
// buffer's gather, once per batch it emits). Instantiated for <false, SortBuffer::RowRef> only.
template <bool kWithNulls, class Ref>
arrow::Status GatherRows(const std::vector<std::shared_ptr<arrow::RecordBatch>>& chunks, int column,
                         std::span<const Ref> refs, arrow::ArrayBuilder& builder);

}  // namespace antb1::exec
