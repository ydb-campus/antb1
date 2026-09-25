#pragma once

#include <expected>
#include <string_view>

#include "antb1/sql/ast.h"
#include "antb1/sql/error.h"

namespace antb1::sql {

// Parses one statement of the supported subset (docs/sql-subset.md); an optional trailing ';' is
// allowed. SQL outside the subset yields ParseError::Kind::kUnsupported with the span of the first
// offending token.
std::expected<SelectStatement, ParseError> Parse(std::string_view text);

}  // namespace antb1::sql
