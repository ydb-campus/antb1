// SUM and AVG over one column sharing an accumulator (src/exec/grouped_sum_alias.h).
//
// The claim under test is that sharing changes no value, so each case has a control that cannot
// share anything: a table holding one of the two calls alone. Two things decide whether a case
// exercises the sharing at all, and both are easy to get wrong:
//
//   * the operand type has to be one the Int128 accumulator serves -- DECIMAL or HUGEINT
//     (grouped_aggregate_state.cc, SumState). An integer column uses another accumulator and is
//     not aliased today, so a case built on BIGINT would pass while testing nothing;
//   * a case that separates "the same column" from "the same type" needs two columns of ONE type,
//     which is why `v` and `u` below are both DECIMAL(15, 2).
//
// The merge case matters most: the alias folds nothing on a merge because the state it points at
// does, and an alias that folded too would count every part's rows twice.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <arrow/api.h>
#include <gtest/gtest.h>

#include "antb1/exec/grouped_aggregate_state.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/types.h"

#include "../group_table.h"
#include "exec_test_util.h"

namespace antb1::exec {
namespace {

using plan::LogicalType;

class GroupedSumAliasTest : public testing::ExecTest {};

// Functions rather than file-scope constants: a LogicalType is not a literal type, so a global
// would construct at static-initialization time.
LogicalType Money() { return LogicalType::Decimal(15, 2); }
LogicalType SumOfMoney() { return LogicalType::Decimal(LogicalType::kMaxDecimalWidth, 2); }

plan::BoundColumn Column(int index, std::string name, LogicalType type) {
  return plan::BoundColumn{.index = index, .name = std::move(name), .type = type};
}

plan::AggregateCall Call(plan::AggKind kind, int column, LogicalType input, LogicalType result) {
  return plan::AggregateCall{.kind = kind, .arg = Column(column, "v", input), .type = result};
}

plan::AggregateCall Sum(int column) {
  return Call(plan::AggKind::kSum, column, Money(), SumOfMoney());
}
plan::AggregateCall Avg(int column) {
  return Call(plan::AggKind::kAvg, column, Money(), LogicalType::kDouble);
}

std::shared_ptr<arrow::Array> Decimals(const std::vector<std::optional<std::string>>& unscaled) {
  arrow::Decimal128Builder builder(plan::ToArrow(Money()));
  for (const auto& v : unscaled) {
    EXPECT_TRUE(
        (v.has_value() ? builder.Append(arrow::Decimal128(*v)) : builder.AppendNull()).ok());
  }
  std::shared_ptr<arrow::Array> out;
  EXPECT_TRUE(builder.Finish(&out).ok());
  return out;
}

// Three groups: one with a NULL among its values, and one whose every value is NULL, so a shared
// accumulator has to report NULL for it exactly as SUM and AVG each would on their own.
std::shared_ptr<arrow::RecordBatch> Rows() {
  const auto keys = testing::Int64s({1, 1, 2, 2, 3, 3, 1});
  const auto v = Decimals({"1000", "2500", "700", std::nullopt, std::nullopt, std::nullopt, "5"});
  const auto u = Decimals({"33", "44", std::nullopt, "55", "66", "77", "88"});
  const auto w = testing::Int64s({5, 6, 7, 8, 9, 10, 11});
  return arrow::RecordBatch::Make(
      arrow::schema({arrow::field("k", arrow::int64()), arrow::field("v", plan::ToArrow(Money())),
                     arrow::field("u", plan::ToArrow(Money())), arrow::field("w", arrow::int64())}),
      7, {keys, v, u, w});
}

// Every row of `batch` through a table over `calls`, drained. With `parts` above one the rows go
// through that many part tables and are merged, which is the path an alias must stay out of.
arrow::Result<std::shared_ptr<arrow::Table>> Through(const std::vector<plan::AggregateCall>& calls,
                                                     const arrow::RecordBatch& batch,
                                                     int parts = 1) {
  const auto key = Column(0, "k", LogicalType::kBigInt);
  arrow::FieldVector fields{arrow::field("key0", arrow::int64())};
  for (std::size_t i = 0; i < calls.size(); ++i) {
    fields.push_back(arrow::field("agg" + std::to_string(i), plan::ToArrow(calls[i].type)));
  }
  const auto schema = arrow::schema(fields);

  ARROW_ASSIGN_OR_RAISE(auto target, GroupTable::Make({key}, calls, batch.num_columns(),
                                                      arrow::default_memory_pool(), nullptr));
  if (parts == 1) {
    ARROW_RETURN_NOT_OK(target->Consume(batch));
  } else {
    const int64_t each = batch.num_rows() / parts;
    for (int p = 0; p < parts; ++p) {
      const int64_t from = p * each;
      const int64_t rows = p + 1 == parts ? batch.num_rows() - from : each;
      ARROW_ASSIGN_OR_RAISE(auto part, GroupTable::Make({key}, calls, batch.num_columns(),
                                                        arrow::default_memory_pool(), nullptr));
      ARROW_RETURN_NOT_OK(part->Consume(*batch.Slice(from, rows)));
      ARROW_RETURN_NOT_OK(target->Merge(*part));
    }
  }
  arrow::RecordBatchVector out;
  while (true) {
    ARROW_ASSIGN_OR_RAISE(auto chunk, target->NextChunk(schema));
    if (chunk == nullptr) {
      break;
    }
    out.push_back(std::move(chunk));
  }
  return arrow::Table::FromRecordBatches(schema, out);
}

// The shape a SUM-heavy grouped query has: both over one column, so the second shares the first's
// sums and counts. Against one table per call, which can share nothing.
TEST_F(GroupedSumAliasTest, SumAndAvgOfOneColumnAnswerAsSeparateTablesDo) {
  const auto rows = Rows();
  auto shared = Through({Sum(1), Avg(1)}, *rows);
  auto sum_only = Through({Sum(1)}, *rows);
  auto avg_only = Through({Avg(1)}, *rows);
  ASSERT_TRUE(shared.ok()) << shared.status().ToString();
  ASSERT_TRUE(sum_only.ok()) << sum_only.status().ToString();
  ASSERT_TRUE(avg_only.ok()) << avg_only.status().ToString();

  EXPECT_TRUE((*shared)->column(1)->Equals(*(*sum_only)->column(1)))
      << (*shared)->ToString() << "\nagainst\n"
      << (*sum_only)->ToString();
  EXPECT_TRUE((*shared)->column(2)->Equals(*(*avg_only)->column(1)))
      << (*shared)->ToString() << "\nagainst\n"
      << (*avg_only)->ToString();
}

// The merge path, where an alias that folded anything would double every part's rows.
TEST_F(GroupedSumAliasTest, MergingTablesThatShareAnAccumulatorGivesTheSinglePassResult) {
  const auto rows = Rows();
  auto single = Through({Sum(1), Avg(1)}, *rows, /*parts=*/1);
  auto merged = Through({Sum(1), Avg(1)}, *rows, /*parts=*/3);
  ASSERT_TRUE(single.ok()) << single.status().ToString();
  ASSERT_TRUE(merged.ok()) << merged.status().ToString();
  EXPECT_TRUE((*merged)->Equals(**single)) << (*merged)->ToString() << "\nagainst\n"
                                           << (*single)->ToString();
}

// AVG first, so it owns the accumulator and the SUM reads it: the alias finalizes either way.
TEST_F(GroupedSumAliasTest, AvgBeforeSumSharesTheSameAccumulator) {
  const auto rows = Rows();
  auto shared = Through({Avg(1), Sum(1)}, *rows);
  auto avg_only = Through({Avg(1)}, *rows);
  auto sum_only = Through({Sum(1)}, *rows);
  ASSERT_TRUE(shared.ok()) << shared.status().ToString();
  ASSERT_TRUE(avg_only.ok()) << avg_only.status().ToString();
  ASSERT_TRUE(sum_only.ok()) << sum_only.status().ToString();
  EXPECT_TRUE((*shared)->column(1)->Equals(*(*avg_only)->column(1)));
  EXPECT_TRUE((*shared)->column(2)->Equals(*(*sum_only)->column(1)));
}

// Two columns of ONE type share nothing. Aliasing on the wrong column would sum the wrong values
// rather than merely waste work, and only comparing the column catches it: both of these are
// DECIMAL(15, 2), so comparing types alone would admit the alias.
TEST_F(GroupedSumAliasTest, SumsOfTwoColumnsOfOneTypeShareNothing) {
  const auto rows = Rows();
  auto both = Through({Sum(1), Sum(2)}, *rows);
  auto first = Through({Sum(1)}, *rows);
  auto second = Through({Sum(2)}, *rows);
  ASSERT_TRUE(both.ok()) << both.status().ToString();
  ASSERT_TRUE(first.ok()) << first.status().ToString();
  ASSERT_TRUE(second.ok()) << second.status().ToString();
  EXPECT_TRUE((*both)->column(1)->Equals(*(*first)->column(1)));
  EXPECT_TRUE((*both)->column(2)->Equals(*(*second)->column(1)))
      << "the second SUM must be of its own column\n"
      << (*both)->ToString();
}

// AVG of one column twice: the second is the same call, so it shares, and both must still answer.
TEST_F(GroupedSumAliasTest, TheSameAverageTwiceShares) {
  const auto rows = Rows();
  auto both = Through({Avg(1), Avg(1)}, *rows);
  auto one = Through({Avg(1)}, *rows);
  ASSERT_TRUE(both.ok()) << both.status().ToString();
  ASSERT_TRUE(one.ok()) << one.status().ToString();
  EXPECT_TRUE((*both)->column(1)->Equals(*(*one)->column(1)));
  EXPECT_TRUE((*both)->column(2)->Equals(*(*one)->column(1)));
}

// The one case that fails if the sharing stops happening at all. Every other case here compares
// answers, and answers are the same either way -- that is the point of the change -- so none of
// them can see the alias. An alias charges no memory and a real accumulator charges its two
// per-group vectors, so memory_usage() tells them apart without a new interface.
TEST_F(GroupedSumAliasTest, OnlyOneAccumulatorIsBuiltForASumAndAnAvgOfOneColumn) {
  const auto rows = Rows();
  const auto key = Column(0, "k", LogicalType::kBigInt);
  const auto built = [&](const std::vector<plan::AggregateCall>& calls) {
    auto table =
        GroupTable::Make({key}, calls, rows->num_columns(), arrow::default_memory_pool(), nullptr);
    EXPECT_TRUE(table.ok()) << table.status().ToString();
    EXPECT_TRUE((*table)->Consume(*rows).ok());  // sizes the per-group vectors
    return std::move(*table);
  };

  const auto shared = built({Sum(1), Avg(1)});
  EXPECT_GT(shared->state(0).memory_usage(), 0) << "the first call owns the accumulator";
  EXPECT_EQ(shared->state(1).memory_usage(), 0)
      << "the second call must read the first's accumulator, not build its own";

  // Two columns: nothing to share, so both calls own one.
  const auto separate = built({Sum(1), Sum(2)});
  EXPECT_GT(separate->state(0).memory_usage(), 0);
  EXPECT_GT(separate->state(1).memory_usage(), 0);

  // An integer column uses another accumulator, which this change does not alias. Pinned here so
  // that widening the scope later has to update a test rather than silently change behaviour.
  const auto integers =
      built({Call(plan::AggKind::kSum, 3, LogicalType::kBigInt, LogicalType::kHugeInt),
             Call(plan::AggKind::kAvg, 3, LogicalType::kBigInt, LogicalType::kDouble)});
  EXPECT_GT(integers->state(0).memory_usage(), 0);
  EXPECT_GT(integers->state(1).memory_usage(), 0) << "an integer SUM and AVG do not share yet";
}

// An integer column is served by another accumulator, which this change does not alias: the point
// of the case is that the answers are right either way, so the scope is pinned rather than assumed
// and widening it later has a test to keep.
TEST_F(GroupedSumAliasTest, AnIntegerColumnAnswersThoughItSharesNothingYet) {
  const auto rows = Rows();
  const auto sum_w = Call(plan::AggKind::kSum, 3, LogicalType::kBigInt, LogicalType::kHugeInt);
  const auto avg_w = Call(plan::AggKind::kAvg, 3, LogicalType::kBigInt, LogicalType::kDouble);
  auto both = Through({sum_w, avg_w}, *rows);
  auto sum_only = Through({sum_w}, *rows);
  auto avg_only = Through({avg_w}, *rows);
  ASSERT_TRUE(both.ok()) << both.status().ToString();
  ASSERT_TRUE(sum_only.ok()) << sum_only.status().ToString();
  ASSERT_TRUE(avg_only.ok()) << avg_only.status().ToString();
  EXPECT_TRUE((*both)->column(1)->Equals(*(*sum_only)->column(1)));
  EXPECT_TRUE((*both)->column(2)->Equals(*(*avg_only)->column(1)));
}

}  // namespace
}  // namespace antb1::exec
