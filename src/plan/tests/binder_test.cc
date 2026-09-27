#include "antb1/plan/binder.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <arrow/api.h>
#include <gtest/gtest.h>

#include "antb1/common/int128.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/sql_status.h"
#include "antb1/sql/parser.h"

#include "plan_test_util.h"

namespace antb1::plan {
namespace {

using testing::BindSql;
using testing::FakeTable;
using testing::MakeCatalog;
using testing::Nth;
using testing::SpanText;

struct ErrorCase {
  std::string_view sql;
  SqlErrorDetail::Kind kind = SqlErrorDetail::Kind::kBind;
  std::string_view span;     // the query text the error points at
  std::string_view message;  // a part of the message
};

void PrintTo(const ErrorCase& c, std::ostream* os) { *os << c.sql; }

class BindErrorTest : public ::testing::TestWithParam<ErrorCase> {};

TEST_P(BindErrorTest, ReportsKindSpanAndMessage) {
  const ErrorCase& c = GetParam();
  const Catalog catalog = MakeCatalog();
  auto plan = BindSql(c.sql, catalog);
  ASSERT_FALSE(plan.ok()) << c.sql;
  const auto detail = GetSqlError(plan.status());
  ASSERT_NE(detail, nullptr) << plan.status().ToString();
  EXPECT_EQ(detail->kind(), c.kind) << plan.status().ToString();
  EXPECT_EQ(SpanText(c.sql, plan.status()), c.span) << plan.status().ToString();
  EXPECT_NE(plan.status().message().find(c.message), std::string::npos) << plan.status().message();
  // kUnsupported is NotImplemented (exit code 4); kBind is Invalid (exit code 1).
  EXPECT_EQ(plan.status().IsNotImplemented(), c.kind == SqlErrorDetail::Kind::kUnsupported);
}

constexpr auto kBind = SqlErrorDetail::Kind::kBind;
constexpr auto kUnsupported = SqlErrorDetail::Kind::kUnsupported;

INSTANTIATE_TEST_SUITE_P(
    Binder, BindErrorTest,
    ::testing::Values(
        // [NOT] IN: each value is typed like `column = value`.
        ErrorCase{"SELECT COUNT(*) FROM t WHERE i16 IN (1, '2')", kBind, "'2'",
                  "cannot compare SMALLINT column 'i16' with a string"},
        ErrorCase{"SELECT COUNT(*) FROM t WHERE s NOT IN ('a', 1)", kBind, "1",
                  "cannot compare VARCHAR column 's' with a number"},
        ErrorCase{"SELECT COUNT(*) FROM t WHERE dt IN (DATE '2013-02-30')", kBind,
                  "DATE '2013-02-30'", "invalid date"},
        // [NOT] LIKE: a VARCHAR column and a string pattern (DuckDB rejects LIKE on numbers).
        ErrorCase{"SELECT COUNT(*) FROM t WHERE i16 LIKE '1%'", kBind, "i16",
                  "LIKE needs a VARCHAR column, but 'i16' is SMALLINT"},
        ErrorCase{"SELECT COUNT(*) FROM t WHERE dt NOT LIKE '2013%'", kBind, "dt",
                  "NOT LIKE needs a VARCHAR column"},
        ErrorCase{"SELECT COUNT(*) FROM t WHERE s LIKE 5", kBind, "5",
                  "the pattern of LIKE must be a string literal"},
        ErrorCase{"SELECT COUNT(*) FROM t WHERE nope LIKE 'x'", kBind, "nope", "does not exist"},
        // COUNT(DISTINCT col): the column must exist and have an engine type; it is an aggregate.
        ErrorCase{"SELECT COUNT(DISTINCT nope) FROM t", kBind, "nope",
                  "column 'nope' does not exist"},
        ErrorCase{"SELECT COUNT(DISTINCT bad) FROM t", kUnsupported, "bad", "unsupported type"},
        ErrorCase{"SELECT i16, COUNT(DISTINCT i32) FROM t", kBind, "i16",
                  "column 'i16' must be inside an aggregate function"},
        ErrorCase{"SELECT i16 FROM t ORDER BY COUNT(DISTINCT i32)", kBind, "i16",
                  "must be inside an aggregate function"},
        // ORDER BY: a select alias (the last one), else a table column; an aggregate makes the
        // query aggregate; a grouped query orders by keys and aggregates only.
        ErrorCase{"SELECT i16 FROM t ORDER BY nope", kBind, "nope", "column 'nope' does not exist"},
        ErrorCase{"SELECT COUNT(*) FROM t ORDER BY SUM(s)", kBind, "SUM(s)", "SUM needs a numeric"},
        ErrorCase{"SELECT i16 FROM t ORDER BY COUNT(*)", kBind, "i16",
                  "column 'i16' must be inside an aggregate function"},
        ErrorCase{"SELECT * FROM ok ORDER BY MAX(i16)", kBind, "*",
                  "column 'i16' must be inside an aggregate function"},
        ErrorCase{"SELECT COUNT(*) FROM t ORDER BY i16", kBind, "i16",
                  "column 'i16' must appear in the GROUP BY clause or be inside an aggregate"},
        ErrorCase{"SELECT i16 AS x, COUNT(*) FROM t GROUP BY i16 ORDER BY i32 DESC", kBind, "i32",
                  "column 'i32' must appear in the GROUP BY clause"},
        ErrorCase{"SELECT s AS i32, i16 AS i32 FROM t GROUP BY s, i16 ORDER BY I32, i64", kBind,
                  "i64", "column 'i64' must appear in the GROUP BY clause"},
        ErrorCase{"SELECT i16 FROM t GROUP BY i16 ORDER BY COUNT(bad)", kUnsupported, "bad",
                  "unsupported type"},
        // GROUP BY: keys are table columns or aliases of plain select columns; every plain select
        // column must be a key.
        ErrorCase{"SELECT i16, i32, COUNT(*) FROM t GROUP BY i16", kBind, "i32",
                  "column 'i32' must appear in the GROUP BY clause"},
        ErrorCase{"SELECT * FROM ok GROUP BY i16", kBind, "*",
                  "column 's' must appear in the GROUP BY clause"},
        ErrorCase{"SELECT COUNT(*) FROM t GROUP BY nope", kBind, "nope",
                  "column 'nope' does not exist"},
        ErrorCase{"SELECT SUM(i16) AS total FROM t GROUP BY total", kBind, "total",
                  "GROUP BY cannot refer to the aggregate 'total'"},
        // A repeated alias means its last select item (DuckDB).
        ErrorCase{"SELECT s AS k, COUNT(*) AS k FROM t GROUP BY k", kBind, "k",
                  "GROUP BY cannot refer to the aggregate 'k'"},
        ErrorCase{"SELECT nope FROM missing GROUP BY nope", kBind, "missing",
                  "table 'missing' does not exist"},
        ErrorCase{"SELECT i16 FROM t WHERE nope = 1 GROUP BY nope", kBind, "nope",
                  "column 'nope' does not exist"},
        // Tables.
        ErrorCase{"SELECT COUNT(*) FROM nope", kBind, "nope", "table 'nope' does not exist"},
        ErrorCase{"SELECT COUNT(*) FROM \"nope\"", kBind, "\"nope\"", "does not exist"},
        ErrorCase{"SELECT COUNT(*) FROM 'x.parquet'", kUnsupported, "'x.parquet'", "not enabled"},
        // Columns: unknown, ambiguous, of an unsupported type; in every clause.
        ErrorCase{"SELECT nope FROM t", kBind, "nope", "column 'nope' does not exist"},
        ErrorCase{"SELECT i16, \"No pe\" FROM t", kBind, "\"No pe\"",
                  "column 'No pe' does not exist"},
        ErrorCase{"SELECT SUM(nope) FROM t", kBind, "nope", "does not exist"},
        ErrorCase{"SELECT COUNT(*) FROM t WHERE nope = 1", kBind, "nope", "does not exist"},
        ErrorCase{"SELECT a FROM dup", kBind, "a", "ambiguous"},
        ErrorCase{"SELECT COUNT(\"A\") FROM dup", kBind, "\"A\"", "'a' and 'A'"},
        ErrorCase{"SELECT COUNT(*) FROM dup WHERE A > 1", kBind, "A", "differ only in case"},
        ErrorCase{"SELECT bad FROM t", kUnsupported, "bad",
                  "column 'bad' has the unsupported type"},
        ErrorCase{"SELECT COUNT(bad) FROM t", kUnsupported, "bad", "unsupported type"},
        ErrorCase{"SELECT COUNT(*) FROM t WHERE bad = 1", kUnsupported, "bad", "unsupported"},
        ErrorCase{"SELECT * FROM t", kUnsupported, "*", "column 'bad' has the unsupported type"},
        ErrorCase{"SELECT  *  FROM u LIMIT 1", kUnsupported, "*", "'bad'"},
        // Aggregates mixed with plain columns (either order); SUM and AVG of non-numbers.
        ErrorCase{"SELECT i16, COUNT(*) FROM t", kBind, "i16",
                  "column 'i16' must be inside an aggregate function"},
        ErrorCase{"SELECT COUNT(*), I16 AS x, i32 FROM t", kBind, "I16", "no GROUP BY"},
        ErrorCase{"SELECT SUM(s) FROM t", kBind, "SUM(s)",
                  "SUM needs a numeric column, but 's' is VARCHAR"},
        ErrorCase{"SELECT sum ( DT ) FROM t", kBind, "sum ( DT )", "'dt' is DATE"},
        ErrorCase{"SELECT COUNT(*), AVG(s) AS a FROM t", kBind, "AVG(s)", "AVG needs a numeric"},
        ErrorCase{"SELECT AVG(dt) FROM t", kBind, "AVG(dt)", "DATE"},
        // Literals of the wrong type: the span is the literal.
        ErrorCase{"SELECT COUNT(*) FROM t WHERE i16 = '1'", kBind, "'1'",
                  "cannot compare SMALLINT column 'i16' with a string; write a number"},
        ErrorCase{"SELECT COUNT(*) FROM t WHERE i64 > DATE '2013-07-01'", kBind,
                  "DATE '2013-07-01'", "with a DATE literal"},
        ErrorCase{"SELECT COUNT(*) FROM t WHERE '2' < u16", kBind, "'2'", "USMALLINT"},
        ErrorCase{"SELECT COUNT(*) FROM t WHERE h = '1'", kBind, "'1'", "HUGEINT"},
        ErrorCase{"SELECT COUNT(*) FROM t WHERE d = '1.5'", kBind, "'1.5'",
                  "cannot compare DOUBLE column 'd' with a string"},
        ErrorCase{"SELECT COUNT(*) FROM t WHERE d <> DATE '2013-07-01'", kBind, "DATE '2013-07-01'",
                  "DATE literal"},
        ErrorCase{"SELECT COUNT(*) FROM t WHERE s = 1", kBind, "1",
                  "cannot compare VARCHAR column 's' with a number; write a string literal"},
        ErrorCase{"SELECT COUNT(*) FROM t WHERE s = -1.5", kBind, "-1.5", "a number"},
        ErrorCase{"SELECT COUNT(*) FROM t WHERE s = DATE '2013-07-01'", kBind, "DATE '2013-07-01'",
                  "with a DATE literal"},
        ErrorCase{"SELECT COUNT(*) FROM t WHERE dt = 15887", kBind, "15887",
                  "cannot compare DATE column 'dt' with a number; write a date as DATE"},
        ErrorCase{"SELECT COUNT(*) FROM t WHERE dt > 1.5", kBind, "1.5", "a number"},
        ErrorCase{"SELECT COUNT(*) FROM t WHERE dt = '2013-7-1'", kBind, "'2013-7-1'",
                  "invalid date '2013-7-1': expected YYYY-MM-DD"},
        ErrorCase{"SELECT COUNT(*) FROM t WHERE dt = DATE '2013-02-29'", kBind, "DATE '2013-02-29'",
                  "invalid date"},
        ErrorCase{"SELECT COUNT(*) FROM t WHERE dt = ''", kBind, "''", "invalid date ''"},
        ErrorCase{"SELECT COUNT(*) FROM t WHERE dt = '2013-07-01 00:00:00'", kBind,
                  "'2013-07-01 00:00:00'", "expected YYYY-MM-DD"},
        // Long literals are clipped to 32 bytes in the message.
        ErrorCase{"SELECT COUNT(*) FROM t WHERE dt = '0123456789012345678901234567890123456789'",
                  kBind, "'0123456789012345678901234567890123456789'",
                  "'01234567890123456789012345678901...'"},
        // The first error in query order wins (after the table): select list, WHERE, LIMIT.
        ErrorCase{"SELECT nope FROM t WHERE s = 1", kBind, "nope", "does not exist"},
        ErrorCase{"SELECT i16 FROM t WHERE s = 1 AND nope = 2", kBind, "1", "VARCHAR"},
        ErrorCase{"SELECT COUNT(*) FROM t WHERE i16 = 1 AND nope = 2", kBind, "nope",
                  "does not exist"}));

TEST(BinderTest, TablesMatchCaseInsensitively) {
  const Catalog catalog = MakeCatalog();
  for (const char* sql :
       {"SELECT COUNT(*) FROM T", "SELECT COUNT(*) FROM \"T\"", "select count(*) from \"t\""}) {
    auto plan = BindSql(sql, catalog);
    ASSERT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    const auto& scan = std::get<ScanNode>(Nth(*plan, 1));
    EXPECT_EQ(scan.table, catalog.Find("t")) << sql;
  }
}

TEST(BinderTest, CountStarIsAnAggregateOverAScanOfEveryField) {
  const Catalog catalog = MakeCatalog();
  constexpr std::string_view kSql = "SELECT COUNT(*) FROM t";
  auto plan = BindSql(kSql, catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  ASSERT_EQ(plan->output.size(), 1U);
  EXPECT_EQ(plan->output[0].name, "count_star()");
  EXPECT_EQ(plan->output[0].type, LogicalType::kBigInt);
  const auto& agg = std::get<AggregateNode>(Nth(*plan, 0));
  ASSERT_EQ(agg.aggregates.size(), 1U);
  EXPECT_EQ(agg.aggregates[0].kind, AggKind::kCountStar);
  EXPECT_FALSE(agg.aggregates[0].arg.has_value());
  EXPECT_EQ(agg.aggregates[0].type, LogicalType::kBigInt);
  EXPECT_EQ(kSql.substr(agg.span.offset, agg.span.length), "COUNT(*)");
  const auto& scan = std::get<ScanNode>(Nth(*plan, 1));
  EXPECT_EQ(scan.table_name, "t");
  EXPECT_EQ(scan.fields, (std::vector<int>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10}));
  EXPECT_EQ(kSql.substr(scan.span.offset, scan.span.length), "t");
}

// GroupAggregate outputs the keys, then the aggregates; the Project on top restores the select
// order, so plain columns point at keys and aggregates follow the keys.
TEST(BinderTest, GroupByBuildsAGroupAggregateUnderAProject) {
  const Catalog catalog = MakeCatalog();
  constexpr std::string_view kSql =
      "SELECT COUNT(*) AS n, s, SUM(i32) FROM t WHERE i16 > 0 GROUP BY s, i16 LIMIT 3";
  auto plan = BindSql(kSql, catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  ASSERT_EQ(plan->output.size(), 3U);
  EXPECT_EQ(plan->output[0].name, "n");
  EXPECT_EQ(plan->output[1].name, "s");
  EXPECT_EQ(plan->output[1].type, LogicalType::kVarchar);
  EXPECT_EQ(plan->output[2].name, "sum(i32)");
  EXPECT_EQ(plan->output[2].type, LogicalType::kHugeInt);
  EXPECT_EQ(std::get<LimitNode>(Nth(*plan, 0)).limit, 3);
  const auto& project = std::get<ProjectNode>(Nth(*plan, 1));
  ASSERT_EQ(project.columns.size(), 3U);
  EXPECT_EQ(project.columns[0].index, 2);  // COUNT(*): after the two keys
  EXPECT_EQ(project.columns[1].index, 0);  // s: the first key
  EXPECT_EQ(project.columns[2].index, 3);  // SUM(i32)
  const auto& group = std::get<GroupAggregateNode>(Nth(*plan, 2));
  ASSERT_EQ(group.keys.size(), 2U);
  EXPECT_EQ(group.keys[0].name, "s");
  EXPECT_EQ(group.keys[0].index, 6);
  EXPECT_EQ(group.keys[1].name, "i16");
  EXPECT_EQ(group.keys[1].index, 0);
  ASSERT_EQ(group.aggregates.size(), 2U);
  EXPECT_EQ(group.aggregates[0].kind, AggKind::kCountStar);
  EXPECT_EQ(group.aggregates[1].kind, AggKind::kSum);
  EXPECT_EQ(kSql.substr(group.span.offset, group.span.length), "GROUP BY s, i16");
  EXPECT_TRUE(std::holds_alternative<FilterNode>(Nth(*plan, 3)));
  EXPECT_TRUE(std::holds_alternative<ScanNode>(Nth(*plan, 4)));
}

// A GROUP BY name is a table column first, else the alias of a plain select column (DuckDB);
// repeated keys, also spelled differently, are one key; a key need not be selected.
TEST(BinderTest, GroupByAliasesDuplicatesAndUnselectedKeys) {
  const Catalog catalog = MakeCatalog();
  auto alias = BindSql("SELECT s AS k, COUNT(*) FROM t GROUP BY k, s, \"S\"", catalog);
  ASSERT_TRUE(alias.ok()) << alias.status().ToString();
  const auto& group = std::get<GroupAggregateNode>(Nth(*alias, 1));
  ASSERT_EQ(group.keys.size(), 1U);
  EXPECT_EQ(group.keys[0].index, 6);
  // An alias that is also a column name means the column.
  auto shadow = BindSql("SELECT i16 AS i32, COUNT(*) FROM t GROUP BY i32, i16", catalog);
  ASSERT_TRUE(shadow.ok()) << shadow.status().ToString();
  EXPECT_EQ(std::get<GroupAggregateNode>(Nth(*shadow, 1)).keys.size(), 2U);
  auto unselected = BindSql("SELECT MAX(d) FROM t GROUP BY dt, u16", catalog);
  ASSERT_TRUE(unselected.ok()) << unselected.status().ToString();
  const auto& project = std::get<ProjectNode>(Nth(*unselected, 0));
  ASSERT_EQ(project.columns.size(), 1U);
  EXPECT_EQ(project.columns[0].index, 2);
  auto last_alias = BindSql("SELECT COUNT(*) AS k, s AS k, i16 AS k FROM t GROUP BY k, s", catalog);
  // k is i16 (the last item named k), so both plain columns are keys.
  ASSERT_TRUE(last_alias.ok()) << last_alias.status().ToString();
  EXPECT_EQ(std::get<GroupAggregateNode>(Nth(*last_alias, 1)).keys.size(), 2U);
  auto last = BindSql("SELECT COUNT(*) AS k, s AS k FROM t GROUP BY k", catalog);
  ASSERT_TRUE(last.ok()) << last.status().ToString();
  EXPECT_EQ(std::get<GroupAggregateNode>(Nth(*last, 1)).keys[0].index, 6);
  auto keys_only = BindSql("SELECT s, i16 FROM t GROUP BY i16, s", catalog);
  ASSERT_TRUE(keys_only.ok()) << keys_only.status().ToString();
  EXPECT_TRUE(std::get<GroupAggregateNode>(Nth(*keys_only, 1)).aggregates.empty());
  EXPECT_EQ(std::get<ProjectNode>(Nth(*keys_only, 0)).columns[0].index, 1);
}

// A projection sorts the table's columns below the Project, so it can order by a column it does
// not select; a select alias comes before a column of the same name (the last alias wins), and a
// column already ordered by is not a key again.
TEST(BinderTest, OrderByInAProjection) {
  const Catalog catalog = MakeCatalog();
  constexpr std::string_view kSql =
      "SELECT i16 AS i32, s FROM t ORDER BY i32 DESC, d NULLS FIRST, I16, s LIMIT 5 OFFSET 2";
  auto plan = BindSql(kSql, catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  const auto& limit = std::get<LimitNode>(Nth(*plan, 0));
  EXPECT_EQ(limit.limit, 5);
  EXPECT_EQ(limit.offset, 2);
  EXPECT_EQ(kSql.substr(limit.span.offset, limit.span.length), "LIMIT 5 OFFSET 2");
  EXPECT_TRUE(std::holds_alternative<ProjectNode>(Nth(*plan, 1)));
  const auto& sort = std::get<SortNode>(Nth(*plan, 2));
  EXPECT_EQ(kSql.substr(sort.span.offset, sort.span.length),
            "ORDER BY i32 DESC, d NULLS FIRST, I16, s");
  ASSERT_EQ(sort.keys.size(), 3U);  // I16 repeats i32 (the alias of i16)
  EXPECT_EQ(sort.keys[0].column.index, 0);
  EXPECT_TRUE(sort.keys[0].descending);
  EXPECT_FALSE(sort.keys[0].nulls_first);
  EXPECT_EQ(sort.keys[1].column.index, 5);
  EXPECT_EQ(sort.keys[1].column.type, LogicalType::kDouble);
  EXPECT_FALSE(sort.keys[1].descending);
  EXPECT_TRUE(sort.keys[1].nulls_first);
  EXPECT_EQ(sort.keys[2].column.index, 6);
  EXPECT_TRUE(std::holds_alternative<ScanNode>(Nth(*plan, 3)));

  auto last_alias = BindSql("SELECT i16 AS x, i32 AS X FROM t ORDER BY x", catalog);
  ASSERT_TRUE(last_alias.ok()) << last_alias.status().ToString();
  EXPECT_EQ(std::get<SortNode>(Nth(*last_alias, 1)).keys[0].column.index, 1);

  auto offset = BindSql("SELECT * FROM ok OFFSET 3", catalog);
  ASSERT_TRUE(offset.ok()) << offset.status().ToString();
  EXPECT_FALSE(std::get<LimitNode>(Nth(*offset, 0)).limit.has_value());
  EXPECT_EQ(std::get<LimitNode>(Nth(*offset, 0)).offset, 3);
  auto no_offset = BindSql("SELECT * FROM ok OFFSET 0", catalog);
  ASSERT_TRUE(no_offset.ok()) << no_offset.status().ToString();
  EXPECT_TRUE(std::holds_alternative<ProjectNode>(Nth(*no_offset, 0)));
}

// A grouped query sorts the GroupAggregate's output (keys, then aggregates): an ORDER BY aggregate
// equal to a select one reuses it, another one is computed as a hidden aggregate that the Project
// drops.
TEST(BinderTest, OrderByInAGroupedQuery) {
  const Catalog catalog = MakeCatalog();
  auto plan = BindSql(
      "SELECT s AS k, COUNT(*) AS n FROM t GROUP BY s, i16 "
      "ORDER BY count(*), MAX(d) DESC, n, i16, k LIMIT 10",
      catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  ASSERT_EQ(plan->output.size(), 2U);
  EXPECT_EQ(std::get<LimitNode>(Nth(*plan, 0)).limit, 10);
  const auto& project = std::get<ProjectNode>(Nth(*plan, 1));
  ASSERT_EQ(project.columns.size(), 2U);
  EXPECT_EQ(project.columns[0].index, 0);
  EXPECT_EQ(project.columns[1].index, 2);
  const auto& sort = std::get<SortNode>(Nth(*plan, 2));
  ASSERT_EQ(sort.keys.size(), 4U);  // n repeats count(*)
  EXPECT_EQ(sort.keys[0].column.index, 2);
  EXPECT_EQ(sort.keys[1].column.index, 3);
  EXPECT_EQ(sort.keys[1].column.name, "max(d)");
  EXPECT_EQ(sort.keys[1].column.type, LogicalType::kDouble);
  EXPECT_TRUE(sort.keys[1].descending);
  EXPECT_EQ(sort.keys[2].column.index, 1);  // i16: the second key
  EXPECT_EQ(sort.keys[3].column.index, 0);  // k: s, the first key
  const auto& group = std::get<GroupAggregateNode>(Nth(*plan, 3));
  ASSERT_EQ(group.aggregates.size(), 2U);
  EXPECT_EQ(group.aggregates[1].kind, AggKind::kMax);
}

// Without GROUP BY an aggregate query has one row: ORDER BY is checked, but needs no Sort.
TEST(BinderTest, OrderByInAGlobalAggregateNeedsNoSort) {
  const Catalog catalog = MakeCatalog();
  auto plan = BindSql("SELECT COUNT(*) AS c FROM t ORDER BY c, MAX(s) OFFSET 1", catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  EXPECT_EQ(std::get<LimitNode>(Nth(*plan, 0)).offset, 1);
  const auto& aggregate = std::get<AggregateNode>(Nth(*plan, 1));
  EXPECT_EQ(aggregate.aggregates.size(), 1U);
}

// COUNT(DISTINCT col) is its own aggregate kind (BIGINT), named like DuckDB; in ORDER BY it reuses
// an equal select aggregate, and COUNT(col) is not equal to it.
TEST(BinderTest, CountDistinct) {
  const Catalog catalog = MakeCatalog();
  auto plan = BindSql(
      "SELECT s, COUNT(DISTINCT \"Mixed Case\"), count(distinct I32) AS n FROM t GROUP BY s "
      "ORDER BY COUNT(DISTINCT i32) DESC, COUNT(i32)",
      catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  ASSERT_EQ(plan->output.size(), 3U);
  EXPECT_EQ(plan->output[1].name, "count(DISTINCT \"Mixed Case\")");
  EXPECT_EQ(plan->output[1].type, LogicalType::kBigInt);
  EXPECT_EQ(plan->output[2].name, "n");
  const auto& sort = std::get<SortNode>(Nth(*plan, 1));
  ASSERT_EQ(sort.keys.size(), 2U);
  EXPECT_EQ(sort.keys[0].column.index, 2);  // the select's COUNT(DISTINCT i32)
  EXPECT_EQ(sort.keys[1].column.index, 3);  // COUNT(i32): hidden
  const auto& group = std::get<GroupAggregateNode>(Nth(*plan, 2));
  ASSERT_EQ(group.aggregates.size(), 3U);
  EXPECT_EQ(group.aggregates[0].kind, AggKind::kCountDistinct);
  EXPECT_EQ(group.aggregates[1].kind, AggKind::kCountDistinct);
  EXPECT_EQ(group.aggregates[2].kind, AggKind::kCount);
  auto name = BindSql("SELECT COUNT(DISTINCT \"i32\") FROM t", catalog);
  ASSERT_TRUE(name.ok()) << name.status().ToString();
  EXPECT_EQ(name->output[0].name, "count(DISTINCT i32)");
}

// [NOT] IN binds each value like `column = value`: values no column value can equal are dropped,
// and without values IN is FALSE and NOT IN is IS NOT NULL.
TEST(BinderTest, In) {
  const Catalog catalog = MakeCatalog();
  auto plan = BindSql(
      "SELECT COUNT(*) FROM t WHERE i16 IN (1, 2.5, 40000, -3.0) AND s NOT IN ('a', 'b') AND "
      "u16 NOT IN (-1, 1.5) AND i32 IN (1.5) AND d IN (0.1)",
      catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  const auto& predicates = std::get<FilterNode>(Nth(*plan, 1)).predicates;
  ASSERT_EQ(predicates.size(), 5U);
  EXPECT_EQ(predicates[0].kind, Predicate::Kind::kIn);
  ASSERT_EQ(predicates[0].values.size(), 2U);
  EXPECT_EQ(std::get<Int128>(predicates[0].values[0].value), Int128{1});
  EXPECT_EQ(std::get<Int128>(predicates[0].values[1].value), Int128{-3});
  EXPECT_EQ(predicates[0].values[1].type, LogicalType::kSmallInt);
  EXPECT_EQ(predicates[1].kind, Predicate::Kind::kNotIn);
  EXPECT_EQ(std::get<std::string>(predicates[1].values[1].value), "b");
  EXPECT_EQ(predicates[2].kind, Predicate::Kind::kIsNotNull);
  EXPECT_EQ(predicates[3].kind, Predicate::Kind::kFalse);
  EXPECT_FALSE(predicates[3].column.has_value());
  EXPECT_EQ(predicates[4].kind, Predicate::Kind::kIn);
  EXPECT_EQ(std::get<double>(predicates[4].values[0].value), 0.1);

  // DuckDB types the whole list as DOUBLE when one number is DOUBLE-typed (an exponent): every
  // integer is then the nearest double (2^53 + 1 becomes 2^53), folded exactly as in D7.
  auto exact = BindSql("SELECT COUNT(*) FROM t WHERE i64 IN (9007199254740993, 1)", catalog);
  ASSERT_TRUE(exact.ok()) << exact.status().ToString();
  EXPECT_EQ(std::get<Int128>(std::get<FilterNode>(Nth(*exact, 1)).predicates[0].values[0].value),
            Int128{9007199254740993});
  auto doubled = BindSql("SELECT COUNT(*) FROM t WHERE i64 IN (9007199254740993, 1e0)", catalog);
  ASSERT_TRUE(doubled.ok()) << doubled.status().ToString();
  const auto& values = std::get<FilterNode>(Nth(*doubled, 1)).predicates[0].values;
  ASSERT_EQ(values.size(), 2U);
  EXPECT_EQ(std::get<Int128>(values[0].value), Int128{9007199254740992});
  EXPECT_EQ(std::get<Int128>(values[1].value), Int128{1});
}

// [NOT] LIKE binds to its own predicate kinds with the pattern as a VARCHAR constant; a pattern of
// only % folds (LIKE: IS NOT NULL, NOT LIKE: FALSE).
TEST(BinderTest, Like) {
  const Catalog catalog = MakeCatalog();
  auto plan = BindSql(
      "SELECT COUNT(*) FROM t WHERE s LIKE '%a_%' AND s NOT LIKE '' AND s LIKE '%%' AND "
      "\"Mixed Case\" > 1 AND s NOT LIKE '%'",
      catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  const auto& predicates = std::get<FilterNode>(Nth(*plan, 1)).predicates;
  ASSERT_EQ(predicates.size(), 5U);
  EXPECT_EQ(predicates[0].kind, Predicate::Kind::kLike);
  EXPECT_EQ(std::get<std::string>(predicates[0].constant.value), "%a_%");
  EXPECT_EQ(predicates[0].constant.type, LogicalType::kVarchar);
  EXPECT_EQ(predicates[0].column.value_or(BoundColumn{}).index, 6);
  EXPECT_EQ(predicates[1].kind, Predicate::Kind::kNotLike);
  EXPECT_EQ(std::get<std::string>(predicates[1].constant.value), "");
  EXPECT_EQ(predicates[2].kind, Predicate::Kind::kIsNotNull);
  EXPECT_EQ(predicates[3].kind, Predicate::Kind::kCompare);
  EXPECT_EQ(predicates[4].kind, Predicate::Kind::kFalse);
  EXPECT_FALSE(predicates[4].column.has_value());
}

TEST(BinderTest, SelectStarProjectsEveryColumn) {
  const Catalog catalog = MakeCatalog();
  constexpr std::string_view kSql = "SELECT * FROM OK LIMIT 5";
  auto plan = BindSql(kSql, catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  ASSERT_EQ(plan->output.size(), 2U);
  EXPECT_EQ(plan->output[0].name, "i16");
  EXPECT_EQ(plan->output[0].type, LogicalType::kSmallInt);
  EXPECT_EQ(plan->output[1].name, "s");
  EXPECT_EQ(plan->output[1].type, LogicalType::kVarchar);
  const auto& limit = std::get<LimitNode>(Nth(*plan, 0));
  EXPECT_EQ(limit.limit, 5);
  EXPECT_EQ(kSql.substr(limit.span.offset, limit.span.length), "LIMIT 5");
  const auto& project = std::get<ProjectNode>(Nth(*plan, 1));
  EXPECT_EQ(kSql.substr(project.span.offset, project.span.length), "*");
  ASSERT_EQ(project.columns.size(), 2U);
  EXPECT_EQ(project.columns[0].index, 0);
  EXPECT_EQ(project.columns[1].index, 1);
  EXPECT_EQ(project.columns[1].name, "s");
  EXPECT_EQ(std::get<ScanNode>(Nth(*plan, 2)).table_name, "OK");
}

TEST(BinderTest, ColumnsResolveCaseInsensitivelyAndKeepTheirDeclaredNames) {
  const Catalog catalog = MakeCatalog();
  constexpr std::string_view kSql =
      R"(SELECT I16, "S", dT, "mixed case", "FROM", u16 AS "Alias", H x FROM t)";
  auto plan = BindSql(kSql, catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  const std::vector<std::pair<std::string, LogicalType>> expected = {
      {"i16", LogicalType::kSmallInt}, {"s", LogicalType::kVarchar},
      {"dt", LogicalType::kDate},      {"Mixed Case", LogicalType::kInteger},
      {"from", LogicalType::kInteger}, {"Alias", LogicalType::kUSmallInt},
      {"x", LogicalType::kHugeInt}};
  ASSERT_EQ(plan->output.size(), expected.size());
  const auto& project = std::get<ProjectNode>(Nth(*plan, 0));
  EXPECT_EQ(kSql.substr(project.span.offset, project.span.length),
            R"(I16, "S", dT, "mixed case", "FROM", u16 AS "Alias", H x)");
  const std::vector<int> fields = {0, 6, 7, 9, 10, 3, 4};
  ASSERT_EQ(project.columns.size(), expected.size());
  for (std::size_t i = 0; i < expected.size(); ++i) {
    EXPECT_EQ(plan->output[i].name, expected[i].first) << i;
    EXPECT_EQ(plan->output[i].type, expected[i].second) << i;
    EXPECT_EQ(project.columns[i].index, fields[i]) << i;
    EXPECT_EQ(project.columns[i].type, expected[i].second) << i;
  }
  EXPECT_EQ(project.columns[3].name, "Mixed Case");
  EXPECT_TRUE(std::holds_alternative<ScanNode>(Nth(*plan, 1)));
}

TEST(BinderTest, AggregateResultTypes) {
  const Catalog catalog = MakeCatalog();
  const std::vector<std::pair<std::string_view, LogicalType>> cases = {
      {"COUNT(*)", LogicalType::kBigInt},   {"COUNT(i16)", LogicalType::kBigInt},
      {"COUNT(s)", LogicalType::kBigInt},   {"COUNT(dt)", LogicalType::kBigInt},
      {"COUNT(h)", LogicalType::kBigInt},   {"SUM(i16)", LogicalType::kHugeInt},
      {"SUM(i32)", LogicalType::kHugeInt},  {"SUM(i64)", LogicalType::kHugeInt},
      {"SUM(u16)", LogicalType::kHugeInt},  {"SUM(h)", LogicalType::kHugeInt},
      {"SUM(d)", LogicalType::kDouble},     {"AVG(i16)", LogicalType::kDouble},
      {"AVG(i64)", LogicalType::kDouble},   {"AVG(u16)", LogicalType::kDouble},
      {"AVG(h)", LogicalType::kDouble},     {"AVG(d)", LogicalType::kDouble},
      {"MIN(i16)", LogicalType::kSmallInt}, {"MAX(i32)", LogicalType::kInteger},
      {"MIN(i64)", LogicalType::kBigInt},   {"MAX(u16)", LogicalType::kUSmallInt},
      {"MIN(h)", LogicalType::kHugeInt},    {"MAX(d)", LogicalType::kDouble},
      {"MIN(s)", LogicalType::kVarchar},    {"MAX(dt)", LogicalType::kDate},
  };
  for (const auto& [call, type] : cases) {
    const std::string sql = "SELECT " + std::string(call) + " FROM t";
    auto plan = BindSql(sql, catalog);
    ASSERT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    EXPECT_EQ(plan->output.at(0).type, type) << sql;
    const auto& agg = std::get<AggregateNode>(Nth(*plan, 0));
    EXPECT_EQ(agg.aggregates.at(0).type, type) << sql;
  }
}

TEST(BinderTest, AggregateCallsAndDuckDbResultNames) {
  const Catalog catalog = MakeCatalog();
  constexpr std::string_view kSql =
      R"(SELECT count(*), COUNT(I16), Sum(i32), avg("i64"), MIN("Mixed Case"), MAX("FROM"),
                COUNT(*) AS n, SUM(d) total, max(dt) AS "Last Day" FROM t)";
  auto plan = BindSql(kSql, catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  const std::vector<std::string> names = {
      "count_star()",   "count(I16)", "sum(i32)", "avg(i64)", R"(min("Mixed Case"))",
      R"(max("FROM"))", "n",          "total",    "Last Day"};
  ASSERT_EQ(plan->output.size(), names.size());
  for (std::size_t i = 0; i < names.size(); ++i) {
    EXPECT_EQ(plan->output[i].name, names[i]) << i;
  }
  const auto& agg = std::get<AggregateNode>(Nth(*plan, 0));
  const std::vector<AggKind> kinds = {AggKind::kCountStar, AggKind::kCount, AggKind::kSum,
                                      AggKind::kAvg,       AggKind::kMin,   AggKind::kMax,
                                      AggKind::kCountStar, AggKind::kSum,   AggKind::kMax};
  const std::vector<std::optional<int>> args = {std::nullopt, 0, 1, 2, 9, 10, std::nullopt, 5, 7};
  ASSERT_EQ(agg.aggregates.size(), kinds.size());
  for (std::size_t i = 0; i < kinds.size(); ++i) {
    EXPECT_EQ(agg.aggregates[i].kind, kinds[i]) << i;
    EXPECT_EQ(agg.aggregates[i].arg.has_value(), args[i].has_value()) << i;
    EXPECT_EQ(agg.aggregates[i].arg.value_or(BoundColumn{.index = -1}).index, args[i].value_or(-1))
        << i;
  }
  const SourceSpan second = agg.aggregates[1].span;
  EXPECT_EQ(kSql.substr(second.offset, second.length), "COUNT(I16)");
  EXPECT_EQ(agg.aggregates[4].arg.value_or(BoundColumn{}).name, "Mixed Case");
}

TEST(BinderTest, WhereBuildsAFilterUnderTheSelectList) {
  const Catalog catalog = MakeCatalog();
  constexpr std::string_view kSql =
      "SELECT i16, s FROM t WHERE i32 >= -7 AND 'abc' < s AND dt = DATE '2022-01-08' "
      "AND d <> 0.5 AND h < 12 LIMIT 0";
  auto plan = BindSql(kSql, catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  EXPECT_EQ(std::get<LimitNode>(Nth(*plan, 0)).limit, 0);
  EXPECT_TRUE(std::holds_alternative<ProjectNode>(Nth(*plan, 1)));
  const auto& filter = std::get<FilterNode>(Nth(*plan, 2));
  EXPECT_EQ(kSql.substr(filter.span.offset, filter.span.length),
            "i32 >= -7 AND 'abc' < s AND dt = DATE '2022-01-08' AND d <> 0.5 AND h < 12");
  ASSERT_EQ(filter.predicates.size(), 5U);
  const auto check = [&](std::size_t i, int index, CompareOp op, LogicalType type,
                         std::string_view span) {
    const Predicate& p = filter.predicates[i];
    EXPECT_EQ(p.kind, Predicate::Kind::kCompare) << i;
    EXPECT_EQ(p.column.value_or(BoundColumn{.index = -1}).index, index) << i;
    EXPECT_EQ(p.column.value_or(BoundColumn{}).type, type) << i;
    EXPECT_EQ(p.op, op) << i;
    EXPECT_EQ(p.constant.type, type) << i;
    EXPECT_EQ(kSql.substr(p.span.offset, p.span.length), span) << i;
  };
  check(0, 1, CompareOp::kGe, LogicalType::kInteger, "i32 >= -7");
  check(1, 6, CompareOp::kGt, LogicalType::kVarchar, "'abc' < s");  // literal first, mirrored
  check(2, 7, CompareOp::kEq, LogicalType::kDate, "dt = DATE '2022-01-08'");
  check(3, 5, CompareOp::kNe, LogicalType::kDouble, "d <> 0.5");
  check(4, 4, CompareOp::kLt, LogicalType::kHugeInt, "h < 12");
  EXPECT_EQ(std::get<Int128>(filter.predicates[0].constant.value), -7);
  EXPECT_EQ(std::get<std::string>(filter.predicates[1].constant.value), "abc");
  EXPECT_EQ(std::get<Int128>(filter.predicates[2].constant.value), 19000);
  EXPECT_EQ(std::get<double>(filter.predicates[3].constant.value), 0.5);
  EXPECT_EQ(std::get<Int128>(filter.predicates[4].constant.value), 12);
  EXPECT_TRUE(std::holds_alternative<ScanNode>(Nth(*plan, 3)));
}

TEST(BinderTest, EveryComparisonOperator) {
  const Catalog catalog = MakeCatalog();
  const std::vector<std::pair<std::string_view, CompareOp>> ops = {
      {"=", CompareOp::kEq},  {"<>", CompareOp::kNe}, {"!=", CompareOp::kNe}, {"<", CompareOp::kLt},
      {"<=", CompareOp::kLe}, {">", CompareOp::kGt},  {">=", CompareOp::kGe}};
  for (const auto& [text, op] : ops) {
    const std::string sql = "SELECT COUNT(*) FROM t WHERE i64 " + std::string(text) + " 5";
    auto plan = BindSql(sql, catalog);
    ASSERT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    const auto& filter = std::get<FilterNode>(Nth(*plan, 1));
    EXPECT_EQ(filter.predicates.at(0).op, op) << sql;
  }
}

// String and DATE literals against VARCHAR and DATE columns; numbers against DOUBLE (rounded to
// the nearest double, as in DuckDB).
TEST(BinderTest, NonIntegerConstants) {
  const Catalog catalog = MakeCatalog();
  const auto constant = [&](std::string_view where) -> Constant {
    const std::string sql = "SELECT COUNT(*) FROM t WHERE " + std::string(where);
    auto plan = BindSql(sql, catalog);
    EXPECT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    if (!plan.ok()) {
      return {};
    }
    const auto& p = std::get<FilterNode>(Nth(*plan, 1)).predicates.at(0);
    EXPECT_EQ(p.kind, Predicate::Kind::kCompare) << sql;
    return p.constant;
  };
  const auto days = [&](std::string_view where) { return std::get<Int128>(constant(where).value); };
  EXPECT_EQ(days("dt = '1970-01-01'"), 0);
  EXPECT_EQ(days("dt = DATE '1969-12-31'"), -1);
  EXPECT_EQ(days("dt >= '2000-02-29'"), 11016);
  EXPECT_EQ(days("dt < DATE '0000-01-01'"), -719528);
  EXPECT_EQ(days("dt <= '9999-12-31'"), 2932896);
  const auto bytes = [&](std::string_view where) {
    return std::get<std::string>(constant(where).value);
  };
  EXPECT_EQ(bytes("s = ''"), "");
  EXPECT_EQ(bytes("s = 'O''Brien'"), "O'Brien");
  EXPECT_EQ(bytes("s < '2013-07-01'"), "2013-07-01");
  const auto real = [&](std::string_view where) { return std::get<double>(constant(where).value); };
  EXPECT_EQ(real("d = 1.5"), 1.5);
  EXPECT_EQ(real("d = -0.25"), -0.25);
  EXPECT_EQ(real("d > 7"), 7.0);
  EXPECT_EQ(real("d > 1e3"), 1000.0);
  EXPECT_EQ(real("d < 9007199254740993"), 9007199254740992.0);
  EXPECT_EQ(real("d < 0.1"), 0.1);
  EXPECT_EQ(real("d < 1e400"), std::numeric_limits<double>::infinity());
  EXPECT_EQ(real("d > -1e400"), -std::numeric_limits<double>::infinity());
  EXPECT_EQ(real("d > 1e-400"), 0.0);
  EXPECT_TRUE(std::signbit(real("d > -1e-400")));
}

TEST(BinderTest, FoldedComparisonsKeepNullRejection) {
  const Catalog catalog = MakeCatalog();
  constexpr std::string_view kSql =
      "SELECT COUNT(*) FROM t WHERE i16 < 40000 AND u16 = -1 AND i64 > 1.5 AND i32 <> 2.5";
  auto plan = BindSql(kSql, catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  const auto& predicates = std::get<FilterNode>(Nth(*plan, 1)).predicates;
  ASSERT_EQ(predicates.size(), 4U);
  // True for every value, but a NULL still rejects the row.
  EXPECT_EQ(predicates[0].kind, Predicate::Kind::kIsNotNull);
  EXPECT_EQ(predicates[0].column.value_or(BoundColumn{}).name, "i16");
  // Never true: the column is not even needed.
  EXPECT_EQ(predicates[1].kind, Predicate::Kind::kFalse);
  EXPECT_FALSE(predicates[1].column.has_value());
  EXPECT_EQ(kSql.substr(predicates[1].span.offset, predicates[1].span.length), "u16 = -1");
  // c > 1.5 is c >= 2.
  EXPECT_EQ(predicates[2].kind, Predicate::Kind::kCompare);
  EXPECT_EQ(predicates[2].op, CompareOp::kGe);
  EXPECT_EQ(std::get<Int128>(predicates[2].constant.value), 2);
  EXPECT_EQ(predicates[2].constant.type, LogicalType::kBigInt);
  // c <> 2.5 holds for every value.
  EXPECT_EQ(predicates[3].kind, Predicate::Kind::kIsNotNull);
}

TEST(BinderTest, NegativeLimitIsABindError) {
  // The parser never produces one; the binder still checks the AST it is given.
  const Catalog catalog = MakeCatalog();
  constexpr std::string_view kSql = "SELECT i16 FROM t LIMIT 3";
  auto stmt = sql::Parse(kSql);
  ASSERT_TRUE(stmt.has_value());
  stmt->limit = -1;
  auto plan = Bind(*stmt, catalog);
  ASSERT_FALSE(plan.ok());
  const auto detail = GetSqlError(plan.status());
  ASSERT_NE(detail, nullptr);
  EXPECT_EQ(detail->kind(), SqlErrorDetail::Kind::kBind);
  EXPECT_EQ(SpanText(kSql, plan.status()), "LIMIT 3");
}

TEST(BinderTest, LargestLimit) {
  const Catalog catalog = MakeCatalog();
  auto plan = BindSql("SELECT COUNT(*) FROM t LIMIT 9223372036854775807", catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  EXPECT_EQ(std::get<LimitNode>(Nth(*plan, 0)).limit, std::numeric_limits<int64_t>::max());
}

TEST(BinderTest, EmptySelectListIsABindError) {
  const Catalog catalog = MakeCatalog();
  auto stmt = sql::Parse("SELECT i16 FROM t");
  ASSERT_TRUE(stmt.has_value());
  stmt->items.clear();
  auto plan = Bind(*stmt, catalog);
  ASSERT_FALSE(plan.ok());
  const auto detail = GetSqlError(plan.status());
  ASSERT_NE(detail, nullptr);
  EXPECT_EQ(detail->kind(), SqlErrorDetail::Kind::kBind);
}

TEST(BinderTest, UnknownRowCountStillBinds) {
  const Catalog catalog = MakeCatalog();
  auto plan = BindSql("SELECT COUNT(*) FROM u", catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  EXPECT_TRUE(std::holds_alternative<AggregateNode>(Nth(*plan, 0)));
}

TEST(BinderTest, PathsOpenThroughTheCatalog) {
  std::vector<std::string> opened;
  const Catalog catalog(
      [&opened](const std::string& path) -> arrow::Result<std::shared_ptr<Table>> {
        opened.push_back(path);
        if (path == "missing.parquet") {
          return arrow::Status::IOError("no such file");
        }
        return std::make_shared<FakeTable>(testing::AllTypesSchema(), 1);
      });
  auto plan = BindSql("SELECT MAX(i64) FROM 'data/it''s.parquet'", catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  EXPECT_EQ(std::get<ScanNode>(Nth(*plan, 1)).table_name, "data/it's.parquet");
  // I/O errors keep their code: they are not SQL errors.
  auto missing = BindSql("SELECT COUNT(*) FROM 'missing.parquet'", catalog);
  EXPECT_TRUE(missing.status().IsIOError());
  EXPECT_EQ(GetSqlError(missing.status()), nullptr);
  EXPECT_EQ(opened, (std::vector<std::string>{"data/it's.parquet", "missing.parquet"}));
}

TEST(BinderTest, DuplicateRegistrationFails) {
  Catalog catalog;
  const auto table = std::make_shared<FakeTable>(testing::AllTypesSchema(), 1);
  ASSERT_TRUE(catalog.Register("t", table).ok());
  EXPECT_TRUE(catalog.Register("T", table).IsAlreadyExists());
  EXPECT_TRUE(catalog.Register("x", nullptr).IsInvalid());
}

}  // namespace
}  // namespace antb1::plan
