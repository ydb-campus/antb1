#include "antb1/exec/grouped_aggregate_state.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <arrow/compute/api_vector.h>
#include <arrow/compute/exec.h>
#include <arrow/compute/row/grouper.h>
#include <arrow/util/decimal.h>

#include "antb1/common/int128.h"
#include "antb1/plan/literal.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/types.h"

#include "double_key.h"
#include "temporal_average.h"

namespace antb1::exec {
namespace {

using GroupIds = std::span<const std::uint32_t>;

arrow::Status CheckRows(const arrow::Array* values, const arrow::DataType* expected, GroupIds ids,
                        std::uint32_t num_groups) {
  if (values != nullptr) {
    if (expected != nullptr && !values->type()->Equals(*expected)) {
      return arrow::Status::Invalid("aggregate input is ", values->type()->ToString(),
                                    ", expected ", expected->ToString());
    }
    if (std::cmp_not_equal(values->length(), ids.size())) {
      return arrow::Status::Invalid(ids.size(), " group ids for ", values->length(), " rows");
    }
  }
  if (!ids.empty() && std::ranges::max(ids) >= num_groups) {
    return arrow::Status::Invalid("group id outside the ", num_groups, " groups of the state");
  }
  return arrow::Status::OK();
}

// The bytes of vectors' storage (capacity), for memory_usage().
template <class... Vectors>
std::int64_t VectorBytes(const Vectors&... vectors) {
  return (... +
          static_cast<std::int64_t>(vectors.capacity() * sizeof(typename Vectors::value_type)));
}

template <class State>
arrow::Result<const State*> SameKind(const GroupedAggregateState& other, GroupIds from, GroupIds to,
                                     std::uint32_t num_groups) {
  const auto* same = dynamic_cast<const State*>(&other);
  if (same == nullptr) {
    return arrow::Status::Invalid("cannot merge states of different aggregates");
  }
  if (from.size() != to.size()) {
    return arrow::Status::Invalid("a merge of ", from.size(), " groups into ", to.size());
  }
  for (std::size_t i = 0; i < from.size(); ++i) {
    if (from[i] >= other.num_groups() || to[i] >= num_groups) {
      return arrow::Status::Invalid("a merge of group ", from[i], " of ", other.num_groups(),
                                    " into group ", to[i], " of ", num_groups);
    }
  }
  return same;
}

// The elements [begin, end) of per-group values, or Invalid for a range outside them.
template <class T>
arrow::Result<std::span<const T>> GroupRange(const std::vector<T>& values, std::uint32_t begin,
                                             std::uint32_t end) {
  if (begin > end || end > values.size()) {
    return arrow::Status::Invalid("groups [", begin, ", ", end, ") of a state with ", values.size(),
                                  " groups");
  }
  return std::span<const T>(values).subspan(begin, end - begin);
}

arrow::Result<std::shared_ptr<arrow::Array>> Int64s(std::span<const std::int64_t> values,
                                                    arrow::MemoryPool* pool) {
  arrow::Int64Builder builder(pool);
  ARROW_RETURN_NOT_OK(
      builder.AppendValues(values.data(), static_cast<std::int64_t>(values.size())));
  return builder.Finish();
}

// Exact sums as their 38-digit type, HUGEINT or DECIMAL(38, s) (decimal128 within +-(10^38 - 1));
// NULL for a group without values.
arrow::Result<std::shared_ptr<arrow::Array>> HugeInts(std::span<const Int128> sums,
                                                      std::span<const std::int64_t> counts,
                                                      plan::LogicalType type,
                                                      arrow::MemoryPool* pool) {
  arrow::Decimal128Builder builder(plan::ToArrow(type), pool);
  ARROW_RETURN_NOT_OK(builder.Reserve(static_cast<std::int64_t>(sums.size())));
  const plan::IntegerRange range = plan::RangeOf(type);
  for (std::size_t g = 0; g < sums.size(); ++g) {
    if (counts[g] == 0) {
      builder.UnsafeAppendNull();
      continue;
    }
    if (sums[g] < range.min || sums[g] > range.max) {
      return arrow::Status::ExecutionError("SUM overflow: the result is outside the range of ",
                                           plan::ToString(type), " (38 decimal digits)");
    }
    const auto bits = static_cast<UInt128>(sums[g]);
    builder.UnsafeAppend(arrow::Decimal128(static_cast<std::int64_t>(bits >> 64U),
                                           static_cast<std::uint64_t>(bits)));
  }
  return builder.Finish();
}

// DuckDB's AVG of a DECIMAL per group (DuckDbDecimalAverage), NULL for a group without values.
arrow::Result<std::shared_ptr<arrow::Array>> DecimalAverages(std::span<const Int128> sums,
                                                             std::span<const std::int64_t> counts,
                                                             plan::LogicalType input,
                                                             arrow::MemoryPool* pool) {
  arrow::DoubleBuilder builder(pool);
  ARROW_RETURN_NOT_OK(builder.Reserve(static_cast<std::int64_t>(sums.size())));
  for (std::size_t g = 0; g < sums.size(); ++g) {
    if (counts[g] == 0) {
      builder.UnsafeAppendNull();
    } else {
      builder.UnsafeAppend(DuckDbDecimalAverage(sums[g], counts[g], input.width(), input.scale()));
    }
  }
  return builder.Finish();
}

// sum / count per group as DOUBLE (exact for integers), NULL for a group without values.
template <class Sum>
arrow::Result<std::shared_ptr<arrow::Array>> Averages(std::span<const Sum> sums,
                                                      std::span<const std::int64_t> counts,
                                                      arrow::MemoryPool* pool) {
  arrow::DoubleBuilder builder(pool);
  ARROW_RETURN_NOT_OK(builder.Reserve(static_cast<std::int64_t>(sums.size())));
  for (std::size_t g = 0; g < sums.size(); ++g) {
    if (counts[g] == 0) {
      builder.UnsafeAppendNull();
    } else if constexpr (std::is_same_v<Sum, double>) {
      builder.UnsafeAppend(sums[g] / static_cast<double>(counts[g]));
    } else {
      builder.UnsafeAppend(ExactDivideToDouble(sums[g], counts[g]));
    }
  }
  return builder.Finish();
}

// ---- COUNT(*) and COUNT(c) ----

class GroupedCount final : public GroupedAggregateState {
 public:
  // type: the argument type, or nullptr for COUNT(*).
  explicit GroupedCount(std::shared_ptr<arrow::DataType> type) : type_(std::move(type)) {}

