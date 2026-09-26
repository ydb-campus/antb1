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
  return agg.arg.has_value() ? agg.arg.value_or(ColumnRef{}).name : "<none>";
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
  EXPECT_TRUE(avg->arg.value_or(ColumnRef{}).quoted);
  EXPECT_EQ(At(kSql, avg->arg.value_or(ColumnRef{}).span), R"("Amount")");
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
  EXPECT_EQ(stmt->where[0].column.name, "limit");
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
  const Comparison& cmp = stmt->where[0];
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
  const Comparison& cmp = stmt->where[0];
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
  const Literal& literal = stmt->where[0].literal;
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
  EXPECT_EQ(stmt->where[0].column.name, "date");
  EXPECT_EQ(stmt->where[0].literal.kind, Literal::Kind::kDate);
  EXPECT_EQ(At(kSql, stmt->where[0].literal.span), "DATE '2024-01-01'");
}

TEST(ParserTest, ConjunctionOfComparisons) {
  constexpr std::string_view kSql =
      "SELECT COUNT(*) FROM events WHERE amount > 0 AND 'north' = region and ts <= DATE "
      R"('2024-12-31' AND "User" <> -1)";
  auto stmt = Parse(kSql);
  ASSERT_TRUE(stmt.has_value()) << stmt.error().message;
  ASSERT_EQ(stmt->where.size(), 4U);
  EXPECT_EQ(At(kSql, stmt->where[0].span), "amount > 0");
  EXPECT_EQ(stmt->where[1].column.name, "region");
  EXPECT_EQ(stmt->where[1].op, CompareOp::kEq);
  EXPECT_EQ(At(kSql, stmt->where[1].span), "'north' = region");
  EXPECT_EQ(stmt->where[2].literal.kind, Literal::Kind::kDate);
  EXPECT_TRUE(stmt->where[3].column.quoted);
  EXPECT_EQ(stmt->where[3].op, CompareOp::kNe);
  EXPECT_TRUE(stmt->where[3].literal.negative);
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
    EXPECT_EQ(stmt->where[0].column.name, "a");
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
  EXPECT_EQ(stmt->where[0].column.name, "notnull");
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
    const Comparison& cmp = stmt->where[i];
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
        RejectCase{"GroupBy", "SELECT COUNT(*) FROM events ^GROUP BY region", kUnsupported, 5,
                   "GROUP BY is not supported"},
        RejectCase{"GroupByAfterWhere", "SELECT COUNT(*) FROM events WHERE a = 1 ^group by a",
                   kUnsupported, 5, "GROUP BY is not supported"},
        RejectCase{"GroupByWithoutFrom", "SELECT a ^GROUP BY a", kUnsupported, 5,
                   "GROUP BY is not supported"},
        RejectCase{"OrderBy", "SELECT a FROM events ^ORDER BY a", kUnsupported, 5,
                   "ORDER BY is not supported"},
        RejectCase{"OrderByAfterLimit", "SELECT a FROM events LIMIT 5 ^ORDER BY a", kUnsupported, 5,
                   "ORDER BY is not supported"},
        RejectCase{"OrderByInAggregate", "SELECT SUM(a ^ORDER BY b) FROM events", kUnsupported, 5,
                   "ORDER BY is not supported"},
        RejectCase{"Distinct", "SELECT ^DISTINCT region FROM events", kUnsupported, 8,
                   "DISTINCT is not supported"},
        RejectCase{"DistinctAggregate", "SELECT COUNT(^distinct user_id) FROM events", kUnsupported,
                   8, "DISTINCT aggregates are not supported"},
        RejectCase{"SelectAll", "SELECT ^ALL a FROM events", kUnsupported, 3,
                   "SELECT ALL is not supported"},
        RejectCase{"AllAggregate", "SELECT COUNT(^ALL a) FROM events", kUnsupported, 3,
                   "ALL in aggregate calls is not supported"},
        RejectCase{"Having", "SELECT COUNT(*) FROM events ^HAVING COUNT(*) > 1", kUnsupported, 6,
                   "HAVING is not supported"},
        RejectCase{"Offset", "SELECT a FROM events LIMIT 10 ^OFFSET 5", kUnsupported, 6,
                   "OFFSET is not supported"},
        RejectCase{"OffsetWithoutLimit", "SELECT a FROM events ^OFFSET 5", kUnsupported, 6,
                   "OFFSET is not supported"},
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
        RejectCase{"Like", "SELECT a FROM events WHERE url ^LIKE '%x%'", kUnsupported, 4,
                   "LIKE is not supported"},
        RejectCase{"NotLike", "SELECT a FROM events WHERE url ^NOT LIKE '%x%'", kUnsupported, 3,
                   "NOT LIKE is not supported"},
        RejectCase{"ILike", "SELECT a FROM events WHERE url ^ILIKE '%x%'", kUnsupported, 5,
                   "ILIKE is not supported"},
        RejectCase{"SimilarTo", "SELECT a FROM events WHERE url ^SIMILAR TO 'x'", kUnsupported, 7,
                   "SIMILAR TO is not supported"},
        RejectCase{"In", "SELECT a FROM events WHERE region ^IN ('a', 'b')", kUnsupported, 2,
                   "IN is not supported"},
        RejectCase{"NotIn", "SELECT a FROM events WHERE region ^not in ('a')", kUnsupported, 3,
                   "NOT IN is not supported"},
        RejectCase{"Between", "SELECT a FROM events WHERE a ^BETWEEN 1 AND 2", kUnsupported, 7,
                   "BETWEEN is not supported"},
        RejectCase{"NotBetween", "SELECT a FROM events WHERE a ^NOT BETWEEN 1 AND 2", kUnsupported,
                   3, "NOT BETWEEN is not supported"},
        RejectCase{"BetweenAfterLiteral", "SELECT a FROM events WHERE 1 ^BETWEEN a AND b",
                   kUnsupported, 7, "BETWEEN is not supported"},
        RejectCase{"Case", "SELECT ^CASE WHEN a = 1 THEN 1 END FROM events", kUnsupported, 4,
                   "CASE is not supported"},
        RejectCase{"CaseInWhere", "SELECT a FROM events WHERE a = ^CASE WHEN b THEN 1 END",
                   kUnsupported, 4, "CASE is not supported"},
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
        RejectCase{"Or", "SELECT a FROM events WHERE a = 1 ^OR b = 2", kUnsupported, 2,
                   "OR is not supported"},
        RejectCase{"OrAfterColumn", "SELECT a FROM events WHERE flag ^or b = 2", kUnsupported, 2,
                   "OR is not supported"},
        RejectCase{"OrAfterSecondComparison",
                   "SELECT a FROM events WHERE a = 1 AND b = 2 ^OR c = 3", kUnsupported, 2,
                   "OR is not supported"},
        RejectCase{"Not", "SELECT a FROM events WHERE ^NOT a = 1", kUnsupported, 3,
                   "NOT is not supported"},
        RejectCase{"NotAfterAnd", "SELECT a FROM events WHERE a = 1 AND ^NOT b = 2", kUnsupported,
                   3, "NOT is not supported"},
        RejectCase{"PlusInSelect", "SELECT a ^+ 1 FROM events", kUnsupported, 1,
                   "arithmetic operator '+' is not supported"},
        RejectCase{"MinusInSelect", "SELECT a ^- 1 FROM events", kUnsupported, 1,
                   "arithmetic operator '-' is not supported"},
        RejectCase{"TimesInSelect", "SELECT amount ^* 2 FROM events", kUnsupported, 1,
                   "arithmetic operator '*' is not supported"},
        RejectCase{"DivideInSelect", "SELECT SUM(a) ^/ COUNT(a) FROM events", kUnsupported, 1,
                   "arithmetic operator '/' is not supported"},
        RejectCase{"ModuloInSelect", "SELECT a ^% 2 FROM events", kUnsupported, 1,
                   "arithmetic operator '%' is not supported"},
        RejectCase{"ArithmeticLeftOfComparison", "SELECT a FROM events WHERE a ^+ 1 = 2",
                   kUnsupported, 1, "arithmetic operator '+' is not supported"},
        RejectCase{"ArithmeticRightOfComparison", "SELECT a FROM events WHERE a = 1 ^* 2",
                   kUnsupported, 1, "arithmetic operator '*' is not supported"},
        RejectCase{"ArithmeticAfterLiteralFirst", "SELECT a FROM events WHERE 1 ^- 1 = a",
                   kUnsupported, 1, "arithmetic operator '-' is not supported"},
        RejectCase{"ArithmeticInAggregate", "SELECT SUM(a ^* 2) FROM events", kUnsupported, 1,
                   "arithmetic operator '*' is not supported"},
        RejectCase{"UnaryMinusOnColumn", "SELECT a FROM events WHERE a = ^-b", kUnsupported, 1,
                   "arithmetic operator '-' is not supported"},
        RejectCase{"DoubleNegation", "SELECT a FROM events WHERE a = ^- -1", kUnsupported, 1,
                   "arithmetic operator '-' is not supported"},
        RejectCase{"UnaryPlus", "SELECT a FROM events WHERE a = ^+1", kUnsupported, 1,
                   "arithmetic operator '+' is not supported"},
        RejectCase{"UnaryMinusInSelect", "SELECT ^-a FROM events", kUnsupported, 1,
                   "arithmetic operator '-' is not supported"},
        RejectCase{"LimitArithmetic", "SELECT a FROM events LIMIT 1 ^+ 1", kUnsupported, 1,
                   "arithmetic operator '+' is not supported"},
        RejectCase{"LimitParenthesized", "SELECT a FROM events LIMIT ^(5)", kUnsupported, 1,
                   "LIMIT expressions are not supported"},
        RejectCase{"LimitFunction", "SELECT a FROM events LIMIT ^abs(5)", kUnsupported, 3,
                   "LIMIT expressions are not supported"},
        RejectCase{"LimitAll", "SELECT a FROM events LIMIT ^ALL", kUnsupported, 3,
                   "LIMIT ALL is not supported"},
        RejectCase{"Concat", "SELECT a ^|| b FROM events", kUnsupported, 2,
                   "string concatenation (||) is not supported"},
        RejectCase{"Function", "SELECT ^lower(url) FROM events", kUnsupported, 5,
                   "function lower() is not supported"},
        RejectCase{"FunctionInWhere", "SELECT a FROM events WHERE ^length(url) > 5", kUnsupported,
                   6, "function length() is not supported"},
        RejectCase{"FunctionRightOfComparison", "SELECT a FROM events WHERE d > ^now()",
                   kUnsupported, 3, "function now() is not supported"},
        RejectCase{"FunctionInAggregate", "SELECT SUM(^abs(a)) FROM events", kUnsupported, 3,
                   "function abs() is not supported"},
        RejectCase{"QuotedFunction", R"(SELECT ^"lower"(url) FROM events)", kUnsupported, 7,
                   "function calls are not supported"},
        RejectCase{"ReservedWordFunction", "SELECT ^left(url, 3) FROM events", kUnsupported, 4,
                   "function left() is not supported"},
        RejectCase{"TableFunction", "SELECT * FROM ^read_parquet('x.parquet')", kUnsupported, 12,
                   "table functions are not supported"},
        RejectCase{"SubqueryInFrom", "SELECT a FROM ^(SELECT a FROM events)", kUnsupported, 1,
                   "subqueries in FROM are not supported"},
        RejectCase{"SubqueryInWhere", "SELECT a FROM events WHERE a = ^(SELECT 1)", kUnsupported, 1,
                   "parenthesized expressions and subqueries are not supported"},
        RejectCase{"ParenthesizedPredicate", "SELECT a FROM events WHERE ^(a = 1)", kUnsupported, 1,
                   "parenthesized expressions and subqueries are not supported"},
        RejectCase{"ParenthesizedColumn", "SELECT ^(a) FROM events", kUnsupported, 1,
                   "parenthesized expressions and subqueries are not supported"},
        RejectCase{"ParenthesizedAggregateArg", "SELECT SUM(^(a)) FROM events", kUnsupported, 1,
                   "parenthesized expressions and subqueries are not supported"},
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
        RejectCase{"SelectConstant", "SELECT ^1 FROM events", kUnsupported, 1,
                   "constants in the select list are not supported"},
        RejectCase{"SelectString", "SELECT a, ^'x' FROM events", kUnsupported, 3,
                   "constants in the select list are not supported"},
        RejectCase{"SelectDate", "SELECT ^DATE '2024-01-01' FROM events", kUnsupported, 17,
                   "constants in the select list are not supported"},
        RejectCase{"SelectNegative", "SELECT ^-1 FROM events", kUnsupported, 2,
                   "constants in the select list are not supported"},
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
        RejectCase{"CountConstant", "SELECT COUNT(^1) FROM events", kUnsupported, 1,
                   "constant aggregate arguments are not supported (use COUNT(*))"},
        RejectCase{"SumConstant", "SELECT SUM(^'x') FROM events", kUnsupported, 3,
                   "constant aggregate arguments are not supported"},
        RejectCase{"StarWithItems", "SELECT *^, a FROM events", kUnsupported, 1,
                   "combining '*' with other select items is not supported"},
        RejectCase{"ItemsWithStar", "SELECT a, ^* FROM events", kUnsupported, 1,
                   "combining '*' with other select items is not supported"},
        RejectCase{"ComparisonInSelect", "SELECT a ^= 1 FROM events", kUnsupported, 1,
                   "comparisons are only supported in WHERE"},
        RejectCase{"AndInSelect", "SELECT a ^AND b FROM events", kUnsupported, 3,
                   "AND is only supported between comparisons in WHERE"},
        RejectCase{"ColumnVsColumn", "SELECT a FROM events WHERE a = ^b", kUnsupported, 1,
                   "comparisons between two columns are not supported"},
        RejectCase{"LiteralVsLiteral", "SELECT a FROM events WHERE 1 = ^1", kUnsupported, 1,
                   "comparisons between two literals are not supported"},
        RejectCase{"BareColumnPredicate", "SELECT a FROM events WHERE ^flag", kUnsupported, 4,
                   "predicates other than comparisons (column <op> literal) are not supported"},
        RejectCase{"BareColumnBeforeAnd", "SELECT a FROM events WHERE ^flag AND a = 1",
                   kUnsupported, 4, "predicates other than comparisons"},
        RejectCase{"BareLiteralBeforeLimit", "SELECT a FROM events WHERE ^1 LIMIT 5", kUnsupported,
                   1, "predicates other than comparisons"},
        RejectCase{"BareColumnBeforeGroupBy", "SELECT a FROM events WHERE ^flag GROUP BY a",
                   kUnsupported, 4, "predicates other than comparisons"},
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
        RejectCase{"OnlySemicolon", "^;", kSyntax, 1, "expected SELECT, found ';'"},
        RejectCase{"Misspelled", "^SELEC a FROM events", kSyntax, 5,
                   "expected SELECT, found identifier SELEC"},
        RejectCase{"Number", "^42", kSyntax, 2, "expected SELECT, found integer literal 42"},
        RejectCase{"SelectAlone", "SELECT^", kSyntax, 0,
                   "expected a column, an aggregate or '*', found end of input"},
        RejectCase{"EmptySelectList", "SELECT ^FROM events", kSyntax, 4,
                   "expected a column, an aggregate or '*', found keyword FROM"},
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
                   "expected a column, an aggregate or '*', found keyword ORDER"},
        RejectCase{"MissingAlias", "SELECT a AS^", kSyntax, 0,
                   "expected an alias after AS, found end of input"},
        RejectCase{"SumStar", "SELECT SUM(^*) FROM events", kSyntax, 1, "only COUNT accepts '*'"},
        RejectCase{"MaxNothing", "SELECT MAX(^) FROM events", kSyntax, 1,
                   "expected a column in MAX()"},
        RejectCase{"UnclosedCountStar", "SELECT COUNT(* ^FROM events", kSyntax, 4,
                   "expected ')' to close COUNT(, found keyword FROM"},
        RejectCase{"TwoArguments", "SELECT SUM(a^, b) FROM events", kSyntax, 1,
                   "expected ')' to close SUM(, found ','"},
        RejectCase{"AggregateWithoutArgument", "SELECT AVG(^FROM) FROM events", kSyntax, 4,
                   "expected a column, found keyword FROM"},
        RejectCase{"NestedAggregate", "SELECT SUM(^COUNT(a)) FROM events", kSyntax, 5,
                   "aggregate function calls cannot be nested"},
        RejectCase{"AggregateInWhere", "SELECT a FROM events WHERE ^COUNT(*) > 1", kSyntax, 5,
                   "aggregate functions are not allowed in WHERE"},
        RejectCase{"EmptyWhere", "SELECT a FROM events WHERE^", kSyntax, 0,
                   "expected a column or a literal, found end of input"},
        RejectCase{"ReservedInWhere", "SELECT a FROM events WHERE ^LIMIT 5", kSyntax, 5,
                   "expected a column or a literal, found keyword LIMIT"},
        RejectCase{"MissingOperator", "SELECT a FROM events WHERE a ^b", kSyntax, 1,
                   "expected a comparison operator (=, <>, !=, <, <=, >, >=), found identifier b"},
        RejectCase{"DoubleEquals", "SELECT a FROM events WHERE a = ^= 1", kSyntax, 1,
                   "expected a column or a literal, found '='"},
        RejectCase{"MissingRightOperand", "SELECT a FROM events WHERE a =^", kSyntax, 0,
                   "expected a column or a literal, found end of input"},
        RejectCase{"DanglingAnd", "SELECT a FROM events WHERE a = 1 AND^", kSyntax, 0,
                   "expected a column or a literal, found end of input"},
        RejectCase{"ChainedComparison", "SELECT a FROM events WHERE a = 1 ^= 2", kSyntax, 1,
                   "unexpected '='; expected AND, LIMIT or the end of the query"},
        RejectCase{"MissingLimit", "SELECT a FROM events LIMIT^", kSyntax, 0,
                   "expected a non-negative integer after LIMIT, found end of input"},
        RejectCase{"NegativeLimit", "SELECT a FROM events LIMIT ^-1", kSyntax, 1,
                   "LIMIT must not be negative"},
        RejectCase{"LimitOverflow", "SELECT a FROM events LIMIT ^9223372036854775808", kSyntax, 19,
                   "LIMIT 9223372036854775808 is out of range"},
        RejectCase{"LimitHuge", "SELECT a FROM events LIMIT ^123456789012345678901234567890",
                   kSyntax, 30, "LIMIT 123456789012345678901234567890 is out of range"},
        RejectCase{"WhereAfterLimit", "SELECT a FROM events LIMIT 5 ^WHERE a = 1", kSyntax, 5,
                   "unexpected keyword WHERE; expected the end of the query"},
        RejectCase{"DuplicateWhere", "SELECT a FROM events WHERE a = 1 ^WHERE b = 2", kSyntax, 5,
                   "unexpected keyword WHERE; expected AND, LIMIT or the end of the query"},
        RejectCase{"DuplicateLimit", "SELECT a FROM events LIMIT 1 ^LIMIT 2", kSyntax, 5,
                   "unexpected keyword LIMIT; expected the end of the query"},
        RejectCase{"StrayParen", "SELECT a FROM events^)", kSyntax, 1,
                   "unexpected ')'; expected WHERE, LIMIT or the end of the query"},
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
  auto group = Parse("SELECT COUNT(*) FROM events GROUP BY region");
  ASSERT_FALSE(group.has_value());
  EXPECT_EQ(group.error().message, "GROUP BY is not supported; see docs/sql-subset.md");
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
    EXPECT_EQ(result.error().span, (SourceSpan{.offset = prefix.size(), .length = 1})) << prefix;
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
  EXPECT_EQ(string_ok->where[0].literal.text, "x\0y"sv);

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
