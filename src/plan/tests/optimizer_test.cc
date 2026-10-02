#include "antb1/plan/optimizer.h"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "antb1/plan/explain.h"
#include "antb1/plan/logical_plan.h"

#include "plan_test_util.h"

namespace antb1::plan {
namespace {

using testing::BindSql;
using testing::MakeCatalog;
using testing::Nth;

LogicalPlan Optimized(std::string_view sql) {
  static const Catalog catalog = MakeCatalog();
  auto plan = BindSql(sql, catalog);
  EXPECT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
  return plan.ok() ? Optimize(*plan) : LogicalPlan{};
}

TEST(OptimizerTest, CountStarWithoutWhereBecomesRowCount) {
  for (const auto& [sql, table] :
       {std::pair{"SELECT COUNT(*) FROM t", "t"}, std::pair{"SELECT count(*) AS n FROM T;", "T"}}) {
    const LogicalPlan plan = Optimized(sql);
    ASSERT_NE(plan.root, nullptr);
    const auto* rows = std::get_if<RowCountNode>(plan.root.get());
    ASSERT_NE(rows, nullptr) << sql << "\n" << Explain(plan);
    EXPECT_EQ(rows->table_name, table);
    EXPECT_EQ(rows->table->exact_row_count(), 100);
    ASSERT_EQ(plan.output.size(), 1U);
    EXPECT_EQ(plan.output[0].type, LogicalType::kBigInt);
  }
  // Under a LIMIT too.
  const LogicalPlan limited = Optimized("SELECT COUNT(*) FROM t LIMIT 1");
  EXPECT_TRUE(std::holds_alternative<RowCountNode>(Nth(limited, 1))) << Explain(limited);
}

TEST(OptimizerTest, RowCountNeedsASingleCountStarWithoutWhereAndAKnownRowCount) {
  for (const char* sql :
       {"SELECT COUNT(*) FROM u", "SELECT COUNT(*), COUNT(*) FROM t", "SELECT COUNT(i16) FROM t",
        "SELECT COUNT(*) FROM t WHERE i16 > 0", "SELECT COUNT(*) FROM t WHERE i16 < 40000",
        "SELECT COUNT(*) FROM t WHERE i16 = 40000"}) {
    const LogicalPlan plan = Optimized(sql);
    ASSERT_NE(plan.root, nullptr);
    EXPECT_TRUE(std::holds_alternative<AggregateNode>(*plan.root)) << sql << "\n" << Explain(plan);
  }
}

TEST(OptimizerTest, ScansReadOnlyReferencedFields) {
  // Fields of t: i16 0, i32 1, i64 2, u16 3, h 4, d 5, s 6, dt 7, bad 8, "Mixed Case" 9, from 10.
  const LogicalPlan plan =
      Optimized("SELECT s, i16, s AS again FROM t WHERE dt > '2013-07-01' AND i16 < 5 LIMIT 3");
  const auto& project = std::get<ProjectNode>(Nth(plan, 0));
  EXPECT_EQ(std::get<LimitNode>(Nth(plan, 1)).limit, 3);  // moved below the Project
  const auto& filter = std::get<FilterNode>(Nth(plan, 2));
  const auto& scan = std::get<ScanNode>(Nth(plan, 3));
  EXPECT_EQ(scan.fields, (std::vector<int>{0, 6, 7}));  // table order
  ASSERT_EQ(project.columns.size(), 3U);
  EXPECT_EQ(project.columns[0].index, 1);  // s
  EXPECT_EQ(project.columns[1].index, 0);  // i16
  EXPECT_EQ(project.columns[2].index, 1);  // s
  ASSERT_EQ(filter.predicates.size(), 2U);
  EXPECT_EQ(filter.predicates[0].column.value_or(BoundColumn{}).index, 2);  // dt
  EXPECT_EQ(filter.predicates[1].column.value_or(BoundColumn{}).index, 0);  // i16
  EXPECT_EQ(plan.output.size(), 3U);
  EXPECT_EQ(plan.output[2].name, "again");
}

TEST(OptimizerTest, AggregatesReadOnlyTheirArguments) {
  const LogicalPlan plan = Optimized("SELECT MAX(\"from\"), COUNT(*), SUM(d), MIN(d) FROM u");
  const auto& agg = std::get<AggregateNode>(Nth(plan, 0));
  const auto& scan = std::get<ScanNode>(Nth(plan, 1));
  EXPECT_EQ(scan.fields, (std::vector<int>{5, 10}));
  ASSERT_EQ(agg.aggregates.size(), 4U);
  EXPECT_EQ(agg.aggregates[0].arg.value_or(BoundColumn{}).index, 1);
  EXPECT_FALSE(agg.aggregates[1].arg.has_value());
  EXPECT_EQ(agg.aggregates[2].arg.value_or(BoundColumn{}).index, 0);
  EXPECT_EQ(agg.aggregates[3].arg.value_or(BoundColumn{}).index, 0);
}

TEST(OptimizerTest, CountStarWithWhereReadsOnlyTheFilteredColumns) {
  // A folded never-true comparison needs no column; the other one keeps its column.
  const LogicalPlan plan = Optimized("SELECT COUNT(*) FROM t WHERE u16 = -1 AND i64 <> 1.5");
  const auto& filter = std::get<FilterNode>(Nth(plan, 1));
  const auto& scan = std::get<ScanNode>(Nth(plan, 2));
  EXPECT_EQ(scan.fields, (std::vector<int>{2}));
  EXPECT_FALSE(filter.predicates.at(0).column.has_value());
  EXPECT_EQ(filter.predicates.at(1).column.value_or(BoundColumn{}).index, 0);
  // Without any column reference the scan reads no field at all (row counts only).
  const LogicalPlan none = Optimized("SELECT COUNT(*) FROM u");
  EXPECT_TRUE(std::get<ScanNode>(Nth(none, 1)).fields.empty());
}

// A Compute reads only the columns its kept expressions use; an expression nothing above uses is
// dropped, and a Compute left with none disappears.
TEST(OptimizerTest, ComputeKeepsOnlyWhatIsUsed) {
  const LogicalPlan plan = Optimized("SELECT i16 + 1 FROM t WHERE i32 // 2 > 0");
  EXPECT_EQ(Explain(plan),
            "Output: (i16 + 1):SMALLINT\n"
            "Project \"(i16 + 1)\"\n"
            "  Compute (i16 + 1)\n"
            "    Filter \"(i32 // 2)\" > 0\n"
            "      Compute (i32 // 2)\n"
            "        Scan table=t source=fake columns=[i16, i32]\n");
  // The input: Scan(i16, i32) <- Compute(i16 + 1, i32 + 2); only i16 and i16 + 1 are used above.
  const auto catalog = testing::MakeCatalog();
  auto bound = BindSql("SELECT i16, i16 + 1 FROM t", catalog);
  ASSERT_TRUE(bound.ok());
  const auto& project = std::get<ProjectNode>(*bound->root);
  const auto& compute = std::get<ComputeNode>(*project.input);
  ComputeNode wider = compute;
  wider.exprs.push_back(std::make_shared<const Expr>(
      Expr{.node = ColumnExpr{.index = 2, .id = std::get<ScanNode>(*compute.input).ids.at(2)},
           .type = LogicalType::kInteger,
           .name = "unused"}));
  // A new column: an id above every id of the plan (the Project's are the last minted).
  wider.ids.push_back(ColumnId{std::to_underlying(std::ranges::max(OutputIds(project))) + 1});
  ProjectNode top = project;
  top.input = std::make_shared<const LogicalNode>(std::move(wider));
  const LogicalPlan pruned = Optimize(LogicalPlan{
      .root = std::make_shared<const LogicalNode>(std::move(top)), .output = bound->output});
  EXPECT_EQ(Explain(pruned),
            "Output: i16:SMALLINT (i16 + 1):SMALLINT\n"
            "Project i16, \"(i16 + 1)\"\n"
            "  Compute (i16 + 1)\n"
            "    Scan table=t source=fake columns=[i16]\n");
  ProjectNode columns_only = project;
  columns_only.columns.pop_back();
  columns_only.ids.pop_back();
  const LogicalPlan dropped =
      Optimize(LogicalPlan{.root = std::make_shared<const LogicalNode>(std::move(columns_only)),
                           .output = {bound->output[0]}});
  EXPECT_EQ(Explain(dropped),
            "Output: i16:SMALLINT\n"
            "Project i16\n"
            "  Scan table=t source=fake columns=[i16]\n");
}

// A Limit moves below a Compute too: only the rows kept are computed.
TEST(OptimizerTest, LimitMovesBelowCompute) {
  const LogicalPlan plan = Optimized("SELECT i32 * 2 FROM t ORDER BY i32 LIMIT 3");
  EXPECT_EQ(Explain(plan),
            "Output: (i32 * 2):INTEGER\n"
            "Project \"(i32 * 2)\"\n"
            "  Limit 3\n"
            "    Sort i32 ASC NULLS LAST\n"
            "      Compute (i32 * 2)\n"
            "        Scan table=t source=fake columns=[i32]\n");
  const LogicalPlan unordered = Optimized("SELECT i32 * 2 FROM t LIMIT 3");
  EXPECT_EQ(Explain(unordered),
            "Output: (i32 * 2):INTEGER\n"
            "Project \"(i32 * 2)\"\n"
            "  Compute (i32 * 2)\n"
            "    Limit 3\n"
            "      Scan table=t source=fake columns=[i32]\n");
}

TEST(OptimizerTest, SelectStarKeepsEveryField) {
  const LogicalPlan plan = Optimized("SELECT * FROM ok");
  EXPECT_EQ(std::get<ScanNode>(Nth(plan, 1)).fields, (std::vector<int>{0, 1}));
}

TEST(OptimizerTest, IsIdempotentAndKeepsTheOutput) {
  for (const char* sql :
       {"SELECT COUNT(*) FROM t", "SELECT * FROM ok WHERE s = 'x' LIMIT 2",
        "SELECT AVG(i32), MAX(dt) FROM t WHERE dt >= DATE '2013-07-01' AND d < 0.5",
        "SELECT i64, dt FROM u WHERE i64 > 9223372036854775807",
        "SELECT i32 - 1, COUNT(*) AS c FROM t GROUP BY i32, i32 - 1 ORDER BY c DESC LIMIT 3",
        "SELECT i32, i32 + 1, COUNT(*) FROM t GROUP BY i32, i32 + 1 LIMIT 1"}) {
    const LogicalPlan once = Optimized(sql);
    const LogicalPlan twice = Optimize(once);
    EXPECT_EQ(Explain(twice), Explain(once)) << sql;
  }
}

// A GROUP BY key computed only from other keys is dropped from the GroupAggregate and computed
// once per group above it (docs/adr/0018-dependent-group-keys.md); the output is unchanged.
TEST(OptimizerTest, GroupsByTheKeysThatDetermineTheOthers) {
  const LogicalPlan plan =
      Optimized("SELECT i32, i32 - 1, i32 * 2, COUNT(*) FROM t GROUP BY i32, i32 - 1, i32 * 2");
  EXPECT_EQ(Explain(plan),
            "Output: i32:INTEGER (i32 - 1):INTEGER (i32 * 2):INTEGER count_star():BIGINT\n"
            "Project i32, \"(i32 - 1)\", \"(i32 * 2)\", \"count_star()\"\n"
            "  Project i32, \"(i32 - 1)\", \"(i32 * 2)\", \"COUNT(*)\"\n"
            "    Compute (i32 - 1), (i32 * 2)\n"
            "      GroupAggregate keys=[i32] COUNT(*)\n"
            "        Scan table=t source=fake columns=[i32]\n");
  // Under ORDER BY ... LIMIT the Limit stays right above the Sort (top-N).
  const LogicalPlan top = Optimized(
      "SELECT i32 - 1, COUNT(*) AS c FROM t GROUP BY i32, i32 - 1 ORDER BY c DESC LIMIT 3");
  EXPECT_EQ(Explain(top),
            "Output: (i32 - 1):INTEGER c:BIGINT\n"
            "Project \"(i32 - 1)\", c\n"
            "  Limit 3\n"
            "    Sort c DESC NULLS LAST\n"
            "      Project i32, \"(i32 - 1)\", \"COUNT(*)\"\n"
            "        Compute (i32 - 1)\n"
            "          GroupAggregate keys=[i32] COUNT(*)\n"
            "            Scan table=t source=fake columns=[i32]\n");
  // A VARCHAR key, HAVING on the dependent key, and a key that is not the first one.
  const LogicalPlan having =
      Optimized("SELECT s, strlen(s), SUM(i16) FROM t GROUP BY s, strlen(s) HAVING strlen(s) > 1");
  EXPECT_EQ(Explain(having),
            "Output: s:VARCHAR strlen(s):BIGINT sum(i16):HUGEINT\n"
            "Project s, \"strlen(s)\", \"sum(i16)\"\n"
            "  Filter \"strlen(s)\" > 1\n"
            "    Project s, \"strlen(s)\", \"SUM(i16)\"\n"
            "      Compute strlen(s)\n"
            "        GroupAggregate keys=[s] SUM(i16)\n"
            "          Scan table=t source=fake columns=[i16, s]\n");
  const LogicalPlan second =
      Optimized("SELECT i32, i16 + 1, COUNT(*) FROM t GROUP BY i32, i16, i16 + 1");
  EXPECT_EQ(Explain(second),
            "Output: i32:INTEGER (i16 + 1):SMALLINT count_star():BIGINT\n"
            "Project i32, \"(i16 + 1)\", \"count_star()\"\n"
            "  Project i32, i16, \"(i16 + 1)\", \"COUNT(*)\"\n"
            "    Compute (i16 + 1)\n"
            "      GroupAggregate keys=[i32, i16] COUNT(*)\n"
            "        Scan table=t source=fake columns=[i16, i32]\n");
}

// Every rule keeps the column ids (ADR 0022): those of the output, those that the nodes above a
// rewritten node read, and those of the fields that a pruned Scan keeps.
TEST(OptimizerTest, KeepsTheColumnIds) {
  static const Catalog catalog = MakeCatalog();
  for (const char* sql :
       {"SELECT i16 + 1 FROM t WHERE i32 // 2 > 0", "SELECT COUNT(*) FROM t HAVING COUNT(*) > 1",
        "SELECT i32, i32 + 1, SUM(i32 + 1) FROM t GROUP BY i32, i32 + 1",
        "SELECT s, strlen(s), SUM(i16) FROM t GROUP BY s, strlen(s) HAVING strlen(s) > 1",
        "SELECT i32 * 2 FROM t ORDER BY i32 LIMIT 3", "SELECT * FROM ok WHERE s = 'x' LIMIT 2"}) {
    auto bound = BindSql(sql, catalog);
    ASSERT_TRUE(bound.ok()) << sql;
    const LogicalPlan optimized = Optimize(*bound);
    EXPECT_EQ(PositionMismatch(optimized), std::nullopt) << sql;
    EXPECT_EQ(OutputIds(*optimized.root), OutputIds(*bound->root)) << sql;
  }

  auto field = BindSql("SELECT i32 FROM t", catalog);
  ASSERT_TRUE(field.ok());
  const LogicalPlan pruned = Optimize(*field);
  const auto& scan = std::get<ScanNode>(Nth(pruned, 1));
  EXPECT_EQ(scan.fields, (std::vector<int>{1}));
  EXPECT_EQ(scan.ids, (std::vector<ColumnId>{std::get<ScanNode>(Nth(*field, 1)).ids.at(1)}));

  // RowCount is the COUNT(*) column it replaces: HAVING above it still reads it.
  auto counted = BindSql("SELECT COUNT(*) FROM t HAVING COUNT(*) > 1", catalog);
  ASSERT_TRUE(counted.ok());
  const LogicalPlan rows = Optimize(*counted);
  const auto& having = std::get<FilterNode>(*rows.root);
  const auto& count = std::get<RowCountNode>(*having.input);
  EXPECT_EQ(count.id, std::get<AggregateNode>(Nth(*counted, 1)).aggregates.at(0).id);
  EXPECT_EQ(having.predicates.at(0).column.value_or(BoundColumn{}).id, count.id);

  // The dependent key keeps its column, computed above the GroupAggregate, and the Project that
  // restores the order passes the GroupAggregate's columns through.
  auto grouped = BindSql("SELECT i32, i32 - 1, COUNT(*) FROM t GROUP BY i32, i32 - 1", catalog);
  ASSERT_TRUE(grouped.ok());
  const std::vector<ColumnId> group_ids = OutputIds(Nth(*grouped, 1));
  ASSERT_EQ(group_ids.size(), 3U);
  const LogicalPlan reduced = Optimize(*grouped);
  EXPECT_EQ(std::get<ProjectNode>(Nth(reduced, 1)).ids, group_ids);
  EXPECT_EQ(std::get<ComputeNode>(Nth(reduced, 2)).ids, (std::vector<ColumnId>{group_ids[1]}));
  EXPECT_EQ(std::get<GroupAggregateNode>(Nth(reduced, 3)).key_ids,
            (std::vector<ColumnId>{group_ids[0]}));
}

// Under a LIMIT without ORDER BY the nodes above stop reading early: a key computed above the
// GroupAggregate would skip the other groups, and so their errors. It stays a key.
TEST(OptimizerTest, KeepsDependentKeysUnderALimitWithoutASort) {
  for (const char* sql : {"SELECT i32, i32 + 1, COUNT(*) FROM t GROUP BY i32, i32 + 1 LIMIT 1",
                          "SELECT i32 + 1, COUNT(*) AS c FROM t GROUP BY i32, i32 + 1 HAVING c > 1 "
                          "LIMIT 2 OFFSET 1"}) {
    const LogicalPlan plan = Optimized(sql);
    ASSERT_NE(plan.root, nullptr);
    bool found = false;
    for (LogicalNodePtr node = plan.root; node != nullptr;) {
      if (const auto* group = std::get_if<GroupAggregateNode>(node.get())) {
        EXPECT_EQ(group->keys.size(), 2U) << sql << "\n" << Explain(plan);
        found = true;
        break;
      }
      const LogicalNodePtr* input = InputOf(*node);
      node = input == nullptr ? nullptr : *input;
    }
    EXPECT_TRUE(found) << sql << "\n" << Explain(plan);
  }
}

TEST(OptimizerTest, KeepsKeysThatOtherKeysDoNotDetermine) {
  // Over a DOUBLE key (-0.0 and 0.0 are one group; a function of them may differ); over a column
  // that is not a key; plain columns only; an expression of a column that is not a key.
  for (const auto& [sql, keys] :
       {std::pair{"SELECT d, d + 1, COUNT(*) FROM t GROUP BY d, d + 1", 2U},
        std::pair{"SELECT i32 + i16, COUNT(*) FROM t GROUP BY i32, i32 + i16", 2U},
        std::pair{"SELECT i32, i16, COUNT(*) FROM t GROUP BY i32, i16", 2U},
        std::pair{"SELECT i32 + 1, COUNT(*) FROM t GROUP BY i32 + 1", 1U}}) {
    const LogicalPlan plan = Optimized(sql);
    ASSERT_NE(plan.root, nullptr);
    const auto& project = std::get<ProjectNode>(*plan.root);
    const auto* group = std::get_if<GroupAggregateNode>(project.input.get());
    ASSERT_NE(group, nullptr) << sql << "\n" << Explain(plan);
    EXPECT_EQ(group->keys.size(), keys) << sql << "\n" << Explain(plan);
  }
}

TEST(OptimizerTest, EmptyPlanStaysEmpty) {
  const LogicalPlan plan = Optimize(LogicalPlan{});
  EXPECT_EQ(plan.root, nullptr);
  EXPECT_TRUE(plan.output.empty());
}

}  // namespace
}  // namespace antb1::plan
