#include "antb1/exec/aggregate_state.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>

#include <arrow/api.h>
#include <arrow/compute/api_aggregate.h>
#include <arrow/compute/api_scalar.h>
#include <arrow/compute/api_vector.h>
#include <arrow/compute/exec.h>
#include <arrow/util/decimal.h>

#include "antb1/common/int128.h"
#include "antb1/plan/literal.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/types.h"

#include "row_mask.h"

namespace antb1::exec {
namespace {

arrow::Status CheckInput(const arrow::Array& values, const arrow::BooleanArray* selection,
                         const arrow::DataType& expected) {
  if (!values.type()->Equals(expected)) {
    return arrow::Status::Invalid("aggregate input is ", values.type()->ToString(), ", expected ",
                                  expected.ToString());
  }
  if (selection != nullptr && selection->length() != values.length()) {
    return arrow::Status::Invalid("selection of ", selection->length(), " rows for a batch of ",
                                  values.length());
  }
  return arrow::Status::OK();
}

template <class State>
arrow::Result<const State*> SameKind(const AggregateState& other) {
  const auto* same = dynamic_cast<const State*>(&other);
  if (same == nullptr) {
    return arrow::Status::Invalid("cannot merge states of different aggregates");
  }
  return same;
}

arrow::Result<std::shared_ptr<arrow::Array>> OneInt64(int64_t value, arrow::MemoryPool* pool) {
  arrow::Int64Builder builder(pool);
  ARROW_RETURN_NOT_OK(builder.Append(value));
  return builder.Finish();
}

arrow::Result<std::shared_ptr<arrow::Array>> OneDouble(std::optional<double> value,
                                                       arrow::MemoryPool* pool) {
  arrow::DoubleBuilder builder(pool);
  ARROW_RETURN_NOT_OK(value.has_value() ? builder.Append(*value) : builder.AppendNull());
  return builder.Finish();
}

// HUGEINT is decimal128(38, 0): +-(10^38 - 1).
arrow::Result<std::shared_ptr<arrow::Array>> OneHugeInt(std::optional<Int128> value,
                                                        arrow::MemoryPool* pool) {
  arrow::Decimal128Builder builder(plan::ToArrow(plan::LogicalType::kHugeInt), pool);
  if (!value.has_value()) {
    ARROW_RETURN_NOT_OK(builder.AppendNull());
    return builder.Finish();
  }
  const plan::IntegerRange range = plan::RangeOf(plan::LogicalType::kHugeInt);
  if (*value < range.min || *value > range.max) {
    return arrow::Status::ExecutionError(
        "SUM overflow: the result is outside the range of HUGEINT (38 decimal digits)");
  }
  const auto bits = static_cast<UInt128>(*value);
  ARROW_RETURN_NOT_OK(builder.Append(
      arrow::Decimal128(static_cast<int64_t>(bits >> 64U), static_cast<uint64_t>(bits))));
  return builder.Finish();
}

// ---- COUNT(*) and COUNT(c) ----

class CountStarState final : public AggregateState {
 public:
  arrow::Status Consume(const arrow::Array& values, const arrow::BooleanArray* selection) override {
    return ConsumeRows(values.length(), selection);
  }
  arrow::Status ConsumeRows(int64_t num_rows, const arrow::BooleanArray* selection) override {
    if (selection != nullptr && selection->length() != num_rows) {
      return arrow::Status::Invalid("selection of ", selection->length(), " rows for a batch of ",
                                    num_rows);
    }
    count_ += selection == nullptr ? num_rows : selection->true_count();
    return arrow::Status::OK();
  }
  arrow::Status Merge(const AggregateState& other) override {
    ARROW_ASSIGN_OR_RAISE(const auto* same, SameKind<CountStarState>(other));
    count_ += same->count_;
    return arrow::Status::OK();
  }
  [[nodiscard]] arrow::Result<std::shared_ptr<arrow::Array>> Finalize(
      arrow::MemoryPool* pool) const override {
    return OneInt64(count_, pool);
  }

