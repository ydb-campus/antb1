#include "antb1/sql/parser.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <variant>

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
  EXPECT_EQ(stmt->from.kind, TableRef::Kind::kName);
  EXPECT_EQ(stmt->from.name, "events");
  EXPECT_FALSE(stmt->from.quoted);
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
  EXPECT_EQ(stmt->from.name, "Events");
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
  EXPECT_EQ(stmt->from.name, "date");
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
  EXPECT_EQ(stmt->from.name, "where");
  EXPECT_TRUE(stmt->from.quoted);
  ASSERT_EQ(stmt->where.size(), 1U);
  EXPECT_EQ(Cmp(stmt->where[0]).column.name, "limit");
}

TEST(ParserTest, TableReferences) {
  auto name = Parse("SELECT a FROM events");
  ASSERT_TRUE(name.has_value());
  EXPECT_EQ(name->from.kind, TableRef::Kind::kName);
  EXPECT_FALSE(name->from.quoted);

  constexpr std::string_view kQuoted = R"(SELECT a FROM "My ""Events""")";
  auto quoted = Parse(kQuoted);
  ASSERT_TRUE(quoted.has_value());
  EXPECT_EQ(quoted->from.kind, TableRef::Kind::kName);
  EXPECT_EQ(quoted->from.name, R"(My "Events")");
  EXPECT_TRUE(quoted->from.quoted);
  EXPECT_EQ(At(kQuoted, quoted->from.span), R"("My ""Events""")");

  constexpr std::string_view kPath = "select count(*) from 'data/it''s part-0.parquet'";
  auto path = Parse(kPath);
  ASSERT_TRUE(path.has_value());
  EXPECT_EQ(path->from.kind, TableRef::Kind::kPath);
  EXPECT_EQ(path->from.name, "data/it's part-0.parquet");
  EXPECT_FALSE(path->from.quoted);
  EXPECT_EQ(At(kPath, path->from.span), "'data/it''s part-0.parquet'");

  auto glob = Parse("SELECT * FROM 'data/*.parquet'");
  ASSERT_TRUE(glob.has_value());
  EXPECT_EQ(glob->from.name, "data/*.parquet");
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
                    "2024-03-01"}),
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
  EXPECT_EQ(stmt->from.name, "isnull");
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
        RejectCase{"TryCast", "SELECT ^try_cast(a AS BIGINT) FROM events", kUnsupported, 8,
                   "function try_cast() with this argument syntax is not supported"},
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
        RejectCase{"Join", "SELECT a FROM events ^JOIN users ON a = b", kUnsupported, 4,
                   "JOIN is not supported"},
        RejectCase{"LeftJoin", "SELECT a FROM events ^LEFT JOIN users ON a = b", kUnsupported, 4,
                   "LEFT JOIN is not supported"},
        RejectCase{"InnerJoin", "SELECT a FROM events ^INNER JOIN users USING (a)", kUnsupported, 5,
                   "INNER JOIN is not supported"},
        RejectCase{"CrossJoin", "SELECT a FROM events ^CROSS JOIN users", kUnsupported, 5,
                   "CROSS JOIN is not supported"},
        RejectCase{"NaturalJoin", "SELECT a FROM events ^NATURAL JOIN users", kUnsupported, 7,
                   "NATURAL JOIN is not supported"},
        RejectCase{"CommaJoin", "SELECT a FROM events^, users", kUnsupported, 1,
                   "multiple tables in FROM (JOIN) are not supported"},
        RejectCase{"Union", "SELECT a FROM events ^UNION SELECT a FROM users", kUnsupported, 5,
                   "UNION is not supported"},
        RejectCase{"UnionAfterWhere", "SELECT a FROM events WHERE a = 1 ^UNION ALL SELECT 1",
                   kUnsupported, 5, "UNION is not supported"},
        RejectCase{"Intersect", "SELECT a FROM events ^INTERSECT SELECT a FROM users", kUnsupported,
                   9, "INTERSECT is not supported"},
        RejectCase{"Except", "SELECT a FROM events ^EXCEPT SELECT a FROM users", kUnsupported, 6,
                   "EXCEPT is not supported"},
        RejectCase{"With", "^WITH x AS (SELECT a FROM events) SELECT a FROM x", kUnsupported, 4,
                   "WITH (common table expressions) is not supported"},
        RejectCase{"LikeEscape", "SELECT a FROM events WHERE url LIKE 'x!%' ^ESCAPE '!'",
                   kUnsupported, 6, "LIKE ... ESCAPE is not supported"},
        RejectCase{"ILike", "SELECT a FROM events WHERE url ^ILIKE '%x%'", kUnsupported, 5,
                   "ILIKE is not supported"},
        RejectCase{"SimilarTo", "SELECT a FROM events WHERE url ^SIMILAR TO 'x'", kUnsupported, 7,
                   "SIMILAR TO is not supported"},
        RejectCase{"InSubquery", "SELECT a FROM events WHERE region IN (^SELECT b FROM t)",
                   kUnsupported, 6, "IN (subquery) is not supported"},
        RejectCase{"Between", "SELECT a FROM events WHERE a ^BETWEEN 1 AND 2", kUnsupported, 7,
                   "BETWEEN is not supported"},
        RejectCase{"NotBetween", "SELECT a FROM events WHERE a ^NOT BETWEEN 1 AND 2", kUnsupported,
                   3, "NOT BETWEEN is not supported"},
        RejectCase{"BetweenAfterLiteral", "SELECT a FROM events WHERE 1 ^BETWEEN a AND b",
                   kUnsupported, 7, "BETWEEN is not supported"},
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
        RejectCase{"SubqueryInFrom", "SELECT a FROM ^(SELECT a FROM events)", kUnsupported, 1,
                   "subqueries in FROM are not supported"},
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
        RejectCase{"Cast", "SELECT ^CAST(a AS BIGINT) FROM events", kUnsupported, 4,
                   "CAST is not supported"},
        RejectCase{"CastInWhere", "SELECT a FROM events WHERE a = ^cast('1' AS INT)", kUnsupported,
                   4, "CAST is not supported"},
        RejectCase{"CastOperator", "SELECT a FROM events WHERE a^::BIGINT = 1", kUnsupported, 2,
                   "CAST (::) is not supported"},
        RejectCase{"CastOperatorInSelect", "SELECT a^::TEXT FROM events", kUnsupported, 2,
                   "CAST (::) is not supported"},
        RejectCase{"TimestampLiteral", "SELECT a FROM events WHERE ts > ^TIMESTAMP '2024-01-01'",
                   kUnsupported, 9, "TIMESTAMP literals are not supported"},
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
        RejectCase{"QualifiedColumn", "SELECT e^.a FROM events", kUnsupported, 1,
                   "qualified names (a.b) are not supported"},
        RejectCase{"QualifiedColumnInWhere", "SELECT a FROM events WHERE e^.a = 1", kUnsupported, 1,
                   "qualified names (a.b) are not supported"},
        RejectCase{"QualifiedColumnInAggregate", "SELECT SUM(e^.a) FROM events", kUnsupported, 1,
                   "qualified names (a.b) are not supported"},
        RejectCase{"QualifiedTable", "SELECT a FROM main^.events", kUnsupported, 1,
                   "qualified table names are not supported"},
        RejectCase{"UnquotedPath", "SELECT a FROM data^.parquet", kUnsupported, 1,
                   "qualified table names are not supported"},
        RejectCase{"TableAlias", "SELECT a FROM events ^e", kUnsupported, 1,
                   "table aliases are not supported"},
        RejectCase{"TableAliasWithAs", "SELECT a FROM events ^AS e", kUnsupported, 2,
                   "table aliases are not supported"},
        RejectCase{"TableAliasQuoted", R"(SELECT a FROM 'x.parquet' ^"e")", kUnsupported, 3,
                   "table aliases are not supported"},
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
                   "typed literals other than DATE '...' and prefixed strings (E'...') are not "
                   "supported"},
        RejectCase{"TypedLiteralFirst", "SELECT a FROM events WHERE ^INT '1' = a", kUnsupported, 3,
                   "typed literals other than DATE"},
        RejectCase{"TypedLiteralInSelect", "SELECT ^int4 '1' FROM events", kUnsupported, 4,
                   "typed literals other than DATE"},
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
                   "unterminated string literal"}),
    CaseName);

