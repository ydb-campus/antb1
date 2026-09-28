#include "antb1/sql/parser.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <format>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include "antb1/common/source_span.h"
#include "antb1/sql/ast.h"
#include "antb1/sql/error.h"
#include "antb1/sql/lexer.h"
#include "antb1/sql/token.h"

// Hand-written recursive-descent parser for the grammar in docs/sql-subset.md:
//
//   statement   := query [';'] EOF
//   query       := SELECT select_list FROM table_ref [WHERE expr] [GROUP BY expr (',' expr)*]
//                  [HAVING expr] [ORDER BY order_item (',' order_item)*]
//                  [LIMIT integer] [OFFSET integer]      (LIMIT and OFFSET in either order)
//   select_list := '*' | expr [[AS] identifier] (',' expr [[AS] identifier])*
//   order_item  := expr [ASC | DESC] [NULLS (FIRST | LAST)]
//   table_ref   := identifier | quoted_identifier | string_literal
//   expr        := precedence climbing over, from loosest to tightest: OR; AND; NOT; comparisons,
//                  [NOT] LIKE and [NOT] IN (not chained); + -; * / // %; unary -; primary
//   primary     := column_ref | literal | '(' expr ')' | agg_call | name '(' [expr (',' expr)*] ')'
//                | CASE [expr] (WHEN expr THEN expr)+ [ELSE expr] END | EXTRACT '(' field FROM expr
//                ')'
//   agg_call    := COUNT '(' '*' ')' | COUNT '(' DISTINCT expr ')'
//                | (COUNT | SUM | AVG | MIN | MAX) '(' expr ')'
//   literal     := ['-'] integer | ['-'] decimal | string_literal | DATE string_literal
//
// The parser keeps expressions as written; what the engine answers is the binder's decision. The
// WHERE and HAVING predicates are split at their top-level AND chain, collected in a loop.
// Recursion is bounded: every level of an expression tree (but that chain) counts against
// kMaxDepth. Tokens are pulled lazily from the lexer (at most three tokens of lookahead), so work
// and memory stop at the first error whatever the input. Recognized SQL outside the grammar yields
// kUnsupported at its first offending token and names the construct; anything else yields kSyntax.
// A lexer error among the tokens the parser looked at wins over the parser's own verdict, which may
// have been reached on the placeholder end-of-input token.