 private:
  int64_t count_ = 0;
};

class CountState final : public AggregateState {
 public:
  CountState(std::shared_ptr<arrow::DataType> type, arrow::MemoryPool* pool)
      : type_(std::move(type)), pool_(pool) {}

  arrow::Status Consume(const arrow::Array& values, const arrow::BooleanArray* selection) override {
    ARROW_RETURN_NOT_OK(CheckInput(values, selection, *type_));
    ARROW_ASSIGN_OR_RAISE(const RowMask mask,
                          RowMask::Make(&values, selection, values.length(), pool_));
    count_ += mask.CountRows();
    return arrow::Status::OK();
  }
  arrow::Status Merge(const AggregateState& other) override {
    ARROW_ASSIGN_OR_RAISE(const auto* same, SameKind<CountState>(other));
    count_ += same->count_;
    return arrow::Status::OK();
  }
  [[nodiscard]] arrow::Result<std::shared_ptr<arrow::Array>> Finalize(
      arrow::MemoryPool* pool) const override {
    return OneInt64(count_, pool);
  }

 private:
  std::shared_ptr<arrow::DataType> type_;
  arrow::MemoryPool* pool_;
  int64_t count_ = 0;
};

// ---- SUM and AVG ----

// The exact sum of n values, in the narrowest accumulator that cannot overflow (which lets the
// compiler vectorize the inner loop): 16-bit values in int32 chunks of 2^15 (|sum| <= 2^31 - 2^15),
// 32-bit values in int64 chunks of 2^30 (|sum| <= 2^62), each chunk then added in 128 bits;
// 64-bit values go straight into 128 bits.
template <class CType>
Int128 SumRun(const CType* values, int64_t n) {
  Int128 total = 0;
  if constexpr (sizeof(CType) <= sizeof(int32_t)) {
    using Partial = std::conditional_t<sizeof(CType) <= sizeof(int16_t), int32_t, int64_t>;
    constexpr int64_t kChunk = sizeof(CType) <= sizeof(int16_t) ? 32'768 : 1'073'741'824;
    for (int64_t start = 0; start < n; start += kChunk) {
      const int64_t end = std::min(n, start + kChunk);
      Partial partial = 0;
      for (int64_t i = start; i < end; ++i) {
        partial += values[i];
      }
      total += partial;
    }
  } else {
    for (int64_t i = 0; i < n; ++i) {
      total += values[i];
    }
  }
  return total;
}

// SUM or AVG of SMALLINT, INTEGER, BIGINT or USMALLINT. The sum of n < 2^63 values of at most 64
// bits stays below 2^126, so the 128-bit accumulator never overflows.
template <class ArrowType>
class IntegerSumState final : public AggregateState {
 public:
  using CType = ArrowType::c_type;

  IntegerSumState(bool average, arrow::MemoryPool* pool) : average_(average), pool_(pool) {}

  arrow::Status Consume(const arrow::Array& values, const arrow::BooleanArray* selection) override {
    ARROW_RETURN_NOT_OK(
        CheckInput(values, selection, *arrow::TypeTraits<ArrowType>::type_singleton()));
    ARROW_ASSIGN_OR_RAISE(const RowMask mask,
                          RowMask::Make(&values, selection, values.length(), pool_));
    const auto* data = values.data()->GetValues<CType>(1);
    Int128 sum = 0;
    int64_t count = 0;
    mask.ForEachRun([&](int64_t position, int64_t length) {
      sum += SumRun(data + position, length);
      count += length;
    });
    sum_ += sum;
    count_ += count;
    return arrow::Status::OK();
  }
  arrow::Status Merge(const AggregateState& other) override {
    ARROW_ASSIGN_OR_RAISE(const auto* same, SameKind<IntegerSumState>(other));
    sum_ += same->sum_;
    count_ += same->count_;
    return arrow::Status::OK();
  }
  [[nodiscard]] arrow::Result<std::shared_ptr<arrow::Array>> Finalize(
      arrow::MemoryPool* pool) const override {
    if (average_) {
      return OneDouble(
          count_ == 0 ? std::nullopt : std::optional(ExactDivideToDouble(sum_, count_)), pool);
    }
    return OneHugeInt(count_ == 0 ? std::nullopt : std::optional(sum_), pool);
  }