  [[nodiscard]] std::uint32_t num_groups() const override {
    return static_cast<std::uint32_t>(counts_.size());
  }
  void Resize(std::uint32_t num_groups) override { counts_.resize(num_groups, 0); }
  [[nodiscard]] std::int64_t memory_usage() const override { return VectorBytes(counts_); }
  arrow::Status Consume(const arrow::Array* values, GroupIds ids) override {
    if ((type_ == nullptr) != (values == nullptr)) {
      return arrow::Status::Invalid(type_ == nullptr ? "COUNT(*) takes no argument column"
                                                     : "COUNT needs an argument column");
    }
    ARROW_RETURN_NOT_OK(CheckRows(values, type_.get(), ids, num_groups()));
    if (values == nullptr || values->null_count() == 0) {
      for (const std::uint32_t g : ids) {
        ++counts_[g];
      }
      return arrow::Status::OK();
    }
    for (std::size_t i = 0; i < ids.size(); ++i) {
      counts_[ids[i]] += values->IsValid(static_cast<std::int64_t>(i)) ? 1 : 0;
    }
    return arrow::Status::OK();
  }
  arrow::Status MergeGroups(const GroupedAggregateState& other, GroupIds from,
                            GroupIds to) override {
    ARROW_ASSIGN_OR_RAISE(const auto* same, SameKind<GroupedCount>(other, from, to, num_groups()));
    for (std::size_t i = 0; i < from.size(); ++i) {
      counts_[to[i]] += same->counts_[from[i]];
    }
    return arrow::Status::OK();
  }
  [[nodiscard]] arrow::Result<std::shared_ptr<arrow::Array>> Finalize(
      std::uint32_t begin, std::uint32_t end, arrow::MemoryPool* pool) const override {
    ARROW_ASSIGN_OR_RAISE(const auto counts, GroupRange(counts_, begin, end));
    return Int64s(counts, pool);
  }

 private:
  std::shared_ptr<arrow::DataType> type_;
  std::vector<std::int64_t> counts_;
};

// ---- SUM and AVG ----

// SUM or AVG of SMALLINT, INTEGER, BIGINT or USMALLINT in 128 bits per group; the sum of fewer
// than 2^63 values of at most 64 bits never overflows it.
template <class ArrowType>
class GroupedIntegerSum final : public GroupedAggregateState {
 public:
  using CType = ArrowType::c_type;

  explicit GroupedIntegerSum(bool average) : average_(average) {}

  [[nodiscard]] std::uint32_t num_groups() const override {
    return static_cast<std::uint32_t>(sums_.size());
  }
  void Resize(std::uint32_t num_groups) override {
    sums_.resize(num_groups, 0);
    counts_.resize(num_groups, 0);
  }
  [[nodiscard]] std::int64_t memory_usage() const override { return VectorBytes(sums_, counts_); }
  arrow::Status Consume(const arrow::Array* values, GroupIds ids) override {
    if (values == nullptr) {
      return arrow::Status::Invalid("SUM needs an argument column");
    }
    ARROW_RETURN_NOT_OK(
        CheckRows(values, arrow::TypeTraits<ArrowType>::type_singleton().get(), ids, num_groups()));
    const auto* data = values->data()->GetValues<CType>(1);
    const bool nulls = values->null_count() != 0;
    for (std::size_t i = 0; i < ids.size(); ++i) {
      if (nulls && values->IsNull(static_cast<std::int64_t>(i))) {
        continue;
      }
      sums_[ids[i]] += data[i];
      ++counts_[ids[i]];
    }
    return arrow::Status::OK();
  }
  arrow::Status MergeGroups(const GroupedAggregateState& other, GroupIds from,
                            GroupIds to) override {
    ARROW_ASSIGN_OR_RAISE(const auto* same,
                          SameKind<GroupedIntegerSum>(other, from, to, num_groups()));
    for (std::size_t i = 0; i < from.size(); ++i) {
      sums_[to[i]] += same->sums_[from[i]];
      counts_[to[i]] += same->counts_[from[i]];
    }
    return arrow::Status::OK();
  }
  [[nodiscard]] arrow::Result<std::shared_ptr<arrow::Array>> Finalize(
      std::uint32_t begin, std::uint32_t end, arrow::MemoryPool* pool) const override {
    ARROW_ASSIGN_OR_RAISE(const auto sums, GroupRange(sums_, begin, end));
    ARROW_ASSIGN_OR_RAISE(const auto counts, GroupRange(counts_, begin, end));
    return average_ ? Averages(sums, counts, pool)
                    : HugeInts(sums, counts, plan::LogicalType::kHugeInt, pool);
  }

