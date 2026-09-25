#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "antb1/common/source_span.h"

namespace antb1::sql {

enum class TokenKind : std::uint8_t {
  kIdentifier,        // unquoted; text as written
  kQuotedIdentifier,  // "..." ; text unescaped ("" -> ")
  kString,            // '...' ; text unescaped ('' -> ')
  kInteger,           // digits only
  kDecimal,           // digits '.' digits (optionally with exponent)
  kStar,
  kComma,
  kLeftParen,
  kRightParen,
  kSemicolon,
  kDot,
  kPlus,
  kMinus,
  kSlash,
  kPercent,
  kEqual,         // =
  kNotEqual,      // <> or !=
  kLess,          // <
  kLessEqual,     // <=
  kGreater,       // >
  kGreaterEqual,  // >=
  kEnd,
};

struct Token {
  TokenKind kind = TokenKind::kEnd;
  std::string text;
  SourceSpan span;

  // Case-insensitive keyword match for kIdentifier tokens (keywords are not reserved in the lexer).
  [[nodiscard]] bool IsKeyword(std::string_view keyword) const;
};

std::string_view ToString(TokenKind kind);

}  // namespace antb1::sql
