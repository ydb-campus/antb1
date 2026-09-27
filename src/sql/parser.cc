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
//   query       := SELECT select_list FROM table_ref [WHERE predicate]
//                  [GROUP BY group_item (',' group_item)*] [HAVING having]
//                  [ORDER BY order_item (',' order_item)*]
//                  [LIMIT integer] [OFFSET integer]      (LIMIT and OFFSET in either order)
//   select_list := '*' | select_item (',' select_item)*
//   select_item := (agg_call | column_ref | literal) [[AS] identifier]
//   group_item  := column_ref | literal
//   agg_call    := COUNT '(' '*' ')' | COUNT '(' DISTINCT column_ref ')'
//                | (COUNT | SUM | AVG | MIN | MAX) '(' column_ref ')'
//   order_item  := (agg_call | column_ref | literal) [ASC | DESC] [NULLS (FIRST | LAST)]
//   table_ref   := identifier | quoted_identifier | string_literal
//   predicate   := comparison (AND comparison)*
//   comparison  := column_ref condition | literal cmp_op column_ref
//   having      := having_cmp (AND having_cmp)*
//   having_cmp  := (agg_call | column_ref) condition | literal cmp_op (agg_call | column_ref)
//   condition   := cmp_op literal | [NOT] LIKE string_literal | [NOT] IN '(' literal (',' literal)*
//   ')' literal     := ['-'] integer | ['-'] decimal | string_literal | DATE string_literal
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
enum class Context : std::uint8_t { kSelect, kAggregateArg, kWhere, kGroupBy, kHaving, kOrderBy };

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
    case CompareOp::kLike:  // never mirrored: the pattern or list must be on the right
    case CompareOp::kNotLike:
    case CompareOp::kIn:
    case CompareOp::kNotIn:
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

// Keywords that start a clause after the select list, supported or not.
bool IsClauseKeyword(std::string_view keyword) {
  return keyword == "FROM" || keyword == "WHERE" || keyword == "GROUP" || keyword == "HAVING" ||
         keyword == "ORDER" || keyword == "LIMIT" || keyword == "OFFSET" || keyword == "INTO" ||
         Find(kUnsupportedClauses, keyword).has_value();
}

// Tokens that may follow a complete predicate; a bare operand before one of them is a predicate
// that is not a comparison (WHERE flag, WHERE 1).
bool EndsPredicate(const Token& token) {
  const std::string keyword = KeywordOf(token);
  return token.kind == TokenKind::kEnd || token.kind == TokenKind::kSemicolon || keyword == "AND" ||
         IsClauseKeyword(keyword);
}

// Tokens that may follow a complete list (select list, GROUP BY, ORDER BY); a comma before one of
// them is a trailing comma.
bool EndsList(const Token& token) {
  return token.kind == TokenKind::kEnd || token.kind == TokenKind::kSemicolon ||
         IsClauseKeyword(KeywordOf(token));
}

