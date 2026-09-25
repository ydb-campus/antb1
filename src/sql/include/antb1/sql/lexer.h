#pragma once

#include <cstddef>
#include <expected>
#include <string_view>
#include <vector>

#include "antb1/sql/error.h"
#include "antb1/sql/token.h"

namespace antb1::sql {

// Streaming tokenizer. Next() returns one token at a time and keeps returning kEnd once the input
// is exhausted; after an error the lexer is exhausted too. Skips whitespace, -- line comments
// (ended by \n or \r) and /* block */ comments (nested, as in PostgreSQL and DuckDB). Operators
// follow PostgreSQL's rules: a run of operator characters is one token, cut before an embedded
// comment start, and loses trailing '+'/'-' unless it contains one of ~ ! @ # % ^ & | ` ? (so
// "a<=-1" is a <= -1, while "a!=-1" uses the operator "!=-"). Unquoted identifiers are ASCII; any
// other name must be "double-quoted". Every byte sequence is handled (NUL, invalid UTF-8): the
// result is a token or an error whose span lies inside the text. Errors are kSyntax, except for
// valid SQL the subset does not support (0x1F, 1_000, unquoted non-ASCII names): kUnsupported.
class Lexer {
 public:
  explicit Lexer(std::string_view text) : text_(text) {}

  std::expected<Token, ParseError> Next();

 private:
  std::string_view text_;
  std::size_t pos_ = 0;
};

// Splits SQL text into tokens; the result always ends with a kEnd token.
std::expected<std::vector<Token>, ParseError> Tokenize(std::string_view text);

}  // namespace antb1::sql