 private:
  bool average_;
  std::vector<Int128> sums_;
  std::vector<std::int64_t> counts_;
};

// AVG of DATE or TIMESTAMP values per group: a TIMESTAMP, averaged in 128 bits as DuckDB does.
class GroupedTemporalAvg final : public GroupedAggregateState {
 public:
  [[nodiscard]] std::uint32_t num_groups() const override {
    return static_cast<std::uint32_t>(sums_.size());
  }
  void Resize(std::uint32_t num_groups) override {
    sums_.resize(num_groups, 0);
    counts_.resize(num_groups, 0);
  }
  [[nodiscard]] std::int64_t memory_usage() const override { return VectorBytes(sums_, counts_); }
  arrow::Status Consume(const arrow::Array* values, GroupIds ids) override {
    if (values == nullptr) {
      return arrow::Status::Invalid("AVG needs an argument column");
    }
    ARROW_RETURN_NOT_OK(CheckRows(values, values->type().get(), ids, num_groups()));
    const bool nulls = values->null_count() != 0;
    for (std::size_t i = 0; i < ids.size(); ++i) {
      const auto row = static_cast<std::int64_t>(i);
      if (nulls && values->IsNull(row)) {
        continue;
      }
      ARROW_ASSIGN_OR_RAISE(const std::int64_t micros, TemporalMicros(*values, row));
      sums_[ids[i]] += micros;
      ++counts_[ids[i]];
    }
    return arrow::Status::OK();
  }
  arrow::Status MergeGroups(const GroupedAggregateState& other, GroupIds from,
                            GroupIds to) override {
    ARROW_ASSIGN_OR_RAISE(const auto* same,
                          SameKind<GroupedTemporalAvg>(other, from, to, num_groups()));
    for (std::size_t i = 0; i < from.size(); ++i) {
      sums_[to[i]] += same->sums_[from[i]];
      counts_[to[i]] += same->counts_[from[i]];
    }
    return arrow::Status::OK();
  }
  [[nodiscard]] arrow::Result<std::shared_ptr<arrow::Array>> Finalize(
      std::uint32_t begin, std::uint32_t end, arrow::MemoryPool* pool) const override {
    ARROW_ASSIGN_OR_RAISE(const auto sums, GroupRange(sums_, begin, end));
    ARROW_ASSIGN_OR_RAISE(const auto counts, GroupRange(counts_, begin, end));
    arrow::TimestampBuilder builder(plan::ToArrow(plan::LogicalType::kTimestamp), pool);
    ARROW_RETURN_NOT_OK(builder.Reserve(static_cast<std::int64_t>(sums.size())));
    for (std::size_t g = 0; g < sums.size(); ++g) {
      if (counts[g] == 0) {
        builder.UnsafeAppendNull();
      } else {
        builder.UnsafeAppend(AverageMicros(sums[g], counts[g]));
      }
    }
    return builder.Finish();
  }

 private:
  std::vector<Int128> sums_;
  std::vector<std::int64_t> counts_;
};

Int128 HugeIntAt(const arrow::Decimal128Array& values, std::int64_t row) {
  const arrow::Decimal128 value(values.GetValue(row));
  const UInt128 bits =
      (static_cast<UInt128>(static_cast<std::uint64_t>(value.high_bits())) << 64U) |
      static_cast<UInt128>(value.low_bits());
  return static_cast<Int128>(bits);
}

// SUM or AVG of a HUGEINT or DECIMAL argument (decimal128): every addition is checked. A
// DECIMAL(p,s) sums to DECIMAL(38,s) and averages as DuckDB does.
class GroupedHugeIntSum final : public GroupedAggregateState {
 public:
  GroupedHugeIntSum(bool average, plan::LogicalType input) : average_(average), input_(input) {}

