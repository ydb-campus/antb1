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
  return plan.ok() ? Optimized(*plan) : LogicalPlan{};
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
  const LogicalPlan pruned = Optimized(LogicalPlan{
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
      Optimized(LogicalPlan{.root = std::make_shared<const LogicalNode>(std::move(columns_only)),
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
    const LogicalPlan twice = Optimized(once);
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
    const LogicalPlan optimized = Optimized(*bound);
    EXPECT_EQ(PositionMismatch(optimized), std::nullopt) << sql;
    EXPECT_EQ(OutputIds(*optimized.root), OutputIds(*bound->root)) << sql;
  }

  auto field = BindSql("SELECT i32 FROM t", catalog);
  ASSERT_TRUE(field.ok());
  const LogicalPlan pruned = Optimized(*field);
  const auto& scan = std::get<ScanNode>(Nth(pruned, 1));
  EXPECT_EQ(scan.fields, (std::vector<int>{1}));
  EXPECT_EQ(scan.ids, (std::vector<ColumnId>{std::get<ScanNode>(Nth(*field, 1)).ids.at(1)}));

  // RowCount is the COUNT(*) column it replaces: HAVING above it still reads it.
  auto counted = BindSql("SELECT COUNT(*) FROM t HAVING COUNT(*) > 1", catalog);
  ASSERT_TRUE(counted.ok());
  const LogicalPlan rows = Optimized(*counted);
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
  const LogicalPlan reduced = Optimized(*grouped);
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
  LogicalNodePtr operator()(DependentJoinNode node) const {
    // No keys and no residual to map: a dependent join holds only its two inputs.
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
    const LogicalPlan expected = Optimized(*bound);
    for (const int position : {0, 999}) {
      const std::function<int(ColumnId, int)> scramble = [position](ColumnId, int) {
        return position;
      };
      const LogicalPlan scrambled{.root = MapReferences{.f = scramble}.Map(bound->root),
                                  .output = bound->output};
      if (position == 999) {  // no plan here has that many columns
        ASSERT_NE(References(scrambled), References(*bound)) << sql;
      }
      const LogicalPlan optimized = Optimized(scrambled);
      EXPECT_EQ(Explain(optimized), Explain(expected)) << sql;
      EXPECT_EQ(References(optimized), References(expected)) << sql;
      EXPECT_EQ(PositionMismatch(optimized), std::nullopt) << sql;
    }
  }
}

// ---- joins (hand-built plans: kinds and shapes no query makes) ----

LogicalNodePtr Node(LogicalNode node) {
  return std::make_shared<const LogicalNode>(std::move(node));
}

// A Scan of fields 0-3 of AllTypesSchema (i16, i32, i64, u16) as columns first .. first + 3.
LogicalNodePtr FourFieldsRows(std::uint32_t first, int64_t rows) {
  return Node(ScanNode{
      .table = std::make_shared<testing::FakeTable>(testing::AllTypesSchema(), rows),
      .table_name = "t" + std::to_string(first),
      .fields = {0, 1, 2, 3},
      .ids = {ColumnId{first}, ColumnId{first + 1}, ColumnId{first + 2}, ColumnId{first + 3}},
      .span = {}});
}

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

// What `needed_above` records, which is the set no other pass computes: the join's own output ids
// that something above it reads. Here the Project reads two columns and the join's keys and
// residual read four more, so the keys and residual columns are carried past the join unread --
// which is the opportunity it exists to name, and the set a consumer would prune a build by.
//
// **It is recorded as ids and must be verified by whoever drops on it.** A column missing from the
// list reads as unneeded, and `CloneWithFreshIds` mints ids no earlier pass can know, so the only
// safe consumer is one that fails loudly when it finds a dropped column is read after all.
TEST(OptimizerTest, AJoinRecordsWhatIsReadAboveIt) {
  const LogicalPlan plan = Optimized(ProjectOverJoin(
      JoinKind::kInner, {FieldOf(11, 3), FieldOf(1, 0)}, {Less(FieldOf(1, 2), FieldOf(11, 2))}));
  const JoinNode& join = JoinBelowProject(plan);
  const std::vector<ColumnId> out = OutputIds(LogicalNode(join));
  // Sorted, so that the plan a pass produces does not depend on a hash order.
  EXPECT_TRUE(std::ranges::is_sorted(join.needed_above));
  // Exactly the Project's two columns, and every one of them an output of this join.
  const auto& project = std::get<ProjectNode>(*plan.root);
  std::vector<ColumnId> read;
  read.reserve(project.columns.size());
  for (const BoundColumn& column : project.columns) {
    read.push_back(column.id);
  }
  std::ranges::sort(read);
  EXPECT_EQ(join.needed_above, read);
  for (const ColumnId id : join.needed_above) {
    EXPECT_NE(std::ranges::find(out, id), out.end()) << "an id this join does not output";
  }
  // And the rest of its output is carried unread: the keys and the residual's columns.
  EXPECT_EQ(out.size() - join.needed_above.size(), 4U);
}

// The same set at a **nested** join, which is where it is worth anything and where it was wrong.
// `Prune` carries one demand set down both branches of every join, so by the time it reaches a join
// in the left input of another it also holds the ids of that other join's right input. Those are
// not this join's columns, and a consumer that verifies the set -- which it must, since a missing
// id reads as unneeded -- cannot tell a foreign id from one a later pass re-minted, so it refuses
// the whole set and drops nothing. The flat case above cannot catch that: there, the two sets
// coincide.
TEST(OptimizerTest, ANestedJoinRecordsOnlyItsOwnColumns) {
  // Project [t1.i16, t21.i16] over Join(Join(t1, t11) on i32, t21) on t1.i64 = t21.i64: the top
  // join reads a column of t21, so t21's ids are in the demand the nested join is pruned under.
  const LogicalNodePtr inner =
      Node(JoinNode{.kind = JoinKind::kInner,
                    .left = FourFields(1),
                    .right = FourFields(11),
                    .keys = {JoinKey{.left = FieldOf(1, 1), .right = FieldOf(11, 1)}},
                    .residual = {},
                    .build = BuildSide::kRight,
                    .span = {}});
  ProjectNode project{
      .input = Node(JoinNode{.kind = JoinKind::kInner,
                             .left = inner,
                             .right = FourFields(21),
                             .keys = {JoinKey{.left = FieldOf(1, 2), .right = FieldOf(21, 2)}},
                             .residual = {},
                             .build = BuildSide::kRight,
                             .span = {}}),
      .columns = {FieldOf(1, 0), FieldOf(21, 0)},
      .constants = {},
      .ids = {ColumnId{100}, ColumnId{101}},
      .span = {}};
  const LogicalPlan plan = Optimized(PlanOver(Node(std::move(project))));
  EXPECT_EQ(PositionMismatch(plan), std::nullopt);
  const auto& top = std::get<JoinNode>(*std::get<ProjectNode>(*plan.root).input);
  const auto& nested = std::get<JoinNode>(*top.left);
  // Every id of either set is an output of the join that holds it. Nothing of t21 is in the nested
  // join's, though the demand it was pruned under held two of t21's columns.
  for (const JoinNode* join : {&top, &nested}) {
    const std::vector<ColumnId> out = OutputIds(LogicalNode(*join));
    EXPECT_TRUE(std::ranges::is_sorted(join->needed_above));
    for (const ColumnId id : join->needed_above) {
      EXPECT_NE(std::ranges::find(out, id), out.end()) << "an id this join does not output";
    }
  }
  // And it is not empty, which is what an over-eager intersection would leave: the top join reads
  // t1.i16 above and t1.i64 as its own key, both of which the nested join must carry.
  EXPECT_EQ(nested.needed_above.size(), 2U);
}

// Each input keeps the columns needed above, the keys and the residual's columns, in field order.
TEST(OptimizerTest, PrunesThroughBothInputsOfAJoin) {
  const LogicalPlan plan = Optimized(ProjectOverJoin(
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
  EXPECT_EQ(Explain(Optimized(plan)), Explain(plan)) << "idempotent";

  // A semi join outputs its left input only: the right input keeps just its key.
  const LogicalPlan semi = Optimized(ProjectOverJoin(JoinKind::kSemi, {FieldOf(1, 3)}));
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
  const LogicalPlan plan = Optimized(PlanOver(Node(JoinNode{.kind = JoinKind::kOneRow,
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

  const LogicalPlan above = Optimized(PlanOver(Node(AggregateNode{
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
  const LogicalPlan plain = Optimized(PlanOver(join));
  EXPECT_EQ(PositionMismatch(plain), std::nullopt);
  const auto* group = Find<GroupAggregateNode>(plain.root);
  ASSERT_NE(group, nullptr);
  EXPECT_EQ(group->keys.size(), 1U) << Explain(plain);

  const LogicalPlan limited =
      Optimized(PlanOver(Node(LimitNode{.input = join, .limit = 3, .offset = 0, .span = {}})));
  EXPECT_EQ(PositionMismatch(limited), std::nullopt);
  group = Find<GroupAggregateNode>(limited.root);
  ASSERT_NE(group, nullptr);
  EXPECT_EQ(group->keys.size(), 2U) << Explain(limited);

  // Limit(Project(Join)): the Limit moves below the Project and stays above the Join.
  const LogicalPlan project = ProjectOverJoin(JoinKind::kInner, {FieldOf(1, 0)});
  const LogicalPlan moved = Optimized(
      PlanOver(Node(LimitNode{.input = project.root, .limit = 2, .offset = 0, .span = {}})));
  EXPECT_EQ(PositionMismatch(moved), std::nullopt);
  const auto& above = std::get<ProjectNode>(*moved.root);
  const auto& limit = std::get<LimitNode>(*above.input);
  EXPECT_TRUE(std::holds_alternative<JoinNode>(*limit.input)) << Explain(moved);

  // Inside an input: Limit(Project(Scan)) of the right input becomes Project(Limit(Scan)).
  auto limited_right = BindSql("SELECT i16 FROM t LIMIT 3", catalog);
  ASSERT_TRUE(limited_right.ok()) << limited_right.status().ToString();
  right_key.id = OutputIds(*limited_right->root).front();
  const LogicalPlan inner = Optimized(
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
  const LogicalPlan expected = Optimized(bound);
  for (const int position : {0, 999}) {
    const std::function<int(ColumnId, int)> scramble = [position](ColumnId, int) {
      return position;
    };
    const LogicalPlan scrambled{.root = MapReferences{.f = scramble}.Map(bound.root),
                                .output = bound.output};
    const LogicalPlan optimized = Optimized(scrambled);
    EXPECT_EQ(Explain(optimized), Explain(expected));
    EXPECT_EQ(References(optimized), References(expected));
  }
}

TEST(OptimizerTest, EmptyPlanStaysEmpty) {
  const LogicalPlan plan = Optimized(LogicalPlan{});
  EXPECT_EQ(plan.root, nullptr);
  EXPECT_TRUE(plan.output.empty());
}

// The unnesting pass, the first and only fallible step (ADR 0023). U1 has no rewrite, so it rejects
// every dependent join: kUnsupported at the subquery's span, which the CLI turns into exit code 4,
// never a bind error (1) or an internal error (70). No other rule runs on a rejected plan.
TEST(OptimizerTest, TheUnnestingPassRejectsWhatNeitherPathFits) {
  const SourceSpan span{.offset = 13, .length = 5};
  // Path 2 takes a one-row dependent join, and declines a right input that is not the ungrouped
  // aggregate over a correlation it knows: a bare Scan is not one.
  const auto one_row = Optimize(PlanOver(Node(DependentJoinNode{.kind = JoinKind::kOneRow,
                                                                .left = FourFields(1),
                                                                .right = FourFields(11),
                                                                .span = span})))
                           .status();
  ASSERT_NE(GetSqlError(one_row), nullptr) << one_row.ToString();
  EXPECT_EQ(GetSqlError(one_row)->kind(), SqlErrorDetail::Kind::kUnsupported);
  EXPECT_EQ(GetSqlError(one_row)->span(), span) << "the subquery, not the whole query";
  EXPECT_EQ(one_row.message(),
            "a correlated scalar subquery over a block that is not an ungrouped aggregate over "
            "its correlation is not supported");

  // Path 1 takes a semi or anti one, and declines a right input that is not a block it knows:
  // a bare Scan has no filter of correlated conjuncts.
  for (const JoinKind kind : {JoinKind::kSemi, JoinKind::kAnti}) {
    const auto status =
        Optimize(PlanOver(Node(DependentJoinNode{
                     .kind = kind, .left = FourFields(1), .right = FourFields(11), .span = span})))
            .status();
    ASSERT_NE(GetSqlError(status), nullptr) << ToString(kind) << ": " << status.ToString();
    EXPECT_EQ(GetSqlError(status)->kind(), SqlErrorDetail::Kind::kUnsupported);
    EXPECT_EQ(GetSqlError(status)->span(), span);
    EXPECT_EQ(status.message(),
              std::format("a correlated {} over a block that is not a plain FROM and WHERE is not "
                          "supported",
                          kind == JoinKind::kSemi ? "EXISTS" : "NOT EXISTS"));
  }
}

// ---- ADR 0023's Path 1 (roadmap id U2) ----

// A correlated subquery's block as the binder leaves it: the inner Scan under a Compute that
// evaluates the correlated conjuncts and a Filter that tests them, which is where the pass looks.
// `conjuncts` are the PredicateExprs, one per correlated conjunct.
LogicalNodePtr CorrelatedBlockOver(LogicalNodePtr inner, std::vector<ExprPtr> conjuncts,
                                   std::uint32_t first_id) {
  std::vector<ColumnId> ids;
  std::vector<Predicate> predicates;
  for (std::size_t i = 0; i < conjuncts.size(); ++i) {
    ids.push_back(ColumnId{first_id + static_cast<std::uint32_t>(i)});
    predicates.push_back(Predicate{
        .kind = Predicate::Kind::kIsTrue,
        .column = BoundColumn{.id = ids.back(), .name = "c", .type = LogicalType::kBoolean}});
  }
  return Node(FilterNode{
      .input = Node(ComputeNode{
          .input = std::move(inner), .exprs = std::move(conjuncts), .ids = std::move(ids)}),
      .predicates = std::move(predicates)});
}

LogicalNodePtr CorrelatedBlock(std::vector<ExprPtr> conjuncts, std::uint32_t first_id) {
  return CorrelatedBlockOver(FourFields(11), std::move(conjuncts), first_id);
}

// `left <op> right` as the binder writes a correlated conjunct: a PredicateExpr whose operands
// carry the ids and whose own column is operand-local.
ExprPtr Compare(const BoundColumn& left, const BoundColumn& right, CompareOp op) {
  const auto leaf = [](const BoundColumn& c) {
    return std::make_shared<const Expr>(
        Expr{.node = ColumnExpr{.id = c.id}, .type = c.type, .name = c.name});
  };
  return std::make_shared<const Expr>(
      Expr{.node = PredicateExpr{.predicate = Predicate{.kind = Predicate::Kind::kCompareColumns,
                                                        .column = BoundColumn{},
                                                        .other = BoundColumn{},
                                                        .op = op},
                                 .operands = {leaf(left), leaf(right)}},
           .type = LogicalType::kBoolean,
           .name = "cmp"});
}

// ADR 0026's C2b and C3: a semi join may build on the side it preserves, the outer one, when the
// subquery's side is estimated at more than `build_side_ratio` hundredths times it -- the shape a
// correlated EXISTS over a large table has. An anti join cannot emit its build rows yet (C2c), so
// it keeps building on the subquery whatever the estimates say.
// The pass rewrites top-down, so the OUTER of two nested dependent joins is rewritten while its
// left input still holds the inner one. Reading a dependent join's rows as unknown therefore left
// that outer join building on its subquery however lopsided it was, and only the inner one ever
// moved -- which is why a query with two correlated subqueries at one level kept the larger of the
// two builds. An estimate now reads through a dependent join to its left input (ADR 0026).
TEST(OptimizerTest, Path1ChoosesASideForTheOuterOfTwoNestedDependentJoins) {
  // 10 outer rows, and a subquery of `inner` rows on each level.
  const auto sides = [](int64_t inner) {
    const LogicalNodePtr nested = Node(DependentJoinNode{
        .kind = JoinKind::kSemi,
        .left = FourFields(1),
        .right = CorrelatedBlockOver(FourFieldsRows(11, inner),
                                     {Compare(FieldOf(11, 1), FieldOf(1, 1), CompareOp::kEq)}, 50),
        .span = {}});
    const arrow::Result<LogicalPlan> plan = Optimize(PlanOver(Node(DependentJoinNode{
        .kind = JoinKind::kSemi,
        .left = nested,
        .right = CorrelatedBlockOver(FourFieldsRows(21, inner),
                                     {Compare(FieldOf(21, 1), FieldOf(1, 1), CompareOp::kEq)}, 60),
        .span = {}})));
    EXPECT_TRUE(plan.ok()) << plan.status().ToString();
    std::vector<BuildSide> out;
    if (!plan.ok()) {
      return out;
    }
    // Outer first, then the inner one it stands on.
    const auto* outer = std::get_if<JoinNode>(plan->root.get());
    EXPECT_NE(outer, nullptr) << Explain(*plan);
    if (outer == nullptr) {
      return out;
    }
    out.push_back(outer->build);
    const auto* inner_join = std::get_if<JoinNode>(outer->left.get());
    EXPECT_NE(inner_join, nullptr) << Explain(*plan);
    if (inner_join != nullptr) {
      out.push_back(inner_join->build);
      // The outer join's own estimate is its left input's, so a join above it would read one too.
      EXPECT_EQ(outer->estimate, std::optional<int64_t>(10)) << Explain(*plan);
    }
    return out;
  };
  // Both levels are lopsided the same way, so both move. Before this, only the inner one did.
  EXPECT_EQ(sides(4000), (std::vector<BuildSide>{BuildSide::kLeft, BuildSide::kLeft}));
  // And neither moves when neither subquery is decisively larger.
  EXPECT_EQ(sides(10), (std::vector<BuildSide>{BuildSide::kRight, BuildSide::kRight}));
}

TEST(OptimizerTest, Path1BuildsOnTheOuterSideOfASemiJoinOverAMuchLargerSubquery) {
  // The outer side is 10 rows; `inner` is the subquery's.
  const auto side = [](JoinKind kind, int64_t inner, int64_t ratio) {
    const arrow::Result<LogicalPlan> plan =
        Optimize(PlanOver(Node(DependentJoinNode{
                     .kind = kind,
                     .left = FourFields(1),
                     .right = CorrelatedBlockOver(
                         FourFieldsRows(11, inner),
                         {Compare(FieldOf(11, 1), FieldOf(1, 1), CompareOp::kEq)}, 50),
                     .span = {}})),
                 BindOptions{.build_side_ratio = ratio});
    EXPECT_TRUE(plan.ok()) << plan.status().ToString();
    if (!plan.ok()) {
      return BuildSide::kRight;
    }
    const auto* join = std::get_if<JoinNode>(plan->root.get());
    EXPECT_NE(join, nullptr) << Explain(*plan);
    return join == nullptr ? BuildSide::kRight : join->build;
  };
  // Exactly the ratio is not more than it, so the boundary keeps the side every join had before.
  EXPECT_EQ(side(JoinKind::kSemi, 40, kBuildSideRatio), BuildSide::kRight);
  EXPECT_EQ(side(JoinKind::kSemi, 41, kBuildSideRatio), BuildSide::kLeft);
  EXPECT_EQ(side(JoinKind::kSemi, 4000, kBuildSideRatio), BuildSide::kLeft);
  // A ratio of 1 builds on the smaller side whenever it is smaller at all (ADR 0022's rule).
  EXPECT_EQ(side(JoinKind::kSemi, 11, 100), BuildSide::kLeft);
  EXPECT_EQ(side(JoinKind::kSemi, 10, 100), BuildSide::kRight) << "a tie keeps the subquery";
  // The subquery being smaller never moves the build: there is nothing to win.
  EXPECT_EQ(side(JoinKind::kSemi, 1, kBuildSideRatio), BuildSide::kRight);
  // An anti join moves by the same rule (ADR 0026's C2c): a correlated NOT EXISTS over a much
  // larger subquery built the whole of it to probe it with far fewer rows, exactly as EXISTS did.
  EXPECT_EQ(side(JoinKind::kAnti, 40, kBuildSideRatio), BuildSide::kRight);
  EXPECT_EQ(side(JoinKind::kAnti, 41, kBuildSideRatio), BuildSide::kLeft);
  EXPECT_EQ(side(JoinKind::kAnti, 4000, kBuildSideRatio), BuildSide::kLeft);
  EXPECT_EQ(side(JoinKind::kAnti, 1, kBuildSideRatio), BuildSide::kRight);
  // A null-aware anti join never reaches this rule at all: the pass admits only semi, anti and
  // one-row kinds and refuses a correlated NOT IN outright, so where it still may not build on its
  // left input is HashJoinOperator::Make (exec.HashJoinTest.MisuseIsInvalid).
}

// The pull-up: the block's conjunct becomes the join's key, the dependent join becomes a semi or
// anti join building on the subquery, and the block's own nodes below the filter are kept.
TEST(OptimizerTest, Path1PullsACorrelatedEqualityIntoTheJoinKey) {
  for (const JoinKind kind : {JoinKind::kSemi, JoinKind::kAnti}) {
    // t11.i32 = t1.i32: one type on both sides, so the key needs no cast.
    const LogicalPlan plan = Optimized(PlanOver(Node(DependentJoinNode{
        .kind = kind,
        .left = FourFields(1),
        .right = CorrelatedBlock({Compare(FieldOf(11, 1), FieldOf(1, 1), CompareOp::kEq)}, 50),
        .span = {}})));
    const auto* join = std::get_if<JoinNode>(plan.root.get());
    ASSERT_NE(join, nullptr) << Explain(plan);
    EXPECT_EQ(join->kind, kind);
    EXPECT_EQ(join->build, BuildSide::kRight) << "the subquery builds (ADR 0022)";
    ASSERT_EQ(join->keys.size(), 1U) << Explain(plan);
    EXPECT_EQ(join->keys[0].left.id, FieldOf(1, 1).id) << "the outer side";
    EXPECT_EQ(join->keys[0].right.id, FieldOf(11, 1).id) << "the inner side";
    EXPECT_TRUE(join->residual.empty()) << Explain(plan);
    // The block's filter and compute are gone; its scan is the join's right input.
    EXPECT_TRUE(std::holds_alternative<ScanNode>(*join->right)) << Explain(plan);
    EXPECT_EQ(PositionMismatch(plan), std::nullopt) << Explain(plan);
  }
}

// A conjunct that reads both sides and is no key becomes a residual, evaluated over the candidate
// pairs; the equality still has to be there, since a correlated EXISTS without a key exits 4.
TEST(OptimizerTest, Path1MakesANonEqualityAResidual) {
  const LogicalPlan plan = Optimized(PlanOver(Node(DependentJoinNode{
      .kind = JoinKind::kSemi,
      .left = FourFields(1),
      .right = CorrelatedBlock({Compare(FieldOf(11, 1), FieldOf(1, 1), CompareOp::kEq),
                                Compare(FieldOf(11, 2), FieldOf(1, 2), CompareOp::kLt)},
                               50),
      .span = {}})));
  const auto* join = std::get_if<JoinNode>(plan.root.get());
  ASSERT_NE(join, nullptr) << Explain(plan);
  EXPECT_EQ(join->keys.size(), 1U);
  EXPECT_EQ(join->residual.size(), 1U) << Explain(plan);
  EXPECT_EQ(PositionMismatch(plan), std::nullopt) << Explain(plan);
}

// Sides of two types are cast to their common one, each on its own side of the join, as ADR 0022's
// keys are: ResolvePositions requires a key's two sides to have one type.
TEST(OptimizerTest, Path1CastsAKeyOfTwoTypes) {
  // t11.i16 is SMALLINT and t1.i32 is INTEGER, so both become INTEGER.
  const LogicalPlan plan = Optimized(PlanOver(Node(DependentJoinNode{
      .kind = JoinKind::kSemi,
      .left = FourFields(1),
      .right = CorrelatedBlock({Compare(FieldOf(11, 0), FieldOf(1, 1), CompareOp::kEq)}, 50),
      .span = {}})));
  const auto* join = std::get_if<JoinNode>(plan.root.get());
  ASSERT_NE(join, nullptr) << Explain(plan);
  ASSERT_EQ(join->keys.size(), 1U) << Explain(plan);
  EXPECT_EQ(join->keys[0].left.type, join->keys[0].right.type) << "one type on both sides";
  EXPECT_EQ(join->keys[0].right.type, LogicalType::kInteger);
  // The cast is computed on the inner side, above the scan.
  EXPECT_TRUE(std::holds_alternative<ComputeNode>(*join->right)) << Explain(plan);
  EXPECT_EQ(PositionMismatch(plan), std::nullopt) << Explain(plan);
}

// An outer-only conjunct is a residual, in both polarities. For an anti join that is required:
// where the conjunct is false or NULL the subquery has no row, so NOT EXISTS must keep the outer
// row, which a filter on the outer rows would drop. For a semi join a residual gives the same
// answer as the filter ADR 0023 suggests, and keeps the join's output columns, which a computed
// probe-side column would widen.
TEST(OptimizerTest, Path1MakesAnOuterOnlyConjunctAResidual) {
  for (const JoinKind kind : {JoinKind::kSemi, JoinKind::kAnti}) {
    const LogicalPlan plan = Optimized(PlanOver(Node(DependentJoinNode{
        .kind = kind,
        .left = FourFields(1),
        .right = CorrelatedBlock({Compare(FieldOf(11, 1), FieldOf(1, 1), CompareOp::kEq),
                                  Compare(FieldOf(1, 2), FieldOf(1, 3), CompareOp::kLt)},
                                 50),
        .span = {}})));
    const auto* join = std::get_if<JoinNode>(plan.root.get());
    ASSERT_NE(join, nullptr) << Explain(plan);
    EXPECT_EQ(join->keys.size(), 1U) << Explain(plan);
    EXPECT_EQ(join->residual.size(), 1U) << ToString(kind) << ": " << Explain(plan);
    EXPECT_TRUE(std::holds_alternative<ScanNode>(*join->left))
        << "no filter and no compute on the outer side: " << Explain(plan);
    EXPECT_EQ(PositionMismatch(plan), std::nullopt) << Explain(plan);
    // The pass keeps the plan's output columns, which a probe-side computed column would widen.
    EXPECT_EQ(OutputIds(*plan.root).size(), 4U) << Explain(plan);
  }
}

// Without a key the pass refuses: the hash table and the candidate pairs need one (ADR 0023).
TEST(OptimizerTest, Path1RefusesABlockWithoutAKey) {
  const auto status =
      Optimize(PlanOver(Node(DependentJoinNode{
                   .kind = JoinKind::kSemi,
                   .left = FourFields(1),
                   .right = CorrelatedBlock(
                       {Compare(FieldOf(11, 1), FieldOf(1, 1), CompareOp::kLt)}, 50),
                   .span = {}})))
          .status();
  ASSERT_NE(GetSqlError(status), nullptr) << status.ToString();
  EXPECT_EQ(GetSqlError(status)->kind(), SqlErrorDetail::Kind::kUnsupported);
  EXPECT_EQ(status.message(), "a correlated EXISTS without a key is not supported");
}

// ---- ADR 0023's Path 2 (roadmap id U3) ----

// An ungrouped-aggregate block over the correlated filter, as the binder leaves it.
LogicalNodePtr ScalarBlock(std::vector<ExprPtr> conjuncts, AggKind kind, std::uint32_t first_id) {
  const ColumnId value{first_id + 90};
  return Node(AggregateNode{
      .input = CorrelatedBlock(std::move(conjuncts), first_id),
      .aggregates = {AggregateCall{
          .kind = kind, .arg = FieldOf(11, 2), .type = LogicalType::kBigInt, .id = value}}});
}

// A plan over a dependent join as a real query has it: a Project of the outer columns on top, so
// the plan's output does not change when Path 2 adds the group's key columns to the join's own.
LogicalPlan PlanOverJoin(LogicalNodePtr join, std::uint32_t outer_first) {
  ProjectNode project{.input = std::move(join), .columns = {}, .constants = {}, .ids = {}};
  for (int field = 0; field < 4; ++field) {
    BoundColumn column = FieldOf(outer_first, field);
    project.ids.push_back(column.id);
    project.columns.push_back(std::move(column));
  }
  return PlanOver(Node(std::move(project)));
}

// The join below the plan's Project.
const JoinNode* JoinUnder(const LogicalPlan& plan) {
  const auto* project = std::get_if<ProjectNode>(plan.root.get());
  return project == nullptr ? nullptr : std::get_if<JoinNode>(project->input.get());
}

// The rewrite: the aggregation groups by the inner equivalent, the dependent join becomes an inner
// join keyed on it, and the block's correlated filter is gone.
TEST(OptimizerTest, Path2GroupsByTheInnerEquivalent) {
  const LogicalPlan plan = Optimized(PlanOverJoin(
      Node(DependentJoinNode{
          .kind = JoinKind::kOneRow,
          .left = FourFields(1),
          .right = ScalarBlock({Compare(FieldOf(11, 1), FieldOf(1, 1), CompareOp::kEq)},
                               AggKind::kMin, 50),
          .span = {}}),
      1));
  const auto* join = JoinUnder(plan);
  ASSERT_NE(join, nullptr) << Explain(plan);
  EXPECT_EQ(join->kind, JoinKind::kInner);
  EXPECT_EQ(join->build, BuildSide::kRight);
  ASSERT_EQ(join->keys.size(), 1U) << Explain(plan);
  EXPECT_EQ(join->keys[0].left.id, FieldOf(1, 1).id) << "the outer column";
  const auto* grouped = std::get_if<GroupAggregateNode>(join->right.get());
  ASSERT_NE(grouped, nullptr) << Explain(plan);
  ASSERT_EQ(grouped->keys.size(), 1U);
  EXPECT_EQ(grouped->keys[0].id, FieldOf(11, 1).id) << "the inner equivalent";
  EXPECT_EQ(join->keys[0].right.id, grouped->key_ids[0]) << "the group's output, not its input";
  EXPECT_TRUE(std::holds_alternative<ScanNode>(*grouped->input))
      << "the correlated filter is gone: " << Explain(plan);
  EXPECT_EQ(PositionMismatch(plan), std::nullopt) << Explain(plan);
}

// ---- ADR 0023's semi-join reduction ----

// A scalar block whose inner relation holds `rows`, so that the gate's ratio can be set: it fires
// only where the aggregation's own input is at least kLeastRestriction times the outer side.
LogicalNodePtr ScalarBlockRows(std::vector<ExprPtr> conjuncts, std::uint32_t first_id,
                               std::optional<int64_t> rows, std::uint32_t inner_first = 11) {
  const ColumnId value{first_id + 90};
  return Node(AggregateNode{
      .input = CorrelatedBlockOver(
          Node(ScanNode{
              .table = std::make_shared<testing::FakeTable>(testing::AllTypesSchema(), rows),
              .table_name = "t" + std::to_string(inner_first),
              .fields = {0, 1, 2, 3},
              .ids = {ColumnId{inner_first}, ColumnId{inner_first + 1}, ColumnId{inner_first + 2},
                      ColumnId{inner_first + 3}},
              .span = {}}),
          std::move(conjuncts), first_id),
      .aggregates = {AggregateCall{.kind = AggKind::kMin,
                                   .arg = FieldOf(inner_first, 2),
                                   .type = LogicalType::kBigInt,
                                   .id = value}}});
}

// The nodes of a plan, counted: what an unbounded clone shows up in.
std::size_t NodeCount(const LogicalNodePtr& node) {
  if (node == nullptr) {
    return 0;
  }
  std::size_t count = 1;
  for (const LogicalNodePtr& input : InputsOf(*node)) {
    count += NodeCount(input);
  }
  return count;
}

// Path 2's plan with an outer side of `outer` rows and an inner one of `inner`.
LogicalPlan Path2Plan(std::vector<ExprPtr> conjuncts, int64_t outer, std::optional<int64_t> inner) {
  return Optimized(
      PlanOverJoin(Node(DependentJoinNode{.kind = JoinKind::kOneRow,
                                          .left = FourFieldsRows(1, outer),
                                          .right = ScalarBlockRows(std::move(conjuncts), 50, inner),
                                          .span = {}}),
                   1));
}

// The restriction below the group aggregate, or nullptr where the gate refused.
const JoinNode* RestrictionUnder(const LogicalPlan& plan) {
  const auto* join = JoinUnder(plan);
  if (join == nullptr) {
    return nullptr;
  }
  const auto* grouped = std::get_if<GroupAggregateNode>(join->right.get());
  return grouped == nullptr ? nullptr : std::get_if<JoinNode>(grouped->input.get());
}

// The reduction the ADR defers under its own name: the aggregation's input restricted to the rows
// whose correlated keys the outer side binds. A semi join, which cannot multiply its probe's rows,
// building on its right input (ADR 0022) so that the outer side is the build whatever the
// estimates say, and with one key per correlated column so that no correlation is dropped.
TEST(OptimizerTest, TheSemiJoinReductionRestrictsTheAggregatesInput) {
  const LogicalPlan plan = Path2Plan({Compare(FieldOf(11, 1), FieldOf(1, 1), CompareOp::kEq),
                                      Compare(FieldOf(11, 2), FieldOf(1, 2), CompareOp::kEq)},
                                     10, 1000);
  const auto* restriction = RestrictionUnder(plan);
  ASSERT_NE(restriction, nullptr) << Explain(plan);
  EXPECT_EQ(restriction->kind, JoinKind::kSemi);
  EXPECT_EQ(restriction->build, BuildSide::kRight) << "the outer side is the build";
  ASSERT_EQ(restriction->keys.size(), 2U) << Explain(plan);
  EXPECT_EQ(restriction->keys[0].left.id, FieldOf(11, 1).id) << "the inner column it groups by";
  EXPECT_EQ(restriction->keys[1].left.id, FieldOf(11, 2).id);
  EXPECT_TRUE(std::holds_alternative<ScanNode>(*restriction->left))
      << "its probe is the aggregation's own input: " << Explain(plan);

  // The domain is a clone, so its keys are columns of neither the outer side nor the inner one: the
  // same node under both the join and the restriction would be a column defined twice.
  const std::vector<ColumnId> outer = OutputIds(*FourFieldsRows(1, 10));
  for (const JoinKey& key : restriction->keys) {
    EXPECT_EQ(std::ranges::find(outer, key.right.id), outer.end())
        << "the restriction keys on the clone, not on the outer side itself";
  }
  EXPECT_EQ(PositionMismatch(plan), std::nullopt) << Explain(plan);
}

// **Stacked rewrites may not multiply the plan.** The pass is top-down, so a dependent join still
// in this one's left input is one the rewrite has not reached; cloning it would duplicate a rewrite
// that clones in turn, and k stacked correlated subqueries would cost 2^k nodes. Measured before
// the condition that forbids it, on a query of k conjuncts over one correlation: 1,533 plan lines
// at k=8 and 24,573 at k=12, where planning alone took 1.3 s.
//
// So the outer of two stacked rewrites declines and the inner one restricts: one clone for the
// chain, and a node count within a factor of the unstacked plan's.
TEST(OptimizerTest, TheSemiJoinReductionDoesNotCloneAStackOfRewrites) {
  const auto two_keys = [](std::uint32_t inner) {
    return std::vector<ExprPtr>{Compare(FieldOf(inner, 1), FieldOf(1, 1), CompareOp::kEq),
                                Compare(FieldOf(inner, 2), FieldOf(1, 2), CompareOp::kEq)};
  };
  const LogicalNodePtr inner =
      Node(DependentJoinNode{.kind = JoinKind::kOneRow,
                             .left = FourFieldsRows(1, 10),
                             .right = ScalarBlockRows(two_keys(11), 50, 1000, 11),
                             .span = {}});
  const LogicalPlan one = Optimized(PlanOverJoin(inner, 1));
  const LogicalPlan two = Optimized(
      PlanOverJoin(Node(DependentJoinNode{.kind = JoinKind::kOneRow,
                                          .left = inner,
                                          .right = ScalarBlockRows(two_keys(21), 60, 1000, 21),
                                          .span = {}}),
                   1));
  EXPECT_EQ(PositionMismatch(one), std::nullopt) << Explain(one);
  EXPECT_EQ(PositionMismatch(two), std::nullopt) << Explain(two);

  // Exactly one restriction in the stacked plan: the inner rewrite's, whose own left input holds no
  // dependent join. Doubling would show here as two, and as a node count far above the single
  // plan's.
  const auto semi_joins = [](const LogicalNodePtr& node) {
    std::size_t found = 0;
    const auto walk = [&found](const LogicalNodePtr& n, const auto& self) -> void {
      if (n == nullptr) {
        return;
      }
      if (const auto* join = std::get_if<JoinNode>(n.get());
          join != nullptr && join->kind == JoinKind::kSemi) {
        ++found;
      }
      for (const LogicalNodePtr& input : InputsOf(*n)) {
        self(input, self);
      }
    };
    walk(node, walk);
    return found;
  };
  EXPECT_EQ(semi_joins(one.root), 1U) << Explain(one);
  EXPECT_EQ(semi_joins(two.root), 1U) << Explain(two);
  EXPECT_LT(NodeCount(two.root), NodeCount(one.root) * 2)
      << "a stacked rewrite costs a constant, not a factor: " << Explain(two);
}

// One correlated column is ADR 0029's D5 to restrict: it costs a filter where this costs a build
// and a probe, and restricting such a query as well was measured at 1.420x. More than one key is
// what D5 cannot reach, so the key count is what this rewrite is gated on.
TEST(OptimizerTest, TheSemiJoinReductionLeavesOneKeyToTheDynamicFilter) {
  const LogicalPlan plan =
      Path2Plan({Compare(FieldOf(11, 1), FieldOf(1, 1), CompareOp::kEq)}, 10, 1000);
  EXPECT_EQ(RestrictionUnder(plan), nullptr) << Explain(plan);
  const auto* join = JoinUnder(plan);
  ASSERT_NE(join, nullptr) << Explain(plan);
  const auto* grouped = std::get_if<GroupAggregateNode>(join->right.get());
  ASSERT_NE(grouped, nullptr) << Explain(plan);
  EXPECT_TRUE(std::holds_alternative<ScanNode>(*grouped->input)) << Explain(plan);
}

// **A join below the aggregation is the other case D5 cannot reach**, whatever the key count: the
// physical planner's offer does not survive a join on its way down, so a single correlated key has
// nowhere to attach and this rewrite is what restricts it. Measured at 0.931x with a peak of
// 483 MiB against 1,011 on the query of the 22 shaped that way.
//
// The pair of tests is the point: the same one key restricts over a join and does not over a scan,
// so the structural fact and not the key count alone is what the gate reads.
TEST(OptimizerTest, TheSemiJoinReductionRestrictsOneKeyOverAJoin) {
  const std::vector<ExprPtr> one = {Compare(FieldOf(11, 1), FieldOf(1, 1), CompareOp::kEq)};
  const LogicalNodePtr joined =
      Node(JoinNode{.kind = JoinKind::kInner,
                    .left = FourFieldsRows(11, 1000),
                    .right = FourFieldsRows(31, 1000),
                    .keys = {JoinKey{.left = FieldOf(11, 2), .right = FieldOf(31, 2)}},
                    .residual = {},
                    .build = BuildSide::kRight,
                    .estimate = 1000,
                    .span = {}});
  const LogicalPlan plan = Optimized(PlanOverJoin(
      Node(DependentJoinNode{
          .kind = JoinKind::kOneRow,
          .left = FourFieldsRows(1, 10),
          .right = Node(AggregateNode{.input = CorrelatedBlockOver(joined, one, 50),
                                      .aggregates = {AggregateCall{.kind = AggKind::kMin,
                                                                   .arg = FieldOf(11, 2),
                                                                   .type = LogicalType::kBigInt,
                                                                   .id = ColumnId{140}}}}),
          .span = {}}),
      1));
  // The block's own join is also directly under the aggregate in this shape, so the assertions have
  // to name what distinguishes the restriction from it: its kind, its build side, and a probe that
  // is the block's join rather than a scan.
  const auto* restriction = RestrictionUnder(plan);
  ASSERT_NE(restriction, nullptr) << Explain(plan);
  EXPECT_EQ(restriction->kind, JoinKind::kSemi) << Explain(plan);
  EXPECT_EQ(restriction->build, BuildSide::kRight) << Explain(plan);
  EXPECT_TRUE(std::holds_alternative<JoinNode>(*restriction->left))
      << "its probe is the block's own join: " << Explain(plan);
  ASSERT_EQ(restriction->keys.size(), 1U) << Explain(plan);
  EXPECT_EQ(restriction->keys[0].left.id, FieldOf(11, 1).id);
  EXPECT_EQ(PositionMismatch(plan), std::nullopt) << Explain(plan);
}

// The gate: an outer side the aggregation's input does not dwarf buys a build and a probe for
// nothing, and an outer side nothing estimated cannot be shown to be small at all. Both leave
// Path 2's plan exactly as it was, which is correct and is what shipped before this.
TEST(OptimizerTest, TheSemiJoinReductionRefusesWhatItCannotShowIsWorthIt) {
  const std::vector<ExprPtr> two = {Compare(FieldOf(11, 1), FieldOf(1, 1), CompareOp::kEq),
                                    Compare(FieldOf(11, 2), FieldOf(1, 2), CompareOp::kEq)};
  const auto refuses = [&two](int64_t outer, std::optional<int64_t> inner) {
    const LogicalPlan plan = Path2Plan(two, outer, inner);
    EXPECT_EQ(RestrictionUnder(plan), nullptr) << Explain(plan);
    const auto* join = JoinUnder(plan);
    ASSERT_NE(join, nullptr) << Explain(plan);
    const auto* grouped = std::get_if<GroupAggregateNode>(join->right.get());
    ASSERT_NE(grouped, nullptr) << Explain(plan);
    EXPECT_TRUE(std::holds_alternative<ScanNode>(*grouped->input)) << Explain(plan);
    EXPECT_EQ(PositionMismatch(plan), std::nullopt) << Explain(plan);
  };
  refuses(1000, 1000);        // the same size: the restriction removes little
  refuses(1000, 3999);        // just under the ratio
  refuses(10, std::nullopt);  // an inner side nothing estimated
  refuses(0, 1000);           // an outer side of no rows, which may not read as small

  // Just over the ratio it fires, so the boundary is the gate's and not an accident of the shape.
  const LogicalPlan fires = Path2Plan(two, 1000, 4000);
  EXPECT_NE(RestrictionUnder(fires), nullptr) << Explain(fires);
}

// One key per OUTER column. Two correlations give two keys and two group keys; the case where two
// outer columns share one inner equivalent, where a key per CLASS would drop the second column's
// constraint and keep rows SQL rejects, needs two outer columns of one type and is pinned against
// the oracle in tests/slt/cases/subqueries/scalar.slt.
TEST(OptimizerTest, Path2KeysEveryOuterColumn) {
  const LogicalPlan plan = Optimized(PlanOverJoin(
      Node(DependentJoinNode{
          .kind = JoinKind::kOneRow,
          .left = FourFields(1),
          .right = ScalarBlock({Compare(FieldOf(11, 1), FieldOf(1, 1), CompareOp::kEq),
                                Compare(FieldOf(11, 2), FieldOf(1, 2), CompareOp::kEq)},
                               AggKind::kMax, 50),
          .span = {}}),
      1));
  const auto* join = JoinUnder(plan);
  ASSERT_NE(join, nullptr) << Explain(plan);
  ASSERT_EQ(join->keys.size(), 2U) << "one per outer column: " << Explain(plan);
  EXPECT_NE(join->keys[0].left.id, join->keys[1].left.id);
  const auto* grouped = std::get_if<GroupAggregateNode>(join->right.get());
  ASSERT_NE(grouped, nullptr) << Explain(plan);
  EXPECT_EQ(grouped->keys.size(), 2U) << Explain(plan);
  EXPECT_EQ(PositionMismatch(plan), std::nullopt) << Explain(plan);
}

// The COUNT bug (R. A. Ganski and H. K. T. Wong, SIGMOD 1987): COUNT is 0 over no rows, so an
// outer row with no group would pass a comparison the inner join drops it from. It must refuse,
// not answer -- this was a live wrong answer before the strictness classifier existed.
TEST(OptimizerTest, Path2RefusesACountAndAnyNonStrictCombination) {
  for (const AggKind kind : {AggKind::kCountStar, AggKind::kCount, AggKind::kCountDistinct}) {
    const auto status =
        Optimize(PlanOver(Node(DependentJoinNode{
                     .kind = JoinKind::kOneRow,
                     .left = FourFields(1),
                     .right = ScalarBlock({Compare(FieldOf(11, 1), FieldOf(1, 1), CompareOp::kEq)},
                                          kind, 50),
                     .span = {}})))
            .status();
    ASSERT_NE(GetSqlError(status), nullptr) << status.ToString();
    EXPECT_EQ(GetSqlError(status)->kind(), SqlErrorDetail::Kind::kUnsupported);
    EXPECT_EQ(status.message(),
              "a correlated scalar subquery whose aggregate is not SUM, AVG, MIN or MAX is not "
              "supported");
  }
}

// Without an inner equivalent for every outer column the substitution cannot run.
TEST(OptimizerTest, Path2RefusesAnOuterColumnWithoutAnInnerEquivalent) {
  const auto status =
      Optimize(PlanOver(Node(DependentJoinNode{
                   .kind = JoinKind::kOneRow,
                   .left = FourFields(1),
                   .right = ScalarBlock({Compare(FieldOf(11, 1), FieldOf(1, 1), CompareOp::kLt)},
                                        AggKind::kSum, 50),
                   .span = {}})))
          .status();
  ASSERT_NE(GetSqlError(status), nullptr) << status.ToString();
  EXPECT_EQ(status.message(),
            "a correlated scalar subquery with an outer column that has no inner equivalent is "
            "not supported");
}

// A plan with no dependent join is unaffected: the pass is an identity and the other rules run.
TEST(OptimizerTest, TheUnnestingPassLeavesAnOrdinaryPlanAlone) {
  const LogicalPlan plan = Optimized("SELECT COUNT(*) FROM t");
  EXPECT_TRUE(std::holds_alternative<RowCountNode>(*plan.root)) << Explain(plan);
}

}  // namespace
}  // namespace antb1::plan
