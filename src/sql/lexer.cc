#include "antb1/sql/lexer.h"

#include <algorithm>
#include <cstddef>
#include <expected>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "antb1/common/source_span.h"
#include "antb1/common/utf8.h"
#include "antb1/sql/error.h"
#include "antb1/sql/token.h"

namespace antb1::sql {
namespace {

// ASCII-only character classes: tokenization must not depend on the C locale.
bool IsAsciiAlpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool IsDigit(char c) { return c >= '0' && c <= '9'; }
bool IsIdentStart(char c) { return IsAsciiAlpha(c) || c == '_'; }
bool IsIdentChar(char c) { return IsIdentStart(c) || IsDigit(c); }
bool IsSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}
char AsciiUpper(char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c; }
bool IsNewline(char c) { return c == '\n' || c == '\r'; }

// PostgreSQL's operator characters, and those that keep a trailing '+'/'-' in an operator.
constexpr std::string_view kOperatorChars = "~!@#^&|`?+-*/%<>=";
constexpr std::string_view kNonSqlOperatorChars = "~!@#^&|`?%";
bool IsOperatorChar(char c) { return kOperatorChars.contains(c); }
bool IsNonSqlOperatorChar(char c) { return kNonSqlOperatorChars.contains(c); }

bool IsDigitOfBase(char c, char base) {
  switch (AsciiUpper(base)) {
    case 'X':
      return IsDigit(c) || (AsciiUpper(c) >= 'A' && AsciiUpper(c) <= 'F');
    case 'O':
      return c >= '0' && c <= '7';
    default:  // 'B'
      return c == '0' || c == '1';
  }
}

std::unexpected<ParseError> Error(std::string message, std::size_t offset, std::size_t length) {
  return std::unexpected(ParseError{.kind = ParseError::Kind::kSyntax,
                                    .message = std::move(message),
                                    .span = SourceSpan{.offset = offset, .length = length}});
}

std::unexpected<ParseError> Unsupported(std::string message, std::size_t offset,
                                        std::size_t length) {
  message += kUnsupportedHint;
  return std::unexpected(ParseError{.kind = ParseError::Kind::kUnsupported,
                                    .message = std::move(message),
                                    .span = SourceSpan{.offset = offset, .length = length}});
}

std::string UnexpectedByte(char c) {
  const auto byte = static_cast<unsigned char>(c);
  if (byte > 0x20 && byte < 0x7f) {
    return std::format("unexpected character '{}'", c);
  }
  if (byte >= 0x80) {
    return std::format("unexpected byte 0x{:02X} (invalid UTF-8)", byte);
  }
  return std::format("unexpected byte 0x{:02X}", byte);
}

// Kind of the operator `op` (a run of operator characters after PostgreSQL's trimming).
TokenKind OperatorKind(std::string_view op) {
  if (op.size() == 1) {
    switch (op[0]) {
      case '*':
        return TokenKind::kStar;
      case '+':
        return TokenKind::kPlus;
      case '-':
        return TokenKind::kMinus;
      case '/':
        return TokenKind::kSlash;
      case '%':
        return TokenKind::kPercent;
      case '=':
        return TokenKind::kEqual;
      case '<':
        return TokenKind::kLess;
      case '>':
        return TokenKind::kGreater;
      default:
        return TokenKind::kOperator;
    }
  }
  if (op == "<>" || op == "!=") {
    return TokenKind::kNotEqual;
  }
  if (op == "<=") {
    return TokenKind::kLessEqual;
  }
  if (op == ">=") {
    return TokenKind::kGreaterEqual;
  }
  if (op == "||") {
    return TokenKind::kConcat;
  }
  return TokenKind::kOperator;
}

Token MakeToken(TokenKind kind, std::size_t start, std::size_t end, std::string text) {
  return Token{.kind = kind,
               .text = std::move(text),
               .span = SourceSpan{.offset = start, .length = end - start}};
}

}  // namespace

bool Token::IsKeyword(std::string_view keyword) const {
  if (kind != TokenKind::kIdentifier || text.size() != keyword.size()) {
    return false;
  }
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (AsciiUpper(text[i]) != AsciiUpper(keyword[i])) {
      return false;
    }
  }
  return true;
}