  [[nodiscard]] std::uint32_t num_groups() const override {
    return static_cast<std::uint32_t>(sums_.size());
  }
  void Resize(std::uint32_t num_groups) override {
    sums_.resize(num_groups, 0);
    counts_.resize(num_groups, 0);
  }
  [[nodiscard]] std::int64_t memory_usage() const override { return VectorBytes(sums_, counts_); }
  arrow::Status Consume(const arrow::Array* values, GroupIds ids) override {
    if (values == nullptr) {
      return arrow::Status::Invalid("SUM needs an argument column");
    }
    ARROW_RETURN_NOT_OK(CheckRows(values, plan::ToArrow(input_).get(), ids, num_groups()));
    const auto& decimals = static_cast<const arrow::Decimal128Array&>(*values);
    for (std::size_t i = 0; i < ids.size(); ++i) {
      const auto row = static_cast<std::int64_t>(i);
      if (decimals.IsNull(row)) {
        continue;
      }
      ARROW_RETURN_NOT_OK(Add(ids[i], HugeIntAt(decimals, row), 1));
    }
    return arrow::Status::OK();
  }
  arrow::Status MergeGroups(const GroupedAggregateState& other, GroupIds from,
                            GroupIds to) override {
    ARROW_ASSIGN_OR_RAISE(const auto* same,
                          SameKind<GroupedHugeIntSum>(other, from, to, num_groups()));
    for (std::size_t i = 0; i < from.size(); ++i) {
      ARROW_RETURN_NOT_OK(Add(to[i], same->sums_[from[i]], same->counts_[from[i]]));
    }
    return arrow::Status::OK();
  }
  [[nodiscard]] arrow::Result<std::shared_ptr<arrow::Array>> Finalize(
      std::uint32_t begin, std::uint32_t end, arrow::MemoryPool* pool) const override {
    ARROW_ASSIGN_OR_RAISE(const auto sums, GroupRange(sums_, begin, end));
    ARROW_ASSIGN_OR_RAISE(const auto counts, GroupRange(counts_, begin, end));
    if (input_ != plan::LogicalType::kDecimal) {
      return average_ ? Averages(sums, counts, pool)
                      : HugeInts(sums, counts, plan::LogicalType::kHugeInt, pool);
    }
    return average_ ? DecimalAverages(sums, counts, input_, pool)
                    : HugeInts(sums, counts,
                               plan::LogicalType::Decimal(plan::LogicalType::kMaxDecimalWidth,
                                                          input_.scale()),
                               pool);
  }

 private:
  arrow::Status Add(std::uint32_t group, Int128 value, std::int64_t count) {
    const auto next = CheckedAdd(sums_[group], value);
    if (!next.has_value()) {
      return arrow::Status::ExecutionError(average_ ? "AVG" : "SUM", " overflow: the sum of a ",
                                           plan::ToString(input_), " column exceeds 128 bits");
    }
    sums_[group] = *next;
    counts_[group] += count;
    return arrow::Status::OK();
  }

  bool average_;
  plan::LogicalType input_;
  std::vector<Int128> sums_;
  std::vector<std::int64_t> counts_;
};

// SUM or AVG of a DOUBLE column: values are added in row order within each group.
class GroupedDoubleSum final : public GroupedAggregateState {
 public:
  explicit GroupedDoubleSum(bool average) : average_(average) {}

  [[nodiscard]] std::uint32_t num_groups() const override {
    return static_cast<std::uint32_t>(sums_.size());
  }
  void Resize(std::uint32_t num_groups) override {
    sums_.resize(num_groups, 0);
    counts_.resize(num_groups, 0);
  }
  [[nodiscard]] std::int64_t memory_usage() const override { return VectorBytes(sums_, counts_); }
  arrow::Status Consume(const arrow::Array* values, GroupIds ids) override {
    if (values == nullptr) {
      return arrow::Status::Invalid("SUM needs an argument column");
    }
    ARROW_RETURN_NOT_OK(CheckRows(values, arrow::float64().get(), ids, num_groups()));
    const auto* data = values->data()->GetValues<double>(1);
    const bool nulls = values->null_count() != 0;
    for (std::size_t i = 0; i < ids.size(); ++i) {
      if (nulls && values->IsNull(static_cast<std::int64_t>(i))) {
        continue;
      }
      sums_[ids[i]] += data[i];
      ++counts_[ids[i]];
    }
    return arrow::Status::OK();
  }
  arrow::Status MergeGroups(const GroupedAggregateState& other, GroupIds from,
                            GroupIds to) override {
    ARROW_ASSIGN_OR_RAISE(const auto* same,
                          SameKind<GroupedDoubleSum>(other, from, to, num_groups()));
    for (std::size_t i = 0; i < from.size(); ++i) {
      sums_[to[i]] += same->sums_[from[i]];
      counts_[to[i]] += same->counts_[from[i]];
    }
    return arrow::Status::OK();
  }
  [[nodiscard]] arrow::Result<std::shared_ptr<arrow::Array>> Finalize(
      std::uint32_t begin, std::uint32_t end, arrow::MemoryPool* pool) const override {
    ARROW_ASSIGN_OR_RAISE(const auto sums, GroupRange(sums_, begin, end));
    ARROW_ASSIGN_OR_RAISE(const auto counts, GroupRange(counts_, begin, end));
    if (average_) {
      return Averages(sums, counts, pool);
    }
    arrow::DoubleBuilder builder(pool);
    ARROW_RETURN_NOT_OK(builder.Reserve(static_cast<std::int64_t>(sums.size())));
    for (std::size_t g = 0; g < sums.size(); ++g) {
      if (counts[g] == 0) {
        builder.UnsafeAppendNull();
      } else {
        builder.UnsafeAppend(sums[g]);
      }
    }
    return builder.Finish();
  }