namespace antb1::sql {
namespace {

template <typename T>
using Expected = std::expected<T, ParseError>;
using Status = std::expected<void, ParseError>;

// Where an operand is parsed; selects the error wording and whether literals are allowed.
enum class Context : std::uint8_t { kSelect, kAggregateArg, kWhere, kGroupBy, kHaving, kOrderBy };

// Binding powers of the operators (see the grammar above).
constexpr int kOrPrecedence = 1;
constexpr int kAndPrecedence = 2;
constexpr int kNotPrecedence = 3;
constexpr int kComparisonPrecedence = 4;
constexpr int kAdditivePrecedence = 5;
constexpr int kMultiplicativePrecedence = 6;
constexpr int kUnaryPrecedence = 7;

// The deepest expression tree the parser builds.
constexpr std::size_t kMaxDepth = 256;

struct Construct {
  std::string_view keyword;
  std::string_view message;
};

// Identifiers longer than every keyword below are never keywords (checked below).
constexpr std::size_t kMaxKeywordLength = 12;

// Words that cannot be unquoted column, table or alias names (sorted).
constexpr auto kReservedWords = std::to_array<std::string_view>({
    "ALL",   "AND",       "ANY",      "ARRAY",  "AS",       "ASC",   "BETWEEN", "BY",     "CASE",
    "CAST",  "COLLATE",   "CROSS",    "DESC",   "DISTINCT", "ELSE",  "END",     "EXCEPT", "EXISTS",
    "FALSE", "FETCH",     "FOR",      "FROM",   "FULL",     "GROUP", "HAVING",  "ILIKE",  "IN",
    "INNER", "INTERSECT", "INTERVAL", "INTO",   "IS",       "JOIN",  "LATERAL", "LEFT",   "LIKE",
    "LIMIT", "NATURAL",   "NOT",      "NULL",   "OFFSET",   "ON",    "OR",      "ORDER",  "OUTER",
    "OVER",  "QUALIFY",   "RIGHT",    "SELECT", "SIMILAR",  "SOME",  "TABLE",   "THEN",   "TRUE",
    "UNION", "USING",     "WHEN",     "WHERE",  "WINDOW",   "WITH",
});
static_assert(std::ranges::is_sorted(kReservedWords));

// Statements other than SELECT (reported at their first keyword).
constexpr auto kOtherStatements = std::to_array<std::string_view>({
    "ALTER",    "ANALYZE", "ATTACH",   "BEGIN",    "CALL",   "CHECKPOINT", "COMMIT",    "COPY",
    "CREATE",   "DELETE",  "DESCRIBE", "DETACH",   "DROP",   "EXECUTE",    "EXPLAIN",   "EXPORT",
    "GRANT",    "IMPORT",  "INSERT",   "INSTALL",  "LOAD",   "MERGE",      "PIVOT",     "PRAGMA",
    "PREPARE",  "RESET",   "REVOKE",   "ROLLBACK", "SET",    "SHOW",       "SUMMARIZE", "TABLE",
    "TRUNCATE", "UNPIVOT", "UPDATE",   "USE",      "VACUUM", "VALUES",
});

// Clauses that can follow the select list, the table, the predicate or LIMIT.
constexpr auto kUnsupportedClauses = std::to_array<Construct>({
    {.keyword = "CROSS", .message = "CROSS JOIN is not supported"},
    {.keyword = "EXCEPT", .message = "EXCEPT is not supported"},
    {.keyword = "FETCH", .message = "FETCH is not supported"},
    {.keyword = "FOR", .message = "FOR UPDATE/SHARE is not supported"},
    {.keyword = "FULL", .message = "FULL JOIN is not supported"},
    {.keyword = "INNER", .message = "INNER JOIN is not supported"},
    {.keyword = "INTERSECT", .message = "INTERSECT is not supported"},
    {.keyword = "JOIN", .message = "JOIN is not supported"},
    {.keyword = "LEFT", .message = "LEFT JOIN is not supported"},
    {.keyword = "NATURAL", .message = "NATURAL JOIN is not supported"},
    {.keyword = "OUTER", .message = "OUTER JOIN is not supported"},
    {.keyword = "QUALIFY", .message = "QUALIFY is not supported"},
    {.keyword = "RIGHT", .message = "RIGHT JOIN is not supported"},
    {.keyword = "UNION", .message = "UNION is not supported"},
    {.keyword = "USING", .message = "USING SAMPLE is not supported"},
    {.keyword = "WINDOW", .message = "WINDOW is not supported"},
});

// Keywords that start an expression outside the subset.
constexpr auto kUnsupportedOperandKeywords = std::to_array<Construct>({
    {.keyword = "ALL", .message = "ALL (quantified comparisons) is not supported"},
    {.keyword = "ANY", .message = "ANY (quantified comparisons) is not supported"},
    {.keyword = "ARRAY", .message = "ARRAY is not supported"},
    {.keyword = "CAST", .message = "CAST is not supported"},
    {.keyword = "EXISTS", .message = "EXISTS (subqueries) is not supported"},
    {.keyword = "FALSE", .message = "boolean literals (TRUE/FALSE) are not supported"},
    {.keyword = "INTERVAL", .message = "INTERVAL is not supported"},
    {.keyword = "NULL", .message = "NULL literals are not supported"},
    {.keyword = "SOME", .message = "SOME (quantified comparisons) is not supported"},
    {.keyword = "TRUE", .message = "boolean literals (TRUE/FALSE) are not supported"},
});

// Keywords that continue a complete operand into an expression outside the subset. They are
// checked before an implicit alias, so "SELECT a ISNULL FROM t" is never read as "a AS isnull".
constexpr auto kUnsupportedOperatorKeywords = std::to_array<Construct>({
    {.keyword = "BETWEEN", .message = "BETWEEN is not supported"},
    {.keyword = "COLLATE", .message = "COLLATE is not supported"},
    {.keyword = "ILIKE", .message = "ILIKE is not supported"},
    {.keyword = "ISNULL", .message = "ISNULL is not supported"},
    {.keyword = "NOTNULL", .message = "NOTNULL is not supported"},
    {.keyword = "OVER", .message = "window functions (OVER) are not supported"},
    {.keyword = "SIMILAR", .message = "SIMILAR TO is not supported"},
});

// Operators written as NOT <op> (a NOT LIKE 'x', a NOT IN (...)).
constexpr auto kNegatableOperators =
    std::to_array<std::string_view>({"BETWEEN", "ILIKE", "IN", "LIKE", "SIMILAR"});

// Modifiers of SELECT * (DuckDB).
constexpr auto kStarModifiers = std::to_array<Construct>({
    {.keyword = "EXCLUDE", .message = "SELECT * EXCLUDE is not supported"},
    {.keyword = "GLOB", .message = "SELECT * GLOB is not supported"},
    {.keyword = "ILIKE", .message = "SELECT * ILIKE is not supported"},
    {.keyword = "LIKE", .message = "SELECT * LIKE is not supported"},
    {.keyword = "NOT", .message = "SELECT * NOT LIKE is not supported"},
    {.keyword = "RENAME", .message = "SELECT * RENAME is not supported"},
    {.keyword = "REPLACE", .message = "SELECT * REPLACE is not supported"},
    {.keyword = "SIMILAR", .message = "SELECT * SIMILAR TO is not supported"},
});

// Typed literals other than DATE '...'.
constexpr auto kUnsupportedTypedLiterals = std::to_array<Construct>({
    {.keyword = "TIME", .message = "TIME literals are not supported"},
    {.keyword = "TIMESTAMP", .message = "TIMESTAMP literals are not supported"},
    {.keyword = "TIMESTAMPTZ", .message = "TIMESTAMPTZ literals are not supported"},
});

constexpr bool FitsKeywordLength(std::string_view word) { return word.size() <= kMaxKeywordLength; }
static_assert(std::ranges::all_of(kReservedWords, FitsKeywordLength));
static_assert(std::ranges::all_of(kOtherStatements, FitsKeywordLength));
static_assert(std::ranges::all_of(kNegatableOperators, FitsKeywordLength));
static_assert(std::ranges::all_of(kUnsupportedClauses, FitsKeywordLength, &Construct::keyword));
static_assert(std::ranges::all_of(kUnsupportedOperandKeywords, FitsKeywordLength,
                                  &Construct::keyword));
static_assert(std::ranges::all_of(kUnsupportedOperatorKeywords, FitsKeywordLength,
                                  &Construct::keyword));
static_assert(std::ranges::all_of(kUnsupportedTypedLiterals, FitsKeywordLength,
                                  &Construct::keyword));
static_assert(std::ranges::all_of(kStarModifiers, FitsKeywordLength, &Construct::keyword));

constexpr std::size_t kMaxQuotedText = 32;

std::optional<std::string_view> Find(std::span<const Construct> table, std::string_view keyword) {
  const auto it = std::ranges::find(table, keyword, &Construct::keyword);
  if (it == table.end()) {
    return std::nullopt;
  }
  return it->message;
}

bool Contains(std::span<const std::string_view> words, std::string_view keyword) {
  return std::ranges::find(words, keyword) != words.end();
}

// Upper-cased text of an unquoted identifier short enough to be a keyword; "" otherwise.
std::string KeywordOf(const Token& token) {
  if (token.kind != TokenKind::kIdentifier || token.text.size() > kMaxKeywordLength) {
    return {};
  }
  std::string upper = token.text;
  for (char& c : upper) {
    if (c >= 'a' && c <= 'z') {
      c = static_cast<char>(c - 'a' + 'A');
    }
  }
  return upper;
}

bool IsReservedKeyword(std::string_view keyword) {
  return !keyword.empty() && std::ranges::binary_search(kReservedWords, keyword);
}

// A token usable as a name: quoted identifier or non-reserved unquoted identifier.
bool IsName(const Token& token) {
  return token.kind == TokenKind::kQuotedIdentifier ||
         (token.kind == TokenKind::kIdentifier && !IsReservedKeyword(KeywordOf(token)));
}

std::string Clip(std::string_view text) {
  if (text.size() <= kMaxQuotedText) {
    return std::string(text);
  }
  return std::string(text.substr(0, kMaxQuotedText)) + "...";
}

// Human-readable token description for error messages; never echoes quoted text (it may hold any
// bytes), only identifiers, numbers and operators, which are ASCII by construction.
std::string Describe(const Token& token) {
  switch (token.kind) {
    case TokenKind::kIdentifier: {
      const std::string keyword = KeywordOf(token);
      if (IsReservedKeyword(keyword)) {
        return "keyword " + keyword;
      }
      return "identifier " + Clip(token.text);
    }
    case TokenKind::kInteger:
    case TokenKind::kDecimal:
      return std::string(ToString(token.kind)) + " " + Clip(token.text);
    case TokenKind::kOperator:
      return "operator '" + Clip(token.text) + "'";
    default:
      return std::string(ToString(token.kind));
  }
}

std::optional<AggKind> AggregateOf(const Token& token) {
  const std::string keyword = KeywordOf(token);
  if (keyword == "COUNT") {
    return AggKind::kCount;
  }
  if (keyword == "SUM") {
    return AggKind::kSum;
  }
  if (keyword == "AVG") {
    return AggKind::kAvg;
  }
  if (keyword == "MIN") {
    return AggKind::kMin;
  }
  if (keyword == "MAX") {
    return AggKind::kMax;
  }
  return std::nullopt;
}

Literal::Kind LiteralKindOf(TokenKind kind) {
  switch (kind) {
    case TokenKind::kInteger:
      return Literal::Kind::kInteger;
    case TokenKind::kDecimal:
      return Literal::Kind::kDecimal;
    default:
      return Literal::Kind::kString;
  }
}

// The binary operator of an operator token (the parser only calls it for one).
BinaryOp BinaryOpOf(const Token& token) {
  switch (token.kind) {
    case TokenKind::kPlus:
      return BinaryOp::kAdd;
    case TokenKind::kMinus:
      return BinaryOp::kSubtract;
    case TokenKind::kStar:
      return BinaryOp::kMultiply;
    case TokenKind::kSlash:
      return BinaryOp::kDivide;
    case TokenKind::kPercent:
      return BinaryOp::kModulo;
    case TokenKind::kOperator:
      return BinaryOp::kIntegerDivide;  // "//"
    case TokenKind::kEqual:
      return BinaryOp::kEq;
    case TokenKind::kNotEqual:
      return BinaryOp::kNe;
    case TokenKind::kLess:
      return BinaryOp::kLt;
    case TokenKind::kLessEqual:
      return BinaryOp::kLe;
    case TokenKind::kGreater:
      return BinaryOp::kGt;
    case TokenKind::kGreaterEqual:
      return BinaryOp::kGe;
    default:
      return token.IsKeyword("AND") ? BinaryOp::kAnd : BinaryOp::kOr;
  }
}

SourceSpan Cover(SourceSpan first, SourceSpan last) {
  return SourceSpan{.offset = first.offset, .length = last.offset + last.length - first.offset};
}

ParseError SyntaxError(SourceSpan span, std::string message) {
  return ParseError{.kind = ParseError::Kind::kSyntax, .message = std::move(message), .span = span};
}

ParseError UnsupportedError(SourceSpan span, std::string_view construct) {
  std::string message(construct);
  message += kUnsupportedHint;
  return ParseError{
      .kind = ParseError::Kind::kUnsupported, .message = std::move(message), .span = span};
}

std::unexpected<ParseError> Syntax(SourceSpan span, std::string message) {
  return std::unexpected(SyntaxError(span, std::move(message)));
}

std::unexpected<ParseError> Unsupported(SourceSpan span, std::string_view construct) {
  return std::unexpected(UnsupportedError(span, construct));
}

// Keywords that start a clause after the select list, supported or not.
bool IsClauseKeyword(std::string_view keyword) {
  return keyword == "FROM" || keyword == "WHERE" || keyword == "GROUP" || keyword == "HAVING" ||
         keyword == "ORDER" || keyword == "LIMIT" || keyword == "OFFSET" || keyword == "INTO" ||
         Find(kUnsupportedClauses, keyword).has_value();
}

// Tokens that may follow a complete list (select list, GROUP BY, ORDER BY); a comma before one of
// them is a trailing comma.
bool EndsList(const Token& token) {
  return token.kind == TokenKind::kEnd || token.kind == TokenKind::kSemicolon ||
         IsClauseKeyword(KeywordOf(token));
}

std::string_view Expectation(Context context) {
  return context == Context::kSelect ? "an expression or '*'" : "an expression";
}

ParseError NotAQuery(const Token& token) {
  const std::string keyword = KeywordOf(token);
  if (keyword == "WITH") {
    return UnsupportedError(token.span, "WITH (common table expressions) is not supported");
  }
  if (keyword == "FROM") {
    return UnsupportedError(token.span, "FROM-first queries are not supported");
  }
  if (Contains(kOtherStatements, keyword)) {
    return UnsupportedError(token.span,
                            keyword + " is not supported; only SELECT queries are supported");
  }
  if (token.kind == TokenKind::kLeftParen) {
    return UnsupportedError(token.span, "parenthesized queries are not supported");
  }
  if (token.kind == TokenKind::kEnd) {
    return SyntaxError(token.span, "empty query; expected SELECT");
  }
  return SyntaxError(token.span, "expected SELECT, found " + Describe(token));
}

class Parser {
 public:
  explicit Parser(std::string_view text) : lexer_(text) {}

