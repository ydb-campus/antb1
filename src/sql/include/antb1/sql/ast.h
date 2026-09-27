#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "antb1/common/source_span.h"

// AST of the supported SQL subset (docs/sql-subset.md). Every node carries the byte span of the
// source text it was parsed from; EqualIgnoringSpans() compares structure only (used by the
// Parse(ToSql(x)) == x round-trip property). Names keep their spelling (quoted ones unescaped); the
// binder matches them case-insensitively.

namespace antb1::sql {

struct ColumnRef {
  std::string name;
  bool quoted = false;
  SourceSpan span;
};

enum class AggKind : std::uint8_t { kCountStar, kCount, kSum, kAvg, kMin, kMax };

struct AggregateCall {
  AggKind kind = AggKind::kCountStar;
  std::optional<ColumnRef> arg;  // empty only for kCountStar
  bool distinct = false;         // COUNT(DISTINCT col); only kCount
  SourceSpan span;
};

struct Literal {
  enum class Kind : std::uint8_t { kInteger, kDecimal, kString, kDate };
  Kind kind = Kind::kInteger;
  bool negative = false;  // leading '-' (integers/decimals only)
  std::string text;       // number as written without the sign / unescaped string or date text
  SourceSpan span;
};

// kLike and kNotLike: column [NOT] LIKE 'pattern' (the pattern is always the literal, on the
// right).
enum class CompareOp : std::uint8_t { kEq, kNe, kLt, kLe, kGt, kGe, kLike, kNotLike };

// column <op> literal. A literal-first comparison is normalized by the parser (5 < c -> c > 5);
// the spans still point at the source text, and `span` covers both operands.
struct Comparison {
  ColumnRef column;
  CompareOp op = CompareOp::kEq;
  Literal literal;
  SourceSpan span;
};

using SelectExpr = std::variant<AggregateCall, ColumnRef>;

struct SelectItem {
  SelectExpr expr;
  std::optional<std::string> alias;  // as written (quoted aliases unescaped)
  SourceSpan span;                   // expression and alias
};

// NULLS FIRST / NULLS LAST as written; kDefault when omitted.
enum class NullsOrder : std::uint8_t { kDefault, kFirst, kLast };

// One ORDER BY item: a column (or a select alias, resolved by the binder) or an aggregate call.
struct OrderItem {
  SelectExpr expr;
  bool descending = false;  // DESC; ASC (written or not) is false
  NullsOrder nulls = NullsOrder::kDefault;
  SourceSpan span;  // expression and modifiers
};

struct TableRef {
  enum class Kind : std::uint8_t { kName, kPath };
  Kind kind = Kind::kName;
  std::string name;  // identifier, or the path from a string literal
  bool quoted = false;
  SourceSpan span;
};

struct SelectStatement {
  bool star = false;     // SELECT *
  SourceSpan star_span;  // the '*' (when star)
  std::vector<SelectItem> items;
  TableRef from;
  std::vector<Comparison> where;      // conjunction (AND)
  std::vector<ColumnRef> group_by;    // GROUP BY columns (a select alias is resolved by the binder)
  SourceSpan group_by_span;           // GROUP BY and its list (when group_by is not empty)
  std::vector<OrderItem> order_by;    // ORDER BY items
  SourceSpan order_by_span;           // ORDER BY and its list (when order_by is not empty)
  std::optional<std::int64_t> limit;  // non-negative
  SourceSpan limit_span;              // LIMIT and its value (when limit)
  std::optional<std::int64_t> offset;  // non-negative
  SourceSpan offset_span;              // OFFSET and its value (when offset)
  SourceSpan span;                     // SELECT .. last token of the query (without ';')
};

std::string_view ToString(AggKind kind);
std::string_view ToString(CompareOp op);
std::string_view ToString(NullsOrder nulls);  // "", "NULLS FIRST" or "NULLS LAST"

bool EqualIgnoringSpans(const SelectStatement& a, const SelectStatement& b);

}  // namespace antb1::sql