 private:
  bool average_;
  arrow::MemoryPool* pool_;
  Int128 sum_ = 0;
  int64_t count_ = 0;
};

Int128 HugeIntAt(const arrow::Decimal128Array& values, int64_t row) {
  const arrow::Decimal128 value(values.GetValue(row));
  const UInt128 bits = (static_cast<UInt128>(static_cast<uint64_t>(value.high_bits())) << 64U) |
                       static_cast<UInt128>(value.low_bits());
  return static_cast<Int128>(bits);
}

// SUM or AVG of a HUGEINT column: every addition is checked.
class HugeIntSumState final : public AggregateState {
 public:
  HugeIntSumState(bool average, arrow::MemoryPool* pool) : average_(average), pool_(pool) {}

  arrow::Status Consume(const arrow::Array& values, const arrow::BooleanArray* selection) override {
    ARROW_RETURN_NOT_OK(CheckInput(values, selection, *plan::ToArrow(plan::LogicalType::kHugeInt)));
    ARROW_ASSIGN_OR_RAISE(const RowMask mask,
                          RowMask::Make(&values, selection, values.length(), pool_));
    const auto& decimals = static_cast<const arrow::Decimal128Array&>(values);
    bool overflow = false;
    mask.ForEachRun([&](int64_t position, int64_t length) {
      for (int64_t row = position; row < position + length && !overflow; ++row) {
        const auto next = CheckedAdd(sum_, HugeIntAt(decimals, row));
        overflow = !next.has_value();
        sum_ = next.value_or(sum_);
      }
      count_ += length;
    });
    if (overflow) {
      return Overflow();
    }
    return arrow::Status::OK();
  }
  arrow::Status Merge(const AggregateState& other) override {
    ARROW_ASSIGN_OR_RAISE(const auto* same, SameKind<HugeIntSumState>(other));
    const auto next = CheckedAdd(sum_, same->sum_);
    if (!next.has_value()) {
      return Overflow();
    }
    sum_ = *next;
    count_ += same->count_;
    return arrow::Status::OK();
  }
  [[nodiscard]] arrow::Result<std::shared_ptr<arrow::Array>> Finalize(
      arrow::MemoryPool* pool) const override {
    if (average_) {
      return OneDouble(
          count_ == 0 ? std::nullopt : std::optional(ExactDivideToDouble(sum_, count_)), pool);
    }
    return OneHugeInt(count_ == 0 ? std::nullopt : std::optional(sum_), pool);
  }

 private:
  [[nodiscard]] arrow::Status Overflow() const {
    return arrow::Status::ExecutionError(average_ ? "AVG" : "SUM",
                                         " overflow: the sum of a HUGEINT column exceeds 128 bits");
  }

  bool average_;
  arrow::MemoryPool* pool_;
  Int128 sum_ = 0;
  int64_t count_ = 0;
};

// SUM or AVG of a DOUBLE column: values are added in row order (as DuckDB does single-threaded).
class DoubleSumState final : public AggregateState {
 public:
  DoubleSumState(bool average, arrow::MemoryPool* pool) : average_(average), pool_(pool) {}