  Expected<SelectStatement> Run() {
    auto result = ParseStatement();
    if (lex_error_.has_value()) {
      return std::unexpected(std::move(*lex_error_));
    }
    return result;
  }

 private:
  Expected<SelectStatement> ParseStatement() {
    if (!Peek().IsKeyword("SELECT")) {
      return std::unexpected(NotAQuery(Peek()));
    }
    const std::size_t begin = Take().span.offset;
    SelectStatement stmt;
    if (auto status = ParseSelectList(stmt); !status) {
      return std::unexpected(std::move(status.error()));
    }
    if (auto status = ExpectFrom(stmt.star); !status) {
      return std::unexpected(std::move(status.error()));
    }
    auto table = ParseTableRef();
    if (!table) {
      return std::unexpected(std::move(table.error()));
    }
    stmt.from = std::move(*table);
    if (auto status = CheckAfterTable(); !status) {
      return std::unexpected(std::move(status.error()));
    }
    if (Peek().IsKeyword("WHERE")) {
      Take();
      if (auto status = ParseConjuncts(Context::kWhere, stmt.where); !status) {
        return std::unexpected(std::move(status.error()));
      }
    }
    if (Peek().IsKeyword("GROUP")) {
      if (auto status = ParseGroupBy(stmt); !status) {
        return std::unexpected(std::move(status.error()));
      }
    }
    if (Peek().IsKeyword("HAVING")) {
      const std::size_t having_begin = Take().span.offset;
      if (auto status = ParseConjuncts(Context::kHaving, stmt.having); !status) {
        return std::unexpected(std::move(status.error()));
      }
      stmt.having_span = SourceSpan{.offset = having_begin, .length = last_end_ - having_begin};
    }
    if (Peek().IsKeyword("ORDER")) {
      if (auto status = ParseOrderBy(stmt); !status) {
        return std::unexpected(std::move(status.error()));
      }
    }
    // LIMIT and OFFSET, each at most once, in either order (as in DuckDB).
    while (true) {
      const bool limit = !stmt.limit.has_value() && Peek().IsKeyword("LIMIT");
      const bool offset = !stmt.offset.has_value() && Peek().IsKeyword("OFFSET");
      if (!limit && !offset) {
        break;
      }
      const std::size_t clause_begin = Take().span.offset;
      auto value = ParseLimit(limit ? "LIMIT" : "OFFSET");
      if (!value) {
        return std::unexpected(std::move(value.error()));
      }
      const SourceSpan span{.offset = clause_begin, .length = last_end_ - clause_begin};
      if (limit) {
        stmt.limit = *value;
        stmt.limit_span = span;
      } else {
        stmt.offset = *value;
        stmt.offset_span = span;
      }
    }
    stmt.span = SourceSpan{.offset = begin, .length = last_end_ - begin};
    if (auto status = ParseEnd(stmt); !status) {
      return std::unexpected(std::move(status.error()));
    }
    return stmt;
  }

  Status ParseSelectList(SelectStatement& stmt) {
    const Token& first = Peek();
    if (first.IsKeyword("DISTINCT")) {
      return Unsupported(first.span, "DISTINCT is not supported");
    }
    if (first.IsKeyword("ALL")) {
      return Unsupported(first.span, "SELECT ALL is not supported");
    }
    if (first.kind == TokenKind::kStar) {
      stmt.star_span = Take().span;
      stmt.star = true;
      const Token& next = Peek();
      if (next.kind == TokenKind::kComma) {
        return Unsupported(next.span, "combining '*' with other select items is not supported");
      }
      if (auto construct = Find(kStarModifiers, KeywordOf(next)); construct.has_value()) {
        return Unsupported(next.span, *construct);
      }
      return {};
    }
    while (true) {
      auto item = ParseSelectItem();
      if (!item) {
        return std::unexpected(std::move(item.error()));
      }
      stmt.items.push_back(std::move(*item));
      if (Peek().kind != TokenKind::kComma) {
        return {};
      }
      const SourceSpan comma = Take().span;
      if (EndsList(Peek())) {
        return Unsupported(comma, "a trailing comma in the select list is not supported");
      }
    }
  }

