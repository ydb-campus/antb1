#include "antb1/plan/optimizer.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
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
      const std::vector<LogicalNodePtr> inputs = InputsOf(*node);
      node = inputs.empty() ? nullptr : inputs[0];
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

// The plan with every column reference that has an id at position f(id, position): Project
// constants and the operand-local columns of a PredicateExpr (no ids) stay as they are.
struct MapReferences {
  const std::function<int(ColumnId, int)>& f;

  [[nodiscard]] LogicalNodePtr Map(const LogicalNodePtr& node) const {
    return std::visit(*this, *node);
  }
  void Column(BoundColumn& column) const {
    if (column.id != kNoColumnId) {
      column.index = f(column.id, column.index);
    }
  }
  void Column(std::optional<BoundColumn>& column) const {
    if (column.has_value()) {
      Column(*column);
    }
  }
  [[nodiscard]] ExprPtr Expression(const ExprPtr& expr) const {
    return MapColumns(expr, [this](ColumnExpr column) {
      if (column.id != kNoColumnId) {
        column.index = f(column.id, column.index);
      }
      return column;
    });
  }
  template <class Node>
  [[nodiscard]] LogicalNodePtr Over(Node node) const {
    node.input = Map(node.input);
    return std::make_shared<const LogicalNode>(std::move(node));
  }

  LogicalNodePtr operator()(const ScanNode& node) const {
    return std::make_shared<const LogicalNode>(node);
  }
  LogicalNodePtr operator()(FilterNode node) const {
    for (Predicate& p : node.predicates) {
      Column(p.column);
      Column(p.other);
    }
    return Over(std::move(node));
  }
  LogicalNodePtr operator()(ComputeNode node) const {
    for (ExprPtr& expr : node.exprs) {
      expr = Expression(expr);
    }
    return Over(std::move(node));
  }
  LogicalNodePtr operator()(ProjectNode node) const {
    for (BoundColumn& column : node.columns) {
      Column(column);
    }
    return Over(std::move(node));
  }
  LogicalNodePtr operator()(AggregateNode node) const {
    for (AggregateCall& call : node.aggregates) {
      Column(call.arg);
    }
    return Over(std::move(node));
  }
  LogicalNodePtr operator()(GroupAggregateNode node) const {
    for (BoundColumn& key : node.keys) {
      Column(key);
    }
    for (AggregateCall& call : node.aggregates) {
      Column(call.arg);
    }
    return Over(std::move(node));
  }
  LogicalNodePtr operator()(SortNode node) const {
    for (SortKey& key : node.keys) {
      Column(key.column);
    }
    return Over(std::move(node));
  }
  LogicalNodePtr operator()(LimitNode node) const { return Over(std::move(node)); }
  LogicalNodePtr operator()(const RowCountNode& node) const {
    return std::make_shared<const LogicalNode>(node);
  }
  LogicalNodePtr operator()(JoinNode node) const {
    for (JoinKey& key : node.keys) {
      Column(key.left);
      Column(key.right);
    }
    for (ExprPtr& conjunct : node.residual) {
      conjunct = Expression(conjunct);
    }
    node.left = Map(node.left);
    node.right = Map(node.right);
    return std::make_shared<const LogicalNode>(std::move(node));
  }
};

// The id and the position of every column reference that has an id, from the root down.
std::vector<std::pair<ColumnId, int>> References(const LogicalPlan& plan) {
  std::vector<std::pair<ColumnId, int>> out;
  const std::function<int(ColumnId, int)> record = [&out](ColumnId id, int index) {
    out.emplace_back(id, index);
    return index;
  };
  (void)MapReferences{.f = record}.Map(plan.root);
  return out;
}

