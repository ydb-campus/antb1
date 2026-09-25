#pragma once

#include <expected>
#include <string_view>
#include <vector>

#include "antb1/sql/error.h"
#include "antb1/sql/token.h"

namespace antb1::sql {

// Splits SQL text into tokens; the result always ends with a kEnd token.
// Supports -- line comments and /* block */ comments.
std::expected<std::vector<Token>, ParseError> Tokenize(std::string_view text);

}  // namespace antb1::sql