  Expected<SelectItem> ParseSelectItem() {
    const Token& first = Peek();
    if (first.kind == TokenKind::kStar) {
      return Unsupported(first.span, "combining '*' with other select items is not supported");
    }
    auto expr = ParseExpr(Context::kSelect);
    if (!expr) {
      return std::unexpected(std::move(expr.error()));
    }
    SelectItem item{.expr = std::move(*expr), .alias = {}, .span = {}};
    item.span = item.expr.span();
    const Token& next = Peek();
    if (next.IsKeyword("AS")) {
      Take();
      const Token& name = Peek();
      if (!IsName(name)) {
        // PostgreSQL and DuckDB accept any keyword after AS, and DuckDB also a string literal.
        if (name.kind == TokenKind::kIdentifier) {
          return Unsupported(name.span, "an alias cannot be the reserved word " + KeywordOf(name) +
                                            "; write it as a quoted identifier");
        }
        if (name.kind == TokenKind::kString) {
          return Unsupported(name.span,
                             "string literal aliases are not supported; write the alias as a "
                             "quoted identifier (\"...\")");
        }
        return Syntax(name.span, "expected an alias after AS, found " + Describe(name));
      }
    } else if (!IsName(next)) {
      return item;
    }
    Token alias = Take();
    item.alias = std::move(alias.text);
    item.span = Cover(item.span, alias.span);
    return item;
  }

  // ---- expressions: precedence climbing ----

  // Binding power of the operator at the next token (0: none): OR 1, AND 2, (NOT 3,) comparisons,
  // LIKE and IN 4, + - 5, * / // % 6.
  int InfixPrecedence() {
    const Token& token = Peek();
    switch (token.kind) {
      case TokenKind::kEqual:
      case TokenKind::kNotEqual:
      case TokenKind::kLess:
      case TokenKind::kLessEqual:
      case TokenKind::kGreater:
      case TokenKind::kGreaterEqual:
        return kComparisonPrecedence;
      case TokenKind::kPlus:
      case TokenKind::kMinus:
        return kAdditivePrecedence;
      case TokenKind::kStar:
      case TokenKind::kSlash:
      case TokenKind::kPercent:
        return kMultiplicativePrecedence;
      case TokenKind::kOperator:
        return token.text == "//" ? kMultiplicativePrecedence : 0;
      case TokenKind::kIdentifier:
        break;
      default:
        return 0;
    }
    const std::string keyword = KeywordOf(token);
    if (keyword == "OR") {
      return kOrPrecedence;
    }
    if (keyword == "AND") {
      return kAndPrecedence;
    }
    if (keyword == "LIKE" || keyword == "IN") {
      return kComparisonPrecedence;
    }
    if (keyword == "NOT") {
      const std::string after = KeywordOf(PeekAt(1));
      return after == "LIKE" || after == "IN" ? kComparisonPrecedence : 0;
    }
    return 0;
  }

  // Every level of the tree (a nested expression, or one more operator in a chain) counts
  // against kMaxDepth, so that the trees stay shallow enough for the recursive code that walks them
  // (copying, comparing, unparsing, binding, destroying).
  std::optional<ParseError> Deeper() {
    if (++depth_ > kMaxDepth) {
      return UnsupportedError(Peek().span, std::format("expressions deeper than {} levels "
                                                       "(operators or parentheses) are not "
                                                       "supported",
                                                       kMaxDepth));
    }
    return std::nullopt;
  }

  // An expression whose operators all bind at least as tightly as `min_precedence`.
  Expected<Expr> ParseExpr(Context context, int min_precedence = kOrPrecedence) {
    const std::size_t depth = depth_;
    auto result = ParseExprAtDepth(context, min_precedence);
    depth_ = depth;
    return result;
  }

  Expected<Expr> ParseExprAtDepth(Context context, int min_precedence) {
    if (auto error = Deeper(); error.has_value()) {
      return std::unexpected(std::move(*error));
    }
    auto lhs = ParsePrefix(context);
    if (!lhs) {
      return lhs;
    }
    while (true) {
      if (auto error = UnsupportedOperator(); error.has_value()) {
        return std::unexpected(std::move(*error));
      }
      const int precedence = InfixPrecedence();
      if (precedence == 0 || precedence < min_precedence) {
        return lhs;
      }
      // One more level for the operator; the right operand's own check (one level deeper) reports
      // a tree that gets too deep.
      ++depth_;
      auto combined = ParseInfix(context, *std::move(lhs), precedence);
      if (!combined) {
        return combined;
      }
      lhs = std::move(combined);
      // Comparisons do not chain (a = b = c), as in PostgreSQL.
      if (precedence == kComparisonPrecedence && InfixPrecedence() == kComparisonPrecedence) {
        return Unsupported(Peek().span, "chained comparisons (a = b = c) are not supported");
      }
    }
  }

  // The operator at the next token applied to `lhs`.
  Expected<Expr> ParseInfix(Context context, Expr lhs, int precedence) {
    const std::string keyword = KeywordOf(Peek());
    if (keyword == "LIKE" || keyword == "IN" || keyword == "NOT") {
      const bool negated = keyword == "NOT";
      const SourceSpan first = Take().span;
      const Token op =
          negated ? Take() : Token{.kind = TokenKind::kIdentifier, .text = keyword, .span = first};
      const SourceSpan op_span = Cover(first, op.span);
      if (op.IsKeyword("LIKE")) {
        auto pattern = ParseExpr(context, kAdditivePrecedence);
        if (!pattern) {
          return pattern;
        }
        if (Peek().IsKeyword("ESCAPE")) {
          return Unsupported(Peek().span, "LIKE ... ESCAPE is not supported");
        }
        const SourceSpan span = Cover(lhs.span(), pattern->span());
        return Expr(LikeExpr{.operand = Box<Expr>(std::move(lhs)),
                             .pattern = Box<Expr>(*std::move(pattern)),
                             .negated = negated,
                             .op_span = op_span,
                             .span = span});
      }
      return ParseInList(context, std::move(lhs), negated, op_span);
    }
    const Token op = Take();
    auto rhs = ParseExpr(context, precedence + 1);
    if (!rhs) {
      return rhs;
    }
    const SourceSpan span = Cover(lhs.span(), rhs->span());
    return Expr(BinaryExpr{.op = BinaryOpOf(op),
                           .left = Box<Expr>(std::move(lhs)),
                           .right = Box<Expr>(*std::move(rhs)),
                           .op_span = op.span,
                           .span = span});
  }

  // IN (value, ...), positioned at '(' after [NOT] IN.
  Expected<Expr> ParseInList(Context context, Expr lhs, bool negated, SourceSpan op_span) {
    if (Peek().kind != TokenKind::kLeftParen) {
      return Syntax(Peek().span, "expected ( after IN, found " + Describe(Peek()));
    }
    Take();
    if (Peek().IsKeyword("SELECT")) {
      return Unsupported(Peek().span, "IN (subquery) is not supported");
    }
    if (Peek().kind == TokenKind::kRightParen) {
      return Syntax(Peek().span, "expected a value in IN (...), found )");
    }
    InExpr in{.operand = Box<Expr>(std::move(lhs)),
              .list = {},
              .negated = negated,
              .op_span = op_span,
              .span = {}};
    while (true) {
      auto value = ParseExpr(context);
      if (!value) {
        return value;
      }
      in.list.push_back(*std::move(value));
      if (Peek().kind == TokenKind::kRightParen) {
        break;
      }
      if (Peek().kind != TokenKind::kComma) {
        return Syntax(Peek().span, "expected , or ) in IN (...), found " + Describe(Peek()));
      }
      Take();
    }
    in.span = Cover(in.operand->span(), Take().span);
    return Expr(std::move(in));
  }

