#include "gather.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>
#include <vector>

#include <arrow/api.h>
#include <arrow/array/data.h>

#include "antb1/exec/join_table.h"
#include "antb1/exec/sort.h"

namespace antb1::exec {
namespace {

// Whether `ref` names no row (only a gather with NULLs has such refs).
template <bool kWithNulls, class Ref>
bool NoRow(const Ref& ref) {
  if constexpr (kWithNulls) {
    return ref.chunk == kNoChunk;
  } else {
    return false;
  }
}

// The rows `refs` of `arrays` (one array per chunk), appended to a builder of their type.
template <class ArrayType, class BuilderType, bool kWithNulls, class Ref>
arrow::Status GatherTyped(const std::vector<const arrow::Array*>& arrays, std::span<const Ref> refs,
                          arrow::ArrayBuilder& out) {
  auto& builder = static_cast<BuilderType&>(out);
  ARROW_RETURN_NOT_OK(builder.Reserve(static_cast<int64_t>(refs.size())));
  if constexpr (std::is_same_v<ArrayType, arrow::BinaryArray>) {
    int64_t bytes = 0;
    for (const Ref& ref : refs) {
      if (!NoRow<kWithNulls>(ref)) {
        bytes += static_cast<const ArrayType&>(*arrays[ref.chunk]).value_length(ref.row);
      }
    }
    ARROW_RETURN_NOT_OK(builder.ReserveData(bytes));
  }
  for (const Ref& ref : refs) {
    if (NoRow<kWithNulls>(ref)) {
      builder.UnsafeAppendNull();
      continue;
    }
    const auto& array = static_cast<const ArrayType&>(*arrays[ref.chunk]);
    if (array.IsNull(ref.row)) {
      builder.UnsafeAppendNull();
    } else if constexpr (std::is_same_v<ArrayType, arrow::BinaryArray>) {
      builder.UnsafeAppend(array.GetView(ref.row));
    } else if constexpr (std::is_same_v<ArrayType, arrow::Decimal128Array>) {
      builder.UnsafeAppend(arrow::Decimal128(array.GetValue(ref.row)));
    } else {
      builder.UnsafeAppend(array.Value(ref.row));
    }
  }
  return arrow::Status::OK();
}

// Any other type: one slice per run of consecutive rows of a chunk (and one run of NULLs per run
// of refs without a row).
template <bool kWithNulls, class Ref>
arrow::Status GatherSlices(const std::vector<std::shared_ptr<arrow::RecordBatch>>& chunks,
                           int column, std::span<const Ref> refs, arrow::ArrayBuilder& builder) {
  ARROW_RETURN_NOT_OK(builder.Reserve(static_cast<int64_t>(refs.size())));
  std::vector<std::optional<arrow::ArraySpan>> spans(chunks.size());
  for (std::size_t i = 0; i < refs.size();) {
    const Ref first = refs[i];
    std::size_t length = 1;
    if (NoRow<kWithNulls>(first)) {
      while (i + length < refs.size() && NoRow<kWithNulls>(refs[i + length])) {
        ++length;
      }
      ARROW_RETURN_NOT_OK(builder.AppendNulls(static_cast<int64_t>(length)));
      i += length;
      continue;
    }
    while (i + length < refs.size() && refs[i + length].chunk == first.chunk &&
           refs[i + length].row == first.row + length) {
      ++length;
    }
    auto& span = spans[first.chunk];
    if (!span.has_value()) {
      span.emplace(*chunks[first.chunk]->column_data(column));
    }
    ARROW_RETURN_NOT_OK(builder.AppendArraySlice(*span, first.row, static_cast<int64_t>(length)));
    i += length;
  }
  return arrow::Status::OK();
}

}  // namespace

template <bool kWithNulls, class Ref>
arrow::Status GatherRows(const std::vector<std::shared_ptr<arrow::RecordBatch>>& chunks, int column,
                         std::span<const Ref> refs, arrow::ArrayBuilder& builder) {
  std::vector<const arrow::Array*> arrays;
  arrays.reserve(chunks.size());
  for (const auto& chunk : chunks) {
    arrays.push_back(chunk->column(column).get());
  }
  switch (builder.type()->id()) {
    case arrow::Type::INT16:
      return GatherTyped<arrow::Int16Array, arrow::Int16Builder, kWithNulls>(arrays, refs, builder);
    case arrow::Type::INT32:
      return GatherTyped<arrow::Int32Array, arrow::Int32Builder, kWithNulls>(arrays, refs, builder);
    case arrow::Type::INT64:
      return GatherTyped<arrow::Int64Array, arrow::Int64Builder, kWithNulls>(arrays, refs, builder);
    case arrow::Type::UINT16:
      return GatherTyped<arrow::UInt16Array, arrow::UInt16Builder, kWithNulls>(arrays, refs,
                                                                               builder);
    case arrow::Type::DATE32:
      return GatherTyped<arrow::Date32Array, arrow::Date32Builder, kWithNulls>(arrays, refs,
                                                                               builder);
    case arrow::Type::TIMESTAMP:
      return GatherTyped<arrow::TimestampArray, arrow::TimestampBuilder, kWithNulls>(arrays, refs,
                                                                                     builder);
    case arrow::Type::DOUBLE:
      return GatherTyped<arrow::DoubleArray, arrow::DoubleBuilder, kWithNulls>(arrays, refs,
                                                                               builder);
    case arrow::Type::DECIMAL128:
      return GatherTyped<arrow::Decimal128Array, arrow::Decimal128Builder, kWithNulls>(arrays, refs,
                                                                                       builder);
    case arrow::Type::BINARY:
      return GatherTyped<arrow::BinaryArray, arrow::BinaryBuilder, kWithNulls>(arrays, refs,
                                                                               builder);
    default:
      return GatherSlices<kWithNulls>(chunks, column, refs, builder);
  }
}

template arrow::Status GatherRows<false, SortBuffer::RowRef>(
    const std::vector<std::shared_ptr<arrow::RecordBatch>>& chunks, int column,
    std::span<const SortBuffer::RowRef> refs, arrow::ArrayBuilder& builder);
template arrow::Status GatherRows<false, JoinRowRef>(
    const std::vector<std::shared_ptr<arrow::RecordBatch>>& chunks, int column,
    std::span<const JoinRowRef> refs, arrow::ArrayBuilder& builder);
template arrow::Status GatherRows<true, JoinRowRef>(
    const std::vector<std::shared_ptr<arrow::RecordBatch>>& chunks, int column,
    std::span<const JoinRowRef> refs, arrow::ArrayBuilder& builder);

}  // namespace antb1::exec
