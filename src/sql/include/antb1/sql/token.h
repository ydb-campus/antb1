#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "antb1/common/source_span.h"

namespace antb1::sql {

enum class TokenKind : std::uint8_t {
  kIdentifier,        // unquoted ASCII [A-Za-z_][A-Za-z0-9_]*; text as written
  kQuotedIdentifier,  // "..." ; text unescaped ("" -> "), never empty
  kString,            // '...' ; text unescaped ('' -> ')
  kInteger,           // digits only
  kDecimal,           // digits with '.' and/or an exponent (1.5, .5, 5., 1e3, 2.5E-3)
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
  kDoubleColon,   // :: (cast; lexed only so the parser can reject it as unsupported)
  kConcat,        // || (concatenation; lexed only so the parser can reject it as unsupported)
  // The kinds below are lexed only so the parser can reject them as unsupported.
  kOperator,     // any other PostgreSQL-style operator (~, !~, ^, &, |, <<, ==, ->, ?, ...)
  kParameter,    // $1, $name (and the start of a $$dollar-quoted$$ string)
  kLeftBracket,  // [ (list literal, subscript)
  kLeftBrace,    // { (struct literal)
  kEnd,
};

struct Token {
  TokenKind kind = TokenKind::kEnd;
  std::string text;
  SourceSpan span;

  // Case-insensitive (ASCII) keyword match for kIdentifier tokens; the lexer reserves no words.
  [[nodiscard]] bool IsKeyword(std::string_view keyword) const;
};

std::string_view ToString(TokenKind kind);

}  // namespace antb1::sql
