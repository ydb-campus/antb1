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
           Case{.input = "SELECT a FROM t WHERE 5 < a AND b != 'it''s' AND DATE '2024-01-31' >= d",
                .canonical =
                    "SELECT a FROM t WHERE a > 5 AND b <> 'it''s' AND d <= DATE '2024-01-31'"},
           Case{.input = "SELECT a FROM t WHERE a = - 1.50 and b >= .5 and c < 1e3 and d > 5.",
                .canonical = "SELECT a FROM t WHERE a = -1.50 AND b >= .5 AND c < 1e3 AND d > 5."},
           Case{.input = "SELECT a FROM t WHERE a = 007 LIMIT 0000010",
                .canonical = "SELECT a FROM t WHERE a = 007 LIMIT 10"},
           Case{.input = R"(SELECT "a""b" FROM "t""u" WHERE "c" = '''')",
                .canonical = R"(SELECT "a""b" FROM "t""u" WHERE "c" = '''')"},
           Case{.input = "/* hi */ SELECT a -- x\n FROM t -- y", .canonical = "SELECT a FROM t"},
           Case{.input = "SELECT MIN(ts), MAX(ts), AVG(x) FROM t WHERE ts > date '2020-01-01'",
                .canonical = "SELECT MIN(ts), MAX(ts), AVG(x) FROM t WHERE ts > DATE '2020-01-01'"},
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
       }) {
    ExpectRoundTrip(sql);
  }
}

TEST(UnparseTest, RendersFullAst) {
  SelectStatement stmt;
  stmt.items.push_back(SelectItem{
      .expr = AggregateCall{.kind = AggKind::kSum, .arg = ColumnRef{.name = "a"}}, .alias = "s"});
  stmt.from = TableRef{.kind = TableRef::Kind::kName, .name = "t"};
  stmt.where.push_back(Comparison{
      .column = ColumnRef{.name = "b"},
      .op = CompareOp::kGe,
      .literal = Literal{.kind = Literal::Kind::kInteger, .negative = true, .text = "5"}});
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
    stmt.where.push_back(
        Comparison{.column = ColumnRef{.name = "c" + std::to_string(i), .quoted = i % 2 == 1},
                   .op = ops[i],
                   .literal = literals[i]});
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
  auto b = Parse("select   count( * )  n ,x from t where 1=a and 'z'>b limit 3 ;");
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(b.has_value());
  EXPECT_NE(a->span, b->span);
  EXPECT_TRUE(EqualIgnoringSpans(*a, *b));
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

TEST(AstTest, ToStringNamesKindsAndOperators) {
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
}

}  // namespace
}  // namespace antb1::sql