std::string_view ToString(TokenKind kind) {
  switch (kind) {
    case TokenKind::kIdentifier:
      return "identifier";
    case TokenKind::kQuotedIdentifier:
      return "quoted identifier";
    case TokenKind::kString:
      return "string literal";
    case TokenKind::kInteger:
      return "integer literal";
    case TokenKind::kDecimal:
      return "decimal literal";
    case TokenKind::kStar:
      return "'*'";
    case TokenKind::kComma:
      return "','";
    case TokenKind::kLeftParen:
      return "'('";
    case TokenKind::kRightParen:
      return "')'";
    case TokenKind::kSemicolon:
      return "';'";
    case TokenKind::kDot:
      return "'.'";
    case TokenKind::kPlus:
      return "'+'";
    case TokenKind::kMinus:
      return "'-'";
    case TokenKind::kSlash:
      return "'/'";
    case TokenKind::kPercent:
      return "'%'";
    case TokenKind::kEqual:
      return "'='";
    case TokenKind::kNotEqual:
      return "'<>'";
    case TokenKind::kLess:
      return "'<'";
    case TokenKind::kLessEqual:
      return "'<='";
    case TokenKind::kGreater:
      return "'>'";
    case TokenKind::kGreaterEqual:
      return "'>='";
    case TokenKind::kDoubleColon:
      return "'::'";
    case TokenKind::kConcat:
      return "'||'";
    case TokenKind::kOperator:
      return "operator";
    case TokenKind::kParameter:
      return "parameter";
    case TokenKind::kLeftBracket:
      return "'['";
    case TokenKind::kLeftBrace:
      return "'{'";
    case TokenKind::kEnd:
      return "end of input";
  }
  return "?";
}

