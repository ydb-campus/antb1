#include "antb1/sql/lexer.h"

#include <cctype>
#include <cstddef>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include "antb1/common/source_span.h"
#include "antb1/sql/error.h"
#include "antb1/sql/token.h"

namespace antb1::sql {
namespace {

bool IsIdentStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_'; }
bool IsIdentChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; }
bool IsDigit(char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }

std::unexpected<ParseError> Error(std::string message, std::size_t offset, std::size_t length) {
  return std::unexpected(ParseError{.kind = ParseError::Kind::kSyntax,
                                    .message = std::move(message),
                                    .span = SourceSpan{.offset = offset, .length = length}});
}

}  // namespace

bool Token::IsKeyword(std::string_view keyword) const {
  if (kind != TokenKind::kIdentifier || text.size() != keyword.size()) {
    return false;
  }
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (std::toupper(static_cast<unsigned char>(text[i])) !=
        std::toupper(static_cast<unsigned char>(keyword[i]))) {
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
    case TokenKind::kEnd:
      return "end of input";
  }
  return "?";
}

std::expected<std::vector<Token>, ParseError> Tokenize(std::string_view text) {
  std::vector<Token> tokens;
  std::size_t i = 0;
  const std::size_t n = text.size();
  auto push = [&](TokenKind kind, std::size_t start, std::size_t end, std::string value) {
    tokens.push_back(Token{.kind = kind,
                           .text = std::move(value),
                           .span = SourceSpan{.offset = start, .length = end - start}});
  };
  while (i < n) {
    const char c = text[i];
    if (std::isspace(static_cast<unsigned char>(c)) != 0) {
      ++i;
      continue;
    }
    if (c == '-' && i + 1 < n && text[i + 1] == '-') {
      while (i < n && text[i] != '\n') {
        ++i;
      }
      continue;
    }
    if (c == '/' && i + 1 < n && text[i + 1] == '*') {
      const std::size_t start = i;
      i += 2;
      while (i + 1 < n && (text[i] != '*' || text[i + 1] != '/')) {
        ++i;
      }
      if (i + 1 >= n) {
        return Error("unterminated block comment", start, n - start);
      }
      i += 2;
      continue;
    }
    const std::size_t start = i;
    if (IsIdentStart(c)) {
      while (i < n && IsIdentChar(text[i])) {
        ++i;
      }
      push(TokenKind::kIdentifier, start, i, std::string(text.substr(start, i - start)));
      continue;
    }
    if (IsDigit(c) || (c == '.' && i + 1 < n && IsDigit(text[i + 1]))) {
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
        return Error("invalid number literal", start, i + 1 - start);
      }
      push(decimal ? TokenKind::kDecimal : TokenKind::kInteger, start, i,
           std::string(text.substr(start, i - start)));
      continue;
    }
    if (c == '\'' || c == '"') {
      const char quote = c;
      std::string value;
      ++i;
      bool closed = false;
      while (i < n) {
        if (text[i] == quote) {
          if (i + 1 < n && text[i + 1] == quote) {
            value.push_back(quote);
            i += 2;
            continue;
          }
          ++i;
          closed = true;
          break;
        }
        value.push_back(text[i]);
        ++i;
      }
      if (!closed) {
        return Error(
            quote == '\'' ? "unterminated string literal" : "unterminated quoted identifier", start,
            n - start);
      }
      push(quote == '\'' ? TokenKind::kString : TokenKind::kQuotedIdentifier, start, i,
           std::move(value));
      continue;
    }
    auto single = [&](TokenKind kind) {
      ++i;
      push(kind, start, i, std::string(1, c));
    };
    auto two = [&](TokenKind kind) {
      i += 2;
      push(kind, start, i, std::string(text.substr(start, 2)));
    };
    const char next = i + 1 < n ? text[i + 1] : '\0';
    switch (c) {
      case '*':
        single(TokenKind::kStar);
        break;
      case ',':
        single(TokenKind::kComma);
        break;
      case '(':
        single(TokenKind::kLeftParen);
        break;
      case ')':
        single(TokenKind::kRightParen);
        break;
      case ';':
        single(TokenKind::kSemicolon);
        break;
      case '.':
        single(TokenKind::kDot);
        break;
      case '+':
        single(TokenKind::kPlus);
        break;
      case '-':
        single(TokenKind::kMinus);
        break;
      case '/':
        single(TokenKind::kSlash);
        break;
      case '%':
        single(TokenKind::kPercent);
        break;
      case '=':
        single(TokenKind::kEqual);
        break;
      case '!':
        if (next == '=') {
          two(TokenKind::kNotEqual);
          break;
        }
        return Error("unexpected character '!'", start, 1);
      case '<':
        if (next == '>') {
          two(TokenKind::kNotEqual);
        } else if (next == '=') {
          two(TokenKind::kLessEqual);
        } else {
          single(TokenKind::kLess);
        }
        break;
      case '>':
        if (next == '=') {
          two(TokenKind::kGreaterEqual);
        } else {
          single(TokenKind::kGreater);
        }
        break;
      default:
        return Error("unexpected character", start, 1);
    }
  }
  push(TokenKind::kEnd, n, n, "");
  return tokens;
}

}  // namespace antb1::sql