// The rules find columns by their ids and never read a position (ADR 0022): with every position of
// the bound plan wrong, Optimize gives the same plan, positions included.
TEST(OptimizerTest, RulesReadColumnIdsNotPositions) {
  static const Catalog catalog = MakeCatalog();
  constexpr const char* kHavingAlias =
      "SELECT i16, CASE WHEN COUNT(*) > 1 THEN 'many' END AS size FROM t GROUP BY i16 "
      "HAVING size = 'many' OR size = 'few'";
  for (const char* sql : {
           "SELECT COUNT(*) FROM t HAVING COUNT(*) > 1",
           "SELECT s, i16, s AS again FROM t WHERE dt > '2013-07-01' AND i16 < 5 LIMIT 3",
           "SELECT i16 + 1 FROM t WHERE i32 // 2 > 0",
           "SELECT i32 * 2 FROM t LIMIT 3",
           "SELECT i32, i32 - 1, i32 * 2, COUNT(*) FROM t GROUP BY i32, i32 - 1, i32 * 2",
           "SELECT i32 - 1, COUNT(*) AS c FROM t GROUP BY i32, i32 - 1 ORDER BY c DESC LIMIT 3",
           "SELECT s, strlen(s), SUM(i16) FROM t GROUP BY s, strlen(s) HAVING strlen(s) > 1",
           "SELECT i32, i16 + 1, COUNT(*) FROM t GROUP BY i32, i16, i16 + 1",
           "SELECT i32, i32 + 1, SUM(i32 + 1) FROM t GROUP BY i32, i32 + 1",
           "SELECT i32, i32 + 1, COUNT(*) FROM t GROUP BY i32, i32 + 1 LIMIT 1",
           "SELECT MAX(\"from\"), COUNT(*), SUM(d), MIN(d) FROM u",
           "SELECT i16, 42, 'x' AS c, i16 * 2 FROM t WHERE i16 > 0 OR i32 < 5 ORDER BY 4",
           "SELECT CASE WHEN i16 > 0 THEN s ELSE 'neg' END AS c FROM t ORDER BY c LIMIT 5",
           "SELECT SUM(i32) + 1, COUNT(*) FROM t HAVING SUM(i32) > 5 OR COUNT(*) < 2",
           kHavingAlias,
       }) {
    auto bound = BindSql(sql, catalog);
    ASSERT_TRUE(bound.ok()) << sql << ": " << bound.status().ToString();
    const LogicalPlan expected = Optimize(*bound);
    for (const int position : {0, 999}) {
      const std::function<int(ColumnId, int)> scramble = [position](ColumnId, int) {
        return position;
      };
      const LogicalPlan scrambled{.root = MapReferences{.f = scramble}.Map(bound->root),
                                  .output = bound->output};
      if (position == 999) {  // no plan here has that many columns
        ASSERT_NE(References(scrambled), References(*bound)) << sql;
      }
      const LogicalPlan optimized = Optimize(scrambled);
      EXPECT_EQ(Explain(optimized), Explain(expected)) << sql;
      EXPECT_EQ(References(optimized), References(expected)) << sql;
      EXPECT_EQ(PositionMismatch(optimized), std::nullopt) << sql;
    }
  }
}

// ---- joins (hand-built plans: no SQL produces a join yet) ----

LogicalNodePtr Node(LogicalNode node) {
  return std::make_shared<const LogicalNode>(std::move(node));
}

// A Scan of fields 0-3 of AllTypesSchema (i16, i32, i64, u16) as columns first .. first + 3.
LogicalNodePtr FourFields(std::uint32_t first) {
  return Node(ScanNode{
      .table = std::make_shared<testing::FakeTable>(testing::AllTypesSchema(), 10),
      .table_name = "t" + std::to_string(first),
      .fields = {0, 1, 2, 3},
      .ids = {ColumnId{first}, ColumnId{first + 1}, ColumnId{first + 2}, ColumnId{first + 3}},
      .span = {}});
}

// Field `field` of FourFields(first).
BoundColumn FieldOf(std::uint32_t first, int field) {
  const std::vector<std::pair<std::string, LogicalType>> fields = {
      {"i16", LogicalType::kSmallInt},
      {"i32", LogicalType::kInteger},
      {"i64", LogicalType::kBigInt},
      {"u16", LogicalType::kUSmallInt}};
  const auto& [name, type] = fields.at(static_cast<std::size_t>(field));
  return BoundColumn{.index = 0,
                     .id = ColumnId{first + static_cast<std::uint32_t>(field)},
                     .name = name,
                     .type = type,
                     .qualifier = "t" + std::to_string(first)};
}

