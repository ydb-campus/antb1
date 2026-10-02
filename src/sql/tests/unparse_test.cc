#include "antb1/sql/unparse.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "antb1/sql/ast.h"
#include "antb1/sql/parser.h"

namespace antb1::sql {
namespace {

using namespace std::string_view_literals;

// Parse -> ToSql -> Parse must give the same AST, and ToSql must be a fixed point.
void ExpectRoundTrip(std::string_view sql) {
  auto stmt = Parse(sql);
  ASSERT_TRUE(stmt.has_value()) << sql << ": " << stmt.error().message;
  const std::string canonical = ToSql(*stmt);
  auto again = Parse(canonical);
  ASSERT_TRUE(again.has_value()) << canonical << ": " << again.error().message;
  EXPECT_TRUE(EqualIgnoringSpans(*stmt, *again)) << sql << " -> " << canonical;
  EXPECT_EQ(ToSql(*again), canonical) << sql;
}

std::string Canonical(std::string_view sql) {
  auto stmt = Parse(sql);
  EXPECT_TRUE(stmt.has_value()) << sql << ": " << (stmt ? "" : stmt.error().message);
  return stmt ? ToSql(*stmt) : std::string();
}

TEST(UnparseTest, RoundTripsCountStar) {
  for (const char* sql : {"select count(*) from events", R"(SELECT COUNT(*) FROM "Events";)",
                          "SELECT COUNT(*) FROM 'a''b.parquet'"}) {
    ExpectRoundTrip(sql);
  }
}

TEST(UnparseTest, CanonicalForms) {
  struct Case {
    std::string_view input;
    std::string_view canonical;
  };
  for (const Case& c : {
           Case{.input = "select count(*) from events", .canonical = "SELECT COUNT(*) FROM events"},
           Case{.input = "SELECT  *  FROM 'data/part-0.parquet' ;",
                .canonical = "SELECT * FROM 'data/part-0.parquet'"},
           Case{.input = "select count ( user_id ) , sum(amount)total from Events",
                .canonical = R"(SELECT COUNT(user_id), SUM(amount) AS "total" FROM Events)"},
           Case{.input = R"(SELECT a AS x, "B" "y" FROM t)",
                .canonical = R"(SELECT a AS "x", "B" AS "y" FROM t)"},
           Case{.input =
                    "select a from t where a in ( 1,-2 , 'x' ) and b not in (date '2024-01-02')",
                .canonical =
                    "SELECT a FROM t WHERE a IN (1, -2, 'x') AND b NOT IN (DATE '2024-01-02')"},
           Case{
               .input = "select 1, -2 m, 'x' , date '2024-01-02' from t group by 1, 'k' order by 2",
               .canonical = "SELECT 1, -2 AS \"m\", 'x', DATE '2024-01-02' FROM t GROUP BY 1, 'k' "
                            "ORDER BY 2"},
           Case{.input = "select a from t where a like '%x''%' and b not like '_'",
                .canonical = "SELECT a FROM t WHERE a LIKE '%x''%' AND b NOT LIKE '_'"},
           Case{.input = "SELECT a FROM t WHERE 5 < a AND b != 'it''s' AND DATE '2024-01-31' >= d",
                .canonical =
                    "SELECT a FROM t WHERE 5 < a AND b <> 'it''s' AND DATE '2024-01-31' >= d"},
           Case{.input = "SELECT a FROM t WHERE a = - 1.50 and b >= .5 and c < 1e3 and d > 5.",
                .canonical = "SELECT a FROM t WHERE a = -1.50 AND b >= .5 AND c < 1e3 AND d > 5."},
           Case{.input = "SELECT a FROM t WHERE a = 007 LIMIT 0000010",
                .canonical = "SELECT a FROM t WHERE a = 007 LIMIT 10"},
           Case{.input = R"(SELECT "a""b" FROM "t""u" WHERE "c" = '''')",
                .canonical = R"(SELECT "a""b" FROM "t""u" WHERE "c" = '''')"},
           Case{.input = "/* hi */ SELECT a -- x\n FROM t -- y", .canonical = "SELECT a FROM t"},
           Case{.input = "SELECT MIN(ts), MAX(ts), AVG(x) FROM t WHERE ts > date '2020-01-01'",
                .canonical = "SELECT MIN(ts), MAX(ts), AVG(x) FROM t WHERE ts > DATE '2020-01-01'"},
           Case{.input = "SELECT timestamp '2020-01-01 10:00' FROM t WHERE ts < Timestamp 'x'",
                .canonical = "SELECT TIMESTAMP '2020-01-01 10:00' FROM t WHERE ts < TIMESTAMP 'x'"},
           Case{.input = "select a, count(*) c from t group by a order by c desc, a asc limit 5",
                .canonical = R"(SELECT a, COUNT(*) AS "c" FROM t GROUP BY a ORDER BY c DESC, a )"
                             "LIMIT 5"},
           Case{.input = "SELECT a FROM t ORDER BY a nulls first OFFSET 2 LIMIT 3",
                .canonical = "SELECT a FROM t ORDER BY a NULLS FIRST LIMIT 3 OFFSET 2"},
           Case{.input = "select a from t group by a having 1 < count(*) and a not in (2, 3) and "
                         "min(s) like 'x%' order by a",
                .canonical = "SELECT a FROM t GROUP BY a HAVING 1 < COUNT(*) AND a NOT IN (2, 3) "
                             "AND MIN(s) LIKE 'x%' ORDER BY a"},
           Case{.input = "select count( distinct a ) from t order by count(distinct a) desc nulls "
                         "last",
                .canonical = "SELECT COUNT(DISTINCT a) FROM t ORDER BY COUNT(DISTINCT a) DESC "
                             "NULLS LAST"},
           // Both spellings of a cast print as CAST; a minus before a cast is not part of the
           // number.
           Case{.input = "select a :: int, -1::integer, (-1)::integer, - 1.5::decimal(4,1) from t",
                .canonical = "SELECT CAST(a AS INT), -(CAST(1 AS INTEGER)), CAST(-1 AS INTEGER), "
                             "-(CAST(1.5 AS DECIMAL(4, 1))) FROM t"},
           Case{.input = "select cast ( d as date ) , try_cast(a as Decimal( 15 , 2 )) x from t "
                         "where d >= '2024-01-31'::Date and d < cast('2024-02-01' as date)",
                .canonical =
                    "SELECT CAST(d AS DATE), TRY_CAST(a AS DECIMAL(15, 2)) AS \"x\" FROM t "
                    "WHERE d >= CAST('2024-01-31' AS DATE) AND d < "
                    "CAST('2024-02-01' AS DATE)"},
           Case{.input =
                    "select a::varchar::date, (a + 1)::bigint, sum(x)::double, not b::boolean, "
                    "cast(a = 1 or b as boolean), try_cast, try_cast::int from t",
                .canonical = "SELECT CAST(CAST(a AS VARCHAR) AS DATE), CAST(a + 1 AS BIGINT), "
                             "CAST(SUM(x) AS DOUBLE), NOT CAST(b AS BOOLEAN), "
                             "CAST(a = 1 OR b AS BOOLEAN), try_cast, CAST(try_cast AS INT) FROM t"},
       }) {
    EXPECT_EQ(Canonical(c.input), c.canonical) << c.input;
    ExpectRoundTrip(c.input);
  }
}

TEST(UnparseTest, RoundTripsCorpus) {
  for (const std::string_view sql : {
           "SELECT * FROM events"sv,
           "SELECT user_id, region AS r FROM events WHERE amount >= 100 LIMIT 5"sv,
           "SELECT COUNT(*) AS n, SUM(amount), AVG(amount), MIN(ts), MAX(ts) FROM events"sv,
           "SELECT COUNT(DISTINCT_ish) FROM events WHERE region <> '' AND 0 < amount"sv,
           R"(SELECT "Mixed Case" FROM "Quoted Table" WHERE "Mixed Case" = 'x')"sv,
           "SELECT a FROM 'dir with space/it''s.parquet' WHERE b = -0"sv,
           "SELECT a FROM t WHERE b = 'multi\nline' AND c = '-- not a comment'"sv,
           "SELECT a FROM t WHERE b = '\xff\xfe' AND \"\xd0\xb8\" = 1"sv,
           "SELECT a FROM t WHERE b = 'nul\0inside'"sv,
           "SELECT a FROM t LIMIT 9223372036854775807"sv,
           "SELECT count, sum, date FROM date WHERE date = DATE '2024-01-01'"sv,
           R"(SELECT a AS """" FROM t)"sv,
           "SELECT a, b, COUNT(*), SUM(x) FROM t WHERE c = 1 GROUP BY a, b ORDER BY COUNT(*) DESC, "
           "a LIMIT 10 OFFSET 20"sv,
           R"(SELECT "g", COUNT(DISTINCT "u") FROM t GROUP BY "g" ORDER BY "g" NULLS FIRST)"sv,
           "SELECT a FROM t OFFSET 5"sv,
           "SELECT COUNT(*) FROM t HAVING SUM(x) >= -1.5 AND COUNT(DISTINCT y) <> 0"sv,
           "SELECT CAST(a AS INT), a::BIGINT, TRY_CAST(a AS DECIMAL(15, 2)) FROM t WHERE d >= "
           "'2024-01-31'::DATE"sv,
           "SELECT -a::INT, -(1)::INT, (-1)::INT, -1::INT, - -a::INT, -(a)::INT FROM t"sv,
           "SELECT a FROM t WHERE CAST('2020-01-02' AS DATE) < d AND d IN ('2020-01-02'::DATE, "
           "CAST('2024-01-31' AS DATE))"sv,
           "SELECT CASE WHEN a > 0 THEN 1 END::INT, EXTRACT(year FROM d)::INT, f(a)::INT, "
           "COUNT(*)::BIGINT FROM t GROUP BY a::VARCHAR ORDER BY 1::INT"sv,
           "SELECT CAST(CAST(a AS VARCHAR) AS DATE), CAST(a = 1 OR b AS BOOLEAN), x::T_1 FROM t"sv,
       }) {
    ExpectRoundTrip(sql);
  }
}

TEST(UnparseTest, RendersFullAst) {
  SelectStatement stmt;
  AggregateCall sum{.kind = AggKind::kSum};
  sum.arg.emplace(Expr(ColumnRef{.name = "a"}));
  stmt.items.push_back(SelectItem{.expr = Expr(std::move(sum)), .alias = "s"});
  stmt.from = TableRef{.kind = TableRef::Kind::kName, .name = "t"};
  stmt.where.push_back(ToExpr(Comparison{
      .column = ColumnRef{.name = "b"},
      .op = CompareOp::kGe,
      .literal = Literal{.kind = Literal::Kind::kInteger, .negative = true, .text = "5"}}));
  stmt.limit = 10;
  EXPECT_EQ(ToSql(stmt), R"(SELECT SUM(a) AS "s" FROM t WHERE b >= -5 LIMIT 10)");
}

TEST(UnparseTest, RendersEveryLiteralKindAndOperator) {
  SelectStatement stmt;
  stmt.star = true;
  stmt.from = TableRef{.kind = TableRef::Kind::kPath, .name = "x'y.parquet"};
  const std::array<Literal, 6> literals{{
      Literal{.kind = Literal::Kind::kInteger, .negative = false, .text = "1"},
      Literal{.kind = Literal::Kind::kDecimal, .negative = true, .text = "2.5"},
      Literal{.kind = Literal::Kind::kString, .negative = false, .text = "it's"},
      Literal{.kind = Literal::Kind::kDate, .negative = false, .text = "2024-01-31"},
      Literal{.kind = Literal::Kind::kInteger, .negative = false, .text = "3"},
      Literal{.kind = Literal::Kind::kInteger, .negative = false, .text = "4"},
  }};
  const std::array<CompareOp, 6> ops{CompareOp::kEq, CompareOp::kNe, CompareOp::kLt,
                                     CompareOp::kLe, CompareOp::kGt, CompareOp::kGe};
  for (std::size_t i = 0; i < ops.size(); ++i) {
    stmt.where.push_back(ToExpr(
        Comparison{.column = ColumnRef{.name = "c" + std::to_string(i), .quoted = i % 2 == 1},
                   .op = ops[i],
                   .literal = literals[i]}));
  }
  EXPECT_EQ(ToSql(stmt),
            R"(SELECT * FROM 'x''y.parquet' WHERE c0 = 1 AND "c1" <> -2.5 AND c2 < 'it''s' AND )"
            R"("c3" <= DATE '2024-01-31' AND c4 > 3 AND "c5" >= 4)");
  auto parsed = Parse(ToSql(stmt));
  ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
  EXPECT_TRUE(EqualIgnoringSpans(stmt, *parsed));
}

TEST(UnparseTest, LimitExtremes) {
  SelectStatement stmt;
  stmt.star = true;
  stmt.from = TableRef{.kind = TableRef::Kind::kName, .name = "t"};
  stmt.limit = 0;
  EXPECT_EQ(ToSql(stmt), "SELECT * FROM t LIMIT 0");
  stmt.limit = std::numeric_limits<std::int64_t>::max();
  EXPECT_EQ(ToSql(stmt), "SELECT * FROM t LIMIT 9223372036854775807");
}

TEST(EqualIgnoringSpansTest, IgnoresOnlySpans) {
  auto a = Parse("SELECT COUNT(*) AS n, x FROM t WHERE a = 1 AND b < 'z' LIMIT 3");
  auto b = Parse("select   count( * )  n ,x from t where ( a=1 ) and b<'z' limit 3 ;");
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(b.has_value());
  EXPECT_NE(a->span, b->span);
  EXPECT_TRUE(EqualIgnoringSpans(*a, *b)) << ToSql(*a) << "\n" << ToSql(*b);
  EXPECT_TRUE(EqualIgnoringSpans(*b, *a));
}

TEST(EqualIgnoringSpansTest, DetectsEveryStructuralDifference) {
  const std::string_view base = "SELECT SUM(a) AS s, b FROM t WHERE c = 1 LIMIT 3";
  for (const std::string_view other : {
           "SELECT SUM(a) AS s, b FROM t WHERE c = 1 LIMIT 4"sv,            // limit
           "SELECT SUM(a) AS s, b FROM t WHERE c = 1"sv,                    // no limit
           "SELECT SUM(a) AS S, b FROM t WHERE c = 1 LIMIT 3"sv,            // alias spelling
           "SELECT SUM(a), b FROM t WHERE c = 1 LIMIT 3"sv,                 // no alias
           "SELECT AVG(a) AS s, b FROM t WHERE c = 1 LIMIT 3"sv,            // aggregate kind
           R"(SELECT SUM("a") AS s, b FROM t WHERE c = 1 LIMIT 3)"sv,       // quoted argument
           "SELECT SUM(a) AS s, B FROM t WHERE c = 1 LIMIT 3"sv,            // column spelling
           "SELECT SUM(a) AS s FROM t WHERE c = 1 LIMIT 3"sv,               // item count
           "SELECT a AS s, b FROM t WHERE c = 1 LIMIT 3"sv,                 // column vs aggregate
           R"(SELECT SUM(a) AS s, b FROM "t" WHERE c = 1 LIMIT 3)"sv,       // quoted table
           "SELECT SUM(a) AS s, b FROM 't' WHERE c = 1 LIMIT 3"sv,          // path table
           "SELECT SUM(a) AS s, b FROM u WHERE c = 1 LIMIT 3"sv,            // table name
           "SELECT SUM(a) AS s, b FROM t WHERE c = -1 LIMIT 3"sv,           // sign
           "SELECT SUM(a) AS s, b FROM t WHERE c = 1.0 LIMIT 3"sv,          // literal kind
           "SELECT SUM(a) AS s, b FROM t WHERE c = 01 LIMIT 3"sv,           // literal text
           "SELECT SUM(a) AS s, b FROM t WHERE c = '1' LIMIT 3"sv,          // string literal
           "SELECT SUM(a) AS s, b FROM t WHERE c <> 1 LIMIT 3"sv,           // operator
           "SELECT SUM(a) AS s, b FROM t WHERE d = 1 LIMIT 3"sv,            // column
           "SELECT SUM(a) AS s, b FROM t WHERE c = 1 AND c = 1 LIMIT 3"sv,  // conjunct count
           "SELECT SUM(a) AS s, b FROM t LIMIT 3"sv,                        // no where
       }) {
    auto x = Parse(base);
    auto y = Parse(other);
    ASSERT_TRUE(x.has_value());
    ASSERT_TRUE(y.has_value()) << other;
    EXPECT_FALSE(EqualIgnoringSpans(*x, *y)) << other;
  }
  auto star = Parse("SELECT * FROM t");
  auto count = Parse("SELECT COUNT(*) FROM t");
  auto count_col = Parse("SELECT COUNT(a) FROM t");
  ASSERT_TRUE(star && count && count_col);
  EXPECT_FALSE(EqualIgnoringSpans(*star, *count));
  EXPECT_FALSE(EqualIgnoringSpans(*count, *count_col));
}

TEST(EqualIgnoringSpansTest, DetectsEveryDifferenceInNewClauses) {
  const std::string_view base =
      "SELECT a, COUNT(DISTINCT u) FROM t GROUP BY a, b ORDER BY a DESC NULLS FIRST, COUNT(*) "
      "LIMIT 3 OFFSET 4";
  for (const std::string_view other : {
           "SELECT a, COUNT(u) FROM t GROUP BY a, b ORDER BY a DESC NULLS FIRST, COUNT(*) LIMIT 3 "
           "OFFSET 4"sv,  // not distinct
           "SELECT a, COUNT(DISTINCT u) FROM t GROUP BY a ORDER BY a DESC NULLS FIRST, COUNT(*) "
           "LIMIT 3 OFFSET 4"sv,  // group count
           "SELECT a, COUNT(DISTINCT u) FROM t GROUP BY a, c ORDER BY a DESC NULLS FIRST, "
           "COUNT(*) LIMIT 3 OFFSET 4"sv,  // group column
           R"(SELECT a, COUNT(DISTINCT u) FROM t GROUP BY a, "b" ORDER BY a DESC NULLS FIRST, )"
           "COUNT(*) LIMIT 3 OFFSET 4"sv,  // quoted group column
           "SELECT a, COUNT(DISTINCT u) FROM t GROUP BY a, b ORDER BY a NULLS FIRST, COUNT(*) "
           "LIMIT 3 OFFSET 4"sv,  // direction
           "SELECT a, COUNT(DISTINCT u) FROM t GROUP BY a, b ORDER BY a DESC NULLS LAST, "
           "COUNT(*) LIMIT 3 OFFSET 4"sv,  // nulls order
           "SELECT a, COUNT(DISTINCT u) FROM t GROUP BY a, b ORDER BY a DESC, COUNT(*) LIMIT 3 "
           "OFFSET 4"sv,  // default nulls order
           "SELECT a, COUNT(DISTINCT u) FROM t GROUP BY a, b ORDER BY a DESC NULLS FIRST, "
           "SUM(a) LIMIT 3 OFFSET 4"sv,  // order expression
           "SELECT a, COUNT(DISTINCT u) FROM t GROUP BY a, b ORDER BY a DESC NULLS FIRST LIMIT 3 "
           "OFFSET 4"sv,  // order count
           "SELECT a, COUNT(DISTINCT u) FROM t GROUP BY a, b ORDER BY a DESC NULLS FIRST, "
           "COUNT(*) LIMIT 3 OFFSET 5"sv,  // offset
           "SELECT a, COUNT(DISTINCT u) FROM t GROUP BY a, b ORDER BY a DESC NULLS FIRST, "
           "COUNT(*) LIMIT 3"sv,  // no offset
       }) {
    auto x = Parse(base);
    auto y = Parse(other);
    ASSERT_TRUE(x.has_value()) << x.error().message;
    ASSERT_TRUE(y.has_value()) << other;
    EXPECT_FALSE(EqualIgnoringSpans(*x, *y)) << other;
  }
  const std::string_view having_base =
      "SELECT a FROM t GROUP BY a HAVING COUNT(*) > 1 AND a IN (1, 2)";
  for (const std::string_view other : {
           "SELECT a FROM t GROUP BY a HAVING COUNT(*) > 1"sv,                     // count
           "SELECT a FROM t GROUP BY a HAVING COUNT(a) > 1 AND a IN (1, 2)"sv,     // aggregate
           "SELECT a FROM t GROUP BY a HAVING b > 1 AND a IN (1, 2)"sv,            // operand kind
           "SELECT a FROM t GROUP BY a HAVING COUNT(*) >= 1 AND a IN (1, 2)"sv,    // operator
           "SELECT a FROM t GROUP BY a HAVING COUNT(*) > 2 AND a IN (1, 2)"sv,     // literal
           "SELECT a FROM t GROUP BY a HAVING COUNT(*) > 1 AND a IN (1, 3)"sv,     // list value
           "SELECT a FROM t GROUP BY a HAVING COUNT(*) > 1 AND a IN (1, 2, 3)"sv,  // list length
       }) {
    auto x = Parse(having_base);
    auto y = Parse(other);
    ASSERT_TRUE(x.has_value()) << x.error().message;
    ASSERT_TRUE(y.has_value()) << other;
    EXPECT_FALSE(EqualIgnoringSpans(*x, *y)) << other;
  }
}

TEST(AstTest, ToStringNamesKindsAndOperators) {
  EXPECT_EQ(ToString(NullsOrder::kDefault), "");
  EXPECT_EQ(ToString(NullsOrder::kFirst), "NULLS FIRST");
  EXPECT_EQ(ToString(NullsOrder::kLast), "NULLS LAST");
  EXPECT_EQ(ToString(AggKind::kCountStar), "COUNT");
  EXPECT_EQ(ToString(AggKind::kCount), "COUNT");
  EXPECT_EQ(ToString(AggKind::kSum), "SUM");
  EXPECT_EQ(ToString(AggKind::kAvg), "AVG");
  EXPECT_EQ(ToString(AggKind::kMin), "MIN");
  EXPECT_EQ(ToString(AggKind::kMax), "MAX");
  EXPECT_EQ(ToString(CompareOp::kEq), "=");
  EXPECT_EQ(ToString(CompareOp::kNe), "<>");
  EXPECT_EQ(ToString(CompareOp::kLt), "<");
  EXPECT_EQ(ToString(CompareOp::kLe), "<=");
  EXPECT_EQ(ToString(CompareOp::kGt), ">");
  EXPECT_EQ(ToString(CompareOp::kGe), ">=");
  EXPECT_EQ(ToString(CompareOp::kLike), "LIKE");
  EXPECT_EQ(ToString(CompareOp::kNotLike), "NOT LIKE");
  EXPECT_EQ(ToString(CompareOp::kIn), "IN");
  EXPECT_EQ(ToString(CompareOp::kNotIn), "NOT IN");
}

// ToExpr is the inverse of AsComparison / AsHavingComparison, for every operator.
TEST(AstTest, ToExprInvertsTheNormalizedForms) {
  const Literal pattern{.kind = Literal::Kind::kString, .negative = false, .text = "a%"};
  const Literal one{.kind = Literal::Kind::kInteger, .negative = false, .text = "1"};
  const Literal two{.kind = Literal::Kind::kInteger, .negative = true, .text = "2"};
  for (const CompareOp op :
       {CompareOp::kEq, CompareOp::kNe, CompareOp::kLt, CompareOp::kLe, CompareOp::kGt,
        CompareOp::kGe, CompareOp::kLike, CompareOp::kNotLike, CompareOp::kIn, CompareOp::kNotIn}) {
    const bool list = op == CompareOp::kIn || op == CompareOp::kNotIn;
    const bool like = op == CompareOp::kLike || op == CompareOp::kNotLike;
    const Comparison cmp{.column = ColumnRef{.name = "c"},
                         .op = op,
                         .literal = like ? pattern : one,
                         .list = list ? std::vector<Literal>{one, two} : std::vector<Literal>{}};
    const auto back = AsComparison(ToExpr(cmp));
    ASSERT_TRUE(back.has_value()) << ToString(op);
    EXPECT_EQ(back.value_or(Comparison{}).op, op);
    EXPECT_EQ(back.value_or(Comparison{}).list.size(), cmp.list.size());
    AggregateCall count{.kind = AggKind::kCountStar};
    const HavingComparison having{
        .operand = count, .op = op, .literal = cmp.literal, .list = cmp.list};
    const auto having_back = AsHavingComparison(ToExpr(having));
    ASSERT_TRUE(having_back.has_value()) << ToString(op);
    EXPECT_TRUE(
        std::holds_alternative<AggregateCall>(having_back.value_or(HavingComparison{}).operand));
    EXPECT_EQ(ToSql(ToExpr(having)).substr(0, 8), "COUNT(*)");
  }
  // Not simple: two columns, an IN list with a column, an arithmetic operand.
  auto stmt = Parse("SELECT a FROM t WHERE a = b AND a IN (1, b) AND a + 1 > 2 AND a LIKE b");
  ASSERT_TRUE(stmt.has_value());
  for (const Expr& conjunct : stmt->where) {
    EXPECT_FALSE(AsComparison(conjunct).has_value()) << ToSql(conjunct);
  }
}

TEST(AstTest, BoxCopiesDeeply) {
  Box<Expr> a(Expr(ColumnRef{.name = "a"}));
  Box<Expr> b(Expr(ColumnRef{.name = "b"}));
  b = a;
  std::get<ColumnRef>(*a).name = "changed";
  EXPECT_EQ(std::get<ColumnRef>(*b).name, "a");
  const Box<Expr>& self = b;
  b = self;
  EXPECT_EQ(std::get<ColumnRef>(*b).name, "a");
}

// EqualIgnoringSpans tells apart every part of a CASE and of the other expression nodes.
TEST(EqualIgnoringSpansTest, DetectsDifferencesInExpressions) {
  const std::string_view base = "CASE a WHEN 1 THEN 'x' ELSE 'y' END";
  for (const std::string_view other : {
           "CASE WHEN 1 THEN 'x' ELSE 'y' END"sv,                    // operand
           "CASE a WHEN 1 THEN 'x' END"sv,                           // else
           "CASE a WHEN 2 THEN 'x' ELSE 'y' END"sv,                  // when
           "CASE a WHEN 1 THEN 'z' ELSE 'y' END"sv,                  // then
           "CASE a WHEN 1 THEN 'x' WHEN 2 THEN 'x' ELSE 'y' END"sv,  // branches
           "f(a)"sv,                                                 // node kind
       }) {
    auto x = Parse("SELECT " + std::string(base) + " FROM t");
    auto y = Parse("SELECT " + std::string(other) + " FROM t");
    ASSERT_TRUE(x.has_value() && y.has_value()) << other;
    EXPECT_FALSE(EqualIgnoringSpans(x->items[0].expr, y->items[0].expr)) << other;
  }
  for (const auto& [a, b] : std::to_array<std::pair<std::string_view, std::string_view>>({
           {"-a", "NOT a"},
           {"a + b", "a - b"},
           {"a LIKE 'x'", "a NOT LIKE 'x'"},
           {"a IN (1)", "a NOT IN (1)"},
           {"a IN (1)", "a IN (1, 2)"},
           {"f(a)", "g(a)"},
           {"f(a)", "\"f\"(a)"},
           {"f(a)", "f(a, b)"},
           {"EXTRACT(minute FROM a)", "EXTRACT(hour FROM a)"},
           {"SUM(a)", "SUM(a + 1)"},
           {"COUNT(*)", "COUNT(a)"},
           {"CAST(a AS INT)", "TRY_CAST(a AS INT)"},
           {"CAST(a AS INT)", "CAST(a AS BIGINT)"},
           {"CAST(a AS INT)", "CAST(b AS INT)"},
           {"CAST(a AS INT)", "a"},
           {"a::INT::INT", "a::INT"},
           {"CAST(a AS DECIMAL(15, 2))", "CAST(a AS DECIMAL(15, 3))"},
           {"CAST(a AS DECIMAL(15))", "CAST(a AS DECIMAL(15, 2))"},
           {"CAST(a AS DECIMAL)", "CAST(a AS DECIMAL(15))"},
       })) {
    auto x = Parse("SELECT " + std::string(a) + " FROM t");
    auto y = Parse("SELECT " + std::string(b) + " FROM t");
    ASSERT_TRUE(x.has_value() && y.has_value()) << a << " / " << b;
    EXPECT_FALSE(EqualIgnoringSpans(x->items[0].expr, y->items[0].expr)) << a << " / " << b;
    EXPECT_TRUE(EqualIgnoringSpans(x->items[0].expr, x->items[0].expr));
  }
  // The spelling of a cast is not recorded, and type names are upper-cased.
  auto cast = Parse("SELECT CAST(a AS DECIMAL(15, 2)) FROM t");
  auto colons = Parse("SELECT a::decimal(15,2) FROM t");
  ASSERT_TRUE(cast.has_value() && colons.has_value());
  EXPECT_TRUE(EqualIgnoringSpans(cast->items[0].expr, colons->items[0].expr));
}

}  // namespace
}  // namespace antb1::sql
