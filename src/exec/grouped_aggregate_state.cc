#include "antb1/exec/grouped_aggregate_state.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeinfo>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <arrow/compute/api_vector.h>
#include <arrow/compute/exec.h>
#include <arrow/compute/row/grouper.h>
#include <arrow/util/decimal.h>

#include "antb1/common/int128.h"
#include "antb1/common/narrow.h"
#include "antb1/exec/memory_budget.h"
#include "antb1/plan/literal.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/types.h"

#include "decimal_bytes.h"
#include "double_key.h"
#include "group_vector.h"
#include "grouped_aggregate_program.h"
#include "grouped_sum_alias.h"
#include "temporal_average.h"
#include "variance.h"

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
  return (... + Narrow<std::int64_t>(vectors.capacity() * sizeof(typename Vectors::value_type)));
}

template <class State>
arrow::Result<const State*> SameState(const GroupedAggregateState& other) {
  const auto* same = dynamic_cast<const State*>(&other);
  if (same == nullptr) {
    return arrow::Status::Invalid("cannot merge states of different aggregates");
  }
  return same;
}

arrow::Status CheckMergeGroups(GroupIds from, GroupIds to, std::uint32_t source_groups,
                               std::uint32_t groups) {
  if (from.size() != to.size()) {
    return arrow::Status::Invalid("a merge of ", from.size(), " groups into ", to.size());
  }
  for (std::size_t i = 0; i < from.size(); ++i) {
    if (from[i] >= source_groups || to[i] >= groups) {
      return arrow::Status::Invalid("a merge of group ", from[i], " of ", source_groups,
                                    " into group ", to[i], " of ", groups);
    }
  }
  return arrow::Status::OK();
}

