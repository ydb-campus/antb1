#include "antb1/plan/explain.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <gtest/gtest.h>

#include "antb1/plan/logical_plan.h"
#include "antb1/plan/optimizer.h"

#include "plan_test_util.h"

namespace antb1::plan {
namespace {

using testing::BindSql;
using testing::FakeTable;
using testing::MakeCatalog;

std::string ExplainSql(std::string_view sql, bool optimize = true) {
  static const Catalog catalog = MakeCatalog();
  auto plan = BindSql(sql, catalog);
  EXPECT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
  if (!plan.ok()) {
    return {};
  }
  return Explain(optimize ? Optimize(*plan) : *plan);
}

TEST(ExplainTest, RowCount) {
  EXPECT_EQ(ExplainSql("SELECT COUNT(*) AS n FROM t"),
            "Output: n:BIGINT\n"
            "RowCount table=t source=fake\n");
}

TEST(ExplainTest, BoundPlanScansEveryField) {
  EXPECT_EQ(ExplainSql("SELECT COUNT(*) FROM t", /*optimize=*/false),
            "Output: count_star():BIGINT\n"
            "Aggregate COUNT(*)\n"
            "  Scan table=t source=fake columns=[i16, i32, i64, u16, h, d, s, dt, bad, "
            "\"Mixed Case\", from]\n");
}

TEST(ExplainTest, AggregatesOverAFilter) {
  EXPECT_EQ(ExplainSql("SELECT COUNT(*), SUM(i16) AS total, AVG(d), MIN(\"Mixed Case\") FROM t "
                       "WHERE i16 <> 0 AND dt >= '2013-07-01' AND d < 0.5 AND i64 > 1.5 "
                       "AND u16 < 70000 AND i32 = 0.5 AND s = 'it''s'"),
            "Output: count_star():BIGINT total:HUGEINT avg(d):DOUBLE "
            "min(\"Mixed Case\"):INTEGER\n"
            "Aggregate COUNT(*), SUM(i16), AVG(d), MIN(\"Mixed Case\")\n"
            "  Filter i16 <> 0 AND dt >= DATE '2013-07-01' AND d < 0.5 AND i64 >= 2 AND "
            "u16 IS NOT NULL AND FALSE AND s = 'it''s'\n"
            "    Scan table=t source=fake columns=[i16, i64, u16, d, s, dt, \"Mixed Case\"]\n");
}

TEST(ExplainTest, ProjectionWithLimit) {
  EXPECT_EQ(ExplainSql("SELECT * FROM ok LIMIT 10"),
            "Output: i16:SMALLINT s:VARCHAR\n"
            "Project i16, s\n"
            "  Limit 10\n"
            "    Scan table=ok source=fake columns=[i16, s]\n");
  EXPECT_EQ(ExplainSql("SELECT s AS \"The S\", I16 FROM OK WHERE i16 >= -32768.5 LIMIT 0"),
            "Output: The S:VARCHAR i16:SMALLINT\n"
            "Project s, i16\n"
            "  Limit 0\n"
            "    Filter i16 >= -32768\n"
            "      Scan table=OK source=fake columns=[i16, s]\n");
}

// The Limit moves below the Project, right above the Sort (a top-N in the executor); the Sort
// reads a column the query does not select and a hidden aggregate.
TEST(ExplainTest, SortAndOffset) {
  EXPECT_EQ(ExplainSql("SELECT s FROM ok ORDER BY i16 DESC, s NULLS FIRST LIMIT 3 OFFSET 2"),
            "Output: s:VARCHAR\n"
            "Project s\n"
            "  Limit 3 OFFSET 2\n"
            "    Sort i16 DESC NULLS LAST, s ASC NULLS FIRST\n"
            "      Scan table=ok source=fake columns=[i16, s]\n");
  EXPECT_EQ(ExplainSql("SELECT i16 FROM ok GROUP BY i16 ORDER BY COUNT(*) DESC OFFSET 1"),
            "Output: i16:SMALLINT\n"
            "Project i16\n"
            "  Limit ALL OFFSET 1\n"
            "    Sort \"count_star()\" DESC NULLS LAST\n"
            "      GroupAggregate keys=[i16] COUNT(*)\n"
            "        Scan table=ok source=fake columns=[i16]\n");
}

TEST(ExplainTest, In) {
  EXPECT_EQ(ExplainSql("SELECT i16 FROM ok WHERE i16 IN (3, 1.5, 7) AND s NOT IN ('x', 'it''s')"),
            "Output: i16:SMALLINT\n"
            "Project i16\n"
            "  Filter i16 IN (3, 7) AND s NOT IN ('x', 'it''s')\n"
            "    Scan table=ok source=fake columns=[i16, s]\n");
}

// A DECIMAL compared with a DOUBLE constant or IN list compares in DOUBLE (ADR 0021 rule 11), so
// the column is shown converted; a folded comparison keeps the column's type and shows no cast.
TEST(ExplainTest, DecimalComparedInDouble) {
  EXPECT_EQ(ExplainSql("SELECT COUNT(*) FROM dec WHERE p > 1e1 AND q IN (1.5, 2e0) AND "
                       "p NOT IN (1.555) AND z = 2.5 AND p < f"),
            "Output: count_star():BIGINT\n"
            "Aggregate COUNT(*)\n"
            "  Filter CAST(p AS DOUBLE) > 10 AND CAST(q AS DOUBLE) IN (1.5, 2) AND "
            "p IS NOT NULL AND z = 2.5000000000 AND p < f\n"
            "    Scan table=dec source=fake columns=[p, q, z, f]\n");
}

TEST(ExplainTest, Like) {
  EXPECT_EQ(ExplainSql("SELECT i16 FROM ok WHERE s LIKE '%it''s_%' AND s NOT LIKE ''"),
            "Output: i16:SMALLINT\n"
            "Project i16\n"
            "  Filter s LIKE '%it''s_%' AND s NOT LIKE ''\n"
            "    Scan table=ok source=fake columns=[i16, s]\n");
}

// HAVING is a Filter over the aggregation, below the Sort; COUNT(*) without WHERE still becomes a
// RowCount below it.
TEST(ExplainTest, Having) {
  EXPECT_EQ(
      ExplainSql("SELECT s, COUNT(*) AS n FROM ok GROUP BY s HAVING n > 1 AND MIN(i16) IN (1, "
                 "2) AND s LIKE 'a%' ORDER BY s LIMIT 5"),
      "Output: s:VARCHAR n:BIGINT\n"
      "Project s, n\n"
      "  Limit 5\n"
      "    Sort s ASC NULLS LAST\n"
      "      Filter n > 1 AND \"min(i16)\" IN (1, 2) AND s LIKE 'a%'\n"
      "        GroupAggregate keys=[s] COUNT(*), MIN(i16)\n"
      "          Scan table=ok source=fake columns=[i16, s]\n");
  EXPECT_EQ(ExplainSql("SELECT COUNT(*) FROM ok HAVING COUNT(*) > 1"),
            "Output: count_star():BIGINT\n"
            "Filter \"count_star()\" > 1\n"
            "  RowCount table=ok source=fake\n");
}

TEST(ExplainTest, ConstantsAndPositions) {
  EXPECT_EQ(ExplainSql("SELECT 1, 'x', s, COUNT(*) FROM ok GROUP BY 1, 3 ORDER BY 4 DESC"),
            "Output: 1:INTEGER 'x':VARCHAR s:VARCHAR count_star():BIGINT\n"
            "Project 1, 'x', s, \"count_star()\"\n"
            "  Sort \"count_star()\" DESC NULLS LAST\n"
            "    GroupAggregate keys=[s] COUNT(*)\n"
            "      Scan table=ok source=fake columns=[s]\n");
  EXPECT_EQ(ExplainSql("SELECT 2 FROM ok"),
            "Output: 2:INTEGER\n"
            "Project 2\n"
            "  Scan table=ok source=fake columns=[]\n");
}

TEST(ExplainTest, CountDistinct) {
  EXPECT_EQ(ExplainSql("SELECT COUNT(DISTINCT s), COUNT(s) FROM ok"),
            "Output: count(DISTINCT s):BIGINT count(s):BIGINT\n"
            "Aggregate COUNT(DISTINCT s), COUNT(s)\n"
            "  Scan table=ok source=fake columns=[s]\n");
}

TEST(ExplainTest, NamesAndStringsStayOnOneAsciiLine) {
  const auto table = std::make_shared<FakeTable>(
      arrow::schema({arrow::field("a\"b", arrow::binary()), arrow::field("é\n", arrow::int32())}),
      1);
  Catalog catalog;
  ASSERT_TRUE(catalog.Register("w", table).ok());
  auto plan = BindSql("SELECT \"a\"\"b\", \"é\n\" FROM w WHERE \"a\"\"b\" = 'x\ty\\z'", catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  EXPECT_EQ(Explain(Optimize(*plan)),
            "Output: a\"b:VARCHAR \\xC3\\xA9\\x0A:INTEGER\n"
            "Project \"a\"\"b\", \"\\xC3\\xA9\\x0A\"\n"
            "  Filter \"a\"\"b\" = 'x\\x09y\\x5Cz'\n"
            "    Scan table=w source=fake columns=[\"a\"\"b\", \"\\xC3\\xA9\\x0A\"]\n");
}

TEST(ExplainTest, EmptyPlan) { EXPECT_EQ(Explain(LogicalPlan{}), "Output:\n"); }

// ---- joins and qualified names (hand-built plans: no SQL produces a join yet) ----

LogicalNodePtr Node(LogicalNode node) {
  return std::make_shared<const LogicalNode>(std::move(node));
}

LogicalNodePtr ScanOf(std::string name, std::vector<int> fields) {
  return Node(ScanNode{.table = std::make_shared<FakeTable>(testing::AllTypesSchema(), 10),
                       .table_name = std::move(name),
                       .fields = std::move(fields),
                       .ids = {},
                       .span = {}});
}

BoundColumn Qualified(std::string qualifier, std::string name) {
  return BoundColumn{.index = 0,
                     .id = kNoColumnId,
                     .name = std::move(name),
                     .type = LogicalType::kInteger,
                     .qualifier = std::move(qualifier)};
}

ExprPtr Condition(std::string name) {
  return std::make_shared<const Expr>(Expr{.node = ConstantExpr{.value = Constant{}},
                                           .type = LogicalType::kBoolean,
                                           .name = std::move(name)});
}

// One line per join, then its left input and its right input one level deeper.
TEST(ExplainTest, JoinsShowBothInputs) {
  const LogicalNodePtr semi = Node(JoinNode{
      .kind = JoinKind::kSemi,
      .left = ScanOf("u", {1}),
      .right = ScanOf("dir/x.parquet", {1, 9}),
      .keys = {JoinKey{.left = Qualified("u", "i32"), .right = Qualified("x y", "Mixed Case")}},
      .residual = {},
      .build = BuildSide::kRight,
      .span = {}});
  const LogicalNodePtr inner =
      Node(JoinNode{.kind = JoinKind::kInner,
                    .left = ScanOf("t", {0, 1, 2}),
                    .right = semi,
                    .keys = {JoinKey{.left = Qualified("t", "i32"), .right = Qualified("u", "i32")},
                             JoinKey{.left = Qualified("t", "i64"), .right = Qualified("", "i64")}},
                    .residual = {Condition("(t.i16 < u.i16)"), Condition("(t.s <> 'it''s\n')")},
                    .build = BuildSide::kLeft,
                    .span = {}});
  const LogicalPlan plan{
      .root = Node(ProjectNode{.input = inner,
                               .columns = {Qualified("t", "i16"), Qualified("", "i64")},
                               .constants = {},
                               .ids = {},
                               .span = {}}),
      .output = {OutputColumn{.name = "i16", .type = LogicalType::kSmallInt},
                 OutputColumn{.name = "i64", .type = LogicalType::kBigInt}}};
  EXPECT_EQ(Explain(plan),
            "Output: i16:SMALLINT i64:BIGINT\n"
            "Project t.i16, i64\n"
            "  Join INNER build=left keys=[t.i32 = u.i32, t.i64 = i64] "
            "residual=[(t.i16 < u.i16), (t.s <> 'it''s\\x0A')]\n"
            "    Scan table=t source=fake columns=[i16, i32, i64]\n"
            "    Join SEMI build=right keys=[u.i32 = \"x y\".\"Mixed Case\"]\n"
            "      Scan table=u source=fake columns=[i32]\n"
            "      Scan table=dir/x.parquet source=fake columns=[i32, \"Mixed Case\"]\n");
}

TEST(ExplainTest, EveryJoinKind) {
  for (const auto& [kind, line] : {
           std::pair{JoinKind::kInner, "Join INNER build=right keys=[a.x = b.y]"},
           std::pair{JoinKind::kLeft, "Join LEFT build=right keys=[a.x = b.y]"},
           std::pair{JoinKind::kSemi, "Join SEMI build=right keys=[a.x = b.y]"},
           std::pair{JoinKind::kAnti, "Join ANTI build=right keys=[a.x = b.y]"},
           std::pair{JoinKind::kNullAwareAnti, "Join NULL-AWARE ANTI build=right keys=[a.x = b.y]"},
       }) {
    EXPECT_EQ(ExplainNode(JoinNode{
                  .kind = kind,
                  .left = ScanOf("t", {0}),
                  .right = ScanOf("t", {0}),
                  .keys = {JoinKey{.left = Qualified("a", "x"), .right = Qualified("b", "y")}},
                  .residual = {},
                  .build = BuildSide::kRight,
                  .span = {}}),
              line);
  }
  EXPECT_EQ(ExplainNode(JoinNode{.kind = JoinKind::kOneRow,
                                 .left = ScanOf("t", {0}),
                                 .right = ScanOf("t", {0}),
                                 .keys = {},
                                 .residual = {Condition("(x > \"sum(y)\")")},
                                 .build = BuildSide::kRight,
                                 .span = {}}),
            "Join ONE-ROW build=right keys=[] residual=[(x > \"sum(y)\")]");
}

// A qualifier is printed wherever a column is (J2b's binder sets it); quoted like a name.
TEST(ExplainTest, QualifiedColumnNames) {
  const LogicalNodePtr scan = ScanOf("t", {0});
  const BoundColumn a = Qualified("a", "x");
  const BoundColumn quoted = Qualified("Two Words", "from");
  EXPECT_EQ(ExplainNode(FilterNode{
                .input = scan,
                .predicates = {Predicate{.kind = Predicate::Kind::kCompareColumns,
                                         .column = a,
                                         .other = quoted,
                                         .op = CompareOp::kGe},
                               Predicate{.kind = Predicate::Kind::kIsNotNull, .column = quoted}},
                .span = {}}),
            "Filter a.x >= \"Two Words\".from AND \"Two Words\".from IS NOT NULL");
  EXPECT_EQ(ExplainNode(AggregateNode{
                .input = scan,
                .aggregates = {AggregateCall{.kind = AggKind::kSum, .arg = a},
                               AggregateCall{.kind = AggKind::kCountDistinct, .arg = quoted}},
                .span = {}}),
            "Aggregate SUM(a.x), COUNT(DISTINCT \"Two Words\".from)");
  EXPECT_EQ(ExplainNode(GroupAggregateNode{
                .input = scan, .keys = {a, quoted}, .key_ids = {}, .aggregates = {}, .span = {}}),
            "GroupAggregate keys=[a.x, \"Two Words\".from]");
  EXPECT_EQ(ExplainNode(SortNode{
                .input = scan, .keys = {SortKey{.column = a, .descending = true}}, .span = {}}),
            "Sort a.x DESC NULLS LAST");
}

}  // namespace
}  // namespace antb1::plan