TEST(ParserTest, ExactMessages) {
  auto join = Parse("SELECT COUNT(*) FROM events JOIN users USING (id)");
  ASSERT_FALSE(join.has_value());
  EXPECT_EQ(join.error().message, "JOIN is not supported; see docs/sql-subset.md");
  auto limit = Parse("SELECT a FROM events LIMIT 9223372036854775808");
  ASSERT_FALSE(limit.has_value());
  EXPECT_EQ(limit.error().message,
            "LIMIT 9223372036854775808 is out of range (the maximum is 9223372036854775807)");
  auto alias = Parse("SELECT a AS from FROM events");
  ASSERT_FALSE(alias.has_value());
  EXPECT_EQ(alias.error().message,
            "an alias cannot be the reserved word FROM; write it as a quoted identifier; see "
            "docs/sql-subset.md");
}

TEST(ParserTest, EveryReservedWordIsRejectedAsAnAlias) {
  for (const std::string_view word :
       {"ALL",    "AND",     "ANY",     "ARRAY", "AS",        "ASC",      "BETWEEN", "BY",
        "CASE",   "CAST",    "COLLATE", "CROSS", "DESC",      "DISTINCT", "ELSE",    "END",
        "EXCEPT", "EXISTS",  "FALSE",   "FETCH", "FOR",       "FROM",     "FULL",    "GROUP",
        "HAVING", "ILIKE",   "IN",      "INNER", "INTERSECT", "INTERVAL", "INTO",    "IS",
        "JOIN",   "LATERAL", "LEFT",    "LIKE",  "LIMIT",     "NATURAL",  "NOT",     "NULL",
        "OFFSET", "ON",      "OR",      "ORDER", "OUTER",     "OVER",     "QUALIFY", "RIGHT",
        "SELECT", "SIMILAR", "SOME",    "TABLE", "THEN",      "TRUE",     "UNION",   "USING",
        "WHEN",   "WHERE",   "WINDOW",  "WITH"}) {
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
        std::string("SELECT COUNT(*) FROM "), std::string("SELECT a FROM t LIMIT ")}) {
    const std::string sql = prefix + parens;
    auto result = Parse(sql);
    ASSERT_FALSE(result.has_value()) << prefix;
    EXPECT_EQ(result.error().kind, ParseError::Kind::kUnsupported) << prefix;
    // In an expression the parentheses nest up to the depth limit; a query, FROM and LIMIT take
    // none.
    const bool expression =
        !prefix.empty() && !prefix.ends_with("FROM ") && !prefix.ends_with("LIMIT ");
    const SourceSpan span = result.error().span;
    EXPECT_EQ(span.length, 1U) << prefix;
    if (expression) {
      EXPECT_GT(span.offset, prefix.size()) << prefix;
      EXPECT_LE(span.offset, prefix.size() + 256) << prefix;
      EXPECT_TRUE(result.error().message.starts_with("expressions deeper than 256 levels"))
          << result.error().message;
    } else {
      EXPECT_EQ(span.offset, prefix.size()) << prefix;
    }
  }
  ExpectWellFormedError(std::string(kSize, ')'));
  ExpectWellFormedError("SELECT a FROM t WHERE a = 1" + std::string(kSize, ')'));
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

  // Large but valid inputs parse in linear time.
  const std::string long_name(kSize, 'n');
  auto name = Parse("SELECT " + long_name + " FROM " + long_name);
  ASSERT_TRUE(name.has_value());
  EXPECT_EQ(name->from.name.size(), kSize);
  auto path = Parse("SELECT * FROM '" + std::string(kSize, 'p') + "'");
  ASSERT_TRUE(path.has_value());
  EXPECT_EQ(path->from.name.size(), kSize);

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
  EXPECT_EQ(ident_ok->from.name, "\xc3\x28.parquet");

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