std::string_view Expectation(Context context) {
  switch (context) {
    case Context::kSelect:
      return "a column, an aggregate or '*'";
    case Context::kAggregateArg:
      return "a column";
    case Context::kWhere:
      return "a column or a literal";
    case Context::kGroupBy:
      return "a column";
    case Context::kHaving:
      return "an aggregate, a column or a literal";
    case Context::kOrderBy:
      return "a column or an aggregate";
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
    if (Peek().IsKeyword("GROUP")) {
      if (auto status = ParseGroupBy(stmt); !status) {
        return std::unexpected(std::move(status.error()));
      }
    }
    if (Peek().IsKeyword("HAVING")) {
      if (auto status = ParseHaving(stmt); !status) {
        return std::unexpected(std::move(status.error()));
      }
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
      item.span = SpanOf(*operand);
      if (auto* column = std::get_if<ColumnRef>(&*operand)) {
        item.expr = std::move(*column);
      } else {
        item.expr = std::get<Literal>(std::move(*operand));
      }
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
    std::optional<ColumnRef> column;
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
    return AggregateCall{
        .kind = kind, .arg = std::move(column), .distinct = distinct, .span = span};
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
      if (!IsReservedKeyword(
              keyword)) {  // type 'text' (INT '1') or a prefixed string (E'\n', X'00')
        return Unsupported(token.span,
                           "typed literals other than DATE '...' and prefixed strings (E'...') are "
                           "not supported");
      }
    }
    const bool function_like =
        !IsReservedKeyword(keyword) || keyword == "LEFT" || keyword == "RIGHT";
    if (next.kind == TokenKind::kLeftParen && function_like) {
      if (AggregateOf(token).has_value()) {
        switch (context) {
          case Context::kWhere:
            return Syntax(token.span, "aggregate functions are not allowed in WHERE");
          case Context::kGroupBy:
            return Syntax(token.span, "aggregate functions are not allowed in GROUP BY");
          case Context::kHaving:  // a LIKE pattern or an IN value
            return Unsupported(token.span, "an aggregate is only supported left of LIKE or IN");
          default:
            return Syntax(token.span, "aggregate function calls cannot be nested");
        }
      }
      return Unsupported(token.span, "function " + Clip(token.text) +
                                         "() is not supported (only COUNT, SUM, AVG, MIN, MAX)");
    }
    if (IsReservedKeyword(keyword)) {
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
    auto* column = std::get_if<ColumnRef>(&*lhs);
    if (AtLike()) {
      auto like = ParseLike(Context::kWhere, SpanOf(*lhs), column != nullptr,
                            "LIKE needs a column on the left (column LIKE 'pattern')");
      if (!like) {
        return std::unexpected(std::move(like.error()));
      }
      const SourceSpan span = Cover(column->span, like->second.span);
      return Comparison{.column = std::move(*column),
                        .op = like->first,
                        .literal = std::move(like->second),
                        .span = span};
    }
    if (AtIn()) {
      auto in = ParseIn(Context::kWhere, SpanOf(*lhs), column != nullptr,
                        "IN needs a column on the left (column IN (...))");
      if (!in) {
        return std::unexpected(std::move(in.error()));
      }
      const SourceSpan span = Cover(column->span, in->close);
      return Comparison{.column = std::move(*column),
                        .op = in->op,
                        .literal = {},
                        .list = std::move(in->list),
                        .span = span};
    }
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

  bool AtLike() {
    return Peek().IsKeyword("LIKE") || (Peek().IsKeyword("NOT") && PeekAt(1).IsKeyword("LIKE"));
  }

  bool AtIn() {
    return Peek().IsKeyword("IN") || (Peek().IsKeyword("NOT") && PeekAt(1).IsKeyword("IN"));
  }

  // [NOT] LIKE 'pattern', positioned at LIKE or NOT, after an operand at `lhs` (`lhs_ok`: one
  // LIKE accepts, else the error is `lhs_error`). Returns kLike or kNotLike and the pattern.
  Expected<std::pair<CompareOp, Literal>> ParseLike(Context context, SourceSpan lhs, bool lhs_ok,
                                                    std::string_view lhs_error) {
    const bool negated = Peek().IsKeyword("NOT");
    if (negated) {
      Take();
    }
    Take();  // LIKE
    auto rhs = ParseOperand(context);
    if (!rhs) {
      return std::unexpected(std::move(rhs.error()));
    }
    if (!lhs_ok) {
      return Unsupported(lhs, lhs_error);
    }
    auto* pattern = std::get_if<Literal>(&*rhs);
    if (pattern == nullptr) {
      return Unsupported(SpanOf(*rhs), "LIKE with a column as the pattern is not supported");
    }
    if (Peek().IsKeyword("ESCAPE")) {
      return Unsupported(Peek().span, "LIKE ... ESCAPE is not supported");
    }
    return std::pair(negated ? CompareOp::kNotLike : CompareOp::kLike, std::move(*pattern));
  }

  struct InList {
    CompareOp op = CompareOp::kIn;
    std::vector<Literal> list;
    SourceSpan close;  // the ')'
  };

  // [NOT] IN (literal, ...), positioned at IN or NOT, after an operand at `lhs` (as in ParseLike).
  Expected<InList> ParseIn(Context context, SourceSpan lhs, bool lhs_ok,
                           std::string_view lhs_error) {
    const bool negated = Peek().IsKeyword("NOT");
    if (negated) {
      Take();
    }
    Take();  // IN
    if (!lhs_ok) {
      return Unsupported(lhs, lhs_error);
    }
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
    InList in{.op = negated ? CompareOp::kNotIn : CompareOp::kIn, .list = {}, .close = {}};
    while (true) {
      auto value = ParseOperand(context);
      if (!value) {
        return std::unexpected(std::move(value.error()));
      }
      auto* literal = std::get_if<Literal>(&*value);
      if (literal == nullptr) {
        return Unsupported(SpanOf(*value), "columns in an IN list are not supported");
      }
      in.list.push_back(std::move(*literal));
      if (auto error = UnsupportedOperator(); error.has_value()) {
        return std::unexpected(std::move(*error));
      }
      if (Peek().kind == TokenKind::kRightParen) {
        break;
      }
      if (Peek().kind != TokenKind::kComma) {
        return Syntax(Peek().span, "expected , or ) in IN (...), found " + Describe(Peek()));
      }
      Take();
    }
    in.close = Take().span;
    return in;
  }

  // GROUP BY column_ref (',' column_ref)*, positioned at GROUP.
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
    while (true) {
      auto operand = ParseOperand(Context::kGroupBy);
      if (!operand) {
        return std::unexpected(std::move(operand.error()));
      }
      if (auto* column = std::get_if<ColumnRef>(&*operand)) {
        stmt.group_by.emplace_back(std::move(*column));
      } else {
        stmt.group_by.emplace_back(std::get<Literal>(std::move(*operand)));
      }
      if (auto error = UnsupportedOperator(); error.has_value()) {
        return std::unexpected(std::move(*error));
      }
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

  // HAVING having_cmp (AND having_cmp)*, positioned at HAVING.
  Status ParseHaving(SelectStatement& stmt) {
    const std::size_t begin = Take().span.offset;
    while (true) {
      auto comparison = ParseHavingComparison();
      if (!comparison) {
        return std::unexpected(std::move(comparison.error()));
      }
      stmt.having.push_back(std::move(*comparison));
      if (auto error = UnsupportedOperator(); error.has_value()) {
        return std::unexpected(std::move(*error));
      }
      if (!Peek().IsKeyword("AND")) {
        break;
      }
      Take();
    }
    stmt.having_span = SourceSpan{.offset = begin, .length = last_end_ - begin};
    return {};
  }

  // An aggregate call, a column or a literal (a HAVING operand).
  Expected<SelectExpr> ParseHavingOperand() {
    if (auto agg_kind = AggregateOf(Peek());
        agg_kind.has_value() && PeekAt(1).kind == TokenKind::kLeftParen) {
      auto agg = ParseAggregate(*agg_kind);
      if (!agg) {
        return std::unexpected(std::move(agg.error()));
      }
      if (Peek().IsKeyword("FILTER") && PeekAt(1).kind == TokenKind::kLeftParen) {
        return Unsupported(Peek().span, "aggregate FILTER clauses are not supported");
      }
      return SelectExpr(std::move(*agg));
    }
    auto operand = ParseOperand(Context::kHaving);
    if (!operand) {
      return std::unexpected(std::move(operand.error()));
    }
    if (auto* column = std::get_if<ColumnRef>(&*operand)) {
      return SelectExpr(std::move(*column));
    }
    return SelectExpr(std::get<Literal>(std::move(*operand)));
  }

  static SourceSpan SpanOfExpr(const SelectExpr& expr) {
    return std::visit([](const auto& node) { return node.span; }, expr);
  }

  static std::optional<HavingOperand> AsHavingOperand(SelectExpr expr) {
    if (auto* agg = std::get_if<AggregateCall>(&expr)) {
      return HavingOperand(std::move(*agg));
    }
    if (auto* column = std::get_if<ColumnRef>(&expr)) {
      return HavingOperand(std::move(*column));
    }
    return std::nullopt;
  }

  Expected<HavingComparison> ParseHavingComparison() {
    auto lhs = ParseHavingOperand();
    if (!lhs) {
      return std::unexpected(std::move(lhs.error()));
    }
    const SourceSpan lhs_span = SpanOfExpr(*lhs);
    std::optional<HavingOperand> operand = AsHavingOperand(*lhs);
    if (AtLike()) {
      auto like = ParseLike(Context::kHaving, lhs_span, operand.has_value(),
                            "LIKE needs a column or an aggregate on the left");
      if (!like) {
        return std::unexpected(std::move(like.error()));
      }
      const SourceSpan span = Cover(lhs_span, like->second.span);
      return HavingComparison{.operand = *std::move(operand),
                              .op = like->first,
                              .literal = std::move(like->second),
                              .list = {},
                              .span = span};
    }
    if (AtIn()) {
      auto in = ParseIn(Context::kHaving, lhs_span, operand.has_value(),
                        "IN needs a column or an aggregate on the left");
      if (!in) {
        return std::unexpected(std::move(in.error()));
      }
      return HavingComparison{.operand = *std::move(operand),
                              .op = in->op,
                              .literal = {},
                              .list = std::move(in->list),
                              .span = Cover(lhs_span, in->close)};
    }
    const Token& op_token = Peek();
    const std::optional<CompareOp> op = CompareOpOf(op_token.kind);
    if (!op.has_value()) {
      if (auto error = UnsupportedOperator(); error.has_value()) {
        return std::unexpected(std::move(*error));
      }
      if (EndsPredicate(op_token)) {
        return Unsupported(lhs_span,
                           "HAVING conditions other than comparisons (aggregate or column <op> "
                           "literal) are not supported");
      }
      return Syntax(
          op_token.span,
          "expected a comparison operator (=, <>, !=, <, <=, >, >=), found " + Describe(op_token));
    }
    Take();
    auto rhs = ParseHavingOperand();
    if (!rhs) {
      return std::unexpected(std::move(rhs.error()));
    }
    const SourceSpan span = Cover(lhs_span, SpanOfExpr(*rhs));
    std::optional<HavingOperand> rhs_operand = AsHavingOperand(*rhs);
    if (operand.has_value() == rhs_operand.has_value()) {
      return Unsupported(SpanOfExpr(*rhs),
                         operand.has_value()
                             ? "HAVING comparisons of two columns or aggregates "
                               "are not supported"
                             : "comparisons between two literals are not supported");
    }
    if (operand.has_value()) {
      return HavingComparison{.operand = *std::move(operand),
                              .op = *op,
                              .literal = std::get<Literal>(std::move(*rhs)),
                              .list = {},
                              .span = span};
    }
    return HavingComparison{.operand = *std::move(rhs_operand),
                            .op = Mirror(*op),
                            .literal = std::get<Literal>(std::move(*lhs)),
                            .list = {},
                            .span = span};
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
    const Token& first = Peek();
    OrderItem item;
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
      auto operand = ParseOperand(Context::kOrderBy);
      if (!operand) {
        return std::unexpected(std::move(operand.error()));
      }
      item.span = SpanOf(*operand);
      if (auto* column = std::get_if<ColumnRef>(&*operand)) {
        item.expr = std::move(*column);
      } else {
        item.expr = std::get<Literal>(std::move(*operand));
      }
    }
    if (auto error = UnsupportedOperator(); error.has_value()) {
      return std::unexpected(std::move(*error));
    }
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