  // NOT expr, - expr, or a primary expression.
  Expected<Expr> ParsePrefix(Context context) {
    const Token& token = Peek();
    if (token.IsKeyword("NOT")) {
      const SourceSpan op = Take().span;
      auto operand = ParseExpr(context, kNotPrecedence);
      if (!operand) {
        return operand;
      }
      const SourceSpan span = Cover(op, operand->span());
      return Expr(UnaryExpr{.op = UnaryOp::kNot,
                            .operand = Box<Expr>(*std::move(operand)),
                            .op_span = op,
                            .span = span});
    }
    if (token.kind == TokenKind::kMinus) {
      const TokenKind next = PeekAt(1).kind;
      if (next == TokenKind::kInteger || next == TokenKind::kDecimal) {
        const SourceSpan minus = Take().span;
        Token digits = Take();
        return Expr(Literal{.kind = LiteralKindOf(next),
                            .negative = true,
                            .text = std::move(digits.text),
                            .span = Cover(minus, digits.span)});
      }
      const SourceSpan op = Take().span;
      auto operand = ParseExpr(context, kUnaryPrecedence);
      if (!operand) {
        return operand;
      }
      const SourceSpan span = Cover(op, operand->span());
      return Expr(UnaryExpr{.op = UnaryOp::kNegate,
                            .operand = Box<Expr>(*std::move(operand)),
                            .op_span = op,
                            .span = span});
    }
    if (token.kind == TokenKind::kPlus) {
      return Unsupported(token.span, "unary '+' is not supported");
    }
    return ParsePrimary(context);
  }

  Expected<Expr> ParsePrimary(Context context) {
    const Token& token = Peek();
    switch (token.kind) {
      case TokenKind::kIdentifier:
        return ParseIdentifierPrimary(context);
      case TokenKind::kQuotedIdentifier: {
        if (PeekAt(1).kind == TokenKind::kLeftParen) {
          return ParseFunction(context);
        }
        Token name = Take();
        return Expr(ColumnRef{.name = std::move(name.text), .quoted = true, .span = name.span});
      }
      case TokenKind::kInteger:
      case TokenKind::kDecimal:
      case TokenKind::kString: {
        Token literal = Take();
        return Expr(Literal{.kind = LiteralKindOf(literal.kind),
                            .negative = false,
                            .text = std::move(literal.text),
                            .span = literal.span});
      }
      case TokenKind::kLeftParen: {
        if (PeekAt(1).IsKeyword("SELECT")) {
          return Unsupported(token.span, "subqueries are not supported");
        }
        const SourceSpan open = Take().span;
        auto inner = ParseExpr(context);
        if (!inner) {
          return inner;
        }
        if (Peek().kind == TokenKind::kComma) {
          return Unsupported(Peek().span, "row values ((a, b)) are not supported");
        }
        if (Peek().kind != TokenKind::kRightParen) {
          return Syntax(Peek().span, "expected ) to close the ( at offset " +
                                         std::to_string(open.offset) + ", found " +
                                         Describe(Peek()));
        }
        Take();
        return inner;
      }
      case TokenKind::kOperator:
        if (token.text == "?") {
          return Unsupported(token.span, "prepared statement parameters (?) are not supported");
        }
        return Unsupported(token.span, "operator '" + Clip(token.text) + "' is not supported");
      case TokenKind::kParameter:
        return Unsupported(token.span,
                           "parameters ($1) and dollar-quoted strings are not supported");
      case TokenKind::kLeftBracket:
        return Unsupported(token.span, "list literals ([...]) are not supported");
      case TokenKind::kLeftBrace:
        return Unsupported(token.span, "struct literals ({...}) are not supported");
      default:
        return Syntax(token.span, "expected " + std::string(Expectation(context)) + ", found " +
                                      Describe(token));
    }
  }

  Expected<Expr> ParseIdentifierPrimary(Context context) {
    const Token& token = Peek();
    const std::string keyword = KeywordOf(token);
    if (auto construct = Find(kUnsupportedOperandKeywords, keyword); construct.has_value()) {
      return Unsupported(token.span, *construct);
    }
    if (keyword == "CASE") {
      return ParseCase(context);
    }
    const Token& next = PeekAt(1);
    if (next.kind == TokenKind::kString) {
      if (keyword == "DATE") {
        const SourceSpan date = Take().span;
        Token text = Take();
        return Expr(Literal{.kind = Literal::Kind::kDate,
                            .negative = false,
                            .text = std::move(text.text),
                            .span = Cover(date, text.span)});
      }
      if (auto construct = Find(kUnsupportedTypedLiterals, keyword); construct.has_value()) {
        return Unsupported(token.span, *construct);
      }
      if (!IsReservedKeyword(keyword)) {  // type 'text' (INT '1') or a prefixed string (E'\n')
        return Unsupported(token.span,
                           "typed literals other than DATE '...' and prefixed strings (E'...') are "
                           "not supported");
      }
    }
    const bool function_like =
        !IsReservedKeyword(keyword) || keyword == "LEFT" || keyword == "RIGHT";
    if (next.kind == TokenKind::kLeftParen && function_like) {
      if (auto agg_kind = AggregateOf(token); agg_kind.has_value()) {
        switch (context) {
          case Context::kWhere:
            return Syntax(token.span, "aggregate functions are not allowed in WHERE");
          case Context::kGroupBy:
            return Syntax(token.span, "aggregate functions are not allowed in GROUP BY");
          case Context::kAggregateArg:
            return Syntax(token.span, "aggregate function calls cannot be nested");
          case Context::kSelect:
          case Context::kHaving:
          case Context::kOrderBy:
            break;
        }
        auto agg = ParseAggregate(*agg_kind);
        if (!agg) {
          return std::unexpected(std::move(agg.error()));
        }
        if (Peek().IsKeyword("FILTER") && PeekAt(1).kind == TokenKind::kLeftParen) {
          return Unsupported(Peek().span, "aggregate FILTER clauses are not supported");
        }
        return Expr(*std::move(agg));
      }
      if (keyword == "EXTRACT") {
        return ParseExtract(context);
      }
      return ParseFunction(context);
    }
    if (IsReservedKeyword(keyword)) {
      return Syntax(token.span,
                    "expected " + std::string(Expectation(context)) + ", found keyword " + keyword);
    }
    Token name = Take();
    return Expr(ColumnRef{.name = std::move(name.text), .quoted = false, .span = name.span});
  }

  // agg_call, positioned at the function name (the next token is '(').
  Expected<AggregateCall> ParseAggregate(AggKind kind) {
    const Token name = Take();
    Take();  // '('
    bool distinct = false;
    if (Peek().IsKeyword("DISTINCT")) {
      if (kind != AggKind::kCount) {
        return Unsupported(Peek().span, std::string(ToString(kind)) +
                                            "(DISTINCT ...) is not supported (only COUNT(DISTINCT "
                                            "column))");
      }
      Take();
      distinct = true;
      const Token& after = Peek();
      if (after.kind == TokenKind::kStar || after.kind == TokenKind::kRightParen) {
        return Syntax(after.span, "expected a column after DISTINCT, found " + Describe(after));
      }
    }
    const Token& arg = Peek();
    std::optional<Box<Expr>> argument;
    if (arg.kind == TokenKind::kStar) {
      if (kind != AggKind::kCount) {
        return Syntax(arg.span, "only COUNT accepts '*'");
      }
      Take();
      kind = AggKind::kCountStar;
    } else if (arg.IsKeyword("ALL")) {
      return Unsupported(arg.span, "ALL in aggregate calls is not supported");
    } else if (arg.kind == TokenKind::kRightParen) {
      if (kind == AggKind::kCount) {  // DuckDB reads COUNT() as COUNT(*)
        return Unsupported(arg.span, "COUNT() without an argument is not supported (use COUNT(*))");
      }
      return Syntax(arg.span, "expected a column in " + std::string(ToString(kind)) + "()");
    } else {
      auto expr = ParseExpr(Context::kAggregateArg);
      if (!expr) {
        return std::unexpected(std::move(expr.error()));
      }
      argument.emplace(*std::move(expr));
      if (Peek().IsKeyword("ORDER")) {
        return Unsupported(Peek().span, "ORDER BY is not supported");
      }
      if (Peek().kind == TokenKind::kComma) {
        return Syntax(Peek().span, std::string(ToString(kind)) + " takes one argument");
      }
    }
    const Token& close = Peek();
    if (close.kind != TokenKind::kRightParen) {
      return Syntax(close.span, "expected ')' to close " + std::string(ToString(kind)) +
                                    "(, found " + Describe(close));
    }
    const SourceSpan span = Cover(name.span, Take().span);
    return AggregateCall{
        .kind = kind, .arg = std::move(argument), .distinct = distinct, .span = span};
  }

