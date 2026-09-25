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
  SourceSpan span;
};

struct Literal {
  enum class Kind : std::uint8_t { kInteger, kDecimal, kString, kDate };
  Kind kind = Kind::kInteger;
  bool negative = false;  // leading '-' (integers/decimals only)
  std::string text;       // number as written without the sign / unescaped string or date text
  SourceSpan span;
};

enum class CompareOp : std::uint8_t { kEq, kNe, kLt, kLe, kGt, kGe };

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
  std::optional<std::int64_t> limit;  // non-negative
  SourceSpan limit_span;              // LIMIT and its value (when limit)
  SourceSpan span;                    // SELECT .. last token of the query (without ';')
};

std::string_view ToString(AggKind kind);
std::string_view ToString(CompareOp op);

bool EqualIgnoringSpans(const SelectStatement& a, const SelectStatement& b);

}  // namespace antb1::sql
