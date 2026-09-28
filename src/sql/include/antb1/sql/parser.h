#pragma once

#include <expected>
#include <string_view>

#include "antb1/sql/ast.h"
#include "antb1/sql/error.h"

namespace antb1::sql {

// Parses one statement of the grammar in docs/sql-subset.md; an optional trailing ';' is allowed.
// Expressions are parsed as written, whether or not the binder answers them. Recognized SQL outside
// the grammar (JOIN, window functions, subqueries, ...) yields ParseError::Kind::kUnsupported with
// the span of the first offending token and a message naming the construct; malformed input yields
// kSyntax. Any byte sequence is accepted as input: Parse never crashes, recurses at most as deep as
// its expression depth limit (256 levels, else kUnsupported), stops working at the first error, and
// every error span lies inside `text`.
std::expected<SelectStatement, ParseError> Parse(std::string_view text);

// Whether `word` (ASCII case-insensitive) is reserved: as a column, table or alias name it must be
// written as a quoted identifier ("from"). Used to quote names that need it, e.g. in result names.
bool IsReservedWord(std::string_view word);

}  // namespace antb1::sql