  // name(arg, ...), positioned at the name (the next token is '('). Special argument syntax that
  // DuckDB has for some functions (position('a' IN s), substring(s FROM 1), try_cast(x AS t), ...)
  // is not a syntax error but an unsupported function call, and so is a FILTER clause.
  Expected<Expr> ParseFunction(Context context) {
    Token name = Take();
    Take();  // '('
    FunctionCall call{.name = std::move(name.text),
                      .quoted = name.kind == TokenKind::kQuotedIdentifier,
                      .args = {},
                      .name_span = name.span,
                      .span = {}};
    const auto unsupported = [&call] {
      return Unsupported(call.name_span, "function " + Clip(call.name) +
                                             "() with this argument syntax is not supported");
    };
    if (Peek().IsKeyword("DISTINCT") || Peek().IsKeyword("ALL") ||
        Peek().kind == TokenKind::kStar) {
      return unsupported();
    }
    while (Peek().kind != TokenKind::kRightParen) {
      auto arg = ParseExpr(context);
      if (!arg) {
        if (arg.error().kind == ParseError::Kind::kSyntax && !lex_error_.has_value()) {
          return unsupported();
        }
        return arg;
      }
      call.args.push_back(*std::move(arg));
      if (Peek().kind == TokenKind::kRightParen) {
        break;
      }
      if (Peek().kind != TokenKind::kComma) {
        return unsupported();
      }
      Take();
    }
    call.span = Cover(call.name_span, Take().span);
    if (Peek().IsKeyword("FILTER") && PeekAt(1).kind == TokenKind::kLeftParen) {
      return Unsupported(Peek().span, "FILTER clauses are not supported");
    }
    return Expr(std::move(call));
  }

  // EXTRACT(field FROM source), positioned at EXTRACT.
  Expected<Expr> ParseExtract(Context context) {
    const SourceSpan begin = Take().span;
    Take();  // '('
    const Token& field = Peek();
    if (field.kind != TokenKind::kIdentifier) {
      return Syntax(field.span, "expected a field name in EXTRACT(, found " + Describe(field));
    }
    Token field_token = Take();
    if (!Peek().IsKeyword("FROM")) {
      return Syntax(Peek().span,
                    "expected FROM in EXTRACT(field FROM ...), found " + Describe(Peek()));
    }
    Take();
    auto source = ParseExpr(context);
    if (!source) {
      return source;
    }
    if (Peek().kind != TokenKind::kRightParen) {
      return Syntax(Peek().span, "expected ) to close EXTRACT(, found " + Describe(Peek()));
    }
    const SourceSpan span = Cover(begin, Take().span);
    return Expr(ExtractExpr{.field = std::move(field_token.text),
                            .source = Box<Expr>(*std::move(source)),
                            .field_span = field_token.span,
                            .span = span});
  }

  // CASE [operand] WHEN .. THEN .. [...] [ELSE ..] END, positioned at CASE.
  Expected<Expr> ParseCase(Context context) {
    const SourceSpan begin = Take().span;
    CaseExpr out{.operand = {}, .branches = {}, .otherwise = {}, .span = {}};
    if (!Peek().IsKeyword("WHEN")) {
      auto operand = ParseExpr(context);
      if (!operand) {
        return operand;
      }
      out.operand.emplace(*std::move(operand));
    }
    while (Peek().IsKeyword("WHEN")) {
      Take();
      auto when = ParseExpr(context);
      if (!when) {
        return when;
      }
      if (!Peek().IsKeyword("THEN")) {
        return Syntax(Peek().span, "expected THEN in CASE, found " + Describe(Peek()));
      }
      Take();
      auto then = ParseExpr(context);
      if (!then) {
        return then;
      }
      out.branches.push_back(
          CaseBranch{.when = Box<Expr>(*std::move(when)), .then = Box<Expr>(*std::move(then))});
    }
    if (out.branches.empty()) {
      return Syntax(Peek().span, "expected WHEN in CASE, found " + Describe(Peek()));
    }
    if (Peek().IsKeyword("ELSE")) {
      Take();
      auto otherwise = ParseExpr(context);
      if (!otherwise) {
        return otherwise;
      }
      out.otherwise.emplace(*std::move(otherwise));
    }
    if (!Peek().IsKeyword("END")) {
      return Syntax(Peek().span, "expected WHEN, ELSE or END in CASE, found " + Describe(Peek()));
    }
    out.span = Cover(begin, Take().span);
    return Expr(std::move(out));
  }

  // Tokens that would continue a complete operand into an expression outside the subset.
  std::optional<ParseError> UnsupportedOperator() {
    const Token& token = Peek();
    switch (token.kind) {
      case TokenKind::kConcat:
        return UnsupportedError(token.span, "string concatenation (||) is not supported");
      case TokenKind::kDoubleColon:
        return UnsupportedError(token.span, "CAST (::) is not supported");
      case TokenKind::kDot:
        return UnsupportedError(token.span, "qualified names (a.b) are not supported");
      case TokenKind::kOperator:
        if (token.text == "//") {
          return std::nullopt;
        }
        return UnsupportedError(token.span, "operator '" + Clip(token.text) + "' is not supported");
      case TokenKind::kLeftBracket:
        return UnsupportedError(token.span, "subscripts ([...]) are not supported");
      case TokenKind::kIdentifier:
        break;
      default:
        return std::nullopt;
    }
    const std::string keyword = KeywordOf(token);
    if (auto construct = Find(kUnsupportedOperatorKeywords, keyword); construct.has_value()) {
      return UnsupportedError(token.span, *construct);
    }
    if (keyword == "IS") {
      const std::string after = KeywordOf(PeekAt(1));
      if (after == "NULL") {
        return UnsupportedError(token.span, "IS NULL is not supported");
      }
      if (after == "NOT" && PeekAt(2).IsKeyword("NULL")) {
        return UnsupportedError(token.span, "IS NOT NULL is not supported");
      }
      return UnsupportedError(token.span, "IS is not supported");
    }
    if (keyword == "NOT") {
      const std::string after = KeywordOf(PeekAt(1));
      if (after == "LIKE" || after == "IN") {
        return std::nullopt;
      }
      if (Contains(kNegatableOperators, after)) {
        return UnsupportedError(token.span, "NOT " + after + " is not supported");
      }
      if (after == "NULL") {
        return UnsupportedError(token.span, "NOT NULL (IS NOT NULL) is not supported");
      }
      return UnsupportedError(token.span, "NOT is not supported");
    }
    return std::nullopt;
  }

  // A predicate, split at its top-level AND chain into `out`. The chain is collected in a loop, so
  // a long conjunction builds no deep tree. Should an OR follow, the chain so far (as a tree) is
  // its left operand and the whole predicate is one conjunct.
  Status ParseConjuncts(Context context, std::vector<Expr>& out) {
    const std::size_t depth = depth_;
    auto status = ParseConjunctsAtDepth(context, out);
    depth_ = depth;
    return status;
  }

