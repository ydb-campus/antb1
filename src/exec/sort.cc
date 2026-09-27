#include "antb1/exec/sort.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string_view>
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

using CompareFn = int (*)(const arrow::Array&, int64_t, const arrow::Array&, int64_t);

std::optional<CompareFn> CompareFor(plan::LogicalType type) {
  switch (type) {
    case plan::LogicalType::kSmallInt:
      return &ComparePrimitive<arrow::Int16Array>;
    case plan::LogicalType::kInteger:
      return &ComparePrimitive<arrow::Int32Array>;
    case plan::LogicalType::kBigInt:
      return &ComparePrimitive<arrow::Int64Array>;
    case plan::LogicalType::kUSmallInt:
      return &ComparePrimitive<arrow::UInt16Array>;
    case plan::LogicalType::kDate:
      return &ComparePrimitive<arrow::Date32Array>;
    case plan::LogicalType::kHugeInt:
      return &CompareDecimal;
    case plan::LogicalType::kDouble:
      return &CompareDouble;
    case plan::LogicalType::kVarchar:
      return &CompareBinary;
  }
  return std::nullopt;
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
    const auto compare = CompareFor(key.column.type);
    if (!compare.has_value() ||
        !schema->field(column)->type()->Equals(plan::ToArrow(key.column.type))) {
      return arrow::Status::Invalid("sort key of type ", schema->field(column)->type()->ToString(),
                                    " declared as ", plan::ToString(key.column.type));
    }
    bound.push_back(Key{.column = column,
                        .compare = *compare,
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

int RowComparator::Compare(const KeyArrays& a, int64_t i, const KeyArrays& b, int64_t j) const {
  for (std::size_t k = 0; k < keys_.size(); ++k) {
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

arrow::Status SortBuffer::Sort(arrow::MemoryPool* pool) {
  if (keep_.has_value()) {
    return Compact(pool);
  }
  order_.clear();
  order_.reserve(Narrow<std::size_t>(rows_));
  for (std::size_t c = 0; c < chunks_.size(); ++c) {
    for (int64_t r = 0; r < chunks_[c]->num_rows(); ++r) {
      order_.push_back(RowRef{.chunk = Narrow<std::uint32_t>(c), .row = Narrow<std::uint32_t>(r)});
    }
  }
  std::ranges::stable_sort(order_, [this](const RowRef& a, const RowRef& b) {
    return comparator_.Compare(chunk_keys_[a.chunk], a.row, chunk_keys_[b.chunk], b.row) < 0;
  });
  sorted_ = true;
  return arrow::Status::OK();
}

arrow::Status SortBuffer::Compact(arrow::MemoryPool* pool) {
  const std::optional<int64_t> keep = keep_;
  keep_.reset();
  const arrow::Status sorted = Sort(pool);
  keep_ = keep;
  ARROW_RETURN_NOT_OK(sorted);
  const int64_t kept = std::min(rows_, *keep_);
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
    if (kept == *keep_) {
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
    ARROW_RETURN_NOT_OK(builder->Reserve(end - begin));
    std::vector<std::optional<arrow::ArraySpan>> spans(chunks_.size());
    // Runs of consecutive rows of one chunk are appended as one slice.
    for (int64_t i = begin; i < end;) {
      const RowRef first = order_[Narrow<std::size_t>(i)];
      int64_t length = 1;
      while (i + length < end) {
        const RowRef next = order_[Narrow<std::size_t>(i + length)];
        if (next.chunk != first.chunk || next.row != first.row + length) {
          break;
        }
        ++length;
      }
      auto& span = spans[first.chunk];
      if (!span.has_value()) {
        span.emplace(*chunks_[first.chunk]->column_data(c));
      }
      ARROW_RETURN_NOT_OK(builder->AppendArraySlice(*span, first.row, length));
      i += length;
    }
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
    }
    ARROW_RETURN_NOT_OK(buffer_->Sort(pool_));
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
  return input_->Close();
}

}  // namespace antb1::exec
