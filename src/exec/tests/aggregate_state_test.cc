#include "antb1/exec/aggregate_state.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <arrow/util/decimal.h>
#include <gtest/gtest.h>

#include "antb1/common/int128.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/types.h"

#include "exec_test_util.h"

namespace antb1::exec {
namespace {

using plan::AggKind;
using plan::LogicalType;
using testing::Bools;
using testing::Int16s;
using testing::Int64s;
using testing::Strings;

constexpr int64_t kI64Max = std::numeric_limits<int64_t>::max();
constexpr int64_t kI64Min = std::numeric_limits<int64_t>::min();

class AggregateStateTest : public testing::ExecTest {};

std::unique_ptr<AggregateState> Make(AggKind kind, std::optional<LogicalType> input,
                                     LogicalType result) {
  auto state = MakeAggregateState(kind, input, result);
  EXPECT_TRUE(state.ok()) << state.status().ToString();
  return state.ok() ? std::move(*state) : nullptr;
}

std::shared_ptr<arrow::Array> Result(const AggregateState& state) {
  auto result = state.Finalize(arrow::default_memory_pool());
  EXPECT_TRUE(result.ok()) << result.status().ToString();
  EXPECT_EQ((*result)->length(), 1);
  return *result;
}

// The canonical text of the single result value ("NULL" for NULL).
std::string Text(const AggregateState& state) {
  const auto array = Result(state);
  if (array->IsNull(0)) {
    return "NULL";
  }
  if (array->type_id() == arrow::Type::DECIMAL128) {
    return arrow::Decimal128(static_cast<const arrow::Decimal128Array&>(*array).GetValue(0))
        .ToIntegerString();
  }
  return array->GetScalar(0).ValueOrDie()->ToString();
}

TEST_F(AggregateStateTest, CountStarCountsSelectedRows) {
  auto state = Make(AggKind::kCountStar, std::nullopt, LogicalType::kBigInt);
  ASSERT_TRUE(state->ConsumeRows(5, nullptr).ok());
  const auto selection = Bools({true, false, std::nullopt, true});
  ASSERT_TRUE(state->ConsumeRows(4, selection.get()).ok());               // NULL counts as false
  ASSERT_TRUE(state->Consume(*Int64s({std::nullopt, 1}), nullptr).ok());  // rows, NULLs too
  EXPECT_EQ(Text(*state), "9");
  EXPECT_TRUE(state->ConsumeRows(3, selection.get()).IsInvalid()) << "selection length";
  EXPECT_EQ(Text(*Make(AggKind::kCountStar, std::nullopt, LogicalType::kBigInt)), "0");
}

TEST_F(AggregateStateTest, CountSkipsNullsAndUnselectedRows) {
  const auto values = Int64s({1, std::nullopt, 3, 4, std::nullopt, 6});
  const auto selection = Bools({true, true, false, std::nullopt, true, true});
  for (const auto& [sel, expected] : std::vector<std::pair<const arrow::BooleanArray*, int>>{
           {nullptr, 4}, {selection.get(), 2}}) {
    auto state = Make(AggKind::kCount, LogicalType::kBigInt, LogicalType::kBigInt);
    ASSERT_TRUE(state->Consume(*values, sel).ok());
    EXPECT_EQ(Text(*state), std::to_string(expected));
  }
  // Without NULLs, and on sliced arrays (non-zero offsets in values and selection).
  auto state = Make(AggKind::kCount, LogicalType::kBigInt, LogicalType::kBigInt);
  const auto dense = Int64s({1, 2, 3, 4, 5, 6, 7, 8, 9, 10});
  const auto wide = Bools({false, true, true, false, true, false, true, true, true, true, false});
  ASSERT_TRUE(state->Consume(*dense, nullptr).ok());
  const auto sliced = std::static_pointer_cast<arrow::BooleanArray>(wide->Slice(1, 9));
  ASSERT_TRUE(state->Consume(*dense->Slice(1), sliced.get()).ok());  // 9 rows, 7 selected
  // NULL, 3, 4, NULL, 6 under true, false, true, false, true: 4 and 6.
  const auto offset_selection = std::static_pointer_cast<arrow::BooleanArray>(wide->Slice(2, 5));
  ASSERT_TRUE(state->Consume(*values->Slice(1), offset_selection.get()).ok());
  EXPECT_EQ(Text(*state), "19");
  EXPECT_EQ(Text(*Make(AggKind::kCount, LogicalType::kBigInt, LogicalType::kBigInt)), "0");
}

TEST_F(AggregateStateTest, IntegerSumIsExactHugeInt) {
  auto state = Make(AggKind::kSum, LogicalType::kBigInt, LogicalType::kHugeInt);
  ASSERT_TRUE(state->Consume(*Int64s({kI64Max, kI64Max, std::nullopt}), nullptr).ok());
  EXPECT_EQ(Text(*state), "18446744073709551614") << "beyond int64: never wraps";
  ASSERT_TRUE(state->Consume(*Int64s({kI64Min, kI64Min, kI64Min, 5}), nullptr).ok());
  EXPECT_EQ(Text(*state), "-9223372036854775805");

  auto small = Make(AggKind::kSum, LogicalType::kSmallInt, LogicalType::kHugeInt);
  ASSERT_TRUE(small->Consume(*Int16s({32767, 32767, -32768, std::nullopt}), nullptr).ok());
  EXPECT_EQ(Text(*small), "32766");

  auto uints = Make(AggKind::kSum, LogicalType::kUSmallInt, LogicalType::kHugeInt);
  ASSERT_TRUE(uints
                  ->Consume(*testing::ArrayOf<arrow::UInt16Builder, uint16_t>(
                                arrow::uint16(), {65535, 65535, std::nullopt}),
                            nullptr)
                  .ok());
  EXPECT_EQ(Text(*uints), "131070");

  auto ints = Make(AggKind::kSum, LogicalType::kInteger, LogicalType::kHugeInt);
  const auto values = testing::ArrayOf<arrow::Int32Builder, int32_t>(
      arrow::int32(), {2147483647, 2147483647, -1, std::nullopt, 10});
  ASSERT_TRUE(ints->Consume(*values, Bools({true, true, false, true, true}).get()).ok());
  EXPECT_EQ(Text(*ints), "4294967304");
}

// Narrow types are added in narrow chunks (int32 for 16-bit values); long runs cross many chunks.
TEST_F(AggregateStateTest, LongRunsOfNarrowValuesSumExactly) {
  constexpr int64_t kN = 100'003;
  arrow::UInt16Builder u16;
  arrow::Int16Builder i16;
  arrow::Int32Builder i32;
  for (int64_t i = 0; i < kN; ++i) {
    ASSERT_TRUE(u16.Append(65535).ok());
    ASSERT_TRUE(i16.Append(-32768).ok());
    ASSERT_TRUE(i32.Append(-2147483648).ok());
  }
  const std::vector<std::pair<LogicalType, std::shared_ptr<arrow::Array>>> inputs = {
      {LogicalType::kUSmallInt, u16.Finish().ValueOrDie()},
      {LogicalType::kSmallInt, i16.Finish().ValueOrDie()},
      {LogicalType::kInteger, i32.Finish().ValueOrDie()},
  };
  const std::vector<std::string> expected = {"6553696605", "-3276898304", "-214754807250944"};
  for (std::size_t k = 0; k < inputs.size(); ++k) {
    auto sum = Make(AggKind::kSum, inputs[k].first, LogicalType::kHugeInt);
    ASSERT_TRUE(sum->Consume(*inputs[k].second, nullptr).ok());
    EXPECT_EQ(Text(*sum), expected[k]) << plan::ToString(inputs[k].first);
  }
}

TEST_F(AggregateStateTest, SumAvgMinMaxOfNoValuesAreNull) {
  const auto nulls = Int64s({std::nullopt, std::nullopt});
  const auto none = Bools({false, false});
  for (const auto& [kind, result] :
       std::vector<std::pair<AggKind, LogicalType>>{{AggKind::kSum, LogicalType::kHugeInt},
                                                    {AggKind::kAvg, LogicalType::kDouble},
                                                    {AggKind::kMin, LogicalType::kBigInt},
                                                    {AggKind::kMax, LogicalType::kBigInt}}) {
    auto state = Make(kind, LogicalType::kBigInt, result);
    EXPECT_EQ(Text(*state), "NULL") << plan::ToString(kind) << " over no batch";
    ASSERT_TRUE(state->Consume(*nulls, nullptr).ok());
    ASSERT_TRUE(state->Consume(*Int64s({1, 2}), none.get()).ok());
    EXPECT_EQ(Text(*state), "NULL") << plan::ToString(kind);
  }
}

TEST_F(AggregateStateTest, AvgDividesTheExactSumOnce) {
  auto state = Make(AggKind::kAvg, LogicalType::kBigInt, LogicalType::kDouble);
  ASSERT_TRUE(state->Consume(*Int64s({kI64Max, kI64Max, 1}), nullptr).ok());
  ASSERT_TRUE(state->Consume(*Int64s({std::nullopt, 7}), Bools({true, false}).get()).ok());
  const auto result = Result(*state);
  const Int128 sum = (static_cast<Int128>(kI64Max) * 2) + 1;
  EXPECT_EQ(static_cast<const arrow::DoubleArray&>(*result).Value(0), ExactDivideToDouble(sum, 3));

  auto small = Make(AggKind::kAvg, LogicalType::kSmallInt, LogicalType::kDouble);
  ASSERT_TRUE(small->Consume(*Int16s({1, 2, std::nullopt}), nullptr).ok());
  EXPECT_EQ(Text(*small), "1.5");
}

TEST_F(AggregateStateTest, DoubleSumAndAvg) {
  const auto values = testing::ArrayOf<arrow::DoubleBuilder, double>(arrow::float64(),
                                                                     {0.5, std::nullopt, 2.25, 4});
  auto sum = Make(AggKind::kSum, LogicalType::kDouble, LogicalType::kDouble);
  auto avg = Make(AggKind::kAvg, LogicalType::kDouble, LogicalType::kDouble);
  ASSERT_TRUE(sum->Consume(*values, nullptr).ok());
  ASSERT_TRUE(avg->Consume(*values, Bools({true, true, true, false}).get()).ok());
  EXPECT_EQ(Text(*sum), "6.75");
  EXPECT_EQ(Text(*avg), "1.375");
}

std::shared_ptr<arrow::Array> HugeInts(const std::vector<std::optional<std::string>>& values) {
  arrow::Decimal128Builder builder(plan::ToArrow(LogicalType::kHugeInt));
  for (const auto& v : values) {
    EXPECT_TRUE(
        (v.has_value() ? builder.Append(arrow::Decimal128(*v)) : builder.AppendNull()).ok());
  }
  return builder.Finish().ValueOrDie();
}

TEST_F(AggregateStateTest, HugeIntSumIsCheckedAgainstTheRange) {
  const std::string big = "99999999999999999999999999999999999999";  // 10^38 - 1
  auto sum = Make(AggKind::kSum, LogicalType::kHugeInt, LogicalType::kHugeInt);
  ASSERT_TRUE(sum->Consume(*HugeInts({big, "-1", std::nullopt}), nullptr).ok());
  EXPECT_EQ(Text(*sum), "99999999999999999999999999999999999998");
  ASSERT_TRUE(sum->Consume(*HugeInts({"2"}), nullptr).ok());
  const auto overflow = sum->Finalize(arrow::default_memory_pool());
  EXPECT_TRUE(overflow.status().IsExecutionError()) << overflow.status().ToString();
  EXPECT_NE(overflow.status().message().find("SUM overflow"), std::string::npos);
  // Beyond 128 bits while accumulating (and while merging).
  auto huge = Make(AggKind::kSum, LogicalType::kHugeInt, LogicalType::kHugeInt);
  EXPECT_TRUE(huge->Consume(*HugeInts({big}), nullptr).ok());
  const auto status = huge->Consume(*HugeInts({"-1", big}), nullptr);  // 2 * 10^38 > 2^127
  EXPECT_TRUE(status.IsExecutionError()) << status.ToString();
  auto other = Make(AggKind::kSum, LogicalType::kHugeInt, LogicalType::kHugeInt);
  ASSERT_TRUE(other->Consume(*HugeInts({big}), nullptr).ok());
  auto again = Make(AggKind::kSum, LogicalType::kHugeInt, LogicalType::kHugeInt);
  ASSERT_TRUE(again->Consume(*HugeInts({big}), nullptr).ok());
  EXPECT_TRUE(again->Merge(*other).IsExecutionError());

  auto avg = Make(AggKind::kAvg, LogicalType::kHugeInt, LogicalType::kDouble);
  ASSERT_TRUE(
      avg->Consume(*HugeInts({"1", "2", std::nullopt}), Bools({true, true, true}).get()).ok());
  EXPECT_EQ(Text(*avg), "1.5");
  auto avg_other = Make(AggKind::kAvg, LogicalType::kHugeInt, LogicalType::kDouble);
  ASSERT_TRUE(avg_other->Consume(*HugeInts({"6"}), nullptr).ok());
  ASSERT_TRUE(avg->Merge(*avg_other).ok());
  EXPECT_EQ(Text(*avg), "3");
}

TEST_F(AggregateStateTest, MinMaxOverSelectedValues) {
  const auto values = Int64s({5, std::nullopt, -3, 9, 1});
  const auto selection = Bools({true, true, false, false, true});
  auto min = Make(AggKind::kMin, LogicalType::kBigInt, LogicalType::kBigInt);
  auto max = Make(AggKind::kMax, LogicalType::kBigInt, LogicalType::kBigInt);
  ASSERT_TRUE(min->Consume(*values, selection.get()).ok());
  ASSERT_TRUE(max->Consume(*values, selection.get()).ok());
  EXPECT_EQ(Text(*min), "1");
  EXPECT_EQ(Text(*max), "5");
  ASSERT_TRUE(min->Consume(*values, nullptr).ok());
  ASSERT_TRUE(max->Consume(*Int64s({std::nullopt}), nullptr).ok());
  EXPECT_EQ(Text(*min), "-3");
  EXPECT_EQ(Text(*max), "5");
}

TEST_F(AggregateStateTest, MinMaxOfVarcharIsByteWise) {
  const auto values = Strings({"b", "", "\xff", "a", std::nullopt, "B"});
  auto min = Make(AggKind::kMin, LogicalType::kVarchar, LogicalType::kVarchar);
  auto max = Make(AggKind::kMax, LogicalType::kVarchar, LogicalType::kVarchar);
  ASSERT_TRUE(min->Consume(*values, nullptr).ok());
  ASSERT_TRUE(max->Consume(*values, nullptr).ok());
  const auto text = [](const AggregateState& s) {
    const auto array = Result(s);
    return std::string(static_cast<const arrow::BinaryArray&>(*array).GetView(0));
  };
  EXPECT_EQ(text(*min), "");
  EXPECT_EQ(text(*max), "\xff") << "bytes compare unsigned: 0xFF sorts after ASCII";
  auto upper = Make(AggKind::kMin, LogicalType::kVarchar, LogicalType::kVarchar);
  ASSERT_TRUE(upper->Consume(*values, Bools({true, false, true, true, true, true}).get()).ok());
  EXPECT_EQ(text(*upper), "B");
}

TEST_F(AggregateStateTest, MinMaxOfDatesUnsignedDoublesAndHugeInts) {
  auto dates = Make(AggKind::kMax, LogicalType::kDate, LogicalType::kDate);
  ASSERT_TRUE(dates
                  ->Consume(*testing::ArrayOf<arrow::Date32Builder, int32_t>(
                                arrow::date32(), {15887, 15917, std::nullopt, -1}),
                            nullptr)
                  .ok());
  EXPECT_EQ(Result(*dates)->type_id(), arrow::Type::DATE32);
  EXPECT_EQ(static_cast<const arrow::Date32Array&>(*Result(*dates)).Value(0), 15917);

  auto u16 = Make(AggKind::kMax, LogicalType::kUSmallInt, LogicalType::kUSmallInt);
  ASSERT_TRUE(u16->Consume(*testing::ArrayOf<arrow::UInt16Builder, uint16_t>(arrow::uint16(),
                                                                             {1, 65535, 32768}),
                           nullptr)
                  .ok());
  EXPECT_EQ(Text(*u16), "65535");

  auto doubles = Make(AggKind::kMin, LogicalType::kDouble, LogicalType::kDouble);
  ASSERT_TRUE(doubles
                  ->Consume(*testing::ArrayOf<arrow::DoubleBuilder, double>(
                                arrow::float64(), {0.5, -9007199254740992.0, std::nullopt}),
                            nullptr)
                  .ok());
  EXPECT_EQ(Text(*doubles), "-9.007199254740992e+15");

  auto huge = Make(AggKind::kMin, LogicalType::kHugeInt, LogicalType::kHugeInt);
  ASSERT_TRUE(
      huge->Consume(*HugeInts({"12", "-99999999999999999999", std::nullopt}), nullptr).ok());
  EXPECT_EQ(Text(*huge), "-99999999999999999999");
}

TEST_F(AggregateStateTest, MergeCombinesPartialStates) {
  const auto first = Int64s({4, std::nullopt, 10});
  const auto second = Int64s({-2, 8});
  const std::vector<std::pair<AggKind, LogicalType>> calls = {
      {AggKind::kCount, LogicalType::kBigInt}, {AggKind::kSum, LogicalType::kHugeInt},
      {AggKind::kAvg, LogicalType::kDouble},   {AggKind::kMin, LogicalType::kBigInt},
      {AggKind::kMax, LogicalType::kBigInt},
  };
  for (const auto& [kind, result] : calls) {
    auto whole = Make(kind, LogicalType::kBigInt, result);
    auto left = Make(kind, LogicalType::kBigInt, result);
    auto right = Make(kind, LogicalType::kBigInt, result);
    auto empty = Make(kind, LogicalType::kBigInt, result);
    ASSERT_TRUE(whole->Consume(*first, nullptr).ok());
    ASSERT_TRUE(whole->Consume(*second, nullptr).ok());
    ASSERT_TRUE(left->Consume(*first, nullptr).ok());
    ASSERT_TRUE(right->Consume(*second, nullptr).ok());
    ASSERT_TRUE(left->Merge(*right).ok());
    ASSERT_TRUE(left->Merge(*empty).ok());
    EXPECT_EQ(Text(*left), Text(*whole)) << plan::ToString(kind);
  }
  auto star = Make(AggKind::kCountStar, std::nullopt, LogicalType::kBigInt);
  auto star_other = Make(AggKind::kCountStar, std::nullopt, LogicalType::kBigInt);
  ASSERT_TRUE(star->ConsumeRows(3, nullptr).ok());
  ASSERT_TRUE(star_other->ConsumeRows(4, nullptr).ok());
  ASSERT_TRUE(star->Merge(*star_other).ok());
  EXPECT_EQ(Text(*star), "7");
  auto doubles = Make(AggKind::kSum, LogicalType::kDouble, LogicalType::kDouble);
  auto doubles_other = Make(AggKind::kSum, LogicalType::kDouble, LogicalType::kDouble);
  ASSERT_TRUE(
      doubles_other
          ->Consume(*testing::ArrayOf<arrow::DoubleBuilder, double>(arrow::float64(), {2}), nullptr)
          .ok());
  ASSERT_TRUE(doubles->Merge(*doubles_other).ok());
  EXPECT_EQ(Text(*doubles), "2");
  // Different aggregates never merge.
  auto min = Make(AggKind::kMin, LogicalType::kBigInt, LogicalType::kBigInt);
  auto max = Make(AggKind::kMax, LogicalType::kBigInt, LogicalType::kBigInt);
  EXPECT_TRUE(min->Merge(*max).IsInvalid());
  EXPECT_TRUE(star->Merge(*min).IsInvalid());
  EXPECT_TRUE(
      Make(AggKind::kCount, LogicalType::kBigInt, LogicalType::kBigInt)->Merge(*star).IsInvalid());
  EXPECT_TRUE(
      Make(AggKind::kSum, LogicalType::kBigInt, LogicalType::kHugeInt)->Merge(*star).IsInvalid());
  EXPECT_TRUE(
      Make(AggKind::kSum, LogicalType::kHugeInt, LogicalType::kHugeInt)->Merge(*star).IsInvalid());
  EXPECT_TRUE(doubles->Merge(*star).IsInvalid());
  auto min_small = Make(AggKind::kMin, LogicalType::kSmallInt, LogicalType::kSmallInt);
  EXPECT_TRUE(min->Merge(*min_small).IsInvalid()) << "another input type";
}

TEST_F(AggregateStateTest, InputsAreChecked) {
  auto sum = Make(AggKind::kSum, LogicalType::kBigInt, LogicalType::kHugeInt);
  EXPECT_TRUE(sum->Consume(*Int16s({1}), nullptr).IsInvalid()) << "another input type";
  EXPECT_TRUE(sum->Consume(*Int64s({1, 2}), Bools({true}).get()).IsInvalid()) << "selection length";
  EXPECT_TRUE(sum->ConsumeRows(1, nullptr).IsInvalid()) << "SUM needs its argument";
  for (const auto kind : {AggKind::kCount, AggKind::kMin, AggKind::kAvg}) {
    const LogicalType result = kind == AggKind::kAvg ? LogicalType::kDouble : LogicalType::kBigInt;
    EXPECT_TRUE(
        Make(kind, LogicalType::kBigInt, result)->Consume(*Strings({"x"}), nullptr).IsInvalid());
  }
  EXPECT_TRUE(Make(AggKind::kSum, LogicalType::kDouble, LogicalType::kDouble)
                  ->Consume(*Int64s({1}), nullptr)
                  .IsInvalid());
  EXPECT_TRUE(Make(AggKind::kSum, LogicalType::kHugeInt, LogicalType::kHugeInt)
                  ->Consume(*Int64s({1}), nullptr)
                  .IsInvalid());
}

TEST_F(AggregateStateTest, OnlyTheBindersCombinationsExist) {
  const auto invalid = [](AggKind kind, std::optional<LogicalType> input, LogicalType result) {
    return MakeAggregateState(kind, input, result).status().IsInvalid();
  };
  EXPECT_TRUE(invalid(AggKind::kCountStar, LogicalType::kBigInt, LogicalType::kBigInt));
  EXPECT_TRUE(invalid(AggKind::kCountStar, std::nullopt, LogicalType::kDouble));
  EXPECT_TRUE(invalid(AggKind::kCount, std::nullopt, LogicalType::kBigInt));
  EXPECT_TRUE(invalid(AggKind::kCount, LogicalType::kVarchar, LogicalType::kInteger));
  EXPECT_TRUE(invalid(AggKind::kSum, LogicalType::kBigInt, LogicalType::kBigInt));
  EXPECT_TRUE(invalid(AggKind::kSum, LogicalType::kDouble, LogicalType::kHugeInt));
  EXPECT_TRUE(invalid(AggKind::kSum, LogicalType::kVarchar, LogicalType::kDouble));
  EXPECT_TRUE(invalid(AggKind::kAvg, LogicalType::kDate, LogicalType::kDouble));
  EXPECT_TRUE(invalid(AggKind::kAvg, LogicalType::kBigInt, LogicalType::kHugeInt));
  EXPECT_TRUE(invalid(AggKind::kMin, LogicalType::kBigInt, LogicalType::kHugeInt));
  EXPECT_TRUE(invalid(AggKind::kMax, std::nullopt, LogicalType::kBigInt));
  EXPECT_FALSE(invalid(AggKind::kCount, LogicalType::kVarchar, LogicalType::kBigInt));
  EXPECT_FALSE(invalid(AggKind::kMax, LogicalType::kVarchar, LogicalType::kVarchar));
}

}  // namespace
}  // namespace antb1::exec
