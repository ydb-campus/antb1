#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <arrow/compute/api_vector.h>
#include <arrow/util/decimal.h>
#include <gtest/gtest.h>

#include "antb1/exec/aggregate_state.h"
#include "antb1/exec/group_aggregate.h"
#include "antb1/exec/grouped_aggregate_state.h"
#include "antb1/exec/operator.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/types.h"

#include "../group_table.h"
#include "exec_test_util.h"

namespace antb1::exec {
namespace {

using plan::AggKind;
using plan::LogicalType;
using testing::Column;
using testing::ScriptedSource;

class GroupedAggregateTest : public testing::ExecTest {};

// splitmix64 with a fixed seed: deterministic test data.
class Rng {
 public:
  explicit Rng(std::uint64_t seed) : state_(seed) {}
  std::uint64_t Next() {
    std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31U);
  }
  std::uint64_t Below(std::uint64_t n) { return Next() % n; }

 private:
  std::uint64_t state_;
};

// A column of `n` values of `type` with NULLs, extremes, repeats and (DOUBLE) NaN, infinities and
// signed zeros.
std::shared_ptr<arrow::Array> RandomColumn(LogicalType type, std::size_t n, Rng& rng) {
  auto builder = arrow::MakeBuilder(plan::ToArrow(type)).ValueOrDie();
  for (std::size_t i = 0; i < n; ++i) {
    if (rng.Below(8) == 0) {
      EXPECT_TRUE(builder->AppendNull().ok());
      continue;
    }
    const std::uint64_t pick = rng.Below(16);
    arrow::Status status;
    switch (type.id()) {
      case LogicalType::kSmallInt: {
        auto v = static_cast<std::int16_t>(static_cast<int>(rng.Below(2001)) - 1000);
        if (pick < 2) {
          v = pick == 0 ? std::numeric_limits<std::int16_t>::min()
                        : std::numeric_limits<std::int16_t>::max();
        }
        status = static_cast<arrow::Int16Builder&>(*builder).Append(v);
        break;
      }
      case LogicalType::kInteger: {
        const auto v = pick == 0 ? std::numeric_limits<std::int32_t>::min()
                                 : static_cast<std::int32_t>(rng.Next());
        status = static_cast<arrow::Int32Builder&>(*builder).Append(v);
        break;
      }
      case LogicalType::kBigInt: {
        auto v = static_cast<std::int64_t>(rng.Next());
        if (pick < 2) {
          v = pick == 0 ? std::numeric_limits<std::int64_t>::min()
                        : std::numeric_limits<std::int64_t>::max();
        }
        status = static_cast<arrow::Int64Builder&>(*builder).Append(v);
        break;
      }
      case LogicalType::kUSmallInt:
        status = static_cast<arrow::UInt16Builder&>(*builder).Append(
            static_cast<std::uint16_t>(rng.Next()));
        break;
      case LogicalType::kHugeInt: {  // |v| < 10^36, so 300 of them never overflow HUGEINT
        const arrow::Decimal128 v(static_cast<std::int64_t>(rng.Next()), rng.Next() >> 8U);
        const arrow::Decimal128 bounded =
            v % arrow::Decimal128("1000000000000000000000000000000000000");
        status = static_cast<arrow::Decimal128Builder&>(*builder).Append(bounded);
        break;
      }
      case LogicalType::kDouble: {
        constexpr auto kSpecial =
            std::to_array<double>({std::numeric_limits<double>::quiet_NaN(), -0.0, 0.0,
                                   std::numeric_limits<double>::infinity(), 1e300});
        const double v =
            pick < 5
                ? kSpecial[pick]
                : static_cast<double>(static_cast<std::int64_t>(rng.Below(20001)) - 10000) / 7.0;
        status = static_cast<arrow::DoubleBuilder&>(*builder).Append(v);
        break;
      }
      case LogicalType::kVarchar: {
        const std::string v =
            pick == 0 ? ""
                      : std::string(1 + rng.Below(3), static_cast<char>('a' + rng.Below(4))) +
                            (pick == 1 ? "\xff" : "");
        status = static_cast<arrow::BinaryBuilder&>(*builder).Append(v);
        break;
      }
      case LogicalType::kDate:
        status = static_cast<arrow::Date32Builder&>(*builder).Append(
            static_cast<std::int32_t>(rng.Below(40000)) - 20000);
        break;
      case LogicalType::kTimestamp:
        status = static_cast<arrow::TimestampBuilder&>(*builder).Append(
            static_cast<std::int64_t>(rng.Next() % 4'000'000'000'000'000U) - 2'000'000'000'000'000);
        break;
      case LogicalType::kBoolean:  // never an aggregate argument
        status = arrow::Status::Invalid("no BOOLEAN input");
        break;
    }
    EXPECT_TRUE(status.ok()) << status.ToString();
  }
  return builder->Finish().ValueOrDie();
}

struct Call {
  AggKind kind;
  std::optional<LogicalType> input;
  LogicalType result;
};

// Every aggregate the binder produces for `type`.
std::vector<Call> CallsOver(LogicalType type) {
  std::vector<Call> calls = {
      {.kind = AggKind::kCountStar, .input = std::nullopt, .result = LogicalType::kBigInt},
      {.kind = AggKind::kCount, .input = type, .result = LogicalType::kBigInt},
      {.kind = AggKind::kMin, .input = type, .result = type},
      {.kind = AggKind::kMax, .input = type, .result = type},
      {.kind = AggKind::kCountDistinct, .input = type, .result = LogicalType::kBigInt}};
  if (plan::IsNumeric(type)) {
    calls.push_back(
        {.kind = AggKind::kSum,
         .input = type,
         .result = plan::IsInteger(type) ? LogicalType::kHugeInt : LogicalType::kDouble});
    calls.push_back({.kind = AggKind::kAvg, .input = type, .result = LogicalType::kDouble});
  }
  if (type == LogicalType::kDate || type == LogicalType::kTimestamp) {
    calls.push_back({.kind = AggKind::kAvg, .input = type, .result = LogicalType::kTimestamp});
  }
  return calls;
}

constexpr auto kTypes = std::to_array<LogicalType>(
    {LogicalType::kSmallInt, LogicalType::kInteger, LogicalType::kBigInt, LogicalType::kUSmallInt,
     LogicalType::kHugeInt, LogicalType::kDouble, LogicalType::kVarchar, LogicalType::kDate,
     LogicalType::kTimestamp});

std::unique_ptr<GroupedAggregateState> MakeGrouped(const Call& call) {
  auto state = MakeGroupedAggregateState(call.kind, call.input, call.result);
  EXPECT_TRUE(state.ok()) << state.status().ToString();
  return state.ok() ? std::move(*state) : nullptr;
}

std::shared_ptr<arrow::Array> Finalized(const GroupedAggregateState& state) {
  auto array = state.Finalize(arrow::default_memory_pool());
  EXPECT_TRUE(array.ok()) << array.status().ToString();
  return array.ok() ? *array : nullptr;
}

arrow::EqualOptions EqualNans() { return arrow::EqualOptions::Defaults().nans_equal(true); }

// Group g of the grouped state equals the scalar state of the same aggregate fed only the rows of
// group g (a selection), for every aggregate and input type, including a group without rows.
TEST_F(GroupedAggregateTest, EveryGroupEqualsTheScalarStateOverItsRows) {
  constexpr std::size_t kRows = 400;
  constexpr std::uint32_t kGroups = 9;  // group 8 gets no row
  Rng rng(20260927);
  for (const LogicalType type : kTypes) {
    const auto values = RandomColumn(type, kRows, rng);
    std::vector<std::uint32_t> ids(kRows);
    for (auto& id : ids) {
      id = static_cast<std::uint32_t>(rng.Below(kGroups - 1));
    }
    for (const Call& call : CallsOver(type)) {
      SCOPED_TRACE(std::string(plan::ToString(call.kind)) + " of " +
                   std::string(plan::ToString(type)));
      auto grouped = MakeGrouped(call);
      ASSERT_NE(grouped, nullptr);
      grouped->Resize(kGroups);
      ASSERT_TRUE(grouped->Consume(call.input ? values.get() : nullptr, ids).ok());
      const auto result = Finalized(*grouped);
      ASSERT_NE(result, nullptr);
      ASSERT_EQ(result->length(), kGroups);
      for (std::uint32_t g = 0; g < kGroups; ++g) {
        std::vector<std::optional<bool>> in_group(kRows);
        for (std::size_t i = 0; i < kRows; ++i) {
          in_group[i] = ids[i] == g;
        }
        auto scalar = MakeAggregateState(call.kind, call.input, call.result).ValueOrDie();
        ASSERT_TRUE(scalar->Consume(*values, testing::Bools(in_group).get()).ok());
        const auto expected = scalar->Finalize(arrow::default_memory_pool()).ValueOrDie();
        EXPECT_TRUE(result->Slice(g, 1)->Equals(*expected, EqualNans()))
            << "group " << g << ": " << result->Slice(g, 1)->ToString() << " vs "
            << expected->ToString();
      }
      // Ranges of groups (empty ones too) are the slices of the whole.
      for (const auto& [begin, end] : std::vector<std::pair<std::uint32_t, std::uint32_t>>{
               {0, 0}, {0, 3}, {3, 8}, {8, kGroups}, {kGroups, kGroups}, {2, 7}}) {
        const auto range = grouped->Finalize(begin, end, arrow::default_memory_pool());
        ASSERT_TRUE(range.ok()) << range.status().ToString();
        EXPECT_TRUE((*range)->Equals(*result->Slice(begin, end - begin), EqualNans()))
            << "groups [" << begin << ", " << end << ")";
      }
      EXPECT_TRUE(grouped->Finalize(3, 2, arrow::default_memory_pool()).status().IsInvalid());
      EXPECT_TRUE(
          grouped->Finalize(0, kGroups + 1, arrow::default_memory_pool()).status().IsInvalid());
    }
  }
}

// COUNT(DISTINCT) per group against a model: a set of the group's normalized non-NULL values
// (-0.0 is 0.0, every NaN one value), for every input type, with values repeating across groups.
TEST_F(GroupedAggregateTest, CountDistinctMatchesASetPerGroup) {
  constexpr std::size_t kRows = 600;
  constexpr std::uint32_t kGroups = 7;  // group 6 gets no row
  Rng rng(42);
  for (const LogicalType type : kTypes) {
    SCOPED_TRACE(std::string(plan::ToString(type)));
    const auto values = RandomColumn(type, kRows, rng);
    std::vector<std::uint32_t> ids(kRows);
    std::vector<std::set<std::string>> model(kGroups);
    for (std::size_t i = 0; i < kRows; ++i) {
      ids[i] = static_cast<std::uint32_t>(rng.Below(kGroups - 1));
      const auto row = static_cast<std::int64_t>(i);
      if (values->IsNull(row)) {
        continue;
      }
      std::string key = values->GetScalar(row).ValueOrDie()->ToString();
      if (type == LogicalType::kDouble) {
        const double v = static_cast<const arrow::DoubleArray&>(*values).Value(row);
        if (std::isnan(v)) {
          key = "nan";
        } else if (v == 0.0) {
          key = "0";  // -0.0 too
        }
      }
      model[ids[i]].insert(key);
    }
    auto state = MakeGrouped(
        {.kind = AggKind::kCountDistinct, .input = type, .result = LogicalType::kBigInt});
    ASSERT_NE(state, nullptr);
    state->Resize(kGroups);
    // In two batches, so that pairs repeat across Consume calls.
    const std::span<const std::uint32_t> all(ids);
    ASSERT_TRUE(state->Consume(values->Slice(0, 250).get(), all.first(250)).ok());
    ASSERT_TRUE(state->Consume(values->Slice(250).get(), all.subspan(250)).ok());
    const auto result = Finalized(*state);
    ASSERT_NE(result, nullptr);
    const auto& counts = static_cast<const arrow::Int64Array&>(*result);
    for (std::uint32_t g = 0; g < kGroups; ++g) {
      EXPECT_EQ(counts.Value(g), static_cast<std::int64_t>(model[g].size())) << "group " << g;
    }
  }
}

// Partial states over parts of the input, each numbering its groups its own way, merged in input
// order: the single-pass result (DOUBLE sums up to rounding: partial sums are added).
TEST_F(GroupedAggregateTest, MergingPartialStatesGivesTheSinglePassResult) {
  constexpr std::size_t kRows = 300;
  constexpr std::uint32_t kGroups = 6;
  Rng rng(7);
  for (const LogicalType type : kTypes) {
    const auto values = RandomColumn(type, kRows, rng);
    std::vector<std::uint32_t> ids(kRows);
    for (auto& id : ids) {
      id = static_cast<std::uint32_t>(rng.Below(kGroups));
    }
    for (const Call& call : CallsOver(type)) {
      SCOPED_TRACE(std::string(plan::ToString(call.kind)) + " of " +
                   std::string(plan::ToString(type)));
      auto single = MakeGrouped(call);
      single->Resize(kGroups);
      ASSERT_TRUE(single->Consume(call.input ? values.get() : nullptr, ids).ok());
      auto merged = MakeGrouped(call);
      merged->Resize(kGroups);
      constexpr auto kCuts = std::to_array<std::size_t>({0, 90, 91, 210, kRows});
      for (std::size_t part = 0; part + 1 < kCuts.size(); ++part) {
        const std::size_t begin = kCuts.at(part);
        const std::size_t length = kCuts.at(part + 1) - begin;
        // Local ids: the global ids reversed, so that merging has to use the map.
        std::vector<std::uint32_t> local(length);
        for (std::size_t i = 0; i < length; ++i) {
          local[i] = kGroups - 1 - ids[begin + i];
        }
        std::vector<std::uint32_t> to_global(kGroups);
        for (std::uint32_t l = 0; l < kGroups; ++l) {
          to_global[l] = kGroups - 1 - l;
        }
        auto partial = MakeGrouped(call);
        partial->Resize(kGroups);
        const auto slice =
            values->Slice(static_cast<std::int64_t>(begin), static_cast<std::int64_t>(length));
        ASSERT_TRUE(partial->Consume(call.input ? slice.get() : nullptr, local).ok());
        ASSERT_TRUE(merged->Merge(*partial, to_global).ok());
      }
      const auto a = Finalized(*single);
      const auto b = Finalized(*merged);
      const bool double_sum = type == LogicalType::kDouble &&
                              (call.kind == AggKind::kSum || call.kind == AggKind::kAvg);
      if (double_sum) {
        EXPECT_TRUE(a->ApproxEquals(*b, EqualNans())) << a->ToString() << " vs " << b->ToString();
      } else {
        EXPECT_TRUE(a->Equals(*b, EqualNans())) << a->ToString() << " vs " << b->ToString();
      }
    }
  }
}

// MergeGroups folds a subset of groups: a state split by a partitioned merge into two (even groups
// into one, odd into the other, merged at the same time from two threads, as the partitions of a
// part merge) gives each group of the single state exactly once.
TEST_F(GroupedAggregateTest, MergingSubsetsOfGroups) {
  constexpr std::size_t kRows = 300;
  constexpr std::uint32_t kGroups = 6;
  Rng rng(11);
  for (const LogicalType type : kTypes) {
    const auto values = RandomColumn(type, kRows, rng);
    std::vector<std::uint32_t> ids(kRows);
    for (auto& id : ids) {
      id = static_cast<std::uint32_t>(rng.Below(kGroups));
    }
    for (const Call& call : CallsOver(type)) {
      SCOPED_TRACE(std::string(plan::ToString(call.kind)) + " of " +
                   std::string(plan::ToString(type)));
      auto part = MakeGrouped(call);
      part->Resize(kGroups);
      ASSERT_TRUE(part->Consume(call.input ? values.get() : nullptr, ids).ok());
      auto even = MakeGrouped(call);
      auto odd = MakeGrouped(call);
      even->Resize(kGroups / 2);
      odd->Resize(kGroups / 2);
      const std::vector<std::uint32_t> evens = {4, 0, 2};  // in any order
      const std::vector<std::uint32_t> odds = {1, 3, 5};
      const std::vector<std::uint32_t> to_even = {2, 0, 1};
      const std::vector<std::uint32_t> to_odd = {0, 1, 2};
      arrow::Status even_status;
      arrow::Status odd_status;
      std::thread even_thread([&] { even_status = even->MergeGroups(*part, evens, to_even); });
      std::thread odd_thread([&] { odd_status = odd->MergeGroups(*part, odds, to_odd); });
      even_thread.join();
      odd_thread.join();
      ASSERT_TRUE(even_status.ok()) << even_status.ToString();
      ASSERT_TRUE(odd_status.ok()) << odd_status.ToString();
      const auto all = Finalized(*part);
      const auto pick = [&](const std::vector<std::int64_t>& rows) {
        arrow::Int64Builder builder;
        EXPECT_TRUE(builder.AppendValues(rows).ok());
        return arrow::compute::Take(all, builder.Finish().ValueOrDie()).ValueOrDie().make_array();
      };
      const auto expected_even = pick({0, 2, 4});
      const auto expected_odd = pick({1, 3, 5});
      EXPECT_TRUE(Finalized(*even)->Equals(*expected_even, EqualNans()))
          << Finalized(*even)->ToString() << " vs " << expected_even->ToString();
      EXPECT_TRUE(Finalized(*odd)->Equals(*expected_odd, EqualNans()))
          << Finalized(*odd)->ToString() << " vs " << expected_odd->ToString();
      // Groups out of range, and lists of different lengths, are Invalid.
      EXPECT_TRUE(
          even->MergeGroups(*part, std::vector<std::uint32_t>{6}, std::vector<std::uint32_t>{0})
              .IsInvalid());
      EXPECT_TRUE(
          even->MergeGroups(*part, std::vector<std::uint32_t>{0}, std::vector<std::uint32_t>{3})
              .IsInvalid());
      EXPECT_TRUE(
          even->MergeGroups(*part, std::vector<std::uint32_t>{0, 1}, std::vector<std::uint32_t>{0})
              .IsInvalid());
    }
  }
}

TEST_F(GroupedAggregateTest, RejectsBadInput) {
  auto count = MakeGrouped(
      {.kind = AggKind::kCount, .input = LogicalType::kBigInt, .result = LogicalType::kBigInt});
  count->Resize(2);
  const auto values = testing::Int64s({1, 2});
  constexpr auto kIds = std::to_array<std::uint32_t>({0, 2});
  EXPECT_TRUE(count->Consume(values.get(), kIds).IsInvalid());  // id 2 outside two groups
  constexpr auto kOne = std::to_array<std::uint32_t>({0});
  EXPECT_TRUE(count->Consume(values.get(), kOne).IsInvalid());                // one id for two rows
  EXPECT_TRUE(count->Consume(testing::Int16s({1}).get(), kOne).IsInvalid());  // wrong type
  auto sum = MakeGrouped(
      {.kind = AggKind::kSum, .input = LogicalType::kBigInt, .result = LogicalType::kHugeInt});
  constexpr auto kMap = std::to_array<std::uint32_t>({0, 0});
  EXPECT_TRUE(count->Merge(*sum, kMap).IsInvalid());  // another aggregate
  EXPECT_FALSE(
      MakeGroupedAggregateState(AggKind::kSum, LogicalType::kVarchar, LogicalType::kDouble).ok());
}

// MIN and MAX of a group whose values are all NaN are NaN, also when the NaN-only group comes
// through Merge; any value beats NaN (divergence D10).
TEST_F(GroupedAggregateTest, NaNOnlyGroupsAreNaNAlsoThroughMerge) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  arrow::DoubleBuilder builder;
  ASSERT_TRUE(builder.AppendValues({nan, nan, 5.0, nan}).ok());
  const auto values = builder.Finish().ValueOrDie();
  constexpr auto kIds = std::to_array<std::uint32_t>({0, 0, 1, 1});
  for (const AggKind kind : {AggKind::kMin, AggKind::kMax}) {
    const Call call{.kind = kind, .input = LogicalType::kDouble, .result = LogicalType::kDouble};
    auto state = MakeGrouped(call);
    state->Resize(3);  // group 2 gets nothing
    ASSERT_TRUE(state->Consume(values.get(), kIds).ok());
    const auto result = Finalized(*state);
    const auto& doubles = static_cast<const arrow::DoubleArray&>(*result);
    EXPECT_TRUE(std::isnan(doubles.Value(0)));
    EXPECT_EQ(doubles.Value(1), 5.0);
    EXPECT_TRUE(doubles.IsNull(2));
    // Merged into a state where the target group has nothing (NaN) and one that has a value.
    auto target = MakeGrouped(call);
    target->Resize(2);
    arrow::DoubleBuilder seven;
    ASSERT_TRUE(seven.Append(7.0).ok());
    constexpr auto kSecond = std::to_array<std::uint32_t>({1});
    ASSERT_TRUE(target->Consume(seven.Finish().ValueOrDie().get(), kSecond).ok());
    constexpr auto kMap = std::to_array<std::uint32_t>({0, 1, 1});  // 0 -> 0, 1 and 2 -> 1
    ASSERT_TRUE(target->Merge(*state, kMap).ok());
    const auto merged = Finalized(*target);
    const auto& m = static_cast<const arrow::DoubleArray&>(*merged);
    EXPECT_TRUE(std::isnan(m.Value(0)));
    EXPECT_EQ(m.Value(1), kind == AggKind::kMin ? 5.0 : 7.0);
  }
}

// Calls the binder never produces, and inputs that do not match the state, are Invalid.
TEST_F(GroupedAggregateTest, InvalidCallsAndInputs) {
  const auto invalid = [](AggKind kind, std::optional<LogicalType> input, LogicalType result) {
    return MakeGroupedAggregateState(kind, input, result).status().IsInvalid();
  };
  EXPECT_TRUE(invalid(AggKind::kCountStar, LogicalType::kBigInt, LogicalType::kBigInt));
  EXPECT_TRUE(invalid(AggKind::kCountStar, std::nullopt, LogicalType::kDouble));
  EXPECT_TRUE(invalid(AggKind::kCount, LogicalType::kBigInt, LogicalType::kDouble));
  EXPECT_TRUE(invalid(AggKind::kSum, std::nullopt, LogicalType::kHugeInt));
  EXPECT_TRUE(invalid(AggKind::kSum, LogicalType::kDate, LogicalType::kDouble));
  EXPECT_TRUE(invalid(AggKind::kSum, LogicalType::kBigInt, LogicalType::kDouble));
  EXPECT_TRUE(invalid(AggKind::kMin, LogicalType::kBigInt, LogicalType::kHugeInt));
  constexpr auto kIds = std::to_array<std::uint32_t>({0});
  for (const LogicalType type : kTypes) {
    for (const Call& call : CallsOver(type)) {
      auto state = MakeGrouped(call);
      state->Resize(1);
      // COUNT(*) takes no argument column; every other call needs one.
      Rng rng(1);
      const auto one = RandomColumn(type, 1, rng);
      const arrow::Array* wrong = call.input.has_value() ? nullptr : one.get();
      EXPECT_TRUE(state->Consume(wrong, kIds).IsInvalid()) << plan::ToString(call.kind);
      // A group map with the wrong number of entries.
      auto other = MakeGrouped(call);
      other->Resize(2);
      EXPECT_TRUE(state->Merge(*other, kIds).IsInvalid());
    }
  }
  auto min = MakeGrouped(
      {.kind = AggKind::kMin, .input = LogicalType::kBigInt, .result = LogicalType::kBigInt});
  auto max = MakeGrouped(
      {.kind = AggKind::kMax, .input = LogicalType::kBigInt, .result = LogicalType::kBigInt});
  min->Resize(1);
  max->Resize(1);
  EXPECT_TRUE(min->Merge(*max, kIds).IsInvalid());
}

// As the scalar state: a sum outside HUGEINT's range fails at Finalize; beyond 128 bits it fails
// while accumulating and while merging.
TEST_F(GroupedAggregateTest, HugeIntSumIsCheckedAgainstTheRange) {
  const auto huge_ints = [](const std::vector<std::string>& values) {
    arrow::Decimal128Builder builder(plan::ToArrow(LogicalType::kHugeInt));
    for (const auto& v : values) {
      EXPECT_TRUE(builder.Append(arrow::Decimal128(v)).ok());
    }
    return builder.Finish().ValueOrDie();
  };
  const std::string big = "99999999999999999999999999999999999999";  // 10^38 - 1
  const Call call{
      .kind = AggKind::kSum, .input = LogicalType::kHugeInt, .result = LogicalType::kHugeInt};
  constexpr auto kBoth = std::to_array<std::uint32_t>({0, 0});
  constexpr auto kFirst = std::to_array<std::uint32_t>({0});
  auto sum = MakeGrouped(call);
  sum->Resize(1);
  ASSERT_TRUE(
      sum->Consume(huge_ints({big, "2"}).get(), kBoth).ok());  // 10^38 + 1: fits in 128 bits
  const auto result = sum->Finalize(arrow::default_memory_pool());
  EXPECT_TRUE(result.status().IsExecutionError()) << result.status().ToString();
  auto wide = MakeGrouped(call);
  wide->Resize(1);
  EXPECT_TRUE(wide->Consume(huge_ints({big, big}).get(), kBoth).IsExecutionError());
  auto a = MakeGrouped(call);
  auto b = MakeGrouped(call);
  a->Resize(1);
  b->Resize(1);
  ASSERT_TRUE(a->Consume(huge_ints({big}).get(), kFirst).ok());
  ASSERT_TRUE(b->Consume(huge_ints({big}).get(), kFirst).ok());
  EXPECT_TRUE(a->Merge(*b, kFirst).IsExecutionError());
}

// ---- GroupAggregateOperator ----

std::shared_ptr<arrow::RecordBatch> Rows(const std::vector<std::optional<std::int64_t>>& keys,
                                         const std::vector<std::optional<std::int64_t>>& values) {
  return arrow::RecordBatch::Make(
      arrow::schema({arrow::field("k", arrow::int64()), arrow::field("v", arrow::int64())}),
      static_cast<std::int64_t>(keys.size()), {testing::Int64s(keys), testing::Int64s(values)});
}

// The operator's output as sorted "key|count|sum" lines (group order is unspecified).
std::vector<std::string> RunGroups(std::vector<Batch> batches,
                                   const std::shared_ptr<arrow::Schema>& schema) {
  auto source = std::make_unique<ScriptedSource>(schema, std::move(batches));
  GroupAggregateOperator op(
      std::move(source), {Column(0, "k", LogicalType::kBigInt)},
      {plan::AggregateCall{.kind = AggKind::kCountStar, .type = LogicalType::kBigInt},
       plan::AggregateCall{.kind = AggKind::kSum,
                           .arg = Column(1, "v", LogicalType::kBigInt),
                           .type = LogicalType::kHugeInt}});
  ExecContext ctx;
  auto table = Drain(op, ctx);
  EXPECT_TRUE(table.ok()) << table.status().ToString();
  std::vector<std::string> lines;
  if (!table.ok()) {
    return lines;
  }
  const auto combined = (*table)->CombineChunks().ValueOrDie();
  for (std::int64_t r = 0; r < combined->num_rows(); ++r) {
    std::string line;
    for (int c = 0; c < combined->num_columns(); ++c) {
      const auto scalar = combined->column(c)->GetScalar(r).ValueOrDie();
      line += (c > 0 ? "|" : "") + (scalar->is_valid ? scalar->ToString() : std::string("NULL"));
    }
    lines.push_back(line);
  }
  std::ranges::sort(lines);
  return lines;
}

TEST_F(GroupedAggregateTest, GroupsAcrossBatchesWithSelectionsAndNullKeys) {
  const auto a = Rows({1, 2, std::nullopt, 1}, {10, 20, 30, 40});
  const auto b = Rows({2, std::nullopt, 3, 1}, {5, std::nullopt, 7, 1});
  const std::vector<Batch> batches = {
      Batch{.data = a, .selection = {}},
      Batch{.data = b, .selection = testing::Bools({true, true, false, std::nullopt})}};
  EXPECT_EQ(RunGroups(batches, a->schema()),
            (std::vector<std::string>{"1|2|50", "2|2|25", "NULL|2|30"}));
}

// The chunks of new groups (one per input batch that made some) come out joined, up to
// kMaxMergeChunk groups and kMaxJoinedKeyBytes of keys, so that VARCHAR keys never grow near the
// 2 GiB of one array (binary offsets are 32 bits); the aggregates line up with them.
TEST_F(GroupedAggregateTest, EmitsOneBatchPerChunkOfNewGroups) {
  const auto a = Rows({1, 2, 1}, {10, 20, 30});
  const auto b = Rows({2, 2}, {1, 1});
  const auto c = Rows({3, 1, 4}, {7, 1, 8});
  auto source = std::make_unique<ScriptedSource>(
      a->schema(), std::vector<Batch>{Batch{.data = a}, Batch{.data = b}, Batch{.data = c}});
  GroupAggregateOperator op(
      std::move(source), {Column(0, "k", LogicalType::kBigInt)},
      {plan::AggregateCall{.kind = AggKind::kCountStar, .type = LogicalType::kBigInt}});
  ExecContext ctx;
  for (int run = 0; run < 2; ++run) {  // a second run after Open again gives the same batches
    const auto table = Drain(op, ctx);
    ASSERT_TRUE(table.ok()) << table.status().ToString();
    ASSERT_EQ((*table)->column(0)->num_chunks(), 1) << "two small chunks joined";
    EXPECT_EQ((*table)->column(0)->chunk(0)->length(), 4);
    std::vector<std::string> lines;
    lines.reserve(static_cast<std::size_t>((*table)->num_rows()));
    for (std::int64_t r = 0; r < (*table)->num_rows(); ++r) {
      lines.push_back(
          testing::Int64Column(**table, 0)[static_cast<std::size_t>(r)]
              .transform([](std::int64_t v) { return std::to_string(v); })
              .value_or("NULL") +
          "|" +
          std::to_string(
              testing::Int64Column(**table, 1)[static_cast<std::size_t>(r)].value_or(-1)));
    }
    std::ranges::sort(lines);
    EXPECT_EQ(lines, (std::vector<std::string>{"1|3", "2|3", "3|1", "4|1"}));
  }
}

// Chunks whose keys are large are not joined past kMaxJoinedKeyBytes.
TEST_F(GroupedAggregateTest, LargeKeyChunksStaySeparate) {
  const auto schema = arrow::schema({arrow::field("s", arrow::binary())});
  const std::string big(static_cast<std::size_t>(GroupTable::kMaxJoinedKeyBytes / 2) + 1, 'x');
  std::vector<Batch> batches;
  for (const char tail : {'a', 'b', 'c'}) {
    batches.push_back(
        Batch{.data = arrow::RecordBatch::Make(schema, 1, {testing::Strings({big + tail})})});
  }
  GroupAggregateOperator op(
      std::make_unique<ScriptedSource>(schema, std::move(batches)),
      {Column(0, "s", LogicalType::kVarchar)},
      {plan::AggregateCall{.kind = AggKind::kCountStar, .type = LogicalType::kBigInt}});
  ExecContext ctx;
  const auto table = Drain(op, ctx);
  ASSERT_TRUE(table.ok()) << table.status().ToString();
  EXPECT_EQ((*table)->num_rows(), 3);
  EXPECT_EQ((*table)->column(0)->num_chunks(), 3) << "each chunk over half of the byte bound";
}

// Without keys (GROUP BY constants only) every row is in one group, emitted only over rows.
TEST_F(GroupedAggregateTest, NoKeysMeanOneGroupOfEveryRow) {
  const auto a = Rows({1, 2}, {10, 20});
  const auto b = Rows({3}, {std::nullopt});
  const auto run = [&](std::vector<Batch> batches) {
    GroupAggregateOperator op(
        std::make_unique<ScriptedSource>(a->schema(), std::move(batches)), {},
        {plan::AggregateCall{.kind = AggKind::kCountStar, .type = LogicalType::kBigInt},
         plan::AggregateCall{.kind = AggKind::kCount,
                             .arg = Column(1, "v", LogicalType::kBigInt),
                             .type = LogicalType::kBigInt}});
    ExecContext ctx;
    return Drain(op, ctx);
  };
  const auto some = run({Batch{.data = a}, Batch{.data = b}});
  ASSERT_TRUE(some.ok()) << some.status().ToString();
  ASSERT_EQ((*some)->num_rows(), 1);
  EXPECT_EQ(testing::Int64Column(**some, 0), (std::vector<std::optional<std::int64_t>>{3}));
  EXPECT_EQ(testing::Int64Column(**some, 1), (std::vector<std::optional<std::int64_t>>{2}));
  const auto none = run({Batch{.data = a, .selection = testing::Bools({false, false})}});
  ASSERT_TRUE(none.ok()) << none.status().ToString();
  EXPECT_EQ((*none)->num_rows(), 0);
}

TEST_F(GroupedAggregateTest, NoRowsGiveNoGroups) {
  const auto a = Rows({1, 2}, {1, 2});
  EXPECT_TRUE(RunGroups({}, a->schema()).empty());
  EXPECT_TRUE(
      RunGroups({Batch{.data = a, .selection = testing::Bools({false, false})}}, a->schema())
          .empty());
}

// -0.0 groups with 0.0 and every NaN with every other NaN; the key printed is the first seen.
TEST_F(GroupedAggregateTest, DoubleKeysGroupSignedZerosAndNaNs) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  arrow::DoubleBuilder keys;
  for (const double k : {-0.0, 0.0, nan, -nan, 1.5, 0.0}) {
    ASSERT_TRUE(keys.Append(k).ok());
  }
  const auto key_array = keys.Finish().ValueOrDie();
  const auto schema = arrow::schema({arrow::field("k", arrow::float64())});
  auto batch = arrow::RecordBatch::Make(schema, 6, {key_array});
  auto source = std::make_unique<ScriptedSource>(
      schema, std::vector<Batch>{Batch{.data = batch, .selection = {}}});
  GroupAggregateOperator op(
      std::move(source), {Column(0, "k", LogicalType::kDouble)},
      {plan::AggregateCall{.kind = AggKind::kCountStar, .type = LogicalType::kBigInt}});
  ExecContext ctx;
  const auto table = Drain(op, ctx).ValueOrDie()->CombineChunks().ValueOrDie();
  ASSERT_EQ(table->num_rows(), 3);
  const auto& k = static_cast<const arrow::DoubleArray&>(*table->column(0)->chunk(0));
  const auto& n = static_cast<const arrow::Int64Array&>(*table->column(1)->chunk(0));
  for (std::int64_t r = 0; r < 3; ++r) {
    if (std::isnan(k.Value(r))) {
      EXPECT_EQ(n.Value(r), 2);
    } else if (k.Value(r) == 0.0) {
      EXPECT_EQ(n.Value(r), 3);
      EXPECT_TRUE(std::signbit(k.Value(r))) << "the first zero seen is -0.0";
    } else {
      EXPECT_EQ(k.Value(r), 1.5);
      EXPECT_EQ(n.Value(r), 1);
    }
  }
}

