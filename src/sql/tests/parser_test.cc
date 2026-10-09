#include "antb1/sql/parser.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <limits>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "antb1/sql/ast.h"
#include "antb1/sql/error.h"
#include "antb1/sql/unparse.h"

namespace antb1::sql {
namespace {

using namespace std::string_view_literals;

constexpr std::string_view kDocsHint = "; see docs/sql-subset.md";

std::string_view At(std::string_view sql, SourceSpan span) {
  return sql.substr(span.offset, span.length);
}

const ColumnRef* ColumnOf(const SelectItem& item) { return std::get_if<ColumnRef>(&item.expr); }
const AggregateCall* AggregateOf(const SelectItem& item) {
  return std::get_if<AggregateCall>(&item.expr);
}
std::string ArgName(const AggregateCall& agg) {
  const ColumnRef* column = agg.arg_column();
  return column != nullptr ? column->name : "<none>";
}

// The table or path of a FROM item that the test expects to have one.
TableRef TableOf(const FromItem& item) {
  const TableRef* table = item.table();
  EXPECT_NE(table, nullptr) << "a derived table";
  return table != nullptr ? *table : TableRef{};
}

// The normalized form of a WHERE or HAVING conjunct the test expects to be simple.
Comparison Cmp(const Expr& expr) {
  auto cmp = AsComparison(expr);
  EXPECT_TRUE(cmp.has_value()) << "not a simple comparison";
  return cmp.value_or(Comparison{});
}

HavingComparison HavingCmp(const Expr& expr) {
  auto cmp = AsHavingComparison(expr);
  EXPECT_TRUE(cmp.has_value()) << "not a simple HAVING comparison";
  return cmp.value_or(HavingComparison{});
}

// ---- productions ----------------------------------------------------------------------------

TEST(ParserTest, SelectStar) {
  auto stmt = Parse("SELECT * FROM events");
  ASSERT_TRUE(stmt.has_value()) << stmt.error().message;
  EXPECT_TRUE(stmt->star);
  EXPECT_TRUE(stmt->items.empty());
  EXPECT_EQ(TableOf(stmt->from.at(0)).kind, TableRef::Kind::kName);
  EXPECT_EQ(TableOf(stmt->from.at(0)).name, "events");
  EXPECT_FALSE(TableOf(stmt->from.at(0)).quoted);
  EXPECT_TRUE(stmt->where.empty());
  EXPECT_FALSE(stmt->limit.has_value());
}

TEST(ParserTest, ColumnsKeepTheirSpelling) {
  constexpr std::string_view kSql = R"(SELECT CustomerId, "Region", "a""b", _x1 FROM Events)";
  auto stmt = Parse(kSql);
  ASSERT_TRUE(stmt.has_value()) << stmt.error().message;
  EXPECT_FALSE(stmt->star);
  ASSERT_EQ(stmt->items.size(), 4U);
  struct Expected {
    std::string_view name;
    bool quoted;
    std::string_view text;
  };
  const std::array<Expected, 4> expected{
      {{.name = "CustomerId", .quoted = false, .text = "CustomerId"},
       {.name = "Region", .quoted = true, .text = R"("Region")"},
       {.name = R"(a"b)", .quoted = true, .text = R"("a""b")"},
       {.name = "_x1", .quoted = false, .text = "_x1"}}};
  for (std::size_t i = 0; i < 4; ++i) {
    const ColumnRef* column = ColumnOf(stmt->items[i]);
    ASSERT_NE(column, nullptr) << i;
    EXPECT_EQ(column->name, expected[i].name);
    EXPECT_EQ(column->quoted, expected[i].quoted);
    EXPECT_EQ(At(kSql, column->span), expected[i].text);
    EXPECT_EQ(stmt->items[i].span, column->span);
    EXPECT_FALSE(stmt->items[i].alias.has_value());
  }
  EXPECT_EQ(TableOf(stmt->from.at(0)).name, "Events");
}

TEST(ParserTest, EveryAggregate) {
  constexpr std::string_view kSql =
      R"(SELECT COUNT(*), COUNT(user_id), SUM(amount), AVG("Amount"), MIN(ts), MAX(ts) FROM events)";
  auto stmt = Parse(kSql);
  ASSERT_TRUE(stmt.has_value()) << stmt.error().message;
  ASSERT_EQ(stmt->items.size(), 6U);
  struct Expected {
    AggKind kind;
    std::string_view arg;
    std::string_view text;
  };
  const std::array<Expected, 6> expected{
      {{.kind = AggKind::kCountStar, .arg = "<none>", .text = "COUNT(*)"},
       {.kind = AggKind::kCount, .arg = "user_id", .text = "COUNT(user_id)"},
       {.kind = AggKind::kSum, .arg = "amount", .text = "SUM(amount)"},
       {.kind = AggKind::kAvg, .arg = "Amount", .text = R"(AVG("Amount"))"},
       {.kind = AggKind::kMin, .arg = "ts", .text = "MIN(ts)"},
       {.kind = AggKind::kMax, .arg = "ts", .text = "MAX(ts)"}}};
  for (std::size_t i = 0; i < 6; ++i) {
    const AggregateCall* agg = AggregateOf(stmt->items[i]);
    ASSERT_NE(agg, nullptr) << i;
    EXPECT_EQ(agg->kind, expected[i].kind) << i;
    EXPECT_EQ(ArgName(*agg), expected[i].arg) << i;
    EXPECT_EQ(At(kSql, agg->span), expected[i].text) << i;
    EXPECT_EQ(stmt->items[i].span, agg->span);
  }
  const AggregateCall* avg = AggregateOf(stmt->items[3]);
  ASSERT_NE(avg, nullptr);
  ASSERT_NE(avg->arg_column(), nullptr);
  EXPECT_TRUE(avg->arg_column()->quoted);
  EXPECT_EQ(At(kSql, avg->arg_column()->span), R"("Amount")");
}

TEST(ParserTest, AggregatesAreCaseInsensitiveAndAllowSpaces) {
  constexpr std::string_view kSql = "select count ( * ) , Sum( amount ) from events";
  auto stmt = Parse(kSql);
  ASSERT_TRUE(stmt.has_value()) << stmt.error().message;
  ASSERT_EQ(stmt->items.size(), 2U);
  const AggregateCall* count = AggregateOf(stmt->items[0]);
  const AggregateCall* sum = AggregateOf(stmt->items[1]);
  ASSERT_NE(count, nullptr);
  ASSERT_NE(sum, nullptr);
  EXPECT_EQ(count->kind, AggKind::kCountStar);
  EXPECT_EQ(At(kSql, count->span), "count ( * )");
  EXPECT_EQ(sum->kind, AggKind::kSum);
  EXPECT_EQ(At(kSql, sum->span), "Sum( amount )");
}

TEST(ParserTest, FunctionNamesAreColumnsWithoutParentheses) {
  auto stmt = Parse("SELECT count, sum, date, min FROM date");
  ASSERT_TRUE(stmt.has_value()) << stmt.error().message;
  ASSERT_EQ(stmt->items.size(), 4U);
  for (const auto& item : stmt->items) {
    EXPECT_NE(ColumnOf(item), nullptr);
  }
  EXPECT_EQ(TableOf(stmt->from.at(0)).name, "date");
}

TEST(ParserTest, Aliases) {
  constexpr std::string_view kSql =
      R"(SELECT COUNT(*) AS Total, amount a, SUM(amount) AS "Sum Of ""Amount""", region "R" FROM t)";
  auto stmt = Parse(kSql);
  ASSERT_TRUE(stmt.has_value()) << stmt.error().message;
  ASSERT_EQ(stmt->items.size(), 4U);
  EXPECT_EQ(stmt->items[0].alias, std::optional<std::string>("Total"));
  EXPECT_EQ(stmt->items[1].alias, std::optional<std::string>("a"));
  EXPECT_EQ(stmt->items[2].alias, std::optional<std::string>(R"(Sum Of "Amount")"));
  EXPECT_EQ(stmt->items[3].alias, std::optional<std::string>("R"));
  EXPECT_EQ(At(kSql, stmt->items[0].span), "COUNT(*) AS Total");
  EXPECT_EQ(At(kSql, stmt->items[1].span), "amount a");
  EXPECT_EQ(At(kSql, stmt->items[3].span), R"(region "R")");
}

TEST(ParserTest, NonReservedKeywordsCanBeAliases) {
  auto stmt = Parse("SELECT a AS date, b count, c AS filter FROM t");
  ASSERT_TRUE(stmt.has_value()) << stmt.error().message;
  ASSERT_EQ(stmt->items.size(), 3U);
  EXPECT_EQ(stmt->items[0].alias, std::optional<std::string>("date"));
  EXPECT_EQ(stmt->items[1].alias, std::optional<std::string>("count"));
  EXPECT_EQ(stmt->items[2].alias, std::optional<std::string>("filter"));
}

TEST(ParserTest, QuotedReservedWordsAreNames) {
  auto stmt = Parse(R"(SELECT "select" AS "from" FROM "where" WHERE "limit" = 1)");
  ASSERT_TRUE(stmt.has_value()) << stmt.error().message;
  ASSERT_EQ(stmt->items.size(), 1U);
  const ColumnRef* column = ColumnOf(stmt->items[0]);
  ASSERT_NE(column, nullptr);
  EXPECT_EQ(column->name, "select");
  EXPECT_EQ(stmt->items[0].alias, std::optional<std::string>("from"));
  EXPECT_EQ(TableOf(stmt->from.at(0)).name, "where");
  EXPECT_TRUE(TableOf(stmt->from.at(0)).quoted);
  ASSERT_EQ(stmt->where.size(), 1U);
  EXPECT_EQ(Cmp(stmt->where[0]).column.name, "limit");
}

TEST(ParserTest, TableReferences) {
  auto name = Parse("SELECT a FROM events");
  ASSERT_TRUE(name.has_value());
  EXPECT_EQ(TableOf(name->from.at(0)).kind, TableRef::Kind::kName);
  EXPECT_FALSE(TableOf(name->from.at(0)).quoted);

  constexpr std::string_view kQuoted = R"(SELECT a FROM "My ""Events""")";
  auto quoted = Parse(kQuoted);
  ASSERT_TRUE(quoted.has_value());
  EXPECT_EQ(TableOf(quoted->from.at(0)).kind, TableRef::Kind::kName);
  EXPECT_EQ(TableOf(quoted->from.at(0)).name, R"(My "Events")");
  EXPECT_TRUE(TableOf(quoted->from.at(0)).quoted);
  EXPECT_EQ(At(kQuoted, TableOf(quoted->from.at(0)).span), R"("My ""Events""")");

  constexpr std::string_view kPath = "select count(*) from 'data/it''s part-0.parquet'";
  auto path = Parse(kPath);
  ASSERT_TRUE(path.has_value());
  EXPECT_EQ(TableOf(path->from.at(0)).kind, TableRef::Kind::kPath);
  EXPECT_EQ(TableOf(path->from.at(0)).name, "data/it's part-0.parquet");
  EXPECT_FALSE(TableOf(path->from.at(0)).quoted);
  EXPECT_EQ(At(kPath, TableOf(path->from.at(0)).span), "'data/it''s part-0.parquet'");

  auto glob = Parse("SELECT * FROM 'data/*.parquet'");
  ASSERT_TRUE(glob.has_value());
  EXPECT_EQ(TableOf(glob->from.at(0)).name, "data/*.parquet");
}

// The FROM list is flat: each item records its connector, alias and ON conjuncts, with spans.
TEST(ParserTest, FromListConnectorsAndSpans) {
  constexpr std::string_view kSql =
      "SELECT t /*c*/ . x FROM t, u CROSS  JOIN v JOIN w ON t.a = w.a inner join 'p.parquet' AS p "
      "ON p.b = 1 LEFT OUTER JOIN x ON x.c = t.c AND x.d = 2 left join \"Y\" y ON y.e = 3 OR y.f";
  auto stmt = Parse(kSql);
  ASSERT_TRUE(stmt.has_value()) << stmt.error().message;
  struct Want {
    Connector connector;
    std::string_view connector_text;
    std::string_view item_text;
    std::string_view alias_text;
    std::size_t conjuncts;
    std::string_view on_text;
  };
  const std::array<Want, 7> want{{
      {.connector = Connector::kFirst,
       .connector_text = "",
       .item_text = "t",
       .alias_text = "",
       .conjuncts = 0,
       .on_text = ""},
      {.connector = Connector::kComma,
       .connector_text = ",",
       .item_text = "u",
       .alias_text = "",
       .conjuncts = 0,
       .on_text = ""},
      {.connector = Connector::kCross,
       .connector_text = "CROSS  JOIN",
       .item_text = "v",
       .alias_text = "",
       .conjuncts = 0,
       .on_text = ""},
      {.connector = Connector::kInner,
       .connector_text = "JOIN",
       .item_text = "w",
       .alias_text = "",
       .conjuncts = 1,
       .on_text = "ON t.a = w.a"},
      {.connector = Connector::kInner,
       .connector_text = "inner join",
       .item_text = "'p.parquet' AS p",
       .alias_text = "AS p",
       .conjuncts = 1,
       .on_text = "ON p.b = 1"},
      {.connector = Connector::kLeft,
       .connector_text = "LEFT OUTER JOIN",
       .item_text = "x",
       .alias_text = "",
       .conjuncts = 2,
       .on_text = "ON x.c = t.c AND x.d = 2"},
      {.connector = Connector::kLeft,
       .connector_text = "left join",
       .item_text = "\"Y\" y",
       .alias_text = "y",
       .conjuncts = 1,
       .on_text = "ON y.e = 3 OR y.f"},
  }};
  ASSERT_EQ(stmt->from.size(), want.size());
  for (std::size_t i = 0; i < want.size(); ++i) {
    const FromItem& item = stmt->from[i];
    EXPECT_EQ(item.connector, want[i].connector) << i;
    EXPECT_EQ(At(kSql, item.connector_span), want[i].connector_text) << i;
    EXPECT_EQ(At(kSql, item.span), want[i].item_text) << i;
    EXPECT_EQ(At(kSql, item.alias_span), want[i].alias_text) << i;
    EXPECT_EQ(item.on.size(), want[i].conjuncts) << i;
    EXPECT_EQ(At(kSql, item.on_span), want[i].on_text) << i;
  }
  EXPECT_EQ(TableOf(stmt->from[4]).kind, TableRef::Kind::kPath);
  EXPECT_EQ(stmt->from[4].alias, std::optional<std::string>("p"));
  EXPECT_TRUE(TableOf(stmt->from[6]).quoted);
  EXPECT_EQ(stmt->from[6].alias, std::optional<std::string>("y"));
  EXPECT_EQ(std::get<BinaryExpr>(stmt->from[6].on.front()).op, BinaryOp::kOr);
  // A qualified name spans its qualifier through its name, comments and spaces included.
  const ColumnRef* column = ColumnOf(stmt->items.at(0));
  ASSERT_NE(column, nullptr);
  EXPECT_EQ(column->qualifier, "t");
  EXPECT_FALSE(column->qualifier_quoted);
  EXPECT_EQ(column->name, "x");
  EXPECT_EQ(At(kSql, column->span), "t /*c*/ . x");
  EXPECT_EQ(ToString(stmt->from[5].connector), "LEFT JOIN");
}

// An alias after AS or without it; a quoted identifier, or after AS a non-empty string; the
// reserved words BETWEEN, EXISTS, INTERVAL and OVER (as DuckDB); after a name or a path.
TEST(ParserTest, FromItemAliases) {
  struct Case {
    std::string_view sql;
    std::string_view alias;
    std::string_view alias_text;
  };
  for (const Case& c : {
           Case{.sql = "SELECT a FROM t e", .alias = "e", .alias_text = "e"},
           Case{.sql = "SELECT a FROM t as E", .alias = "E", .alias_text = "as E"},
           Case{.sql = R"(SELECT a FROM t "a ""b""")",
                .alias = R"(a "b")",
                .alias_text = R"("a ""b""")"},
           Case{.sql = R"(SELECT a FROM t AS "select")",
                .alias = "select",
                .alias_text = R"(AS "select")"},
           Case{.sql = "SELECT a FROM t AS 'it''s'", .alias = "it's", .alias_text = "AS 'it''s'"},
           Case{.sql = "SELECT a FROM 'p.parquet' p", .alias = "p", .alias_text = "p"},
           Case{.sql = "SELECT a FROM 'p.parquet' AS /* c */ p",
                .alias = "p",
                .alias_text = "AS /* c */ p"},
           Case{.sql = "SELECT a FROM t over", .alias = "over", .alias_text = "over"},
           Case{
               .sql = "SELECT a FROM t AS Between", .alias = "Between", .alias_text = "AS Between"},
           Case{.sql = "SELECT a FROM t EXISTS WHERE a = 1",
                .alias = "EXISTS",
                .alias_text = "EXISTS"},
           Case{.sql = "SELECT a FROM t AS interval LIMIT 1",
                .alias = "interval",
                .alias_text = "AS interval"},
           Case{.sql = "SELECT a FROM t date", .alias = "date", .alias_text = "date"},
       }) {
    auto stmt = Parse(c.sql);
    ASSERT_TRUE(stmt.has_value()) << c.sql << ": " << stmt.error().message;
    ASSERT_EQ(stmt->from.size(), 1U) << c.sql;
    const FromItem& item = stmt->from.front();
    EXPECT_EQ(item.alias, std::optional<std::string>(c.alias)) << c.sql;
    EXPECT_EQ(At(c.sql, item.alias_span), c.alias_text) << c.sql;
    EXPECT_TRUE(At(c.sql, item.span).ends_with(At(c.sql, item.alias_span))) << c.sql;
  }
  auto none = Parse("SELECT a FROM t");
  ASSERT_TRUE(none.has_value());
  EXPECT_FALSE(none->from.front().alias.has_value());
  EXPECT_EQ(none->from.front().alias_span, SourceSpan{});
}

// ON is split at its top-level AND chain, like WHERE: a parenthesized AND or a top-level OR makes
// one conjunct.
TEST(ParserTest, OnConjunctsSplitLikeWhere) {
  auto chain = Parse("SELECT a FROM t JOIN u ON t.a = u.a AND (u.b = 1) AND t.c < u.c WHERE a = 1");
  ASSERT_TRUE(chain.has_value()) << chain.error().message;
  EXPECT_EQ(chain->from.at(1).on.size(), 3U);
  EXPECT_EQ(chain->where.size(), 1U);
  auto nested = Parse("SELECT a FROM t JOIN u ON (t.a = u.a AND u.b = 1) AND t.c < u.c");
  ASSERT_TRUE(nested.has_value());
  ASSERT_EQ(nested->from.at(1).on.size(), 2U);
  EXPECT_EQ(std::get<BinaryExpr>(nested->from.at(1).on[0]).op, BinaryOp::kAnd);
  auto with_or = Parse("SELECT a FROM t LEFT JOIN u ON t.a = u.a AND u.b = 1 OR u.c = 2");
  ASSERT_TRUE(with_or.has_value());
  ASSERT_EQ(with_or->from.at(1).on.size(), 1U);
  EXPECT_EQ(std::get<BinaryExpr>(with_or->from.at(1).on[0]).op, BinaryOp::kOr);
  // A long chain builds no deep tree.
  std::string sql = "SELECT a FROM t JOIN u ON a0 = 0";
  for (int i = 1; i < 1000; ++i) {
    sql += " AND a" + std::to_string(i) + " = " + std::to_string(i);
  }
  auto wide = Parse(sql);
  ASSERT_TRUE(wide.has_value()) << wide.error().message;
  EXPECT_EQ(wide->from.at(1).on.size(), 1000U);
}

// Qualified names, unquoted or quoted on either side, wherever a column may stand.
TEST(ParserTest, QualifiedColumnsInEveryClause) {
  constexpr std::string_view kSql =
      R"(SELECT t.a, SUM("T".b), COUNT(DISTINCT t."B c"), CASE WHEN t.c IN (t.d, 1) THEN t.e END, )"
      "lower(t.f), EXTRACT(year FROM t.g), t.h::INT FROM t WHERE t.i BETWEEN t.j AND 2 AND t.k "
      "LIKE 'x' GROUP BY t.a HAVING MAX(t.l) > 1 ORDER BY t.a DESC, \"x y\".m";
  auto stmt = Parse(kSql);
  ASSERT_TRUE(stmt.has_value()) << stmt.error().message;
  // Every column of the statement is qualified: collect them through ToSql's canonical form.
  const std::string canonical = ToSql(*stmt);
  EXPECT_EQ(canonical,
            R"(SELECT t.a, SUM("T".b), COUNT(DISTINCT t."B c"), CASE WHEN t.c IN (t.d, 1) THEN )"
            "t.e END, lower(t.f), EXTRACT(year FROM t.g), CAST(t.h AS INT) FROM t WHERE t.i "
            "BETWEEN t.j AND 2 AND t.k LIKE 'x' GROUP BY t.a HAVING MAX(t.l) > 1 ORDER BY t.a "
            "DESC, \"x y\".m");
  const auto* sum = std::get_if<AggregateCall>(&stmt->items.at(1).expr);
  ASSERT_NE(sum, nullptr);
  ASSERT_NE(sum->arg_column(), nullptr);
  EXPECT_EQ(sum->arg_column()->qualifier, "T");
  EXPECT_TRUE(sum->arg_column()->qualifier_quoted);
  EXPECT_FALSE(sum->arg_column()->quoted);
  EXPECT_EQ(At(kSql, sum->arg_column()->span), R"("T".b)");
  const auto* distinct = std::get_if<AggregateCall>(&stmt->items.at(2).expr);
  ASSERT_NE(distinct, nullptr);
  ASSERT_NE(distinct->arg_column(), nullptr);
  EXPECT_EQ(distinct->arg_column()->name, "B c");
  EXPECT_TRUE(distinct->arg_column()->quoted);
  EXPECT_EQ(Cmp(stmt->where.at(1)).column.qualifier, "t");
  EXPECT_EQ(std::get<ColumnRef>(stmt->group_by.at(0)).qualifier, "t");
  EXPECT_EQ(HavingCmp(stmt->having.at(0)).op, CompareOp::kGt);
  const auto& last = std::get<ColumnRef>(stmt->order_by.at(1).expr);
  EXPECT_EQ(last.qualifier, "x y");
  EXPECT_EQ(At(kSql, last.span), "\"x y\".m");
  // A qualified and an unqualified name differ, and so do the quoting of either part.
  auto plain = Parse("SELECT a FROM t");
  auto qualified = Parse("SELECT t.a FROM t");
  ASSERT_TRUE(plain.has_value() && qualified.has_value());
  EXPECT_FALSE(EqualIgnoringSpans(*plain, *qualified));
}

// A JOIN binds tighter than a comma, so every FROM clause without nested joins is a flat list:
// `a, b JOIN c ON ...` is not `a CROSS JOIN b JOIN c ON ...`.
TEST(ParserTest, JoinsStayAFlatList) {
  auto stmt = Parse(
      "SELECT a FROM a, b JOIN c ON b.k = c.k, d LEFT JOIN e ON d.k = e.k CROSS JOIN f JOIN g ON "
      "f.k = g.k");
  ASSERT_TRUE(stmt.has_value()) << stmt.error().message;
  const std::array<Connector, 7> connectors = {
      Connector::kFirst, Connector::kComma, Connector::kInner, Connector::kComma,
      Connector::kLeft,  Connector::kCross, Connector::kInner};
  ASSERT_EQ(stmt->from.size(), connectors.size());
  for (std::size_t i = 0; i < connectors.size(); ++i) {
    EXPECT_EQ(stmt->from[i].connector, connectors[i]) << i;
    EXPECT_EQ(TableOf(stmt->from[i]).name, std::string(1, static_cast<char>('a' + i))) << i;
  }
  auto comma = Parse("SELECT a FROM a, b JOIN c ON b.k = c.k");
  auto cross = Parse("SELECT a FROM a CROSS JOIN b JOIN c ON b.k = c.k");
  ASSERT_TRUE(comma.has_value() && cross.has_value());
  EXPECT_FALSE(EqualIgnoringSpans(*comma, *cross));
  EXPECT_EQ(Depth(*stmt), 2U) << "the ON conjuncts count";
}

// The query of a derived table or of a CTE: the test expects the item or the CTE to have one.
const SelectStatement& QueryOf(const FromItem& item) {
  static const SelectStatement kNone;
  const auto* derived = std::get_if<DerivedTable>(&item.source);
  EXPECT_NE(derived, nullptr) << "not a derived table";
  return derived != nullptr ? *derived->query : kNone;
}

// A derived table is a FROM item like a table: with or without an alias (after AS also a string),
// with a column alias list after its alias, joined, holding a WITH list or a derived table of its
// own. Its span runs from its '(' through its alias or column list; its query's span from its
// SELECT (or WITH) through its last token, without the parentheses.
TEST(ParserTest, DerivedTables) {
  constexpr std::string_view kSql =
      "SELECT s.x FROM (SELECT a, b FROM t WHERE a > 1) AS s(x, \"Y\") JOIN (SELECT c FROM "
      "'p.parquet') r ON s.x = r.c, (WITH w AS (SELECT d FROM u) SELECT d FROM w) AS 'q', "
      "( SELECT * FROM (SELECT e FROM v) n ) ORDER BY 1";
  auto stmt = Parse(kSql);
  ASSERT_TRUE(stmt.has_value()) << stmt.error().message;
  ASSERT_EQ(stmt->from.size(), 4U);
  for (const FromItem& item : stmt->from) {
    EXPECT_EQ(item.table(), nullptr);
  }
  const FromItem& s = stmt->from[0];
  EXPECT_EQ(At(kSql, s.span), "(SELECT a, b FROM t WHERE a > 1) AS s(x, \"Y\")");
  EXPECT_EQ(At(kSql, std::get<DerivedTable>(s.source).span), "(SELECT a, b FROM t WHERE a > 1)");
  EXPECT_EQ(s.alias, std::optional<std::string>("s"));
  EXPECT_EQ(At(kSql, s.alias_span), "AS s");
  EXPECT_EQ(s.columns, (std::vector<std::string>{"x", "Y"}));
  EXPECT_EQ(At(kSql, s.columns_span), "(x, \"Y\")");
  const SelectStatement& s_query = QueryOf(s);
  EXPECT_EQ(At(kSql, s_query.span), "SELECT a, b FROM t WHERE a > 1");
  EXPECT_EQ(s_query.items.size(), 2U);
  EXPECT_EQ(TableOf(s_query.from.at(0)).name, "t");
  EXPECT_EQ(s_query.where.size(), 1U);
  const FromItem& r = stmt->from[1];
  EXPECT_EQ(r.connector, Connector::kInner);
  EXPECT_EQ(At(kSql, r.span), "(SELECT c FROM 'p.parquet') r");
  EXPECT_EQ(r.alias, std::optional<std::string>("r"));
  EXPECT_TRUE(r.columns.empty());
  EXPECT_EQ(r.columns_span, SourceSpan{});
  EXPECT_EQ(At(kSql, r.on_span), "ON s.x = r.c");
  EXPECT_EQ(TableOf(QueryOf(r).from.at(0)).kind, TableRef::Kind::kPath);
  const FromItem& q = stmt->from[2];
  EXPECT_EQ(q.connector, Connector::kComma);
  EXPECT_EQ(q.alias, std::optional<std::string>("q"));
  const SelectStatement& q_query = QueryOf(q);
  ASSERT_EQ(q_query.with.size(), 1U);
  EXPECT_EQ(At(kSql, q_query.span), "WITH w AS (SELECT d FROM u) SELECT d FROM w");
  EXPECT_EQ(At(kSql, q_query.with_span), "WITH");
  const FromItem& unnamed = stmt->from[3];
  EXPECT_FALSE(unnamed.alias.has_value());
  EXPECT_EQ(At(kSql, unnamed.span), "( SELECT * FROM (SELECT e FROM v) n )");
  const SelectStatement& outer = QueryOf(unnamed);
  EXPECT_TRUE(outer.star);
  ASSERT_EQ(outer.from.size(), 1U);
  EXPECT_EQ(outer.from[0].alias, std::optional<std::string>("n"));
  EXPECT_EQ(TableOf(QueryOf(outer.from[0]).from.at(0)).name, "v");
  EXPECT_EQ(stmt->order_by.size(), 1U);
  EXPECT_EQ(At(kSql, stmt->span), kSql);
  // Each nested query is one level below its statement: the derived table's derived table.
  EXPECT_EQ(Depth(*stmt), 3U);
  // An alias after AS: a quoted identifier, BETWEEN, EXISTS, INTERVAL or OVER (as after a table),
  // also before a column alias list.
  for (const std::string_view alias : {"\"from\"", "over", "AS interval", "AS 'it''s'"}) {
    const std::string sql = "SELECT 1 FROM (SELECT a FROM t) " + std::string(alias) + "(x)";
    auto aliased = Parse(sql);
    ASSERT_TRUE(aliased.has_value()) << sql << ": " << aliased.error().message;
    EXPECT_EQ(aliased->from.at(0).columns, std::vector<std::string>{"x"}) << sql;
  }
}

// A WITH list: one or more CTEs, each named as written, with a column alias list or not, whose
// queries may hold WITH lists of their own. The statement's span starts at WITH.
TEST(ParserTest, WithLists) {
  constexpr std::string_view kSql =
      "/* c */ with a AS (SELECT x FROM t), \"B c\"(y, \"Z\") AS (WITH d AS (SELECT 1 FROM u) "
      "SELECT "
      "y FROM d), over AS (SELECT * FROM a) SELECT x FROM a;";
  auto stmt = Parse(kSql);
  ASSERT_TRUE(stmt.has_value()) << stmt.error().message;
  EXPECT_EQ(At(kSql, stmt->with_span), "with");
  EXPECT_EQ(At(kSql, stmt->span),
            "with a AS (SELECT x FROM t), \"B c\"(y, \"Z\") AS (WITH d AS (SELECT 1 FROM u) SELECT "
            "y FROM d), over AS (SELECT * FROM a) SELECT x FROM a");
  ASSERT_EQ(stmt->with.size(), 3U);
  const CommonTableExpr& a = stmt->with[0];
  EXPECT_EQ(a.name, "a");
  EXPECT_TRUE(a.columns.empty());
  EXPECT_EQ(At(kSql, a.name_span), "a");
  EXPECT_EQ(a.columns_span, SourceSpan{});
  EXPECT_EQ(At(kSql, a.span), "a AS (SELECT x FROM t)");
  EXPECT_EQ(At(kSql, a.query->span), "SELECT x FROM t");
  const CommonTableExpr& b = stmt->with[1];
  EXPECT_EQ(b.name, "B c");
  EXPECT_EQ(b.columns, (std::vector<std::string>{"y", "Z"}));
  EXPECT_EQ(At(kSql, b.name_span), "\"B c\"");
  EXPECT_EQ(At(kSql, b.columns_span), "(y, \"Z\")");
  EXPECT_EQ(At(kSql, b.span), "\"B c\"(y, \"Z\") AS (WITH d AS (SELECT 1 FROM u) SELECT y FROM d)");
  ASSERT_EQ(b.query->with.size(), 1U);
  EXPECT_EQ(b.query->with[0].name, "d");
  EXPECT_EQ(At(kSql, b.query->span), "WITH d AS (SELECT 1 FROM u) SELECT y FROM d");
  EXPECT_EQ(stmt->with[2].name, "over");
  EXPECT_TRUE(stmt->with[2].query->star);
  // The statement's own block follows the list.
  ASSERT_EQ(stmt->items.size(), 1U);
  EXPECT_EQ(TableOf(stmt->from.at(0)).name, "a");
  EXPECT_EQ(Depth(*stmt), 3U) << "the CTE's CTE: 2 levels below the statement, then x";
  // A query without a WITH list has none.
  auto plain = Parse("SELECT a FROM t");
  ASSERT_TRUE(plain.has_value());
  EXPECT_TRUE(plain->with.empty());
  EXPECT_EQ(plain->with_span, SourceSpan{});
}

// The names of a column alias list follow the rules of an implicit table alias (as in DuckDB):
// names, quoted identifiers and BETWEEN, EXISTS, INTERVAL and OVER, as written. A CTE's list may
// be of any length.
TEST(ParserTest, ColumnAliasLists) {
  auto derived =
      Parse("SELECT 1 FROM (SELECT a FROM t) s(over, Between, \"select\", \"a\"\"b\", date, x_1)");
  ASSERT_TRUE(derived.has_value()) << derived.error().message;
  EXPECT_EQ(derived->from.at(0).columns,
            (std::vector<std::string>{"over", "Between", "select", "a\"b", "date", "x_1"}));
  auto cte = Parse("WITH c(exists, interval) AS (SELECT a FROM t) SELECT 1 FROM c");
  ASSERT_TRUE(cte.has_value()) << cte.error().message;
  EXPECT_EQ(cte->with.at(0).columns, (std::vector<std::string>{"exists", "interval"}));
  // More names than the query has columns: DuckDB ignores the extra ones of a CTE.
  auto longer = Parse("WITH c(x, y, z) AS (SELECT a FROM t) SELECT x FROM c");
  ASSERT_TRUE(longer.has_value()) << longer.error().message;
  EXPECT_EQ(longer->with.at(0).columns.size(), 3U);
  // A column alias list follows an alias only: after a derived table without one, '(' ends the
  // FROM list (a syntax error, as in DuckDB).
  auto bare = Parse("SELECT 1 FROM (SELECT a FROM t) (x)");
  ASSERT_FALSE(bare.has_value());
  EXPECT_EQ(bare.error().kind, ParseError::Kind::kSyntax);
  EXPECT_EQ(bare.error().span.offset, 32U);
}

// CTE names of one WITH list match ASCII case-insensitively, quoted or not and also as strings (as
// in DuckDB): a repeated name is a syntax error at the name, before its query is parsed, so a
// construct in that query that antb1 does not support never hides it. A nested WITH list may reuse
// a name.
TEST(ParserTest, DuplicateCteNames) {
  constexpr std::string_view kMessage =
      "duplicate CTE name in the WITH list (names match case-insensitively)";
  for (const std::string_view sql : {
           "WITH c AS (SELECT a FROM t), ^C AS (SELECT a FROM t) SELECT a FROM c"sv,
           "WITH \"C\" AS (SELECT a FROM t), ^\"c\" AS (SELECT a FROM t) SELECT a FROM c"sv,
           "WITH c AS (SELECT a FROM t), ^'c' AS (SELECT a FROM t) SELECT a FROM c"sv,
           "WITH c AS (SELECT a FROM t), d AS (SELECT a FROM t), ^c AS (SELECT a FROM t) SELECT "
           "a FROM c"sv,
           // Whatever the repeated CTE holds: unsupported SQL, a malformed list or query.
           "WITH c AS (SELECT a FROM t), ^c AS (SELECT a FROM t WHERE a IS NULL) SELECT a FROM c"sv,
           "WITH c AS (SELECT a FROM t), ^c(x,) AS (SELECT a FROM t) SELECT a FROM c"sv,
           "WITH c AS (SELECT a FROM t), ^c AS MATERIALIZED (SELECT a FROM t) SELECT a FROM c"sv,
           "WITH c AS (SELECT a FROM t), ^c USING KEY (a) AS (SELECT a FROM t) SELECT a FROM c"sv,
           "WITH c AS (SELECT a FROM t), ^c AS (SELECT FROM) SELECT a FROM c"sv,
           "SELECT a FROM (WITH c AS (SELECT a FROM t), ^c AS (SELECT a FROM t) SELECT a FROM c)"sv,
       }) {
    const std::size_t caret = sql.find('^');
    const std::string text = std::string(sql.substr(0, caret)) + std::string(sql.substr(caret + 1));
    auto result = Parse(text);
    ASSERT_FALSE(result.has_value()) << text;
    EXPECT_EQ(result.error().kind, ParseError::Kind::kSyntax) << text;
    EXPECT_EQ(result.error().span.offset, caret) << text;
    EXPECT_EQ(result.error().message, kMessage) << text;
  }
  for (const std::string_view sql : {
           // Only ASCII letters match case-insensitively.
           "WITH \"\xc3\x89\" AS (SELECT a FROM t), \"\xc3\xa9\" AS (SELECT a FROM t) SELECT a "
           "FROM t"sv,
           // A nested WITH list starts afresh.
           "WITH c AS (WITH c AS (SELECT a FROM t) SELECT a FROM c) SELECT a FROM c"sv,
           "WITH c AS (SELECT a FROM t) SELECT a FROM (WITH C AS (SELECT a FROM t) SELECT a FROM "
           "c)"sv,
           "SELECT a FROM (WITH c AS (SELECT a FROM t) SELECT a FROM c), (WITH c AS (SELECT a FROM "
           "t) SELECT a FROM c)"sv,
           "WITH c AS (SELECT a FROM t), \"c \" AS (SELECT a FROM t) SELECT a FROM c"sv,
       }) {
    auto result = Parse(sql);
    EXPECT_TRUE(result.has_value()) << sql << ": " << result.error().message;
  }
}

// RECURSIVE right after WITH is unsupported, unless it is the name of the first CTE (AS, '(' or
// USING follows it), as in DuckDB; elsewhere it is a name like any other.
TEST(ParserTest, WithRecursiveOrACteNamedRecursive) {
  for (const std::string_view sql : {
           "WITH RECURSIVE c AS (SELECT a FROM t) SELECT a FROM c"sv,
           "with recursive recursive AS (SELECT a FROM t) SELECT a FROM recursive"sv,
           "WITH RECURSIVE c(x) AS (SELECT a FROM t) SELECT x FROM c"sv,
       }) {
    auto result = Parse(sql);
    ASSERT_FALSE(result.has_value()) << sql;
    EXPECT_EQ(result.error().kind, ParseError::Kind::kUnsupported) << sql;
    EXPECT_EQ(result.error().span, (SourceSpan{.offset = 5, .length = 9})) << sql;
    EXPECT_TRUE(result.error().message.starts_with("WITH RECURSIVE is not supported")) << sql;
  }
  auto named = Parse("WITH recursive AS (SELECT a FROM t) SELECT a FROM recursive");
  ASSERT_TRUE(named.has_value()) << named.error().message;
  EXPECT_EQ(named->with.at(0).name, "recursive");
  auto listed = Parse("WITH Recursive(x) AS (SELECT a FROM t) SELECT x FROM recursive");
  ASSERT_TRUE(listed.has_value()) << listed.error().message;
  EXPECT_EQ(listed->with.at(0).name, "Recursive");
  EXPECT_EQ(listed->with.at(0).columns, std::vector<std::string>{"x"});
  auto later = Parse("WITH c AS (SELECT a FROM t), recursive AS (SELECT a FROM c) SELECT a FROM c");
  ASSERT_TRUE(later.has_value()) << later.error().message;
  EXPECT_EQ(later->with.at(1).name, "recursive");
  // USING KEY after a CTE named recursive is unsupported at USING, not at the name.
  auto keyed =
      Parse("WITH recursive USING KEY (x) AS (SELECT a AS x FROM t) SELECT x FROM recursive");
  ASSERT_FALSE(keyed.has_value());
  EXPECT_EQ(keyed.error().kind, ParseError::Kind::kUnsupported);
  EXPECT_EQ(keyed.error().span, (SourceSpan{.offset = 15, .length = 9}));
  EXPECT_TRUE(keyed.error().message.starts_with("USING KEY is not supported"));
}

struct OpCase {
  std::string_view name;
  std::string_view text;
  CompareOp op;
  CompareOp mirrored;
};

class CompareOpTest : public ::testing::TestWithParam<OpCase> {};

TEST_P(CompareOpTest, ColumnFirst) {
  const OpCase& c = GetParam();
  const std::string sql = "SELECT a FROM t WHERE amount " + std::string(c.text) + " 42";
  auto stmt = Parse(sql);
  ASSERT_TRUE(stmt.has_value()) << stmt.error().message;
  ASSERT_EQ(stmt->where.size(), 1U);
  const Comparison& cmp = Cmp(stmt->where[0]);
  EXPECT_EQ(cmp.op, c.op);
  EXPECT_EQ(cmp.column.name, "amount");
  EXPECT_EQ(cmp.literal.text, "42");
  EXPECT_EQ(At(sql, cmp.span), "amount " + std::string(c.text) + " 42");
}

TEST_P(CompareOpTest, LiteralFirstIsNormalized) {
  const OpCase& c = GetParam();
  const std::string sql = "SELECT a FROM t WHERE -4.5 " + std::string(c.text) + " amount";
  auto stmt = Parse(sql);
  ASSERT_TRUE(stmt.has_value()) << stmt.error().message;
  ASSERT_EQ(stmt->where.size(), 1U);
  const Comparison& cmp = Cmp(stmt->where[0]);
  EXPECT_EQ(cmp.op, c.mirrored);
  EXPECT_EQ(cmp.column.name, "amount");
  EXPECT_EQ(cmp.literal.kind, Literal::Kind::kDecimal);
  EXPECT_TRUE(cmp.literal.negative);
  EXPECT_EQ(cmp.literal.text, "4.5");
  // Spans still point at the source text in its original order.
  EXPECT_EQ(At(sql, cmp.column.span), "amount");
  EXPECT_EQ(At(sql, cmp.literal.span), "-4.5");
  EXPECT_EQ(At(sql, cmp.span), "-4.5 " + std::string(c.text) + " amount");
}

INSTANTIATE_TEST_SUITE_P(Ops, CompareOpTest,
                         ::testing::Values(OpCase{"Eq", "=", CompareOp::kEq, CompareOp::kEq},
                                           OpCase{"Ne", "<>", CompareOp::kNe, CompareOp::kNe},
                                           OpCase{"BangEq", "!=", CompareOp::kNe, CompareOp::kNe},
                                           OpCase{"Lt", "<", CompareOp::kLt, CompareOp::kGt},
                                           OpCase{"Le", "<=", CompareOp::kLe, CompareOp::kGe},
                                           OpCase{"Gt", ">", CompareOp::kGt, CompareOp::kLt},
                                           OpCase{"Ge", ">=", CompareOp::kGe, CompareOp::kLe}),
                         [](const ::testing::TestParamInfo<OpCase>& param_info) {
                           return std::string(param_info.param.name);
                         });

struct LiteralCase {
  std::string_view name;
  std::string_view text;  // as written after "WHERE c = "
  Literal::Kind kind;
  bool negative;
  std::string_view value;
};

class LiteralTest : public ::testing::TestWithParam<LiteralCase> {};

TEST_P(LiteralTest, Parses) {
  const LiteralCase& c = GetParam();
  const std::string sql = "SELECT a FROM t WHERE c = " + std::string(c.text);
  auto stmt = Parse(sql);
  ASSERT_TRUE(stmt.has_value()) << stmt.error().message;
  ASSERT_EQ(stmt->where.size(), 1U);
  const Literal& literal = Cmp(stmt->where[0]).literal;
  EXPECT_EQ(literal.kind, c.kind);
  EXPECT_EQ(literal.negative, c.negative);
  EXPECT_EQ(literal.text, c.value);
  EXPECT_EQ(At(sql, literal.span), c.text);
}

INSTANTIATE_TEST_SUITE_P(
    Literals, LiteralTest,
    ::testing::Values(
        LiteralCase{"Integer", "42", Literal::Kind::kInteger, false, "42"},
        LiteralCase{"LeadingZeros", "007", Literal::Kind::kInteger, false, "007"},
        LiteralCase{"HugeInteger", "123456789012345678901234567890", Literal::Kind::kInteger, false,
                    "123456789012345678901234567890"},
        LiteralCase{"NegativeInteger", "-42", Literal::Kind::kInteger, true, "42"},
        LiteralCase{"NegativeWithSpace", "- 42", Literal::Kind::kInteger, true, "42"},
        LiteralCase{"NegativeWithComment", "-/* c */42", Literal::Kind::kInteger, true, "42"},
        LiteralCase{"Decimal", "1.50", Literal::Kind::kDecimal, false, "1.50"},
        LiteralCase{"DecimalNoInteger", ".5", Literal::Kind::kDecimal, false, ".5"},
        LiteralCase{"DecimalNoFraction", "5.", Literal::Kind::kDecimal, false, "5."},
        LiteralCase{"Exponent", "2.5E-3", Literal::Kind::kDecimal, false, "2.5E-3"},
        LiteralCase{"NegativeDecimal", "-0.25", Literal::Kind::kDecimal, true, "0.25"},
        LiteralCase{"String", "'north'", Literal::Kind::kString, false, "north"},
        LiteralCase{"EmptyString", "''", Literal::Kind::kString, false, ""},
        LiteralCase{"EscapedString", "'it''s'", Literal::Kind::kString, false, "it's"},
        LiteralCase{"StringWithCommentMarkers", "'-- /* */'", Literal::Kind::kString, false,
                    "-- /* */"},
        LiteralCase{"Date", "DATE '2024-01-31'", Literal::Kind::kDate, false, "2024-01-31"},
        LiteralCase{"LowerCaseDate", "date '2024-02-29'", Literal::Kind::kDate, false,
                    "2024-02-29"},
        LiteralCase{"DateWithComment", "DATE /* c */ '2024-03-01'", Literal::Kind::kDate, false,
                    "2024-03-01"},
        LiteralCase{"Timestamp", "TIMESTAMP '2024-01-31 12:34:56.5'", Literal::Kind::kTimestamp,
                    false, "2024-01-31 12:34:56.5"},
        LiteralCase{"LowerCaseTimestamp", "timestamp '2024-02-29'", Literal::Kind::kTimestamp,
                    false, "2024-02-29"}),
    [](const ::testing::TestParamInfo<LiteralCase>& param_info) {
      return std::string(param_info.param.name);
    });

TEST(ParserTest, DateIsAColumnUnlessFollowedByAString) {
  constexpr std::string_view kSql = "SELECT date FROM t WHERE date >= DATE '2024-01-01'";
  auto stmt = Parse(kSql);
  ASSERT_TRUE(stmt.has_value()) << stmt.error().message;
  ASSERT_EQ(stmt->where.size(), 1U);
  EXPECT_EQ(Cmp(stmt->where[0]).column.name, "date");
  EXPECT_EQ(Cmp(stmt->where[0]).literal.kind, Literal::Kind::kDate);
  EXPECT_EQ(At(kSql, Cmp(stmt->where[0]).literal.span), "DATE '2024-01-01'");
}

TEST(ParserTest, ConjunctionOfComparisons) {
  constexpr std::string_view kSql =
      "SELECT COUNT(*) FROM events WHERE amount > 0 AND 'north' = region and ts <= DATE "
      R"('2024-12-31' AND "User" <> -1)";
  auto stmt = Parse(kSql);
  ASSERT_TRUE(stmt.has_value()) << stmt.error().message;
  ASSERT_EQ(stmt->where.size(), 4U);
  EXPECT_EQ(At(kSql, Cmp(stmt->where[0]).span), "amount > 0");
  EXPECT_EQ(Cmp(stmt->where[1]).column.name, "region");
  EXPECT_EQ(Cmp(stmt->where[1]).op, CompareOp::kEq);
  EXPECT_EQ(At(kSql, Cmp(stmt->where[1]).span), "'north' = region");
  EXPECT_EQ(Cmp(stmt->where[2]).literal.kind, Literal::Kind::kDate);
  EXPECT_TRUE(Cmp(stmt->where[3]).column.quoted);
  EXPECT_EQ(Cmp(stmt->where[3]).op, CompareOp::kNe);
  EXPECT_TRUE(Cmp(stmt->where[3]).literal.negative);
}

TEST(ParserTest, Limit) {
  struct Case {
    std::string_view text;
    std::int64_t value;
  };
  for (const Case& c :
       {Case{.text = "0", .value = 0}, Case{.text = "10", .value = 10},
        Case{.text = "0000000000000000000000042", .value = 42},
        Case{.text = "9223372036854775807", .value = std::numeric_limits<std::int64_t>::max()}}) {
    const std::string sql = "SELECT a FROM t WHERE b = 1 LIMIT " + std::string(c.text);
    auto stmt = Parse(sql);
    ASSERT_TRUE(stmt.has_value()) << stmt.error().message;
    EXPECT_EQ(stmt->limit, std::optional<std::int64_t>(c.value)) << c.text;
  }
  auto no_where = Parse("SELECT * FROM t LIMIT 3;");
  ASSERT_TRUE(no_where.has_value());
  EXPECT_EQ(no_where->limit, std::optional<std::int64_t>(3));
}

TEST(ParserTest, GroupByOrderByLimitOffset) {
  const std::string sql =
      "SELECT a, COUNT(*) AS c FROM events WHERE b = 1 GROUP BY a, \"B\" ORDER BY c DESC, "
      "SUM(x) NULLS FIRST, a asc nulls last LIMIT 10 OFFSET 5";
  auto stmt = Parse(sql);
  ASSERT_TRUE(stmt.has_value()) << stmt.error().message;
  ASSERT_EQ(stmt->group_by.size(), 2U);
  EXPECT_EQ(std::get<ColumnRef>(stmt->group_by[0]).name, "a");
  EXPECT_TRUE(std::get<ColumnRef>(stmt->group_by[1]).quoted);
  EXPECT_EQ(At(sql, stmt->group_by_span), "GROUP BY a, \"B\"");
  ASSERT_EQ(stmt->order_by.size(), 3U);
  EXPECT_EQ(std::get<ColumnRef>(stmt->order_by[0].expr).name, "c");
  EXPECT_TRUE(stmt->order_by[0].descending);
  EXPECT_EQ(stmt->order_by[0].nulls, NullsOrder::kDefault);
  EXPECT_EQ(std::get<AggregateCall>(stmt->order_by[1].expr).kind, AggKind::kSum);
  EXPECT_FALSE(stmt->order_by[1].descending);
  EXPECT_EQ(stmt->order_by[1].nulls, NullsOrder::kFirst);
  EXPECT_EQ(stmt->order_by[2].nulls, NullsOrder::kLast);
  EXPECT_EQ(At(sql, stmt->order_by[2].span), "a asc nulls last");
  EXPECT_EQ(At(sql, stmt->order_by_span), "ORDER BY c DESC, SUM(x) NULLS FIRST, a asc nulls last");
  EXPECT_EQ(stmt->limit, 10);
  EXPECT_EQ(stmt->offset, 5);
  EXPECT_EQ(At(sql, stmt->offset_span), "OFFSET 5");
  EXPECT_EQ(At(sql, stmt->span), sql);
}

TEST(ParserTest, OffsetBeforeLimitAndAlone) {
  auto both = Parse("SELECT a FROM events OFFSET 3 LIMIT 2");
  ASSERT_TRUE(both.has_value()) << both.error().message;
  EXPECT_EQ(both->limit, 2);
  EXPECT_EQ(both->offset, 3);
  auto alone = Parse("SELECT a FROM events ORDER BY a OFFSET 0");
  ASSERT_TRUE(alone.has_value()) << alone.error().message;
  EXPECT_FALSE(alone->limit.has_value());
  EXPECT_EQ(alone->offset, 0);
}

// column [NOT] LIKE 'pattern' in WHERE, in any case; the span covers the column and the pattern.
TEST(ParserTest, Like) {
  constexpr std::string_view kSql =
      "SELECT a FROM events WHERE url like '%x_%' AND title Not LIKE 'y' AND b = 1";
  auto stmt = Parse(kSql);
  ASSERT_TRUE(stmt.has_value()) << stmt.error().message;
  ASSERT_EQ(stmt->where.size(), 3U);
  EXPECT_EQ(Cmp(stmt->where[0]).op, CompareOp::kLike);
  EXPECT_EQ(Cmp(stmt->where[0]).column.name, "url");
  EXPECT_EQ(Cmp(stmt->where[0]).literal.text, "%x_%");
  EXPECT_EQ(kSql.substr(Cmp(stmt->where[0]).span.offset, Cmp(stmt->where[0]).span.length),
            "url like '%x_%'");
  EXPECT_EQ(Cmp(stmt->where[1]).op, CompareOp::kNotLike);
  EXPECT_EQ(kSql.substr(Cmp(stmt->where[1]).span.offset, Cmp(stmt->where[1]).span.length),
            "title Not LIKE 'y'");
  EXPECT_EQ(Cmp(stmt->where[2]).op, CompareOp::kEq);
  // A number as the pattern parses; the binder rejects it.
  auto number = Parse("SELECT a FROM t WHERE b LIKE 5");
  ASSERT_TRUE(number.has_value()) << number.error().message;
  EXPECT_EQ(Cmp(number->where[0]).literal.kind, Literal::Kind::kInteger);
}

// x [NOT] BETWEEN low AND high at comparison precedence: its bounds are additive expressions, so
// the AND is BETWEEN's own and the conjunction around it still splits; NOT x BETWEEN and
// x NOT BETWEEN are different trees.
TEST(ParserTest, Between) {
  constexpr std::string_view kSql =
      "SELECT a FROM t WHERE b between 1 AND c + 2 AND d NOT BETWEEN 'x' AND 'y' AND "
      "NOT e BETWEEN -1 AND 2 * 3 AND f = 1";
  auto stmt = Parse(kSql);
  ASSERT_TRUE(stmt.has_value()) << stmt.error().message;
  ASSERT_EQ(stmt->where.size(), 4U);
  const auto text = [&](SourceSpan span) { return kSql.substr(span.offset, span.length); };

  const auto* plain = std::get_if<BetweenExpr>(&stmt->where.front());
  ASSERT_NE(plain, nullptr);
  EXPECT_FALSE(plain->negated);
  EXPECT_EQ(text(plain->span), "b between 1 AND c + 2");
  EXPECT_EQ(text(plain->op_span), "between");
  EXPECT_EQ(text(plain->operand->span()), "b");
  EXPECT_EQ(text(plain->low->span()), "1");
  EXPECT_TRUE(std::holds_alternative<BinaryExpr>(*plain->high));

  const auto* negated = std::get_if<BetweenExpr>(&stmt->where[1]);
  ASSERT_NE(negated, nullptr);
  EXPECT_TRUE(negated->negated);
  EXPECT_EQ(text(negated->op_span), "NOT BETWEEN");
  EXPECT_EQ(text(negated->span), "d NOT BETWEEN 'x' AND 'y'");

  const auto* not_expr = std::get_if<UnaryExpr>(&stmt->where[2]);
  ASSERT_NE(not_expr, nullptr);
  EXPECT_EQ(not_expr->op, UnaryOp::kNot);
  const auto* under_not = std::get_if<BetweenExpr>(&*not_expr->operand);
  ASSERT_NE(under_not, nullptr);
  EXPECT_FALSE(under_not->negated);
  EXPECT_EQ(text(under_not->span), "e BETWEEN -1 AND 2 * 3");
  EXPECT_FALSE(EqualIgnoringSpans(stmt->where[1], stmt->where[2]));
  EXPECT_EQ(Cmp(stmt->where[3]).op, CompareOp::kEq);

  // Under OR, in CASE, and with parenthesized bounds; a comparison is not a bound.
  for (const char* sql : {"SELECT a FROM t WHERE b BETWEEN 1 AND 2 OR c BETWEEN (1 = 1) AND 3",
                          "SELECT CASE WHEN b BETWEEN 1 AND 2 THEN 1 END FROM t",
                          "SELECT a FROM t WHERE b BETWEEN (c AND d) AND 2"}) {
    EXPECT_TRUE(Parse(sql).has_value()) << sql;
  }
}

// column [NOT] IN (literal, ...) in WHERE; the span runs from the column to the closing paren.
TEST(ParserTest, In) {
  constexpr std::string_view kSql =
      "SELECT a FROM t WHERE b in (1, -2.5, 'x', DATE '2024-01-02') AND c NOT IN ('y') AND d = 1";
  auto stmt = Parse(kSql);
  ASSERT_TRUE(stmt.has_value()) << stmt.error().message;
  ASSERT_EQ(stmt->where.size(), 3U);
  const Comparison& in = Cmp(stmt->where[0]);
  EXPECT_EQ(in.op, CompareOp::kIn);
  ASSERT_EQ(in.list.size(), 4U);
  EXPECT_EQ(in.list[1].text, "2.5");
  EXPECT_TRUE(in.list[1].negative);
  EXPECT_EQ(in.list[2].kind, Literal::Kind::kString);
  EXPECT_EQ(in.list[3].kind, Literal::Kind::kDate);
  EXPECT_EQ(kSql.substr(in.span.offset, in.span.length), "b in (1, -2.5, 'x', DATE '2024-01-02')");
  EXPECT_EQ(Cmp(stmt->where[1]).op, CompareOp::kNotIn);
  EXPECT_EQ(Cmp(stmt->where[1]).list.size(), 1U);
  EXPECT_EQ(Cmp(stmt->where[2]).op, CompareOp::kEq);
}

// HAVING: a conjunction of an aggregate or a column <op> literal (comparisons, LIKE, IN), literal
// first normalized as in WHERE; with or without GROUP BY, before ORDER BY.
TEST(ParserTest, Having) {
  constexpr std::string_view kSql =
      "SELECT a, COUNT(*) AS c FROM t GROUP BY a HAVING count(*) > 1 AND 5 >= SUM(b) AND a "
      "NOT LIKE 'x%' AND c IN (1, 2) AND MIN(s) like 'y' ORDER BY a LIMIT 3";
  auto stmt = Parse(kSql);
  ASSERT_TRUE(stmt.has_value()) << stmt.error().message;
  ASSERT_EQ(stmt->having.size(), 5U);
  const HavingComparison& count = HavingCmp(stmt->having[0]);
  EXPECT_EQ(std::get<AggregateCall>(count.operand).kind, AggKind::kCountStar);
  EXPECT_EQ(count.op, CompareOp::kGt);
  EXPECT_EQ(count.literal.text, "1");
  EXPECT_EQ(At(kSql, count.span), "count(*) > 1");
  const HavingComparison& sum = HavingCmp(stmt->having[1]);
  EXPECT_EQ(std::get<AggregateCall>(sum.operand).kind, AggKind::kSum);
  EXPECT_EQ(sum.op, CompareOp::kLe) << "5 >= SUM(b) is SUM(b) <= 5";
  EXPECT_EQ(At(kSql, sum.span), "5 >= SUM(b)");
  EXPECT_EQ(std::get<ColumnRef>(HavingCmp(stmt->having[2]).operand).name, "a");
  EXPECT_EQ(HavingCmp(stmt->having[2]).op, CompareOp::kNotLike);
  EXPECT_EQ(At(kSql, HavingCmp(stmt->having[2]).span), "a NOT LIKE 'x%'");
  EXPECT_EQ(HavingCmp(stmt->having[3]).op, CompareOp::kIn);
  EXPECT_EQ(HavingCmp(stmt->having[3]).list.size(), 2U);
  EXPECT_EQ(At(kSql, HavingCmp(stmt->having[3]).span), "c IN (1, 2)");
  EXPECT_EQ(std::get<AggregateCall>(HavingCmp(stmt->having[4]).operand).kind, AggKind::kMin);
  EXPECT_EQ(HavingCmp(stmt->having[4]).op, CompareOp::kLike);
  EXPECT_EQ(At(kSql, stmt->having_span),
            "HAVING count(*) > 1 AND 5 >= SUM(b) AND a NOT LIKE 'x%' AND c IN (1, 2) AND MIN(s) "
            "like 'y'");
  EXPECT_EQ(stmt->order_by.size(), 1U);
  EXPECT_EQ(stmt->limit, 3);
  auto global = Parse("SELECT COUNT(*) FROM t WHERE b = 1 HAVING COUNT(DISTINCT b) <> 0");
  ASSERT_TRUE(global.has_value()) << global.error().message;
  EXPECT_TRUE(global->group_by.empty());
  ASSERT_EQ(global->having.size(), 1U);
  EXPECT_TRUE(std::get<AggregateCall>(HavingCmp(global->having[0]).operand).distinct);
  EXPECT_EQ(HavingCmp(global->having[0]).op, CompareOp::kNe);
}

// The expression grammar: precedence (OR < AND < NOT < comparisons < + - < * / // % < unary -),
// left associativity, and parentheses that group without leaving a node.
TEST(ParserTest, ExpressionPrecedenceAndAssociativity) {
  const auto canonical = [](std::string_view expr) {
    auto stmt = Parse("SELECT " + std::string(expr) + " FROM t");
    EXPECT_TRUE(stmt.has_value()) << expr << ": " << stmt.error().message;
    return stmt.has_value() ? ToSql(stmt->items[0].expr) : std::string();
  };
  EXPECT_EQ(canonical("a + b * c"), "a + b * c");
  EXPECT_EQ(canonical("(a + b) * c"), "(a + b) * c");
  EXPECT_EQ(canonical("a - b - c"), "a - b - c");
  EXPECT_EQ(canonical("a - (b - c)"), "a - (b - c)");
  EXPECT_EQ(canonical("a // 2 % 3 / 4"), "a // 2 % 3 / 4");
  EXPECT_EQ(canonical("((a))"), "a");
  EXPECT_EQ(canonical("-a * b"), "-(a) * b");
  EXPECT_EQ(canonical("-(a * b)"), "-(a * b)");
  EXPECT_EQ(canonical("a*-5"), "a * -5") << "a negative literal";
  EXPECT_EQ(canonical("a = 1 OR b = 2 AND c = 3"), "a = 1 OR b = 2 AND c = 3");
  EXPECT_EQ(canonical("(a = 1 OR b = 2) AND c = 3"), "(a = 1 OR b = 2) AND c = 3");
  EXPECT_EQ(canonical("NOT a = 1 AND b"), "NOT a = 1 AND b");
  EXPECT_EQ(canonical("NOT (a AND b)"), "NOT (a AND b)");
  EXPECT_EQ(canonical("a + 1 < b * 2"), "a + 1 < b * 2");
  EXPECT_EQ(canonical("(a < b) = (c < d)"), "(a < b) = (c < d)");
  EXPECT_EQ(canonical("-a::INT"), "-(CAST(a AS INT))") << "'::' binds tighter than the minus";
  EXPECT_EQ(canonical("a + b::int"), "a + CAST(b AS INT)");
  EXPECT_EQ(canonical("(a + b)::INT * c"), "CAST(a + b AS INT) * c");
  EXPECT_EQ(canonical("NOT a::BOOLEAN"), "NOT CAST(a AS BOOLEAN)");
  EXPECT_FALSE(Parse("SELECT x LIKE 'a' || 'b' FROM t").has_value()) << "|| stays unsupported";
  auto tree = Parse("SELECT a + b * c FROM t");
  ASSERT_TRUE(tree.has_value());
  const auto& add = std::get<BinaryExpr>(tree->items[0].expr);
  EXPECT_EQ(add.op, BinaryOp::kAdd);
  EXPECT_EQ(std::get<BinaryExpr>(*add.right).op, BinaryOp::kMultiply);
  EXPECT_EQ(At("SELECT a + b * c FROM t", add.op_span), "+");
  EXPECT_EQ(At("SELECT a + b * c FROM t", add.span), "a + b * c");
}

TEST(ParserTest, FunctionsCaseExtractAndExpressionOperands) {
  constexpr std::string_view kSql =
      "SELECT regexp_replace(url, '^x(.*)$', '\\1') AS k, f(), \"Quoted\"(a, 1), "
      "CASE WHEN a = 0 AND b = 0 THEN c ELSE '' END, CASE a WHEN 1 THEN 'one' END, "
      "extract(minute FROM ts), SUM(a + 1), COUNT(DISTINCT a % 7) FROM t "
      "WHERE a + 1 IN (b, 2 * c) AND lower(s) NOT LIKE '%x%' GROUP BY a - 1, k";
  auto stmt = Parse(kSql);
  ASSERT_TRUE(stmt.has_value()) << stmt.error().message;
  ASSERT_EQ(stmt->items.size(), 8U);
  const auto& replace = std::get<FunctionCall>(stmt->items[0].expr);
  EXPECT_EQ(replace.name, "regexp_replace");
  EXPECT_EQ(replace.args.size(), 3U);
  EXPECT_EQ(At(kSql, replace.name_span), "regexp_replace");
  EXPECT_TRUE(std::get<FunctionCall>(stmt->items[1].expr).args.empty());
  EXPECT_TRUE(std::get<FunctionCall>(stmt->items[2].expr).quoted);
  const auto& searched = std::get<CaseExpr>(stmt->items[3].expr);
  EXPECT_FALSE(searched.operand.has_value());
  ASSERT_EQ(searched.branches.size(), 1U);
  EXPECT_EQ(std::get<BinaryExpr>(*searched.branches[0].when).op, BinaryOp::kAnd);
  EXPECT_TRUE(searched.otherwise.has_value());
  const auto& simple = std::get<CaseExpr>(stmt->items[4].expr);
  EXPECT_TRUE(simple.operand.has_value());
  EXPECT_FALSE(simple.otherwise.has_value());
  const auto& extract = std::get<ExtractExpr>(stmt->items[5].expr);
  EXPECT_EQ(extract.field, "minute");
  EXPECT_EQ(At(kSql, extract.span), "extract(minute FROM ts)");
  const auto& sum = std::get<AggregateCall>(stmt->items[6].expr);
  EXPECT_EQ(sum.arg_column(), nullptr);
  const Expr* sum_arg = sum.arg.has_value() ? &**sum.arg : nullptr;
  ASSERT_NE(sum_arg, nullptr);
  EXPECT_EQ(std::get<BinaryExpr>(*sum_arg).op, BinaryOp::kAdd);
  EXPECT_TRUE(std::get<AggregateCall>(stmt->items[7].expr).distinct);
  ASSERT_EQ(stmt->where.size(), 2U);
  EXPECT_FALSE(AsComparison(stmt->where[0]).has_value()) << "not column <op> literal";
  EXPECT_EQ(std::get<InExpr>(stmt->where[0]).list.size(), 2U);
  EXPECT_TRUE(std::get<LikeExpr>(stmt->where[1]).negated);
  ASSERT_EQ(stmt->group_by.size(), 2U);
  EXPECT_EQ(std::get<BinaryExpr>(stmt->group_by[0]).op, BinaryOp::kSubtract);
  auto again = Parse(ToSql(*stmt));
  ASSERT_TRUE(again.has_value()) << ToSql(*stmt) << ": " << again.error().message;
  EXPECT_TRUE(EqualIgnoringSpans(*stmt, *again)) << ToSql(*stmt);
}

// LEFT and RIGHT start joins, not clauses: after a comma of the select list, GROUP BY or ORDER BY,
// left( and right( are calls (DuckDB's string functions), not a trailing comma.
TEST(ParserTest, LeftAndRightCallsAfterListCommas) {
  auto stmt =
      Parse("SELECT a, left(s, 1) FROM events GROUP BY a, right(s, 1) ORDER BY a, LEFT(s, 2)");
  ASSERT_TRUE(stmt.has_value()) << stmt.error().message;
  ASSERT_EQ(stmt->items.size(), 2U);
  EXPECT_EQ(std::get<FunctionCall>(stmt->items[1].expr).name, "left");
  ASSERT_EQ(stmt->group_by.size(), 2U);
  EXPECT_EQ(std::get<FunctionCall>(stmt->group_by[1]).name, "right");
  ASSERT_EQ(stmt->order_by.size(), 2U);
  EXPECT_EQ(std::get<FunctionCall>(stmt->order_by[1].expr).name, "LEFT");
}

// CAST(x AS T), TRY_CAST(x AS T) and x::T are one node: the type upper-cased with its integer
// parameters as written, the spelling not recorded. '::' applies to any primary expression, and a
// cast's span starts at the primary's first token.
TEST(ParserTest, Casts) {
  constexpr std::string_view kSql =
      "SELECT CAST(a AS decimal( 15 ,2 )) AS x, try_cast(b as date), c::int, "
      "(a + 1)::BIGINT::Varchar, COUNT(*)::BIGINT, CASE WHEN a = 1 THEN 2 END::INTEGER, "
      "DATE '2024-01-01'::VARCHAR, SUM(a::BIGINT), try_cast, x::DATE d FROM t "
      "WHERE d >= '2024-01-31'::Date AND e IN (CAST('2024-01-01' AS DATE), 1)";
  auto stmt = Parse(kSql);
  ASSERT_TRUE(stmt.has_value()) << stmt.error().message;
  ASSERT_EQ(stmt->items.size(), 10U);
  const auto cast_of = [&](const Expr& e) -> const CastExpr& { return std::get<CastExpr>(e); };
  const CastExpr& decimal = cast_of(stmt->items[0].expr);
  EXPECT_EQ(decimal.type, "DECIMAL");
  EXPECT_EQ(decimal.type_params, (std::vector<std::string>{"15", "2"}));
  EXPECT_FALSE(decimal.try_cast);
  EXPECT_EQ(At(kSql, decimal.op_span), "CAST");
  EXPECT_EQ(At(kSql, decimal.type_span), "decimal( 15 ,2 )");
  EXPECT_EQ(At(kSql, decimal.span), "CAST(a AS decimal( 15 ,2 ))");
  EXPECT_EQ(stmt->items[0].alias, std::optional<std::string>("x"));
  const CastExpr& try_date = cast_of(stmt->items[1].expr);
  EXPECT_TRUE(try_date.try_cast);
  EXPECT_EQ(try_date.type, "DATE");
  EXPECT_EQ(At(kSql, try_date.op_span), "try_cast");
  const CastExpr& operator_int = cast_of(stmt->items[2].expr);
  EXPECT_EQ(operator_int.type, "INT");
  EXPECT_EQ(At(kSql, operator_int.op_span), "::");
  EXPECT_EQ(At(kSql, operator_int.span), "c::int");
  const CastExpr& chain = cast_of(stmt->items[3].expr);
  EXPECT_EQ(chain.type, "VARCHAR");
  const CastExpr& inner = cast_of(*chain.operand);
  EXPECT_EQ(inner.type, "BIGINT");
  EXPECT_TRUE(std::holds_alternative<BinaryExpr>(*inner.operand));
  EXPECT_EQ(At(kSql, inner.span), "(a + 1)::BIGINT");
  EXPECT_EQ(At(kSql, chain.span), "(a + 1)::BIGINT::Varchar");
  EXPECT_TRUE(std::holds_alternative<AggregateCall>(*cast_of(stmt->items[4].expr).operand));
  EXPECT_TRUE(std::holds_alternative<CaseExpr>(*cast_of(stmt->items[5].expr).operand));
  EXPECT_EQ(std::get<Literal>(*cast_of(stmt->items[6].expr).operand).kind, Literal::Kind::kDate);
  const auto& sum = std::get<AggregateCall>(stmt->items[7].expr);
  const Expr* sum_arg = sum.arg.has_value() ? &**sum.arg : nullptr;
  ASSERT_NE(sum_arg, nullptr);
  EXPECT_TRUE(std::holds_alternative<CastExpr>(*sum_arg));
  EXPECT_EQ(std::get<ColumnRef>(stmt->items[8].expr).name, "try_cast");
  EXPECT_EQ(cast_of(stmt->items[9].expr).type, "DATE");
  EXPECT_EQ(stmt->items[9].alias, std::optional<std::string>("d"));
  ASSERT_EQ(stmt->where.size(), 2U);
  const auto& ge = std::get<BinaryExpr>(stmt->where[0]);
  EXPECT_EQ(std::get<Literal>(*cast_of(*ge.right).operand).text, "2024-01-31");
  EXPECT_TRUE(std::holds_alternative<CastExpr>(std::get<InExpr>(stmt->where[1]).list.at(0)));
  auto again = Parse(ToSql(*stmt));
  ASSERT_TRUE(again.has_value()) << ToSql(*stmt) << ": " << again.error().message;
  EXPECT_TRUE(EqualIgnoringSpans(*stmt, *again)) << ToSql(*stmt);
}

// A minus is not folded into a number that '::' follows: -1::INTEGER is -(CAST(1 AS INTEGER)), as
// in DuckDB, while (-1)::INTEGER and CAST(-1 AS INTEGER) cast the negative literal.
TEST(ParserTest, MinusBeforeCastIsNotFoldedIntoTheNumber) {
  const auto item = [](std::string_view expr) {
    auto stmt = Parse("SELECT " + std::string(expr) + " FROM t");
    EXPECT_TRUE(stmt.has_value()) << expr << ": " << stmt.error().message;
    return stmt.has_value() ? stmt->items.at(0).expr : Expr(ColumnRef{});
  };
  const Expr minus = item("-1::INTEGER");
  const auto& unary = std::get<UnaryExpr>(minus);
  EXPECT_EQ(unary.op, UnaryOp::kNegate);
  const auto& one = std::get<Literal>(*std::get<CastExpr>(*unary.operand).operand);
  EXPECT_FALSE(one.negative);
  EXPECT_EQ(one.text, "1");
  EXPECT_EQ(ToSql(minus), "-(CAST(1 AS INTEGER))");
  EXPECT_TRUE(EqualIgnoringSpans(item("(-1)::INTEGER"), item("CAST(-1 AS INTEGER)")));
  EXPECT_TRUE(std::get<Literal>(*std::get<CastExpr>(item("(-1)::INTEGER")).operand).negative);
  EXPECT_EQ(ToSql(item("- 1.5e3::DOUBLE")), "-(CAST(1.5e3 AS DOUBLE))");
  EXPECT_EQ(ToSql(item("a-1::INT")), "a - CAST(1 AS INT)");
  EXPECT_EQ(ToSql(item("- -1::INT")), "-(-(CAST(1 AS INT)))");
  EXPECT_TRUE(std::get<Literal>(item("-1")).negative);  // without '::' the minus still folds
  EXPECT_EQ(ToSql(item("-1 + 2")), "-1 + 2");
  auto where = Parse("SELECT a FROM t WHERE a<=-1::INT");
  ASSERT_TRUE(where.has_value()) << where.error().message;
  EXPECT_EQ(ToSql(where->where.at(0)), "a <= -(CAST(1 AS INT))");
}

// WHERE and HAVING split their top-level AND chain; a parenthesized AND, or an OR at the top,
// makes one conjunct.
TEST(ParserTest, PredicatesSplitAtTheirTopLevelAndChain) {
  auto flat = Parse("SELECT a FROM t WHERE a = 1 AND (b = 2) AND c = 3");
  ASSERT_TRUE(flat.has_value());
  EXPECT_EQ(flat->where.size(), 3U);
  auto nested = Parse("SELECT a FROM t WHERE (a = 1 AND b = 2) AND c = 3");
  ASSERT_TRUE(nested.has_value());
  ASSERT_EQ(nested->where.size(), 2U);
  EXPECT_EQ(std::get<BinaryExpr>(nested->where[0]).op, BinaryOp::kAnd);
  auto with_or = Parse("SELECT a FROM t WHERE a = 1 AND b = 2 OR c = 3 AND d = 4");
  ASSERT_TRUE(with_or.has_value());
  ASSERT_EQ(with_or->where.size(), 1U);
  const auto& top = std::get<BinaryExpr>(with_or->where[0]);
  EXPECT_EQ(top.op, BinaryOp::kOr);
  EXPECT_EQ(std::get<BinaryExpr>(*top.left).op, BinaryOp::kAnd);
  EXPECT_EQ(std::get<BinaryExpr>(*top.right).op, BinaryOp::kAnd);
  auto having = Parse("SELECT a FROM t GROUP BY a HAVING COUNT(*) > 1 AND NOT MIN(b) = 2");
  ASSERT_TRUE(having.has_value());
  ASSERT_EQ(having->having.size(), 2U);
  EXPECT_EQ(std::get<UnaryExpr>(having->having[1]).op, UnaryOp::kNot);
  // A long chain builds no deep tree: it is no deeper than one conjunct.
  std::string chain = "SELECT a FROM t WHERE a = 0";
  for (int i = 1; i < 1000; ++i) {
    chain += " AND a <> " + std::to_string(i);
  }
  auto wide = Parse(chain);
  ASSERT_TRUE(wide.has_value()) << wide.error().message;
  EXPECT_EQ(wide->where.size(), 1000U);
}

// Every level of an expression tree counts against the depth limit (256).
TEST(ParserTest, ExpressionDepthIsLimited) {
  const auto nested = [](std::size_t depth) {
    return "SELECT " + std::string(depth, '(') + "a" + std::string(depth, ')') + " FROM t";
  };
  EXPECT_TRUE(Parse(nested(200)).has_value());
  auto deep = Parse(nested(300));
  ASSERT_FALSE(deep.has_value());
  EXPECT_EQ(deep.error().kind, ParseError::Kind::kUnsupported);
  EXPECT_TRUE(deep.error().message.starts_with("expressions deeper than 256 levels"))
      << deep.error().message;
  std::string sum = "SELECT a";
  for (int i = 0; i < 300; ++i) {
    sum += " + a";
  }
  auto chain = Parse(sum + " FROM t");
  ASSERT_FALSE(chain.has_value()) << "an operator chain is a deep tree too";
  EXPECT_EQ(chain.error().kind, ParseError::Kind::kUnsupported);
  std::string ors = "SELECT a FROM t WHERE a = 0";
  for (int i = 0; i < 300; ++i) {
    ors += " OR a = 1";
  }
  EXPECT_FALSE(Parse(ors).has_value());
  // An AND chain is free until an OR makes it an operand: then its ANDs count too.
  std::string and_chain = "SELECT a FROM t WHERE a = 0";
  for (int i = 0; i < 300; ++i) {
    and_chain += " AND a = 1";
  }
  EXPECT_TRUE(Parse(and_chain).has_value());
  auto ored = Parse(and_chain + " OR a = 2");
  ASSERT_FALSE(ored.has_value());
  EXPECT_EQ(ored.error().kind, ParseError::Kind::kUnsupported);
  // The depth of one clause does not carry over to the next.
  std::string then = "SELECT a FROM t WHERE a = 0";
  for (int i = 0; i < 200; ++i) {
    then += " OR a = 1";
  }
  then += " GROUP BY " + std::string(100, '(') + "a" + std::string(100, ')');
  EXPECT_TRUE(Parse(then).has_value());
}

// The first n at which `sql(n)` is rejected, after checking that every accepted n round-trips
// through the canonical form within the depth limit and that every n from there on is the depth
// error. 0 when none up to `max` is rejected.
std::size_t FirstTooDeep(const std::function<std::string(std::size_t)>& sql,
                         std::size_t max = 400) {
  const auto accepted = [&](std::size_t n) { return Parse(sql(n)).has_value(); };
  // Acceptance shrinks as n grows: find the first rejected n by bisection (a sweep of every n
  // costs quadratic time, too slow under the sanitizers), then check the window around it and a
  // sample below it.
  if (accepted(max)) {
    ADD_FAILURE() << "nothing rejected up to n=" << max;
    return 0;
  }
  std::size_t lo = 0;    // accepted, or 0
  std::size_t hi = max;  // rejected
  while (hi - lo > 1) {
    const std::size_t mid = lo + ((hi - lo) / 2);
    (accepted(mid) ? lo : hi) = mid;
  }
  const std::size_t first = hi;
  for (std::size_t n = first; n <= std::min(max, first + 3); ++n) {
    auto stmt = Parse(sql(n));
    if (stmt.has_value()) {
      ADD_FAILURE() << "accepted after a rejection: n=" << n;
      continue;
    }
    EXPECT_EQ(stmt.error().kind, ParseError::Kind::kUnsupported) << stmt.error().message;
    EXPECT_TRUE(stmt.error().message.starts_with("expressions deeper than 256 levels"))
        << stmt.error().message;
  }
  std::vector<std::size_t> below;
  for (std::size_t n = 1; n < first; n = (n * 2) + 1) {
    below.push_back(n);
  }
  for (std::size_t n = first > 4 ? first - 4 : 1; n < first; ++n) {
    below.push_back(n);
  }
  for (const std::size_t n : below) {
    auto stmt = Parse(sql(n));
    if (!stmt.has_value()) {
      ADD_FAILURE() << "rejected below the first rejection " << first << ": n=" << n << ": "
                    << stmt.error().message;
      continue;
    }
    EXPECT_LE(Depth(*stmt), kMaxExpressionDepth) << "n=" << n;
    const std::string canonical = ToSql(*stmt);
    auto again = Parse(canonical);
    if (!again.has_value()) {
      ADD_FAILURE() << "n=" << n
                    << ": the canonical form does not parse: " << again.error().message;
      continue;
    }
    EXPECT_TRUE(EqualIgnoringSpans(*stmt, *again)) << "n=" << n;
    EXPECT_EQ(ToSql(*again), canonical) << "n=" << n;
  }
  return first;
}

std::string Repeat(std::string_view text, std::size_t n) {
  std::string out;
  for (std::size_t i = 0; i < n; ++i) {
    out += text;
  }
  return out;
}

// Every accepted statement's canonical form parses (the round trip the fuzzer checks), also next
// to the depth limit: a top-level OR in WHERE or HAVING reads back as parsed, and a NOT that ToSql
// parenthesizes as an operand counts that level. Written with the parentheses, the same tree is
// rejected at the same n.
TEST(ParserTest, CanonicalFormStaysWithinTheDepthLimit) {
  const auto where = [](std::string predicate) {
    return "SELECT a FROM t WHERE " + std::move(predicate);
  };
  // A top-level OR, also after a long AND chain whose conjuncts it makes one tree.
  EXPECT_NE(FirstTooDeep([&](std::size_t n) { return where("a = 0" + Repeat(" OR a = 1", n)); }),
            0U);
  EXPECT_NE(FirstTooDeep([](std::size_t n) {
              return "SELECT a FROM t GROUP BY a HAVING a = 0" + Repeat(" OR a = 1", n);
            }),
            0U);
  EXPECT_NE(
      FirstTooDeep([&](std::size_t n) {
        return where(Repeat("a AND ", 200) + Repeat("f(", n) + "a" + std::string(n, ')') + " OR b");
      }),
      0U);
  EXPECT_NE(
      FirstTooDeep([&](std::size_t n) {
        return where(Repeat("a AND ", n) + Repeat("f(", 50) + "a" + std::string(50, ')') + " OR b");
      }),
      0U);
  // ON conditions, also with a top-level OR before the next join (written bare: it reads back as
  // parsed, the join keywords end it).
  const auto on = [](std::string predicate) {
    return "SELECT a FROM t JOIN u ON " + std::move(predicate) + " LEFT JOIN v ON v.a = 1";
  };
  EXPECT_NE(FirstTooDeep([&](std::size_t n) { return on("a = 0" + Repeat(" OR a = 1", n)); }), 0U);
  EXPECT_NE(
      FirstTooDeep([&](std::size_t n) {
        return on(Repeat("a AND ", 100) + Repeat("f(", n) + "a" + std::string(n, ')') + " OR b");
      }),
      0U);
  EXPECT_EQ(FirstTooDeep([&](std::size_t n) { return on("a = " + Repeat("NOT a = ", n) + "a"); }),
            FirstTooDeep([&](std::size_t n) {
              return on("a = " + Repeat("(NOT a = ", n) + "a" + std::string(n, ')'));
            }));
  // NOT as the right operand of a comparison or arithmetic, a LIKE pattern and BETWEEN bounds,
  // bare and parenthesized; under a unary minus it gets no second pair.
  struct Family {
    std::string_view name;
    std::function<std::string(std::size_t)> bare;
    std::function<std::string(std::size_t)> parenthesized;
  };
  for (const Family& f : {
           Family{
               .name = "=",
               .bare = [&](std::size_t n) { return where("a = " + Repeat("NOT a = ", n) + "a"); },
               .parenthesized =
                   [&](std::size_t n) {
                     return where("a = " + Repeat("(NOT a = ", n) + "a" + std::string(n, ')'));
                   }},
           Family{
               .name = "+",
               .bare =
                   [&](std::size_t n) { return where("a = 1 + " + Repeat("NOT a + ", n) + "a"); },
               .parenthesized =
                   [&](std::size_t n) {
                     return where("a = 1 + " + Repeat("(NOT a + ", n) + "a" + std::string(n, ')'));
                   }},
           Family{
               .name = "LIKE",
               .bare =
                   [&](std::size_t n) { return where("a LIKE " + Repeat("NOT a LIKE ", n) + "a"); },
               .parenthesized =
                   [&](std::size_t n) {
                     return where("a LIKE " + Repeat("(NOT a LIKE ", n) + "a" +
                                  std::string(n, ')'));
                   }},
           Family{.name = "BETWEEN low",
                  .bare =
                      [&](std::size_t n) {
                        return where("a BETWEEN " + Repeat("NOT a BETWEEN ", n) + "a" +
                                     Repeat(" AND 1", n + 1));
                      },
                  .parenthesized =
                      [&](std::size_t n) {
                        return where("a BETWEEN " + Repeat("(NOT a BETWEEN ", n) + "a" +
                                     Repeat(" AND 1)", n) + " AND 1");
                      }},
           Family{.name = "BETWEEN high",
                  .bare =
                      [&](std::size_t n) {
                        return where("a BETWEEN 0 AND " + Repeat("NOT a BETWEEN 0 AND ", n) + "a");
                      },
                  .parenthesized =
                      [&](std::size_t n) {
                        return where("a BETWEEN 0 AND " + Repeat("(NOT a BETWEEN 0 AND ", n) + "a" +
                                     std::string(n, ')'));
                      }},
           // A NOT after a minus's operand is not the minus's operand: it counts.
           Family{
               .name = "after minus",
               .bare = [&](std::size_t n) { return where("-a = " + Repeat("NOT -a = ", n) + "a"); },
               .parenthesized =
                   [&](std::size_t n) {
                     return where("-a = " + Repeat("(NOT -a = ", n) + "a" + std::string(n, ')'));
                   }},
           Family{.name = "minus",
                  .bare = [](std::size_t n) { return "SELECT " + Repeat("-NOT ", n) + "a FROM t"; },
                  .parenthesized =
                      [](std::size_t n) {
                        return "SELECT " + Repeat("-(NOT ", n) + "a" + std::string(n, ')') +
                               " FROM t";
                      }},
       }) {
    const std::size_t bare = FirstTooDeep(f.bare);
    EXPECT_NE(bare, 0U) << f.name;
    EXPECT_EQ(bare, FirstTooDeep(f.parenthesized)) << f.name;
  }
}

// Every level of the tree counts, also where a chain's operators push its left operand (and its
// earlier right operands) down: without parentheses or casts, the deepest accepted tree has exactly
// kMaxExpressionDepth levels.
TEST(ParserTest, OperatorChainsCountTheirOperandsDepth) {
  const auto calls = [](std::size_t k) { return Repeat("f(", k) + "a" + std::string(k, ')'); };
  const auto select = [](const std::string& expr) { return "SELECT " + expr + " FROM t"; };
  const auto where = [](const std::string& predicate) {
    return "SELECT a FROM t WHERE " + predicate;
  };
  const auto having = [](const std::string& predicate) {
    return "SELECT a FROM t GROUP BY a HAVING " + predicate;
  };
  const auto on = [](const std::string& predicate) {
    return "SELECT a FROM t, u INNER JOIN v ON " + predicate + " CROSS JOIN w";
  };
  const std::vector<std::pair<std::string_view, std::function<std::string(std::size_t)>>> exact = {
      {"ON: deep conjunct, ANDs, OR",
       [&](std::size_t n) { return on(calls(200) + " = 1" + Repeat(" AND a", n) + " OR b"); }},
      {"ON: OR chain after a deep conjunct",
       [&](std::size_t n) { return on(calls(200) + " = 1" + Repeat(" OR a = 1", n)); }},
      {"deep left operand", [&](std::size_t n) { return select(calls(200) + Repeat(" + 1", n)); }},
      {"deep early right operand",
       [&](std::size_t n) { return select("a + " + calls(200) + Repeat(" + 1", n)); }},
      {"long chain, deep last operand",
       [&](std::size_t n) { return select("a" + Repeat(" + 1", 100) + " + " + calls(n)); }},
      {"deep conjunct, ANDs, OR",
       [&](std::size_t n) { return where(calls(200) + " = 1" + Repeat(" AND a", n) + " OR b"); }},
      {"ANDs, deep conjunct, OR",
       [&](std::size_t n) { return where(Repeat("a AND ", n) + calls(200) + " = 1 OR b"); }},
      {"OR chain after a deep conjunct",
       [&](std::size_t n) { return having(calls(200) + " = 1" + Repeat(" OR a = 1", n)); }},
  };
  for (const auto& [name, sql] : exact) {
    const std::size_t first = FirstTooDeep(sql);
    ASSERT_GT(first, 1U) << name;
    auto deepest = Parse(sql(first - 1));
    ASSERT_TRUE(deepest.has_value()) << name;
    EXPECT_EQ(Depth(*deepest), kMaxExpressionDepth) << name;
  }
  // Parentheses, casts and minuses count levels that are no nodes: those trees stay shallower.
  for (const auto& sql : std::vector<std::function<std::string(std::size_t)>>{
           [](std::size_t n) {
             std::string text = "SELECT " + std::string(n, '(') + "a";
             for (std::size_t i = 0; i < n; ++i) {
               text += ")" + Repeat(" + 1", n);
             }
             return text + " FROM t";
           },
           [&](std::size_t n) { return select("-" + calls(100) + "::INT" + Repeat(" * 2", n)); },
       }) {
    EXPECT_NE(FirstTooDeep(sql), 0U);
  }
}

// The levels that the canonical form adds count too (x::T is CAST(x AS T), -x is -(x)), so that
// the canonical form of every accepted query parses.
TEST(ParserTest, CanonicalLevelsCountAgainstTheDepthLimit) {
  const auto calls = [](std::size_t n, std::string_view before, std::string_view after) {
    std::string sql = "SELECT " + std::string(before);
    for (std::size_t i = 0; i < n; ++i) {
      sql += "f(";
    }
    sql += "a" + std::string(n, ')') + std::string(after) + " FROM t";
    return sql;
  };
  const auto depth_error = [](const std::string& sql) {
    auto result = Parse(sql);
    return !result.has_value() && result.error().kind == ParseError::Kind::kUnsupported &&
           result.error().message.starts_with("expressions deeper than 256 levels");
  };
  for (const auto& [before, after] : {std::pair<std::string_view, std::string_view>{"", "::INT"},
                                      std::pair<std::string_view, std::string_view>{"-", ""}}) {
    const std::string fits = calls(254 - before.size(), before, after);
    auto stmt = Parse(fits);
    ASSERT_TRUE(stmt.has_value()) << stmt.error().message;
    auto again = Parse(ToSql(*stmt));
    ASSERT_TRUE(again.has_value()) << again.error().message;
    EXPECT_TRUE(EqualIgnoringSpans(*stmt, *again));
    EXPECT_TRUE(depth_error(calls(255 - before.size(), before, after))) << before << after;
  }
  EXPECT_TRUE(Parse(calls(255, "", "")).has_value()) << "no cast: one level less";
  // A minus before parentheses adds no level: its canonical form keeps them.
  const std::string parenthesized = calls(253, "-(", ")");
  auto negated = Parse(parenthesized);
  ASSERT_TRUE(negated.has_value()) << negated.error().message;
  EXPECT_EQ(ToSql(*negated), parenthesized);
  EXPECT_TRUE(depth_error(calls(254, "-(", ")")));
  std::string chain = "SELECT a";
  for (int i = 0; i < 300; ++i) {
    chain += "::INT";
  }
  auto chained = Parse(chain + " FROM t");
  ASSERT_FALSE(chained.has_value());
  EXPECT_TRUE(depth_error(chain + " FROM t"));
  EXPECT_EQ(chained.error().span.offset, std::string("SELECT a").size() + (std::size_t{255} * 5));
  std::string casts = "SELECT ";
  for (int i = 0; i < 300; ++i) {
    casts += "CAST(";
  }
  EXPECT_TRUE(depth_error(casts + "a"));
}

// The query of a derived table or of a CTE is one level below the statement that holds it, so
// nested queries count against the depth limit together with the expressions inside them, and
// Depth() agrees with the parser: the deepest accepted statement of each family is exactly 256
// levels deep.
TEST(ParserTest, NestedQueriesCountTowardTheDepthLimit) {
  // n derived tables in each other, around a query without expressions: n levels.
  const auto derived = [](std::size_t n) {
    return "SELECT * FROM " + Repeat("(SELECT * FROM ", n) + "t" + std::string(n, ')');
  };
  // n CTEs in each other's queries, around SELECT a: n + 1 levels.
  const auto ctes = [](std::size_t n) {
    return Repeat("WITH c AS (", n) + "SELECT a FROM t" + Repeat(") SELECT a FROM c", n);
  };
  // A call chain of k levels in a derived table: k + 1 levels.
  const auto calls = [](std::size_t k) {
    return "SELECT * FROM (SELECT " + Repeat("f(", k) + "a" + std::string(k, ')') + " FROM t)";
  };
  // Derived tables and CTEs in turn, around SELECT a: n + 1 levels.
  const auto mixed = [](std::size_t n) {
    std::string sql = "SELECT a FROM t";
    for (std::size_t i = 1; i <= n; ++i) {
      sql = i % 2 == 1 ? "SELECT * FROM (" + sql + ") AS d"
                       : "WITH c AS (" + sql + ") SELECT a FROM c";
    }
    return sql;
  };
  // WHERE conjuncts and an ON condition in nested queries.
  const auto predicates = [](std::size_t n) {
    return Repeat("SELECT a FROM t JOIN (", n) + "SELECT a FROM t WHERE a = 1" +
           Repeat(") AS x ON a = b WHERE c = 1", n);
  };
  const std::vector<
      std::tuple<std::string_view, std::function<std::string(std::size_t)>, std::size_t>>
      families = {{"derived tables", derived, 257},
                  {"CTEs", ctes, 256},
                  {"calls in a derived table", calls, 255},
                  {"derived tables and CTEs", mixed, 256},
                  {"predicates", predicates, 255}};
  for (const auto& [name, sql, first] : families) {
    EXPECT_EQ(FirstTooDeep(sql), first) << name;
    auto deepest = Parse(sql(first - 1));
    ASSERT_TRUE(deepest.has_value()) << name << ": " << deepest.error().message;
    EXPECT_EQ(Depth(*deepest), kMaxExpressionDepth) << name;
  }
  // The depth error points at the '(' of the query one level too deep, before its query.
  auto chain = Parse(derived(300));
  ASSERT_FALSE(chain.has_value());
  EXPECT_EQ(chain.error().span, (SourceSpan{.offset = 14 + (256 * 15), .length = 1}));
  // A column alias list adds no level.
  EXPECT_EQ(FirstTooDeep([](std::size_t n) {
              return Repeat("WITH c(x, y) AS (", n) + "SELECT a FROM t" +
                     Repeat(") SELECT x FROM c", n);
            }),
            256U);
  EXPECT_EQ(FirstTooDeep([](std::size_t n) {
              return "SELECT * FROM " + Repeat("(SELECT * FROM ", n) + "t" +
                     Repeat(") AS s(x, y)", n);
            }),
            257U);
}

// Literals are select items (constants), and in GROUP BY and ORDER BY positions or constants; the
// binder tells them apart.
TEST(ParserTest, ConstantsAndPositions) {
  const std::string sql =
      "SELECT 1, -2 AS m, 'x', DATE '2024-01-02', a FROM t GROUP BY 1, a, 'k' ORDER BY 2 DESC, 'z'";
  auto stmt = Parse(sql);
  ASSERT_TRUE(stmt.has_value()) << stmt.error().message;
  ASSERT_EQ(stmt->items.size(), 5U);
  EXPECT_EQ(std::get<Literal>(stmt->items[0].expr).text, "1");
  EXPECT_TRUE(std::get<Literal>(stmt->items[1].expr).negative);
  EXPECT_EQ(stmt->items[1].alias, "m");
  EXPECT_EQ(At(sql, stmt->items[1].span), "-2 AS m");
  EXPECT_EQ(std::get<Literal>(stmt->items[2].expr).kind, Literal::Kind::kString);
  EXPECT_EQ(std::get<Literal>(stmt->items[3].expr).kind, Literal::Kind::kDate);
  ASSERT_EQ(stmt->group_by.size(), 3U);
  EXPECT_EQ(std::get<Literal>(stmt->group_by[0]).text, "1");
  EXPECT_EQ(std::get<ColumnRef>(stmt->group_by[1]).name, "a");
  EXPECT_EQ(std::get<Literal>(stmt->group_by[2]).kind, Literal::Kind::kString);
  ASSERT_EQ(stmt->order_by.size(), 2U);
  EXPECT_EQ(std::get<Literal>(stmt->order_by[0].expr).text, "2");
  EXPECT_TRUE(stmt->order_by[0].descending);
  EXPECT_EQ(At(sql, stmt->order_by[0].span), "2 DESC");
}

TEST(ParserTest, CountDistinct) {
  auto stmt = Parse("SELECT count( distinct user_id ), COUNT(user_id) FROM events");
  ASSERT_TRUE(stmt.has_value()) << stmt.error().message;
  const auto& distinct = std::get<AggregateCall>(stmt->items[0].expr);
  EXPECT_EQ(distinct.kind, AggKind::kCount);
  EXPECT_TRUE(distinct.distinct);
  EXPECT_EQ(ArgName(distinct), "user_id");
  EXPECT_FALSE(std::get<AggregateCall>(stmt->items[1].expr).distinct);
}

// NULLS, FIRST and LAST are not reserved: they stay usable as names.
TEST(ParserTest, OrderByModifierWordsAreNames) {
  auto stmt = Parse("SELECT nulls, first, last FROM events ORDER BY nulls NULLS LAST, first");
  ASSERT_TRUE(stmt.has_value()) << stmt.error().message;
  ASSERT_EQ(stmt->order_by.size(), 2U);
  EXPECT_EQ(std::get<ColumnRef>(stmt->order_by[0].expr).name, "nulls");
  EXPECT_EQ(stmt->order_by[0].nulls, NullsOrder::kLast);
  EXPECT_EQ(std::get<ColumnRef>(stmt->order_by[1].expr).name, "first");
}

TEST(ParserTest, KeywordsAreCaseInsensitive) {
  auto stmt = Parse("sElEcT cOuNt(*) aS n FrOm events wHeRe a = 1 AnD b = 2 LiMiT 5");
  ASSERT_TRUE(stmt.has_value()) << stmt.error().message;
  EXPECT_EQ(stmt->items.size(), 1U);
  EXPECT_EQ(stmt->items[0].alias, std::optional<std::string>("n"));
  EXPECT_EQ(stmt->where.size(), 2U);
  EXPECT_EQ(stmt->limit, std::optional<std::int64_t>(5));
}

TEST(ParserTest, CommentsAndWhitespaceAnywhere) {
  constexpr std::string_view kSql =
      "-- leading comment\n/*x*/SELECT/*y*/a/*z*/,\tb\r\nFROM\vt\fWHERE a\n=\n1 -- c\nLIMIT/**/2 ; "
      "-- trailing\n";
  auto stmt = Parse(kSql);
  ASSERT_TRUE(stmt.has_value()) << stmt.error().message;
  EXPECT_EQ(stmt->items.size(), 2U);
  EXPECT_EQ(stmt->where.size(), 1U);
  EXPECT_EQ(stmt->limit, std::optional<std::int64_t>(2));
  // The statement span starts at SELECT and ends at the last token of the query (before ';').
  EXPECT_EQ(kSql.substr(stmt->span.offset, 6), "SELECT");
  EXPECT_EQ(kSql.substr(stmt->span.offset + stmt->span.length - 1, 1), "2");
}

TEST(ParserTest, StatementSpan) {
  constexpr std::string_view kSql = "  SELECT COUNT(*) FROM events;  ";
  auto stmt = Parse(kSql);
  ASSERT_TRUE(stmt.has_value());
  EXPECT_EQ(At(kSql, stmt->span), "SELECT COUNT(*) FROM events");
}

TEST(ParserTest, StarAndLimitSpans) {
  constexpr std::string_view kSql = "SELECT /* all */ * FROM events LIMIT -- n\n 10 ;";
  auto stmt = Parse(kSql);
  ASSERT_TRUE(stmt.has_value()) << stmt.error().message;
  EXPECT_EQ(At(kSql, stmt->star_span), "*");
  EXPECT_EQ(At(kSql, stmt->limit_span), "LIMIT -- n\n 10");
  auto no_limit = Parse("SELECT a FROM t");
  ASSERT_TRUE(no_limit.has_value());
  EXPECT_EQ(no_limit->star_span, SourceSpan{});
  EXPECT_EQ(no_limit->limit_span, SourceSpan{});
}

TEST(ParserTest, IsReservedWord) {
  for (const std::string_view word : {"from", "FROM", "Select", "and", "where", "limit", "null"}) {
    EXPECT_TRUE(IsReservedWord(word)) << word;
  }
  for (const std::string_view word : {"", "x", "date", "count", "sum", "year", "name", "events",
                                      "from_", "fr om", "é", "a_very_long_identifier_name"}) {
    EXPECT_FALSE(IsReservedWord(word)) << word;
  }
}

TEST(ParserTest, TrailingSemicolon) {
  EXPECT_TRUE(Parse("SELECT a FROM t;").has_value());
  EXPECT_TRUE(Parse("SELECT a FROM t ;\n").has_value());
  EXPECT_TRUE(Parse("SELECT a FROM t; -- done").has_value());
}

// Regression: a line comment used to run to the next \n only, so a WHERE after a bare \r (old Mac
// line endings) was silently dropped and the query answered without its filter.
TEST(ParserTest, LineCommentEndsAtCarriageReturn) {
  for (const std::string_view sql :
       {"SELECT a FROM t -- all rows?\rWHERE a = 1", "SELECT a FROM t -- c\r\nWHERE a = 1",
        "SELECT a FROM t --\rWHERE a = 1"}) {
    auto stmt = Parse(sql);
    ASSERT_TRUE(stmt.has_value()) << stmt.error().message;
    ASSERT_EQ(stmt->where.size(), 1U) << testing::PrintToString(sql);
    EXPECT_EQ(Cmp(stmt->where[0]).column.name, "a");
  }
}

// Regression: block comments nest (PostgreSQL, DuckDB). Without nesting the first */ ended the
// comment and the rest of it was parsed as SQL: here a WHERE that DuckDB treats as comment text.
TEST(ParserTest, BlockCommentsNest) {
  auto stmt = Parse("SELECT a FROM t /* old: /* inner */ WHERE a = 1 -- */");
  ASSERT_TRUE(stmt.has_value()) << stmt.error().message;
  EXPECT_TRUE(stmt->where.empty());
  auto limited = Parse("SELECT a /* x /* y */ z */ FROM t /**/ LIMIT /*/**/*/ 3");
  ASSERT_TRUE(limited.has_value()) << limited.error().message;
  EXPECT_EQ(limited->limit, std::optional<std::int64_t>(3));
}

// ISNULL/NOTNULL are rejected where they would act as postfix operators, and stay usable as names.
TEST(ParserTest, IsNullAndNotNullAreNamesOnlyWhereNotOperators) {
  constexpr std::string_view kSql =
      R"(SELECT isnull, SUM(notnull) AS notnull, a AS "isnull" FROM isnull WHERE notnull = 1)";
  auto stmt = Parse(kSql);
  ASSERT_TRUE(stmt.has_value()) << stmt.error().message;
  ASSERT_EQ(stmt->items.size(), 3U);
  const ColumnRef* first = ColumnOf(stmt->items[0]);
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(first->name, "isnull");
  EXPECT_EQ(stmt->items[1].alias, std::optional<std::string>("notnull"));
  EXPECT_EQ(stmt->items[2].alias, std::optional<std::string>("isnull"));
  EXPECT_EQ(TableOf(stmt->from.at(0)).name, "isnull");
  ASSERT_EQ(stmt->where.size(), 1U);
  EXPECT_EQ(Cmp(stmt->where[0]).column.name, "notnull");
}

// Operator runs follow PostgreSQL's lexer: '-' after a comparison still starts a negative literal.
TEST(ParserTest, ComparisonsGluedToNegativeLiterals) {
  constexpr std::string_view kSql =
      "SELECT a FROM t WHERE a<=-1 AND b<>-2 AND c=-3 AND d>=-.5 AND e<-1e3 AND f>-0 AND "
      "g=--c\n4 AND h=/*c*/5";
  auto stmt = Parse(kSql);
  ASSERT_TRUE(stmt.has_value()) << stmt.error().message;
  ASSERT_EQ(stmt->where.size(), 8U);
  const std::array<CompareOp, 8> ops = {CompareOp::kLe, CompareOp::kNe, CompareOp::kEq,
                                        CompareOp::kGe, CompareOp::kLt, CompareOp::kGt,
                                        CompareOp::kEq, CompareOp::kEq};
  const std::array<std::string_view, 8> literals = {"-1",   "-2", "-3", "-.5",
                                                    "-1e3", "-0", "4",  "5"};
  for (std::size_t i = 0; i < ops.size(); ++i) {
    const Comparison& cmp = Cmp(stmt->where[i]);
    EXPECT_EQ(cmp.op, ops[i]) << i;
    EXPECT_EQ(std::string(cmp.literal.negative ? "-" : "") + cmp.literal.text, literals[i]) << i;
    EXPECT_EQ(At(kSql, cmp.literal.span), literals[i]) << i;
  }
}

// ---- rejections -----------------------------------------------------------------------------

struct RejectCase {
  std::string_view name;
  std::string_view sql;  // '^' marks the expected error offset and is removed before parsing
  ParseError::Kind kind;
  std::size_t length;
  std::string_view message;  // expected message prefix

  friend void PrintTo(const RejectCase& c, std::ostream* os) { *os << c.sql; }
};

class RejectTest : public ::testing::TestWithParam<RejectCase> {};

TEST_P(RejectTest, ReportsKindSpanAndMessage) {
  const RejectCase& c = GetParam();
  const std::size_t caret = c.sql.find('^');
  ASSERT_NE(caret, std::string_view::npos) << "test case lacks a '^' marker";
  const std::string sql =
      std::string(c.sql.substr(0, caret)) + std::string(c.sql.substr(caret + 1));
  auto result = Parse(sql);
  ASSERT_FALSE(result.has_value()) << sql;
  const ParseError& error = result.error();
  EXPECT_EQ(error.kind, c.kind) << error.message;
  EXPECT_EQ(error.span.offset, caret) << error.message;
  EXPECT_EQ(error.span.length, c.length) << error.message;
  EXPECT_TRUE(error.message.starts_with(c.message)) << error.message;
  if (c.kind == ParseError::Kind::kUnsupported) {
    EXPECT_TRUE(error.message.ends_with(kDocsHint)) << error.message;
  }
}

std::string CaseName(const ::testing::TestParamInfo<RejectCase>& param_info) {
  return std::string(param_info.param.name);
}

constexpr auto kUnsupported = ParseError::Kind::kUnsupported;
constexpr auto kSyntax = ParseError::Kind::kSyntax;

// Recognized SQL outside the subset: kUnsupported at the first offending token.
INSTANTIATE_TEST_SUITE_P(
    Unsupported, RejectTest,
    ::testing::Values(
        RejectCase{"GroupByWithoutFrom", "SELECT a ^GROUP BY a", kUnsupported, 5,
                   "SELECT without FROM is not supported"},
        RejectCase{"GroupByAll", "SELECT a FROM events GROUP BY ^ALL", kUnsupported, 3,
                   "GROUP BY ALL is not supported"},
        RejectCase{"GroupByTrailingComma", "SELECT a FROM events GROUP BY a^, ORDER BY a",
                   kUnsupported, 1, "a trailing comma in GROUP BY is not supported"},
        RejectCase{"HavingAggregateFilter",
                   "SELECT a FROM events GROUP BY a HAVING COUNT(b) ^FILTER (WHERE b = 1) > 1",
                   kUnsupported, 6, "aggregate FILTER clauses are not supported"},
        RejectCase{"OrderByAll", "SELECT a FROM events ORDER BY ^ALL", kUnsupported, 3,
                   "ORDER BY ALL is not supported"},
        RejectCase{"OrderByTrailingComma", "SELECT a FROM events ORDER BY a DESC^, LIMIT 5",
                   kUnsupported, 1, "a trailing comma in ORDER BY is not supported"},
        RejectCase{"OrderByCollate", "SELECT a FROM events ORDER BY a ^COLLATE nocase",
                   kUnsupported, 7, "COLLATE is not supported"},
        RejectCase{"OrderByAggregateFilter",
                   "SELECT a FROM events ORDER BY COUNT(a) ^FILTER (WHERE a = 1)", kUnsupported, 6,
                   "aggregate FILTER clauses are not supported"},
        RejectCase{"OrderByUsing", "SELECT a FROM events ORDER BY a ^USING <", kUnsupported, 5,
                   "ORDER BY ... USING is not supported"},
        RejectCase{"GroupingSets", "SELECT a FROM events GROUP BY ^GROUPING SETS ((a))",
                   kUnsupported, 8, "GROUPING SETS are not supported"},
        RejectCase{"OrderByInAggregate", "SELECT SUM(a ^ORDER BY b) FROM events", kUnsupported, 5,
                   "ORDER BY is not supported"},
        RejectCase{"Distinct", "SELECT ^DISTINCT region FROM events", kUnsupported, 8,
                   "DISTINCT is not supported"},
        RejectCase{"SumDistinct", "SELECT SUM(^distinct user_id) FROM events", kUnsupported, 8,
                   "SUM(DISTINCT ...) is not supported"},
        RejectCase{"FunctionDistinct", "SELECT ^f(DISTINCT a) FROM events", kUnsupported, 1,
                   "function f() with this argument syntax is not supported"},
        RejectCase{"FunctionStar", "SELECT ^f(*) FROM events", kUnsupported, 1,
                   "function f() with this argument syntax is not supported"},
        RejectCase{"FunctionOrderBy", "SELECT ^string_agg(a ORDER BY a) FROM events", kUnsupported,
                   10, "function string_agg() with this argument syntax"},
        RejectCase{"PositionIn", "SELECT ^position('a' IN u) FROM events", kUnsupported, 8,
                   "function position() with this argument syntax is not supported"},
        RejectCase{"SubstringFrom", "SELECT ^substring(u FROM 1 FOR 2) FROM events", kUnsupported,
                   9, "function substring() with this argument syntax is not supported"},
        RejectCase{"FunctionSyntaxError", "SELECT ^f(a +) FROM events", kUnsupported, 1,
                   "function f() with this argument syntax is not supported"},
        RejectCase{"FunctionFilter", "SELECT count_if(a > 0) ^FILTER (WHERE a < 5) FROM events",
                   kUnsupported, 6, "FILTER clauses are not supported"},
        RejectCase{"PostfixNotNull", "SELECT a FROM events WHERE a ^NOT NULL", kUnsupported, 3,
                   "NOT NULL (IS NOT NULL) is not supported"},
        RejectCase{"PostfixNot", "SELECT a ^NOT b FROM events", kUnsupported, 3,
                   "NOT is not supported"},
        RejectCase{"Rollup", "SELECT a FROM events GROUP BY ^ROLLUP (a)", kUnsupported, 6,
                   "ROLLUP is not supported"},
        RejectCase{"MaxDistinctInOrderBy", "SELECT a FROM events ORDER BY MAX(^DISTINCT a)",
                   kUnsupported, 8, "MAX(DISTINCT ...) is not supported"},
        RejectCase{"SelectAll", "SELECT ^ALL a FROM events", kUnsupported, 3,
                   "SELECT ALL is not supported"},
        RejectCase{"AllAggregate", "SELECT COUNT(^ALL a) FROM events", kUnsupported, 3,
                   "ALL in aggregate calls is not supported"},
        RejectCase{"OffsetAll", "SELECT a FROM events OFFSET ^ALL", kUnsupported, 3,
                   "OFFSET ALL is not supported"},
        RejectCase{"OffsetExpression", "SELECT a FROM events OFFSET ^(1)", kUnsupported, 1,
                   "OFFSET expressions are not supported (OFFSET takes an integer)"},
        RejectCase{"LimitCommaOffset", "SELECT a FROM events LIMIT 5^, 10", kUnsupported, 1,
                   "LIMIT with an offset (LIMIT n, m) is not supported"},
        // Joins outside the subset, and words that DuckDB reads as joins after a FROM item.
        RejectCase{"InnerJoinUsing", "SELECT a FROM events INNER JOIN users ^USING (a)",
                   kUnsupported, 5,
                   "JOIN ... USING is not supported (write the condition with ON)"},
        RejectCase{"LeftJoinUsing", "SELECT a FROM events LEFT JOIN users ^using (a)", kUnsupported,
                   5, "JOIN ... USING is not supported"},
        RejectCase{"NestedJoin", "SELECT a FROM events JOIN users ^JOIN items ON a = b ON a = c",
                   kUnsupported, 4,
                   "nested joins (a JOIN before the ON of an earlier JOIN) are not supported"},
        RejectCase{"NestedCrossJoin", "SELECT a FROM events JOIN users ^CROSS JOIN items ON a = b",
                   kUnsupported, 5, "nested joins"},
        RejectCase{"NestedNaturalJoin",
                   "SELECT a FROM events LEFT JOIN users ^NATURAL JOIN items ON a = b",
                   kUnsupported, 7, "nested joins"},
        RejectCase{"NestedLeftOuterJoin",
                   "SELECT a FROM events JOIN users ^LEFT OUTER JOIN items ON a = b ON a = c",
                   kUnsupported, 4, "nested joins"},
        RejectCase{"NestedRightJoin",
                   "SELECT a FROM events JOIN users ^RIGHT JOIN items ON a = b ON a = c",
                   kUnsupported, 5, "nested joins"},
        RejectCase{"NestedInnerJoin",
                   "SELECT a FROM events JOIN users ^INNER JOIN items ON a = b ON a = c",
                   kUnsupported, 5, "nested joins"},
        RejectCase{"RightJoin", "SELECT a FROM events ^RIGHT JOIN users ON a = b", kUnsupported, 5,
                   "RIGHT JOIN is not supported"},
        RejectCase{"RightOuterJoin", "SELECT a FROM events ^right outer join users ON a = b",
                   kUnsupported, 5, "RIGHT JOIN is not supported"},
        RejectCase{"FullJoin", "SELECT a FROM events ^FULL JOIN users ON a = b", kUnsupported, 4,
                   "FULL JOIN is not supported"},
        RejectCase{"FullOuterJoin", "SELECT a FROM events, items ^FULL OUTER JOIN users ON a = b",
                   kUnsupported, 4, "FULL JOIN is not supported"},
        RejectCase{"NaturalJoin", "SELECT a FROM events ^NATURAL JOIN users", kUnsupported, 7,
                   "NATURAL JOIN is not supported"},
        RejectCase{"NaturalLeftJoin", "SELECT a FROM events ^NATURAL LEFT JOIN users", kUnsupported,
                   7, "NATURAL JOIN is not supported"},
        RejectCase{"NaturalFullOuterJoin", "SELECT a FROM events ^NATURAL FULL OUTER JOIN users",
                   kUnsupported, 7, "NATURAL JOIN is not supported"},
        RejectCase{"NaturalInnerJoin", "SELECT a FROM events ^NATURAL INNER JOIN users",
                   kUnsupported, 7, "NATURAL JOIN is not supported"},
        RejectCase{"NaturalSemiJoin", "SELECT a FROM events ^NATURAL SEMI JOIN users", kUnsupported,
                   7, "NATURAL JOIN is not supported"},
        RejectCase{"NaturalAntiJoin", "SELECT a FROM events ^natural anti join users", kUnsupported,
                   7, "NATURAL JOIN is not supported"},
        RejectCase{"SemiJoin", "SELECT a FROM events ^semi JOIN users ON a = b", kUnsupported, 4,
                   "SEMI JOIN is not supported"},
        RejectCase{"AntiJoin", "SELECT a FROM events ^ANTI JOIN users ON a = b", kUnsupported, 4,
                   "ANTI JOIN is not supported"},
        RejectCase{"AsofJoin", "SELECT a FROM events ^asof JOIN users ON a >= b", kUnsupported, 4,
                   "ASOF JOIN is not supported"},
        RejectCase{"AsofLeftJoin", "SELECT a FROM events ^ASOF LEFT JOIN users ON a >= b",
                   kUnsupported, 4, "ASOF JOIN is not supported"},
        RejectCase{"AsofRightOuterJoin",
                   "SELECT a FROM events ^asof right outer join users ON a >= b", kUnsupported, 4,
                   "ASOF JOIN is not supported"},
        RejectCase{"AsofSemiJoin", "SELECT a FROM events ^ASOF SEMI JOIN users ON a >= b",
                   kUnsupported, 4, "ASOF JOIN is not supported"},
        RejectCase{"AsofAntiJoin", "SELECT a FROM events ^ASOF ANTI JOIN users ON a >= b",
                   kUnsupported, 4, "ASOF JOIN is not supported"},
        RejectCase{"PositionalJoin", "SELECT a FROM events ^POSITIONAL JOIN users", kUnsupported,
                   10, "POSITIONAL JOIN is not supported"},
        RejectCase{"SemiJoinAfterAlias", "SELECT a FROM events AS e ^SEMI JOIN users ON a = b",
                   kUnsupported, 4, "SEMI JOIN is not supported"},
        RejectCase{"SemiJoinAfterPath", "SELECT a FROM 'e.parquet' ^semi JOIN users ON a = b",
                   kUnsupported, 4, "SEMI JOIN is not supported"},
        RejectCase{"SemiJoinAfterOn",
                   "SELECT a FROM events JOIN users ON a = b ^SEMI JOIN items ON a = c",
                   kUnsupported, 4, "SEMI JOIN is not supported"},
        RejectCase{"AsofJoinAfterOn",
                   "SELECT a FROM events JOIN users ON a = b ^ASOF JOIN items ON a >= c",
                   kUnsupported, 4, "ASOF JOIN is not supported"},
        RejectCase{"TimeTravel", "SELECT a FROM events ^AT (VERSION => 1)", kUnsupported, 2,
                   "AT (time travel) is not supported"},
        RejectCase{"Pivot", "SELECT a FROM events ^PIVOT (SUM(a) FOR b IN (1, 2))", kUnsupported, 5,
                   "PIVOT is not supported"},
        RejectCase{"PivotAfterOn",
                   "SELECT a FROM events JOIN users ON a = b ^pivot (SUM(a) FOR b IN (1))",
                   kUnsupported, 5, "PIVOT is not supported"},
        RejectCase{"Unpivot", "SELECT a FROM events ^UNPIVOT (v FOR k IN (a, b))", kUnsupported, 7,
                   "UNPIVOT is not supported"},
        RejectCase{"UnpivotIncludeNulls",
                   "SELECT a FROM events e ^UNPIVOT INCLUDE NULLS (v FOR k IN (a, b))",
                   kUnsupported, 7, "UNPIVOT is not supported"},
        RejectCase{"UnpivotExcludeNulls",
                   "SELECT a FROM events ^UNPIVOT EXCLUDE NULLS (v FOR k IN (a, b))", kUnsupported,
                   7, "UNPIVOT is not supported"},
        RejectCase{"TableSampleAfterAlias", "SELECT a FROM events e ^TABLESAMPLE 10%", kUnsupported,
                   11, "TABLESAMPLE is not supported"},
        RejectCase{"TableSampleMethod", "SELECT a FROM events ^tablesample reservoir(10)",
                   kUnsupported, 11, "TABLESAMPLE is not supported"},
        RejectCase{"TableSampleParenthesized", "SELECT a FROM events ^TABLESAMPLE (10)",
                   kUnsupported, 11, "TABLESAMPLE is not supported"},
        RejectCase{"TableSampleDecimal", "SELECT a FROM events ^TABLESAMPLE 2.5 PERCENT",
                   kUnsupported, 11, "TABLESAMPLE is not supported"},
        // After an ON condition, the words that continue it as an operator.
        RejectCase{"GlobAfterOn", "SELECT a FROM events JOIN users ON a = b ^GLOB 'x'",
                   kUnsupported, 4, "GLOB is not supported"},
        RejectCase{"AtTimeZoneAfterOn",
                   "SELECT a FROM events JOIN users ON a = b ^AT TIME ZONE 'UTC'", kUnsupported, 2,
                   "AT TIME ZONE is not supported"},
        RejectCase{"IsNullAfterOn", "SELECT a FROM events JOIN users ON a = b ^isnull",
                   kUnsupported, 6, "ISNULL is not supported"},
        // Other FROM items and FROM-list forms DuckDB parses.
        RejectCase{"Lateral", "SELECT a FROM ^LATERAL (SELECT 1)", kUnsupported, 7,
                   "LATERAL is not supported"},
        RejectCase{"CrossJoinLateral", "SELECT a FROM events CROSS JOIN ^lateral f(a)",
                   kUnsupported, 7, "LATERAL is not supported"},
        // LATERAL before a function with a qualified name: three tokens show the first dot only,
        // so LATERAL s.t, a syntax error in DuckDB, is unsupported too.
        RejectCase{"LateralQualifiedFunction", "SELECT a FROM events, ^LATERAL main.range(3)",
                   kUnsupported, 7, "LATERAL is not supported"},
        RejectCase{"JoinLateralQuotedQualifier",
                   R"(SELECT a FROM events JOIN ^LATERAL "main".range(3) r ON a = r.range)",
                   kUnsupported, 7, "LATERAL is not supported"},
        RejectCase{"LateralThreePartFunction", "SELECT a FROM ^LATERAL system.main.range(3)",
                   kUnsupported, 7, "LATERAL is not supported"},
        RejectCase{"LateralQualifiedNameWithoutCall", "SELECT a FROM events, ^LATERAL main.users",
                   kUnsupported, 7, "LATERAL is not supported"},
        // The reserved words that DuckDB takes as function names call table functions where a FROM
        // item starts (also after LATERAL and as the first word in parentheses).
        RejectCase{"TableFunctionNamedLeft", "SELECT * FROM events, ^left(7)", kUnsupported, 4,
                   "table functions are not supported"},
        RejectCase{"FirstTableFunctionNamedSimilar", "SELECT * FROM ^similar(1)", kUnsupported, 7,
                   "table functions are not supported"},
        RejectCase{"JoinTableFunctionNamedInner",
                   "SELECT * FROM events JOIN ^inner(1) i ON a = i.v", kUnsupported, 5,
                   "table functions are not supported"},
        RejectCase{"LeftJoinTableFunctionNamedLeft",
                   "SELECT * FROM events LEFT JOIN ^left(1) l ON a = l.v", kUnsupported, 4,
                   "table functions are not supported"},
        RejectCase{"CrossJoinTableFunctionNamedRight", "SELECT * FROM events CROSS JOIN ^RIGHT(1)",
                   kUnsupported, 5, "table functions are not supported"},
        RejectCase{"LateralFunctionNamedLeft", "SELECT a FROM events, ^LATERAL left(1)",
                   kUnsupported, 7, "LATERAL is not supported"},
        RejectCase{"ParenthesizedJoinOfAFunctionNamedLeft",
                   "SELECT a FROM ^(left(1) l JOIN events ON a = l.v)", kUnsupported, 1,
                   "parenthesized joins in FROM are not supported"},
        // DuckDB takes BETWEEN, EXISTS, INTERVAL and OVER for qualifiers where a FROM item starts:
        // of a table, of a table function and after LATERAL, also first in parentheses. Three
        // tokens show only the dot, so LATERAL over.x and (over.x), syntax errors in DuckDB, are
        // unsupported too, and so is LATERAL before one of these words in parentheses.
        RejectCase{"TableQualifiedByOverAfterComma", "SELECT a FROM events, ^over.x", kUnsupported,
                   4,
                   "the reserved word OVER as a qualifier is not supported; write it as a quoted "
                   "identifier"},
        RejectCase{"FirstTableQualifiedByExists", "SELECT a FROM ^EXISTS.x", kUnsupported, 6,
                   "the reserved word EXISTS as a qualifier is not supported"},
        RejectCase{"JoinedTableQualifiedByInterval",
                   "SELECT a FROM events JOIN ^interval.x ON a = b", kUnsupported, 8,
                   "the reserved word INTERVAL as a qualifier is not supported"},
        RejectCase{"FirstTableFunctionQualifiedByOver", "SELECT a FROM ^over.f(1)", kUnsupported, 4,
                   "the reserved word OVER as a qualifier is not supported"},
        RejectCase{"TableFunctionQualifiedByBetweenAfterComma",
                   "SELECT a FROM events, ^between.f(1)", kUnsupported, 7,
                   "the reserved word BETWEEN as a qualifier is not supported"},
        RejectCase{"LateralFunctionQualifiedByBetween",
                   "SELECT a FROM events, ^LATERAL between.f(1)", kUnsupported, 7,
                   "LATERAL is not supported"},
        RejectCase{"LateralQualifiedByOverWithoutCall", "SELECT a FROM events, ^LATERAL over.users",
                   kUnsupported, 7, "LATERAL is not supported"},
        RejectCase{"ParenthesizedJoinOfATableQualifiedByExists",
                   "SELECT a FROM ^(exists.x CROSS JOIN events)", kUnsupported, 1,
                   "parenthesized joins in FROM are not supported"},
        RejectCase{"TableQualifiedByOverAloneInParentheses", "SELECT a FROM ^(over.x)",
                   kUnsupported, 1, "parenthesized joins in FROM are not supported"},
        RejectCase{"ParenthesizedJoinOfALateralFunctionQualifiedByExists",
                   "SELECT a FROM ^(LATERAL exists.f(1) CROSS JOIN events)", kUnsupported, 1,
                   "parenthesized joins in FROM are not supported"},
        RejectCase{"ParenthesizedLateralBeforeInterval",
                   "SELECT a FROM ^(LATERAL interval CROSS JOIN events)", kUnsupported, 1,
                   "parenthesized joins in FROM are not supported"},
        RejectCase{"Only", "SELECT a FROM ^ONLY events", kUnsupported, 4, "ONLY is not supported"},
        RejectCase{"OnlyPath", "SELECT a FROM ^only 'e.parquet'", kUnsupported, 4,
                   "ONLY is not supported"},
        RejectCase{"CommaOnly", "SELECT a FROM events, ^ONLY users", kUnsupported, 4,
                   "ONLY is not supported"},
        RejectCase{"ColumnAliasList", "SELECT a FROM events AS e^(x, y)", kUnsupported, 1,
                   "column alias lists (t AS a(x, y)) are not supported"},
        RejectCase{"ColumnAliasListImplicit", "SELECT a FROM events e^(x)", kUnsupported, 1,
                   "column alias lists"},
        RejectCase{"ColumnAliasListPath", "SELECT a FROM 'e.parquet' AS 'p'^(x)", kUnsupported, 1,
                   "column alias lists"},
        RejectCase{"ParenthesizedJoin", "SELECT a FROM ^(events JOIN users ON a = b)", kUnsupported,
                   1, "parenthesized joins in FROM are not supported"},
        RejectCase{"ParenthesizedJoinWithAlias", "SELECT a FROM ^(events e CROSS JOIN users)",
                   kUnsupported, 1, "parenthesized joins in FROM are not supported"},
        // In parentheses a LATERAL item only starts a join in DuckDB; three tokens show LATERAL and
        // the '(' or the name after it.
        RejectCase{"ParenthesizedJoinOfALateralSubquery",
                   "SELECT a FROM ^(LATERAL (SELECT 1) x CROSS JOIN events)", kUnsupported, 1,
                   "parenthesized joins in FROM are not supported"},
        RejectCase{"ParenthesizedJoinOfALateralFunction",
                   "SELECT a FROM ^(LATERAL left(1) CROSS JOIN events)", kUnsupported, 1,
                   "parenthesized joins in FROM are not supported"},
        // A query in parentheses in FROM is a derived table: one other than SELECT or WITH is
        // unsupported at its first token.
        RejectCase{"SubqueryValues", "SELECT a FROM (^VALUES (1))", kUnsupported, 6,
                   "VALUES is not supported; only SELECT queries are supported"},
        RejectCase{"SubqueryValuesAfterComma", "SELECT a FROM t, (^values(1)) v", kUnsupported, 6,
                   "VALUES is not supported; only SELECT queries are supported"},
        RejectCase{"SubqueryFromFirst", "SELECT a FROM (^FROM events)", kUnsupported, 4,
                   "FROM-first queries are not supported"},
        RejectCase{"SubqueryParenthesized", "SELECT a FROM (^(SELECT 1))", kUnsupported, 1,
                   "parenthesized queries are not supported"},
        RejectCase{"SubqueryParenthesizedJoin", "SELECT a FROM (^(t JOIN u ON a = b))",
                   kUnsupported, 1, "parenthesized queries are not supported"},
        RejectCase{"SubqueryWith", "SELECT a FROM (WITH x AS (SELECT 1^) SELECT * FROM x)",
                   kUnsupported, 1, "SELECT without FROM is not supported"},
        RejectCase{"SubqueryTable", "SELECT a FROM (^TABLE events)", kUnsupported, 5,
                   "TABLE is not supported; only SELECT queries are supported"},
        RejectCase{"SubqueryShow", "SELECT a FROM (^SHOW events)", kUnsupported, 4,
                   "SHOW is not supported; only SELECT queries are supported"},
        RejectCase{"SubqueryDescribe", "SELECT a FROM (^DESCRIBE events)", kUnsupported, 8,
                   "DESCRIBE is not supported; only SELECT queries are supported"},
        RejectCase{"SubquerySummarize", "SELECT a FROM (^SUMMARIZE events)", kUnsupported, 9,
                   "SUMMARIZE is not supported; only SELECT queries are supported"},
        RejectCase{"SubqueryPivot", "SELECT a FROM (^PIVOT events ON a)", kUnsupported, 5,
                   "PIVOT is not supported; only SELECT queries are supported"},
        RejectCase{"SubqueryUnpivot", "SELECT a FROM (^UNPIVOT events ON a INTO NAME k VALUE v)",
                   kUnsupported, 7, "UNPIVOT is not supported; only SELECT queries are supported"},
        RejectCase{"SubquerySelectWithoutFrom", "SELECT a FROM (SELECT 1^)", kUnsupported, 1,
                   "SELECT without FROM is not supported"},
        RejectCase{"SubquerySelectWithoutFromSemicolon", "SELECT a FROM (SELECT 1^;)", kUnsupported,
                   1, "SELECT without FROM is not supported"},
        RejectCase{"SubqueryTrailingCommaInSelect", "SELECT a FROM (SELECT a^, FROM t)",
                   kUnsupported, 1, "a trailing comma in the select list is not supported"},
        RejectCase{"SubqueryTrailingCommaInFrom", "SELECT a FROM (SELECT a FROM t^,)", kUnsupported,
                   1, "a trailing comma in FROM is not supported"},
        RejectCase{"SubqueryTrailingCommaInGroupBy", "SELECT a FROM (SELECT a FROM t GROUP BY a^,)",
                   kUnsupported, 1, "a trailing comma in GROUP BY is not supported"},
        RejectCase{"SubqueryTrailingCommaInOrderBy", "SELECT a FROM (SELECT a FROM t ORDER BY a^,)",
                   kUnsupported, 1, "a trailing comma in ORDER BY is not supported"},
        RejectCase{"SubqueryUnion", "SELECT a FROM (SELECT a FROM t ^UNION SELECT a FROM u)",
                   kUnsupported, 5, "UNION is not supported"},
        RejectCase{"SubqueryFetch", "SELECT a FROM (SELECT a FROM t ^FETCH FIRST 1 ROWS ONLY) s",
                   kUnsupported, 5, "FETCH is not supported"},
        RejectCase{"UnionAfterSubquery", "SELECT a FROM (SELECT a FROM t) s ^UNION SELECT a FROM u",
                   kUnsupported, 5, "UNION is not supported"},
        RejectCase{"SubqueryInSubquery", "SELECT a FROM (SELECT a FROM (^VALUES (1)) v) w",
                   kUnsupported, 6, "VALUES is not supported"},
        // After a derived table, what DuckDB gives a meaning there (not AT: time travel reads a
        // table).
        RejectCase{"SubqueryPivotAfter",
                   "SELECT a FROM (SELECT a FROM t) ^PIVOT (SUM(a) FOR a IN (1))", kUnsupported, 5,
                   "PIVOT is not supported"},
        RejectCase{"SubqueryUnpivotAfterColumns",
                   "SELECT a FROM (SELECT a FROM t) s(x) ^UNPIVOT (v FOR k IN (x))", kUnsupported,
                   7, "UNPIVOT is not supported"},
        RejectCase{"SubqueryTablesample", "SELECT a FROM (SELECT a FROM t) ^TABLESAMPLE 10%",
                   kUnsupported, 11, "TABLESAMPLE is not supported"},
        RejectCase{"SubquerySemiJoin", "SELECT a FROM (SELECT a FROM t) ^semi JOIN u ON a = b",
                   kUnsupported, 4, "SEMI JOIN is not supported"},
        RejectCase{"SubqueryColumnsAntiJoin",
                   "SELECT a FROM (SELECT a FROM t) AS s(x) ^anti JOIN u ON x = b", kUnsupported, 4,
                   "ANTI JOIN is not supported"},
        RejectCase{"SubqueryAsofJoin", "SELECT a FROM (SELECT a FROM t) s ^ASOF JOIN u ON a >= b",
                   kUnsupported, 4, "ASOF JOIN is not supported"},
        RejectCase{"SubqueryEmptyStringAlias", "SELECT a FROM (SELECT a FROM t) AS ^''",
                   kUnsupported, 2, "an empty table alias ('') is not supported"},
        RejectCase{"SubqueryEscapeStringAlias", "SELECT a FROM (SELECT a FROM t) AS ^E's'",
                   kUnsupported, 1, "prefixed strings (E'...') are not supported"},
        RejectCase{"LateralSubquery", "SELECT a FROM t, ^LATERAL (SELECT a FROM u) s", kUnsupported,
                   7, "LATERAL is not supported"},
        // Column alias lists: DuckDB takes a trailing comma and strings in them.
        RejectCase{"ColumnAliasListTrailingComma", "SELECT a FROM (SELECT a FROM t) s(x^,)",
                   kUnsupported, 1, "a trailing comma in a column alias list is not supported"},
        RejectCase{"ColumnAliasListTrailingCommaAfterQuoted",
                   "SELECT a FROM (SELECT a, b FROM t) AS s(\"x\", y^,)", kUnsupported, 1,
                   "a trailing comma in a column alias list"},
        RejectCase{"ColumnAliasListString", "SELECT a FROM (SELECT a FROM t) s(^'x')", kUnsupported,
                   3,
                   "string literals as column aliases are not supported; write the alias as a "
                   "quoted identifier"},
        RejectCase{"ColumnAliasListEmptyString", "SELECT a FROM (SELECT a FROM t) s(x, ^'')",
                   kUnsupported, 2, "string literals as column aliases are not supported"},
        RejectCase{"ColumnAliasListEscapeString", "SELECT a FROM (SELECT a FROM t) s(^E'x')",
                   kUnsupported, 1, "string literals as column aliases are not supported"},
        RejectCase{"ColumnAliasListDollarQuoted", "SELECT a FROM (SELECT a FROM t) s(^$$x$$)",
                   kUnsupported, 1, "string literals as column aliases are not supported"},
        RejectCase{"ColumnAliasListOfAStringAlias", "SELECT a FROM (SELECT a FROM t) AS 's'(^'x')",
                   kUnsupported, 3, "string literals as column aliases are not supported"},
        // WITH lists: what DuckDB answers and antb1 does not, at its first token.
        RejectCase{"WithRecursive", "WITH ^RECURSIVE c AS (SELECT a FROM t) SELECT a FROM c",
                   kUnsupported, 9, "WITH RECURSIVE is not supported"},
        RejectCase{"Materialized", "WITH c AS ^MATERIALIZED (SELECT a FROM t) SELECT a FROM c",
                   kUnsupported, 12, "MATERIALIZED is not supported"},
        RejectCase{"MaterializedWithoutParenthesis", "WITH c AS ^MATERIALIZED SELECT 1",
                   kUnsupported, 12, "MATERIALIZED is not supported"},
        RejectCase{"NotMaterialized",
                   "WITH c AS ^not materialized (SELECT a FROM t) SELECT a FROM c", kUnsupported,
                   16, "NOT MATERIALIZED is not supported"},
        RejectCase{"UsingKey", "WITH c ^USING KEY (a) AS (SELECT a FROM t) SELECT a FROM c",
                   kUnsupported, 9, "USING KEY is not supported"},
        RejectCase{"UsingKeyAfterColumns",
                   "WITH c(x) ^using key (x) AS (SELECT a AS x FROM t) SELECT x FROM c",
                   kUnsupported, 9, "USING KEY is not supported"},
        RejectCase{"CteStringName", "WITH ^'c' AS (SELECT a FROM t) SELECT a FROM c", kUnsupported,
                   3,
                   "string literals as CTE names are not supported; write the name as a quoted "
                   "identifier"},
        RejectCase{"CteEmptyStringName", "WITH ^'' AS (SELECT a FROM t) SELECT a FROM t",
                   kUnsupported, 2, "string literals as CTE names are not supported"},
        RejectCase{"CteEscapeStringName", "WITH ^E'c' AS (SELECT a FROM t) SELECT a FROM t",
                   kUnsupported, 1, "string literals as CTE names are not supported"},
        RejectCase{"CteDollarQuotedName", "WITH ^$$c$$ AS (SELECT a FROM t) SELECT a FROM t",
                   kUnsupported, 1, "string literals as CTE names are not supported"},
        RejectCase{"CteStringNameLater",
                   "WITH c AS (SELECT a FROM t), ^'d' AS (SELECT a FROM t) SELECT a FROM c",
                   kUnsupported, 3, "string literals as CTE names are not supported"},
        RejectCase{"CteColumnAliasTrailingComma",
                   "WITH c(x^,) AS (SELECT a FROM t) SELECT x FROM c", kUnsupported, 1,
                   "a trailing comma in a column alias list is not supported"},
        RejectCase{"CteColumnAliasString", "WITH c(^'x') AS (SELECT a FROM t) SELECT a FROM c",
                   kUnsupported, 3, "string literals as column aliases are not supported"},
        RejectCase{"CteWithoutFrom", "WITH c AS (SELECT 1^) SELECT a FROM t", kUnsupported, 1,
                   "SELECT without FROM is not supported"},
        RejectCase{"CteFromFirst", "WITH c AS (^FROM t) SELECT a FROM c", kUnsupported, 4,
                   "FROM-first queries are not supported"},
        RejectCase{"CteValues", "WITH c AS (^VALUES (1)) SELECT a FROM c", kUnsupported, 6,
                   "VALUES is not supported"},
        RejectCase{"CteParenthesizedQuery", "WITH c AS (^(SELECT a FROM t)) SELECT a FROM c",
                   kUnsupported, 1, "parenthesized queries are not supported"},
        RejectCase{"CteUnion", "WITH c AS (SELECT a FROM t ^UNION SELECT a FROM u) SELECT a FROM c",
                   kUnsupported, 5, "UNION is not supported"},
        RejectCase{"FromFirstAfterWith", "WITH c AS (SELECT a FROM t) ^FROM c", kUnsupported, 4,
                   "FROM-first queries are not supported"},
        RejectCase{"ValuesAfterWith", "WITH c AS (SELECT a FROM t) ^VALUES (1)", kUnsupported, 6,
                   "VALUES is not supported"},
        RejectCase{"TableAfterWith", "WITH c AS (SELECT a FROM t) ^TABLE c", kUnsupported, 5,
                   "TABLE is not supported"},
        RejectCase{"ParenthesizedQueryAfterWith", "WITH c AS (SELECT a FROM t) ^(SELECT a FROM c)",
                   kUnsupported, 1, "parenthesized queries are not supported"},
        RejectCase{"UnionAfterWith", "WITH c AS (SELECT a FROM t) SELECT a FROM c ^UNION SELECT 1",
                   kUnsupported, 5, "UNION is not supported"},
        RejectCase{"EmptyStringAlias", "SELECT a FROM events AS ^''", kUnsupported, 2,
                   "an empty table alias ('') is not supported"},
        RejectCase{"EscapeStringAlias", "SELECT a FROM events AS ^E'x'", kUnsupported, 1,
                   "prefixed strings (E'...') are not supported"},
        RejectCase{"EscapeStringAliasOfAJoin", "SELECT a FROM events JOIN users AS ^e'u' ON a = b",
                   kUnsupported, 1, "prefixed strings (E'...') are not supported"},
        RejectCase{"DollarQuotedAlias", "SELECT a FROM events AS ^$$x$$", kUnsupported, 1,
                   "dollar-quoted strings are not supported"},
        RejectCase{"DollarTagQuotedAlias", "SELECT a FROM events AS ^$tag$x$tag$", kUnsupported, 4,
                   "dollar-quoted strings are not supported"},
        RejectCase{"EmptyDollarQuotedAlias", "SELECT a FROM events AS ^$$$$", kUnsupported, 1,
                   "dollar-quoted strings are not supported"},
        RejectCase{"NaturalLeftOuterWithoutJoin", "SELECT a FROM events ^NATURAL LEFT OUTER users",
                   kUnsupported, 7, "NATURAL JOIN is not supported"},
        RejectCase{"TrailingCommaInFrom", "SELECT a FROM events^,", kUnsupported, 1,
                   "a trailing comma in FROM is not supported"},
        RejectCase{"TrailingCommaBeforeWhere", "SELECT a FROM events^, WHERE a = 1", kUnsupported,
                   1, "a trailing comma in FROM is not supported"},
        RejectCase{"TrailingCommaAfterJoin", "SELECT a FROM events JOIN users ON a = b^, LIMIT 1",
                   kUnsupported, 1, "a trailing comma in FROM is not supported"},
        // Qualified names: one qualifier, and a column name that needs no quoting.
        RejectCase{"QualifiedStar", "SELECT e.^* FROM events e", kUnsupported, 1,
                   "qualified * (t.*) is not supported"},
        RejectCase{"CountQualifiedStar", "SELECT COUNT(e.^*) FROM events e", kUnsupported, 1,
                   "qualified * (t.*) is not supported"},
        RejectCase{"ThreePartName", "SELECT main.e^.a FROM events e", kUnsupported, 1,
                   "names of more than two parts (a.b.c) are not supported"},
        RejectCase{"MethodCall", "SELECT e.^lower(a) FROM events e", kUnsupported, 5,
                   "qualified function names and method calls (a.f()) are not supported"},
        RejectCase{"QuotedMethodCall", R"(SELECT "e".^"f"() FROM events e)", kUnsupported, 3,
                   "qualified function names and method calls"},
        RejectCase{"ReservedWordAfterDot", "SELECT e.^from FROM events e", kUnsupported, 4,
                   "the reserved word FROM after '.' is not supported; write it as a quoted "
                   "identifier"},
        RejectCase{"AliasKeywordQualifier", "SELECT ^over.a FROM events over", kUnsupported, 4,
                   "the reserved word OVER as a qualifier is not supported; write it as a quoted "
                   "identifier"},
        RejectCase{"ExistsQualifier", "SELECT a FROM events WHERE ^exists.a = 1", kUnsupported, 6,
                   "the reserved word EXISTS as a qualifier is not supported"},
        RejectCase{"FieldAccess", "SELECT f(a)^.b FROM events", kUnsupported, 1,
                   "'.' after an expression (a field or a method) is not supported"},
        RejectCase{"FieldOfAString", "SELECT 'p'^.x FROM events", kUnsupported, 1,
                   "'.' after an expression"},
        // GLOB and AT TIME ZONE: operators in every expression, never an implicit alias.
        RejectCase{"GlobInWhere", "SELECT a FROM events WHERE a ^GLOB 'x*'", kUnsupported, 4,
                   "GLOB is not supported"},
        RejectCase{"GlobInSelect", "SELECT a ^glob 'x*' FROM events", kUnsupported, 4,
                   "GLOB is not supported"},
        RejectCase{"GlobAsSelectAlias", "SELECT a ^glob FROM events", kUnsupported, 4,
                   "GLOB is not supported"},
        RejectCase{"AtTimeZoneInSelect", "SELECT ts ^AT TIME ZONE 'UTC' FROM events", kUnsupported,
                   2, "AT TIME ZONE is not supported"},
        RejectCase{"AtTimeZoneInWhere", "SELECT a FROM events WHERE ts ^at time zone 'UTC' = 1",
                   kUnsupported, 2, "AT TIME ZONE is not supported"},
        RejectCase{"Union", "SELECT a FROM events ^UNION SELECT a FROM users", kUnsupported, 5,
                   "UNION is not supported"},
        RejectCase{"UnionAfterWhere", "SELECT a FROM events WHERE a = 1 ^UNION ALL SELECT 1",
                   kUnsupported, 5, "UNION is not supported"},
        RejectCase{"Intersect", "SELECT a FROM events ^INTERSECT SELECT a FROM users", kUnsupported,
                   9, "INTERSECT is not supported"},
        RejectCase{"Except", "SELECT a FROM events ^EXCEPT SELECT a FROM users", kUnsupported, 6,
                   "EXCEPT is not supported"},
        RejectCase{"LikeEscape", "SELECT a FROM events WHERE url LIKE 'x!%' ^ESCAPE '!'",
                   kUnsupported, 6, "LIKE ... ESCAPE is not supported"},
        RejectCase{"ILike", "SELECT a FROM events WHERE url ^ILIKE '%x%'", kUnsupported, 5,
                   "ILIKE is not supported"},
        RejectCase{"SimilarTo", "SELECT a FROM events WHERE url ^SIMILAR TO 'x'", kUnsupported, 7,
                   "SIMILAR TO is not supported"},
        RejectCase{"InSubquery", "SELECT a FROM events WHERE region IN (^SELECT b FROM t)",
                   kUnsupported, 6, "IN (subquery) is not supported"},
        RejectCase{"ComparisonAfterBetween", "SELECT a FROM events WHERE a BETWEEN 1 AND 2 ^= b",
                   kUnsupported, 1, "chained comparisons (a = b = c) are not supported"},
        RejectCase{"BetweenAfterComparison", "SELECT a FROM events WHERE a = b ^BETWEEN 1 AND 2",
                   kUnsupported, 7, "chained comparisons (a = b = c) are not supported"},
        RejectCase{"IsNull", "SELECT a FROM events WHERE a ^IS NULL", kUnsupported, 2,
                   "IS NULL is not supported"},
        RejectCase{"IsNotNull", "SELECT a FROM events WHERE a ^is not null", kUnsupported, 2,
                   "IS NOT NULL is not supported"},
        RejectCase{"IsTrue", "SELECT a FROM events WHERE a ^IS TRUE", kUnsupported, 2,
                   "IS is not supported"},
        RejectCase{"IsNullInSelect", "SELECT a ^IS NULL FROM events", kUnsupported, 2,
                   "IS NULL is not supported"},
        RejectCase{"NullLiteral", "SELECT a FROM events WHERE a = ^NULL", kUnsupported, 4,
                   "NULL literals are not supported"},
        RejectCase{"NullLiteralFirst", "SELECT a FROM events WHERE ^NULL = a", kUnsupported, 4,
                   "NULL literals are not supported"},
        RejectCase{"NullInSelect", "SELECT ^NULL FROM events", kUnsupported, 4,
                   "NULL literals are not supported"},
        RejectCase{"LimitNull", "SELECT a FROM events LIMIT ^NULL", kUnsupported, 4,
                   "NULL literals are not supported"},
        RejectCase{"UnaryPlus", "SELECT a FROM events WHERE a = ^+1", kUnsupported, 1,
                   "unary '+' is not supported"},
        RejectCase{"LimitArithmetic", "SELECT a FROM events LIMIT 1 ^+ 1", kUnsupported, 1,
                   "LIMIT expressions are not supported (LIMIT takes an integer)"},
        RejectCase{"LimitParenthesized", "SELECT a FROM events LIMIT ^(5)", kUnsupported, 1,
                   "LIMIT expressions are not supported"},
        RejectCase{"LimitFunction", "SELECT a FROM events LIMIT ^abs(5)", kUnsupported, 3,
                   "LIMIT expressions are not supported"},
        RejectCase{"LimitAll", "SELECT a FROM events LIMIT ^ALL", kUnsupported, 3,
                   "LIMIT ALL is not supported"},
        RejectCase{"Concat", "SELECT a ^|| b FROM events", kUnsupported, 2,
                   "string concatenation (||) is not supported"},
        RejectCase{"TableFunction", "SELECT * FROM ^read_parquet('x.parquet')", kUnsupported, 12,
                   "table functions are not supported"},
        RejectCase{"SubqueryInWhere", "SELECT a FROM events WHERE a = ^(SELECT 1)", kUnsupported, 1,
                   "subqueries are not supported"},
        RejectCase{"ParenthesizedQuery", "^(SELECT a FROM events)", kUnsupported, 1,
                   "parenthesized queries are not supported"},
        RejectCase{"Exists", "SELECT a FROM events WHERE ^EXISTS (SELECT 1)", kUnsupported, 6,
                   "EXISTS (subqueries) is not supported"},
        RejectCase{"Any", "SELECT a FROM events WHERE a = ^ANY (SELECT 1)", kUnsupported, 3,
                   "ANY (quantified comparisons) is not supported"},
        RejectCase{"MultipleStatements", "SELECT a FROM events; ^SELECT b FROM events",
                   kUnsupported, 6, "multiple statements are not supported"},
        RejectCase{"EmptySecondStatement", "SELECT a FROM events;^;", kUnsupported, 1,
                   "multiple statements are not supported"},
        RejectCase{"True", "SELECT a FROM events WHERE flag = ^TRUE", kUnsupported, 4,
                   "boolean literals (TRUE/FALSE) are not supported"},
        RejectCase{"False", "SELECT a FROM events WHERE ^false = flag", kUnsupported, 5,
                   "boolean literals (TRUE/FALSE) are not supported"},
        RejectCase{"Interval", "SELECT a FROM events WHERE d > ^INTERVAL '1 day'", kUnsupported, 8,
                   "INTERVAL is not supported"},
        RejectCase{"CastQuotedType", R"(SELECT CAST(a AS ^"DATE") FROM events)", kUnsupported, 6,
                   "quoted type names are not supported"},
        RejectCase{"CastOperatorQuotedType", R"(SELECT a::^"int" FROM events)", kUnsupported, 5,
                   "quoted type names are not supported"},
        RejectCase{"CastInterval", "SELECT CAST(a AS ^INTERVAL) FROM events", kUnsupported, 8,
                   "CAST to INTERVAL is not supported"},
        RejectCase{"CastUnion", "SELECT a::^union FROM events", kUnsupported, 5,
                   "CAST to UNION is not supported"},
        RejectCase{"CastDoublePrecision", "SELECT a FROM events WHERE b::^DOUBLE PRECISION > 1",
                   kUnsupported, 16, "type names of more than one word are not supported"},
        RejectCase{"CastTimestampWithTimeZone",
                   "SELECT CAST(a AS ^TIMESTAMP WITH TIME ZONE) FROM events", kUnsupported, 14,
                   "type names of more than one word are not supported"},
        RejectCase{"CastTimestampParamsWithTimeZone",
                   "SELECT CAST(a AS ^TIMESTAMP(3) WITH TIME ZONE) FROM events", kUnsupported, 17,
                   "type names of more than one word are not supported"},
        RejectCase{"CastCharacterVarying", "SELECT a::^character varying(3) FROM events",
                   kUnsupported, 17, "type names of more than one word are not supported"},
        RejectCase{"CastArray", "SELECT CAST(a AS INT^[]) FROM events", kUnsupported, 1,
                   "array types are not supported"},
        RejectCase{"CastArrayKeyword", "SELECT CAST(a AS INT ^ARRAY) FROM events", kUnsupported, 5,
                   "array types are not supported"},
        RejectCase{"CastNameParameter", "SELECT CAST(a AS DECIMAL(^p)) FROM events", kUnsupported,
                   1, "type parameters other than integers are not supported"},
        RejectCase{"CastStructType", "SELECT CAST(a AS STRUCT(^x INT)) FROM events", kUnsupported,
                   1, "type parameters other than integers are not supported"},
        RejectCase{"CastEnumType", "SELECT CAST(a AS ENUM(^'x')) FROM events", kUnsupported, 3,
                   "type parameters other than integers are not supported"},
        RejectCase{"CastQualifiedType", "SELECT CAST(a AS main^.int) FROM events", kUnsupported, 1,
                   "qualified type names are not supported"},
        RejectCase{"CastOfAnInCondition", "SELECT a FROM events WHERE a IN (1)^::BOOLEAN",
                   kUnsupported, 2, "CAST (::) of an IN condition is not supported"},
        RejectCase{"LimitCastOperator", "SELECT a FROM events LIMIT 5^::INT", kUnsupported, 2,
                   "LIMIT expressions are not supported"},
        RejectCase{"LimitCast", "SELECT a FROM events LIMIT ^CAST(5 AS INT)", kUnsupported, 4,
                   "LIMIT expressions are not supported"},
        RejectCase{"TimestampTzLiteral",
                   "SELECT a FROM events WHERE ts > ^TIMESTAMPTZ '2024-01-01'", kUnsupported, 11,
                   "TIMESTAMPTZ literals are not supported"},
        RejectCase{"TimeLiteral", "SELECT a FROM events WHERE t > ^time '10:00'", kUnsupported, 4,
                   "TIME literals are not supported"},
        RejectCase{"SelectWithoutFrom", "SELECT a^", kUnsupported, 0,
                   "SELECT without FROM is not supported"},
        RejectCase{"SelectWithoutFromSemicolon", "SELECT COUNT(*)^;", kUnsupported, 1,
                   "SELECT without FROM is not supported"},
        RejectCase{"SelectWithoutFromWhere", "SELECT a ^WHERE a = 1", kUnsupported, 5,
                   "SELECT without FROM is not supported"},
        RejectCase{"SelectWithoutFromLimit", "SELECT * ^LIMIT 1", kUnsupported, 5,
                   "SELECT without FROM is not supported"},
        RejectCase{"SelectInto", "SELECT a ^INTO copy FROM events", kUnsupported, 4,
                   "SELECT INTO is not supported"},
        RejectCase{"QualifiedTable", "SELECT a FROM main^.events", kUnsupported, 1,
                   "qualified table names are not supported"},
        RejectCase{"QualifiedJoinedTable", "SELECT a FROM events JOIN main^.users ON a = b",
                   kUnsupported, 1, "qualified table names are not supported"},
        RejectCase{"UnquotedPath", "SELECT a FROM data^.parquet", kUnsupported, 1,
                   "qualified table names are not supported"},
        RejectCase{"Window", "SELECT COUNT(*) ^OVER (PARTITION BY a) FROM events", kUnsupported, 4,
                   "window functions (OVER) are not supported"},
        RejectCase{"Filter", "SELECT COUNT(*) ^FILTER (WHERE a = 1) FROM events", kUnsupported, 6,
                   "aggregate FILTER clauses are not supported"},
        RejectCase{"StarWithItems", "SELECT *^, a FROM events", kUnsupported, 1,
                   "combining '*' with other select items is not supported"},
        RejectCase{"ItemsWithStar", "SELECT a, ^* FROM events", kUnsupported, 1,
                   "combining '*' with other select items is not supported"},
        RejectCase{"Collate", "SELECT a FROM events WHERE a = 'x' ^COLLATE nocase", kUnsupported, 7,
                   "COLLATE is not supported"},
        RejectCase{"Qualify", "SELECT a FROM events ^QUALIFY a = 1", kUnsupported, 7,
                   "QUALIFY is not supported"},
        RejectCase{"WindowClause", "SELECT a FROM events ^WINDOW w AS ()", kUnsupported, 6,
                   "WINDOW is not supported"},
        RejectCase{"Fetch", "SELECT a FROM events ^FETCH FIRST 1 ROWS ONLY", kUnsupported, 5,
                   "FETCH is not supported"},
        RejectCase{"ForUpdate", "SELECT a FROM events ^FOR UPDATE", kUnsupported, 3,
                   "FOR UPDATE/SHARE is not supported"},
        RejectCase{"Insert", "^INSERT INTO events VALUES (1)", kUnsupported, 6,
                   "INSERT is not supported; only SELECT queries are supported"},
        RejectCase{"Explain", "^explain SELECT a FROM events", kUnsupported, 7,
                   "EXPLAIN is not supported"},
        RejectCase{"Values", "^VALUES (1)", kUnsupported, 6, "VALUES is not supported"},
        RejectCase{"FromFirst", "^FROM events SELECT a", kUnsupported, 4,
                   "FROM-first queries are not supported"},
        // ISNULL/NOTNULL are postfix operators: never an implicit alias (a silent misparse before).
        RejectCase{"IsNullPostfix", "SELECT a ^ISNULL FROM events", kUnsupported, 6,
                   "ISNULL is not supported"},
        RejectCase{"NotNullPostfixAfterAggregate", "SELECT COUNT(*) ^notnull FROM events",
                   kUnsupported, 7, "NOTNULL is not supported"},
        RejectCase{"IsNullPostfixInWhere", "SELECT a FROM events WHERE a ^isnull", kUnsupported, 6,
                   "ISNULL is not supported"},
        RejectCase{"NotNullAfterComparison", "SELECT a FROM events WHERE a = 1 ^NOTNULL",
                   kUnsupported, 7, "NOTNULL is not supported"},
        RejectCase{"IsNullAfterLimit", "SELECT a FROM events LIMIT 5 ^ISNULL", kUnsupported, 6,
                   "ISNULL is not supported"},
        // Operators, parameters and literals DuckDB parses but the subset does not support.
        RejectCase{"RegexOperator", "SELECT a FROM events WHERE url ^~ 'x'", kUnsupported, 1,
                   "operator '~' is not supported"},
        RejectCase{"NotRegexOperator", "SELECT a FROM events WHERE url ^!~ 'x'", kUnsupported, 2,
                   "operator '!~' is not supported"},
        RejectCase{"DoubleEqualsOperator", "SELECT a FROM events WHERE a ^== 1", kUnsupported, 2,
                   "operator '==' is not supported"},
        RejectCase{"NotEqualGluedToMinus", "SELECT a FROM events WHERE a ^!=-1", kUnsupported, 3,
                   "operator '!=-' is not supported"},
        RejectCase{"BitwiseOperatorInSelect", "SELECT a ^& 1 FROM events", kUnsupported, 1,
                   "operator '&' is not supported"},
        RejectCase{"ShiftOperator", "SELECT a ^<< 2 FROM events", kUnsupported, 2,
                   "operator '<<' is not supported"},
        RejectCase{"ArrowOperator", "SELECT a ^->> 'k' FROM events", kUnsupported, 3,
                   "operator '->>' is not supported"},
        RejectCase{"PrefixOperator", "SELECT ^@a FROM events", kUnsupported, 1,
                   "operator '@' is not supported"},
        RejectCase{"QuestionMarkParameter", "SELECT a FROM events WHERE a = ^?", kUnsupported, 1,
                   "prepared statement parameters (?) are not supported"},
        RejectCase{"DollarParameter", "SELECT a FROM events WHERE ^$1 < a", kUnsupported, 2,
                   "parameters ($1) and dollar-quoted strings are not supported"},
        RejectCase{"DollarQuotedString", "SELECT a FROM events WHERE a = ^$$x$$", kUnsupported, 1,
                   "parameters ($1) and dollar-quoted strings are not supported"},
        RejectCase{"ListLiteral", "SELECT ^[1, 2] FROM events", kUnsupported, 1,
                   "list literals ([...]) are not supported"},
        RejectCase{"Subscript", "SELECT a^[1] FROM events", kUnsupported, 1,
                   "subscripts ([...]) are not supported"},
        RejectCase{"StructLiteral", "SELECT a FROM events WHERE a = ^{'k': 1}", kUnsupported, 1,
                   "struct literals ({...}) are not supported"},
        RejectCase{"HexLiteral", "SELECT a FROM events WHERE a = ^0x1F", kUnsupported, 4,
                   "hexadecimal, octal and binary integer literals are not supported"},
        RejectCase{"DigitSeparators", "SELECT a FROM events LIMIT ^1_000", kUnsupported, 5,
                   "digit separators in numbers (1_000) are not supported"},
        RejectCase{"NonAsciiIdentifier", "SELECT ^\xd0\xb8 FROM events", kUnsupported, 2,
                   "unquoted non-ASCII names are not supported (double-quote the name)"},
        RejectCase{"EscapeString", "SELECT a FROM events WHERE a = ^E'x\\n'", kUnsupported, 1,
                   "typed literals other than DATE '...', TIMESTAMP '...' and prefixed strings "
                   "(E'...') are not supported"},
        RejectCase{"TypedLiteralFirst", "SELECT a FROM events WHERE ^INT '1' = a", kUnsupported, 3,
                   "typed literals other than DATE"},
        RejectCase{"TypedLiteralInSelect", "SELECT ^int4 '1' FROM events", kUnsupported, 4,
                   "typed literals other than DATE"},
        // A string after a qualified name: a typed literal of a qualified type in DuckDB.
        RejectCase{"QualifiedTypedLiteral", "SELECT ^main.integer '5' FROM events", kUnsupported,
                   12, "typed literals other than DATE"},
        RejectCase{"QualifiedTypedLiteralInWhere",
                   "SELECT a FROM events WHERE b = ^main.integer '2'", kUnsupported, 12,
                   "typed literals other than DATE '...', TIMESTAMP '...' and prefixed strings "
                   "(E'...') are not supported"},
        RejectCase{"QuotedQualifiedTypedLiteral",
                   R"(SELECT COUNT(^"main"."integer" '5') FROM events)", kUnsupported, 16,
                   "typed literals other than DATE"},
        RejectCase{"QualifiedTypedEscapeString", "SELECT a FROM events ORDER BY ^main.mood E'x'",
                   kUnsupported, 9, "typed literals other than DATE"},
        RejectCase{"QualifiedTypedDollarQuotedString", "SELECT ^e.a $$x$$ FROM events e",
                   kUnsupported, 3, "typed literals other than DATE"},
        // E and a string with a space between them, and N before a string, which DuckDB lexes as
        // NCHAR and a string, make a typed literal of a qualified type in DuckDB too.
        RejectCase{"QualifiedTypeSpacedEscape", "SELECT ^e.E 'x' FROM events e", kUnsupported, 3,
                   "typed literals other than DATE"},
        RejectCase{"QualifiedTypeNationalString", "SELECT ^e.N'x' FROM events e", kUnsupported, 3,
                   "typed literals other than DATE"},
        // A quoted E before a string is a name, not the start of an escape string.
        RejectCase{"QualifiedQuotedTypeLetter", "SELECT ^e.\"E\"'x' FROM events e", kUnsupported, 5,
                   "typed literals other than DATE"},
        RejectCase{"QualifiedQuotedTypeLetterLimit", "SELECT a FROM events LIMIT ^e.\"E\"'5'",
                   kUnsupported, 5, "LIMIT expressions are not supported"},
        // So does an escape or a dollar-quoted string after a name, and any string after a quoted
        // name; DATE and TIMESTAMP make literals before a plain string only.
        RejectCase{"TypedEscapeString", "SELECT ^integer E'5' FROM events", kUnsupported, 7,
                   "typed literals other than DATE"},
        RejectCase{"DateEscapeString", "SELECT a FROM events WHERE d = ^DATE E'2020-01-01'",
                   kUnsupported, 4,
                   "typed literals other than DATE '...', TIMESTAMP '...' and prefixed strings "
                   "(E'...') are not supported"},
        RejectCase{"TypedDollarQuotedString", "SELECT ^integer $$5$$ FROM events", kUnsupported, 7,
                   "typed literals other than DATE"},
        RejectCase{"TimestampTaggedDollarQuotedString",
                   "SELECT a FROM events WHERE ts < ^TIMESTAMP $t$2020-01-01$t$", kUnsupported, 9,
                   "typed literals other than DATE"},
        RejectCase{"TimeEscapeString", "SELECT ^TIME E'12:00' FROM events", kUnsupported, 4,
                   "TIME literals are not supported"},
        RejectCase{"QuotedTypedLiteral", R"(SELECT ^"integer" '5' FROM events)", kUnsupported, 9,
                   "typed literals other than DATE"},
        RejectCase{"QuotedTypedLiteralInWhere",
                   R"(SELECT a FROM events WHERE d = ^"DATE" '2020-01-01')", kUnsupported, 6,
                   "typed literals other than DATE"},
        RejectCase{"QuotedTypedEscapeString", R"(SELECT a FROM events ORDER BY ^"integer" E'5')",
                   kUnsupported, 9, "typed literals other than DATE"},
        RejectCase{"QuotedTypedDollarQuotedString", R"(SELECT COUNT(^"integer" $$5$$) FROM events)",
                   kUnsupported, 9, "typed literals other than DATE"},
        RejectCase{"CountWithoutArgument", "SELECT COUNT(^) FROM events", kUnsupported, 1,
                   "COUNT() without an argument is not supported (use COUNT(*))"},
        RejectCase{"TrailingComma", "SELECT a, b^, FROM events", kUnsupported, 1,
                   "a trailing comma in the select list is not supported"},
        RejectCase{"TrailingCommaAtEnd", "SELECT a^,", kUnsupported, 1,
                   "a trailing comma in the select list is not supported"},
        RejectCase{"ReservedAlias", "SELECT a AS ^FROM events", kUnsupported, 4,
                   "an alias cannot be the reserved word FROM"},
        RejectCase{"ReservedAliasLowerCase", "SELECT a AS ^select FROM events", kUnsupported, 6,
                   "an alias cannot be the reserved word SELECT"},
        RejectCase{"StringAlias", "SELECT a AS ^'x' FROM events", kUnsupported, 3,
                   "string literal aliases are not supported"},
        RejectCase{"StarExclude", "SELECT * ^EXCLUDE (a) FROM events", kUnsupported, 7,
                   "SELECT * EXCLUDE is not supported"},
        RejectCase{"StarReplace", "SELECT * ^replace (a + 1 AS a) FROM events", kUnsupported, 7,
                   "SELECT * REPLACE is not supported"},
        RejectCase{"StarLike", "SELECT * ^LIKE 'a%' FROM events", kUnsupported, 4,
                   "SELECT * LIKE is not supported"},
        RejectCase{"LimitPercent", "SELECT a FROM events LIMIT 10 ^PERCENT", kUnsupported, 7,
                   "LIMIT with a percentage is not supported"},
        RejectCase{"LimitPercentSign", "SELECT a FROM events LIMIT 10^%", kUnsupported, 1,
                   "LIMIT with a percentage is not supported"},
        RejectCase{"DecimalLimit", "SELECT a FROM events LIMIT ^1.5", kUnsupported, 3,
                   "non-integer LIMIT values are not supported"},
        RejectCase{"StringLimit", "SELECT a FROM events LIMIT ^'5'", kUnsupported, 3,
                   "LIMIT expressions are not supported"},
        RejectCase{"ParameterLimit", "SELECT a FROM events LIMIT ^$1", kUnsupported, 2,
                   "LIMIT expressions are not supported"},
        // A typed literal or a call, also of a quoted or a qualified name, is an expression too.
        RejectCase{"TypedLiteralLimit", "SELECT a FROM events LIMIT ^integer '5'", kUnsupported, 7,
                   "LIMIT expressions are not supported (LIMIT takes an integer)"},
        RejectCase{"EscapeStringLimit", "SELECT a FROM events LIMIT ^E'5'", kUnsupported, 1,
                   "LIMIT expressions are not supported"},
        RejectCase{"TypedEscapeStringLimit", "SELECT a FROM events LIMIT ^integer E'5'",
                   kUnsupported, 7, "LIMIT expressions are not supported"},
        RejectCase{"QuotedTypedDollarQuotedLimit", R"(SELECT a FROM events LIMIT ^"integer" $$5$$)",
                   kUnsupported, 9, "LIMIT expressions are not supported"},
        RejectCase{"QualifiedTypedLiteralLimit", "SELECT a FROM events LIMIT ^main.integer '5'",
                   kUnsupported, 12, "LIMIT expressions are not supported"},
        RejectCase{"TypedLiteralOffset", "SELECT a FROM events OFFSET ^integer '0'", kUnsupported,
                   7, "OFFSET expressions are not supported (OFFSET takes an integer)"},
        RejectCase{"QuotedQualifiedTypedEscapeOffset",
                   R"(SELECT a FROM events LIMIT 1 OFFSET ^"main"."integer" E'0')", kUnsupported,
                   16, "OFFSET expressions are not supported (OFFSET takes an integer)"},
        RejectCase{"QuotedFunctionLimit", R"(SELECT a FROM events LIMIT ^"abs"(5))", kUnsupported,
                   5, "LIMIT expressions are not supported"},
        RejectCase{"QualifiedFunctionLimit", "SELECT a FROM events LIMIT ^main.abs(5)",
                   kUnsupported, 8, "LIMIT expressions are not supported"},
        // Names of any number of parts, and a reserved word after a dot, as DuckDB takes them.
        RejectCase{"ThreePartFunctionLimit", "SELECT a FROM events LIMIT ^system.main.abs(5)",
                   kUnsupported, 15, "LIMIT expressions are not supported"},
        RejectCase{"ThreePartTypedLiteralLimit",
                   "SELECT a FROM events LIMIT ^system.main.integer '5'", kUnsupported, 19,
                   "LIMIT expressions are not supported"},
        RejectCase{"QualifiedReservedFunctionLimit",
                   "SELECT a FROM events LIMIT ^main.left('5', 1)", kUnsupported, 9,
                   "LIMIT expressions are not supported"},
        RejectCase{"QualifiedReservedFunctionOffset",
                   "SELECT a FROM events OFFSET ^main.left('8', 1)", kUnsupported, 9,
                   "OFFSET expressions are not supported (OFFSET takes an integer)"},
        RejectCase{"UsingSample", "SELECT a FROM events ^USING SAMPLE 10%", kUnsupported, 5,
                   "USING SAMPLE is not supported"},
        RejectCase{"UsingSampleAfterWhere", "SELECT a FROM events WHERE a = 1 ^using sample 5",
                   kUnsupported, 5, "USING SAMPLE is not supported"},
        RejectCase{"TableSample", "SELECT a FROM events ^TABLESAMPLE 10%", kUnsupported, 11,
                   "TABLESAMPLE is not supported"}),
    CaseName);

// Malformed input: kSyntax with a precise span.
INSTANTIATE_TEST_SUITE_P(
    Syntax, RejectTest,
    ::testing::Values(
        RejectCase{"Empty", "^", kSyntax, 0, "empty query; expected SELECT"},
        RejectCase{"OnlyComment", "  -- nothing\n^", kSyntax, 0, "empty query; expected SELECT"},
        RejectCase{"BetweenWithoutAnd", "SELECT a FROM t WHERE b BETWEEN 1 ^OR 2", kSyntax, 2,
                   "expected AND in BETWEEN, found keyword OR"},
        RejectCase{"BetweenWithoutHigh", "SELECT a FROM t WHERE b BETWEEN 1 AND ^", kSyntax, 0,
                   "expected"},
        RejectCase{"InEmptyList", "SELECT a FROM t WHERE b IN (^)", kSyntax, 1,
                   "expected a value in IN (...), found )"},
        RejectCase{"InWithoutParen", "SELECT a FROM t WHERE b IN ^1", kSyntax, 1,
                   "expected ( after IN, found"},
        RejectCase{"InUnclosed", "SELECT a FROM t WHERE b IN (1, 2^", kSyntax, 0,
                   "expected , or ) in IN (...), found end of input"},
        RejectCase{"OnlySemicolon", "^;", kSyntax, 1, "expected SELECT, found ';'"},
        RejectCase{"Misspelled", "^SELEC a FROM events", kSyntax, 5,
                   "expected SELECT, found identifier SELEC"},
        RejectCase{"Number", "^42", kSyntax, 2, "expected SELECT, found integer literal 42"},
        RejectCase{"SelectAlone", "SELECT^", kSyntax, 0,
                   "expected an expression or '*', found end of input"},
        RejectCase{"EmptySelectList", "SELECT ^FROM events", kSyntax, 4,
                   "expected an expression or '*', found keyword FROM"},
        RejectCase{"MissingTable", "SELECT a FROM^", kSyntax, 0,
                   "expected a table name or a quoted file path, found end of input"},
        RejectCase{"ReservedTable", "SELECT a FROM ^WHERE a = 1", kSyntax, 5,
                   "expected a table name or a quoted file path, found keyword WHERE"},
        RejectCase{"NumericTable", "SELECT a FROM ^5", kSyntax, 1,
                   "expected a table name or a quoted file path, found integer literal 5"},
        RejectCase{"MissingComma", "SELECT a b ^c FROM events", kSyntax, 1,
                   "expected ',' or FROM, found identifier c"},
        RejectCase{"StarStar", "SELECT * ^* FROM events", kSyntax, 1, "expected FROM, found '*'"},
        RejectCase{"ReservedColumn", "SELECT ^order FROM events", kSyntax, 5,
                   "expected an expression or '*', found keyword ORDER"},
        RejectCase{"MissingAlias", "SELECT a AS^", kSyntax, 0,
                   "expected an alias after AS, found end of input"},
        RejectCase{"SumStar", "SELECT SUM(^*) FROM events", kSyntax, 1, "only COUNT accepts '*'"},
        RejectCase{"MaxNothing", "SELECT MAX(^) FROM events", kSyntax, 1,
                   "expected a column in MAX()"},
        RejectCase{"UnclosedCountStar", "SELECT COUNT(* ^FROM events", kSyntax, 4,
                   "expected ')' to close COUNT(, found keyword FROM"},
        RejectCase{"TwoArguments", "SELECT SUM(a^, b) FROM events", kSyntax, 1,
                   "SUM takes one argument"},
        RejectCase{"AggregateWithoutArgument", "SELECT AVG(^FROM) FROM events", kSyntax, 4,
                   "expected an expression, found keyword FROM"},
        RejectCase{"NestedAggregate", "SELECT SUM(^COUNT(a)) FROM events", kSyntax, 5,
                   "aggregate function calls cannot be nested"},
        RejectCase{"AggregateInWhere", "SELECT a FROM events WHERE ^COUNT(*) > 1", kSyntax, 5,
                   "aggregate functions are not allowed in WHERE"},
        RejectCase{"EmptyWhere", "SELECT a FROM events WHERE^", kSyntax, 0,
                   "expected an expression, found end of input"},
        RejectCase{"ReservedInWhere", "SELECT a FROM events WHERE ^LIMIT 5", kSyntax, 5,
                   "expected an expression, found keyword LIMIT"},
        RejectCase{"MissingOperator", "SELECT a FROM events WHERE a ^b", kSyntax, 1,
                   "unexpected identifier b; expected AND, GROUP BY, HAVING, ORDER BY, LIMIT, "
                   "OFFSET or the end of the query"},
        RejectCase{"DoubleEquals", "SELECT a FROM events WHERE a = ^= 1", kSyntax, 1,
                   "expected an expression, found '='"},
        RejectCase{"MissingRightOperand", "SELECT a FROM events WHERE a =^", kSyntax, 0,
                   "expected an expression, found end of input"},
        RejectCase{"DanglingAnd", "SELECT a FROM events WHERE a = 1 AND^", kSyntax, 0,
                   "expected an expression, found end of input"},
        RejectCase{"ChainedComparison", "SELECT a FROM events WHERE a = 1 ^= 2", kUnsupported, 1,
                   "chained comparisons (a = b = c) are not supported"},
        RejectCase{"ExtractStringField", "SELECT EXTRACT(^'minute' FROM a) FROM events", kSyntax, 8,
                   "expected a field name in EXTRACT(, found string literal"},
        RejectCase{"ExtractWithoutFrom", "SELECT EXTRACT(minute ^a) FROM events", kSyntax, 1,
                   "expected FROM in EXTRACT(field FROM ...), found identifier a"},
        RejectCase{"ExtractWithoutSource", "SELECT EXTRACT(minute FROM ^) FROM events", kSyntax, 1,
                   "expected an expression or '*', found ')'"},
        RejectCase{"ExtractUnclosed", "SELECT EXTRACT(minute FROM a ^b) FROM events", kSyntax, 1,
                   "expected ) to close EXTRACT(, found identifier b"},
        RejectCase{"CaseWithoutWhen", "SELECT CASE a ^END FROM events", kSyntax, 3,
                   "expected WHEN in CASE, found keyword END"},
        RejectCase{"CaseWithoutThen", "SELECT CASE WHEN a ^END FROM events", kSyntax, 3,
                   "expected THEN in CASE, found keyword END"},
        RejectCase{"CaseEmptyWhen", "SELECT CASE WHEN ^THEN 1 END FROM events", kSyntax, 4,
                   "expected an expression or '*', found keyword THEN"},
        RejectCase{"CaseEmptyThen", "SELECT CASE WHEN a THEN ^END FROM events", kSyntax, 3,
                   "expected an expression or '*', found keyword END"},
        RejectCase{"CaseEmptyElse", "SELECT CASE WHEN a THEN 1 ELSE ^END FROM events", kSyntax, 3,
                   "expected an expression or '*', found keyword END"},
        RejectCase{"CaseUnclosed", "SELECT CASE WHEN a THEN 1 ^FROM events", kSyntax, 4,
                   "expected WHEN, ELSE or END in CASE, found keyword FROM"},
        RejectCase{"InListEmptyValue", "SELECT a FROM events WHERE a IN (1, ^)", kSyntax, 1,
                   "expected an expression, found ')'"},
        RejectCase{"MissingLimit", "SELECT a FROM events LIMIT^", kSyntax, 0,
                   "expected a non-negative integer after LIMIT, found end of input"},
        RejectCase{"NegativeLimit", "SELECT a FROM events LIMIT ^-1", kSyntax, 1,
                   "LIMIT must not be negative"},
        RejectCase{"LimitOverflow", "SELECT a FROM events LIMIT ^9223372036854775808", kSyntax, 19,
                   "LIMIT 9223372036854775808 is out of range"},
        RejectCase{"LimitHuge", "SELECT a FROM events LIMIT ^123456789012345678901234567890",
                   kSyntax, 30, "LIMIT 123456789012345678901234567890 is out of range"},
        RejectCase{"WhereAfterLimit", "SELECT a FROM events LIMIT 5 ^WHERE a = 1", kSyntax, 5,
                   "unexpected keyword WHERE; expected OFFSET or the end of the query"},
        RejectCase{"DuplicateWhere", "SELECT a FROM events WHERE a = 1 ^WHERE b = 2", kSyntax, 5,
                   "unexpected keyword WHERE; expected AND, GROUP BY, HAVING, ORDER BY, LIMIT, "
                   "OFFSET or the end of the query"},
        RejectCase{"DuplicateLimit", "SELECT a FROM events LIMIT 1 ^LIMIT 2", kSyntax, 5,
                   "unexpected keyword LIMIT; expected OFFSET or the end of the query"},
        RejectCase{"StrayParen", "SELECT a FROM events^)", kSyntax, 1,
                   "unexpected ')'; expected WHERE, GROUP BY, HAVING, ORDER BY, LIMIT, OFFSET or "
                   "the end of the query"},
        RejectCase{"GroupWithoutBy", "SELECT a FROM events GROUP ^a", kSyntax, 1,
                   "expected BY after GROUP, found identifier a"},
        RejectCase{"OrderWithoutBy", "SELECT a FROM events ORDER ^a", kSyntax, 1,
                   "expected BY after ORDER, found identifier a"},
        RejectCase{"EmptyGroupBy", "SELECT a FROM events GROUP BY^", kSyntax, 0,
                   "expected an expression, found end of input"},
        RejectCase{"EmptyOrderBy", "SELECT a FROM events ORDER BY ^LIMIT 1", kSyntax, 5,
                   "expected an expression, found keyword LIMIT"},
        RejectCase{"GroupByAggregate", "SELECT a FROM events GROUP BY ^COUNT(a)", kSyntax, 5,
                   "aggregate functions are not allowed in GROUP BY"},
        RejectCase{"NullsWithoutFirstOrLast", "SELECT a FROM events ORDER BY a NULLS ^LATE",
                   kSyntax, 4, "expected FIRST or LAST after NULLS, found identifier LATE"},
        RejectCase{"CountDistinctStar", "SELECT COUNT(DISTINCT ^*) FROM events", kSyntax, 1,
                   "expected a column after DISTINCT, found '*'"},
        RejectCase{"CountDistinctEmpty", "SELECT COUNT(DISTINCT ^) FROM events", kSyntax, 1,
                   "expected a column after DISTINCT, found ')'"},
        RejectCase{"GroupByAfterOrderBy", "SELECT a FROM events ORDER BY a ^GROUP BY a", kSyntax, 5,
                   "unexpected keyword GROUP; expected LIMIT, OFFSET or the end of the query"},
        RejectCase{"WhereAfterGroupBy", "SELECT a FROM events GROUP BY a ^WHERE a = 1", kSyntax, 5,
                   "unexpected keyword WHERE; expected HAVING, ORDER BY, LIMIT, OFFSET or the end "
                   "of the query"},
        RejectCase{"GroupByAfterHaving", "SELECT a FROM events HAVING COUNT(*) > 1 ^GROUP BY a",
                   kSyntax, 5,
                   "unexpected keyword GROUP; expected AND, ORDER BY, LIMIT, OFFSET or the end of "
                   "the query"},
        RejectCase{"HavingAfterOrderBy", "SELECT a FROM events ORDER BY a ^HAVING COUNT(*) > 1",
                   kSyntax, 6, "unexpected keyword HAVING; expected LIMIT, OFFSET or the end"},
        RejectCase{"EmptyHaving", "SELECT a FROM events GROUP BY a HAVING^", kSyntax, 0,
                   "expected an expression, found end of input"},
        RejectCase{"DuplicateOffset", "SELECT a FROM events OFFSET 1 ^OFFSET 2", kSyntax, 6,
                   "unexpected keyword OFFSET; expected LIMIT or the end of the query"},
        RejectCase{"ThirdLimit", "SELECT a FROM events LIMIT 1 OFFSET 2 ^LIMIT 3", kSyntax, 5,
                   "unexpected keyword LIMIT; expected the end of the query"},
        RejectCase{"NegativeOffset", "SELECT a FROM events OFFSET ^-1", kSyntax, 1,
                   "OFFSET must not be negative"},
        // Names before what is no string in DuckDB: E and a string with a space between them, and
        // a parameter.
        RejectCase{"TypeSpacedEscape", "SELECT integer E ^'5' FROM events", kSyntax, 3,
                   "expected ',' or FROM, found string literal"},
        RejectCase{"DateSpacedEscape", "SELECT a FROM events WHERE d = DATE ^E '2020-01-01'",
                   kSyntax, 1, "unexpected identifier E"},
        RejectCase{"QuotedTypeParameter", R"(SELECT "integer" ^$1 FROM events)", kSyntax, 2,
                   "expected ',' or FROM, found parameter"},
        // A column after LIMIT or OFFSET, which DuckDB parses and then refuses, and names there
        // before what is no string in DuckDB.
        RejectCase{"ColumnLimit", "SELECT a FROM events LIMIT ^a", kSyntax, 1,
                   "expected a non-negative integer after LIMIT, found identifier a"},
        RejectCase{"QualifiedColumnOffset", "SELECT a FROM events OFFSET ^e.a", kSyntax, 1,
                   "expected a non-negative integer after OFFSET, found identifier e"},
        RejectCase{"ThreePartColumnLimit", "SELECT a FROM events LIMIT ^a.b.c", kSyntax, 1,
                   "expected a non-negative integer after LIMIT, found identifier a"},
        // A call or a typed literal of a name of more than three parts, which DuckDB does not
        // parse: it takes catalog.schema.name at most.
        RejectCase{"FourPartFunctionLimit", "SELECT a FROM events LIMIT ^a.b.c.d(1)", kSyntax, 1,
                   "expected a non-negative integer after LIMIT, found identifier a"},
        RejectCase{"FourPartQuotedReservedFunctionOffset",
                   R"(SELECT a FROM events OFFSET ^"a".b.c.left('5', 1))", kSyntax, 3,
                   "expected a non-negative integer after OFFSET, found quoted identifier"},
        RejectCase{"FourPartTypedLiteralOffset", "SELECT a FROM events OFFSET ^a.b.c.d '5'",
                   kSyntax, 1, "expected a non-negative integer after OFFSET, found identifier a"},
        RejectCase{"FourPartTypedEscapeLimit", "SELECT a FROM events LIMIT ^a.b.c.d E'5'", kSyntax,
                   1, "expected a non-negative integer after LIMIT, found identifier a"},
        RejectCase{"FivePartTypedDollarQuotedOffset",
                   "SELECT a FROM events LIMIT 1 OFFSET ^a.b.c.d.e $$5$$", kSyntax, 1,
                   "expected a non-negative integer after OFFSET, found identifier a"},
        RejectCase{"QualifiedReservedColumnLimit", "SELECT a FROM events LIMIT ^main.left", kSyntax,
                   4, "expected a non-negative integer after LIMIT, found identifier main"},
        RejectCase{"QualifiedTypeSpacedEscapeLimit",
                   "SELECT a FROM events LIMIT ^main.integer E '5'", kSyntax, 4,
                   "expected a non-negative integer after LIMIT, found identifier main"},
        RejectCase{"TypeParameterLimit", "SELECT a FROM events LIMIT ^integer $1", kSyntax, 7,
                   "expected a non-negative integer after LIMIT, found identifier integer"},
        RejectCase{"OrderByAfterLimit", "SELECT a FROM events LIMIT 5 ^ORDER BY a", kSyntax, 5,
                   "unexpected keyword ORDER; expected OFFSET or the end of the query"},
        RejectCase{"UnterminatedString", "SELECT a FROM events WHERE a = ^'open", kSyntax, 5,
                   "unterminated string literal"},
        RejectCase{"UnterminatedIdentifier", R"(SELECT ^"open FROM events)", kSyntax, 17,
                   "unterminated quoted identifier"},
        RejectCase{"UnterminatedComment", "SELECT a FROM events ^/* open", kSyntax, 7,
                   "unterminated block comment"},
        RejectCase{"UnexpectedCharacter", "SELECT a FROM events WHERE a ^\\ 1", kSyntax, 1,
                   "unexpected character '\\'"},
        RejectCase{"UnmatchedBracket", "SELECT a FROM events^]", kSyntax, 1,
                   "unexpected character ']'"},
        RejectCase{"InvalidNumber", "SELECT a FROM events WHERE a = ^12abc", kSyntax, 3,
                   "invalid number literal"},
        RejectCase{"ZeroLengthIdentifier", R"(SELECT ^"" FROM events)", kSyntax, 2,
                   "zero-length quoted identifier"},
        RejectCase{"InvalidUtf8", "SELECT ^\xff\xfe FROM events", kSyntax, 1,
                   "unexpected byte 0xFF (invalid UTF-8)"},
        RejectCase{"UnterminatedNestedComment", "SELECT a FROM events ^/* /* */", kSyntax, 8,
                   "unterminated block comment"},
        RejectCase{"MalformedHexLiteral", "SELECT a FROM events WHERE a = ^0x", kSyntax, 2,
                   "invalid number literal"},
        RejectCase{"NumberGluedToKeyword", "SELECT a FROM events WHERE a = ^1AND b = 2", kSyntax, 2,
                   "invalid number literal"},
        RejectCase{"ParserErrorBeforeLexerError", "SELECT a b ^c 'open", kSyntax, 1,
                   "expected ',' or FROM, found identifier c"},
        RejectCase{"LexerErrorWinsOnceReached", "SELECT a FROM events WHERE 5 ^'open", kSyntax, 5,
                   "unterminated string literal"},
        RejectCase{"LexerErrorAfterSemicolon", "SELECT a FROM events; ^'open", kSyntax, 5,
                   "unterminated string literal"},
        RejectCase{"CastWithoutAs", "SELECT CAST(a^) FROM events", kSyntax, 1,
                   "expected AS in CAST(x AS type), found ')'"},
        RejectCase{"CastTypeWithoutAs", "SELECT CAST(a ^BIGINT) FROM events", kSyntax, 6,
                   "expected AS in CAST(x AS type), found identifier BIGINT"},
        RejectCase{"CastComma", "SELECT CAST(a^, BIGINT) FROM events", kSyntax, 1,
                   "expected AS in CAST(x AS type), found ','"},
        RejectCase{"TryCastWithoutAs", "SELECT TRY_CAST(a^) FROM events", kSyntax, 1,
                   "expected AS in TRY_CAST(x AS type), found ')'"},
        RejectCase{"CastWithoutType", "SELECT CAST(a AS ^) FROM events", kSyntax, 1,
                   "expected a type name, found ')'"},
        RejectCase{"CastNumberType", "SELECT CAST(a AS ^5) FROM events", kSyntax, 1,
                   "expected a type name, found integer literal 5"},
        RejectCase{"CastReservedType", "SELECT CAST(a AS ^FROM) FROM events", kSyntax, 4,
                   "expected a type name, found keyword FROM"},
        RejectCase{"CastOperatorWithoutType", "SELECT a:: ^FROM events", kSyntax, 4,
                   "expected a type name, found keyword FROM"},
        RejectCase{"CastTwoTypes", "SELECT CAST(a AS INT ^b) FROM events", kSyntax, 1,
                   "expected ) to close CAST(, found identifier b"},
        RejectCase{"CastUnclosed", "SELECT CAST(a AS INT ^FROM events", kSyntax, 4,
                   "expected ) to close CAST(, found keyword FROM"},
        RejectCase{"CastEmptyParameters", "SELECT CAST(a AS DECIMAL(^)) FROM events", kSyntax, 1,
                   "expected a type parameter, found ')'"},
        RejectCase{"CastParametersWithoutComma", "SELECT CAST(a AS DECIMAL(15 ^2)) FROM events",
                   kSyntax, 1, "expected , or ) after a type parameter, found integer literal 2"},
        RejectCase{"BareCast", "SELECT ^CAST FROM events", kSyntax, 4,
                   "expected an expression or '*', found keyword CAST"},
        // The FROM list: where DuckDB gives a syntax error, so does antb1.
        RejectCase{"JoinWithoutOn", "SELECT a FROM events JOIN users^", kSyntax, 0,
                   "expected ON after the JOIN at offset 21, found end of input"},
        RejectCase{"JoinCommaBeforeOn", "SELECT a FROM events JOIN users^, items ON a = b", kSyntax,
                   1, "expected ON after the JOIN at offset 21, found ','"},
        RejectCase{"CrossJoinWithOn", "SELECT a FROM events CROSS JOIN users ^ON a = b", kSyntax, 2,
                   "unexpected keyword ON; expected WHERE"},
        RejectCase{"CommaWithOn", "SELECT a FROM events, users ^ON a = b", kSyntax, 2,
                   "unexpected keyword ON"},
        RejectCase{"OnWithoutJoin", "SELECT a FROM events ^ON a = b", kSyntax, 2,
                   "unexpected keyword ON"},
        RejectCase{"OnTwice", "SELECT a FROM events JOIN users ON a = b ^ON a = c", kSyntax, 2,
                   "unexpected keyword ON"},
        RejectCase{"EmptyOn", "SELECT a FROM events JOIN users ON^", kSyntax, 0,
                   "expected an expression, found end of input"},
        RejectCase{"OnWhere", "SELECT a FROM events JOIN users ON ^WHERE a = 1", kSyntax, 5,
                   "expected an expression, found keyword WHERE"},
        RejectCase{"AggregateInOn", "SELECT a FROM events JOIN users ON ^SUM(a) = 1", kSyntax, 3,
                   "aggregate functions are not allowed in ON"},
        RejectCase{"CrossWithoutJoin", "SELECT a FROM events CROSS ^users", kSyntax, 5,
                   "expected JOIN after CROSS, found identifier users"},
        RejectCase{"InnerWithoutJoin", "SELECT a FROM events INNER ^users ON a = b", kSyntax, 5,
                   "expected JOIN after INNER, found identifier users"},
        RejectCase{"InnerOuterJoin", "SELECT a FROM events INNER ^OUTER JOIN users ON a = b",
                   kSyntax, 5, "expected JOIN after INNER, found keyword OUTER"},
        RejectCase{"LeftWithoutJoin", "SELECT a FROM events LEFT ^users ON a = b", kSyntax, 5,
                   "expected JOIN after LEFT, found identifier users"},
        RejectCase{"LeftOuterWithoutJoin", "SELECT a FROM events LEFT OUTER ^users ON a = b",
                   kSyntax, 5, "expected JOIN after LEFT OUTER, found identifier users"},
        RejectCase{"RightWithoutJoin", "SELECT a FROM events RIGHT^", kSyntax, 0,
                   "expected JOIN after RIGHT, found end of input"},
        RejectCase{"NaturalCross", "SELECT a FROM events NATURAL ^CROSS JOIN users", kSyntax, 5,
                   "expected JOIN after NATURAL, found keyword CROSS"},
        RejectCase{"NaturalLeftWithoutJoin", "SELECT a FROM events NATURAL LEFT ^users", kSyntax, 5,
                   "expected JOIN after NATURAL LEFT, found identifier users"},
        RejectCase{"NaturalSemiWithoutJoin", "SELECT a FROM events NATURAL semi ^OUTER JOIN users",
                   kSyntax, 5, "expected JOIN after NATURAL SEMI, found keyword OUTER"},
        RejectCase{"AsofLeftWithoutJoin", "SELECT a FROM events ^ASOF LEFT users ON a >= b",
                   kSyntax, 4, "a table alias cannot be the keyword ASOF"},
        RejectCase{"AsofInnerAfterOnWithoutJoin",
                   "SELECT a FROM events JOIN users ON a = b ^asof INNER items ON a >= c", kSyntax,
                   4, "unexpected identifier asof"},
        // Before the ON of a JOIN: a join keyword without the rest of its join is no nested join.
        RejectCase{"NestedLeftWithoutJoin", "SELECT a FROM events JOIN users LEFT ^x ON a = b",
                   kSyntax, 1, "expected JOIN after LEFT, found identifier x"},
        RejectCase{"NestedRightAtEnd", "SELECT a FROM events JOIN users RIGHT^", kSyntax, 0,
                   "expected JOIN after RIGHT, found end of input"},
        RejectCase{"NestedFullOuterWithoutJoin",
                   "SELECT a FROM events JOIN users FULL OUTER ^ON a = b", kSyntax, 2,
                   "expected JOIN after FULL OUTER, found keyword ON"},
        RejectCase{"NestedCrossWithoutJoin",
                   "SELECT a FROM events JOIN users CROSS ^items ON a = b", kSyntax, 5,
                   "expected JOIN after CROSS, found identifier items"},
        RejectCase{"NestedNaturalWithoutJoin",
                   "SELECT a FROM events JOIN users NATURAL LEFT ^items ON a = b", kSyntax, 5,
                   "expected JOIN after NATURAL LEFT, found identifier items"},
        RejectCase{"OuterJoinAlone", "SELECT a FROM events ^OUTER JOIN users ON a = b", kSyntax, 5,
                   "expected LEFT, RIGHT or FULL before OUTER"},
        RejectCase{"JoinBeforeFrom", "SELECT 1 ^JOIN users ON a = b", kSyntax, 4,
                   "expected ',' or FROM, found keyword JOIN"},
        RejectCase{"JoinAfterWhere", "SELECT a FROM events WHERE a = 1 ^JOIN users ON a = b",
                   kSyntax, 4,
                   "unexpected keyword JOIN; expected AND, GROUP BY, HAVING, ORDER BY, LIMIT, "
                   "OFFSET or the end of the query"},
        // A join keyword after a list comma is no clause, so the comma is no trailing comma.
        RejectCase{"JoinAfterSelectComma", "SELECT a, ^JOIN FROM events", kSyntax, 4,
                   "expected an expression or '*', found keyword JOIN"},
        RejectCase{"LeftAfterGroupByComma", "SELECT a FROM events GROUP BY a, ^LEFT", kSyntax, 4,
                   "expected an expression, found keyword LEFT"},
        RejectCase{"TwoAliases", "SELECT a FROM events e ^f", kSyntax, 1,
                   "unexpected identifier f"},
        RejectCase{"AliasAfterAlias", "SELECT a FROM events e ^AS f", kSyntax, 2,
                   "unexpected keyword AS"},
        RejectCase{"AsWithoutAlias", "SELECT a FROM events AS^", kSyntax, 0,
                   "expected a table alias after AS, found end of input"},
        RejectCase{"ParameterAlias", "SELECT a FROM events AS ^$1", kSyntax, 2,
                   "expected a table alias after AS, found parameter"},
        RejectCase{"NamedParameterAlias", "SELECT a FROM events AS ^$x", kSyntax, 2,
                   "expected a table alias after AS, found parameter"},
        RejectCase{"NumberTagIsNoDollarQuote", "SELECT a FROM events AS ^$1$x$1$", kSyntax, 2,
                   "expected a table alias after AS, found parameter"},
        RejectCase{"EscapeStringAfterSpace", "SELECT a FROM events AS E ^'x'", kSyntax, 3,
                   "unexpected string literal"},
        RejectCase{"BitStringAlias", "SELECT a FROM events AS B^'1'", kSyntax, 3,
                   "unexpected string literal"},
        RejectCase{"ImplicitDollarQuotedAlias", "SELECT a FROM events ^$$x$$", kSyntax, 1,
                   "unexpected parameter"},
        RejectCase{"ReservedAliasAfterAs", "SELECT a FROM events AS ^select", kSyntax, 6,
                   "expected a table alias after AS, found keyword SELECT"},
        RejectCase{"NotAnAliasAfterAs", "SELECT a FROM events AS ^semi", kSyntax, 4,
                   "a table alias cannot be the keyword SEMI; write it as a quoted identifier"},
        RejectCase{"NotAnAliasImplicit", "SELECT a FROM events ^verbose", kSyntax, 7,
                   "a table alias cannot be the keyword VERBOSE"},
        RejectCase{"SemiWithoutJoin", "SELECT a FROM events ^semi users", kSyntax, 4,
                   "a table alias cannot be the keyword SEMI"},
        RejectCase{"TableSampleWithoutSample", "SELECT a FROM events ^TABLESAMPLE WHERE a = 1",
                   kSyntax, 11, "a table alias cannot be the keyword TABLESAMPLE"},
        RejectCase{"TableSampleNameWithoutParentheses",
                   "SELECT a FROM events ^TABLESAMPLE reservoir WHERE a = 1", kSyntax, 11,
                   "a table alias cannot be the keyword TABLESAMPLE"},
        RejectCase{"SemiAfterAlias", "SELECT a FROM events e ^semi", kSyntax, 4,
                   "unexpected identifier semi"},
        RejectCase{"TimeTravelAfterOn",
                   "SELECT a FROM events JOIN users ON a = b ^AT (VERSION => 1)", kSyntax, 2,
                   "unexpected identifier AT"},
        RejectCase{"TableSampleAfterOn",
                   "SELECT a FROM events JOIN users ON a = b ^TABLESAMPLE 10%", kSyntax, 11,
                   "unexpected identifier TABLESAMPLE"},
        RejectCase{"StringImplicitAlias", "SELECT a FROM events ^'w'", kSyntax, 3,
                   "unexpected string literal"},
        RejectCase{"DoubleComma", "SELECT a FROM events, ^, users", kSyntax, 1,
                   "expected a table name or a quoted file path, found ','"},
        RejectCase{"CommaBeforeFrom", "SELECT 1 FROM events, ^FROM users", kSyntax, 4,
                   "expected a table name or a quoted file path, found keyword FROM"},
        RejectCase{"CommaBeforeInto", "SELECT 1 FROM events, ^into x", kSyntax, 4,
                   "expected a table name or a quoted file path, found keyword INTO"},
        RejectCase{"PathQualified", "SELECT a FROM 'x.parquet'^.y", kSyntax, 1, "unexpected '.'"},
        RejectCase{"TableInParentheses", "SELECT a FROM (events^)", kSyntax, 1,
                   "expected a join after the table in parentheses, found ')'"},
        RejectCase{"CommaInParentheses", "SELECT a FROM ('e.parquet'^, users)", kSyntax, 1,
                   "expected a join after the table in parentheses, found ','"},
        RejectCase{"UnclosedParentheses", "SELECT a FROM (events^", kSyntax, 0,
                   "expected a join after the table in parentheses, found end of input"},
        RejectCase{"SemicolonInParentheses", "SELECT a FROM (events^;", kSyntax, 1,
                   "expected a join after the table in parentheses, found ';'"},
        RejectCase{"EmptyParentheses", "SELECT a FROM (^)", kSyntax, 1,
                   "expected a table name, a quoted file path or a subquery after '(', found ')'"},
        RejectCase{"LateralWithoutParentheses", "SELECT a FROM events, ^LATERAL users", kSyntax, 7,
                   "expected a table name or a quoted file path, found keyword LATERAL"},
        RejectCase{"LateralAloneInParentheses", "SELECT a FROM (^LATERAL)", kSyntax, 7,
                   "expected a table name, a quoted file path or a subquery after '(', found "
                   "keyword LATERAL"},
        RejectCase{"LateralPathInParentheses",
                   "SELECT a FROM (^LATERAL 'e.parquet' CROSS JOIN users)", kSyntax, 7,
                   "expected a table name, a quoted file path or a subquery after '(', found "
                   "keyword LATERAL"},
        // A reserved word that DuckDB takes as a function name is no table name.
        RejectCase{"LeftAfterCommaWithoutCall", "SELECT a FROM events, ^LEFT users", kSyntax, 4,
                   "expected a table name or a quoted file path, found keyword LEFT"},
        RejectCase{"LeftInParenthesesWithoutCall", "SELECT a FROM (^left users)", kSyntax, 4,
                   "expected a table name, a quoted file path or a subquery after '(', found "
                   "keyword LEFT"},
        RejectCase{"BetweenCallAfterComma", "SELECT a FROM events, ^between(1)", kSyntax, 7,
                   "expected a table name or a quoted file path, found keyword BETWEEN"},
        // Before no dot BETWEEN, EXISTS, INTERVAL and OVER are no table names (DuckDB's are:
        // divergence D21), and BETWEEN, EXISTS and INTERVAL call no LATERAL function, as in DuckDB.
        RejectCase{"OverAsTableName", "SELECT a FROM ^over", kSyntax, 4,
                   "expected a table name or a quoted file path, found keyword OVER"},
        RejectCase{"ExistsAsTableNameInParentheses", "SELECT a FROM (^exists CROSS JOIN events)",
                   kSyntax, 6,
                   "expected a table name, a quoted file path or a subquery after '(', found "
                   "keyword EXISTS"},
        RejectCase{"LateralBetweenCall", "SELECT a FROM events, ^LATERAL between(1)", kSyntax, 7,
                   "expected a table name or a quoted file path, found keyword LATERAL"},
        // After a qualified name, what DuckDB lexes as no string constant.
        RejectCase{"QualifiedNameBeforeBitString", "SELECT main.integer B^'1' FROM events", kSyntax,
                   3, "expected ',' or FROM, found string literal"},
        RejectCase{"QualifiedNameBeforeSpacedEscapeString",
                   "SELECT main.integer E ^'5' FROM events", kSyntax, 3,
                   "expected ',' or FROM, found string literal"},
        // DuckDB lexes B'1', E'x' and X'1F' as one string constant each, which is no name after a
        // dot.
        RejectCase{"EscapeStringAfterDot", "SELECT e.^E'x' FROM events e", kSyntax, 4,
                   "expected a column name after '.', found string literal"},
        RejectCase{"BitStringAfterDot", "SELECT e.^B'1' FROM events e", kSyntax, 4,
                   "expected a column name after '.', found string literal"},
        RejectCase{"HexStringAfterDot", "SELECT e.^X'1F' FROM events e", kSyntax, 5,
                   "expected a column name after '.', found string literal"},
        RejectCase{"EscapeStringAfterDotInWhere", "SELECT a FROM events e WHERE a = e.^E'5'",
                   kSyntax, 4, "expected a column name after '.', found string literal"},
        RejectCase{"EscapeStringAfterDotLimit", "SELECT a FROM events LIMIT main.^E'5'", kSyntax, 4,
                   "expected a name after '.', found string literal"},
        RejectCase{"EscapeStringAfterDotOffset", "SELECT a FROM events OFFSET e.^e'0'", kSyntax, 4,
                   "expected a name after '.', found string literal"},
        RejectCase{"DotThenOperator", "SELECT e.^+ FROM events", kSyntax, 1,
                   "expected a column name after '.', found '+'"},
        RejectCase{"DotAtEnd", "SELECT e.^", kSyntax, 0,
                   "expected a column name after '.', found end of input"},
        // Derived tables and WITH lists where DuckDB gives a syntax error too.
        RejectCase{"SubqueryUnclosed", "SELECT a FROM (SELECT a FROM t^", kSyntax, 0,
                   "unexpected end of input; expected WHERE, GROUP BY, HAVING, ORDER BY, LIMIT, "
                   "OFFSET or ')'"},
        RejectCase{"SubquerySemicolon", "SELECT a FROM (SELECT a FROM t ORDER BY a^;)", kSyntax, 1,
                   "unexpected ';'; expected LIMIT, OFFSET or ')'"},
        RejectCase{"SubqueryEmptyWith", "SELECT a FROM (WITH c AS (SELECT a FROM t)^)", kSyntax, 1,
                   "expected SELECT, found ')'"},
        RejectCase{"ValuesInParentheses", "SELECT a FROM (values^)", kSyntax, 1,
                   "expected a join after the table in parentheses, found ')'"},
        RejectCase{"SubqueryColumnsWithoutAlias", "SELECT a FROM (SELECT a FROM t) ^(x)", kSyntax,
                   1, "unexpected '('; expected WHERE"},
        RejectCase{"SubqueryAsColumns", "SELECT a FROM (SELECT a FROM t) AS ^(x)", kSyntax, 1,
                   "expected a table alias after AS, found '('"},
        RejectCase{"SubqueryTwoColumnLists", "SELECT a FROM (SELECT a FROM t) s(x) ^(y)", kSyntax,
                   1, "unexpected '('; expected WHERE"},
        RejectCase{"SubqueryTwoAliases", "SELECT a FROM (SELECT a FROM t) s(x) ^s2", kSyntax, 2,
                   "unexpected identifier s2"},
        RejectCase{"SubqueryStringImplicitAlias", "SELECT a FROM (SELECT a FROM t) ^'s'", kSyntax,
                   3, "unexpected string literal"},
        RejectCase{"SubqueryAt", "SELECT a FROM (SELECT a FROM t) ^AT (VERSION => 1)", kSyntax, 2,
                   "a table alias cannot be the keyword AT; write it as a quoted identifier"},
        RejectCase{"SubqueryAliasAt", "SELECT a FROM (SELECT a FROM t) s ^at (VERSION => 1)",
                   kSyntax, 2, "unexpected identifier at"},
        RejectCase{"SubqueryAsSemi", "SELECT a FROM (SELECT a FROM t) AS ^semi", kSyntax, 4,
                   "a table alias cannot be the keyword SEMI"},
        RejectCase{"EmptyColumnAliasList", "SELECT a FROM (SELECT a FROM t) s(^)", kSyntax, 1,
                   "expected a column name in the column alias list, found ')'"},
        RejectCase{"CommaColumnAliasList", "SELECT a FROM (SELECT a FROM t) s(^,)", kSyntax, 1,
                   "expected a column name in the column alias list, found ','"},
        RejectCase{"ColumnAliasListTwoCommas", "SELECT a FROM (SELECT a FROM t) s(x,^,y)", kSyntax,
                   1, "expected a column name in the column alias list, found ','"},
        RejectCase{"ColumnAliasListWithoutComma", "SELECT a FROM (SELECT a FROM t) s(x ^y)",
                   kSyntax, 1, "expected , or ) in the column alias list, found identifier y"},
        RejectCase{"ColumnAliasListQualifiedName", "SELECT a FROM (SELECT a FROM t) s(x^.y)",
                   kSyntax, 1, "expected , or ) in the column alias list, found '.'"},
        RejectCase{"ColumnAliasListNumber", "SELECT a FROM (SELECT a FROM t) s(^1)", kSyntax, 1,
                   "expected a column name in the column alias list, found integer literal 1"},
        RejectCase{"ColumnAliasListReservedWord", "SELECT a FROM (SELECT a FROM t) s(^select)",
                   kSyntax, 6,
                   "expected a column name in the column alias list, found keyword SELECT"},
        RejectCase{"ColumnAliasListWordThatIsNoAlias",
                   "SELECT a FROM (SELECT a FROM t) s(x, ^semi)", kSyntax, 4,
                   "a column alias cannot be the keyword SEMI; write it as a quoted identifier"},
        RejectCase{"ColumnAliasListUnclosed", "SELECT a FROM (SELECT a FROM t) s(x^", kSyntax, 0,
                   "expected , or ) in the column alias list, found end of input"},
        RejectCase{"WithAlone", "WITH c AS (SELECT a FROM t)^", kSyntax, 0,
                   "expected SELECT, found end of input"},
        RejectCase{"WithSemicolon", "WITH c AS (SELECT a FROM t)^;", kSyntax, 1,
                   "expected SELECT, found ';'"},
        RejectCase{"WithOnly", "WITH^", kSyntax, 0, "expected a CTE name, found end of input"},
        RejectCase{"WithoutAs", "WITH c (^SELECT a FROM t) SELECT a FROM c", kSyntax, 6,
                   "expected a column name in the column alias list, found keyword SELECT"},
        RejectCase{"WithoutParenthesis", "WITH c AS ^SELECT a FROM t", kSyntax, 6,
                   "expected ( after AS in the WITH list, found keyword SELECT"},
        RejectCase{"WithNotWithoutMaterialized", "WITH c AS ^NOT (SELECT a FROM t) SELECT a FROM c",
                   kSyntax, 3, "expected ( after AS in the WITH list, found keyword NOT"},
        RejectCase{"WithUsingWithoutKey", "WITH c ^USING (SELECT a FROM t) SELECT a FROM c",
                   kSyntax, 5, "expected AS in the WITH list, found keyword USING"},
        RejectCase{"WithNameThenString", "WITH c ^'x' AS (SELECT a FROM t) SELECT a FROM c",
                   kSyntax, 3, "expected AS in the WITH list, found string literal"},
        RejectCase{"WithTrailingComma", "WITH c AS (SELECT a FROM t), ^SELECT a FROM c", kSyntax, 6,
                   "expected a CTE name, found keyword SELECT"},
        RejectCase{"WithAfterWith",
                   "WITH c AS (SELECT a FROM t) ^WITH d AS (SELECT a FROM t) SELECT a FROM c",
                   kSyntax, 4, "expected SELECT, found keyword WITH"},
        RejectCase{"WithEmptyColumnAliasList", "WITH c(^) AS (SELECT a FROM t) SELECT a FROM c",
                   kSyntax, 1, "expected a column name in the column alias list, found ')'"},
        RejectCase{"WithCommaColumnAliasList", "WITH c(^,) AS (SELECT a FROM t) SELECT a FROM c",
                   kSyntax, 1, "expected a column name in the column alias list, found ','"},
        RejectCase{"CteNameReservedWord", "WITH ^select AS (SELECT a FROM t) SELECT a FROM t",
                   kSyntax, 6, "expected a CTE name, found keyword SELECT"},
        RejectCase{"CteNameWordThatIsNoAlias", "WITH ^Semi AS (SELECT a FROM t) SELECT a FROM t",
                   kSyntax, 4,
                   "a CTE name cannot be the keyword SEMI; write it as a quoted identifier"},
        RejectCase{"CteNameParameter", "WITH ^$1 AS (SELECT a FROM t) SELECT a FROM t", kSyntax, 2,
                   "expected a CTE name, found"},
        RejectCase{"CteNameNumber", "WITH ^1 AS (SELECT a FROM t) SELECT a FROM t", kSyntax, 1,
                   "expected a CTE name, found integer literal 1"},
        RejectCase{"CteQualifiedName", "WITH s^.c AS (SELECT a FROM t) SELECT a FROM t", kSyntax, 1,
                   "expected AS in the WITH list, found '.'"},
        RejectCase{"CteDuplicateName",
                   "WITH c AS (SELECT a FROM t), ^C AS (SELECT a FROM t) SELECT a FROM c", kSyntax,
                   1, "duplicate CTE name in the WITH list (names match case-insensitively)"},
        RejectCase{"CteBodyUnclosed", "WITH c AS (SELECT a FROM t^", kSyntax, 0,
                   "unexpected end of input; expected WHERE"}),
    CaseName);

TEST(ParserTest, ExactMessages) {
  auto window_function = Parse("SELECT row_number() OVER () FROM events");
  ASSERT_FALSE(window_function.has_value());
  EXPECT_EQ(window_function.error().message,
            "window functions (OVER) are not supported; see docs/sql-subset.md");
  auto limit = Parse("SELECT a FROM events LIMIT 9223372036854775808");
  ASSERT_FALSE(limit.has_value());
  EXPECT_EQ(limit.error().message,
            "LIMIT 9223372036854775808 is out of range (the maximum is 9223372036854775807)");
  auto alias = Parse("SELECT a AS from FROM events");
  ASSERT_FALSE(alias.has_value());
  EXPECT_EQ(alias.error().message,
            "an alias cannot be the reserved word FROM; write it as a quoted identifier; see "
            "docs/sql-subset.md");
  auto semi = Parse("SELECT a FROM t semi JOIN u ON t.a = u.a");
  ASSERT_FALSE(semi.has_value());
  EXPECT_EQ(semi.error().message, "SEMI JOIN is not supported; see docs/sql-subset.md");
  auto keyword = Parse("SELECT a FROM t AS only");
  ASSERT_FALSE(keyword.has_value());
  EXPECT_EQ(keyword.error().message,
            "a table alias cannot be the keyword ONLY; write it as a quoted identifier");
  auto qualifier = Parse("SELECT interval.a FROM t interval");
  ASSERT_FALSE(qualifier.has_value());
  EXPECT_EQ(qualifier.error().message,
            "the reserved word INTERVAL as a qualifier is not supported; write it as a quoted "
            "identifier; see docs/sql-subset.md");
  auto cte = Parse("WITH only AS (SELECT a FROM t) SELECT a FROM t");
  ASSERT_FALSE(cte.has_value());
  EXPECT_EQ(cte.error().message,
            "a CTE name cannot be the keyword ONLY; write it as a quoted identifier");
  auto column = Parse("SELECT a FROM (SELECT a FROM t) s(x, To)");
  ASSERT_FALSE(column.has_value());
  EXPECT_EQ(column.error().message,
            "a column alias cannot be the keyword TO; write it as a quoted identifier");
  auto duplicate =
      Parse("WITH \"Name\" AS (SELECT a FROM t), nAME AS (SELECT a FROM t) SELECT a FROM t");
  ASSERT_FALSE(duplicate.has_value());
  EXPECT_EQ(duplicate.error().message,
            "duplicate CTE name in the WITH list (names match case-insensitively)");
  auto with = Parse("WITH c AS (SELECT a FROM t) SELECT a FROM (SELECT a FROM c)");
  ASSERT_TRUE(with.has_value()) << with.error().message;
}

// antb1's reserved words (as a name they must be quoted).
constexpr auto kReserved = std::to_array<std::string_view>(
    {"ALL",   "AND",       "ANY",      "ARRAY",  "AS",       "ASC",   "BETWEEN", "BY",     "CASE",
     "CAST",  "COLLATE",   "CROSS",    "DESC",   "DISTINCT", "ELSE",  "END",     "EXCEPT", "EXISTS",
     "FALSE", "FETCH",     "FOR",      "FROM",   "FULL",     "GROUP", "HAVING",  "ILIKE",  "IN",
     "INNER", "INTERSECT", "INTERVAL", "INTO",   "IS",       "JOIN",  "LATERAL", "LEFT",   "LIKE",
     "LIMIT", "NATURAL",   "NOT",      "NULL",   "OFFSET",   "ON",    "OR",      "ORDER",  "OUTER",
     "OVER",  "QUALIFY",   "RIGHT",    "SELECT", "SIMILAR",  "SOME",  "TABLE",   "THEN",   "TRUE",
     "UNION", "USING",     "WHEN",     "WHERE",  "WINDOW",   "WITH"});

TEST(ParserTest, EveryReservedWordIsRejectedAsAnAlias) {
  ASSERT_EQ(kReserved.size(), 60U);
  for (const std::string_view word : kReserved) {
    EXPECT_TRUE(IsReservedWord(word)) << word;
    const std::string sql = "SELECT a AS " + std::string(word) + " FROM t";
    auto result = Parse(sql);
    ASSERT_FALSE(result.has_value()) << sql;
    // PostgreSQL and DuckDB accept any keyword after AS: unsupported, not malformed.
    EXPECT_EQ(result.error().kind, ParseError::Kind::kUnsupported) << sql;
    EXPECT_EQ(result.error().span.offset, 12U) << sql;
    // Quoting makes every reserved word a valid name.
    EXPECT_TRUE(Parse(R"(SELECT a AS ")" + std::string(word) + R"(" FROM t)").has_value()) << word;
  }
}

// A table alias after AS: DuckDB refuses every reserved word but BETWEEN, EXISTS, INTERVAL and
// OVER (which it also takes without AS), so a syntax error; quoted, every word is an alias.
TEST(ParserTest, EveryReservedWordAfterAsInFrom) {
  const std::string prefix = "SELECT a FROM t AS ";
  for (const std::string_view word : kReserved) {
    const bool accepted =
        word == "BETWEEN" || word == "EXISTS" || word == "INTERVAL" || word == "OVER";
    auto result = Parse(prefix + std::string(word));
    if (accepted) {
      ASSERT_TRUE(result.has_value()) << word << ": " << result.error().message;
      EXPECT_EQ(result->from.at(0).alias, std::optional<std::string>(word));
      auto implicit = Parse("SELECT a FROM t " + std::string(word) + " WHERE a = 1");
      ASSERT_TRUE(implicit.has_value()) << word << ": " << implicit.error().message;
      EXPECT_EQ(implicit->from.at(0).alias, std::optional<std::string>(word));
    } else {
      ASSERT_FALSE(result.has_value()) << word;
      EXPECT_EQ(result.error().kind, ParseError::Kind::kSyntax) << word;
      EXPECT_EQ(result.error().span.offset, prefix.size()) << word;
    }
    auto quoted = Parse(prefix + "\"" + std::string(word) + "\"");
    ASSERT_TRUE(quoted.has_value()) << word;
    EXPECT_EQ(quoted->from.at(0).alias, std::optional<std::string>(word));
  }
}

// The reserved words that DuckDB 1.5.5 takes as function names (probed on DuckDB).
constexpr auto kFunctionWords =
    std::to_array<std::string_view>({"CROSS", "FULL", "ILIKE", "INNER", "IS", "JOIN", "LEFT",
                                     "LIKE", "NATURAL", "OUTER", "OVER", "RIGHT", "SIMILAR"});

// Where a FROM item starts, the reserved words of kFunctionWords call table functions before '(',
// unsupported; no other reserved word does. After LATERAL such a call is a LATERAL item, and first
// in parentheses it starts a join.
TEST(ParserTest, ReservedWordsThatNameTableFunctions) {
  for (const std::string_view word : kReserved) {
    const bool function = std::ranges::find(kFunctionWords, word) != kFunctionWords.end();
    for (const std::string_view before :
         {"SELECT a FROM "sv, "SELECT a FROM t, "sv, "SELECT a FROM t JOIN "sv,
          "SELECT a FROM t CROSS JOIN "sv}) {
      const std::string sql = std::string(before) + std::string(word) + "(1)";
      auto result = Parse(sql);
      ASSERT_FALSE(result.has_value()) << sql;
      const ParseError& error = result.error();
      EXPECT_EQ(error.message.starts_with("table functions are not supported"), function)
          << sql << ": " << error.message;
      if (function) {
        EXPECT_EQ(error.kind, ParseError::Kind::kUnsupported) << sql;
        EXPECT_EQ(error.span.offset, before.size()) << sql;
        EXPECT_EQ(error.span.length, word.size()) << sql;
      }
    }
    if (!function) {
      continue;
    }
    auto lateral = Parse("SELECT a FROM t, LATERAL " + std::string(word) + "(1)");
    ASSERT_FALSE(lateral.has_value()) << word;
    EXPECT_TRUE(lateral.error().message.starts_with("LATERAL is not supported"))
        << word << ": " << lateral.error().message;
    auto parenthesized = Parse("SELECT a FROM (" + std::string(word) + "(1) f CROSS JOIN t)");
    ASSERT_FALSE(parenthesized.has_value()) << word;
    EXPECT_TRUE(
        parenthesized.error().message.starts_with("parenthesized joins in FROM are not supported"))
        << word << ": " << parenthesized.error().message;
  }
}

// Where a FROM item starts, DuckDB 1.5.5 takes BETWEEN, EXISTS, INTERVAL and OVER, and no other
// reserved word, for a qualifier before a dot (probed on DuckDB): of a table (FROM t, over.x), of a
// table function (FROM over.f(1)) and after LATERAL. As qualifiers they are unsupported, as in
// expressions (divergence D21), and first in parentheses such a name starts a join. LATERAL first
// in parentheses starts one before a word of kFunctionWords too: three tokens do not show the dot.
TEST(ParserTest, ReservedWordsThatQualifyNamesInFrom) {
  constexpr std::string_view kParenthesizedJoin = "parenthesized joins in FROM are not supported";
  for (const std::string_view word : kReserved) {
    const bool qualifier =
        word == "BETWEEN" || word == "EXISTS" || word == "INTERVAL" || word == "OVER";
    const bool function = std::ranges::find(kFunctionWords, word) != kFunctionWords.end();
    const std::string message =
        "the reserved word " + std::string(word) + " as a qualifier is not supported";
    for (const std::string_view before :
         {"SELECT a FROM "sv, "SELECT a FROM t, "sv, "SELECT a FROM t JOIN "sv,
          "SELECT a FROM t CROSS JOIN "sv}) {
      for (const std::string_view after : {".x"sv, ".f(1)"sv}) {
        const std::string sql = std::string(before) + std::string(word) + std::string(after);
        auto result = Parse(sql);
        ASSERT_FALSE(result.has_value()) << sql;
        const ParseError& error = result.error();
        EXPECT_EQ(error.message.starts_with(message), qualifier) << sql << ": " << error.message;
        if (qualifier) {
          EXPECT_EQ(error.kind, ParseError::Kind::kUnsupported) << sql;
          EXPECT_EQ(error.span.offset, before.size()) << sql;
          EXPECT_EQ(error.span.length, word.size()) << sql;
        }
      }
    }
    auto lateral = Parse("SELECT a FROM t, LATERAL " + std::string(word) + ".f(1)");
    ASSERT_FALSE(lateral.has_value()) << word;
    EXPECT_EQ(lateral.error().message.starts_with("LATERAL is not supported"), qualifier)
        << word << ": " << lateral.error().message;
    auto parenthesized = Parse("SELECT a FROM (" + std::string(word) + ".x CROSS JOIN t)");
    ASSERT_FALSE(parenthesized.has_value()) << word;
    EXPECT_EQ(parenthesized.error().message.starts_with(kParenthesizedJoin), qualifier)
        << word << ": " << parenthesized.error().message;
    auto parenthesized_lateral =
        Parse("SELECT a FROM (LATERAL " + std::string(word) + ".f(1) CROSS JOIN t)");
    ASSERT_FALSE(parenthesized_lateral.has_value()) << word;
    EXPECT_EQ(parenthesized_lateral.error().message.starts_with(kParenthesizedJoin),
              qualifier || function)
        << word << ": " << parenthesized_lateral.error().message;
  }
}

// The words that DuckDB 1.5.5 refuses as table aliases and antb1 does not reserve (an independent
// copy of the parser's table), with what follows each where DuckDB gives it a meaning (probed on
// DuckDB): after a FROM item, and after an ON condition.
struct NotAnAliasCase {
  std::string_view word;
  std::array<std::string_view, 2> after_item;  // continuations with a meaning ("" none)
  bool means_after_on = false;
  std::string_view after_on;  // the continuation with a meaning after an ON condition
};

constexpr auto kNotAnAliasCases = std::to_array<NotAnAliasCase>({
    {.word = "analyse"},
    {.word = "ANALYZE"},
    {.word = "anti",
     .after_item = {"JOIN u ON t.a = u.a"},
     .means_after_on = true,
     .after_on = "JOIN v ON t.a = v.a"},
    {.word = "ASOF",
     .after_item = {"JOIN u ON t.a >= u.a", "SEMI JOIN u ON t.a >= u.a"},
     .means_after_on = true,
     .after_on = "LEFT OUTER JOIN v ON t.a >= v.a"},
    {.word = "asymmetric"},
    {.word = "at",
     .after_item = {"(VERSION => 1)"},
     .means_after_on = true,
     .after_on = "TIME ZONE 'UTC'"},
    {.word = "AUTHORIZATION"},
    {.word = "binary"},
    {.word = "both"},
    {.word = "CHECK"},
    {.word = "collation"},
    {.word = "column"},
    {.word = "concurrently"},
    {.word = "constraint"},
    {.word = "create"},
    {.word = "default"},
    {.word = "deferrable"},
    {.word = "describe"},
    {.word = "do"},
    {.word = "foreign"},
    {.word = "freeze"},
    {.word = "GLOB", .means_after_on = true, .after_on = "'x*'"},
    {.word = "initially"},
    {.word = "isnull", .means_after_on = true},
    {.word = "lambda"},
    {.word = "leading"},
    {.word = "NOTNULL", .means_after_on = true},
    {.word = "only"},
    {.word = "overlaps"},
    {.word = "pivot",
     .after_item = {"(SUM(a) FOR b IN (1, 2))"},
     .means_after_on = true,
     .after_on = "(SUM(a) FOR b IN (1))"},
    {.word = "pivot_longer"},
    {.word = "pivot_wider"},
    {.word = "placing"},
    {.word = "POSITIONAL", .after_item = {"JOIN u"}, .means_after_on = true, .after_on = "JOIN v"},
    {.word = "primary"},
    {.word = "references"},
    {.word = "returning"},
    {.word = "semi",
     .after_item = {"JOIN u ON t.a = u.a"},
     .means_after_on = true,
     .after_on = "JOIN v ON t.a = v.a"},
    {.word = "show"},
    {.word = "summarize"},
    {.word = "symmetric"},
    {.word = "TABLESAMPLE", .after_item = {"10%", "reservoir(10)"}},
    {.word = "to"},
    {.word = "trailing"},
    {.word = "unique"},
    {.word = "unpack"},
    {.word = "UNPIVOT",
     .after_item = {"(v FOR k IN (a, b))", "INCLUDE NULLS (v FOR k IN (a, b))"},
     .means_after_on = true,
     .after_on = "(v FOR k IN (a, b))"},
    {.word = "variadic"},
    {.word = "verbose"},
});

// The kind and offset of the error that `sql` gives (kSyntax at 0 when it parses).
std::pair<ParseError::Kind, std::size_t> ErrorOf(const std::string& sql) {
  auto result = Parse(sql);
  EXPECT_FALSE(result.has_value()) << sql;
  return result ? std::pair{ParseError::Kind::kSyntax, std::size_t{0}}
                : std::pair{result.error().kind, result.error().span.offset};
}

TEST(ParserTest, WordsThatCannotBeImplicitAliases) {
  ASSERT_EQ(kNotAnAliasCases.size(), 49U);
  const auto unsupported_at = [](std::size_t offset) {
    return std::pair{ParseError::Kind::kUnsupported, offset};
  };
  const auto syntax_at = [](std::size_t offset) {
    return std::pair{ParseError::Kind::kSyntax, offset};
  };
  for (const NotAnAliasCase& c : kNotAnAliasCases) {
    const std::string w(c.word);
    SCOPED_TRACE(w);
    // After a table, a path or an alias: never an alias.
    EXPECT_EQ(ErrorOf("SELECT a FROM t " + w), syntax_at(16));
    EXPECT_EQ(ErrorOf("SELECT a FROM 'p.parquet' " + w + " WHERE a = 1"), syntax_at(26));
    EXPECT_EQ(ErrorOf("SELECT a FROM t, u " + w + " ORDER BY a"), syntax_at(19));
    EXPECT_EQ(ErrorOf("SELECT a FROM t JOIN u " + w + " ON t.a = u.a"), syntax_at(23));
    EXPECT_EQ(ErrorOf("SELECT a FROM t AS " + w), syntax_at(19));
    EXPECT_EQ(ErrorOf("SELECT a FROM t AS a " + w), syntax_at(21));
    // After a derived table, its alias or its column alias list, never one either; in a column
    // alias list and as a CTE's name neither (as in DuckDB).
    const std::string derived = "SELECT a FROM (SELECT a FROM t)";
    EXPECT_EQ(ErrorOf(derived + " " + w), syntax_at(32));
    EXPECT_EQ(ErrorOf(derived + " AS " + w + " WHERE a = 1"), syntax_at(35));
    EXPECT_EQ(ErrorOf(derived + " s(x) " + w), syntax_at(37));
    EXPECT_EQ(ErrorOf(derived + " s(x, " + w + ")"), syntax_at(37));
    EXPECT_EQ(ErrorOf("WITH " + w + " AS (SELECT a FROM t) SELECT a FROM t"), syntax_at(5));
    EXPECT_EQ(ErrorOf("WITH c(" + w + ") AS (SELECT a FROM t) SELECT a FROM t"), syntax_at(7));
    // Where DuckDB gives the word a meaning: kUnsupported, after a derived table too, but for time
    // travel (AT), which only a table or a path has.
    for (const std::string_view continuation : c.after_item) {
      if (continuation.empty()) {
        continue;
      }
      const std::string after = " " + w + " " + std::string(continuation);
      EXPECT_EQ(ErrorOf("SELECT a FROM t" + after), unsupported_at(16)) << continuation;
      EXPECT_EQ(ErrorOf("SELECT a FROM 'p.parquet'" + after), unsupported_at(26)) << continuation;
      EXPECT_EQ(ErrorOf("SELECT a FROM t AS a" + after), unsupported_at(21)) << continuation;
      EXPECT_EQ(ErrorOf("SELECT a FROM t \"a b\"" + after), unsupported_at(22)) << continuation;
      const bool tables_only = w == "at";
      EXPECT_EQ(ErrorOf(derived + after), tables_only ? syntax_at(32) : unsupported_at(32))
          << continuation;
      EXPECT_EQ(ErrorOf(derived + " AS s(x)" + after),
                tables_only ? syntax_at(40) : unsupported_at(40))
          << continuation;
    }
    // After an ON condition: the meanings DuckDB has there, else a syntax error.
    const std::string on = "SELECT a FROM t JOIN u ON t.a = u.a ";
    const std::string after_on =
        c.means_after_on ? std::string(c.after_on) : std::string(c.after_item.front());
    EXPECT_EQ(ErrorOf(on + w + (after_on.empty() ? "" : " " + after_on)),
              c.means_after_on ? unsupported_at(on.size()) : syntax_at(on.size()))
        << after_on;
    // Quoted, it is an alias like any name, and a column alias and a CTE name too.
    for (const std::string& quoted :
         {"SELECT a FROM t \"" + w + "\"", "SELECT a FROM t AS \"" + w + "\"",
          derived + " \"" + w + "\"(\"" + w + "\")"}) {
      auto stmt = Parse(quoted);
      ASSERT_TRUE(stmt.has_value()) << quoted << ": " << stmt.error().message;
      EXPECT_EQ(stmt->from.at(0).alias, std::optional<std::string>(w));
    }
    const std::string quoted_cte =
        "WITH \"" + w + "\"(\"" + w + "\") AS (SELECT a FROM t) SELECT a FROM t";
    auto cte = Parse(quoted_cte);
    ASSERT_TRUE(cte.has_value()) << quoted_cte << ": " << cte.error().message;
    EXPECT_EQ(cte->with.at(0).name, w);
    EXPECT_EQ(cte->with.at(0).columns, std::vector<std::string>{w});
    // Anywhere else an unquoted name (divergence D21): a column, a qualifier, a table, a function
    // and a select alias after AS.
    const std::string names =
        std::format("SELECT {0}, {0}.{0}, {0}(a), a AS {0} FROM {0} WHERE {0} = 1", w);
    auto stmt = Parse(names);
    ASSERT_TRUE(stmt.has_value()) << names << ": " << stmt.error().message;
    EXPECT_EQ(TableOf(stmt->from.at(0)).name, w);
    EXPECT_FALSE(stmt->from.at(0).alias.has_value());
    EXPECT_EQ(std::get<ColumnRef>(stmt->items.at(1).expr).qualifier, w);
    EXPECT_FALSE(IsReservedWord(w));
  }
}

// ---- robustness ------------------------------------------------------------------------------

void ExpectWellFormedError(const std::string& sql) {
  auto result = Parse(sql);
  ASSERT_FALSE(result.has_value());
  EXPECT_FALSE(result.error().message.empty());
  EXPECT_LE(result.error().span.offset, sql.size());
  EXPECT_LE(result.error().span.length, sql.size() - result.error().span.offset);
}

TEST(ParserRobustnessTest, MegabyteOfParentheses) {
  constexpr std::size_t kSize = std::size_t{1} << 20U;
  const std::string parens(kSize, '(');
  for (const std::string& prefix :
       {std::string(), std::string("SELECT "), std::string("SELECT a FROM t WHERE "),
        std::string("SELECT a FROM t WHERE a = "), std::string("SELECT SUM("),
        std::string("SELECT COUNT(*) FROM "), std::string("SELECT a FROM t LIMIT "),
        std::string("SELECT a FROM t JOIN u ON "), std::string("SELECT a FROM t, u CROSS JOIN ")}) {
    const std::string sql = prefix + parens;
    auto result = Parse(sql);
    ASSERT_FALSE(result.has_value()) << prefix;
    EXPECT_EQ(result.error().kind, ParseError::Kind::kUnsupported) << prefix;
    // In an expression (ON too) the parentheses nest up to the depth limit; a query and LIMIT take
    // none, and in FROM the first opens a derived table, whose query a '(' cannot start.
    const bool expression = !prefix.empty() && !prefix.ends_with("FROM ") &&
                            !prefix.ends_with("JOIN ") && !prefix.ends_with("LIMIT ");
    const bool from_item = prefix.ends_with("FROM ") || prefix.ends_with("JOIN ");
    const SourceSpan span = result.error().span;
    EXPECT_EQ(span.length, 1U) << prefix;
    if (expression) {
      EXPECT_GT(span.offset, prefix.size()) << prefix;
      EXPECT_LE(span.offset, prefix.size() + 256) << prefix;
      EXPECT_TRUE(result.error().message.starts_with("expressions deeper than 256 levels"))
          << result.error().message;
    } else if (from_item) {
      EXPECT_EQ(span.offset, prefix.size() + 1) << prefix;
      EXPECT_TRUE(result.error().message.starts_with("parenthesized queries are not supported"))
          << result.error().message;
    } else {
      EXPECT_EQ(span.offset, prefix.size()) << prefix;
    }
  }
  ExpectWellFormedError(std::string(kSize, ')'));
  ExpectWellFormedError("SELECT a FROM t WHERE a = 1" + std::string(kSize, ')'));
}

// The offset of the n-th '(' (1-based) of `sql`.
std::size_t NthOpenParenthesis(std::string_view sql, std::size_t n) {
  std::size_t offset = std::string_view::npos;
  for (std::size_t i = 0; i < n; ++i) {
    offset = sql.find('(', offset + 1);
  }
  return offset;
}

// A megabyte of nested queries stops at the depth limit: the parser recurses through 256 levels of
// queries at most (the sanitizer builds run this too), and reports the '(' that opens the 257th.
TEST(ParserRobustnessTest, MegabyteOfNestedBlocks) {
  constexpr std::size_t kSize = std::size_t{1} << 20U;
  for (const std::string_view unit :
       {"SELECT * FROM ("sv, "WITH c AS ("sv, "SELECT * FROM t JOIN ("sv,
        "SELECT * FROM (WITH c AS ("sv, "SELECT * FROM (SELECT * FROM t) s, ("sv}) {
    std::string sql;
    while (sql.size() < kSize) {
      sql += unit;
    }
    auto result = Parse(sql);
    ASSERT_FALSE(result.has_value()) << unit;
    EXPECT_EQ(result.error().kind, ParseError::Kind::kUnsupported) << unit;
    EXPECT_TRUE(result.error().message.starts_with("expressions deeper than 256 levels"))
        << unit << ": " << result.error().message;
    // Every '(' of the unit but the one of a nested derived table opens a level.
    const std::size_t per_unit = static_cast<std::size_t>(std::ranges::count(unit, '('));
    const std::size_t levels_per_unit = unit.ends_with("s, (") ? 1 : per_unit;
    const std::size_t n = ((256 / levels_per_unit) * per_unit) + 1;
    EXPECT_EQ(result.error().span, (SourceSpan{.offset = NthOpenParenthesis(sql, n), .length = 1}))
        << unit;
  }
  // A select item is one level below its query: in the 256th derived table, a column is too deep.
  std::string items;
  while (items.size() < kSize) {
    items += "SELECT a FROM t JOIN (";
  }
  auto item = Parse(items);
  ASSERT_FALSE(item.has_value());
  EXPECT_EQ(item.error().span, (SourceSpan{.offset = (256 * 22) + 7, .length = 1}));
  // Calls in the select list of nested derived tables: the levels of both count together.
  const std::string blocks = Repeat("SELECT * FROM (", 200) + "SELECT ";
  std::string sql = blocks;
  while (sql.size() < kSize) {
    sql += "f(";
  }
  auto calls = Parse(sql);
  ASSERT_FALSE(calls.has_value());
  EXPECT_TRUE(calls.error().message.starts_with("expressions deeper than 256 levels"))
      << calls.error().message;
  EXPECT_EQ(calls.error().span, (SourceSpan{.offset = blocks.size() + (56 * 2), .length = 1}));
}

// The deepest statement of each family of nested queries survives every walk of the AST: copy,
// comparison, printing, Depth and destruction.
TEST(ParserRobustnessTest, DeepestBlocksSurviveEveryWalk) {
  const std::string derived =
      "SELECT * FROM " + Repeat("(SELECT * FROM ", 256) + "t" + std::string(256, ')');
  const std::string ctes =
      Repeat("WITH c AS (", 255) + "SELECT a FROM t" + Repeat(") SELECT a FROM c", 255);
  std::string mixed = "SELECT a FROM t";
  for (std::size_t i = 1; i <= 255; ++i) {
    mixed = i % 2 == 1 ? "SELECT * FROM (" + mixed + ") AS \"d\"(x)"
                       : "WITH c(x) AS (" + mixed + ") SELECT x FROM c";
  }
  const std::string calls = "SELECT * FROM " + Repeat("(SELECT * FROM ", 127) + "(SELECT " +
                            Repeat("f(", 127) + "a" + std::string(127, ')') + " FROM t)" +
                            std::string(127, ')');
  for (const std::string& sql : {derived, ctes, mixed, calls}) {
    auto stmt = Parse(sql);
    ASSERT_TRUE(stmt.has_value()) << stmt.error().message;
    EXPECT_EQ(Depth(*stmt), kMaxExpressionDepth);
    SelectStatement copy = *stmt;
    EXPECT_TRUE(EqualIgnoringSpans(*stmt, copy));
    // The innermost query's table, renamed in the copy, makes it differ.
    SelectStatement* innermost = &copy;
    while (true) {
      if (!innermost->with.empty()) {
        innermost = &*innermost->with.front().query;
      } else if (auto* nested = std::get_if<DerivedTable>(&innermost->from.at(0).source)) {
        innermost = &*nested->query;
      } else {
        break;
      }
    }
    std::get<TableRef>(innermost->from.at(0).source).name += '_';
    EXPECT_FALSE(EqualIgnoringSpans(*stmt, copy));
    const std::string canonical = ToSql(*stmt);
    auto again = Parse(canonical);
    ASSERT_TRUE(again.has_value()) << again.error().message;
    EXPECT_TRUE(EqualIgnoringSpans(*stmt, *again));
    EXPECT_EQ(ToSql(*again), canonical);
    EXPECT_EQ(Depth(*again), kMaxExpressionDepth);
  }
}

// WITH lists and column alias lists are walked in loops everywhere, so long ones cost no stack.
TEST(ParserRobustnessTest, LongWithListsAndColumnLists) {
  std::string with = "WITH c0 AS (SELECT a FROM t)";
  for (int i = 1; i < 5000; ++i) {
    with += ", c" + std::to_string(i) + "(x) AS (SELECT a FROM c" + std::to_string(i - 1) + ")";
  }
  with += " SELECT x FROM c4999";
  std::string columns = "SELECT 1 FROM (SELECT a FROM t) AS s(x0";
  for (int i = 1; i < 50000; ++i) {
    columns += ", x" + std::to_string(i);
  }
  columns += ")";
  for (const std::string& sql : {with, columns}) {
    auto stmt = Parse(sql);
    ASSERT_TRUE(stmt.has_value()) << stmt.error().message;
    EXPECT_EQ(Depth(*stmt), 2U);
    SelectStatement copy = *stmt;
    EXPECT_TRUE(EqualIgnoringSpans(*stmt, copy));
    if (copy.with.empty()) {
      copy.from.at(0).columns.back() += '_';
    } else {
      copy.with.back().columns.back() += '_';
    }
    EXPECT_FALSE(EqualIgnoringSpans(*stmt, copy));
    const std::string canonical = ToSql(*stmt);
    auto again = Parse(canonical);
    ASSERT_TRUE(again.has_value()) << again.error().message;
    EXPECT_TRUE(EqualIgnoringSpans(*stmt, *again));
    EXPECT_EQ(ToSql(*again), canonical);
  }
  auto counted = Parse(with);
  ASSERT_TRUE(counted.has_value());
  EXPECT_EQ(counted->with.size(), 5000U);
  EXPECT_EQ(counted->with.back().name, "c4999");
  auto listed = Parse(columns);
  ASSERT_TRUE(listed.has_value());
  EXPECT_EQ(listed->from.at(0).columns.size(), 50000U);
}

TEST(ParserRobustnessTest, MegabyteInputs) {
  constexpr std::size_t kSize = std::size_t{1} << 20U;
  ExpectWellFormedError(std::string(kSize, '-'));  // one long comment: empty query
  ExpectWellFormedError(std::string(kSize, '/'));
  ExpectWellFormedError(std::string(kSize, '*'));
  ExpectWellFormedError(std::string(kSize, '\''));
  ExpectWellFormedError(std::string(kSize, '"'));
  ExpectWellFormedError(std::string(kSize, '\0'));
  ExpectWellFormedError(std::string(kSize, '\xff'));
  ExpectWellFormedError("/*" + std::string(kSize, 'x'));
  std::string nots = "SELECT a FROM t WHERE ";
  for (std::size_t i = 0; i < kSize / 4; ++i) {
    nots += "NOT ";
  }
  ExpectWellFormedError(nots);
  std::string casts = "SELECT a";
  std::string calls = "SELECT ";
  for (std::size_t i = 0; i < kSize / 5; ++i) {
    casts += "::a";
    calls += "CAST(";
  }
  ExpectWellFormedError(casts);
  ExpectWellFormedError(calls);

  // Large but valid inputs parse in linear time.
  const std::string long_name(kSize, 'n');
  auto name = Parse("SELECT " + long_name + " FROM " + long_name);
  ASSERT_TRUE(name.has_value());
  EXPECT_EQ(TableOf(name->from.at(0)).name.size(), kSize);
  auto path = Parse("SELECT * FROM '" + std::string(kSize, 'p') + "'");
  ASSERT_TRUE(path.has_value());
  EXPECT_EQ(TableOf(path->from.at(0)).name.size(), kSize);

  std::string items = "SELECT a0";
  std::string predicate = " WHERE a = 1";
  for (int i = 1; i < 5000; ++i) {
    items += ", a" + std::to_string(i);
    predicate += " AND a" + std::to_string(i) + " <> " + std::to_string(i);
  }
  auto wide = Parse(items + " FROM t" + predicate);
  ASSERT_TRUE(wide.has_value()) << wide.error().message;
  EXPECT_EQ(wide->items.size(), 5000U);
  EXPECT_EQ(wide->where.size(), 5000U);
}

// A FROM list is walked in loops everywhere (parse, copy, compare, print, Depth, destruction), so
// long lists and join chains cost no stack. No timing is asserted.
TEST(ParserRobustnessTest, LongFromListsAndJoinChains) {
  std::string commas = "SELECT a FROM t0";
  for (int i = 1; i < 50000; ++i) {
    commas += ", t" + std::to_string(i);
  }
  std::string joins = "SELECT a FROM t";
  for (int i = 0; i < 20000; ++i) {
    joins += i % 2 == 0 ? " JOIN t ON a = b" : " LEFT JOIN u x ON x.a = b AND c";
  }
  for (const std::string& sql : {commas, joins}) {
    auto stmt = Parse(sql);
    ASSERT_TRUE(stmt.has_value()) << stmt.error().message;
    SelectStatement copy = *stmt;
    copy.from.back().span = {};
    EXPECT_TRUE(EqualIgnoringSpans(*stmt, copy));
    std::get<TableRef>(copy.from.back().source).name += '_';
    EXPECT_FALSE(EqualIgnoringSpans(*stmt, copy));
    EXPECT_LE(Depth(*stmt), 3U);
    const std::string canonical = ToSql(*stmt);
    auto again = Parse(canonical);
    ASSERT_TRUE(again.has_value()) << again.error().message;
    EXPECT_TRUE(EqualIgnoringSpans(*stmt, *again));
    EXPECT_EQ(ToSql(*again), canonical);
  }
  auto counted = Parse(commas);
  ASSERT_TRUE(counted.has_value());
  EXPECT_EQ(counted->from.size(), 50000U);
  EXPECT_EQ(TableOf(counted->from.back()).name, "t49999");
  auto chain = Parse(joins);
  ASSERT_TRUE(chain.has_value());
  EXPECT_EQ(chain->from.size(), 20001U);
  EXPECT_EQ(chain->from.back().on.size(), 2U);
}

TEST(ParserRobustnessTest, EmbeddedNulAndInvalidUtf8) {
  auto string_ok = Parse("SELECT a FROM t WHERE s = 'x\0y'"sv);
  ASSERT_TRUE(string_ok.has_value()) << string_ok.error().message;
  ASSERT_EQ(string_ok->where.size(), 1U);
  EXPECT_EQ(Cmp(string_ok->where[0]).literal.text, "x\0y"sv);

  auto ident_ok = Parse("SELECT \"\xff\xfe\0\" FROM '\xc3\x28.parquet'"sv);
  ASSERT_TRUE(ident_ok.has_value()) << ident_ok.error().message;
  ASSERT_EQ(ident_ok->items.size(), 1U);
  const ColumnRef* column = ColumnOf(ident_ok->items[0]);
  ASSERT_NE(column, nullptr);
  EXPECT_EQ(column->name, "\xff\xfe\0"sv);
  EXPECT_EQ(TableOf(ident_ok->from.at(0)).name, "\xc3\x28.parquet");

  auto nul = Parse("SELECT a\0 FROM t"sv);
  ASSERT_FALSE(nul.has_value());
  EXPECT_EQ(nul.error().span, (SourceSpan{.offset = 8, .length = 1}));
  EXPECT_EQ(nul.error().message, "unexpected byte 0x00");

  auto invalid = Parse("SELECT a FROM t WHERE a = 1 \xc3\x28");
  ASSERT_FALSE(invalid.has_value());
  EXPECT_EQ(invalid.error().span.offset, 28U);
}

TEST(ParserRobustnessTest, EveryByteAloneAndAfterAValidQuery) {
  for (int byte = 0; byte < 256; ++byte) {
    const char c = static_cast<char>(byte);
    const std::string alone(1, c);
    ExpectWellFormedError(alone);
    const std::string after = "SELECT a FROM t WHERE b = 1 LIMIT 2" + alone;
    auto result = Parse(after);
    if (!result) {
      EXPECT_LE(result.error().span.offset + result.error().span.length, after.size()) << byte;
    }
  }
}

}  // namespace
}  // namespace antb1::sql