 private:
  bool average_;
  std::vector<double> sums_;
  std::vector<std::int64_t> counts_;
};

// ---- MIN and MAX ----

// How MIN and MAX read and store the values of one column type.
template <class ArrowType>
struct MinMaxTraits {
  using ArrayType = arrow::TypeTraits<ArrowType>::ArrayType;
  using BuilderType = arrow::TypeTraits<ArrowType>::BuilderType;
  using Value = ArrowType::c_type;   // a value as read
  using Stored = ArrowType::c_type;  // a group's best value
  static Value Get(const ArrayType& values, std::int64_t row) { return values.Value(row); }
  static Stored Store(Value value) { return value; }
  static Value View(const Stored& value) { return value; }
};

template <>
struct MinMaxTraits<arrow::Decimal128Type> {
  using ArrayType = arrow::Decimal128Array;
  using BuilderType = arrow::Decimal128Builder;
  using Value = arrow::Decimal128;
  using Stored = arrow::Decimal128;
  static Value Get(const ArrayType& values, std::int64_t row) {
    const Value value(values.GetValue(row));
    return value;
  }
  static Stored Store(Value value) { return value; }
  static Value View(const Stored& value) { return value; }
};

template <>
struct MinMaxTraits<arrow::BinaryType> {  // byte-wise, as std::string_view compares
  using ArrayType = arrow::BinaryArray;
  using BuilderType = arrow::BinaryBuilder;
  using Value = std::string_view;
  using Stored = std::string;
  static Value Get(const ArrayType& values, std::int64_t row) { return values.GetView(row); }
  static Stored Store(Value value) { return std::string(value); }
  static Value View(const Stored& value) { return value; }
};

// The best value per group. NaN is decided first, as in the scalar state: any value replaces a
// NaN best and NaN never replaces a value, so a group is NaN only when every value is NaN. Ties
// keep the value seen first.
template <class ArrowType>
class GroupedMinMax final : public GroupedAggregateState {
 public:
  using Traits = MinMaxTraits<ArrowType>;
  using Value = Traits::Value;

  GroupedMinMax(bool min, std::shared_ptr<arrow::DataType> type)
      : min_(min), type_(std::move(type)) {}

  [[nodiscard]] std::uint32_t num_groups() const override {
    return static_cast<std::uint32_t>(seen_.size());
  }
  void Resize(std::uint32_t num_groups) override {
    best_.resize(num_groups);
    seen_.resize(num_groups, Seen::kNothing);
  }
  [[nodiscard]] std::int64_t memory_usage() const override {
    return VectorBytes(best_, seen_) + heap_;
  }
  arrow::Status Consume(const arrow::Array* values, GroupIds ids) override {
    if (values == nullptr) {
      return arrow::Status::Invalid("MIN and MAX need an argument column");
    }
    ARROW_RETURN_NOT_OK(CheckRows(values, type_.get(), ids, num_groups()));
    const auto& typed = static_cast<const Traits::ArrayType&>(*values);
    const bool nulls = values->null_count() != 0;
    for (std::size_t i = 0; i < ids.size(); ++i) {
      const auto row = static_cast<std::int64_t>(i);
      if (nulls && typed.IsNull(row)) {
        continue;
      }
      Offer(ids[i], Traits::Get(typed, row));
    }
    return arrow::Status::OK();
  }
  arrow::Status MergeGroups(const GroupedAggregateState& other, GroupIds from,
                            GroupIds to) override {
    ARROW_ASSIGN_OR_RAISE(const auto* same, SameKind<GroupedMinMax>(other, from, to, num_groups()));
    if (same->min_ != min_) {
      return arrow::Status::Invalid("cannot merge MIN with MAX");
    }
    for (std::size_t i = 0; i < from.size(); ++i) {
      const std::uint32_t g = from[i];
      if (same->seen_[g] == Seen::kValue) {
        Offer(to[i], Traits::View(same->best_[g]));
      } else if (same->seen_[g] == Seen::kNaN && seen_[to[i]] == Seen::kNothing) {
        seen_[to[i]] = Seen::kNaN;
      }
    }
    return arrow::Status::OK();
  }
  [[nodiscard]] arrow::Result<std::shared_ptr<arrow::Array>> Finalize(
      std::uint32_t begin, std::uint32_t end, arrow::MemoryPool* pool) const override {
    ARROW_RETURN_NOT_OK(GroupRange(seen_, begin, end).status());
    typename Traits::BuilderType builder(type_, pool);
    ARROW_RETURN_NOT_OK(builder.Reserve(static_cast<std::int64_t>(end - begin)));
    for (std::size_t g = begin; g < end; ++g) {
      switch (seen_[g]) {
        case Seen::kNothing:
          ARROW_RETURN_NOT_OK(builder.AppendNull());
          break;
        case Seen::kNaN:
          if constexpr (std::is_floating_point_v<Value>) {
            ARROW_RETURN_NOT_OK(builder.Append(std::numeric_limits<Value>::quiet_NaN()));
          }
          break;
        case Seen::kValue:
          ARROW_RETURN_NOT_OK(builder.Append(Traits::View(best_[g])));
          break;
      }
    }
    return builder.Finish();
  }