  arrow::Status Consume(const arrow::Array& values, const arrow::BooleanArray* selection) override {
    ARROW_RETURN_NOT_OK(CheckInput(values, selection, *arrow::float64()));
    ARROW_ASSIGN_OR_RAISE(const RowMask mask,
                          RowMask::Make(&values, selection, values.length(), pool_));
    const auto* data = values.data()->GetValues<double>(1);
    mask.ForEachRun([&](int64_t position, int64_t length) {
      for (int64_t i = position; i < position + length; ++i) {
        sum_ += data[i];
      }
      count_ += length;
    });
    return arrow::Status::OK();
  }
  arrow::Status Merge(const AggregateState& other) override {
    ARROW_ASSIGN_OR_RAISE(const auto* same, SameKind<DoubleSumState>(other));
    sum_ += same->sum_;
    count_ += same->count_;
    return arrow::Status::OK();
  }
  [[nodiscard]] arrow::Result<std::shared_ptr<arrow::Array>> Finalize(
      arrow::MemoryPool* pool) const override {
    if (count_ == 0) {
      return OneDouble(std::nullopt, pool);
    }
    return OneDouble(average_ ? sum_ / static_cast<double>(count_) : sum_, pool);
  }

 private:
  bool average_;
  arrow::MemoryPool* pool_;
  double sum_ = 0;
  int64_t count_ = 0;
};

// ---- MIN and MAX ----

// Whether `scalar` is a DOUBLE NaN (DOUBLE is the engine's only floating-point type).
bool IsNaN(const arrow::Scalar& scalar) {
  return scalar.type->id() == arrow::Type::DOUBLE &&
         std::isnan(static_cast<const arrow::DoubleScalar&>(scalar).value);
}

// Arrow's min_max over the selected values of each batch (filtered first under WHERE), merged
// with Arrow's comparison kernels: byte-wise for VARCHAR, chronological for DATE. NaN is ignored
// across batches as min_max ignores it within one: NaN only when every value is NaN (D10).
class MinMaxState final : public AggregateState {
 public:
  MinMaxState(bool min, std::shared_ptr<arrow::DataType> type, arrow::MemoryPool* pool)
      : min_(min), type_(std::move(type)), pool_(pool) {}

  arrow::Status Consume(const arrow::Array& values, const arrow::BooleanArray* selection) override {
    ARROW_RETURN_NOT_OK(CheckInput(values, selection, *type_));
    arrow::compute::ExecContext kernels(pool_);
    arrow::Datum input(values.data());
    if (selection != nullptr) {
      ARROW_ASSIGN_OR_RAISE(
          input, arrow::compute::Filter(input, arrow::Datum(selection->data()),
                                        arrow::compute::FilterOptions::Defaults(), &kernels));
    }
    ARROW_ASSIGN_OR_RAISE(
        const arrow::Datum extremes,
        arrow::compute::MinMax(input,
                               arrow::compute::ScalarAggregateOptions(/*skip_nulls=*/true,
                                                                      /*min_count=*/1),
                               &kernels));
    const auto& pair = extremes.scalar_as<arrow::StructScalar>();
    return Offer(pair.value.at(min_ ? 0 : 1));
  }
  arrow::Status Merge(const AggregateState& other) override {
    ARROW_ASSIGN_OR_RAISE(const auto* same, SameKind<MinMaxState>(other));
    if (same->min_ != min_ || !same->type_->Equals(*type_)) {
      return arrow::Status::Invalid("cannot merge states of different aggregates");
    }
    return same->best_ == nullptr ? arrow::Status::OK() : Offer(same->best_);
  }
  [[nodiscard]] arrow::Result<std::shared_ptr<arrow::Array>> Finalize(
      arrow::MemoryPool* pool) const override {
    if (best_ == nullptr) {
      return arrow::MakeArrayOfNull(type_, 1, pool);
    }
    return arrow::MakeArrayFromScalar(*best_, 1, pool);
  }

