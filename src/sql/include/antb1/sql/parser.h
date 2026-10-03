#pragma once

#include <cstddef>
#include <expected>
#include <string_view>

#include "antb1/sql/ast.h"
#include "antb1/sql/error.h"

namespace antb1::sql {

// The deepest expression tree Parse accepts: nodes on the longest path from a root to a leaf,
// counted together with the parentheses of the input and the levels its canonical form (ToSql)
// adds, so that the canonical form of an accepted statement parses too.
inline constexpr std::size_t kMaxExpressionDepth = 256;

// Parses one statement of the grammar in docs/sql-subset.md; an optional trailing ';' is allowed.
// Expressions are parsed as written, whether or not the binder answers them. Recognized SQL outside
// the grammar (JOIN, window functions, subqueries, ...) yields ParseError::Kind::kUnsupported with
// the span of the first offending token and a message naming the construct; malformed input yields
// kSyntax. Any byte sequence is accepted as input: Parse never crashes, recurses at most as deep as
// its expression depth limit (kMaxExpressionDepth levels, else kUnsupported), stops working at the
// first error, and every error span lies inside `text`.
std::expected<SelectStatement, ParseError> Parse(std::string_view text);

// Whether `word` (ASCII case-insensitive) is reserved: as a column, table or alias name it must be
// written as a quoted identifier ("from"). Used to quote names that need it, e.g. in result names.
bool IsReservedWord(std::string_view word);

}  // namespace antb1::sql