ExprPtr Ref(const BoundColumn& column) {
  return std::make_shared<const Expr>(Expr{.node = ColumnExpr{.index = 0, .id = column.id},
                                           .type = column.type,
                                           .name = column.qualifier + "." + column.name});
}

// a < b over two INTEGER columns, as a BOOLEAN residual.
ExprPtr Less(const BoundColumn& a, const BoundColumn& b) {
  return std::make_shared<const Expr>(Expr{
      .node =
          PredicateExpr{.predicate = Predicate{.kind = Predicate::Kind::kCompareColumns,
                                               .column = BoundColumn{.index = 0, .type = a.type},
                                               .other = BoundColumn{.index = 1, .type = b.type},
                                               .op = CompareOp::kLt},
                        .operands = {Ref(a), Ref(b)}},
      .type = LogicalType::kBoolean,
      .name = "(" + a.qualifier + "." + a.name + " < " + b.qualifier + "." + b.name + ")"});
}

LogicalPlan PlanOver(LogicalNodePtr root) {
  std::vector<OutputColumn> output;
  for (const ColumnId id : OutputIds(*root)) {
    output.push_back(OutputColumn{.name = "c", .type = LogicalType::kBigInt, .id = id});
  }
  return LogicalPlan{.root = std::move(root), .output = std::move(output)};
}

// A Project of `columns` over a join of FourFields(1) and FourFields(11) on t1.i32 = t11.i32.
LogicalPlan ProjectOverJoin(JoinKind kind, const std::vector<BoundColumn>& columns,
                            std::vector<ExprPtr> residual = {}) {
  ProjectNode project{
      .input = Node(JoinNode{.kind = kind,
                             .left = FourFields(1),
                             .right = FourFields(11),
                             .keys = {JoinKey{.left = FieldOf(1, 1), .right = FieldOf(11, 1)}},
                             .residual = std::move(residual),
                             .build = BuildSide::kRight,
                             .span = {}}),
      .columns = columns,
      .constants = {},
      .ids = {},
      .span = {}};
  for (std::size_t i = 0; i < columns.size(); ++i) {
    project.ids.push_back(ColumnId{100 + static_cast<std::uint32_t>(i)});
  }
  return PlanOver(Node(std::move(project)));
}

// The first node of type T, depth first (left input before right).
template <class T>
const T* Find(const LogicalNodePtr& node) {
  if (node == nullptr) {
    return nullptr;
  }
  if (const auto* found = std::get_if<T>(node.get())) {
    return found;
  }
  for (const LogicalNodePtr& input : InputsOf(*node)) {
    if (const T* found = Find<T>(input)) {
      return found;
    }
  }
  return nullptr;
}

const JoinNode& JoinBelowProject(const LogicalPlan& plan) {
  return std::get<JoinNode>(*std::get<ProjectNode>(*plan.root).input);
}