template <class State>
arrow::Result<const State*> SameKind(const GroupedAggregateState& other, GroupIds from, GroupIds to,
                                     std::uint32_t num_groups) {
  ARROW_ASSIGN_OR_RAISE(const auto* same, SameState<State>(other));
  ARROW_RETURN_NOT_OK(CheckMergeGroups(from, to, other.num_groups(), num_groups));
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

// The error of a sum outside its 38-digit type, HUGEINT or DECIMAL(38, s).
arrow::Status SumOutOfRange(plan::LogicalType type) {
  return arrow::Status::ExecutionError("SUM overflow: the result is outside the range of ",
                                       plan::ToString(type), " (38 decimal digits)");
}

// OK when every group with values has its sum within `type` (HugeInts would succeed).
arrow::Status CheckSumRange(std::span<const Int128> sums, std::span<const std::int64_t> counts,
                            plan::LogicalType type) {
  const plan::IntegerRange range = plan::RangeOf(type);
  for (std::size_t g = 0; g < sums.size(); ++g) {
    if (counts[g] != 0 && (sums[g] < range.min || sums[g] > range.max)) {
      return SumOutOfRange(type);
    }
  }
  return arrow::Status::OK();
}

// Exact sums as their 38-digit type, HUGEINT or DECIMAL(38, s) (decimal128 within +-(10^38 - 1));
// NULL for a group without values.
arrow::Result<std::shared_ptr<arrow::Array>> HugeInts(std::span<const Int128> sums,
                                                      std::span<const std::int64_t> counts,
                                                      plan::LogicalType type,
                                                      arrow::MemoryPool* pool) {
  arrow::Decimal128Builder builder(DecimalsType(type), pool);
  ARROW_RETURN_NOT_OK(builder.Reserve(static_cast<std::int64_t>(sums.size())));
  const plan::IntegerRange range = plan::RangeOf(type);
  for (std::size_t g = 0; g < sums.size(); ++g) {
    if (counts[g] == 0) {
      builder.UnsafeAppendNull();
      continue;
    }
    if (sums[g] < range.min || sums[g] > range.max) {
      return SumOutOfRange(type);
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

// sum / count per group as DOUBLE, NULL for a group without values: an integer sum as DuckDB
// divides it (DuckDbIntegerAverage; `smallint` for a SMALLINT's), a DOUBLE sum in double.
template <class Sum>
arrow::Result<std::shared_ptr<arrow::Array>> Averages(std::span<const Sum> sums,
                                                      std::span<const std::int64_t> counts,
                                                      arrow::MemoryPool* pool,
                                                      bool smallint = false) {
  arrow::DoubleBuilder builder(pool);
  ARROW_RETURN_NOT_OK(builder.Reserve(static_cast<std::int64_t>(sums.size())));
  for (std::size_t g = 0; g < sums.size(); ++g) {
    if (counts[g] == 0) {
      builder.UnsafeAppendNull();
    } else if constexpr (std::is_same_v<Sum, double>) {
      builder.UnsafeAppend(sums[g] / static_cast<double>(counts[g]));
    } else {
      builder.UnsafeAppend(DuckDbIntegerAverage(sums[g], counts[g], smallint));
    }
  }
  return builder.Finish();
}

}  // namespace

// The program binds input types once per batch and checks each group map once for all calls.
// Standalone state callers still get the checked, whole-array Consume and MergeGroups APIs.
class BatchAggregateState : public GroupedAggregateState {
 public:
  virtual arrow::Status ReserveGroups(std::uint32_t groups, MemoryReservation& memory) = 0;
  virtual arrow::Status ConsumeRange(const arrow::Array* values, std::size_t offset,
                                     GroupIds ids) = 0;
  virtual arrow::Status MergeRange(const GroupedAggregateState& other, GroupIds from,
                                   GroupIds to) = 0;
  arrow::Status MergeGroups(const GroupedAggregateState& other, GroupIds from, GroupIds to) final {
    auto bounds = CheckMergeGroups(from, to, other.num_groups(), num_groups());
    if (!bounds.ok()) {
      // The standalone API diagnoses a different aggregate before an invalid group map.
      // MergeRange checks concrete state types on the valid-map path as well.
      if (typeid(*this) != typeid(other)) {
        return arrow::Status::Invalid("cannot merge states of different aggregates");
      }
      return bounds;
    }
    return MergeRange(other, from, to);
  }
};

namespace {

// ---- COUNT(*) and COUNT(c) ----

class GroupedCount final : public BatchAggregateState {
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
    return ConsumeRange(values, 0, ids);
  }
  arrow::Status ReserveGroups(std::uint32_t groups, MemoryReservation& memory) override {
    return ReserveGroupVector(counts_, groups, memory);
  }
  arrow::Status ConsumeRange(const arrow::Array* values, std::size_t offset,
                             GroupIds ids) override {
    if (values == nullptr || values->null_count() == 0) {
      for (const std::uint32_t g : ids) {
        ++counts_[g];
      }
      return arrow::Status::OK();
    }
    for (std::size_t i = 0; i < ids.size(); ++i) {
      counts_[ids[i]] += values->IsValid(static_cast<std::int64_t>(offset + i)) ? 1 : 0;
    }
    return arrow::Status::OK();
  }
  arrow::Status MergeRange(const GroupedAggregateState& other, GroupIds from,
                           GroupIds to) override {
    ARROW_ASSIGN_OR_RAISE(const auto* same, SameState<GroupedCount>(other));
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
class GroupedIntegerSum final : public BatchAggregateState {
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
    return ConsumeRange(values, 0, ids);
  }
  arrow::Status ReserveGroups(std::uint32_t groups, MemoryReservation& memory) override {
    ARROW_RETURN_NOT_OK(ReserveGroupVector(sums_, groups, memory));
    return ReserveGroupVector(counts_, groups, memory);
  }
  arrow::Status ConsumeRange(const arrow::Array* values, std::size_t offset,
                             GroupIds ids) override {
    const auto* data = values->data()->GetValues<CType>(1);
    const bool nulls = values->null_count() != 0;
    for (std::size_t i = 0; i < ids.size(); ++i) {
      if (nulls && values->IsNull(static_cast<std::int64_t>(offset + i))) {
        continue;
      }
      sums_[ids[i]] += data[offset + i];
      ++counts_[ids[i]];
    }
    return arrow::Status::OK();
  }
  arrow::Status MergeRange(const GroupedAggregateState& other, GroupIds from,
                           GroupIds to) override {
    ARROW_ASSIGN_OR_RAISE(const auto* same, SameState<GroupedIntegerSum>(other));
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
    return average_ ? Averages(sums, counts, pool, std::is_same_v<ArrowType, arrow::Int16Type>)
                    : HugeInts(sums, counts, plan::LogicalType::kHugeInt, pool);
  }

 private:
  bool average_;
  std::vector<Int128> sums_;
  std::vector<std::int64_t> counts_;
};

// The final half of a count computed in two places, per group (ADR 0032's G2c): BIGINT counts
// summed into a BIGINT. A NULL partial is one the join found no row for, which counted nothing, so
// it adds zero and a group of nothing but NULLs answers 0 rather than NULL -- which is what
// COUNT(col) answers for a row with no match. See the ungrouped SumCountsState for why a BIGINT
// holds the total.
class GroupedSumCounts final : public BatchAggregateState {
 public:
  [[nodiscard]] std::uint32_t num_groups() const override {
    return static_cast<std::uint32_t>(sums_.size());
  }
  void Resize(std::uint32_t num_groups) override { sums_.resize(num_groups, 0); }
  [[nodiscard]] std::int64_t memory_usage() const override { return VectorBytes(sums_); }
  arrow::Status Consume(const arrow::Array* values, GroupIds ids) override {
    if (values == nullptr) {
      return arrow::Status::Invalid("SUM of counts needs an argument column");
    }
    ARROW_RETURN_NOT_OK(CheckRows(values, arrow::int64().get(), ids, num_groups()));
    return ConsumeRange(values, 0, ids);
  }
  arrow::Status ReserveGroups(std::uint32_t groups, MemoryReservation& memory) override {
    return ReserveGroupVector(sums_, groups, memory);
  }
  arrow::Status ConsumeRange(const arrow::Array* values, std::size_t offset,
                             GroupIds ids) override {
    const auto* data = values->data()->GetValues<std::int64_t>(1);
    const bool nulls = values->null_count() != 0;
    for (std::size_t i = 0; i < ids.size(); ++i) {
      if (nulls && values->IsNull(static_cast<std::int64_t>(offset + i))) {
        continue;
      }
      sums_[ids[i]] += data[offset + i];
    }
    return arrow::Status::OK();
  }
  arrow::Status MergeRange(const GroupedAggregateState& other, GroupIds from,
                           GroupIds to) override {
    ARROW_ASSIGN_OR_RAISE(const auto* same, SameState<GroupedSumCounts>(other));
    for (std::size_t i = 0; i < from.size(); ++i) {
      sums_[to[i]] += same->sums_[from[i]];
    }
    return arrow::Status::OK();
  }
  [[nodiscard]] arrow::Result<std::shared_ptr<arrow::Array>> Finalize(
      std::uint32_t begin, std::uint32_t end, arrow::MemoryPool* pool) const override {
    ARROW_ASSIGN_OR_RAISE(const auto sums, GroupRange(sums_, begin, end));
    std::vector<std::int64_t> totals;
    totals.reserve(sums.size());
    for (const Int128 sum : sums) {
      const std::optional<std::int64_t> total = Int128ToInt64(sum);
      if (!total.has_value()) {
        return arrow::Status::Invalid("SUM of counts outside BIGINT");
      }
      totals.push_back(*total);
    }
    return Int64s(totals, pool);
  }
  [[nodiscard]] arrow::Status CheckFinalize() const override {
    for (const Int128 sum : sums_) {
      if (!Int128ToInt64(sum).has_value()) {
        return arrow::Status::Invalid("SUM of counts outside BIGINT");
      }
    }
    return arrow::Status::OK();
  }

 private:
  std::vector<Int128> sums_;
};

// AVG of DATE or TIMESTAMP values per group: a TIMESTAMP, averaged in 128 bits as DuckDB does.
class GroupedTemporalAvg final : public BatchAggregateState {
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
    return ConsumeRange(values, 0, ids);
  }
  arrow::Status ReserveGroups(std::uint32_t groups, MemoryReservation& memory) override {
    ARROW_RETURN_NOT_OK(ReserveGroupVector(sums_, groups, memory));
    return ReserveGroupVector(counts_, groups, memory);
  }
  arrow::Status ConsumeRange(const arrow::Array* values, std::size_t offset,
                             GroupIds ids) override {
    const bool nulls = values->null_count() != 0;
    for (std::size_t i = 0; i < ids.size(); ++i) {
      const auto row = static_cast<std::int64_t>(offset + i);
      if (nulls && values->IsNull(row)) {
        continue;
      }
      ARROW_ASSIGN_OR_RAISE(const std::int64_t micros, TemporalMicros(*values, row));
      sums_[ids[i]] += micros;
      ++counts_[ids[i]];
    }
    return arrow::Status::OK();
  }
  arrow::Status MergeRange(const GroupedAggregateState& other, GroupIds from,
                           GroupIds to) override {
    ARROW_ASSIGN_OR_RAISE(const auto* same, SameState<GroupedTemporalAvg>(other));
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

// The unscaled value of row `row` of a decimal128 values buffer: Arrow stores them as 16
// contiguous bytes a row, which is what arrow::Decimal128's byte constructor reads. The buffer's
// base is taken once by the caller rather than per row, since Decimal128Array::GetValue redoes the
// array's offset arithmetic on every call.
Int128 HugeIntAt(const std::uint8_t* base, std::size_t row) {
  const arrow::Decimal128 value(base + (row * sizeof(Int128)));
  const UInt128 bits =
      (static_cast<UInt128>(static_cast<std::uint64_t>(value.high_bits())) << 64U) |
      static_cast<UInt128>(value.low_bits());
  return static_cast<Int128>(bits);
}

// SUM or AVG of a HUGEINT or DECIMAL argument (decimal128): every addition is checked. A
// DECIMAL(p,s) sums to DECIMAL(38,s) and averages as DuckDB does.
class GroupedHugeIntSum final : public BatchAggregateState {
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
    return ConsumeRange(values, 0, ids);
  }
  arrow::Status ReserveGroups(std::uint32_t groups, MemoryReservation& memory) override {
    ARROW_RETURN_NOT_OK(ReserveGroupVector(sums_, groups, memory));
    return ReserveGroupVector(counts_, groups, memory);
  }
  arrow::Status ConsumeRange(const arrow::Array* values, std::size_t offset,
                             GroupIds ids) override {
    const arrow::FixedSizeBinaryArray& decimals = AsDecimalValues(*values);
    // Nothing to add. Also the one case where the array is allowed to carry no values buffer, so
    // the base below would be null: the loop would skip every row and never read it, which makes
    // this a guard that states the case rather than one an answer depends on.
    if (ids.empty() || decimals.null_count() == decimals.length()) {
      return arrow::Status::OK();
    }
    // One overflow test for the batch rather than one per row. __builtin_add_overflow leaves a
    // defined wrapped sum and reports whether it wrapped, so collecting that in a flag and failing
    // after the loop says exactly what failing inside it said -- the message names no row or value
    // -- while the loop keeps no branch to an error and builds no arrow::Status per row. The
    // declared width cannot rule an overflow out instead: a file may hold a decimal128 outside its
    // column's precision and the scan does not narrow it (see compute.cc's DecimalOperand), so only
    // the addition itself knows.
    // The stride is decided here, once a batch, so the loops below carry no test for it.
    const bool overflow = values->type_id() == arrow::Type::DECIMAL64
                              ? AddAll<sizeof(std::int64_t)>(decimals, offset, ids)
                              : AddAll<sizeof(Int128)>(decimals, offset, ids);
    return overflow ? Overflowed() : arrow::Status::OK();
  }
  arrow::Status MergeRange(const GroupedAggregateState& other, GroupIds from,
                           GroupIds to) override {
    ARROW_ASSIGN_OR_RAISE(const auto* same, SameState<GroupedHugeIntSum>(other));
    for (std::size_t i = 0; i < from.size(); ++i) {
      ARROW_RETURN_NOT_OK(Add(to[i], same->sums_[from[i]], same->counts_[from[i]]));
    }
    return arrow::Status::OK();
  }
  [[nodiscard]] arrow::Result<std::shared_ptr<arrow::Array>> Finalize(
      std::uint32_t begin, std::uint32_t end, arrow::MemoryPool* pool) const override {
    return FinalizeAs(average_, begin, end, pool);
  }
  [[nodiscard]] arrow::Status CheckFinalize() const override { return CheckAs(average_); }

  // This accumulator as either SUM or AVG, which is what lets one of the two read the other's
  // state instead of adding the same column again (grouped_sum_alias.h). The sums and the counts
  // are the same either way; only the output differs.
  [[nodiscard]] arrow::Result<std::shared_ptr<arrow::Array>> FinalizeAs(
      bool average, std::uint32_t begin, std::uint32_t end, arrow::MemoryPool* pool) const {
    ARROW_ASSIGN_OR_RAISE(const auto sums, GroupRange(sums_, begin, end));
    ARROW_ASSIGN_OR_RAISE(const auto counts, GroupRange(counts_, begin, end));
    if (average) {
      return input_ != plan::LogicalType::kDecimal ? Averages(sums, counts, pool)
                                                   : DecimalAverages(sums, counts, input_, pool);
    }
    return HugeInts(sums, counts, SumType(), pool);
  }
  // CheckFinalize of FinalizeAs(average, ...) over every group: only a SUM can fail, and only of
  // values beyond 18 digits (fewer than 2^63 values below 10^18 sum to less than 10^37), so the
  // pass over every group is skipped for the narrow decimals that most sums are of.
  [[nodiscard]] arrow::Status CheckAs(bool average) const {
    const bool narrow = input_ == plan::LogicalType::kDecimal &&
                        input_.width() <= plan::LogicalType::kMaxNarrowDecimalWidth;
    return average || narrow ? arrow::Status::OK() : CheckSumRange(sums_, counts_, SumType());
  }

 private:
  // The type of the SUM: HUGEINT, or DECIMAL(38, s) for a decimal input of scale s.
  [[nodiscard]] plan::LogicalType SumType() const {
    return input_ != plan::LogicalType::kDecimal
               ? plan::LogicalType(plan::LogicalType::kHugeInt)
               : plan::LogicalType::Decimal(plan::LogicalType::kMaxDecimalWidth, input_.scale());
  }

  // The unscaled value of row `row` of a buffer of `kBytes`-wide values. A decimal64's eight bytes
  // are its value in two's complement, little-endian on both targets, so they widen by sign
  // extension (ADR 0021's decimal64 item).
  template <std::size_t kBytes>
  static Int128 ValueAt(const std::uint8_t* base, std::size_t row) {
    if constexpr (kBytes == sizeof(std::int64_t)) {
      std::int64_t value = 0;
      std::memcpy(&value, base + (row * kBytes), sizeof(value));
      return value;
    } else {
      return HugeIntAt(base, row);
    }
  }

  // Row `row` of a decimal buffer into `group`: true when the sum wrapped, which the caller
  // collects for the whole batch. Callers use `|=` rather than `||` because short-circuiting puts
  // back the per-row branch this is here to remove; either spelling answers the same, since a batch
  // that sets the flag fails and its sums are discarded.
  template <std::size_t kBytes>
  [[nodiscard]] bool AddAt(std::uint32_t group, const std::uint8_t* base, std::size_t row) {
    ++counts_[group];
    return __builtin_add_overflow(sums_[group], ValueAt<kBytes>(base, row), &sums_[group]);
  }

  // Every row of `values` into its group, at one stride. A column with no nulls gets its own loop
  // so it pays no validity test per row, as KeyHashes splits them for the same reason
  // (group_table.cc).
  template <std::size_t kBytes>
  [[nodiscard]] bool AddAll(const arrow::FixedSizeBinaryArray& values, std::size_t offset,
                            GroupIds ids) {
    const std::uint8_t* const base = values.GetValue(static_cast<std::int64_t>(offset));
    bool overflow = false;
    if (values.null_count() == 0) {
      for (std::size_t i = 0; i < ids.size(); ++i) {
        overflow |= AddAt<kBytes>(ids[i], base, i);
      }
      return overflow;
    }
    for (std::size_t i = 0; i < ids.size(); ++i) {
      if (values.IsNull(static_cast<std::int64_t>(offset + i))) {
        continue;
      }
      overflow |= AddAt<kBytes>(ids[i], base, i);
    }
    return overflow;
  }

  [[nodiscard]] arrow::Status Overflowed() const {
    return arrow::Status::ExecutionError(average_ ? "AVG" : "SUM", " overflow: the sum of a ",
                                         plan::ToString(input_), " column exceeds 128 bits");
  }

  // A merge folds one value per group, not per row, so it keeps the checked form.
  arrow::Status Add(std::uint32_t group, Int128 value, std::int64_t count) {
    const auto next = CheckedAdd(sums_[group], value);
    if (!next.has_value()) {
      return Overflowed();
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

// Finalizes another state's accumulator and does nothing else (grouped_sum_alias.h). Every method
// that would touch an accumulator is empty, because the state it points at has one and is driven
// by its own entry of the same table: consuming or merging here would count every row twice.
class GroupedSumAlias final : public GroupedAggregateState {
 public:
  GroupedSumAlias(const GroupedHugeIntSum* owner, bool average)
      : owner_(owner), average_(average) {}

  [[nodiscard]] std::uint32_t num_groups() const override { return owner_->num_groups(); }
  void Resize(std::uint32_t /*num_groups*/) override {}  // the owner's entry resizes it
  [[nodiscard]] std::int64_t memory_usage() const override { return 0; }  // the owner charges it
  arrow::Status Consume(const arrow::Array* /*values*/, GroupIds /*ids*/) override {
    return arrow::Status::OK();  // the owner consumes the same column
  }
  arrow::Status MergeGroups(const GroupedAggregateState& other, GroupIds from,
                            GroupIds to) override {
    // Nothing to fold, but the shapes are still checked: a table merged into this one must have
    // an alias at this aggregate too, or the two disagree about which entry owns the accumulator.
    ARROW_RETURN_NOT_OK(SameKind<GroupedSumAlias>(other, from, to, num_groups()).status());
    return arrow::Status::OK();
  }
  [[nodiscard]] arrow::Result<std::shared_ptr<arrow::Array>> Finalize(
      std::uint32_t begin, std::uint32_t end, arrow::MemoryPool* pool) const override {
    return owner_->FinalizeAs(average_, begin, end, pool);
  }
  [[nodiscard]] arrow::Status CheckFinalize() const override { return owner_->CheckAs(average_); }

 private:
  const GroupedHugeIntSum* owner_;
  bool average_;
};

// SUM or AVG of a DOUBLE column: values are added in row order within each group.
class GroupedDoubleSum final : public BatchAggregateState {
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
    return ConsumeRange(values, 0, ids);
  }
  arrow::Status ReserveGroups(std::uint32_t groups, MemoryReservation& memory) override {
    ARROW_RETURN_NOT_OK(ReserveGroupVector(sums_, groups, memory));
    return ReserveGroupVector(counts_, groups, memory);
  }
  arrow::Status ConsumeRange(const arrow::Array* values, std::size_t offset,
                             GroupIds ids) override {
    const auto* data = values->data()->GetValues<double>(1);
    const bool nulls = values->null_count() != 0;
    for (std::size_t i = 0; i < ids.size(); ++i) {
      if (nulls && values->IsNull(static_cast<std::int64_t>(offset + i))) {
        continue;
      }
      sums_[ids[i]] += data[offset + i];
      ++counts_[ids[i]];
    }
    return arrow::Status::OK();
  }
  arrow::Status MergeRange(const GroupedAggregateState& other, GroupIds from,
                           GroupIds to) override {
    ARROW_ASSIGN_OR_RAISE(const auto* same, SameState<GroupedDoubleSum>(other));
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

// ---- The variance family ----

// STDDEV_SAMP, STDDEV_POP, VAR_SAMP or VAR_POP per group (ADR 0037's AG2): each value as DuckDB
// casts it to DOUBLE, into the group's running moments in row order (variance.h). CheckFinalize
// raises DuckDB's out-of-range error of any group before the first row.
class GroupedVariance final : public GroupedAggregateState {
 public:
  GroupedVariance(plan::AggKind kind, arrow::MemoryPool* pool) : kind_(kind), pool_(pool) {}

  [[nodiscard]] std::uint32_t num_groups() const override {
    return static_cast<std::uint32_t>(moments_.size());
  }
  void Resize(std::uint32_t num_groups) override { moments_.resize(num_groups); }
  [[nodiscard]] std::int64_t memory_usage() const override { return VectorBytes(moments_); }
  arrow::Status Consume(const arrow::Array* values, GroupIds ids) override {
    if (values == nullptr) {
      return arrow::Status::Invalid("a variance aggregate needs an argument column");
    }
    ARROW_ASSIGN_OR_RAISE(const auto doubles, ToDuckDbDoubles(*values, pool_));
    ARROW_RETURN_NOT_OK(CheckRows(doubles.get(), arrow::float64().get(), ids, num_groups()));
    const double* data = doubles->raw_values();
    const bool nulls = doubles->null_count() != 0;
    for (std::size_t i = 0; i < ids.size(); ++i) {
      if (nulls && doubles->IsNull(static_cast<std::int64_t>(i))) {
        continue;
      }
      AddValue(moments_[ids[i]], data[i]);
    }
    return arrow::Status::OK();
  }
  arrow::Status MergeGroups(const GroupedAggregateState& other, GroupIds from,
                            GroupIds to) override {
    ARROW_ASSIGN_OR_RAISE(const auto* same,
                          SameKind<GroupedVariance>(other, from, to, num_groups()));
    for (std::size_t i = 0; i < from.size(); ++i) {
      Combine(same->moments_[from[i]], moments_[to[i]]);
    }
    return arrow::Status::OK();
  }
  [[nodiscard]] arrow::Result<std::shared_ptr<arrow::Array>> Finalize(
      std::uint32_t begin, std::uint32_t end, arrow::MemoryPool* pool) const override {
    ARROW_ASSIGN_OR_RAISE(const auto groups, GroupRange(moments_, begin, end));
    arrow::DoubleBuilder builder(pool);
    ARROW_RETURN_NOT_OK(builder.Reserve(static_cast<std::int64_t>(groups.size())));
    for (const Moments& group : groups) {
      ARROW_ASSIGN_OR_RAISE(const std::optional<double> result, FinalizeVariance(kind_, group));
      if (result.has_value()) {
        builder.UnsafeAppend(*result);
      } else {
        builder.UnsafeAppendNull();
      }
    }
    return builder.Finish();
  }
  [[nodiscard]] arrow::Status CheckFinalize() const override {
    for (const Moments& group : moments_) {
      ARROW_RETURN_NOT_OK(FinalizeVariance(kind_, group).status());
    }
    return arrow::Status::OK();
  }

 private:
  plan::AggKind kind_;
  arrow::MemoryPool* pool_;
  std::vector<Moments> moments_;
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
struct MinMaxTraits<arrow::Decimal64Type> {  // ADR 0021's decimal64 item: eight bytes a value
  using ArrayType = arrow::Decimal64Array;
  using BuilderType = arrow::Decimal64Builder;
  using Value = arrow::Decimal64;
  using Stored = arrow::Decimal64;
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
class GroupedMinMax final : public BatchAggregateState {
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
    return ConsumeRange(values, 0, ids);
  }
  arrow::Status ReserveGroups(std::uint32_t groups, MemoryReservation& memory) override {
    ARROW_RETURN_NOT_OK(ReserveGroupVector(best_, groups, memory));
    return ReserveGroupVector(seen_, groups, memory);
  }
  arrow::Status ConsumeRange(const arrow::Array* values, std::size_t offset,
                             GroupIds ids) override {
    const auto& typed = static_cast<const Traits::ArrayType&>(*values);
    const bool nulls = values->null_count() != 0;
    for (std::size_t i = 0; i < ids.size(); ++i) {
      const auto row = static_cast<std::int64_t>(offset + i);
      if (nulls && typed.IsNull(row)) {
        continue;
      }
      Offer(ids[i], Traits::Get(typed, row));
    }
    return arrow::Status::OK();
  }
  arrow::Status MergeRange(const GroupedAggregateState& other, GroupIds from,
                           GroupIds to) override {
    ARROW_ASSIGN_OR_RAISE(const auto* same, SameState<GroupedMinMax>(other));
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
    case plan::LogicalType::kDecimal:
      // decimal128 of the column's own precision and scale. This is the one decimal site selected
      // by the LOGICAL type rather than the physical one, so it is the one that would keep picking
      // a 16-byte traits for an 8-byte array when ADR 0021's N3 narrows the representation.
      // DecimalsType re-derives the same type and checks it, so that becomes a failure.
      if (type->id() == arrow::Type::DECIMAL64) {
        return std::make_unique<GroupedMinMax<arrow::Decimal64Type>>(min, std::move(type));
      }
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

arrow::Result<GroupedAggregateProgram> GroupedAggregateProgram::Make(
    std::span<const plan::AggregateCall> calls,
    std::span<const std::unique_ptr<GroupedAggregateState>> states, arrow::MemoryPool* pool) {
  ANTB1_CHECK(calls.size() == states.size());
  GroupedAggregateProgram program(pool);
  // Variance requires a whole-batch numeric conversion, DISTINCT has its own grouper, and
  // VARCHAR extrema can acquire heap storage inside an update. Keep their whole program on
  // the existing path. Two fallible updates could report a different first failure if their
  // chunks are interleaved, so retain call order for that case too.
  bool has_fallible = false;
  for (std::size_t i = 0; i < calls.size(); ++i) {
    if (dynamic_cast<GroupedSumAlias*>(states[i].get()) != nullptr) {
      continue;  // evaluates the owner's accumulator only during finalization
    }
    if (dynamic_cast<BatchAggregateState*>(states[i].get()) == nullptr) {
      program.supported_ = false;
      return program;
    }
    const auto& call = calls[i];
    if (!call.arg.has_value()) {
      continue;  // COUNT(*)
    }
    const auto input = call.arg->type;
    const bool string_extremum =
        (call.kind == plan::AggKind::kMin || call.kind == plan::AggKind::kMax) &&
        input == plan::LogicalType::kVarchar;
    const bool sums = call.kind == plan::AggKind::kSum || call.kind == plan::AggKind::kAvg;
    const bool fallible = sums && (input == plan::LogicalType::kHugeInt ||
                                   (input == plan::LogicalType::kDecimal &&
                                    input.width() > plan::LogicalType::kMaxNarrowDecimalWidth) ||
                                   input == plan::LogicalType::kDate);
    if (string_extremum || (fallible && has_fallible)) {
      program.supported_ = false;
      return program;
    }
    has_fallible |= fallible;
  }
  try {
    program.instructions_.reserve(calls.size());
    for (std::size_t i = 0; i < calls.size(); ++i) {
      if (auto* state = dynamic_cast<BatchAggregateState*>(states[i].get())) {
        const auto& call = calls[i];
        program.instructions_.push_back(
            {.state = state,
             .column = call.arg ? call.arg->index : -1,
             .input = call.arg ? std::optional(call.arg->type) : std::nullopt,
             .kind = call.kind});
      }
    }
  } catch (const arrow::stl::BadAlloc& error) {
    return arrow::Status::OutOfMemory(error.what());
  } catch (const std::bad_alloc&) {
    return arrow::Status::OutOfMemory("building a grouped aggregate program");
  }
  return program;
}

arrow::Result<GroupedAggregateProgram::BoundBatch> GroupedAggregateProgram::Bind(
    const arrow::RecordBatch& rows) const {
  if (!supported_) {
    return arrow::Status::Invalid("unsupported grouped aggregate program");
  }
  std::span<const std::shared_ptr<arrow::Array>> columns;
  for (const auto& op : instructions_) {
    if (!op.input.has_value()) {
      continue;
    }
    if (op.column < 0 || op.column >= rows.num_columns()) {
      return arrow::Status::Invalid("aggregate over a column outside its input");
    }
    if (columns.empty()) {
      columns = rows.columns();
    }
    const auto& column = columns[static_cast<std::size_t>(op.column)];
    const auto expected = plan::ToArrow(*op.input);
    if (!column->type()->Equals(*expected) || column->length() != rows.num_rows()) {
      return arrow::Status::Invalid("aggregate input is ", column->type()->ToString(), " with ",
                                    column->length(), " rows, expected ", expected->ToString(),
                                    " with ", rows.num_rows(), " rows");
    }
  }
  return BoundBatch(*this, columns, Narrow<std::size_t>(rows.num_rows()));
}

arrow::Status GroupedAggregateProgram::BoundBatch::Consume(std::size_t offset, GroupIds ids) const {
  if (offset > rows_ || ids.size() > rows_ - offset) {
    return arrow::Status::Invalid("aggregate input range outside the batch");
  }
  if (ids.empty() || program_.instructions_.empty()) {
    return arrow::Status::OK();
  }
  const auto groups = program_.instructions_.front().state->num_groups();
  if (std::ranges::max(ids) >= groups) {
    return arrow::Status::Invalid("group id outside the ", groups, " groups of the state");
  }
  for (const auto& op : program_.instructions_) {
    ARROW_RETURN_NOT_OK(op.state->ConsumeRange(
        op.column < 0 ? nullptr : columns_[static_cast<std::size_t>(op.column)].get(), offset,
        ids));
  }
  return arrow::Status::OK();
}

arrow::Status GroupedAggregateProgram::Resize(std::uint32_t groups,
                                              MemoryReservation& memory) const {
  ANTB1_CHECK(supported_);
  // A merge can publish first-seen key chunks after the corresponding states were updated.
  // Publishing an earlier chunk must leave the already populated later groups in place.
  if (instructions_.empty() || groups <= instructions_.front().state->num_groups()) {
    return arrow::Status::OK();
  }
  for (const auto& op : instructions_) {
    ARROW_RETURN_NOT_OK(op.state->ReserveGroups(groups, memory));
  }
  // Resize only after every reservation succeeds, so refusal leaves all state sizes aligned.
  for (const auto& op : instructions_) {
    op.state->Resize(groups);
  }
  return arrow::Status::OK();
}

arrow::Status GroupedAggregateProgram::MergeGroups(const GroupedAggregateProgram& source,
                                                   GroupIds from, GroupIds to) const {
  if (!supported_ || !source.supported_ || instructions_.size() != source.instructions_.size()) {
    return arrow::Status::Invalid("incompatible grouped aggregate programs");
  }
  if (instructions_.empty()) {
    return arrow::Status::OK();
  }
  ARROW_RETURN_NOT_OK(CheckMergeGroups(from, to, source.instructions_.front().state->num_groups(),
                                       instructions_.front().state->num_groups()));
  for (std::size_t i = 0; i < instructions_.size(); ++i) {
    const auto& op = instructions_[i];
    const auto& other = source.instructions_[i];
    if (op.kind != other.kind || op.input != other.input) {
      return arrow::Status::Invalid("incompatible grouped aggregate programs");
    }
    ARROW_RETURN_NOT_OK(op.state->MergeRange(*other.state, from, to));
  }
  return arrow::Status::OK();
}

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
    case plan::AggKind::kSumCounts:
      if (*input != plan::LogicalType::kBigInt || result != plan::LogicalType::kBigInt) {
        return invalid();
      }
      return std::make_unique<GroupedSumCounts>();
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
    case plan::AggKind::kStddevSamp:
    case plan::AggKind::kStddevPop:
    case plan::AggKind::kVarSamp:
    case plan::AggKind::kVarPop:
      if ((!plan::IsNumeric(*input) && *input != plan::LogicalType::kDecimal) ||
          result != plan::LogicalType::kDouble) {
        return invalid();
      }
      return std::make_unique<GroupedVariance>(kind, pool);
    case plan::AggKind::kSingleValue:
      // The value of a scalar subquery is one aggregate over its whole plan, never grouped (ADR
      // 0041's decision 22: a check per group would see the wrong rows).
      break;
  }
  return invalid();
}

std::unique_ptr<GroupedAggregateState> MakeSumAlias(const GroupedAggregateState& owner,
                                                    bool average) {
  // Only the Int128 accumulator for now: it is where a DECIMAL or HUGEINT SUM and AVG cost most,
  // and the integer and DOUBLE sums would each need the same split of Finalize to join in.
  const auto* sum = dynamic_cast<const GroupedHugeIntSum*>(&owner);
  if (sum == nullptr) {
    return nullptr;
  }
  return std::make_unique<GroupedSumAlias>(sum, average);
}

}  // namespace antb1::exec