// Every key type groups, several keys together; the result does not depend on the batch size.
TEST_F(GroupedAggregateTest, EveryKeyTypeAndBatchSizeInvariance) {
  constexpr std::size_t kRows = 500;
  Rng rng(3);
  arrow::FieldVector fields;
  arrow::ArrayVector columns;
  for (const LogicalType type : kTypes) {
    fields.push_back(arrow::field(std::string(plan::ToString(type)), plan::ToArrow(type)));
    // Few distinct values per column: keep only rng.Below(3)-derived repeats.
    auto column = RandomColumn(type, kRows, rng);
    std::vector<std::int64_t> small(kRows);
    for (auto& i : small) {
      i = static_cast<std::int64_t>(rng.Below(4));
    }
    arrow::Int64Builder indices;
    ASSERT_TRUE(indices.AppendValues(small).ok());
    columns.push_back(
        arrow::compute::Take(column, indices.Finish().ValueOrDie()).ValueOrDie().make_array());
  }
  const auto schema = arrow::schema(fields);
  const auto batch = arrow::RecordBatch::Make(schema, kRows, columns);
  std::vector<plan::BoundColumn> keys;
  for (std::size_t k = 0; k < kTypes.size(); k += 3) {  // three keys of other types
    keys.push_back(Column(static_cast<int>(k), fields.at(k)->name(), kTypes.at(k)));
  }
  std::vector<std::string> first;
  for (const std::int64_t batch_size : {1, 7, 64, 500}) {
    std::vector<Batch> batches;
    for (std::int64_t start = 0; std::cmp_less(start, kRows); start += batch_size) {
      batches.push_back(Batch{.data = batch->Slice(start, batch_size), .selection = {}});
    }
    GroupAggregateOperator op(
        std::make_unique<ScriptedSource>(schema, std::move(batches)), keys,
        {plan::AggregateCall{.kind = AggKind::kCountStar, .type = LogicalType::kBigInt},
         plan::AggregateCall{.kind = AggKind::kMax,
                             .arg = Column(6, "VARCHAR", LogicalType::kVarchar),
                             .type = LogicalType::kVarchar}});
    ExecContext ctx;
    const auto table = Drain(op, ctx).ValueOrDie()->CombineChunks().ValueOrDie();
    std::vector<std::string> lines;
    std::int64_t total = 0;
    for (std::int64_t r = 0; r < table->num_rows(); ++r) {
      std::string line;
      for (int c = 0; c < table->num_columns(); ++c) {
        line += table->column(c)->GetScalar(r).ValueOrDie()->ToString() + "|";
      }
      total += static_cast<const arrow::Int64Array&>(*table->column(3)->chunk(0)).Value(r);
      lines.push_back(line);
    }
    std::ranges::sort(lines);
    EXPECT_EQ(total, static_cast<std::int64_t>(kRows));
    EXPECT_EQ(std::ranges::adjacent_find(lines), lines.end()) << "a key combination twice";
    if (first.empty()) {
      first = lines;
    } else {
      EXPECT_EQ(lines, first) << "batch size " << batch_size;
    }
  }
}