// Each input keeps the columns needed above, the keys and the residual's columns, in field order.
TEST(OptimizerTest, PrunesThroughBothInputsOfAJoin) {
  const LogicalPlan plan = Optimize(ProjectOverJoin(
      JoinKind::kInner, {FieldOf(11, 3), FieldOf(1, 0)}, {Less(FieldOf(1, 2), FieldOf(11, 2))}));
  EXPECT_EQ(PositionMismatch(plan), std::nullopt);
  const JoinNode& join = JoinBelowProject(plan);
  EXPECT_EQ(std::get<ScanNode>(*join.left).fields, (std::vector<int>{0, 1, 2}));
  EXPECT_EQ(std::get<ScanNode>(*join.right).fields, (std::vector<int>{1, 2, 3}));
  // Positions after pruning: the keys in their own input, the residual over both.
  EXPECT_EQ(join.keys[0].left.index, 1);
  EXPECT_EQ(join.keys[0].right.index, 0);
  const auto& residual = std::get<PredicateExpr>(join.residual[0]->node);
  EXPECT_EQ(std::get<ColumnExpr>(residual.operands[0]->node).index, 2);
  EXPECT_EQ(std::get<ColumnExpr>(residual.operands[1]->node).index, 4);
  const auto& project = std::get<ProjectNode>(*plan.root);
  EXPECT_EQ(project.columns[0].index, 5) << "t11.u16, after the 3 left columns";
  EXPECT_EQ(project.columns[1].index, 0);
  EXPECT_EQ(Explain(Optimize(plan)), Explain(plan)) << "idempotent";

  // A semi join outputs its left input only: the right input keeps just its key.
  const LogicalPlan semi = Optimize(ProjectOverJoin(JoinKind::kSemi, {FieldOf(1, 3)}));
  EXPECT_EQ(PositionMismatch(semi), std::nullopt);
  EXPECT_EQ(std::get<ScanNode>(*JoinBelowProject(semi).left).fields, (std::vector<int>{1, 3}));
  EXPECT_EQ(std::get<ScanNode>(*JoinBelowProject(semi).right).fields, (std::vector<int>{1}));
}

// COUNT(*) of a whole table inside a join input still becomes RowCount; above a join it does not.
TEST(OptimizerTest, CountStarInsideAJoinInputBecomesRowCount) {
  const LogicalNodePtr count = Node(
      AggregateNode{.input = FourFields(11),
                    .aggregates = {AggregateCall{.kind = AggKind::kCountStar, .id = ColumnId{20}}},
                    .span = {}});
  const LogicalPlan plan = Optimize(PlanOver(Node(JoinNode{.kind = JoinKind::kOneRow,
                                                           .left = FourFields(1),
                                                           .right = count,
                                                           .keys = {},
                                                           .residual = {},
                                                           .build = BuildSide::kRight,
                                                           .span = {}})));
  EXPECT_EQ(PositionMismatch(plan), std::nullopt);
  const auto& join = std::get<JoinNode>(*plan.root);
  EXPECT_TRUE(std::holds_alternative<RowCountNode>(*join.right)) << Explain(plan);
  EXPECT_EQ(std::get<ScanNode>(*join.left).fields, (std::vector<int>{0, 1, 2, 3}));

  const LogicalPlan above = Optimize(PlanOver(Node(AggregateNode{
      .input = std::get<ProjectNode>(*ProjectOverJoin(JoinKind::kInner, {}).root).input,
      .aggregates = {AggregateCall{.kind = AggKind::kCountStar, .id = ColumnId{30}}},
      .span = {}})));
  EXPECT_TRUE(std::holds_alternative<AggregateNode>(*above.root)) << Explain(above);
  const auto* join_below = Find<JoinNode>(above.root);
  ASSERT_NE(join_below, nullptr);
  EXPECT_EQ(std::get<ScanNode>(*join_below->left).fields, (std::vector<int>{1})) << "the key";
  EXPECT_EQ(std::get<ScanNode>(*join_below->right).fields, (std::vector<int>{1}));
}