  Status ParseConjunctsAtDepth(Context context, std::vector<Expr>& out) {
    std::vector<Expr> chain;
    while (true) {
      auto conjunct = ParseExpr(context, kNotPrecedence);
      if (!conjunct) {
        return std::unexpected(std::move(conjunct.error()));
      }
      chain.push_back(*std::move(conjunct));
      if (!Peek().IsKeyword("AND")) {
        break;
      }
      Take();
    }
    if (!Peek().IsKeyword("OR")) {
      std::ranges::move(chain, std::back_inserter(out));
      return {};
    }
    auto lhs = Conjunction(std::move(chain));
    if (!lhs) {
      return std::unexpected(std::move(lhs.error()));
    }
    while (Peek().IsKeyword("OR")) {
      ++depth_;  // checked by the right operand, one level deeper
      const Token op = Take();
      auto rhs = ParseExpr(context, kAndPrecedence);
      if (!rhs) {
        return std::unexpected(std::move(rhs.error()));
      }
      const SourceSpan span = Cover(lhs->span(), rhs->span());
      lhs = Expr(BinaryExpr{.op = BinaryOp::kOr,
                            .left = Box<Expr>(*std::move(lhs)),
                            .right = Box<Expr>(*std::move(rhs)),
                            .op_span = op.span,
                            .span = span});
    }
    out.push_back(*std::move(lhs));
    return {};
  }

  // The AND of `chain` as a left-deep tree; every AND counts against the depth limit.
  Expected<Expr> Conjunction(std::vector<Expr> chain) {
    Expr tree = std::move(chain.front());
    for (std::size_t i = 1; i < chain.size(); ++i) {
      if (auto error = Deeper(); error.has_value()) {
        return std::unexpected(std::move(*error));
      }
      const SourceSpan span = Cover(tree.span(), chain[i].span());
      tree = Expr(BinaryExpr{.op = BinaryOp::kAnd,
                             .left = Box<Expr>(std::move(tree)),
                             .right = Box<Expr>(std::move(chain[i])),
                             .op_span = {},
                             .span = span});
    }
    return tree;
  }

  Status ExpectFrom(bool star) {
    const Token& token = Peek();
    if (token.IsKeyword("FROM")) {
      Take();
      return {};
    }
    const std::string keyword = KeywordOf(token);
    if (auto construct = Find(kUnsupportedClauses, keyword); construct.has_value()) {
      return Unsupported(token.span, *construct);
    }
    if (keyword == "INTO") {
      return Unsupported(token.span, "SELECT INTO is not supported");
    }
    if (token.kind == TokenKind::kEnd || token.kind == TokenKind::kSemicolon ||
        IsClauseKeyword(keyword)) {
      return Unsupported(token.span, "SELECT without FROM is not supported");
    }
    return Syntax(token.span, std::string(star ? "expected FROM" : "expected ',' or FROM") +
                                  ", found " + Describe(token));
  }

  Expected<TableRef> ParseTableRef() {
    const Token& token = Peek();
    switch (token.kind) {
      case TokenKind::kIdentifier:
      case TokenKind::kQuotedIdentifier: {
        const std::string keyword = KeywordOf(token);
        if (PeekAt(1).kind == TokenKind::kLeftParen && !IsReservedKeyword(keyword)) {
          return Unsupported(token.span, "table functions are not supported");
        }
        if (IsReservedKeyword(keyword)) {
          return Syntax(token.span,
                        "expected a table name or a quoted file path, found keyword " + keyword);
        }
        const bool quoted = token.kind == TokenKind::kQuotedIdentifier;
        Token name = Take();
        return TableRef{.kind = TableRef::Kind::kName,
                        .name = std::move(name.text),
                        .quoted = quoted,
                        .span = name.span};
      }
      case TokenKind::kString: {
        Token path = Take();
        return TableRef{.kind = TableRef::Kind::kPath,
                        .name = std::move(path.text),
                        .quoted = false,
                        .span = path.span};
      }
      case TokenKind::kLeftParen:
        return Unsupported(token.span, "subqueries in FROM are not supported");
      default:
        return Syntax(token.span,
                      "expected a table name or a quoted file path, found " + Describe(token));
    }
  }

  Status CheckAfterTable() {
    const Token& token = Peek();
    if (token.kind == TokenKind::kDot) {
      return Unsupported(
          token.span,
          "qualified table names are not supported (quote file paths: 'dir/f.parquet')");
    }
    if (token.kind == TokenKind::kComma) {
      return Unsupported(token.span, "multiple tables in FROM (JOIN) are not supported");
    }
    if (token.IsKeyword("TABLESAMPLE")) {
      return Unsupported(token.span, "TABLESAMPLE is not supported");
    }
    if (token.IsKeyword("AS") || IsName(token)) {
      return Unsupported(token.span, "table aliases are not supported");
    }
    return {};
  }

  // GROUP BY expr (',' expr)*, positioned at GROUP.
  Status ParseGroupBy(SelectStatement& stmt) {
    const std::size_t begin = Take().span.offset;
    if (!Peek().IsKeyword("BY")) {
      return Syntax(Peek().span, "expected BY after GROUP, found " + Describe(Peek()));
    }
    Take();
    if (Peek().IsKeyword("ALL")) {
      return Unsupported(Peek().span, "GROUP BY ALL is not supported");
    }
    if (Peek().IsKeyword("GROUPING") && PeekAt(1).IsKeyword("SETS")) {
      return Unsupported(Peek().span, "GROUPING SETS are not supported");
    }
    if (Peek().IsKeyword("ROLLUP") || Peek().IsKeyword("CUBE")) {
      return Unsupported(Peek().span, KeywordOf(Peek()) + " is not supported");
    }
    while (true) {
      auto expr = ParseExpr(Context::kGroupBy);
      if (!expr) {
        return std::unexpected(std::move(expr.error()));
      }
      stmt.group_by.push_back(*std::move(expr));
      if (Peek().kind != TokenKind::kComma) {
        break;
      }
      const SourceSpan comma = Take().span;
      if (EndsList(Peek())) {
        return Unsupported(comma, "a trailing comma in GROUP BY is not supported");
      }
    }
    stmt.group_by_span = SourceSpan{.offset = begin, .length = last_end_ - begin};
    return {};
  }

  // ORDER BY order_item (',' order_item)*, positioned at ORDER.
  Status ParseOrderBy(SelectStatement& stmt) {
    const std::size_t begin = Take().span.offset;
    if (!Peek().IsKeyword("BY")) {
      return Syntax(Peek().span, "expected BY after ORDER, found " + Describe(Peek()));
    }
    Take();
    if (Peek().IsKeyword("ALL")) {
      return Unsupported(Peek().span, "ORDER BY ALL is not supported");
    }
    while (true) {
      auto item = ParseOrderItem();
      if (!item) {
        return std::unexpected(std::move(item.error()));
      }
      stmt.order_by.push_back(std::move(*item));
      if (Peek().kind != TokenKind::kComma) {
        break;
      }
      const SourceSpan comma = Take().span;
      if (EndsList(Peek())) {
        return Unsupported(comma, "a trailing comma in ORDER BY is not supported");
      }
    }
    stmt.order_by_span = SourceSpan{.offset = begin, .length = last_end_ - begin};
    return {};
  }