// A malformed operator (a column outside its input, Next() before Open()) is Invalid.
TEST_F(GroupedAggregateTest, OperatorRejectsMalformedPlans) {
  const auto a = Rows({1}, {1});
  const auto run = [&](std::vector<plan::BoundColumn> keys,
                       std::vector<plan::AggregateCall> calls) {
    GroupAggregateOperator op(std::make_unique<ScriptedSource>(a->schema(), std::vector<Batch>{}),
                              std::move(keys), std::move(calls));
    ExecContext ctx;
    return op.Open(ctx);
  };
  const plan::AggregateCall count_star{.kind = AggKind::kCountStar, .type = LogicalType::kBigInt};
  EXPECT_TRUE(run({}, {count_star}).ok()) << "no keys: one group of every row";
  EXPECT_TRUE(run({Column(5, "k", LogicalType::kBigInt)}, {count_star}).IsInvalid());
  EXPECT_TRUE(run({Column(0, "k", LogicalType::kBigInt)},
                  {plan::AggregateCall{.kind = AggKind::kSum,
                                       .arg = Column(9, "v", LogicalType::kBigInt),
                                       .type = LogicalType::kHugeInt}})
                  .IsInvalid());
  GroupAggregateOperator unopened(
      std::make_unique<ScriptedSource>(a->schema(), std::vector<Batch>{}),
      {Column(0, "k", LogicalType::kBigInt)}, {count_star});
  EXPECT_TRUE(unopened.Next().status().IsInvalid());
}

}  // namespace
}  // namespace antb1::exec