// The rules reach a join's inputs: dependent GROUP BY keys inside the right input are rewritten,
// but not under a Limit above the join (conservative: a probe may stop early), and a Limit moves
// below a Project down to the join, never through it.
TEST(OptimizerTest, RulesReachBothInputsOfAJoin) {
  static const Catalog catalog = MakeCatalog();
  auto grouped = BindSql("SELECT i16, i16 + 1, COUNT(*) FROM t GROUP BY i16, i16 + 1", catalog);
  ASSERT_TRUE(grouped.ok()) << grouped.status().ToString();
  ASSERT_LT(std::to_underlying(OutputIds(*grouped->root).back()), 100U) << "ids below 100";
  BoundColumn right_key = FieldOf(1, 0);  // i16 SMALLINT, like the subplan's first column
  right_key.id = OutputIds(*grouped->root).front();
  right_key.qualifier.clear();
  const LogicalNodePtr join =
      Node(JoinNode{.kind = JoinKind::kSemi,
                    .left = FourFields(101),
                    .right = grouped->root,
                    .keys = {JoinKey{.left = FieldOf(101, 0), .right = right_key}},
                    .residual = {},
                    .build = BuildSide::kRight,
                    .span = {}});
  const LogicalPlan plain = Optimize(PlanOver(join));
  EXPECT_EQ(PositionMismatch(plain), std::nullopt);
  const auto* group = Find<GroupAggregateNode>(plain.root);
  ASSERT_NE(group, nullptr);
  EXPECT_EQ(group->keys.size(), 1U) << Explain(plain);

  const LogicalPlan limited =
      Optimize(PlanOver(Node(LimitNode{.input = join, .limit = 3, .offset = 0, .span = {}})));
  EXPECT_EQ(PositionMismatch(limited), std::nullopt);
  group = Find<GroupAggregateNode>(limited.root);
  ASSERT_NE(group, nullptr);
  EXPECT_EQ(group->keys.size(), 2U) << Explain(limited);

  // Limit(Project(Join)): the Limit moves below the Project and stays above the Join.
  const LogicalPlan project = ProjectOverJoin(JoinKind::kInner, {FieldOf(1, 0)});
  const LogicalPlan moved = Optimize(
      PlanOver(Node(LimitNode{.input = project.root, .limit = 2, .offset = 0, .span = {}})));
  EXPECT_EQ(PositionMismatch(moved), std::nullopt);
  const auto& above = std::get<ProjectNode>(*moved.root);
  const auto& limit = std::get<LimitNode>(*above.input);
  EXPECT_TRUE(std::holds_alternative<JoinNode>(*limit.input)) << Explain(moved);

  // Inside an input: Limit(Project(Scan)) of the right input becomes Project(Limit(Scan)).
  auto limited_right = BindSql("SELECT i16 FROM t LIMIT 3", catalog);
  ASSERT_TRUE(limited_right.ok()) << limited_right.status().ToString();
  right_key.id = OutputIds(*limited_right->root).front();
  const LogicalPlan inner = Optimize(
      PlanOver(Node(JoinNode{.kind = JoinKind::kInner,
                             .left = FourFields(101),
                             .right = limited_right->root,
                             .keys = {JoinKey{.left = FieldOf(101, 0), .right = right_key}},
                             .residual = {},
                             .build = BuildSide::kLeft,
                             .span = {}})));
  EXPECT_EQ(PositionMismatch(inner), std::nullopt);
  const auto& right = std::get<ProjectNode>(*std::get<JoinNode>(*inner.root).right);
  EXPECT_TRUE(std::holds_alternative<LimitNode>(*right.input)) << Explain(inner);
}

// As RulesReadColumnIdsNotPositions, over a join: every position wrong, the same plan.
TEST(OptimizerTest, JoinRulesReadColumnIdsNotPositions) {
  const LogicalPlan bound = ProjectOverJoin(JoinKind::kInner, {FieldOf(11, 3), FieldOf(1, 0)},
                                            {Less(FieldOf(1, 2), FieldOf(11, 2))});
  const LogicalPlan expected = Optimize(bound);
  for (const int position : {0, 999}) {
    const std::function<int(ColumnId, int)> scramble = [position](ColumnId, int) {
      return position;
    };
    const LogicalPlan scrambled{.root = MapReferences{.f = scramble}.Map(bound.root),
                                .output = bound.output};
    const LogicalPlan optimized = Optimize(scrambled);
    EXPECT_EQ(Explain(optimized), Explain(expected));
    EXPECT_EQ(References(optimized), References(expected));
  }
}

TEST(OptimizerTest, EmptyPlanStaysEmpty) {
  const LogicalPlan plan = Optimize(LogicalPlan{});
  EXPECT_EQ(plan.root, nullptr);
  EXPECT_TRUE(plan.output.empty());
}

}  // namespace
}  // namespace antb1::plan