 private:
  enum class Seen : std::uint8_t { kNothing, kNaN, kValue };

  void Offer(std::uint32_t group, Value value) {
    if constexpr (std::is_floating_point_v<Value>) {
      if (std::isnan(value)) {
        if (seen_[group] == Seen::kNothing) {
          seen_[group] = Seen::kNaN;
        }
        return;
      }
    }
    if (seen_[group] == Seen::kValue) {
      const Value best = Traits::View(best_[group]);
      if (!(min_ ? value < best : best < value)) {
        return;
      }
    }
    if constexpr (std::is_same_v<typename Traits::Stored, std::string>) {
      heap_ -= HeapBytes(best_[group]);
      best_[group] = Traits::Store(value);
      heap_ += HeapBytes(best_[group]);
    } else {
      best_[group] = Traits::Store(value);
    }
    seen_[group] = Seen::kValue;
  }

  // The bytes a stored VARCHAR holds outside its std::string (none within the small-string
  // buffer).
  static std::int64_t HeapBytes(const std::string& value) {
    return value.capacity() > std::string().capacity()
               ? static_cast<std::int64_t>(value.capacity()) + 1
               : 0;
  }

  bool min_;
  std::shared_ptr<arrow::DataType> type_;
  std::vector<typename Traits::Stored> best_;
  std::vector<Seen> seen_;
  std::int64_t heap_ = 0;  // HeapBytes of the VARCHAR values in best_
};

arrow::Result<std::unique_ptr<GroupedAggregateState>> SumState(bool average,
                                                               plan::LogicalType input) {
  switch (input.id()) {
    case plan::LogicalType::kSmallInt:
      return std::make_unique<GroupedIntegerSum<arrow::Int16Type>>(average);
    case plan::LogicalType::kInteger:
      return std::make_unique<GroupedIntegerSum<arrow::Int32Type>>(average);
    case plan::LogicalType::kBigInt:
      return std::make_unique<GroupedIntegerSum<arrow::Int64Type>>(average);
    case plan::LogicalType::kUSmallInt:
      return std::make_unique<GroupedIntegerSum<arrow::UInt16Type>>(average);
    case plan::LogicalType::kHugeInt:
    case plan::LogicalType::kDecimal:
      return std::make_unique<GroupedHugeIntSum>(average, input);
    case plan::LogicalType::kDouble:
      return std::make_unique<GroupedDoubleSum>(average);
    case plan::LogicalType::kVarchar:
    case plan::LogicalType::kDate:
    case plan::LogicalType::kTimestamp:
    case plan::LogicalType::kBoolean:
      break;
  }
  return arrow::Status::Invalid(average ? "AVG" : "SUM", " of ", plan::ToString(input));
}

std::unique_ptr<GroupedAggregateState> MinMaxState(bool min, plan::LogicalType input) {
  auto type = plan::ToArrow(input);
  switch (input.id()) {
    case plan::LogicalType::kSmallInt:
      return std::make_unique<GroupedMinMax<arrow::Int16Type>>(min, std::move(type));
    case plan::LogicalType::kInteger:
      return std::make_unique<GroupedMinMax<arrow::Int32Type>>(min, std::move(type));
    case plan::LogicalType::kBigInt:
      return std::make_unique<GroupedMinMax<arrow::Int64Type>>(min, std::move(type));
    case plan::LogicalType::kUSmallInt:
      return std::make_unique<GroupedMinMax<arrow::UInt16Type>>(min, std::move(type));
    case plan::LogicalType::kHugeInt:
    case plan::LogicalType::kDecimal:  // decimal128 of the column's own precision and scale
      return std::make_unique<GroupedMinMax<arrow::Decimal128Type>>(min, std::move(type));
    case plan::LogicalType::kDouble:
      return std::make_unique<GroupedMinMax<arrow::DoubleType>>(min, std::move(type));
    case plan::LogicalType::kVarchar:
      return std::make_unique<GroupedMinMax<arrow::BinaryType>>(min, std::move(type));
    case plan::LogicalType::kDate:
      return std::make_unique<GroupedMinMax<arrow::Date32Type>>(min, std::move(type));
    case plan::LogicalType::kTimestamp:
      return std::make_unique<GroupedMinMax<arrow::TimestampType>>(min, std::move(type));
    case plan::LogicalType::kBoolean:  // never an aggregate argument
      break;
  }
  return nullptr;
}

// ---- COUNT(DISTINCT c) ----

// The distinct (group id, value) pairs, as the keys of a grouper: a pair seen for the first time
// adds one to its group, unless the value is NULL. DOUBLE values are normalized first (-0.0 is 0.0,
// every NaN one value). Merge feeds the other state's pairs through the group map.
class GroupedCountDistinct final : public GroupedAggregateState {
 public:
  static arrow::Result<std::unique_ptr<GroupedAggregateState>> Make(
      std::shared_ptr<arrow::DataType> type, arrow::MemoryPool* pool) {
    auto state =
        std::unique_ptr<GroupedCountDistinct>(new GroupedCountDistinct(std::move(type), pool));
    ARROW_ASSIGN_OR_RAISE(
        state->pairs_,
        arrow::compute::Grouper::Make({arrow::uint32(), state->type_}, state->kernels_.get()));
    return state;
  }