 private:
  // Keeps `candidate` if it is valid and beats the current best. min_max yields NaN for a batch
  // only when all its selected values are NaN, and every comparison with NaN is false, so NaN is
  // decided first: any value replaces a NaN best and NaN never replaces another value. The result
  // then does not depend on how the rows split into batches, files or partial states.
  arrow::Status Offer(const std::shared_ptr<arrow::Scalar>& candidate) {
    if (candidate == nullptr || !candidate->is_valid) {
      return arrow::Status::OK();
    }
    if (best_ != nullptr && !IsNaN(*best_)) {
      if (IsNaN(*candidate)) {
        return arrow::Status::OK();
      }
      arrow::compute::ExecContext kernels(pool_);
      ARROW_ASSIGN_OR_RAISE(
          const arrow::Datum better,
          arrow::compute::CallFunction(min_ ? "less" : "greater", {candidate, best_}, &kernels));
      if (!better.scalar_as<arrow::BooleanScalar>().value) {
        return arrow::Status::OK();
      }
    }
    best_ = candidate;
    return arrow::Status::OK();
  }

  bool min_;
  std::shared_ptr<arrow::DataType> type_;
  arrow::MemoryPool* pool_;
  std::shared_ptr<arrow::Scalar> best_;
};

arrow::Result<std::unique_ptr<AggregateState>> SumState(bool average, plan::LogicalType input,
                                                        arrow::MemoryPool* pool) {
  switch (input) {
    case plan::LogicalType::kSmallInt:
      return std::make_unique<IntegerSumState<arrow::Int16Type>>(average, pool);
    case plan::LogicalType::kInteger:
      return std::make_unique<IntegerSumState<arrow::Int32Type>>(average, pool);
    case plan::LogicalType::kBigInt:
      return std::make_unique<IntegerSumState<arrow::Int64Type>>(average, pool);
    case plan::LogicalType::kUSmallInt:
      return std::make_unique<IntegerSumState<arrow::UInt16Type>>(average, pool);
    case plan::LogicalType::kHugeInt:
      return std::make_unique<HugeIntSumState>(average, pool);
    case plan::LogicalType::kDouble:
      return std::make_unique<DoubleSumState>(average, pool);
    case plan::LogicalType::kVarchar:
    case plan::LogicalType::kDate:
      break;
  }
  return arrow::Status::Invalid(average ? "AVG" : "SUM", " of ", plan::ToString(input));
}

}  // namespace

arrow::Status AggregateState::ConsumeRows(int64_t /*num_rows*/,
                                          const arrow::BooleanArray* /*selection*/) {
  return arrow::Status::Invalid("this aggregate needs an argument column");
}

arrow::Result<std::unique_ptr<AggregateState>> MakeAggregateState(
    plan::AggKind kind, std::optional<plan::LogicalType> input, plan::LogicalType result,
    arrow::MemoryPool* pool) {
  const auto invalid = [&] {
    return arrow::Status::Invalid(
        "no aggregate ", plan::ToString(kind), "(",
        input.has_value() ? plan::ToString(*input) : std::string_view("*"), ") -> ",
        plan::ToString(result));
  };
  if (kind == plan::AggKind::kCountStar) {
    if (input.has_value() || result != plan::LogicalType::kBigInt) {
      return invalid();
    }
    return std::make_unique<CountStarState>();
  }
  if (!input.has_value()) {
    return invalid();
  }
  switch (kind) {
    case plan::AggKind::kCountStar:
    case plan::AggKind::kCount:
      if (result != plan::LogicalType::kBigInt) {
        return invalid();
      }
      return std::make_unique<CountState>(plan::ToArrow(*input), pool);
    case plan::AggKind::kSum:
    case plan::AggKind::kAvg: {
      const bool average = kind == plan::AggKind::kAvg;
      const plan::LogicalType expected = !average && plan::IsInteger(*input)
                                             ? plan::LogicalType::kHugeInt
                                             : plan::LogicalType::kDouble;
      if (!plan::IsNumeric(*input) || result != expected) {
        return invalid();
      }
      return SumState(average, *input, pool);
    }
    case plan::AggKind::kMin:
    case plan::AggKind::kMax:
      if (result != *input) {
        return invalid();
      }
      return std::make_unique<MinMaxState>(kind == plan::AggKind::kMin, plan::ToArrow(*input),
                                           pool);
  }
  return invalid();
}

}  // namespace antb1::exec
