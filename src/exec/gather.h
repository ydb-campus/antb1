#pragma once

#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <vector>

#include <arrow/array/builder_base.h>
#include <arrow/record_batch.h>
#include <arrow/status.h>

// Gathering rows of several batches into one array: the sort buffer's output (SortBuffer::Slice)
// and a hash join's build columns (HashJoinOperator).

namespace antb1::exec {

// The chunk of a row reference that names no row: GatherRows<true> appends NULL for it.
inline constexpr std::uint32_t kNoChunk = std::numeric_limits<std::uint32_t>::max();

// The rows `refs` of column `column` (row `row` of chunks[chunk] for a ref {chunk, row}), in order,
// appended to `builder`, a builder of the column's type. Typed loops for SMALLINT, INTEGER, BIGINT,
// USMALLINT, DATE, TIMESTAMP, DOUBLE, DECIMAL and VARCHAR (whose bytes are reserved once); one
// slice per run of consecutive rows of a chunk for any other type (BOOLEAN). With kWithNulls, a
// ref whose chunk is kNoChunk appends NULL. Ref is SortBuffer::RowRef or JoinRowRef (instantiated
// in gather.cc for <false, SortBuffer::RowRef>, <false, JoinRowRef> and <true, JoinRowRef>); the
// template arguments are given explicitly, as a span is not deduced from a vector.
template <bool kWithNulls, class Ref>
arrow::Status GatherRows(const std::vector<std::shared_ptr<arrow::RecordBatch>>& chunks, int column,
                         std::span<const Ref> refs, arrow::ArrayBuilder& builder);

}  // namespace antb1::exec