std::expected<Token, ParseError> Lexer::Next() {
  const std::string_view text = text_;
  const std::size_t n = text.size();
  std::size_t i = pos_;
  while (i < n) {  // whitespace and comments
    if (IsSpace(text[i])) {
      ++i;
    } else if (text.substr(i, 2) == "--") {  // up to the end of the line (\n, \r or \r\n)
      i += 2;
      while (i < n && !IsNewline(text[i])) {
        ++i;
      }
    } else if (text.substr(i, 2) == "/*") {  // block comments nest
      const std::size_t open = i;
      std::size_t depth = 0;
      do {
        if (text.substr(i, 2) == "/*") {
          ++depth;
          i += 2;
        } else if (text.substr(i, 2) == "*/") {
          --depth;
          i += 2;
        } else {
          ++i;
        }
      } while (depth > 0 && i < n);
      if (depth > 0) {
        pos_ = n;
        return Error("unterminated block comment", open, n - open);
      }
    } else {
      break;
    }
  }
  pos_ = i;
  if (i == n) {
    return MakeToken(TokenKind::kEnd, n, n, "");
  }
  const std::size_t start = i;
  const char c = text[i];
  const char next = i + 1 < n ? text[i + 1] : '\0';
  auto emit = [&](TokenKind kind, std::size_t length) {
    pos_ = start + length;
    return MakeToken(kind, start, pos_, std::string(text.substr(start, length)));
  };

  if (IsIdentStart(c)) {
    while (i < n && IsIdentChar(text[i])) {
      ++i;
    }
    return emit(TokenKind::kIdentifier, i - start);
  }

  if (IsDigit(c) || (c == '.' && IsDigit(next))) {
    const char base = AsciiUpper(next);
    if (c == '0' && (base == 'X' || base == 'O' || base == 'B') && i + 2 < n &&
        IsDigitOfBase(text[i + 2], next)) {
      i += 2;
      while (i < n && IsIdentChar(text[i])) {
        ++i;
      }
      pos_ = n;
      return Unsupported("hexadecimal, octal and binary integer literals are not supported", start,
                         i - start);
    }
    bool decimal = false;
    while (i < n && IsDigit(text[i])) {
      ++i;
    }
    if (i < n && text[i] == '.') {
      decimal = true;
      ++i;
      while (i < n && IsDigit(text[i])) {
        ++i;
      }
    }
    if (i < n && (text[i] == 'e' || text[i] == 'E')) {
      std::size_t j = i + 1;
      if (j < n && (text[j] == '+' || text[j] == '-')) {
        ++j;
      }
      if (j < n && IsDigit(text[j])) {
        decimal = true;
        i = j;
        while (i < n && IsDigit(text[i])) {
          ++i;
        }
      }
    }
    if (i < n && IsIdentStart(text[i])) {
      pos_ = n;
      if (text[i] == '_' && i + 1 < n && IsDigit(text[i + 1])) {
        while (i < n && (IsDigit(text[i]) || text[i] == '_')) {
          ++i;
        }
        return Unsupported("digit separators in numbers (1_000) are not supported", start,
                           i - start);
      }
      return Error("invalid number literal", start, i + 1 - start);
    }
    return emit(decimal ? TokenKind::kDecimal : TokenKind::kInteger, i - start);
  }

  if (c == '\'' || c == '"') {
    const char quote = c;
    const bool is_string = quote == '\'';
    std::string value;
    ++i;
    while (true) {
      const std::size_t close = text.find(quote, i);
      if (close == std::string_view::npos) {
        pos_ = n;
        return Error(is_string ? "unterminated string literal" : "unterminated quoted identifier",
                     start, n - start);
      }
      value.append(text.substr(i, close - i));
      i = close + 1;
      if (i < n && text[i] == quote) {  // a doubled quote is an escaped quote
        value.push_back(quote);
        ++i;
        continue;
      }
      break;
    }
    if (!is_string && value.empty()) {
      pos_ = n;
      return Error("zero-length quoted identifier", start, i - start);
    }
    pos_ = i;
    return MakeToken(is_string ? TokenKind::kString : TokenKind::kQuotedIdentifier, start, i,
                     std::move(value));
  }

  if (IsOperatorChar(c)) {
    // PostgreSQL's operator rule: the longest run of operator characters, cut before an embedded
    // comment start (never at the first character: comments were skipped above). A trailing '+'
    // or '-' is split off unless the run contains a non-SQL operator character, so "a<=-1" is
    // "a", "<=", "-", "1" while "!=-" stays one (unsupported) operator.
    std::size_t end = start + 1;
    while (end < n && IsOperatorChar(text[end]) && text.substr(end, 2) != "--" &&
           text.substr(end, 2) != "/*") {
      ++end;
    }
    std::string_view op = text.substr(start, end - start);
    if (op.size() > 1 && (op.back() == '+' || op.back() == '-') &&
        std::ranges::none_of(op, IsNonSqlOperatorChar)) {
      while (op.size() > 1 && (op.back() == '+' || op.back() == '-')) {
        op.remove_suffix(1);
      }
    }
    return emit(OperatorKind(op), op.size());
  }

  switch (c) {
    case ',':
      return emit(TokenKind::kComma, 1);
    case '(':
      return emit(TokenKind::kLeftParen, 1);
    case ')':
      return emit(TokenKind::kRightParen, 1);
    case ';':
      return emit(TokenKind::kSemicolon, 1);
    case '.':
      return emit(TokenKind::kDot, 1);
    case '[':
      return emit(TokenKind::kLeftBracket, 1);
    case '{':
      return emit(TokenKind::kLeftBrace, 1);
    case ':':
      if (next == ':') {
        return emit(TokenKind::kDoubleColon, 2);
      }
      break;
    case '$': {  // $1, $name; also the start of $$dollar-quoted$$ and $tag$...$tag$ strings
      std::size_t end = start + 1;
      while (end < n && IsIdentChar(text[end])) {
        ++end;
      }
      return emit(TokenKind::kParameter, end - start);
    }
    default:
      break;
  }
  pos_ = n;
  if (const std::size_t length = Utf8SequenceLength(text, start); length > 0) {
    return Unsupported("unquoted non-ASCII names are not supported (double-quote the name)", start,
                       length);
  }
  return Error(UnexpectedByte(c), start, 1);
}

std::expected<std::vector<Token>, ParseError> Tokenize(std::string_view text) {
  Lexer lexer(text);
  std::vector<Token> tokens;
  while (true) {
    auto token = lexer.Next();
    if (!token) {
      return std::unexpected(std::move(token.error()));
    }
    const bool end = token->kind == TokenKind::kEnd;
    tokens.push_back(std::move(*token));
    if (end) {
      return tokens;
    }
  }
}

}  // namespace antb1::sql
