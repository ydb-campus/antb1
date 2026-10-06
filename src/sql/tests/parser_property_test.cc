// Seeded property tests for the parser and the unparser (deterministic on every platform: the PRNG
// is splitmix64 and no <random> distribution is used).
//   1. Parse never crashes, hangs or reports a span outside the input, for random token soups,
//      random bytes and random mutations of valid queries; whatever parses round-trips, and every
//      token of an accepted query is accounted for by the AST (no token is silently ignored).
//   2. Every random valid AST (random expression trees included) round-trips: Parse(ToSql(ast)) ==
//      ast and ToSql is idempotent; a comparison written literal-first normalizes like the
//      column-first one.

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "antb1/sql/ast.h"
#include "antb1/sql/lexer.h"
#include "antb1/sql/parser.h"
#include "antb1/sql/token.h"
#include "antb1/sql/unparse.h"

namespace antb1::sql {
namespace {

using namespace std::string_view_literals;

class Rng {
 public:
  explicit Rng(std::uint64_t seed) : state_(seed) {}

  std::uint64_t Next() {
    state_ += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = state_;
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31U);
  }
  std::size_t Below(std::size_t n) { return Next() % n; }
  bool Percent(std::size_t p) { return Below(100) < p; }
  template <typename Container>
  const auto& Pick(const Container& items) {
    return items[Below(items.size())];
  }

