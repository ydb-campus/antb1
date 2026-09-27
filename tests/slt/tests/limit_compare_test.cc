// The comparison of a LIMIT without ORDER BY (result_diff.h: CompareSubset, CompareLimited) and the
// unlimited query it runs on the oracle (unordered_limit.h: UnlimitedSql).

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "engine.h"
#include "result_diff.h"
#include "unordered_limit.h"

namespace antb1::slt {
namespace {

using Row = std::vector<std::optional<std::string>>;

ResultSet Result(std::vector<ColumnClass> classes, std::vector<Row> rows) {
  std::vector<std::string> names(classes.size(), "BIGINT");
  return ResultSet{
      .classes = std::move(classes), .type_names = std::move(names), .rows = std::move(rows)};
}

std::vector<ColumnClass> TwoInts() { return {ColumnClass::kInteger, ColumnClass::kInteger}; }

TEST(CompareSubset, AnyRowsOfTheUnlimitedAnswerAreRight) {
  const auto unlimited = Result(TwoInts(), {{"1", "10"}, {"2", "20"}, {"3", "30"}, {"3", "30"}});
  const auto oracle = Result(TwoInts(), {{"1", "10"}, {"2", "20"}});
  // Other rows than the oracle's, in another order, a repeated row that is repeated there too.
  EXPECT_FALSE(CompareSubset(oracle, unlimited, Result(TwoInts(), {{"3", "30"}, {"1", "10"}})));
  EXPECT_FALSE(CompareSubset(oracle, unlimited, Result(TwoInts(), {{"3", "30"}, {"3", "30"}})));
}

TEST(CompareSubset, RejectsForeignRowsRepeatsAndOtherCounts) {
  const auto unlimited = Result(TwoInts(), {{"1", "10"}, {"2", "20"}, {"3", "30"}});
  const auto oracle = Result(TwoInts(), {{"1", "10"}, {"2", "20"}});
  const auto foreign =
      CompareSubset(oracle, unlimited, Result(TwoInts(), {{"1", "10"}, {"9", "90"}}));
  ASSERT_TRUE(foreign.has_value());
  EXPECT_TRUE(foreign.value_or(Discrepancy{}).mismatch);
  // A row that the unlimited answer has once cannot be returned twice (a multiset).
  EXPECT_TRUE(CompareSubset(oracle, unlimited, Result(TwoInts(), {{"2", "20"}, {"2", "20"}})));
  // As many rows as the oracle's (limited) answer.
  EXPECT_TRUE(CompareSubset(oracle, unlimited, Result(TwoInts(), {{"1", "10"}})));
  EXPECT_TRUE(
      CompareSubset(oracle, unlimited, Result(TwoInts(), {{"1", "10"}, {"2", "20"}, {"3", "30"}})));
  // Column types must match.
  auto other_type = Result(TwoInts(), {{"1", "10"}, {"2", "20"}});
  other_type.type_names[1] = "HUGEINT";
  EXPECT_TRUE(CompareSubset(oracle, unlimited, other_type));
}

TEST(CompareSubset, RealColumnsMatchWithinTheTolerance) {
  const std::vector<ColumnClass> ir = {ColumnClass::kInteger, ColumnClass::kReal};
  const auto unlimited = Result(ir, {{"1", "0.30000000000000004"}, {"2", "1000000000"}});
  const auto oracle = Result(ir, {{"1", "0.30000000000000004"}});
  EXPECT_FALSE(CompareSubset(oracle, unlimited, Result(ir, {{"1", "0.3"}})));
  EXPECT_FALSE(CompareSubset(oracle, unlimited, Result(ir, {{"2", "1000000001"}})));
  EXPECT_TRUE(CompareSubset(oracle, unlimited, Result(ir, {{"2", "1000000100"}})));
  EXPECT_TRUE(CompareSubset(oracle, unlimited, Result(ir, {{"3", "0.3"}}))) << "the I cell differs";
}

TEST(CompareLimited, RunsTheUnlimitedQueryOnlyWhenTheRowsDiffer) {
  const auto oracle = Result(TwoInts(), {{"1", "10"}, {"2", "20"}});
  int calls = 0;
  const auto unlimited = [&] -> ExecResult {
    ++calls;
    return Result(TwoInts(), {{"1", "10"}, {"2", "20"}, {"3", "30"}});
  };
  EXPECT_FALSE(CompareLimited(oracle, Result(TwoInts(), {{"2", "20"}, {"1", "10"}}), unlimited));
  EXPECT_EQ(calls, 0);
  EXPECT_FALSE(CompareLimited(oracle, Result(TwoInts(), {{"3", "30"}, {"1", "10"}}), unlimited));
  EXPECT_EQ(calls, 1);
  EXPECT_TRUE(CompareLimited(oracle, Result(TwoInts(), {{"4", "40"}, {"1", "10"}}), unlimited));
  const auto failing = [] -> ExecResult {
    return std::unexpected(EngineError{.kind = "execution", .message = "boom"});
  };
  EXPECT_TRUE(CompareLimited(oracle, Result(TwoInts(), {{"3", "30"}, {"1", "10"}}), failing));
}

TEST(UnlimitedSql, DropsLimitAndOffsetOfUnorderedQueries) {
  EXPECT_EQ(UnlimitedSql("select a from t limit 5"), "SELECT a FROM t");
  EXPECT_EQ(UnlimitedSql("SELECT a, COUNT(*) FROM t WHERE b = 1 GROUP BY a LIMIT 10 OFFSET 3"),
            "SELECT a, COUNT(*) FROM t WHERE b = 1 GROUP BY a");
  EXPECT_EQ(UnlimitedSql("SELECT a FROM t OFFSET 2"), "SELECT a FROM t");
  EXPECT_EQ(UnlimitedSql("SELECT a FROM t"), std::nullopt) << "no LIMIT";
  EXPECT_EQ(UnlimitedSql("SELECT a FROM t ORDER BY a LIMIT 5"), std::nullopt) << "ordered";
  EXPECT_EQ(UnlimitedSql("SELECT a FROM t WHERE a LIKE 'x' LIMIT 5"), std::nullopt)
      << "not in antb1's grammar";
}

}  // namespace
}  // namespace antb1::slt