  [[nodiscard]] std::uint32_t num_groups() const override {
    return static_cast<std::uint32_t>(counts_.size());
  }
  void Resize(std::uint32_t num_groups) override { counts_.resize(num_groups, 0); }
  [[nodiscard]] std::int64_t memory_usage() const override { return VectorBytes(counts_); }
  arrow::Status Consume(const arrow::Array* values, GroupIds ids) override {
    if (values == nullptr) {
      return arrow::Status::Invalid("COUNT(DISTINCT) needs an argument column");
    }
    ARROW_RETURN_NOT_OK(CheckRows(values, type_.get(), ids, num_groups()));
    arrow::UInt32Builder groups(pool_);
    ARROW_RETURN_NOT_OK(groups.AppendValues(ids.data(), static_cast<std::int64_t>(ids.size())));
    ARROW_ASSIGN_OR_RAISE(auto group_array, groups.Finish());
    return AddPairs(group_array, arrow::MakeArray(values->data()));
  }
  arrow::Status MergeGroups(const GroupedAggregateState& other, GroupIds from,
                            GroupIds to) override {
    ARROW_ASSIGN_OR_RAISE(const auto* same,
                          SameKind<GroupedCountDistinct>(other, from, to, num_groups()));
    if (!same->type_->Equals(*type_)) {
      return arrow::Status::Invalid("cannot merge COUNT(DISTINCT) of different types");
    }
    ARROW_ASSIGN_OR_RAISE(const Pairs* pairs, same->PairsByGroup());
    // The pairs of the merged groups, in the order of `from`.
    arrow::UInt32Builder groups(pool_);
    arrow::Int64Builder rows(pool_);
    for (std::size_t i = 0; i < from.size(); ++i) {
      for (std::int64_t k = pairs->start[from[i]]; k < pairs->start[from[i] + 1]; ++k) {
        ARROW_RETURN_NOT_OK(groups.Append(to[i]));
        ARROW_RETURN_NOT_OK(rows.Append(pairs->order[static_cast<std::size_t>(k)]));
      }
    }
    if (groups.length() == 0) {
      return arrow::Status::OK();
    }
    ARROW_ASSIGN_OR_RAISE(auto group_array, groups.Finish());
    ARROW_ASSIGN_OR_RAISE(const auto row_array, rows.Finish());
    ARROW_ASSIGN_OR_RAISE(
        const arrow::Datum values,
        arrow::compute::Take(pairs->values, row_array, arrow::compute::TakeOptions::NoBoundsCheck(),
                             kernels_.get()));
    return AddPairs(group_array, values.make_array());
  }
  [[nodiscard]] arrow::Result<std::shared_ptr<arrow::Array>> Finalize(
      std::uint32_t begin, std::uint32_t end, arrow::MemoryPool* pool) const override {
    ARROW_ASSIGN_OR_RAISE(const auto counts, GroupRange(counts_, begin, end));
    return Int64s(counts, pool);
  }

 private:
  GroupedCountDistinct(std::shared_ptr<arrow::DataType> type, arrow::MemoryPool* pool)
      : type_(std::move(type)),
        pool_(pool),
        kernels_(std::make_unique<arrow::compute::ExecContext>(pool)) {}

  // Rows of (group id, value); a pair new to the grouper counts once for its group.
  arrow::Status AddPairs(const std::shared_ptr<arrow::Array>& groups,
                         std::shared_ptr<arrow::Array> values) {
    const std::int64_t rows = values->length();
    if (rows == 0) {
      return arrow::Status::OK();
    }
    if (type_->id() == arrow::Type::DOUBLE) {
      ARROW_ASSIGN_OR_RAISE(values, NormalizeDoubleKey(values, pool_));
    }
    const std::uint32_t before = pairs_->num_groups();
    const arrow::compute::ExecBatch batch({arrow::Datum(groups), arrow::Datum(values)}, rows);
    ARROW_ASSIGN_OR_RAISE(const arrow::Datum pair_ids,
                          pairs_->Consume(arrow::compute::ExecSpan(batch)));
    const std::uint32_t after = pairs_->num_groups();
    if (after == before) {
      return arrow::Status::OK();
    }
    const auto ids = std::static_pointer_cast<arrow::UInt32Array>(pair_ids.make_array());
    const auto& group_ids = static_cast<const arrow::UInt32Array&>(*groups);
    std::vector<bool> seen(after - before, false);
    for (std::int64_t i = 0; i < rows; ++i) {
      const std::uint32_t pair = ids->Value(i);
      if (pair < before || seen[pair - before]) {
        continue;
      }
      seen[pair - before] = true;
      if (values->IsValid(i)) {
        ++counts_[group_ids.Value(i)];
      }
    }
    return arrow::Status::OK();
  }

