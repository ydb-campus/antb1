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
//   query       := SELECT select_list FROM from_list [WHERE expr] [GROUP BY expr (',' expr)*]
//                  [HAVING expr] [ORDER BY order_item (',' order_item)*]
//                  [LIMIT integer] [OFFSET integer]      (LIMIT and OFFSET in either order)
//   select_list := '*' | expr [[AS] identifier] (',' expr [[AS] identifier])*
//   order_item  := expr [ASC | DESC] [NULLS (FIRST | LAST)]
//   from_list   := from_item (',' from_item | CROSS JOIN from_item | [INNER] JOIN from_item ON expr
//                  | LEFT [OUTER] JOIN from_item ON expr)*
//   from_item   := (identifier | quoted_identifier | string_literal) [[AS] alias]
//   alias       := identifier | quoted_identifier, and after AS also a string_literal
//   expr        := precedence climbing over, from loosest to tightest: OR; AND; NOT; comparisons,
//                  [NOT] LIKE, [NOT] IN and [NOT] BETWEEN (not chained); + -; * / // %; unary -;
//                  postfix. BETWEEN's bounds are additive expressions, so it consumes its own AND.
//   postfix     := primary ('::' type)*
//   primary     := column_ref | literal | '(' expr ')' | agg_call | name '(' [expr (',' expr)*] ')'
//                | CASE [expr] (WHEN expr THEN expr)+ [ELSE expr] END | EXTRACT '(' field FROM expr
//                ')' | (CAST | TRY_CAST) '(' expr AS type ')'
//   column_ref  := [name '.'] name
//   agg_call    := COUNT '(' '*' ')' | COUNT '(' DISTINCT expr ')'
//                | (COUNT | SUM | AVG | MIN | MAX) '(' expr ')'
//   literal     := ['-'] integer | ['-'] decimal | string_literal | (DATE | TIMESTAMP)
//                  string_literal          ('-' joins the number only when no '::' follows it)
//   type        := name ['(' integer (',' integer)* ')']
//
// The parser keeps expressions as written; what the engine answers is the binder's decision. The
// FROM list is flat (ADR 0022): each item records its connector to the items before it, and the
// list is collected in a loop. The ON, WHERE and HAVING predicates are split at their top-level
// AND chain, collected in a loop too.
// Recursion is bounded: every level of an expression tree (but that chain) counts against
// kMaxDepth, and so do the levels that the canonical form of a cast, a unary minus or a NOT
// operand adds (ToSql writes x::T as CAST(x AS T), -x as -(x) and a = NOT b as a = (NOT b)); a
// predicate with a top-level OR is written bare, so it reads back as it was parsed. Tokens are
// pulled lazily from the lexer (at most three tokens of lookahead), so work and memory stop at the
// first error whatever the input.
// Recognized SQL outside the grammar yields kUnsupported at its first offending token and names the
// construct; anything else yields kSyntax. A lexer error among the tokens the parser looked at wins
// over the parser's own verdict, which may have been reached on the placeholder end-of-input token.

