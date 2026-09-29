#include "antb1/exec/sort.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <arrow/array/data.h>
#include <arrow/compute/api_vector.h>
#include <arrow/compute/exec.h>

#include "antb1/common/narrow.h"
#include "antb1/exec/operator.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/types.h"

namespace antb1::exec {
namespace {

template <class T>
int Order(const T& x, const T& y) {
  return static_cast<int>(y < x) - static_cast<int>(x < y);
}

template <class ArrayType>
int ComparePrimitive(const arrow::Array& a, int64_t i, const arrow::Array& b, int64_t j) {
  return Order(static_cast<const ArrayType&>(a).Value(i),
               static_cast<const ArrayType&>(b).Value(j));
}

// NaN above every number and equal to NaN; -0.0 == 0.0.
int CompareDouble(const arrow::Array& a, int64_t i, const arrow::Array& b, int64_t j) {
  const double x = static_cast<const arrow::DoubleArray&>(a).Value(i);
  const double y = static_cast<const arrow::DoubleArray&>(b).Value(j);
  const bool x_nan = std::isnan(x);
  const bool y_nan = std::isnan(y);
  if (x_nan || y_nan) {
    return static_cast<int>(x_nan) - static_cast<int>(y_nan);
  }
  return Order(x, y);
}

int CompareDecimal(const arrow::Array& a, int64_t i, const arrow::Array& b, int64_t j) {
  const arrow::Decimal128 x(static_cast<const arrow::Decimal128Array&>(a).GetValue(i));
  const arrow::Decimal128 y(static_cast<const arrow::Decimal128Array&>(b).GetValue(j));
  return Order(x, y);
}

// By bytes: std::char_traits<char> compares characters as unsigned char.
int CompareBinary(const arrow::Array& a, int64_t i, const arrow::Array& b, int64_t j) {
  const std::string_view x = static_cast<const arrow::BinaryArray&>(a).GetView(i);
  const std::string_view y = static_cast<const arrow::BinaryArray&>(b).GetView(j);
  const int c = x.compare(y);
  return static_cast<int>(c > 0) - static_cast<int>(c < 0);
}

// ---- order-preserving 64-bit prefixes (ascending) ----

template <class ArrayType>
std::uint64_t SignedPrefix(const arrow::Array& a, int64_t i) {
  const auto v = static_cast<int64_t>(static_cast<const ArrayType&>(a).Value(i));
  return static_cast<std::uint64_t>(v) ^ (std::uint64_t{1} << 63U);
}

std::uint64_t UInt16Prefix(const arrow::Array& a, int64_t i) {
  return static_cast<const arrow::UInt16Array&>(a).Value(i);
}

// NaN above every number (and every NaN equal), -0.0 equal to 0.0.
std::uint64_t DoublePrefix(const arrow::Array& a, int64_t i) {
  double v = static_cast<const arrow::DoubleArray&>(a).Value(i);
  if (std::isnan(v)) {
    return std::numeric_limits<std::uint64_t>::max();
  }
  if (v == 0.0) {
    v = 0.0;
  }
  const auto bits = std::bit_cast<std::uint64_t>(v);
  constexpr std::uint64_t kSign = std::uint64_t{1} << 63U;
  return (bits & kSign) != 0 ? ~bits : bits | kSign;
}

// The high 64 bits, as a signed number: not exact.
std::uint64_t DecimalPrefix(const arrow::Array& a, int64_t i) {
  const arrow::Decimal128 v(static_cast<const arrow::Decimal128Array&>(a).GetValue(i));
  return static_cast<std::uint64_t>(v.high_bits()) ^ (std::uint64_t{1} << 63U);
}

// The first 8 bytes, big-endian, zero-padded: never larger for a smaller string. Not exact.
std::uint64_t BinaryPrefix(const arrow::Array& a, int64_t i) {
  const std::string_view v = static_cast<const arrow::BinaryArray&>(a).GetView(i);
  std::uint64_t bits = 0;
  for (std::size_t k = 0; k < 8; ++k) {
    bits = (bits << 8U) | (k < v.size() ? static_cast<unsigned char>(v[k]) : 0U);
  }
  return bits;
}

using CompareFn = int (*)(const arrow::Array&, int64_t, const arrow::Array&, int64_t);
using PrefixFn = std::uint64_t (*)(const arrow::Array&, int64_t);

struct TypeOrder {
  CompareFn compare = nullptr;
  PrefixFn prefix = nullptr;
  bool exact = false;  // the prefix is the whole value
};

std::optional<TypeOrder> OrderFor(plan::LogicalType type) {
  switch (type) {
    case plan::LogicalType::kSmallInt:
      return TypeOrder{.compare = &ComparePrimitive<arrow::Int16Array>,
                       .prefix = &SignedPrefix<arrow::Int16Array>,
                       .exact = true};
    case plan::LogicalType::kInteger:
      return TypeOrder{.compare = &ComparePrimitive<arrow::Int32Array>,
                       .prefix = &SignedPrefix<arrow::Int32Array>,
                       .exact = true};
    case plan::LogicalType::kBigInt:
      return TypeOrder{.compare = &ComparePrimitive<arrow::Int64Array>,
                       .prefix = &SignedPrefix<arrow::Int64Array>,
                       .exact = true};
    case plan::LogicalType::kUSmallInt:
      return TypeOrder{
          .compare = &ComparePrimitive<arrow::UInt16Array>, .prefix = &UInt16Prefix, .exact = true};
    case plan::LogicalType::kDate:
      return TypeOrder{.compare = &ComparePrimitive<arrow::Date32Array>,
                       .prefix = &SignedPrefix<arrow::Date32Array>,
                       .exact = true};
    case plan::LogicalType::kHugeInt:
      return TypeOrder{.compare = &CompareDecimal, .prefix = &DecimalPrefix, .exact = false};
    case plan::LogicalType::kDouble:
      return TypeOrder{.compare = &CompareDouble, .prefix = &DoublePrefix, .exact = true};
    case plan::LogicalType::kVarchar:
      return TypeOrder{.compare = &CompareBinary, .prefix = &BinaryPrefix, .exact = false};
    case plan::LogicalType::kTimestamp:
      return TypeOrder{.compare = &ComparePrimitive<arrow::TimestampArray>,
                       .prefix = &SignedPrefix<arrow::TimestampArray>,
                       .exact = true};
    case plan::LogicalType::kBoolean:  // never a sort key
      break;
  }
  return std::nullopt;
}

// ---- gathering the rows of several chunks into one array ----

// The rows `refs` of `arrays` (one array per chunk), appended to a builder of their type.
template <class ArrayType, class BuilderType>
arrow::Status GatherTyped(const std::vector<const arrow::Array*>& arrays,
                          std::span<const SortBuffer::RowRef> refs, arrow::ArrayBuilder& out) {
  auto& builder = static_cast<BuilderType&>(out);
  ARROW_RETURN_NOT_OK(builder.Reserve(static_cast<int64_t>(refs.size())));
  if constexpr (std::is_same_v<ArrayType, arrow::BinaryArray>) {
    int64_t bytes = 0;
    for (const auto& ref : refs) {
      bytes += static_cast<const ArrayType&>(*arrays[ref.chunk]).value_length(ref.row);
    }
    ARROW_RETURN_NOT_OK(builder.ReserveData(bytes));
  }
  for (const auto& ref : refs) {
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

// Any other type: one slice per run of consecutive rows of a chunk.
arrow::Status GatherSlices(const std::vector<std::shared_ptr<arrow::RecordBatch>>& chunks,
                           int column, std::span<const SortBuffer::RowRef> refs,
                           arrow::ArrayBuilder& builder) {
  ARROW_RETURN_NOT_OK(builder.Reserve(static_cast<int64_t>(refs.size())));
  std::vector<std::optional<arrow::ArraySpan>> spans(chunks.size());
  for (std::size_t i = 0; i < refs.size();) {
    const auto first = refs[i];
    std::size_t length = 1;
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

arrow::Status Gather(const std::vector<std::shared_ptr<arrow::RecordBatch>>& chunks, int column,
                     std::span<const SortBuffer::RowRef> refs, arrow::ArrayBuilder& builder) {
  std::vector<const arrow::Array*> arrays;
  arrays.reserve(chunks.size());
  for (const auto& chunk : chunks) {
    arrays.push_back(chunk->column(column).get());
  }
  switch (builder.type()->id()) {
    case arrow::Type::INT16:
      return GatherTyped<arrow::Int16Array, arrow::Int16Builder>(arrays, refs, builder);
    case arrow::Type::INT32:
      return GatherTyped<arrow::Int32Array, arrow::Int32Builder>(arrays, refs, builder);
    case arrow::Type::INT64:
      return GatherTyped<arrow::Int64Array, arrow::Int64Builder>(arrays, refs, builder);
    case arrow::Type::UINT16:
      return GatherTyped<arrow::UInt16Array, arrow::UInt16Builder>(arrays, refs, builder);
    case arrow::Type::DATE32:
      return GatherTyped<arrow::Date32Array, arrow::Date32Builder>(arrays, refs, builder);
    case arrow::Type::TIMESTAMP:
      return GatherTyped<arrow::TimestampArray, arrow::TimestampBuilder>(arrays, refs, builder);
    case arrow::Type::DOUBLE:
      return GatherTyped<arrow::DoubleArray, arrow::DoubleBuilder>(arrays, refs, builder);
    case arrow::Type::DECIMAL128:
      return GatherTyped<arrow::Decimal128Array, arrow::Decimal128Builder>(arrays, refs, builder);
    case arrow::Type::BINARY:
      return GatherTyped<arrow::BinaryArray, arrow::BinaryBuilder>(arrays, refs, builder);
    default:
      return GatherSlices(chunks, column, refs, builder);
  }
}

// a + b, saturated at INT64_MAX (both non-negative).
int64_t SaturatingAdd(int64_t a, int64_t b) {
  int64_t sum = 0;
  return __builtin_add_overflow(a, b, &sum) ? std::numeric_limits<int64_t>::max() : sum;
}

}  // namespace

// ---- RowComparator ----

arrow::Result<RowComparator> RowComparator::Make(std::shared_ptr<arrow::Schema> schema,
                                                 const std::vector<plan::SortKey>& keys) {
  if (schema == nullptr) {
    return arrow::Status::Invalid("sort without a schema");
  }
  std::vector<Key> bound;
  bound.reserve(keys.size());
  for (const plan::SortKey& key : keys) {
    const int column = key.column.index;
    if (column < 0 || column >= schema->num_fields()) {
      return arrow::Status::Invalid("sort key outside its input");
    }
    const auto order = OrderFor(key.column.type);
    if (!order.has_value() ||
        !schema->field(column)->type()->Equals(plan::ToArrow(key.column.type))) {
      return arrow::Status::Invalid("sort key of type ", schema->field(column)->type()->ToString(),
                                    " declared as ", plan::ToString(key.column.type));
    }
    bound.push_back(Key{.column = column,
                        .compare = order->compare,
                        .prefix = order->prefix,
                        .exact = order->exact,
                        .descending = key.descending,
                        .nulls_first = key.nulls_first});
  }
  return RowComparator(std::move(schema), std::move(bound));
}

RowComparator::KeyArrays RowComparator::KeysOf(const arrow::RecordBatch& batch) const {
  KeyArrays arrays;
  arrays.reserve(keys_.size());
  for (const Key& key : keys_) {
    arrays.push_back(batch.column(key.column).get());
  }
  return arrays;
}

int RowComparator::Compare(const KeyArrays& a, int64_t i, const KeyArrays& b, int64_t j,
                           std::size_t first_key) const {
  for (std::size_t k = first_key; k < keys_.size(); ++k) {
    const Key& key = keys_[k];
    const bool a_null = a[k]->IsNull(i);
    const bool b_null = b[k]->IsNull(j);
    if (a_null || b_null) {
      if (a_null && b_null) {
        continue;
      }
      return a_null == key.nulls_first ? -1 : 1;
    }
    const int c = key.compare(*a[k], i, *b[k], j);
    if (c != 0) {
      return key.descending ? -c : c;
    }
  }
  return 0;
}

RowComparator::Prefix RowComparator::PrefixOf(const KeyArrays& keys, int64_t row) const {
  const Key& key = keys_.front();
  if (keys.front()->IsNull(row)) {
    return Prefix{.bits = 0, .group = static_cast<std::uint8_t>(key.nulls_first ? 0 : 2)};
  }
  const std::uint64_t bits = key.prefix(*keys.front(), row);
  return Prefix{.bits = key.descending ? ~bits : bits, .group = 1};
}

// ---- SortBuffer ----

SortBuffer::SortBuffer(RowComparator comparator, std::optional<int64_t> keep)
    : comparator_(std::move(comparator)), keep_(keep) {}

arrow::Status SortBuffer::Add(std::shared_ptr<arrow::RecordBatch> rows, arrow::MemoryPool* pool) {
  if (rows == nullptr || rows->num_rows() == 0) {
    return arrow::Status::OK();
  }
  if (!rows->schema()->Equals(*comparator_.schema(), /*check_metadata=*/false)) {
    return arrow::Status::Invalid("sort input with another schema");
  }
  if (!TryNarrow<std::uint32_t>(rows->num_rows()).has_value() ||
      !TryNarrow<std::uint32_t>(chunks_.size() + 1).has_value()) {
    return arrow::Status::CapacityError("too many rows to sort in memory");
  }
  sorted_ = false;
  RowComparator::KeyArrays keys = comparator_.KeysOf(*rows);
  if (threshold_.has_value()) {
    // Only rows before the last kept one can still be among the first `keep` rows; a row that ties
    // with it comes later in the input, so it loses the tie.
    const RowComparator::KeyArrays& last = chunk_keys_[threshold_->chunk];
    std::vector<int64_t> better;
    for (int64_t i = 0; i < rows->num_rows(); ++i) {
      if (comparator_.Compare(keys, i, last, threshold_->row) < 0) {
        better.push_back(i);
      }
    }
    if (better.empty()) {
      return arrow::Status::OK();
    }
    if (std::cmp_less(better.size(), rows->num_rows())) {
      arrow::Int64Builder indices(pool);
      ARROW_RETURN_NOT_OK(indices.AppendValues(better));
      ARROW_ASSIGN_OR_RAISE(const auto index_array, indices.Finish());
      arrow::compute::ExecContext kernels(pool);
      ARROW_ASSIGN_OR_RAISE(
          const arrow::Datum taken,
          arrow::compute::Take(rows, index_array, arrow::compute::TakeOptions::NoBoundsCheck(),
                               &kernels));
      rows = taken.record_batch();
      keys = comparator_.KeysOf(*rows);
    }
  }
  rows_ += rows->num_rows();
  chunks_.push_back(std::move(rows));
  chunk_keys_.push_back(std::move(keys));
  // Compact once the rows beyond `keep` outgrow `keep` (and a minimum, so that a small `keep` does
  // not sort every batch).
  constexpr int64_t kMinSlack = 4096;
  if (keep_.has_value() && rows_ > SaturatingAdd(*keep_, std::max(*keep_, kMinSlack))) {
    return Compact(pool);
  }
  return arrow::Status::OK();
}

arrow::Status SortBuffer::Merge(SortBuffer& other, arrow::MemoryPool* pool) {
  if (!other.comparator_.schema()->Equals(*comparator_.schema(), /*check_metadata=*/false) ||
      other.keep_ != keep_) {
    return arrow::Status::Invalid("merge of sort buffers of different shapes");
  }
  // Unsorted chunks are in the other buffer's input order; a compacted chunk holds its first rows
  // in sorted order, which is also a valid input order for them (ties keep their order).
  for (auto& chunk : other.chunks_) {
    ARROW_RETURN_NOT_OK(Add(std::move(chunk), pool));
  }
  other.chunks_.clear();
  other.chunk_keys_.clear();
  other.order_.clear();
  other.threshold_.reset();
  other.rows_ = 0;
  return arrow::Status::OK();
}

int64_t SortBuffer::memory_usage() const {
  std::size_t bytes = (chunks_.capacity() * sizeof(std::shared_ptr<arrow::RecordBatch>)) +
                      (chunk_keys_.capacity() * sizeof(RowComparator::KeyArrays)) +
                      (order_.capacity() * sizeof(RowRef));
  for (const RowComparator::KeyArrays& keys : chunk_keys_) {
    bytes += keys.capacity() * sizeof(const arrow::Array*);
  }
  return Narrow<int64_t>(bytes);
}

int64_t SortBuffer::sort_memory() const {
  // Sort's entries (a 64-bit prefix, a row reference and a group, padded) and the new order.
  constexpr auto kPerRow = static_cast<int64_t>((3 * sizeof(std::uint64_t)) + sizeof(RowRef));
  return rows_ * kPerRow;
}

arrow::Status SortBuffer::Sort(arrow::MemoryPool* pool) {
  if (keep_.has_value()) {
    return Compact(pool);
  }
  // Rows with the prefix of their first key: most comparisons are two integer compares, and the
  // full comparison runs only for equal prefixes.
  struct Entry {
    std::uint64_t bits;
    RowRef ref;
    std::uint8_t group;
  };
  std::vector<Entry> entries;
  entries.reserve(Narrow<std::size_t>(rows_));
  for (std::size_t c = 0; c < chunks_.size(); ++c) {
    for (int64_t r = 0; r < chunks_[c]->num_rows(); ++r) {
      const RowComparator::Prefix prefix = comparator_.PrefixOf(chunk_keys_[c], r);
      entries.push_back(
          Entry{.bits = prefix.bits,
                .ref = RowRef{.chunk = Narrow<std::uint32_t>(c), .row = Narrow<std::uint32_t>(r)},
                .group = prefix.group});
    }
  }
  const std::size_t tie_key = comparator_.prefix_is_exact() ? 1 : 0;
  std::ranges::stable_sort(entries, [this, tie_key](const Entry& a, const Entry& b) {
    if (a.group != b.group) {
      return a.group < b.group;
    }
    if (a.group != 1) {  // both NULL: the first key ties
      return comparator_.Compare(chunk_keys_[a.ref.chunk], a.ref.row, chunk_keys_[b.ref.chunk],
                                 b.ref.row, 1) < 0;
    }
    if (a.bits != b.bits) {
      return a.bits < b.bits;
    }
    return comparator_.Compare(chunk_keys_[a.ref.chunk], a.ref.row, chunk_keys_[b.ref.chunk],
                               b.ref.row, tie_key) < 0;
  });
  order_.clear();
  order_.reserve(entries.size());
  for (const Entry& entry : entries) {
    order_.push_back(entry.ref);
  }
  sorted_ = true;
  return arrow::Status::OK();
}

arrow::Status SortBuffer::Compact(arrow::MemoryPool* pool) {
  const std::optional<int64_t> keep = keep_;
  keep_.reset();
  const arrow::Status sorted = Sort(pool);
  keep_ = keep;
  ARROW_RETURN_NOT_OK(sorted);
  const int64_t limit = keep.value_or(rows_);
  const int64_t kept = std::min(rows_, limit);
  ARROW_ASSIGN_OR_RAISE(auto first, Slice(0, kept, pool));
  chunks_.clear();
  chunk_keys_.clear();
  order_.clear();
  threshold_.reset();
  rows_ = kept;
  if (kept > 0) {
    chunk_keys_.push_back(comparator_.KeysOf(*first));
    chunks_.push_back(std::move(first));
    for (int64_t r = 0; r < kept; ++r) {
      order_.push_back(RowRef{.chunk = 0, .row = Narrow<std::uint32_t>(r)});
    }
    if (kept == limit) {
      threshold_ = order_.back();
    }
  }
  sorted_ = true;
  return arrow::Status::OK();
}

arrow::Result<std::shared_ptr<arrow::RecordBatch>> SortBuffer::Slice(
    int64_t begin, int64_t end, arrow::MemoryPool* pool) const {
  if (!sorted_) {
    return arrow::Status::Invalid("sort buffer read before Sort()");
  }
  begin = std::clamp<int64_t>(begin, 0, rows_);
  end = std::clamp<int64_t>(end, begin, rows_);
  const arrow::Schema& schema = *comparator_.schema();
  arrow::ArrayVector columns;
  columns.reserve(Narrow<std::size_t>(schema.num_fields()));
  for (int c = 0; c < schema.num_fields(); ++c) {
    ARROW_ASSIGN_OR_RAISE(auto builder, arrow::MakeBuilder(schema.field(c)->type(), pool));
    ARROW_RETURN_NOT_OK(Gather(chunks_, c,
                               std::span<const RowRef>(order_).subspan(
                                   Narrow<std::size_t>(begin), Narrow<std::size_t>(end - begin)),
                               *builder));
    ARROW_ASSIGN_OR_RAISE(auto column, builder->Finish());
    columns.push_back(std::move(column));
  }
  return arrow::RecordBatch::Make(comparator_.schema(), end - begin, std::move(columns));
}

// ---- SortOperator ----

SortOperator::SortOperator(std::unique_ptr<Operator> input, std::vector<plan::SortKey> keys,
                           std::optional<int64_t> limit, int64_t offset)
    : input_(std::move(input)), keys_(std::move(keys)), limit_(limit), offset_(offset) {}

arrow::Status SortOperator::Open(ExecContext& ctx) {
  if (keys_.empty()) {
    return arrow::Status::Invalid("sort without keys");
  }
  if ((limit_.has_value() && *limit_ < 0) || offset_ < 0) {
    return arrow::Status::Invalid("negative LIMIT or OFFSET");
  }
  pool_ = ctx.pool;
  memory_.Reset(ctx.budget);
  batch_size_ = std::max<int64_t>(ctx.batch_size, 1);
  ARROW_ASSIGN_OR_RAISE(RowComparator comparator,
                        RowComparator::Make(input_->output_schema(), keys_));
  const std::optional<int64_t> keep =
      limit_.has_value() ? std::optional(SaturatingAdd(*limit_, offset_)) : std::nullopt;
  buffer_ = std::make_unique<SortBuffer>(std::move(comparator), keep);
  next_ = 0;
  end_ = 0;
  sorted_ = false;
  return input_->Open(ctx);
}

arrow::Result<Batch> SortOperator::Next() {
  if (buffer_ == nullptr) {
    return arrow::Status::Invalid("sort: Next() before Open()");
  }
  if (!sorted_) {
    while (true) {
      ARROW_ASSIGN_OR_RAISE(const Batch in, input_->Next());
      if (in.end()) {
        break;
      }
      ARROW_ASSIGN_OR_RAISE(auto rows, Materialize(in, pool_));
      ARROW_RETURN_NOT_OK(buffer_->Add(std::move(rows), pool_));
      ARROW_RETURN_NOT_OK(memory_.Resize(buffer_->memory_usage()));
    }
    ARROW_RETURN_NOT_OK(memory_.Resize(buffer_->memory_usage() + buffer_->sort_memory()));
    ARROW_RETURN_NOT_OK(buffer_->Sort(pool_));
    ARROW_RETURN_NOT_OK(memory_.Resize(buffer_->memory_usage()));
    sorted_ = true;
    next_ = std::min(offset_, buffer_->num_rows());
    end_ = limit_.has_value() ? std::min(buffer_->num_rows(), SaturatingAdd(next_, *limit_))
                              : buffer_->num_rows();
  }
  if (next_ >= end_) {
    return Batch{};
  }
  const int64_t stop = std::min(end_, SaturatingAdd(next_, batch_size_));
  ARROW_ASSIGN_OR_RAISE(auto rows, buffer_->Slice(next_, stop, pool_));
  next_ = stop;
  return Batch{.data = std::move(rows), .selection = {}};
}

arrow::Status SortOperator::Close() {
  buffer_.reset();
  memory_.Release();
  return input_->Close();
}

}  // namespace antb1::exec