 private:
  std::uint64_t state_;
};

constexpr std::array<std::uint64_t, 4> kSeeds = {1, 42, 20240131, 0xA17B1};

// ---- invariants -------------------------------------------------------------------------------

bool SpanInside(SourceSpan span, std::string_view sql) {
  return span.offset <= sql.size() && span.length <= sql.size() - span.offset;
}

// What an AST explains of its query's tokens.
struct Counts {
  std::size_t commas = 0;
  std::size_t and_count = 0;
  std::size_t or_count = 0;
  std::size_t comparisons = 0;  // comparison operator tokens
  std::size_t parens = 0;       // '(' that belong to a call, an IN list or EXTRACT
  std::size_t stars = 0;
  std::size_t minuses = 0;
  std::size_t pluses = 0;
  std::size_t slashes = 0;
  std::size_t percents = 0;
  std::size_t integer_divides = 0;
  std::size_t strings = 0;
  std::size_t numbers = 0;
  std::size_t casts = 0;  // CAST( / TRY_CAST( or '::'
  std::size_t dots = 0;   // '.' of a qualified name
};

std::size_t Separators(std::size_t n) { return n == 0 ? 0U : n - 1; }

void Count(const Expr& expr, Counts& c);

void CountAll(const std::vector<Expr>& exprs, Counts& c) {
  c.commas += Separators(exprs.size());
  for (const Expr& e : exprs) {
    Count(e, c);
  }
}

struct CountOf {
  Counts& c;
  void operator()(const ColumnRef& column) const { c.dots += column.qualifier.empty() ? 0U : 1U; }
  void operator()(const Literal& lit) const {
    c.minuses += lit.negative ? 1U : 0U;
    const bool numeric = lit.kind == Literal::Kind::kInteger || lit.kind == Literal::Kind::kDecimal;
    (numeric ? c.numbers : c.strings) += 1;
  }
  void operator()(const AggregateCall& agg) const {
    ++c.parens;
    c.stars += agg.kind == AggKind::kCountStar ? 1U : 0U;
    if (agg.arg.has_value()) {
      Count(**agg.arg, c);
    }
  }
  void operator()(const UnaryExpr& unary) const {
    c.minuses += unary.op == UnaryOp::kNegate ? 1U : 0U;
    Count(*unary.operand, c);
  }
  void operator()(const BinaryExpr& binary) const {
    switch (binary.op) {
      case BinaryOp::kAdd:
        ++c.pluses;
        break;
      case BinaryOp::kSubtract:
        ++c.minuses;
        break;
      case BinaryOp::kMultiply:
        ++c.stars;
        break;
      case BinaryOp::kDivide:
        ++c.slashes;
        break;
      case BinaryOp::kIntegerDivide:
        ++c.integer_divides;
        break;
      case BinaryOp::kModulo:
        ++c.percents;
        break;
      case BinaryOp::kAnd:
        ++c.and_count;
        break;
      case BinaryOp::kOr:
        ++c.or_count;
        break;
      default:
        ++c.comparisons;
        break;
    }
    Count(*binary.left, c);
    Count(*binary.right, c);
  }
  void operator()(const LikeExpr& like) const {
    Count(*like.operand, c);
    Count(*like.pattern, c);
  }
  void operator()(const InExpr& in) const {
    ++c.parens;
    Count(*in.operand, c);
    CountAll(in.list, c);
  }
  // BETWEEN's own AND is an AND token.
  void operator()(const BetweenExpr& between) const {
    ++c.and_count;
    Count(*between.operand, c);
    Count(*between.low, c);
    Count(*between.high, c);
  }
  void operator()(const FunctionCall& call) const {
    ++c.parens;
    CountAll(call.args, c);
  }
  void operator()(const CaseExpr& e) const {
    if (e.operand.has_value()) {
      Count(**e.operand, c);
    }
    for (const CaseBranch& branch : e.branches) {
      Count(*branch.when, c);
      Count(*branch.then, c);
    }
    if (e.otherwise.has_value()) {
      Count(**e.otherwise, c);
    }
  }
  void operator()(const ExtractExpr& e) const {
    ++c.parens;
    Count(*e.source, c);
  }
  // The parentheses of CAST( are not counted: x::T has none. The type's parameters have theirs.
  void operator()(const CastExpr& cast) const {
    ++c.casts;
    c.numbers += cast.type_params.size();
    c.commas += Separators(cast.type_params.size());
    c.parens += cast.type_params.empty() ? 0U : 1U;
    Count(*cast.operand, c);
  }
};

void Count(const Expr& expr, Counts& c) {
  std::visit(CountOf{.c = c}, static_cast<const ExprNode&>(expr));
}

// Token counts of an accepted query must match what its AST explains; a token the parser skipped
// (a silent misparse such as an operator read as an alias) breaks one of the equalities.
// Parentheses that only group are not in the AST: they come in pairs, on top of the explained ones.
void CheckTokensAccountedFor(const std::string& sql, const SelectStatement& stmt) {
  auto tokens = Tokenize(sql);
  ASSERT_TRUE(tokens.has_value()) << testing::PrintToString(sql);
  Counts seen;
  std::size_t left_parens = 0;
  std::size_t right_parens = 0;
  for (std::size_t i = 0; i < tokens->size(); ++i) {
    const Token& token = (*tokens)[i];
    switch (token.kind) {
      case TokenKind::kIdentifier:
        seen.and_count += token.IsKeyword("AND") ? 1U : 0U;
        seen.or_count += token.IsKeyword("OR") ? 1U : 0U;
        // A name followed by '(' is a call or a cast.
        seen.casts += (token.IsKeyword("CAST") || token.IsKeyword("TRY_CAST")) &&
                              i + 1 < tokens->size() &&
                              (*tokens)[i + 1].kind == TokenKind::kLeftParen
                          ? 1U
                          : 0U;
        break;
      case TokenKind::kDoubleColon:
        ++seen.casts;
        break;
      case TokenKind::kDot:
        ++seen.dots;
        break;
      case TokenKind::kQuotedIdentifier:
      case TokenKind::kSemicolon:
      case TokenKind::kEnd:
        break;
      case TokenKind::kComma:
        ++seen.commas;
        break;
      case TokenKind::kEqual:
      case TokenKind::kNotEqual:
      case TokenKind::kLess:
      case TokenKind::kLessEqual:
      case TokenKind::kGreater:
      case TokenKind::kGreaterEqual:
        ++seen.comparisons;
        break;
      case TokenKind::kLeftParen:
        ++left_parens;
        break;
      case TokenKind::kRightParen:
        ++right_parens;
        break;
      case TokenKind::kStar:
        ++seen.stars;
        break;
      case TokenKind::kMinus:
        ++seen.minuses;
        break;
      case TokenKind::kPlus:
        ++seen.pluses;
        break;
      case TokenKind::kSlash:
        ++seen.slashes;
        break;
      case TokenKind::kPercent:
        ++seen.percents;
        break;
      case TokenKind::kOperator:
        ASSERT_EQ(token.text, "//")
            << "token " << token.text << " in accepted query " << testing::PrintToString(sql);
        ++seen.integer_divides;
        break;
      case TokenKind::kString:
        ++seen.strings;
        break;
      case TokenKind::kInteger:
      case TokenKind::kDecimal:
        ++seen.numbers;
        break;
      default:
        FAIL() << "token " << ToString(token.kind) << " in accepted query "
               << testing::PrintToString(sql);
    }
  }
  Counts want;
  want.stars = stmt.star ? 1U : 0U;
  want.numbers = (stmt.limit.has_value() ? 1U : 0U) + (stmt.offset.has_value() ? 1U : 0U);
  want.commas = Separators(stmt.items.size()) + Separators(stmt.order_by.size());
  want.and_count = Separators(stmt.where.size()) + Separators(stmt.having.size());
  for (const SelectItem& item : stmt.items) {
    Count(item.expr, want);
  }
  // A path, a string alias (its last byte a quote), a comma, and ON's AND chain.
  for (const FromItem& item : stmt.from) {
    want.strings += item.table.kind == TableRef::Kind::kPath ? 1U : 0U;
    want.strings += item.alias.has_value() && item.alias_span.length > 0 &&
                            sql[item.alias_span.offset + item.alias_span.length - 1] == '\''
                        ? 1U
                        : 0U;
    want.commas += item.connector == Connector::kComma ? 1U : 0U;
    want.and_count += Separators(item.on.size());
    for (const Expr& e : item.on) {
      Count(e, want);
    }
  }
  for (const Expr& e : stmt.where) {
    Count(e, want);
  }
  CountAll(stmt.group_by, want);
  for (const Expr& e : stmt.having) {
    Count(e, want);
  }
  for (const OrderItem& item : stmt.order_by) {
    Count(item.expr, want);
  }
  const std::string context = testing::PrintToString(sql);
  EXPECT_EQ(seen.commas, want.commas) << context;
  EXPECT_EQ(seen.and_count, want.and_count) << context;
  EXPECT_EQ(seen.or_count, want.or_count) << context;
  EXPECT_EQ(seen.comparisons, want.comparisons) << context;
  EXPECT_EQ(left_parens, right_parens) << context;
  EXPECT_GE(left_parens, want.parens) << context;
  EXPECT_EQ(seen.stars, want.stars) << context;
  EXPECT_EQ(seen.minuses, want.minuses) << context;
  EXPECT_EQ(seen.pluses, want.pluses) << context;
  EXPECT_EQ(seen.slashes, want.slashes) << context;
  EXPECT_EQ(seen.percents, want.percents) << context;
  EXPECT_EQ(seen.integer_divides, want.integer_divides) << context;
  EXPECT_EQ(seen.strings, want.strings) << context;
  EXPECT_EQ(seen.numbers, want.numbers) << context;
  EXPECT_EQ(seen.casts, want.casts) << context;
  EXPECT_EQ(seen.dots, want.dots) << context;
}

// Every span of the expression lies inside the query. A column's span runs from its qualifier (when
// it has one) to its name.
void CheckSpans(const Expr& expr, const std::string& sql) {
  ASSERT_TRUE(SpanInside(expr.span(), sql)) << testing::PrintToString(sql);
  if (const auto* column = std::get_if<ColumnRef>(&expr)) {
    const std::string text = sql.substr(column->span.offset, column->span.length);
    if (!column->quoted) {
      ASSERT_TRUE(text.ends_with(column->name)) << text;
    }
    if (column->qualifier.empty() && !column->quoted) {
      ASSERT_EQ(text, column->name);
    }
    if (!column->qualifier.empty() && !column->qualifier_quoted) {
      ASSERT_TRUE(text.starts_with(column->qualifier)) << text;
    }
  }
  if (const auto* binary = std::get_if<BinaryExpr>(&expr)) {
    ASSERT_TRUE(SpanInside(binary->op_span, sql));
    CheckSpans(*binary->left, sql);
    CheckSpans(*binary->right, sql);
  } else if (const auto* unary = std::get_if<UnaryExpr>(&expr)) {
    CheckSpans(*unary->operand, sql);
  } else if (const auto* like = std::get_if<LikeExpr>(&expr)) {
    CheckSpans(*like->operand, sql);
    CheckSpans(*like->pattern, sql);
  } else if (const auto* in = std::get_if<InExpr>(&expr)) {
    CheckSpans(*in->operand, sql);
    for (const Expr& value : in->list) {
      CheckSpans(value, sql);
    }
  } else if (const auto* call = std::get_if<FunctionCall>(&expr)) {
    for (const Expr& arg : call->args) {
      CheckSpans(arg, sql);
    }
  } else if (const auto* agg = std::get_if<AggregateCall>(&expr); agg != nullptr && agg->arg) {
    CheckSpans(**agg->arg, sql);
  } else if (const auto* cast = std::get_if<CastExpr>(&expr)) {
    ASSERT_TRUE(SpanInside(cast->op_span, sql));
    ASSERT_TRUE(SpanInside(cast->type_span, sql));
    std::string op = sql.substr(cast->op_span.offset, cast->op_span.length);
    std::string type = sql.substr(cast->type_span.offset, cast->type_span.length);
    for (std::string* text : {&op, &type}) {
      std::ranges::transform(*text, text->begin(),
                             [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    }
    if (cast->try_cast) {
      ASSERT_EQ(op, "TRY_CAST");
    } else {
      ASSERT_TRUE(op == "CAST" || op == "::") << op;
    }
    ASSERT_TRUE(type.starts_with(cast->type)) << type;
    CheckSpans(*cast->operand, sql);
  }
}

// Checks the invariants of one Parse call (fatal gtest failures on a violation).
void CheckParse(const std::string& sql) {
  auto result = Parse(sql);
  if (!result) {
    const ParseError& error = result.error();
    ASSERT_TRUE(SpanInside(error.span, sql))
        << "span outside input for: " << testing::PrintToString(sql);
    ASSERT_FALSE(error.message.empty()) << testing::PrintToString(sql);
    if (error.kind == ParseError::Kind::kUnsupported) {
      ASSERT_TRUE(error.message.ends_with("; see docs/sql-subset.md")) << error.message;
    }
    return;
  }
  const SelectStatement& stmt = *result;
  ASSERT_TRUE(SpanInside(stmt.span, sql)) << testing::PrintToString(sql);
  ASSERT_FALSE(stmt.from.empty());
  for (const FromItem& item : stmt.from) {
    const bool first = &item == &stmt.from.front();
    ASSERT_EQ(item.connector == Connector::kFirst, first) << testing::PrintToString(sql);
    ASSERT_TRUE(SpanInside(item.span, sql));
    ASSERT_TRUE(SpanInside(item.table.span, sql));
    ASSERT_EQ(item.span.offset, item.table.span.offset);
    ASSERT_TRUE(SpanInside(item.connector_span, sql));
    ASSERT_EQ(item.connector_span.length > 0, !first);
    ASSERT_TRUE(SpanInside(item.alias_span, sql));
    ASSERT_EQ(item.alias_span.length > 0, item.alias.has_value());
    const bool joined = item.connector == Connector::kInner || item.connector == Connector::kLeft;
    ASSERT_EQ(!item.on.empty(), joined) << testing::PrintToString(sql);
    ASSERT_EQ(item.on_span.length > 0, joined);
    ASSERT_TRUE(SpanInside(item.on_span, sql));
    if (joined) {
      std::string on = sql.substr(item.on_span.offset, 2);
      std::ranges::transform(on, on.begin(),
                             [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
      ASSERT_EQ(on, "ON") << testing::PrintToString(sql);
    }
    for (const Expr& e : item.on) {
      ASSERT_NO_FATAL_FAILURE(CheckSpans(e, sql));
    }
  }
  for (const SelectItem& item : stmt.items) {
    ASSERT_TRUE(SpanInside(item.span, sql));
    ASSERT_NO_FATAL_FAILURE(CheckSpans(item.expr, sql));
  }
  for (const auto* clause : {&stmt.where, &stmt.group_by, &stmt.having}) {
    for (const Expr& e : *clause) {
      ASSERT_NO_FATAL_FAILURE(CheckSpans(e, sql));
    }
  }
  for (const OrderItem& item : stmt.order_by) {
    ASSERT_TRUE(SpanInside(item.span, sql));
    ASSERT_NO_FATAL_FAILURE(CheckSpans(item.expr, sql));
  }
  ASSERT_TRUE(SpanInside(stmt.group_by_span, sql));
  ASSERT_TRUE(SpanInside(stmt.having_span, sql));
  ASSERT_TRUE(SpanInside(stmt.order_by_span, sql));
  ASSERT_TRUE(SpanInside(stmt.offset_span, sql));
  ASSERT_NO_FATAL_FAILURE(CheckTokensAccountedFor(sql, stmt));
  const std::string canonical = ToSql(stmt);
  auto again = Parse(canonical);
  ASSERT_TRUE(again.has_value()) << testing::PrintToString(sql) << " -> "
                                 << testing::PrintToString(canonical) << ": "
                                 << again.error().message;
  ASSERT_TRUE(EqualIgnoringSpans(stmt, *again)) << testing::PrintToString(canonical);
  ASSERT_EQ(ToSql(*again), canonical);
}

// ---- random token soup ------------------------------------------------------------------------

constexpr auto kStructure = std::to_array<std::string_view>({
    "SELECT", "select", "FROM",   "WHERE", "LIMIT", "AND",  "AS",       "*",   ",",
    "(",      ")",      ";",      "COUNT", "SUM",   "AVG",  "MIN",      "MAX", "=",
    "<>",     "!=",     "<",      "<=",    ">",     ">=",   "GROUP",    "BY",  "ORDER",
    "ASC",    "desc",   "OFFSET", "NULLS", "FIRST", "last", "DISTINCT",
});
constexpr auto kOperands = std::to_array<std::string_view>({
    "events",
    "user_id",
    "amount",
    "Region",
    "date",
    "count",
    R"("Quoted Name")",
    R"("a""b")",
    "'data/part-0.parquet'",
    "'it''s'",
    "''",
    "'2024-01-31'",
    "DATE",
    "0",
    "1",
    "42",
    "007",
    "9223372036854775807",
    "9223372036854775808",
    "1.5",
    ".5",
    "5.",
    "1e3",
    "2.5E-3",
    "-",
});
constexpr auto kOther = std::to_array<std::string_view>({
    "OR",          "NOT",         "ISNULL",    "notnull", "HAVING",
    "JOIN",        "UNION",       "WITH",      "LIKE",    "IN",
    "BETWEEN",     "CASE",        "WHEN",      "THEN",    "END",
    "IS",          "NULL",        "TRUE",      "FALSE",   "INTERVAL",
    "CAST",        "TIMESTAMPTZ", "EXISTS",    "ALL",     "OVER",
    "FILTER",      "INTO",        "LEFT",      "COLLATE", "lower",
    ".",           "+",           "/",         "%",       "::",
    "||",          "'open",       R"("open)",  "/* open", "-- comment\n",
    "!",           "#",           "~",         "!~",      "!=-",
    "==",          "<<",          "->",        "?",       "$1",
    "{",           "0x1F",        "1_000",     "E'x'",    "INT",
    "EXCLUDE",     "PERCENT",     "USING",     "-- c\r",  "/* /* */ */",
    "/* /* */",    "\xd0\xb8",    "\x01",      "\xff",    "\xc3\x28",
    "1e",          "12abc",       R"("")",     ":",       "|",
    "[",           "TRY_CAST",    "PRECISION", "CROSS",   "INNER",
    "OUTER",       "ON",          "semi",      "ANTI",    "asof",
    "NATURAL",     "RIGHT",       "LATERAL",   "only",    "PIVOT",
    "TABLESAMPLE", "at",          "GLOB",      "FULL",    "POSITIONAL",
});
constexpr auto kSeparators = std::to_array<std::string_view>(
    {" ", " ", " ", "", "\n", "\t", "/**/", "--\n", "\r", "--\r", "/*/**/*/"});

std::string_view RandomToken(Rng& rng) {
  const std::size_t bucket = rng.Below(10);
  if (bucket < 4) {
    return rng.Pick(kStructure);
  }
  if (bucket < 8) {
    return rng.Pick(kOperands);
  }
  return rng.Pick(kOther);
}

// Tokens of a query that follows the grammar (names and literals drawn from small pools).
std::vector<std::string_view> Skeleton(Rng& rng) {
  static constexpr auto kNames = std::to_array<std::string_view>(
      {"events", "user_id", "amount", "Region", "date", "count", R"("Quoted Name")", R"("a""b")"});
  static constexpr auto kAggregates =
      std::to_array<std::string_view>({"COUNT", "count", "SUM", "AVG", "MIN", "MAX"});
  static constexpr auto kOps =
      std::to_array<std::string_view>({"=", "<>", "!=", "<", "<=", ">", ">="});
  static constexpr auto kLiterals = std::to_array<std::string_view>(
      {"0", "42", "007", "1.5", ".5", "5.", "1e3", "'it''s'", "''", "'north'"});
  std::vector<std::string_view> tokens = {"SELECT"};
  if (rng.Percent(20)) {
    tokens.emplace_back("*");
  } else {
    const std::size_t items = 1 + rng.Below(3);
    for (std::size_t i = 0; i < items; ++i) {
      if (i > 0) {
        tokens.emplace_back(",");
      }
      if (rng.Percent(50)) {
        const std::string_view agg = rng.Pick(kAggregates);
        tokens.insert(tokens.end(),
                      {agg, "(", agg.size() == 5 && rng.Percent(50) ? "*" : rng.Pick(kNames), ")"});
      } else if (rng.Percent(10)) {
        tokens.insert(tokens.end(), {"CAST", "(", rng.Pick(kNames), "AS", "DECIMAL", "(", "15", ",",
                                     "2", ")", ")"});
      } else {
        tokens.push_back(rng.Pick(kNames));
      }
      if (rng.Percent(30)) {
        if (rng.Percent(50)) {
          tokens.emplace_back("AS");
        }
        tokens.push_back(rng.Pick(kNames));
      }
    }
  }
  tokens.emplace_back("FROM");
  // One item, or a list joined by commas, CROSS JOIN, JOIN ... ON and LEFT [OUTER] JOIN ... ON.
  static constexpr auto kAliases =
      std::to_array<std::string_view>({"e", "x_", R"("A b")", "over", "'s'"});
  const std::size_t items = rng.Percent(70) ? 1 : 2 + rng.Below(3);
  for (std::size_t i = 0; i < items; ++i) {
    bool on = false;
    if (i > 0) {
      const std::size_t kind = rng.Below(5);
      if (kind == 0) {
        tokens.emplace_back(",");
      } else if (kind == 1) {
        tokens.insert(tokens.end(), {"CROSS", "JOIN"});
      } else {
        if (kind == 3) {
          tokens.emplace_back("INNER");
        } else if (kind == 4) {
          tokens.emplace_back("LEFT");
          if (rng.Percent(50)) {
            tokens.emplace_back("OUTER");
          }
        }
        tokens.emplace_back("JOIN");
        on = true;
      }
    }
    tokens.push_back(rng.Percent(70) ? rng.Pick(kNames) : "'data/part-0.parquet'");
    if (rng.Percent(30)) {
      const std::string_view alias = rng.Pick(kAliases);
      if (alias.starts_with('\'') || rng.Percent(50)) {
        tokens.emplace_back("AS");
      }
      tokens.push_back(alias);
    }
    if (on) {
      tokens.emplace_back("ON");
      for (std::size_t j = 1 + rng.Below(2); j > 0; --j) {
        tokens.insert(tokens.end(), {"e", ".", rng.Pick(kNames), "=", "x_", ".", "amount"});
        if (j > 1) {
          tokens.emplace_back("AND");
        }
      }
    }
  }
  if (rng.Percent(60)) {
    const std::size_t conjuncts = 1 + rng.Below(3);
    for (std::size_t i = 0; i < conjuncts; ++i) {
      tokens.emplace_back(i == 0 ? "WHERE" : "AND");
      std::vector<std::string_view> literal;
      if (rng.Percent(20)) {
        literal = {"-", rng.Pick(kLiterals).substr(0, 2)};
      } else if (rng.Percent(15)) {
        literal = {"DATE", "'2024-01-31'"};
      } else if (rng.Percent(10)) {
        literal = rng.Percent(50) ? std::vector<std::string_view>{"CAST", "(",    "'2024-01-31'",
                                                                  "AS",   "DATE", ")"}
                                  : std::vector<std::string_view>{"'2024-01-31'", "::", "date"};
      } else if (rng.Percent(10)) {
        literal = {"TIMESTAMP", "'2024-01-31 12:34:56.5'"};
      } else {
        literal = {rng.Pick(kLiterals)};
      }
      std::vector<std::string_view> column = {rng.Pick(kNames)};
      if (rng.Percent(20)) {
        column.insert(column.begin(), {rng.Pick(kNames), "."});
      }
      const std::string_view op = rng.Pick(kOps);
      if (rng.Percent(70)) {
        tokens.insert(tokens.end(), column.begin(), column.end());
        tokens.push_back(op);
        tokens.insert(tokens.end(), literal.begin(), literal.end());
      } else {
        tokens.insert(tokens.end(), literal.begin(), literal.end());
        tokens.push_back(op);
        tokens.insert(tokens.end(), column.begin(), column.end());
      }
    }
  }
  if (rng.Percent(40)) {
    tokens.emplace_back("LIMIT");
    tokens.emplace_back(rng.Percent(90) ? "10" : "9223372036854775808");
  }
  if (rng.Percent(30)) {
    tokens.emplace_back(";");
  }
  return tokens;
}

// A grammar-shaped query with token-level noise (70%), or plain token soup (30%).
std::string TokenSoup(Rng& rng) {
  std::vector<std::string_view> tokens;
  if (rng.Percent(70)) {
    for (const std::string_view token : Skeleton(rng)) {
      const std::size_t roll = rng.Below(100);
      if (roll < 4) {
        continue;  // drop
      }
      if (roll < 10) {
        tokens.push_back(RandomToken(rng));  // replace
        continue;
      }
      if (roll < 14) {
        tokens.push_back(RandomToken(rng));  // insert
      }
      tokens.push_back(token);
    }
  } else {
    if (rng.Percent(80)) {
      tokens.emplace_back("SELECT");
    }
    const std::size_t count = rng.Below(24);
    for (std::size_t i = 0; i < count; ++i) {
      tokens.push_back(RandomToken(rng));
    }
  }
  std::string sql;
  for (const std::string_view token : tokens) {
    sql += sql.empty() ? " " : rng.Pick(kSeparators);
    sql += token;
  }
  if (rng.Percent(5)) {
    sql += "\0"sv;
  }
  return sql;
}

TEST(ParserPropertyTest, RandomTokenSoupNeverBreaksTheParser) {
  std::size_t parsed = 0;
  for (const std::uint64_t seed : kSeeds) {
    Rng rng(seed);
    for (int i = 0; i < 2000; ++i) {
      const std::string sql = TokenSoup(rng);
      ASSERT_NO_FATAL_FAILURE(CheckParse(sql)) << "seed " << seed << " iteration " << i;
      if (Parse(sql).has_value()) {
        ++parsed;
      }
    }
  }
  EXPECT_GT(parsed, 500U);  // the generator reaches valid queries too
}

TEST(ParserPropertyTest, RandomBytesNeverBreakTheParser) {
  for (const std::uint64_t seed : kSeeds) {
    Rng rng(seed);
    for (int i = 0; i < 500; ++i) {
      std::string sql = rng.Percent(50) ? "SELECT " : "";
      const std::size_t size = rng.Below(64);
      for (std::size_t j = 0; j < size; ++j) {
        sql.push_back(static_cast<char>(rng.Below(256)));
      }
      ASSERT_NO_FATAL_FAILURE(CheckParse(sql)) << "seed " << seed << " iteration " << i;
    }
  }
}

// ---- mutations of valid queries --------------------------------------------------------------

constexpr auto kCorpus = std::to_array<std::string_view>({
    "SELECT COUNT(*) FROM events",
    "SELECT * FROM 'data/part-0.parquet' LIMIT 10",
    "SELECT user_id, SUM(amount) AS total FROM events WHERE amount > 0 AND region = 'north'",
    R"(SELECT MIN(ts), MAX(ts) FROM "Events" WHERE ts >= DATE '2024-01-01' LIMIT 5;)",
    "SELECT AVG(price) p FROM sales WHERE -1.5 < price AND 'x' <> sku",
    "select count(user_id) from events where user_id != 7 -- trailing\n",
    "SELECT CAST(amount AS DECIMAL(15, 2)) FROM sales WHERE day >= '2024-01-31'::date AND "
    "-1::INTEGER < amount",
    "SELECT s.region, SUM(e.amount) FROM sales AS s JOIN events e ON s.id = e.sale_id AND "
    "e.amount > 0 GROUP BY s.region",
    R"(SELECT COUNT(*) FROM a, "B" b CROSS JOIN 'c.parquet' AS 'c' LEFT OUTER JOIN d ON a.k = )"
    "d.k OR d.k = 1",
    "select x.a from t x inner join u on x.a = u.b, v left join w on v.c = w.c where x.a < 3",
});

std::string Mutate(Rng& rng, std::string sql) {
  const std::size_t edits = 1 + rng.Below(4);
  for (std::size_t e = 0; e < edits; ++e) {
    const std::size_t pos = sql.empty() ? 0 : rng.Below(sql.size() + 1);
    switch (rng.Below(6)) {
      case 0:  // insert a random byte
        sql.insert(pos, 1, static_cast<char>(rng.Below(256)));
        break;
      case 1:  // delete a range
        if (!sql.empty() && pos < sql.size()) {
          sql.erase(pos, 1 + rng.Below(8));
        }
        break;
      case 2:  // insert a token
        sql.insert(pos, std::string(" ") + std::string(rng.Pick(kOther)) + " ");
        break;
      case 3:  // insert a structural token
        sql.insert(pos, std::string(" ") + std::string(rng.Pick(kStructure)) + " ");
        break;
      case 4:  // duplicate a range
        if (pos < sql.size()) {
          sql.insert(pos, sql.substr(pos, 1 + rng.Below(12)));
        }
        break;
      default:  // truncate
        sql.resize(pos);
        break;
    }
  }
  return sql;
}

TEST(ParserPropertyTest, MutatedQueriesNeverBreakTheParser) {
  for (const std::string_view sql : kCorpus) {
    ASSERT_TRUE(Parse(sql).has_value()) << sql;
  }
  for (const std::uint64_t seed : kSeeds) {
    Rng rng(seed);
    for (int i = 0; i < 2000; ++i) {
      const std::string sql = Mutate(rng, std::string(rng.Pick(kCorpus)));
      ASSERT_NO_FATAL_FAILURE(CheckParse(sql)) << "seed " << seed << " iteration " << i;
    }
  }
}

// ---- random valid ASTs -------------------------------------------------------------------------

std::string RandomBytes(Rng& rng, std::size_t max_size, bool non_empty) {
  static constexpr std::string_view kAlphabet = "aZ_09 '\"-/*;.,()\n\t";
  std::string out;
  const std::size_t size = (non_empty ? 1 : 0) + rng.Below(max_size);
  for (std::size_t i = 0; i < size; ++i) {
    out.push_back(rng.Percent(70) ? rng.Pick(kAlphabet) : static_cast<char>(rng.Below(256)));
  }
  return out;
}

// An unquoted identifier the parser accepts as a name (reserved words are rejected by retrying).
std::string RandomName(Rng& rng) {
  static constexpr auto kStems = std::to_array<std::string_view>(
      {"a", "b", "user_id", "amount", "Region", "ts", "count", "sum", "date", "x_", "_t", "e"});
  static constexpr std::string_view kTail = "abcdefghijklmnopqrstuvwxyzABCXYZ_0123456789";
  while (true) {
    std::string name(rng.Pick(kStems));
    const std::size_t tail = rng.Below(4);
    for (std::size_t i = 0; i < tail; ++i) {
      name.push_back(rng.Pick(kTail));
    }
    auto probe = Parse("SELECT x AS " + name + " FROM t");
    if (probe.has_value() && probe->items.size() == 1 &&
        probe->items[0].alias == std::optional<std::string>(name)) {
      return name;
    }
  }
}

// An unquoted name or any non-empty quoted text (the lexer makes no empty quoted identifier).
std::pair<std::string, bool> RandomNameOrQuoted(Rng& rng) {
  if (rng.Percent(30)) {
    return {RandomBytes(rng, 12, true), true};
  }
  return {RandomName(rng), false};
}

// A column, qualified by a name 20% of the time.
ColumnRef RandomColumn(Rng& rng) {
  auto [name, quoted] = RandomNameOrQuoted(rng);
  ColumnRef column{.name = std::move(name), .quoted = quoted};
  if (rng.Percent(20)) {
    std::tie(column.qualifier, column.qualifier_quoted) = RandomNameOrQuoted(rng);
  }
  return column;
}

std::string RandomDigits(Rng& rng, std::size_t max_size) {
  std::string digits;
  const std::size_t size = 1 + rng.Below(max_size);
  for (std::size_t i = 0; i < size; ++i) {
    digits.push_back(static_cast<char>('0' + rng.Below(10)));
  }
  return digits;
}

Literal RandomLiteral(Rng& rng) {
  Literal literal;
  switch (rng.Below(5)) {
    case 0:
      literal.kind = Literal::Kind::kInteger;
      literal.text = RandomDigits(rng, 25);
      literal.negative = rng.Percent(40);
      break;
    case 1: {
      literal.kind = Literal::Kind::kDecimal;
      literal.negative = rng.Percent(40);
      switch (rng.Below(4)) {
        case 0:
          literal.text = RandomDigits(rng, 6) + "." + RandomDigits(rng, 6);
          break;
        case 1:
          literal.text = "." + RandomDigits(rng, 6);
          break;
        case 2:
          literal.text = RandomDigits(rng, 6) + ".";
          break;
        default:
          literal.text = RandomDigits(rng, 3) + (rng.Percent(50) ? "e" : "E") +
                         (rng.Percent(50) ? "-" : "") + RandomDigits(rng, 3);
          break;
      }
      break;
    }
    case 2:
    case 3:
      literal.kind = Literal::Kind::kString;
      literal.text = RandomBytes(rng, 16, false);
      break;
    default:
      literal.kind = rng.Percent(50) ? Literal::Kind::kDate : Literal::Kind::kTimestamp;
      literal.text = RandomBytes(rng, 12, false);
      break;
  }
  return literal;
}

// A random expression of at most `depth` levels; aggregates only where `aggregates` allows them
// (never nested).
Expr RandomExpr(Rng& rng, std::size_t depth, bool aggregates) {
  static constexpr auto kKinds =
      std::to_array<AggKind>({AggKind::kCountStar, AggKind::kCount, AggKind::kSum, AggKind::kAvg,
                              AggKind::kMin, AggKind::kMax});
  static constexpr auto kBinary = std::to_array<BinaryOp>(
      {BinaryOp::kAdd, BinaryOp::kSubtract, BinaryOp::kMultiply, BinaryOp::kDivide,
       BinaryOp::kIntegerDivide, BinaryOp::kModulo, BinaryOp::kEq, BinaryOp::kNe, BinaryOp::kLt,
       BinaryOp::kLe, BinaryOp::kGt, BinaryOp::kGe, BinaryOp::kAnd, BinaryOp::kOr});
  static constexpr auto kFunctions =
      std::to_array<std::string_view>({"strlen", "regexp_replace", "f", "date_trunc", "abs"});
  const auto sub = [&](bool agg) { return Box<Expr>(RandomExpr(rng, depth - 1, agg)); };
  static constexpr auto kTypes =
      std::to_array<std::string_view>({"DATE", "INTEGER", "BIGINT", "VARCHAR", "DECIMAL", "T_1"});
  const std::size_t roll = depth == 0 ? rng.Below(2) : rng.Below(12);
  switch (roll) {
    case 0:
      return Expr(RandomColumn(rng));
    case 1:
      return Expr(RandomLiteral(rng));
    case 2: {
      if (!aggregates) {
        return Expr(RandomColumn(rng));
      }
      const AggKind kind = rng.Pick(kKinds);
      AggregateCall agg{.kind = kind, .arg = {}, .distinct = false, .span = {}};
      if (kind != AggKind::kCountStar) {
        agg.arg.emplace(rng.Percent(70) ? Expr(RandomColumn(rng))
                                        : RandomExpr(rng, depth - 1, false));
        agg.distinct = kind == AggKind::kCount && rng.Percent(30);
      }
      return Expr(std::move(agg));
    }
    case 3:
    case 4:
    case 5:
      return Expr(BinaryExpr{.op = rng.Pick(kBinary),
                             .left = sub(aggregates),
                             .right = sub(aggregates),
                             .op_span = {},
                             .span = {}});
    case 6:
      return Expr(UnaryExpr{.op = rng.Percent(50) ? UnaryOp::kNegate : UnaryOp::kNot,
                            .operand = sub(aggregates),
                            .op_span = {},
                            .span = {}});
    case 7:
      if (rng.Percent(30)) {
        return Expr(BetweenExpr{.operand = sub(aggregates),
                                .low = sub(aggregates),
                                .high = sub(aggregates),
                                .negated = rng.Percent(40),
                                .op_span = {},
                                .span = {}});
      }
      return Expr(LikeExpr{.operand = sub(aggregates),
                           .pattern = sub(aggregates),
                           .negated = rng.Percent(40),
                           .op_span = {},
                           .span = {}});
    case 8: {
      InExpr in{.operand = sub(aggregates),
                .list = {},
                .negated = rng.Percent(40),
                .op_span = {},
                .span = {}};
      for (std::size_t n = 1 + rng.Below(3); n > 0; --n) {
        in.list.push_back(RandomExpr(rng, depth - 1, aggregates));
      }
      return Expr(std::move(in));
    }
    case 10: {
      CastExpr cast{.operand = sub(aggregates),
                    .type = std::string(rng.Pick(kTypes)),
                    .type_params = {},
                    .try_cast = rng.Percent(20),
                    .op_span = {},
                    .type_span = {},
                    .span = {}};
      for (std::size_t n = rng.Below(3); n > 0; --n) {
        cast.type_params.push_back(RandomDigits(rng, 3));
      }
      return Expr(std::move(cast));
    }
    case 9: {
      FunctionCall call{.name = std::string(rng.Pick(kFunctions)),
                        .quoted = rng.Percent(20),
                        .args = {},
                        .name_span = {},
                        .span = {}};
      for (std::size_t n = rng.Below(4); n > 0; --n) {
        call.args.push_back(RandomExpr(rng, depth - 1, aggregates));
      }
      return Expr(std::move(call));
    }
    default: {
      if (rng.Percent(30)) {
        return Expr(ExtractExpr{.field = rng.Percent(50) ? "minute" : "YEAR",
                                .source = sub(aggregates),
                                .field_span = {},
                                .span = {}});
      }
      CaseExpr c{.operand = {}, .branches = {}, .otherwise = {}, .span = {}};
      if (rng.Percent(30)) {
        c.operand.emplace(RandomExpr(rng, depth - 1, aggregates));
      }
      for (std::size_t n = 1 + rng.Below(2); n > 0; --n) {
        c.branches.push_back(CaseBranch{.when = sub(aggregates), .then = sub(aggregates)});
      }
      if (rng.Percent(50)) {
        c.otherwise.emplace(RandomExpr(rng, depth - 1, aggregates));
      }
      return Expr(std::move(c));
    }
  }
}

// A predicate: conjuncts, mostly simple comparisons.
std::vector<Expr> RandomPredicate(Rng& rng, bool aggregates) {
  static constexpr auto kOps =
      std::to_array<CompareOp>({CompareOp::kEq, CompareOp::kNe, CompareOp::kLt, CompareOp::kLe,
                                CompareOp::kGt, CompareOp::kGe});
  std::vector<Expr> out;
  for (std::size_t n = 1 + rng.Below(4); n > 0; --n) {
    if (rng.Percent(60)) {
      out.push_back(ToExpr(Comparison{.column = RandomColumn(rng),
                                      .op = rng.Pick(kOps),
                                      .literal = RandomLiteral(rng),
                                      .list = {},
                                      .span = {}}));
    } else {
      out.push_back(RandomExpr(rng, 3, aggregates));
    }
  }
  return out;
}

SelectStatement RandomStatement(Rng& rng) {
  static constexpr auto kNulls =
      std::to_array<NullsOrder>({NullsOrder::kDefault, NullsOrder::kFirst, NullsOrder::kLast});
  SelectStatement stmt;
  stmt.star = rng.Percent(15);
  if (!stmt.star) {
    const std::size_t items = 1 + rng.Below(5);
    for (std::size_t i = 0; i < items; ++i) {
      SelectItem item{.expr = RandomExpr(rng, 3, true), .alias = {}, .span = {}};
      if (rng.Percent(35)) {
        item.alias = RandomBytes(rng, 10, true);
      }
      stmt.items.push_back(std::move(item));
    }
  }
  // 1 to 4 FROM items, each a name, a quoted name or a path, joined by random connectors, with
  // random aliases and ON predicates.
  static constexpr auto kConnectors = std::to_array<Connector>(
      {Connector::kComma, Connector::kCross, Connector::kInner, Connector::kLeft});
  const std::size_t items = 1 + rng.Below(4);
  for (std::size_t i = 0; i < items; ++i) {
    FromItem item;
    item.connector = i == 0 ? Connector::kFirst : rng.Pick(kConnectors);
    switch (rng.Below(3)) {
      case 0:
        item.table =
            TableRef{.kind = TableRef::Kind::kName, .name = RandomName(rng), .quoted = false};
        break;
      case 1:
        item.table = TableRef{
            .kind = TableRef::Kind::kName, .name = RandomBytes(rng, 12, true), .quoted = true};
        break;
      default:
        item.table = TableRef{.kind = TableRef::Kind::kPath, .name = RandomBytes(rng, 20, false)};
        break;
    }
    if (rng.Percent(30)) {
      item.alias = RandomBytes(rng, 10, true);
    }
    if (item.connector == Connector::kInner || item.connector == Connector::kLeft) {
      item.on = RandomPredicate(rng, false);
    }
    stmt.from.push_back(std::move(item));
  }
  if (rng.Percent(60)) {
    stmt.where = RandomPredicate(rng, false);
  }
  if (rng.Percent(35)) {
    for (std::size_t n = 1 + rng.Below(3); n > 0; --n) {
      stmt.group_by.push_back(RandomExpr(rng, 2, false));
    }
  }
  if (rng.Percent(25)) {
    stmt.having = RandomPredicate(rng, true);
  }
  if (rng.Percent(35)) {
    for (std::size_t n = 1 + rng.Below(3); n > 0; --n) {
      stmt.order_by.push_back(OrderItem{.expr = RandomExpr(rng, 2, true),
                                        .descending = rng.Percent(50),
                                        .nulls = rng.Pick(kNulls),
                                        .span = {}});
    }
  }
  if (rng.Percent(20)) {
    stmt.offset = static_cast<std::int64_t>(rng.Below(1000));
  }
  if (rng.Percent(50)) {
    switch (rng.Below(4)) {
      case 0:
        stmt.limit = 0;
        break;
      case 1:
        stmt.limit = std::numeric_limits<std::int64_t>::max();
        break;
      case 2:
        stmt.limit = static_cast<std::int64_t>(rng.Below(1000));
        break;
      default:
        stmt.limit = static_cast<std::int64_t>(rng.Next() >> 1U);
        break;
    }
  }
  return stmt;
}

TEST(ParserPropertyTest, RandomValidAstsRoundTrip) {
  for (const std::uint64_t seed : kSeeds) {
    Rng rng(seed);
    for (int i = 0; i < 300; ++i) {
      const SelectStatement stmt = RandomStatement(rng);
      const std::string sql = ToSql(stmt);
      auto parsed = Parse(sql);
      ASSERT_TRUE(parsed.has_value())
          << "seed " << seed << " iteration " << i << ": " << testing::PrintToString(sql) << ": "
          << parsed.error().message;
      ASSERT_TRUE(EqualIgnoringSpans(stmt, *parsed)) << testing::PrintToString(sql);
      ASSERT_EQ(ToSql(*parsed), sql);
      ASSERT_NO_FATAL_FAILURE(CheckParse(sql));
    }
  }
}

// A comparison written literal-first has the normalized form of the column-first one.
TEST(ParserPropertyTest, LiteralFirstComparisonsNormalize) {
  static constexpr auto kOps =
      std::to_array<CompareOp>({CompareOp::kEq, CompareOp::kNe, CompareOp::kLt, CompareOp::kLe,
                                CompareOp::kGt, CompareOp::kGe});
  static constexpr auto kMirror =
      std::to_array<std::string_view>({"=", "<>", ">", ">=", "<", "<="});
  for (const std::uint64_t seed : kSeeds) {
    Rng rng(seed);
    for (int i = 0; i < 300; ++i) {
      const std::size_t op = rng.Below(kOps.size());
      const Comparison cmp{.column = RandomColumn(rng),
                           .op = kOps[op],
                           .literal = RandomLiteral(rng),
                           .list = {},
                           .span = {}};
      const Expr expr = ToExpr(cmp);
      const auto& binary = std::get<BinaryExpr>(expr);
      const std::string flipped = "SELECT * FROM t WHERE " + ToSql(*binary.right) + " " +
                                  std::string(kMirror[op]) + " " + ToSql(*binary.left);
      auto parsed = Parse(flipped);
      ASSERT_TRUE(parsed.has_value()) << testing::PrintToString(flipped);
      ASSERT_EQ(parsed->where.size(), 1U);
      const auto normalized = AsComparison(parsed->where[0]);
      ASSERT_TRUE(normalized.has_value()) << testing::PrintToString(flipped);
      EXPECT_TRUE(EqualIgnoringSpans(ToExpr(normalized.value_or(Comparison{})), ToExpr(cmp)))
          << testing::PrintToString(flipped);
    }
  }
}

}  // namespace
}  // namespace antb1::sql