  Expected<OrderItem> ParseOrderItem() {
    auto expr = ParseExpr(Context::kOrderBy);
    if (!expr) {
      return std::unexpected(std::move(expr.error()));
    }
    OrderItem item{.expr = *std::move(expr), .descending = false, .nulls = {}, .span = {}};
    item.span = item.expr.span();
    if (Peek().IsKeyword("USING")) {  // PostgreSQL's ORDER BY a USING <
      return Unsupported(Peek().span, "ORDER BY ... USING is not supported");
    }
    const bool asc = Peek().IsKeyword("ASC");
    if (asc || Peek().IsKeyword("DESC")) {
      item.descending = !asc;
      item.span = Cover(item.span, Take().span);
    }
    if (Peek().IsKeyword("NULLS")) {
      const Token& which = PeekAt(1);
      if (which.IsKeyword("FIRST")) {
        item.nulls = NullsOrder::kFirst;
      } else if (which.IsKeyword("LAST")) {
        item.nulls = NullsOrder::kLast;
      } else {
        return Syntax(which.span, "expected FIRST or LAST after NULLS, found " + Describe(which));
      }
      Take();
      item.span = Cover(item.span, Take().span);
    }
    return item;
  }
  // The integer after LIMIT or OFFSET (`clause`).
  Expected<std::int64_t> ParseLimit(std::string_view clause) {
    const std::string name(clause);
    const Token& token = Peek();
    if (token.kind == TokenKind::kInteger) {
      Token digits = Take();
      std::int64_t value = 0;
      const char* first = digits.text.data();
      const char* last = first + digits.text.size();
      const auto [ptr, ec] = std::from_chars(first, last, value);
      if (ec != std::errc() || ptr != last) {
        return Syntax(digits.span, name + " " + Clip(digits.text) +
                                       " is out of range (the maximum is 9223372036854775807)");
      }
      const Token& next = Peek();
      if (next.kind == TokenKind::kPercent || next.IsKeyword("PERCENT")) {
        return Unsupported(next.span, name + " with a percentage is not supported");
      }
      if (next.kind == TokenKind::kPlus || next.kind == TokenKind::kMinus ||
          next.kind == TokenKind::kStar || next.kind == TokenKind::kSlash ||
          (next.kind == TokenKind::kOperator && next.text == "//")) {
        return Unsupported(next.span,
                           name + " expressions are not supported (" + name + " takes an integer)");
      }
      if (auto error = UnsupportedOperator(); error.has_value()) {
        return std::unexpected(std::move(*error));
      }
      if (clause == "LIMIT" && Peek().kind == TokenKind::kComma) {
        return Unsupported(Peek().span, "LIMIT with an offset (LIMIT n, m) is not supported");
      }
      return value;
    }
    const std::string keyword = KeywordOf(token);
    if (keyword == "ALL") {
      return Unsupported(token.span, name + " ALL is not supported");
    }
    if (token.kind == TokenKind::kMinus) {
      return Syntax(token.span, name + " must not be negative");
    }
    if (token.kind == TokenKind::kDecimal) {  // PostgreSQL and DuckDB round it
      return Unsupported(token.span, "non-integer " + name + " values are not supported");
    }
    if (auto construct = Find(kUnsupportedOperandKeywords, keyword); construct.has_value()) {
      return Unsupported(token.span, *construct);
    }
    switch (token.kind) {
      case TokenKind::kLeftParen:
      case TokenKind::kPlus:
      case TokenKind::kString:
      case TokenKind::kOperator:
      case TokenKind::kParameter:
      case TokenKind::kLeftBracket:
      case TokenKind::kLeftBrace:
        return Unsupported(token.span,
                           name + " expressions are not supported (" + name + " takes an integer)");
      default:
        break;
    }
    if (token.kind == TokenKind::kIdentifier && PeekAt(1).kind == TokenKind::kLeftParen) {
      return Unsupported(token.span,
                         name + " expressions are not supported (" + name + " takes an integer)");
    }
    return Syntax(token.span,
                  "expected a non-negative integer after " + name + ", found " + Describe(token));
  }

  Status ParseEnd(const SelectStatement& stmt) {
    const Token& token = Peek();
    if (token.kind == TokenKind::kEnd) {
      return {};
    }
    if (token.kind == TokenKind::kSemicolon) {
      Take();
      if (Peek().kind != TokenKind::kEnd) {
        return Unsupported(Peek().span, "multiple statements are not supported");
      }
      return {};
    }
    const std::string keyword = KeywordOf(token);
    if (auto construct = Find(kUnsupportedClauses, keyword); construct.has_value()) {
      return Unsupported(token.span, *construct);
    }
    // The clauses that could still follow, in grammar order.
    const bool ordered_or_later =
        !stmt.order_by.empty() || stmt.limit.has_value() || stmt.offset.has_value();
    const bool grouped_or_later =
        !stmt.group_by.empty() || !stmt.having.empty() || ordered_or_later;
    std::vector<std::string_view> next;
    if (!grouped_or_later) {
      next.emplace_back(stmt.where.empty() ? "WHERE" : "AND");
      next.emplace_back("GROUP BY");
    }
    if (!ordered_or_later) {
      next.emplace_back(stmt.having.empty() ? "HAVING" : "AND");
      next.emplace_back("ORDER BY");
    }
    if (!stmt.limit.has_value()) {
      next.emplace_back("LIMIT");
    }
    if (!stmt.offset.has_value()) {
      next.emplace_back("OFFSET");
    }
    std::string expected = "expected ";
    for (const std::string_view clause : next) {
      expected += std::string(clause) + ", ";
    }
    if (!next.empty()) {
      expected.replace(expected.size() - 2, 2, " or ");
    }
    expected += "the end of the query";
    return Syntax(token.span, "unexpected " + Describe(token) + "; " + expected);
  }

  // Lazily lexed lookahead. A lexer error becomes a kEnd token at the error offset (and is reported
  // by Run()). std::deque keeps references to buffered tokens valid while more are appended.
  const Token& PeekAt(std::size_t ahead) {
    while (tokens_.size() <= ahead && (tokens_.empty() || tokens_.back().kind != TokenKind::kEnd)) {
      auto next = lexer_.Next();
      if (next) {
        tokens_.push_back(std::move(*next));
      } else {
        const SourceSpan at{.offset = next.error().span.offset, .length = 0};
        lex_error_ = std::move(next.error());
        tokens_.push_back(Token{.kind = TokenKind::kEnd, .text = {}, .span = at});
      }
    }
    return ahead < tokens_.size() ? tokens_[ahead] : tokens_.back();
  }

  const Token& Peek() { return PeekAt(0); }

  // Consumes the next token (kEnd is never consumed). Invalidates references returned by Peek().
  Token Take() {
    const Token& front = Peek();
    if (front.kind == TokenKind::kEnd) {
      return front;
    }
    Token token = std::move(tokens_.front());
    tokens_.pop_front();
    last_end_ = token.span.offset + token.span.length;
    return token;
  }

  Lexer lexer_;
  std::deque<Token> tokens_;
  std::optional<ParseError> lex_error_;
  std::size_t last_end_ = 0;
  std::size_t depth_ = 0;  // levels of the expression being parsed
};

}  // namespace

std::expected<SelectStatement, ParseError> Parse(std::string_view text) {
  return Parser(text).Run();
}

bool IsReservedWord(std::string_view word) {
  if (word.size() > kMaxKeywordLength) {
    return false;  // also keeps KeywordOf from copying long names
  }
  const Token token{.kind = TokenKind::kIdentifier, .text = std::string(word), .span = {}};
  return IsReservedKeyword(KeywordOf(token));
}

}  // namespace antb1::sql
