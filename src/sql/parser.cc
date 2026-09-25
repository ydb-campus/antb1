#include "antb1/sql/parser.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
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

// Hand-written recursive-descent parser for the subset in docs/sql-subset.md:
//
//   statement   := query [';'] EOF
//   query       := SELECT select_list FROM table_ref [WHERE predicate] [LIMIT integer]
//   select_list := '*' | select_item (',' select_item)*
//   select_item := (agg_call | column_ref) [[AS] identifier]
//   agg_call    := COUNT '(' '*' ')' | (COUNT | SUM | AVG | MIN | MAX) '(' column_ref ')'
//   table_ref   := identifier | quoted_identifier | string_literal
//   predicate   := comparison (AND comparison)*
//   comparison  := column_ref cmp_op literal | literal cmp_op column_ref
//   literal     := ['-'] integer | ['-'] decimal | string_literal | DATE string_literal
//
// No production is recursive, so neither is the parser. Tokens are pulled lazily from the lexer
// (at most three tokens of lookahead), so work and memory stop at the first error whatever the
// input. Recognized SQL outside the subset yields kUnsupported at its first offending token and
// names the construct; anything else yields kSyntax. The lexer is only asked for the tokens the
// parser looks at (at most two past the one being parsed); a lexer error among them wins over the
// parser's own verdict, which may have been reached on the placeholder end-of-input token.