  std::shared_ptr<arrow::DataType> type_;
  arrow::MemoryPool* pool_;
  std::unique_ptr<arrow::compute::ExecContext> kernels_;
  // The distinct pairs of a state merged into others, by group: computed once, then read by the
  // merges, possibly from several threads at once (a partitioned merge reads each part's state
  // from every partition).
  struct Pairs {
    std::shared_ptr<arrow::Array> values;  // the pairs' values
    std::vector<std::int64_t> order;       // pair rows sorted by group
    std::vector<std::int64_t> start;       // group g's pairs: order[start[g] .. start[g + 1])
  };
  arrow::Result<const Pairs*> PairsByGroup() const {
    std::call_once(pairs_once_, [this] {
      auto uniques = pairs_->GetUniques();
      if (!uniques.ok()) {
        pairs_status_ = uniques.status();
        return;
      }
      const auto groups =
          std::static_pointer_cast<arrow::UInt32Array>(uniques->values.at(0).make_array());
      by_group_.values = uniques->values.at(1).make_array();
      by_group_.start.assign(counts_.size() + 1, 0);
      for (std::int64_t i = 0; i < groups->length(); ++i) {
        ++by_group_.start[groups->Value(i) + 1];
      }
      for (std::size_t g = 1; g < by_group_.start.size(); ++g) {
        by_group_.start[g] += by_group_.start[g - 1];
      }
      by_group_.order.resize(static_cast<std::size_t>(groups->length()));
      std::vector<std::int64_t> next(by_group_.start.begin(), by_group_.start.end() - 1);
      for (std::int64_t i = 0; i < groups->length(); ++i) {
        by_group_.order[static_cast<std::size_t>(next[groups->Value(i)]++)] = i;
      }
    });
    ARROW_RETURN_NOT_OK(pairs_status_);
    return &by_group_;
  }

  std::unique_ptr<arrow::compute::Grouper> pairs_;
  std::vector<std::int64_t> counts_;
  mutable std::once_flag pairs_once_;
  mutable arrow::Status pairs_status_;
  mutable Pairs by_group_;
};

}  // namespace

arrow::Status GroupedAggregateState::Merge(const GroupedAggregateState& other,
                                           std::span<const std::uint32_t> group_map) {
  if (group_map.size() != other.num_groups()) {
    return arrow::Status::Invalid("a group map of ", group_map.size(), " entries for ",
                                  other.num_groups(), " groups");
  }
  std::vector<std::uint32_t> from(group_map.size());
  std::ranges::iota(from, 0U);
  return MergeGroups(other, from, group_map);
}

arrow::Result<std::unique_ptr<GroupedAggregateState>> MakeGroupedAggregateState(
    plan::AggKind kind, std::optional<plan::LogicalType> input, plan::LogicalType result,
    arrow::MemoryPool* pool) {
  const auto invalid = [&] {
    return arrow::Status::Invalid("no grouped aggregate ", plan::ToString(kind), "(",
                                  input.has_value() ? plan::ToString(*input) : std::string("*"),
                                  ") -> ", plan::ToString(result));
  };
  if (kind == plan::AggKind::kCountStar) {
    if (input.has_value() || result != plan::LogicalType::kBigInt) {
      return invalid();
    }
    return std::make_unique<GroupedCount>(nullptr);
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
      return std::make_unique<GroupedCount>(plan::ToArrow(*input));
    case plan::AggKind::kSum:
    case plan::AggKind::kAvg: {
      const bool average = kind == plan::AggKind::kAvg;
      if (average &&
          (*input == plan::LogicalType::kDate || *input == plan::LogicalType::kTimestamp)) {
        if (result != plan::LogicalType::kTimestamp) {
          return invalid();
        }
        return std::make_unique<GroupedTemporalAvg>();
      }
      plan::LogicalType expected = !average && plan::IsInteger(*input) ? plan::LogicalType::kHugeInt
                                                                       : plan::LogicalType::kDouble;
      const bool decimal = *input == plan::LogicalType::kDecimal;
      if (decimal && !average) {
        expected = plan::LogicalType::Decimal(plan::LogicalType::kMaxDecimalWidth, input->scale());
      }
      if ((!plan::IsNumeric(*input) && !decimal) || result != expected) {
        return invalid();
      }
      return SumState(average, *input);
    }
    case plan::AggKind::kMin:
    case plan::AggKind::kMax:
      if (result != *input) {
        return invalid();
      }
      return MinMaxState(kind == plan::AggKind::kMin, *input);
    case plan::AggKind::kCountDistinct:
      if (result != plan::LogicalType::kBigInt) {
        return invalid();
      }
      return GroupedCountDistinct::Make(plan::ToArrow(*input), pool);
  }
  return invalid();
}

}  // namespace antb1::exec