namespace antb1::sql {
namespace {

template <typename T>
using Expected = std::expected<T, ParseError>;
using Status = std::expected<void, ParseError>;

// Where an operand is parsed; selects the error wording and whether aggregates are allowed.
enum class Context : std::uint8_t {
  kSelect,
  kAggregateArg,
  kOn,
  kWhere,
  kGroupBy,
  kHaving,
  kOrderBy
};

// Binding powers of the operators (see the grammar above).
constexpr int kOrPrecedence = 1;
constexpr int kAndPrecedence = 2;
constexpr int kNotPrecedence = 3;
constexpr int kComparisonPrecedence = 4;
constexpr int kAdditivePrecedence = 5;
constexpr int kMultiplicativePrecedence = 6;
constexpr int kUnaryPrecedence = 7;

// The deepest expression tree the parser builds.
constexpr std::size_t kMaxDepth = kMaxExpressionDepth;

struct Construct {
  std::string_view keyword;
  std::string_view message;
};

// Identifiers longer than every keyword below are never keywords (checked below): AUTHORIZATION.
constexpr std::size_t kMaxKeywordLength = 13;

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

// Clauses that can follow the select list, the FROM list, the predicate or LIMIT. The join keywords
// are not among them: the FROM list parses them, and elsewhere they are syntax errors, as in
// DuckDB.
constexpr auto kUnsupportedClauses = std::to_array<Construct>({
    {.keyword = "EXCEPT", .message = "EXCEPT is not supported"},
    {.keyword = "FETCH", .message = "FETCH is not supported"},
    {.keyword = "FOR", .message = "FOR UPDATE/SHARE is not supported"},
    {.keyword = "INTERSECT", .message = "INTERSECT is not supported"},
    {.keyword = "QUALIFY", .message = "QUALIFY is not supported"},
    {.keyword = "UNION", .message = "UNION is not supported"},
    {.keyword = "USING", .message = "USING SAMPLE is not supported"},
    {.keyword = "WINDOW", .message = "WINDOW is not supported"},
});

// Keywords that start an expression outside the subset.
constexpr auto kUnsupportedOperandKeywords = std::to_array<Construct>({
    {.keyword = "ALL", .message = "ALL (quantified comparisons) is not supported"},
    {.keyword = "ANY", .message = "ANY (quantified comparisons) is not supported"},
    {.keyword = "ARRAY", .message = "ARRAY is not supported"},
    {.keyword = "EXISTS", .message = "EXISTS (subqueries) is not supported"},
    {.keyword = "FALSE", .message = "boolean literals (TRUE/FALSE) are not supported"},
    {.keyword = "INTERVAL", .message = "INTERVAL is not supported"},
    {.keyword = "NULL", .message = "NULL literals are not supported"},
    {.keyword = "SOME", .message = "SOME (quantified comparisons) is not supported"},
    {.keyword = "TRUE", .message = "boolean literals (TRUE/FALSE) are not supported"},
});

// Keywords that continue a complete operand into an expression outside the subset. They are
// checked before an implicit alias, so "SELECT a ISNULL FROM t" is never read as "a AS isnull".
// (AT TIME ZONE is one too: see UnsupportedOperator.)
constexpr auto kUnsupportedOperatorKeywords = std::to_array<Construct>({
    {.keyword = "COLLATE", .message = "COLLATE is not supported"},
    {.keyword = "GLOB", .message = "GLOB is not supported"},
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
    {.keyword = "TIMESTAMPTZ", .message = "TIMESTAMPTZ literals are not supported"},
});

// Type names of two or more words (DuckDB): their first two words.
struct WordPair {
  std::string_view first;
  std::string_view second;
};
constexpr auto kMultiWordTypes = std::to_array<WordPair>({
    {.first = "BIT", .second = "VARYING"},
    {.first = "CHAR", .second = "VARYING"},
    {.first = "CHARACTER", .second = "VARYING"},
    {.first = "DOUBLE", .second = "PRECISION"},
    {.first = "NATIONAL", .second = "CHAR"},
    {.first = "NATIONAL", .second = "CHARACTER"},
    {.first = "NCHAR", .second = "VARYING"},
    {.first = "TIME", .second = "WITH"},
    {.first = "TIME", .second = "WITHOUT"},
    {.first = "TIMESTAMP", .second = "WITH"},
    {.first = "TIMESTAMP", .second = "WITHOUT"},
});

// What a word of kNotAnAlias means right after a FROM item in DuckDB, decided by the tokens that
// follow it.
enum class Follows : std::uint8_t {
  kNothing,   // no meaning there
  kJoin,      // JOIN: SEMI JOIN, ANTI JOIN, POSITIONAL JOIN
  kAsofJoin,  // JOIN, or a join kind (kJoinKinds) and JOIN: ASOF [INNER | LEFT [OUTER] | ...] JOIN
  kParen,     // '(': AT (VERSION => 1), PIVOT (...)
  kUnpivot,   // '(', INCLUDE or EXCLUDE: UNPIVOT [INCLUDE NULLS] (...)
  kSample,    // a number, '(' or a name and '(': TABLESAMPLE 10%, TABLESAMPLE reservoir(10)
};

struct NotAnAlias {
  std::string_view keyword;
  Follows follows = Follows::kNothing;
  std::string_view construct;  // the kUnsupported message where the word has its meaning
  bool after_on = false;       // the meaning also exists right after an ON condition
};

// The words that DuckDB 1.5.5 refuses as table aliases (implicit or after AS) and antb1 does not
// reserve (sorted; checked below). They stay out of kReservedWords, which decides where a name
// needs quoting (result names, divergence D8): they remain column, table and function names and
// select aliases (divergence D21). After a FROM item such a word is never an alias (read as one,
// SEMI and ANTI would run an inner join instead of DuckDB's semi or anti join): kUnsupported where
// DuckDB gives it a meaning there, else a syntax error, as in DuckDB (ADR 0022). After an ON
// condition GLOB, ISNULL, NOTNULL and AT TIME ZONE continue the expression (UnsupportedOperator).
constexpr auto kNotAnAlias = std::to_array<NotAnAlias>({
    {.keyword = "ANALYSE"},
    {.keyword = "ANALYZE"},
    {.keyword = "ANTI",
     .follows = Follows::kJoin,
     .construct = "ANTI JOIN is not supported",
     .after_on = true},
    {.keyword = "ASOF",
     .follows = Follows::kAsofJoin,
     .construct = "ASOF JOIN is not supported",
     .after_on = true},
    {.keyword = "ASYMMETRIC"},
    {.keyword = "AT", .follows = Follows::kParen, .construct = "AT (time travel) is not supported"},
    {.keyword = "AUTHORIZATION"},
    {.keyword = "BINARY"},
    {.keyword = "BOTH"},
    {.keyword = "CHECK"},
    {.keyword = "COLLATION"},
    {.keyword = "COLUMN"},
    {.keyword = "CONCURRENTLY"},
    {.keyword = "CONSTRAINT"},
    {.keyword = "CREATE"},
    {.keyword = "DEFAULT"},
    {.keyword = "DEFERRABLE"},
    {.keyword = "DESCRIBE"},
    {.keyword = "DO"},
    {.keyword = "FOREIGN"},
    {.keyword = "FREEZE"},
    {.keyword = "GLOB"},
    {.keyword = "INITIALLY"},
    {.keyword = "ISNULL"},
    {.keyword = "LAMBDA"},
    {.keyword = "LEADING"},
    {.keyword = "NOTNULL"},
    {.keyword = "ONLY"},
    {.keyword = "OVERLAPS"},
    {.keyword = "PIVOT",
     .follows = Follows::kParen,
     .construct = "PIVOT is not supported",
     .after_on = true},
    {.keyword = "PIVOT_LONGER"},
    {.keyword = "PIVOT_WIDER"},
    {.keyword = "PLACING"},
    {.keyword = "POSITIONAL",
     .follows = Follows::kJoin,
     .construct = "POSITIONAL JOIN is not supported",
     .after_on = true},
    {.keyword = "PRIMARY"},
    {.keyword = "REFERENCES"},
    {.keyword = "RETURNING"},
    {.keyword = "SEMI",
     .follows = Follows::kJoin,
     .construct = "SEMI JOIN is not supported",
     .after_on = true},
    {.keyword = "SHOW"},
    {.keyword = "SUMMARIZE"},
    {.keyword = "SYMMETRIC"},
    {.keyword = "TABLESAMPLE",
     .follows = Follows::kSample,
     .construct = "TABLESAMPLE is not supported"},
    {.keyword = "TO"},
    {.keyword = "TRAILING"},
    {.keyword = "UNIQUE"},
    {.keyword = "UNPACK"},
    {.keyword = "UNPIVOT",
     .follows = Follows::kUnpivot,
     .construct = "UNPIVOT is not supported",
     .after_on = true},
    {.keyword = "VARIADIC"},
    {.keyword = "VERBOSE"},
});

// Reserved words that DuckDB accepts as table aliases, with or without AS (checked below), and as
// table names and qualifiers. antb1 takes them as table aliases; as qualifiers they are
// unsupported (ReservedQualifier), as table names an error (divergence D21). ToSql prints every
// alias quoted, so the canonical form never writes them bare.
constexpr auto kAliasKeywords =
    std::to_array<std::string_view>({"BETWEEN", "EXISTS", "INTERVAL", "OVER"});

// The words after NATURAL or ASOF that start a join of DuckDB's (NATURAL SEMI JOIN, ASOF LEFT JOIN;
// IncompleteJoinKind checks the rest).
constexpr auto kJoinKinds =
    std::to_array<std::string_view>({"ANTI", "FULL", "INNER", "JOIN", "LEFT", "RIGHT", "SEMI"});

// The reserved words that start a join after a FROM item (ParseJoinKeywords).
constexpr auto kJoinStarts =
    std::to_array<std::string_view>({"CROSS", "FULL", "INNER", "JOIN", "LEFT", "NATURAL", "RIGHT"});

// The reserved words that DuckDB 1.5.5 also takes as function names (sorted; checked below): where
// a FROM item starts, one before '(' calls a table function (FROM t, left(1)).
constexpr auto kFunctionKeywords =
    std::to_array<std::string_view>({"CROSS", "FULL", "ILIKE", "INNER", "IS", "JOIN", "LEFT",
                                     "LIKE", "NATURAL", "OUTER", "OVER", "RIGHT", "SIMILAR"});

// The first words of a query in parentheses in FROM (a subquery), as DuckDB parses them; a '('
// starts one too.
constexpr auto kSubqueryStarts =
    std::to_array<std::string_view>({"DESCRIBE", "FROM", "PIVOT", "SELECT", "SHOW", "SUMMARIZE",
                                     "TABLE", "UNPIVOT", "VALUES", "WITH"});

constexpr bool IsReservedWordOf(std::string_view word) {
  return std::ranges::binary_search(kReservedWords, word);
}
static_assert(kNotAnAlias.size() == 49, "DuckDB 1.5.5 refuses 49 such words as table aliases");
static_assert(std::ranges::is_sorted(kNotAnAlias, {}, &NotAnAlias::keyword));
static_assert(std::ranges::none_of(kNotAnAlias, IsReservedWordOf, &NotAnAlias::keyword),
              "a word that cannot be an alias is no reserved word");
static_assert(std::ranges::all_of(kAliasKeywords, IsReservedWordOf));

constexpr bool FitsKeywordLength(std::string_view word) { return word.size() <= kMaxKeywordLength; }
static_assert(std::ranges::all_of(kMultiWordTypes, FitsKeywordLength, &WordPair::first));
static_assert(std::ranges::all_of(kMultiWordTypes, FitsKeywordLength, &WordPair::second));
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
static_assert(std::ranges::all_of(kNotAnAlias, FitsKeywordLength, &NotAnAlias::keyword));
static_assert(std::ranges::all_of(kJoinKinds, FitsKeywordLength));
static_assert(std::ranges::all_of(kJoinStarts, IsReservedWordOf));
static_assert(std::ranges::is_sorted(kFunctionKeywords));
static_assert(std::ranges::all_of(kFunctionKeywords, IsReservedWordOf));
static_assert(std::ranges::all_of(kSubqueryStarts, FitsKeywordLength));

// The kUnsupported message of a typed literal (INT '1', main.integer '1') or a prefixed string.
constexpr std::string_view kTypedLiterals =
    "typed literals other than DATE '...', TIMESTAMP '...' and prefixed strings (E'...') are not "
    "supported";

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

std::string AsciiUpper(std::string_view text) {
  std::string upper(text);
  for (char& c : upper) {
    if (c >= 'a' && c <= 'z') {
      c = static_cast<char>(c - 'a' + 'A');
    }
  }
  return upper;
}

// Upper-cased text of an unquoted identifier short enough to be a keyword; "" otherwise.
std::string KeywordOf(const Token& token) {
  if (token.kind != TokenKind::kIdentifier || token.text.size() > kMaxKeywordLength) {
    return {};
  }
  return AsciiUpper(token.text);
}

bool IsReservedKeyword(std::string_view keyword) {
  return !keyword.empty() && std::ranges::binary_search(kReservedWords, keyword);
}

// A token usable as a name: quoted identifier or non-reserved unquoted identifier.
bool IsName(const Token& token) {
  return token.kind == TokenKind::kQuotedIdentifier ||
         (token.kind == TokenKind::kIdentifier && !IsReservedKeyword(KeywordOf(token)));
}

// The entry of kNotAnAlias for an unquoted identifier, or nullptr.
const NotAnAlias* FindNotAnAlias(const Token& token) {
  const std::string keyword = KeywordOf(token);
  if (keyword.empty()) {
    return nullptr;
  }
  const auto* it = std::ranges::lower_bound(kNotAnAlias, keyword, {}, &NotAnAlias::keyword);
  return it != kNotAnAlias.end() && it->keyword == keyword ? it : nullptr;
}

// A token that can be a table alias: a name other than the words of kNotAnAlias, one of the
// reserved words of kAliasKeywords, and (`after_as`) a non-empty string literal.
bool IsTableAlias(const Token& token, bool after_as) {
  if (token.kind == TokenKind::kString) {
    return after_as && !token.text.empty();
  }
  if (FindNotAnAlias(token) != nullptr) {
    return false;
  }
  return IsName(token) || Contains(kAliasKeywords, KeywordOf(token));
}

// A token that starts a FROM item's relation (after ONLY): a name, a string or a word that
// DuckDB accepts as a table name (kAliasKeywords).
bool StartsRelation(const Token& token) {
  return token.kind == TokenKind::kString || IsName(token) ||
         Contains(kAliasKeywords, KeywordOf(token));
}

// A token that can name a table function before its '(': a name or a word of kFunctionKeywords.
bool IsFunctionName(const Token& token) {
  return IsName(token) || Contains(kFunctionKeywords, KeywordOf(token));
}

// A token that DuckDB accepts before the dot of a qualified name: a name or a word of
// kAliasKeywords.
bool IsQualifier(const Token& token) {
  return IsName(token) || Contains(kAliasKeywords, KeywordOf(token));
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

// A word of kAliasKeywords before a dot, which DuckDB takes for a qualifier: of a column (over.a),
// a table (FROM t, over.x) or a table function (FROM over.f(1)).
std::unexpected<ParseError> ReservedQualifier(SourceSpan span, std::string_view keyword) {
  return Unsupported(span, "the reserved word " + std::string(keyword) +
                               " as a qualifier is not supported; write it as a quoted identifier");
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
    if (auto status = ParseFromList(stmt); !status) {
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
    if (keyword == "LIKE" || keyword == "IN" || keyword == "BETWEEN") {
      return kComparisonPrecedence;
    }
    if (keyword == "NOT") {
      const std::string after = KeywordOf(PeekAt(1));
      return after == "LIKE" || after == "IN" || after == "BETWEEN" ? kComparisonPrecedence : 0;
    }
    return 0;
  }

  // Every level of the tree (a nested expression, or one more operator in a chain) counts
  // against kMaxDepth, so that the trees stay shallow enough for the recursive code that walks them
  // (copying, comparing, unparsing, binding, destroying).
  std::optional<ParseError> Deeper() {
    if (++depth_ > kMaxDepth) {
      return DepthError(Peek().span);
    }
    peak_ = std::max(peak_, depth_);
    return std::nullopt;
  }

  static ParseError DepthError(SourceSpan span) {
    return UnsupportedError(span, std::format("expressions deeper than {} levels (operators or "
                                              "parentheses) are not supported",
                                              kMaxDepth));
  }

  // One more level for everything parsed since `outer_peak` was saved (the canonical form of the
  // node at `span` wraps it), or the depth error at `span`; then the outer peak includes it.
  std::optional<ParseError> CanonicalLevel(std::size_t outer_peak, SourceSpan span) {
    if (++peak_ > kMaxDepth) {
      return DepthError(span);
    }
    peak_ = std::max(outer_peak, peak_);
    return std::nullopt;
  }

  // A new root over a subtree whose deepest level was `lower_peak`, next to what was parsed since
  // (whose peak is the current one): the subtree moves one level down. The depth error at `span`
  // when that is too deep.
  std::optional<ParseError> Lifted(std::size_t lower_peak, SourceSpan span) {
    if (lower_peak + 1 > kMaxDepth) {
      return DepthError(span);
    }
    peak_ = std::max(lower_peak + 1, peak_);
    return std::nullopt;
  }

  // An expression whose operators all bind at least as tightly as `min_precedence`, one level
  // below the current one; afterwards the peak includes its tree.
  Expected<Expr> ParseExpr(Context context, int min_precedence = kOrPrecedence) {
    const std::size_t depth = depth_;
    const std::size_t outer_peak = peak_;
    auto result = ParseExprAtDepth(context, min_precedence);
    depth_ = depth;
    peak_ = std::max(outer_peak, peak_);
    return result;
  }

  Expected<Expr> ParseExprAtDepth(Context context, int min_precedence) {
    if (auto error = Deeper(); error.has_value()) {
      return std::unexpected(std::move(*error));
    }
    peak_ = depth_;  // the deepest level of this frame's tree so far
    auto lhs = ParsePrefix(context, min_precedence);
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
      // The operator becomes the root at this level: the tree so far moves one level down, and
      // the right operand is parsed as its child (one level deeper).
      const SourceSpan op = Peek().span;
      const std::size_t lhs_peak = peak_;
      if (lhs_peak + 1 > kMaxDepth) {
        return std::unexpected(DepthError(op));  // before the right operand: the first error wins
      }
      peak_ = depth_;
      auto combined = ParseInfix(context, *std::move(lhs), precedence);
      if (!combined) {
        return combined;
      }
      if (auto error = Lifted(lhs_peak, op); error.has_value()) {
        return std::unexpected(std::move(*error));
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
    if (keyword == "LIKE" || keyword == "IN" || keyword == "BETWEEN" || keyword == "NOT") {
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
      if (op.IsKeyword("BETWEEN")) {
        return ParseBetween(context, std::move(lhs), negated, op_span);
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

  // low AND high, positioned after [NOT] BETWEEN. The bounds bind tighter than comparisons (as the
  // LIKE pattern does), so the AND is BETWEEN's own, and a conjunction may follow.
  Expected<Expr> ParseBetween(Context context, Expr lhs, bool negated, SourceSpan op_span) {
    auto low = ParseExpr(context, kAdditivePrecedence);
    if (!low) {
      return low;
    }
    if (!Peek().IsKeyword("AND")) {
      return Syntax(Peek().span, "expected AND in BETWEEN, found " + Describe(Peek()));
    }
    Take();
    auto high = ParseExpr(context, kAdditivePrecedence);
    if (!high) {
      return high;
    }
    const SourceSpan span = Cover(lhs.span(), high->span());
    return Expr(BetweenExpr{.operand = Box<Expr>(std::move(lhs)),
                            .low = Box<Expr>(*std::move(low)),
                            .high = Box<Expr>(*std::move(high)),
                            .negated = negated,
                            .op_span = op_span,
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

  // NOT expr, - expr, or a primary expression, in a frame that binds at `min_precedence`.
  Expected<Expr> ParsePrefix(Context context, int min_precedence) {
    const Token& token = Peek();
    if (token.IsKeyword("NOT")) {
      const SourceSpan op = Take().span;
      // Where NOT binds tighter than NOT does (the right operand of a comparison or of arithmetic,
      // a LIKE pattern, a BETWEEN bound), ToSql writes (NOT x): one level more, unless a unary
      // minus already wraps it as -(NOT x).
      const bool wrapped = min_precedence > kNotPrecedence && !minus_operand_;
      minus_operand_ = false;
      const std::size_t outer_peak = std::exchange(peak_, depth_);
      auto operand = ParseExpr(context, kNotPrecedence);
      if (!operand) {
        return operand;
      }
      if (!wrapped) {
        peak_ = std::max(outer_peak, peak_);
      } else if (auto error = CanonicalLevel(outer_peak, op); error.has_value()) {
        return std::unexpected(std::move(*error));
      }
      const SourceSpan span = Cover(op, operand->span());
      return Expr(UnaryExpr{.op = UnaryOp::kNot,
                            .operand = Box<Expr>(*std::move(operand)),
                            .op_span = op,
                            .span = span});
    }
    minus_operand_ = false;
    if (token.kind == TokenKind::kMinus) {
      const TokenKind next = PeekAt(1).kind;
      // -1::INTEGER is -(CAST(1 AS INTEGER)), as in DuckDB: '::' binds tighter than the minus.
      if ((next == TokenKind::kInteger || next == TokenKind::kDecimal) &&
          PeekAt(2).kind != TokenKind::kDoubleColon) {
        const SourceSpan minus = Take().span;
        Token digits = Take();
        return Expr(Literal{.kind = LiteralKindOf(next),
                            .negative = true,
                            .text = std::move(digits.text),
                            .span = Cover(minus, digits.span)});
      }
      const SourceSpan op = Take().span;
      const bool open = Peek().kind == TokenKind::kLeftParen;
      const std::size_t open_offset = Peek().span.offset;
      const std::size_t outer_peak = std::exchange(peak_, depth_);
      minus_operand_ = true;
      auto operand = ParseExpr(context, kUnaryPrecedence);
      minus_operand_ = false;
      if (!operand) {
        return operand;
      }
      // ToSql writes -(operand): one level more, unless the operand is written in parentheses
      // already (its node then starts inside them; a cast of a parenthesized operand starts at
      // the '(' and gets parentheses anew).
      if (open && operand->span().offset != open_offset) {
        peak_ = std::max(outer_peak, peak_);
      } else if (auto error = CanonicalLevel(outer_peak, op); error.has_value()) {
        return std::unexpected(std::move(*error));
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
    return ParsePostfix(context);
  }

  // A primary expression and its casts with '::' (x::T::U), which bind tighter than any operator.
  // A cast's span starts at the primary's first token, its '(' included.
  Expected<Expr> ParsePostfix(Context context) {
    const std::size_t begin = Peek().span.offset;
    const std::size_t outer_peak = std::exchange(peak_, depth_);
    auto expr = ParsePrimary(context);
    while (expr && Peek().kind == TokenKind::kDoubleColon) {
      const SourceSpan op = Peek().span;
      // ToSql writes CAST(<operand> AS T): one level more for the operand.
      if (++peak_ > kMaxDepth) {
        return std::unexpected(DepthError(op));
      }
      Take();
      auto type = ParseTypeName();
      if (!type) {
        return std::unexpected(std::move(type.error()));
      }
      expr = Expr(CastExpr{.operand = Box<Expr>(*std::move(expr)),
                           .type = std::move(type->name),
                           .type_params = std::move(type->params),
                           .try_cast = false,
                           .op_span = op,
                           .type_span = type->span,
                           .span = SourceSpan{.offset = begin, .length = last_end_ - begin}});
    }
    peak_ = std::max(outer_peak, peak_);
    return expr;
  }

  Expected<Expr> ParsePrimary(Context context) {
    const Token& token = Peek();
    switch (token.kind) {
      case TokenKind::kIdentifier:
        return ParseIdentifierPrimary(context);
      case TokenKind::kQuotedIdentifier: {
        if (PeekAt(1).kind == TokenKind::kDot) {
          return ParseQualifiedColumn();
        }
        if (PeekAt(1).kind == TokenKind::kLeftParen) {
          return ParseFunction(context);
        }
        // A string after a quoted name makes a typed literal of a quoted type in DuckDB ("integer"
        // '5', "DATE" E'2020-01-01'): unsupported, as typed literals are.
        if (PeekAt(1).kind == TokenKind::kString || PrefixedStringAt(1) != PrefixedString::kNone) {
          return Unsupported(token.span, kTypedLiterals);
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
    if (PeekAt(1).kind == TokenKind::kDot) {
      if (Contains(kAliasKeywords, keyword)) {  // DuckDB accepts them as qualifiers
        return ReservedQualifier(token.span, keyword);
      }
      if (!IsReservedKeyword(keyword)) {
        return ParseQualifiedColumn();
      }
    }
    if (auto construct = Find(kUnsupportedOperandKeywords, keyword); construct.has_value()) {
      return Unsupported(token.span, *construct);
    }
    if (keyword == "CASE") {
      return ParseCase(context);
    }
    const Token& next = PeekAt(1);
    if ((keyword == "CAST" || keyword == "TRY_CAST") && next.kind == TokenKind::kLeftParen) {
      return ParseCast(context);
    }
    // A string after the name, also an escape or a dollar-quoted one, makes a typed literal in
    // DuckDB (INT '1', integer E'5', DATE $$2020-01-01$$), and E'\n' is one string itself. Only
    // DATE and TIMESTAMP before a plain string are supported. Every unreserved word is taken for a
    // type here, also one that DuckDB does not take for one (coalesce '5', a syntax error there).
    if (next.kind == TokenKind::kString || PrefixedStringAt(1) != PrefixedString::kNone) {
      if (next.kind == TokenKind::kString && (keyword == "DATE" || keyword == "TIMESTAMP")) {
        const SourceSpan date = Take().span;
        Token text = Take();
        return Expr(
            Literal{.kind = keyword == "DATE" ? Literal::Kind::kDate : Literal::Kind::kTimestamp,
                    .negative = false,
                    .text = std::move(text.text),
                    .span = Cover(date, text.span)});
      }
      if (auto construct = Find(kUnsupportedTypedLiterals, keyword); construct.has_value()) {
        return Unsupported(token.span, *construct);
      }
      if (!IsReservedKeyword(keyword)) {
        return Unsupported(token.span, kTypedLiterals);
      }
    }
    const bool function_like =
        !IsReservedKeyword(keyword) || keyword == "LEFT" || keyword == "RIGHT";
    if (next.kind == TokenKind::kLeftParen && function_like) {
      if (auto agg_kind = AggregateOf(token); agg_kind.has_value()) {
        switch (context) {
          case Context::kOn:
            return Syntax(token.span, "aggregate functions are not allowed in ON");
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

  // qualifier '.' name, positioned at the qualifier (a name; the next token is '.'). One qualifier
  // only, and the column a name that needs no quoting to be one.
  Expected<Expr> ParseQualifiedColumn() {
    Token qualifier = Take();
    Take();  // '.'
    const Token& name = Peek();
    if (name.kind == TokenKind::kStar) {
      return Unsupported(name.span, "qualified * (t.*) is not supported");
    }
    if (name.kind == TokenKind::kIdentifier && !IsName(name)) {
      return Unsupported(name.span, "the reserved word " + KeywordOf(name) +
                                        " after '.' is not supported; write it as a quoted "
                                        "identifier");
    }
    if (!IsName(name)) {
      return Syntax(name.span, "expected a column name after '.', found " + Describe(name));
    }
    Token column = Take();
    if (Peek().kind == TokenKind::kDot) {
      return Unsupported(Peek().span, "names of more than two parts (a.b.c) are not supported");
    }
    if (Peek().kind == TokenKind::kLeftParen) {
      return Unsupported(column.span,
                         "qualified function names and method calls (a.f()) are not supported");
    }
    const SourceSpan span = Cover(qualifier.span, column.span);
    // A string after the name makes a typed literal of a qualified type in DuckDB (main.integer
    // '5', main.mood E'x'): unsupported, as typed literals are.
    if (Peek().kind == TokenKind::kString || PrefixedStringAt() != PrefixedString::kNone) {
      return Unsupported(span, kTypedLiterals);
    }
    return Expr(ColumnRef{.name = std::move(column.text),
                          .quoted = column.kind == TokenKind::kQuotedIdentifier,
                          .span = span,
                          .qualifier = std::move(qualifier.text),
                          .qualifier_quoted = qualifier.kind == TokenKind::kQuotedIdentifier});
  }

  // DuckDB's string constants other than a plain string literal, at PeekAt(ahead) and the token
  // after it: an escape string (E'x'), which lexes as E and an adjacent string, and a dollar-quoted
  // string ($$x$$, $tag$x$tag$), which lexes as a $-token without a digit and an adjacent $-token.
  // $1, $x and the other prefixes (B'1', N'x') are none.
  enum class PrefixedString : std::uint8_t { kNone, kEscape, kDollarQuoted };
  PrefixedString PrefixedStringAt(std::size_t ahead = 0) {
    const Token& token = PeekAt(ahead);
    const Token& next = PeekAt(ahead + 1);
    if (next.span.offset != token.span.offset + token.span.length) {
      return PrefixedString::kNone;
    }
    if (KeywordOf(token) == "E" && next.kind == TokenKind::kString) {
      return PrefixedString::kEscape;
    }
    if (token.kind == TokenKind::kParameter && next.kind == TokenKind::kParameter &&
        (token.text.size() == 1 || token.text[1] < '0' || token.text[1] > '9')) {
      return PrefixedString::kDollarQuoted;
    }
    return PrefixedString::kNone;
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
  // DuckDB has for some functions (position('a' IN s), substring(s FROM 1), ...) is not a syntax
  // error but an unsupported function call, and so is a FILTER clause.
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

  // CAST(operand AS type) or TRY_CAST(operand AS type), positioned at the keyword (the next token
  // is '(').
  Expected<Expr> ParseCast(Context context) {
    const Token keyword = Take();
    const std::string name = KeywordOf(keyword);
    Take();  // '('
    auto operand = ParseExpr(context);
    if (!operand) {
      return operand;
    }
    if (!Peek().IsKeyword("AS")) {
      return Syntax(Peek().span,
                    "expected AS in " + name + "(x AS type), found " + Describe(Peek()));
    }
    Take();
    auto type = ParseTypeName();
    if (!type) {
      return std::unexpected(std::move(type.error()));
    }
    if (Peek().kind != TokenKind::kRightParen) {
      return Syntax(Peek().span, "expected ) to close " + name + "(, found " + Describe(Peek()));
    }
    const SourceSpan span = Cover(keyword.span, Take().span);
    return Expr(CastExpr{.operand = Box<Expr>(*std::move(operand)),
                         .type = std::move(type->name),
                         .type_params = std::move(type->params),
                         .try_cast = name == "TRY_CAST",
                         .op_span = keyword.span,
                         .type_span = type->span,
                         .span = span});
  }

  struct TypeName {
    std::string name;  // upper-cased
    std::vector<std::string> params;
    SourceSpan span;
  };

  // The second word of a type name of several words (DOUBLE PRECISION), the error at both words.
  std::optional<ParseError> MultiWordType(std::string_view first, SourceSpan first_span) {
    const Token& next = Peek();
    const std::string second = KeywordOf(next);
    const bool pair = std::ranges::any_of(
        kMultiWordTypes, [&](const WordPair& p) { return p.first == first && p.second == second; });
    if (!pair) {
      return std::nullopt;
    }
    return UnsupportedError(Cover(first_span, next.span),
                            "type names of more than one word are not supported");
  }

  // The type of a cast, after AS or '::': a name with optional integer parameters, DECIMAL(15, 2).
  // DuckDB's other type syntax is unsupported; anything else is a syntax error.
  Expected<TypeName> ParseTypeName() {
    const Token& token = Peek();
    if (token.kind == TokenKind::kQuotedIdentifier) {
      return Unsupported(token.span, "quoted type names are not supported");
    }
    if (token.kind != TokenKind::kIdentifier) {
      return Syntax(token.span, "expected a type name, found " + Describe(token));
    }
    const std::string keyword = KeywordOf(token);
    if (keyword == "INTERVAL" || keyword == "UNION") {
      return Unsupported(token.span, "CAST to " + keyword + " is not supported");
    }
    if (IsReservedKeyword(keyword)) {
      return Syntax(token.span, "expected a type name, found keyword " + keyword);
    }
    const Token name = Take();
    TypeName out{.name = AsciiUpper(name.text), .params = {}, .span = name.span};
    if (auto error = MultiWordType(out.name, name.span); error.has_value()) {
      return std::unexpected(std::move(*error));
    }
    if (Peek().kind == TokenKind::kLeftParen) {
      Take();
      while (true) {
        const Token& param = Peek();
        if (param.kind == TokenKind::kRightParen || param.kind == TokenKind::kComma ||
            param.kind == TokenKind::kSemicolon || param.kind == TokenKind::kEnd) {
          return Syntax(param.span, "expected a type parameter, found " + Describe(param));
        }
        if (param.kind != TokenKind::kInteger) {
          return Unsupported(param.span, "type parameters other than integers are not supported");
        }
        out.params.push_back(Take().text);
        if (Peek().kind == TokenKind::kRightParen) {
          out.span = Cover(out.span, Take().span);
          break;
        }
        if (Peek().kind != TokenKind::kComma) {
          return Syntax(Peek().span,
                        "expected , or ) after a type parameter, found " + Describe(Peek()));
        }
        Take();
      }
      if (auto error = MultiWordType(out.name, name.span); error.has_value()) {
        return std::unexpected(std::move(*error));  // TIMESTAMP(3) WITH TIME ZONE
      }
    }
    if (Peek().kind == TokenKind::kLeftBracket || Peek().IsKeyword("ARRAY")) {
      return Unsupported(Peek().span, "array types are not supported");
    }
    if (Peek().kind == TokenKind::kDot) {
      return Unsupported(Peek().span, "qualified type names are not supported");
    }
    return out;
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
        return UnsupportedError(token.span, "CAST (::) of an IN condition is not supported");
      case TokenKind::kDot:  // a qualified name is one primary expression (ParseQualifiedColumn)
        return UnsupportedError(token.span,
                                "'.' after an expression (a field or a method) is not supported");
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
    if (keyword == "AT" && PeekAt(1).IsKeyword("TIME")) {
      return UnsupportedError(token.span, "AT TIME ZONE is not supported");
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
      if (after == "LIKE" || after == "IN" || after == "BETWEEN") {
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
    const std::size_t outer_peak = peak_;
    auto status = ParseConjunctsAtDepth(context, out);
    depth_ = depth;
    peak_ = std::max(outer_peak, peak_);
    return status;
  }

  Status ParseConjunctsAtDepth(Context context, std::vector<Expr>& out) {
    // Each conjunct is its own tree, one level below the clause; its peak is kept in case an OR
    // makes the chain one tree.
    std::vector<Expr> chain;
    std::vector<std::size_t> peaks;
    while (true) {
      peak_ = depth_;
      auto conjunct = ParseExpr(context, kNotPrecedence);
      if (!conjunct) {
        return std::unexpected(std::move(conjunct.error()));
      }
      chain.push_back(*std::move(conjunct));
      peaks.push_back(peak_);
      if (!Peek().IsKeyword("AND")) {
        break;
      }
      Take();
    }
    if (!Peek().IsKeyword("OR")) {
      peak_ = std::ranges::max(peaks);
      std::ranges::move(chain, std::back_inserter(out));
      return {};
    }
    auto lhs = Conjunction(std::move(chain), peaks);
    if (!lhs) {
      return std::unexpected(std::move(lhs.error()));
    }
    while (Peek().IsKeyword("OR")) {
      const Token op = Take();
      // Like the AND chain: each OR is the new root one level below the clause, over the tree so
      // far and its right operand, both one level down.
      const std::size_t lhs_peak = peak_;
      if (lhs_peak + 1 > kMaxDepth) {
        return std::unexpected(DepthError(op.span));  // before the right operand: first error wins
      }
      peak_ = depth_;
      auto rhs = ParseExpr(context, kAndPrecedence);
      if (!rhs) {
        return std::unexpected(std::move(rhs.error()));
      }
      if (auto error = Lifted(std::max(lhs_peak, peak_), op.span); error.has_value()) {
        return std::unexpected(std::move(*error));
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

  // The AND of `chain` as a left-deep tree, from the conjuncts' peaks (`peaks`): each AND is the
  // new root one level below the clause, over the tree so far and the next conjunct, both one
  // level down. Leaves the tree's peak.
  Expected<Expr> Conjunction(std::vector<Expr> chain, const std::vector<std::size_t>& peaks) {
    Expr tree = std::move(chain.front());
    peak_ = peaks.front();
    for (std::size_t i = 1; i < chain.size(); ++i) {
      const std::size_t lower = std::max(peak_, peaks[i]);
      if (auto error = Lifted(lower, chain[i].span()); error.has_value()) {
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

  // ---- the FROM list ----

  // from_list, positioned after FROM: the items in a loop, each with its connector and, for a
  // JOIN with ON, its condition. A JOIN binds tighter than a comma, so the list is never nested.
  Status ParseFromList(SelectStatement& stmt) {
    Connector connector = Connector::kFirst;
    SourceSpan connector_span;
    while (true) {
      auto item = ParseFromItem(connector, connector_span);
      if (!item) {
        return std::unexpected(std::move(item.error()));
      }
      if (connector == Connector::kInner || connector == Connector::kLeft) {
        if (auto status = ParseOn(*item); !status) {
          return status;
        }
      }
      stmt.from.push_back(*std::move(item));
      if (Peek().kind == TokenKind::kComma) {
        connector_span = Take().span;
        // DuckDB allows a trailing comma before the end and the clauses after FROM. Before FROM
        // or INTO it gives a syntax error, and so does ParseTableRef.
        if (const Token& next = Peek();
            EndsList(next) && !next.IsKeyword("FROM") && !next.IsKeyword("INTO")) {
          return Unsupported(connector_span, "a trailing comma in FROM is not supported");
        }
        connector = Connector::kComma;
        continue;
      }
      auto join = ParseJoinKeywords();
      if (!join) {
        return std::unexpected(std::move(join.error()));
      }
      if (!join->has_value()) {
        return {};
      }
      connector = (*join)->connector;
      connector_span = (*join)->span;
    }
  }

  struct JoinKeywords {
    Connector connector = Connector::kInner;
    SourceSpan span;  // CROSS JOIN, [INNER] JOIN or LEFT [OUTER] JOIN
  };

  // The join keywords at the next token, or std::nullopt when none starts there (the FROM list
  // ends). At most three tokens of lookahead (LEFT OUTER JOIN). The joins that DuckDB has and the
  // subset lacks are kUnsupported; a join keyword without the rest is a syntax error, as in DuckDB.
  Expected<std::optional<JoinKeywords>> ParseJoinKeywords() {
    if (auto error = IncompleteJoin()) {
      return std::unexpected(std::move(*error));
    }
    const Token& token = Peek();
    const std::string keyword = KeywordOf(token);
    const auto take = [this](Connector connector, std::size_t words) {
      const SourceSpan first = Take().span;
      SourceSpan last = first;
      for (std::size_t i = 1; i < words; ++i) {
        last = Take().span;
      }
      return std::optional(JoinKeywords{.connector = connector, .span = Cover(first, last)});
    };
    if (keyword == "JOIN") {
      return take(Connector::kInner, 1);
    }
    if (keyword == "INNER" || keyword == "CROSS") {
      return take(keyword == "INNER" ? Connector::kInner : Connector::kCross, 2);
    }
    if (keyword == "LEFT") {
      return take(Connector::kLeft, PeekAt(1).IsKeyword("OUTER") ? 3 : 2);
    }
    if (keyword == "RIGHT" || keyword == "FULL") {
      return Unsupported(token.span, keyword + " JOIN is not supported");
    }
    if (keyword == "NATURAL") {
      return Unsupported(token.span, "NATURAL JOIN is not supported");
    }
    if (keyword == "OUTER") {
      return Syntax(token.span, "expected LEFT, RIGHT or FULL before OUTER");
    }
    return std::nullopt;
  }

  // A syntax error when the join keyword at the next token lacks the rest of its join, as in
  // DuckDB; std::nullopt otherwise, also when no join keyword is there. Within three tokens: the
  // JOIN of NATURAL LEFT OUTER JOIN is beyond them.
  std::optional<ParseError> IncompleteJoin() {
    const std::string keyword = KeywordOf(Peek());
    if (keyword == "INNER" || keyword == "CROSS") {
      return ExpectJoinAt(1, keyword);
    }
    if (keyword == "LEFT" || keyword == "RIGHT" || keyword == "FULL") {
      return PeekAt(1).IsKeyword("OUTER") ? ExpectJoinAt(2, keyword + " OUTER")
                                          : ExpectJoinAt(1, keyword);
    }
    if (keyword == "NATURAL") {
      return IncompleteJoinKind();
    }
    return std::nullopt;
  }

  // After NATURAL or ASOF at the next token: a syntax error unless a join of DuckDB's follows,
  // JOIN, or INNER, SEMI or ANTI and then JOIN, or LEFT, RIGHT or FULL and then JOIN or OUTER (the
  // JOIN after OUTER is beyond the three tokens).
  std::optional<ParseError> IncompleteJoinKind() {
    const std::string word = KeywordOf(Peek());
    const std::string kind = KeywordOf(PeekAt(1));
    if (kind == "JOIN") {
      return std::nullopt;
    }
    if (!Contains(kJoinKinds, kind)) {
      return ExpectJoinAt(1, word);
    }
    if ((kind == "LEFT" || kind == "RIGHT" || kind == "FULL") && PeekAt(2).IsKeyword("OUTER")) {
      return std::nullopt;
    }
    return ExpectJoinAt(2, word + " " + kind);
  }

  // A syntax error unless PeekAt(ahead) is JOIN; `after` names the words before it.
  std::optional<ParseError> ExpectJoinAt(std::size_t ahead, const std::string& after) {
    const Token& token = PeekAt(ahead);
    if (token.IsKeyword("JOIN")) {
      return std::nullopt;
    }
    return SyntaxError(token.span, "expected JOIN after " + after + ", found " + Describe(token));
  }

  // from_item: a table or a path, then its alias. After an item a word of kNotAnAlias is never an
  // alias (ParseAlias).
  Expected<FromItem> ParseFromItem(Connector connector, SourceSpan connector_span) {
    auto table = ParseTableRef();
    if (!table) {
      return std::unexpected(std::move(table.error()));
    }
    FromItem item{.connector = connector,
                  .table = *std::move(table),
                  .alias = {},
                  .on = {},
                  .connector_span = connector_span,
                  .alias_span = {},
                  .on_span = {},
                  .span = {}};
    item.span = item.table.span;
    if (item.table.kind == TableRef::Kind::kName && Peek().kind == TokenKind::kDot) {
      return Unsupported(
          Peek().span,
          "qualified table names are not supported (quote file paths: 'dir/f.parquet')");
    }
    if (auto status = ParseAlias(item); !status) {
      return std::unexpected(std::move(status.error()));
    }
    return item;
  }

  Expected<TableRef> ParseTableRef() {
    const Token& token = Peek();
    switch (token.kind) {
      case TokenKind::kIdentifier:
      case TokenKind::kQuotedIdentifier: {
        const std::string keyword = KeywordOf(token);
        const Token& next = PeekAt(1);
        if (keyword == "LATERAL" && StartsLateralItem()) {
          return Unsupported(token.span, "LATERAL is not supported");
        }
        if (keyword == "ONLY" && StartsRelation(next)) {  // ONLY t; a table "only" otherwise
          return Unsupported(token.span, "ONLY is not supported");
        }
        if (next.kind == TokenKind::kLeftParen && IsFunctionName(token)) {
          return Unsupported(token.span, "table functions are not supported");
        }
        if (next.kind == TokenKind::kDot && Contains(kAliasKeywords, keyword)) {
          return ReservedQualifier(token.span, keyword);  // over.x, over.f(1)
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
      case TokenKind::kLeftParen: {
        // DuckDB's subqueries, else its joins in parentheses. A table alone in them, or before a
        // comma or the end, is a syntax error, as in DuckDB; anything else after the table is
        // taken for a join (as far as three tokens tell), and so is a call (also of a word of
        // kFunctionKeywords), a qualified name (also one qualified by a word of kAliasKeywords)
        // and LATERAL before '(' or what may start a function's name.
        const Token& next = PeekAt(1);
        if (next.kind == TokenKind::kLeftParen || Contains(kSubqueryStarts, KeywordOf(next))) {
          return Unsupported(token.span, "subqueries in FROM are not supported");
        }
        if (const Token& after = PeekAt(2);
            (IsFunctionName(next) && after.kind == TokenKind::kLeftParen) ||
            (IsQualifier(next) && after.kind == TokenKind::kDot) ||
            (next.IsKeyword("LATERAL") && (after.kind == TokenKind::kLeftParen ||
                                           IsFunctionName(after) || IsQualifier(after)))) {
          return Unsupported(token.span, "parenthesized joins in FROM are not supported");
        }
        if (next.kind != TokenKind::kString && !IsName(next)) {
          return Syntax(next.span,
                        "expected a table name, a quoted file path or a subquery after '(', "
                        "found " +
                            Describe(next));
        }
        if (const Token& after = PeekAt(2);
            after.kind == TokenKind::kRightParen || after.kind == TokenKind::kComma ||
            after.kind == TokenKind::kSemicolon || after.kind == TokenKind::kEnd) {
          return Syntax(after.span,
                        "expected a join after the table in parentheses, found " + Describe(after));
        }
        return Unsupported(token.span, "parenthesized joins in FROM are not supported");
      }
      default:
        return Syntax(token.span,
                      "expected a table name or a quoted file path, found " + Describe(token));
    }
  }

  // Whether the LATERAL at the next token starts one of DuckDB's LATERAL items, a subquery or a
  // table function, as far as three tokens tell: of LATERAL s.f(1) they show the name's first dot
  // (also after a word of kAliasKeywords: LATERAL over.f(1)).
  bool StartsLateralItem() {
    const Token& next = PeekAt(1);
    if (next.kind == TokenKind::kLeftParen) {
      return true;
    }
    const TokenKind after = PeekAt(2).kind;
    return (IsFunctionName(next) && after == TokenKind::kLeftParen) ||
           (IsQualifier(next) && after == TokenKind::kDot);
  }

  // [AS] alias after a FROM item's table, and what may not follow it: a word of kNotAnAlias is
  // never an alias, and after an alias a column alias list is unsupported.
  Status ParseAlias(FromItem& item) {
    const Token& token = Peek();
    if (token.IsKeyword("AS")) {
      const SourceSpan as = Take().span;
      const Token& name = Peek();
      if (name.kind == TokenKind::kString && name.text.empty()) {  // DuckDB allows it
        return Unsupported(name.span, "an empty table alias ('') is not supported");
      }
      // DuckDB also takes an escape string (E'x') or a dollar-quoted string ($$x$$, $tag$x$tag$) as
      // the alias here. $1, $x and the other prefixes (B'1', N'x') stay syntax errors, as in
      // DuckDB.
      switch (PrefixedStringAt()) {
        case PrefixedString::kEscape:
          return Unsupported(name.span, "prefixed strings (E'...') are not supported");
        case PrefixedString::kDollarQuoted:
          return Unsupported(name.span, "dollar-quoted strings are not supported");
        case PrefixedString::kNone:
          break;
      }
      if (!IsTableAlias(name, /*after_as=*/true)) {
        if (const NotAnAlias* word = FindNotAnAlias(name)) {
          return Syntax(name.span, NotAnAliasMessage(*word));
        }
        return Syntax(name.span, "expected a table alias after AS, found " + Describe(name));
      }
      SetAlias(item, as);
    } else if (const NotAnAlias* word = FindNotAnAlias(token)) {
      if (auto error = Meaning(*word, After::kItem)) {
        return std::unexpected(std::move(*error));
      }
      return Syntax(token.span, NotAnAliasMessage(*word));
    } else if (IsTableAlias(token, /*after_as=*/false)) {
      SetAlias(item, token.span);
    } else {
      return {};
    }
    if (Peek().kind == TokenKind::kLeftParen) {
      return Unsupported(Peek().span, "column alias lists (t AS a(x, y)) are not supported");
    }
    if (const NotAnAlias* word = FindNotAnAlias(Peek())) {
      if (auto error = Meaning(*word, After::kItem)) {
        return std::unexpected(std::move(*error));
      }
    }
    return {};
  }

  static std::string NotAnAliasMessage(const NotAnAlias& word) {
    return "a table alias cannot be the keyword " + std::string(word.keyword) +
           "; write it as a quoted identifier";
  }

  // Takes the alias at the next token: its spans run from `begin` (AS or the alias itself).
  void SetAlias(FromItem& item, SourceSpan begin) {
    Token alias = Take();
    item.alias = std::move(alias.text);
    item.alias_span = Cover(begin, alias.span);
    item.span = Cover(item.span, alias.span);
  }

  enum class After : std::uint8_t { kItem, kOn };

  // kUnsupported when the word of kNotAnAlias at the next token has its DuckDB meaning there (after
  // a FROM item, or after an ON condition), told by the tokens after it; std::nullopt otherwise.
  std::optional<ParseError> Meaning(const NotAnAlias& word, After after) {
    if (after == After::kOn && !word.after_on) {
      return std::nullopt;
    }
    const Token& next = PeekAt(1);
    bool means = false;
    switch (word.follows) {
      case Follows::kNothing:
        break;
      case Follows::kJoin:
        means = next.IsKeyword("JOIN");
        break;
      case Follows::kAsofJoin:
        means = !IncompleteJoinKind().has_value();
        break;
      case Follows::kParen:
        means = next.kind == TokenKind::kLeftParen;
        break;
      case Follows::kUnpivot:
        means = next.kind == TokenKind::kLeftParen || next.IsKeyword("INCLUDE") ||
                next.IsKeyword("EXCLUDE");
        break;
      case Follows::kSample:
        means = next.kind == TokenKind::kInteger || next.kind == TokenKind::kDecimal ||
                next.kind == TokenKind::kLeftParen ||
                (IsName(next) && PeekAt(2).kind == TokenKind::kLeftParen);
        break;
    }
    if (!means) {
      return std::nullopt;
    }
    return UnsupportedError(Peek().span, word.construct);
  }

  // ON and its condition, after the item of an [INNER] JOIN or a LEFT JOIN, split at its top-level
  // AND chain like WHERE.
  Status ParseOn(FromItem& item) {
    const Token& token = Peek();
    if (!token.IsKeyword("ON")) {
      if (token.IsKeyword("USING")) {
        return Unsupported(token.span,
                           "JOIN ... USING is not supported (write the condition with ON)");
      }
      if (Contains(kJoinStarts, KeywordOf(token))) {
        const SourceSpan join = token.span;
        if (auto error = IncompleteJoin()) {  // not a join after all: a syntax error, as in DuckDB
          return std::unexpected(std::move(*error));
        }
        return Unsupported(join,
                           "nested joins (a JOIN before the ON of an earlier JOIN) are not "
                           "supported");
      }
      return Syntax(token.span, "expected ON after the JOIN at offset " +
                                    std::to_string(item.connector_span.offset) + ", found " +
                                    Describe(token));
    }
    const std::size_t begin = Take().span.offset;
    if (auto status = ParseConjuncts(Context::kOn, item.on); !status) {
      return status;
    }
    item.on_span = SourceSpan{.offset = begin, .length = last_end_ - begin};
    if (const NotAnAlias* word = FindNotAnAlias(Peek())) {
      if (auto error = Meaning(*word, After::kOn)) {
        return std::unexpected(std::move(*error));
      }
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
          next.kind == TokenKind::kDoubleColon ||
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
    // A call or a typed literal, also of a quoted or a qualified name (abs(5), "abs"(5),
    // main.abs(5), integer '5', "integer" E'5', main.integer $$5$$, and E'5' itself). A column
    // (LIMIT a, LIMIT t.a), which DuckDB refuses when it binds, stays a syntax error.
    const SourceSpan first = token.span;
    const std::string found = Describe(token);
    SourceSpan span = first;
    if (IsName(token) && PeekAt(1).kind == TokenKind::kDot && IsName(PeekAt(2))) {
      // What follows the name is beyond the three tokens of lookahead: take the qualifier and the
      // dot first.
      Take();
      Take();
      span = Cover(first, Peek().span);
    }
    const Token& head = Peek();  // the name, or its part after the dot
    const bool call =
        (head.kind == TokenKind::kIdentifier || head.kind == TokenKind::kQuotedIdentifier) &&
        PeekAt(1).kind == TokenKind::kLeftParen;
    const bool typed_literal = IsName(head) && (PeekAt(1).kind == TokenKind::kString ||
                                                PrefixedStringAt(1) != PrefixedString::kNone);
    if (call || typed_literal) {
      return Unsupported(span,
                         name + " expressions are not supported (" + name + " takes an integer)");
    }
    return Syntax(first, "expected a non-negative integer after " + name + ", found " + found);
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
  std::size_t peak_ = 0;   // the deepest level reached, with the levels the canonical form adds
  bool minus_operand_ = false;  // the next prefix is a unary minus's operand, which -(x) wraps
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