namespace antb1::sql {
namespace {

template <typename T>
using Expected = std::expected<T, ParseError>;
using Status = std::expected<void, ParseError>;
using Operand = std::variant<ColumnRef, Literal>;

// Where an operand is parsed; selects the error wording and whether literals are allowed.
enum class Context : std::uint8_t { kSelect, kAggregateArg, kWhere };

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
    {.keyword = "GROUP", .message = "GROUP BY is not supported"},
    {.keyword = "HAVING", .message = "HAVING is not supported"},
    {.keyword = "INNER", .message = "INNER JOIN is not supported"},
    {.keyword = "INTERSECT", .message = "INTERSECT is not supported"},
    {.keyword = "JOIN", .message = "JOIN is not supported"},
    {.keyword = "LEFT", .message = "LEFT JOIN is not supported"},
    {.keyword = "NATURAL", .message = "NATURAL JOIN is not supported"},
    {.keyword = "OFFSET", .message = "OFFSET is not supported"},
    {.keyword = "ORDER", .message = "ORDER BY is not supported"},
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
    {.keyword = "CASE", .message = "CASE is not supported"},
    {.keyword = "CAST", .message = "CAST is not supported"},
    {.keyword = "EXISTS", .message = "EXISTS (subqueries) is not supported"},
    {.keyword = "FALSE", .message = "boolean literals (TRUE/FALSE) are not supported"},
    {.keyword = "INTERVAL", .message = "INTERVAL is not supported"},
    {.keyword = "NOT", .message = "NOT is not supported"},
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
    {.keyword = "IN", .message = "IN is not supported"},
    {.keyword = "ISNULL", .message = "ISNULL is not supported"},
    {.keyword = "LIKE", .message = "LIKE is not supported"},
    {.keyword = "NOTNULL", .message = "NOTNULL is not supported"},
    {.keyword = "OR", .message = "OR is not supported"},
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

bool IsReservedWord(std::string_view keyword) {
  return !keyword.empty() && std::ranges::binary_search(kReservedWords, keyword);
}

// A token usable as a name: quoted identifier or non-reserved unquoted identifier.
bool IsName(const Token& token) {
  return token.kind == TokenKind::kQuotedIdentifier ||
         (token.kind == TokenKind::kIdentifier && !IsReservedWord(KeywordOf(token)));
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
      if (IsReservedWord(keyword)) {
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

std::optional<CompareOp> CompareOpOf(TokenKind kind) {
  switch (kind) {
    case TokenKind::kEqual:
      return CompareOp::kEq;
    case TokenKind::kNotEqual:
      return CompareOp::kNe;
    case TokenKind::kLess:
      return CompareOp::kLt;
    case TokenKind::kLessEqual:
      return CompareOp::kLe;
    case TokenKind::kGreater:
      return CompareOp::kGt;
    case TokenKind::kGreaterEqual:
      return CompareOp::kGe;
    default:
      return std::nullopt;
  }
}

// The operator with swapped operands: 5 < c  <=>  c > 5.
CompareOp Mirror(CompareOp op) {
  switch (op) {
    case CompareOp::kLt:
      return CompareOp::kGt;
    case CompareOp::kLe:
      return CompareOp::kGe;
    case CompareOp::kGt:
      return CompareOp::kLt;
    case CompareOp::kGe:
      return CompareOp::kLe;
    case CompareOp::kEq:
    case CompareOp::kNe:
      break;
  }
  return op;
}

SourceSpan Cover(SourceSpan first, SourceSpan last) {
  return SourceSpan{.offset = first.offset, .length = last.offset + last.length - first.offset};
}

SourceSpan SpanOf(const Operand& operand) {
  return std::visit([](const auto& node) { return node.span; }, operand);
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

// Tokens that may follow a complete predicate; a bare operand before one of them is a predicate
// that is not a comparison (WHERE flag, WHERE 1).
bool EndsPredicate(const Token& token) {
  const std::string keyword = KeywordOf(token);
  return token.kind == TokenKind::kEnd || token.kind == TokenKind::kSemicolon || keyword == "AND" ||
         keyword == "LIMIT" || Find(kUnsupportedClauses, keyword).has_value();
}

// Tokens that may follow a complete select list; a comma before one of them is a trailing comma.
bool EndsSelectList(const Token& token) {
  const std::string keyword = KeywordOf(token);
  return token.kind == TokenKind::kEnd || token.kind == TokenKind::kSemicolon ||
         keyword == "FROM" || keyword == "WHERE" || keyword == "LIMIT" || keyword == "INTO" ||
         Find(kUnsupportedClauses, keyword).has_value();
}

std::string_view Expectation(Context context) {
  switch (context) {
    case Context::kSelect:
      return "a column, an aggregate or '*'";
    case Context::kAggregateArg:
      return "a column";
    case Context::kWhere:
      return "a column or a literal";
  }
  return "an operand";
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
      if (auto status = ParsePredicate(stmt.where); !status) {
        return std::unexpected(std::move(status.error()));
      }
    }
    if (Peek().IsKeyword("LIMIT")) {
      Take();
      auto limit = ParseLimit();
      if (!limit) {
        return std::unexpected(std::move(limit.error()));
      }
      stmt.limit = *limit;
    }
    stmt.span = SourceSpan{.offset = begin, .length = last_end_ - begin};
    if (auto status = ParseEnd(!stmt.where.empty(), stmt.limit.has_value()); !status) {
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
      Take();
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
      if (EndsSelectList(Peek())) {
        return Unsupported(comma, "a trailing comma in the select list is not supported");
      }
    }
  }

  Expected<SelectItem> ParseSelectItem() {
    const Token& first = Peek();
    if (first.kind == TokenKind::kStar) {
      return Unsupported(first.span, "combining '*' with other select items is not supported");
    }
    SelectItem item;
    if (auto agg_kind = AggregateOf(first);
        agg_kind.has_value() && PeekAt(1).kind == TokenKind::kLeftParen) {
      auto agg = ParseAggregate(*agg_kind);
      if (!agg) {
        return std::unexpected(std::move(agg.error()));
      }
      item.span = agg->span;
      item.expr = std::move(*agg);
      if (Peek().IsKeyword("FILTER") && PeekAt(1).kind == TokenKind::kLeftParen) {
        return Unsupported(Peek().span, "aggregate FILTER clauses are not supported");
      }
    } else {
      auto operand = ParseOperand(Context::kSelect);
      if (!operand) {
        return std::unexpected(std::move(operand.error()));
      }
      auto* column = std::get_if<ColumnRef>(&*operand);
      if (column == nullptr) {
        return Unsupported(SpanOf(*operand), "constants in the select list are not supported");
      }
      item.span = column->span;
      item.expr = std::move(*column);
    }
    if (auto error = UnsupportedOperator(); error.has_value()) {
      return std::unexpected(std::move(*error));
    }
    const Token& next = Peek();
    if (CompareOpOf(next.kind).has_value()) {
      return Unsupported(next.span, "comparisons are only supported in WHERE");
    }
    if (next.IsKeyword("AND")) {
      return Unsupported(next.span, "AND is only supported between comparisons in WHERE");
    }
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

  // agg_call, positioned at the function name (the next token is '(').
  Expected<AggregateCall> ParseAggregate(AggKind kind) {
    const Token name = Take();
    Take();  // '('
    const Token& arg = Peek();
    std::optional<ColumnRef> column;
    if (arg.kind == TokenKind::kStar) {
      if (kind != AggKind::kCount) {
        return Syntax(arg.span, "only COUNT accepts '*'");
      }
      Take();
      kind = AggKind::kCountStar;
    } else if (arg.IsKeyword("DISTINCT")) {
      return Unsupported(arg.span, "DISTINCT aggregates are not supported");
    } else if (arg.IsKeyword("ALL")) {
      return Unsupported(arg.span, "ALL in aggregate calls is not supported");
    } else if (arg.kind == TokenKind::kRightParen) {
      if (kind == AggKind::kCount) {  // DuckDB reads COUNT() as COUNT(*)
        return Unsupported(arg.span, "COUNT() without an argument is not supported (use COUNT(*))");
      }
      return Syntax(arg.span, "expected a column in " + std::string(ToString(kind)) + "()");
    } else {
      auto operand = ParseOperand(Context::kAggregateArg);
      if (!operand) {
        return std::unexpected(std::move(operand.error()));
      }
      auto* ref = std::get_if<ColumnRef>(&*operand);
      if (ref == nullptr) {
        return Unsupported(SpanOf(*operand),
                           "constant aggregate arguments are not supported" +
                               std::string(kind == AggKind::kCount ? " (use COUNT(*))" : ""));
      }
      column = std::move(*ref);
      if (auto error = UnsupportedOperator(); error.has_value()) {
        return std::unexpected(std::move(*error));
      }
      if (Peek().IsKeyword("ORDER")) {
        return Unsupported(Peek().span, "ORDER BY is not supported");
      }
    }
    const Token& close = Peek();
    if (close.kind != TokenKind::kRightParen) {
      return Syntax(close.span, "expected ')' to close " + std::string(ToString(kind)) +
                                    "(, found " + Describe(close));
    }
    const SourceSpan span = Cover(name.span, Take().span);
    return AggregateCall{.kind = kind, .arg = std::move(column), .span = span};
  }

  // A column reference or a literal; rejects every other expression start.
  Expected<Operand> ParseOperand(Context context) {
    const Token& token = Peek();
    switch (token.kind) {
      case TokenKind::kIdentifier:
        return ParseIdentifierOperand(context);
      case TokenKind::kQuotedIdentifier: {
        if (PeekAt(1).kind == TokenKind::kLeftParen) {
          return Unsupported(token.span,
                             "function calls are not supported (only COUNT, SUM, AVG, MIN, MAX)");
        }
        Token name = Take();
        return ColumnRef{.name = std::move(name.text), .quoted = true, .span = name.span};
      }
      case TokenKind::kInteger:
      case TokenKind::kDecimal:
      case TokenKind::kString: {
        Token literal = Take();
        return Literal{.kind = LiteralKindOf(literal.kind),
                       .negative = false,
                       .text = std::move(literal.text),
                       .span = literal.span};
      }
      case TokenKind::kMinus: {
        const TokenKind number = PeekAt(1).kind;
        if (number != TokenKind::kInteger && number != TokenKind::kDecimal) {
          return Unsupported(token.span, "arithmetic operator '-' is not supported");
        }
        const SourceSpan minus = Take().span;
        Token digits = Take();
        return Literal{.kind = LiteralKindOf(number),
                       .negative = true,
                       .text = std::move(digits.text),
                       .span = Cover(minus, digits.span)};
      }
      case TokenKind::kPlus:
        return Unsupported(token.span, "arithmetic operator '+' is not supported");
      case TokenKind::kLeftParen:
        return Unsupported(token.span,
                           "parenthesized expressions and subqueries are not supported");
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

  Expected<Operand> ParseIdentifierOperand(Context context) {
    const Token& token = Peek();
    const std::string keyword = KeywordOf(token);
    if (auto construct = Find(kUnsupportedOperandKeywords, keyword); construct.has_value()) {
      return Unsupported(token.span, *construct);
    }
    const Token& next = PeekAt(1);
    if (next.kind == TokenKind::kString) {
      if (keyword == "DATE") {
        const SourceSpan date = Take().span;
        Token text = Take();
        return Literal{.kind = Literal::Kind::kDate,
                       .negative = false,
                       .text = std::move(text.text),
                       .span = Cover(date, text.span)};
      }
      if (auto construct = Find(kUnsupportedTypedLiterals, keyword); construct.has_value()) {
        return Unsupported(token.span, *construct);
      }
      if (!IsReservedWord(keyword)) {  // type 'text' (INT '1') or a prefixed string (E'\n', X'00')
        return Unsupported(token.span,
                           "typed literals other than DATE '...' and prefixed strings (E'...') are "
                           "not supported");
      }
    }
    const bool function_like = !IsReservedWord(keyword) || keyword == "LEFT" || keyword == "RIGHT";
    if (next.kind == TokenKind::kLeftParen && function_like) {
      if (AggregateOf(token).has_value()) {
        return Syntax(token.span, context == Context::kWhere
                                      ? "aggregate functions are not allowed in WHERE"
                                      : "aggregate function calls cannot be nested");
      }
      return Unsupported(token.span, "function " + Clip(token.text) +
                                         "() is not supported (only COUNT, SUM, AVG, MIN, MAX)");
    }
    if (IsReservedWord(keyword)) {
      return Syntax(token.span,
                    "expected " + std::string(Expectation(context)) + ", found keyword " + keyword);
    }
    Token name = Take();
    return ColumnRef{.name = std::move(name.text), .quoted = false, .span = name.span};
  }

  // Tokens that would continue a complete operand into an expression outside the subset.
  std::optional<ParseError> UnsupportedOperator() {
    const Token& token = Peek();
    switch (token.kind) {
      case TokenKind::kPlus:
      case TokenKind::kMinus:
      case TokenKind::kStar:
      case TokenKind::kSlash:
      case TokenKind::kPercent:
        return UnsupportedError(
            token.span,
            "arithmetic operator " + std::string(ToString(token.kind)) + " is not supported");
      case TokenKind::kConcat:
        return UnsupportedError(token.span, "string concatenation (||) is not supported");
      case TokenKind::kDoubleColon:
        return UnsupportedError(token.span, "CAST (::) is not supported");
      case TokenKind::kDot:
        return UnsupportedError(token.span, "qualified names (a.b) are not supported");
      case TokenKind::kOperator:
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
      if (Contains(kNegatableOperators, after)) {
        return UnsupportedError(token.span, "NOT " + after + " is not supported");
      }
      return UnsupportedError(token.span, "NOT is not supported");
    }
    return std::nullopt;
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
        keyword == "WHERE" || keyword == "LIMIT") {
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
        if (PeekAt(1).kind == TokenKind::kLeftParen && !IsReservedWord(keyword)) {
          return Unsupported(token.span, "table functions are not supported");
        }
        if (IsReservedWord(keyword)) {
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

  Status ParsePredicate(std::vector<Comparison>& out) {
    while (true) {
      auto comparison = ParseComparison();
      if (!comparison) {
        return std::unexpected(std::move(comparison.error()));
      }
      out.push_back(std::move(*comparison));
      if (auto error = UnsupportedOperator(); error.has_value()) {
        return std::unexpected(std::move(*error));
      }
      if (!Peek().IsKeyword("AND")) {
        return {};
      }
      Take();
    }
  }

  Expected<Comparison> ParseComparison() {
    auto lhs = ParseOperand(Context::kWhere);
    if (!lhs) {
      return std::unexpected(std::move(lhs.error()));
    }
    const Token& op_token = Peek();
    const std::optional<CompareOp> op = CompareOpOf(op_token.kind);
    if (!op.has_value()) {
      if (auto error = UnsupportedOperator(); error.has_value()) {
        return std::unexpected(std::move(*error));
      }
      if (EndsPredicate(op_token)) {
        return Unsupported(SpanOf(*lhs),
                           "predicates other than comparisons (column <op> literal) are not "
                           "supported");
      }
      return Syntax(
          op_token.span,
          "expected a comparison operator (=, <>, !=, <, <=, >, >=), found " + Describe(op_token));
    }
    Take();
    auto rhs = ParseOperand(Context::kWhere);
    if (!rhs) {
      return std::unexpected(std::move(rhs.error()));
    }
    const SourceSpan span = Cover(SpanOf(*lhs), SpanOf(*rhs));
    auto* lhs_column = std::get_if<ColumnRef>(&*lhs);
    auto* rhs_column = std::get_if<ColumnRef>(&*rhs);
    if (lhs_column != nullptr && rhs_column != nullptr) {
      return Unsupported(rhs_column->span, "comparisons between two columns are not supported");
    }
    if (lhs_column == nullptr && rhs_column == nullptr) {
      return Unsupported(SpanOf(*rhs), "comparisons between two literals are not supported");
    }
    if (lhs_column != nullptr) {
      return Comparison{.column = std::move(*lhs_column),
                        .op = *op,
                        .literal = std::get<Literal>(std::move(*rhs)),
                        .span = span};
    }
    return Comparison{.column = std::move(*rhs_column),
                      .op = Mirror(*op),
                      .literal = std::get<Literal>(std::move(*lhs)),
                      .span = span};
  }

  Expected<std::int64_t> ParseLimit() {
    const Token& token = Peek();
    if (token.kind == TokenKind::kInteger) {
      Token digits = Take();
      std::int64_t value = 0;
      const char* first = digits.text.data();
      const char* last = first + digits.text.size();
      const auto [ptr, ec] = std::from_chars(first, last, value);
      if (ec != std::errc() || ptr != last) {
        return Syntax(digits.span, "LIMIT " + Clip(digits.text) +
                                       " is out of range (the maximum is 9223372036854775807)");
      }
      const Token& next = Peek();
      if (next.kind == TokenKind::kPercent || next.IsKeyword("PERCENT")) {
        return Unsupported(next.span, "LIMIT with a percentage is not supported");
      }
      if (auto error = UnsupportedOperator(); error.has_value()) {
        return std::unexpected(std::move(*error));
      }
      if (Peek().kind == TokenKind::kComma) {
        return Unsupported(Peek().span, "LIMIT with an offset (LIMIT n, m) is not supported");
      }
      return value;
    }
    const std::string keyword = KeywordOf(token);
    if (keyword == "ALL") {
      return Unsupported(token.span, "LIMIT ALL is not supported");
    }
    if (token.kind == TokenKind::kMinus) {
      return Syntax(token.span, "LIMIT must not be negative");
    }
    if (token.kind == TokenKind::kDecimal) {  // PostgreSQL and DuckDB round it
      return Unsupported(token.span, "non-integer LIMIT values are not supported");
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
                           "LIMIT expressions are not supported (LIMIT takes an integer)");
      default:
        break;
    }
    if (token.kind == TokenKind::kIdentifier && PeekAt(1).kind == TokenKind::kLeftParen) {
      return Unsupported(token.span,
                         "LIMIT expressions are not supported (LIMIT takes an integer)");
    }
    return Syntax(token.span,
                  "expected a non-negative integer after LIMIT, found " + Describe(token));
  }

  Status ParseEnd(bool has_where, bool has_limit) {
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
    std::string expected = "expected the end of the query";
    if (!has_limit) {
      expected = has_where ? "expected AND, LIMIT or the end of the query"
                           : "expected WHERE, LIMIT or the end of the query";
    }
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
};

}  // namespace

std::expected<SelectStatement, ParseError> Parse(std::string_view text) {
  return Parser(text).Run();
}

}  // namespace antb1::sql
