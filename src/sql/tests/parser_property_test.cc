// Seeded property tests for the parser and the unparser (deterministic on every platform: the PRNG
// is splitmix64 and no <random> distribution is used).
//   1. Parse never crashes, hangs or reports a span outside the input, for random token soups,
//      random bytes and random mutations of valid queries; whatever parses round-trips, and every
//      token of an accepted query is accounted for by the AST (no token is silently ignored).
//   2. Every random valid AST round-trips: Parse(ToSql(ast)) == ast and ToSql is idempotent, and
//      the same query written literal-first parses to the same AST.

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
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

// Token counts of an accepted query must match what its AST explains; a token the parser skipped
// (a silent misparse such as an operator read as an alias) breaks one of the equalities.
void CheckTokensAccountedFor(const std::string& sql, const SelectStatement& stmt) {
  auto tokens = Tokenize(sql);
  ASSERT_TRUE(tokens.has_value()) << testing::PrintToString(sql);
  std::size_t commas = 0;
  std::size_t and_count = 0;
  std::size_t comparisons = 0;
  std::size_t left_parens = 0;
  std::size_t right_parens = 0;
  std::size_t stars = 0;
  std::size_t minuses = 0;
  std::size_t strings = 0;
  std::size_t numbers = 0;
  for (const Token& token : *tokens) {
    switch (token.kind) {
      case TokenKind::kIdentifier:
        and_count += token.IsKeyword("AND") ? 1U : 0U;
        break;
      case TokenKind::kQuotedIdentifier:
      case TokenKind::kSemicolon:
      case TokenKind::kEnd:
        break;
      case TokenKind::kComma:
        ++commas;
        break;
      case TokenKind::kEqual:
      case TokenKind::kNotEqual:
      case TokenKind::kLess:
      case TokenKind::kLessEqual:
      case TokenKind::kGreater:
      case TokenKind::kGreaterEqual:
        ++comparisons;
        break;
      case TokenKind::kLeftParen:
        ++left_parens;
        break;
      case TokenKind::kRightParen:
        ++right_parens;
        break;
      case TokenKind::kStar:
        ++stars;
        break;
      case TokenKind::kMinus:
        ++minuses;
        break;
      case TokenKind::kString:
        ++strings;
        break;
      case TokenKind::kInteger:
      case TokenKind::kDecimal:
        ++numbers;
        break;
      default:
        FAIL() << "token " << ToString(token.kind) << " in accepted query "
               << testing::PrintToString(sql);
    }
  }
  std::size_t aggregates = 0;
  std::size_t count_stars = 0;
  for (const SelectItem& item : stmt.items) {
    if (const auto* agg = std::get_if<AggregateCall>(&item.expr); agg != nullptr) {
      ++aggregates;
      count_stars += agg->kind == AggKind::kCountStar ? 1U : 0U;
    }
  }
  std::size_t negatives = 0;
  std::size_t string_literals = stmt.from.kind == TableRef::Kind::kPath ? 1U : 0U;
  std::size_t number_literals = stmt.limit.has_value() ? 1U : 0U;
  for (const Comparison& cmp : stmt.where) {
    negatives += cmp.literal.negative ? 1U : 0U;
    const bool numeric =
        cmp.literal.kind == Literal::Kind::kInteger || cmp.literal.kind == Literal::Kind::kDecimal;
    (numeric ? number_literals : string_literals) += 1;
  }
  const std::string context = testing::PrintToString(sql);
  EXPECT_EQ(commas, stmt.items.empty() ? 0U : stmt.items.size() - 1) << context;
  EXPECT_EQ(and_count, stmt.where.empty() ? 0U : stmt.where.size() - 1) << context;
  EXPECT_EQ(comparisons, stmt.where.size()) << context;
  EXPECT_EQ(left_parens, aggregates) << context;
  EXPECT_EQ(right_parens, aggregates) << context;
  EXPECT_EQ(stars, (stmt.star ? 1U : 0U) + count_stars) << context;
  EXPECT_EQ(minuses, negatives) << context;
  EXPECT_EQ(strings, string_literals) << context;
  EXPECT_EQ(numbers, number_literals) << context;
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
  ASSERT_TRUE(SpanInside(stmt.from.span, sql));
  for (const SelectItem& item : stmt.items) {
    ASSERT_TRUE(SpanInside(item.span, sql));
    if (const auto* column = std::get_if<ColumnRef>(&item.expr); column != nullptr) {
      ASSERT_TRUE(SpanInside(column->span, sql));
      if (!column->quoted) {
        ASSERT_EQ(sql.substr(column->span.offset, column->span.length), column->name);
      }
    }
  }
  for (const Comparison& cmp : stmt.where) {
    ASSERT_TRUE(SpanInside(cmp.span, sql));
    ASSERT_TRUE(SpanInside(cmp.column.span, sql));
    ASSERT_TRUE(SpanInside(cmp.literal.span, sql));
  }
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
    "SELECT", "select", "FROM", "WHERE", "LIMIT", "AND", "AS", "*",  ",", "(",  ")", ";",
    "COUNT",  "SUM",    "AVG",  "MIN",   "MAX",   "=",   "<>", "!=", "<", "<=", ">", ">=",
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
    "OR",       "NOT",       "ISNULL",   "notnull",  "GROUP",
    "BY",       "ORDER",     "HAVING",   "DISTINCT", "OFFSET",
    "JOIN",     "UNION",     "WITH",     "LIKE",     "IN",
    "BETWEEN",  "CASE",      "WHEN",     "THEN",     "END",
    "IS",       "NULL",      "TRUE",     "FALSE",    "INTERVAL",
    "CAST",     "TIMESTAMP", "EXISTS",   "ALL",      "OVER",
    "FILTER",   "INTO",      "LEFT",     "COLLATE",  "lower",
    ".",        "+",         "/",        "%",        "::",
    "||",       "'open",     R"("open)", "/* open",  "-- comment\n",
    "!",        "#",         "~",        "!~",       "!=-",
    "==",       "<<",        "->",       "?",        "$1",
    "{",        "0x1F",      "1_000",    "E'x'",     "INT",
    "EXCLUDE",  "PERCENT",   "USING",    "-- c\r",   "/* /* */ */",
    "/* /* */", "\xd0\xb8",  "\x01",     "\xff",     "\xc3\x28",
    "1e",       "12abc",     R"("")",    ":",        "|",
    "[",
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
  tokens.push_back(rng.Percent(70) ? rng.Pick(kNames) : "'data/part-0.parquet'");
  if (rng.Percent(60)) {
    const std::size_t conjuncts = 1 + rng.Below(3);
    for (std::size_t i = 0; i < conjuncts; ++i) {
      tokens.emplace_back(i == 0 ? "WHERE" : "AND");
      std::vector<std::string_view> literal;
      if (rng.Percent(20)) {
        literal = {"-", rng.Pick(kLiterals).substr(0, 2)};
      } else if (rng.Percent(15)) {
        literal = {"DATE", "'2024-01-31'"};
      } else {
        literal = {rng.Pick(kLiterals)};
      }
      const std::string_view column = rng.Pick(kNames);
      const std::string_view op = rng.Pick(kOps);
      if (rng.Percent(70)) {
        tokens.push_back(column);
        tokens.push_back(op);
        tokens.insert(tokens.end(), literal.begin(), literal.end());
      } else {
        tokens.insert(tokens.end(), literal.begin(), literal.end());
        tokens.push_back(op);
        tokens.push_back(column);
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

ColumnRef RandomColumn(Rng& rng) {
  if (rng.Percent(30)) {
    return ColumnRef{.name = RandomBytes(rng, 12, true), .quoted = true};
  }
  return ColumnRef{.name = RandomName(rng), .quoted = false};
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
      literal.kind = Literal::Kind::kDate;
      literal.text = RandomBytes(rng, 12, false);
      break;
  }
  return literal;
}

SelectStatement RandomStatement(Rng& rng) {
  static constexpr auto kKinds =
      std::to_array<AggKind>({AggKind::kCountStar, AggKind::kCount, AggKind::kSum, AggKind::kAvg,
                              AggKind::kMin, AggKind::kMax});
  static constexpr auto kOps =
      std::to_array<CompareOp>({CompareOp::kEq, CompareOp::kNe, CompareOp::kLt, CompareOp::kLe,
                                CompareOp::kGt, CompareOp::kGe});
  SelectStatement stmt;
  stmt.star = rng.Percent(15);
  if (!stmt.star) {
    const std::size_t items = 1 + rng.Below(5);
    for (std::size_t i = 0; i < items; ++i) {
      SelectItem item;
      if (rng.Percent(50)) {
        const AggKind kind = rng.Pick(kKinds);
        AggregateCall agg{.kind = kind};
        if (kind != AggKind::kCountStar) {
          agg.arg = RandomColumn(rng);
        }
        item.expr = std::move(agg);
      } else {
        item.expr = RandomColumn(rng);
      }
      if (rng.Percent(35)) {
        item.alias = RandomBytes(rng, 10, true);
      }
      stmt.items.push_back(std::move(item));
    }
  }
  switch (rng.Below(3)) {
    case 0:
      stmt.from = TableRef{.kind = TableRef::Kind::kName, .name = RandomName(rng), .quoted = false};
      break;
    case 1:
      stmt.from = TableRef{
          .kind = TableRef::Kind::kName, .name = RandomBytes(rng, 12, true), .quoted = true};
      break;
    default:
      stmt.from = TableRef{.kind = TableRef::Kind::kPath, .name = RandomBytes(rng, 20, false)};
      break;
  }
  const std::size_t conjuncts = rng.Below(5);
  for (std::size_t i = 0; i < conjuncts; ++i) {
    stmt.where.push_back(Comparison{
        .column = RandomColumn(rng), .op = rng.Pick(kOps), .literal = RandomLiteral(rng)});
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

// The same statement with every comparison written literal-first and the operator mirrored.
std::string LiteralFirstSql(const SelectStatement& stmt) {
  SelectStatement head = stmt;
  head.where.clear();
  head.limit.reset();
  std::string sql = ToSql(head);
  for (std::size_t i = 0; i < stmt.where.size(); ++i) {
    // Render "column op literal" through the unparser, then swap the operands around the operator.
    SelectStatement one;
    one.star = true;
    one.from = TableRef{.kind = TableRef::Kind::kName, .name = "t"};
    one.where.push_back(stmt.where[i]);
    const std::string text = ToSql(one);
    const std::string op(ToString(stmt.where[i].op));
    const std::size_t where = text.find(" WHERE ") + 7;
    std::string column_text;
    const ColumnRef& column = stmt.where[i].column;
    if (column.quoted) {
      column_text = R"(")";
      for (const char c : column.name) {
        column_text += c == '"' ? std::string(R"("")") : std::string(1, c);
      }
      column_text += '"';
    } else {
      column_text = column.name;
    }
    const std::size_t literal_at = where + column_text.size() + 1 + op.size() + 1;
    CompareOp mirrored = stmt.where[i].op;
    switch (stmt.where[i].op) {
      case CompareOp::kLt:
        mirrored = CompareOp::kGt;
        break;
      case CompareOp::kLe:
        mirrored = CompareOp::kGe;
        break;
      case CompareOp::kGt:
        mirrored = CompareOp::kLt;
        break;
      case CompareOp::kGe:
        mirrored = CompareOp::kLe;
        break;
      case CompareOp::kEq:
      case CompareOp::kNe:
        break;
    }
    sql += i == 0 ? " WHERE " : " AND ";
    sql += text.substr(literal_at) + " " + std::string(ToString(mirrored)) + " " + column_text;
  }
  if (stmt.limit.has_value()) {
    sql += " LIMIT " + std::to_string(stmt.limit.value_or(0));
  }
  return sql;
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

      const std::string flipped = LiteralFirstSql(stmt);
      auto normalized = Parse(flipped);
      ASSERT_TRUE(normalized.has_value())
          << testing::PrintToString(flipped) << ": " << normalized.error().message;
      ASSERT_TRUE(EqualIgnoringSpans(stmt, *normalized)) << testing::PrintToString(flipped);
      ASSERT_EQ(ToSql(*normalized), sql);
    }
  }
}

}  // namespace
}  // namespace antb1::sql
