#include "antb1/sql/parser.h"

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "antb1/common/source_span.h"
#include "antb1/sql/ast.h"
#include "antb1/sql/error.h"
#include "antb1/sql/lexer.h"
#include "antb1/sql/token.h"

// Recursive-descent parser. Current coverage: SELECT COUNT(*) FROM <table> (ClickBench Q0).
// The full grammar of docs/sql-subset.md is added incrementally; every construct outside the
// implemented subset is reported as kUnsupported (never as a crash or a silent misparse).

namespace antb1::sql {
namespace {

class Parser {
 public:
  explicit Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

  std::expected<SelectStatement, ParseError> ParseStatement() {
    SelectStatement stmt;
    const std::size_t begin = Peek().span.offset;
    if (!Peek().IsKeyword("SELECT")) {
      return Unsupported(Peek(), "only SELECT statements are supported");
    }
    Advance();
    auto item = ParseSelectItem();
    if (!item) {
      return std::unexpected(item.error());
    }
    stmt.items.push_back(std::move(*item));
    if (Peek().kind == TokenKind::kComma) {
      return Unsupported(Peek(), "multiple select items are not supported yet");
    }
    if (!Peek().IsKeyword("FROM")) {
      return Syntax(Peek(), "expected FROM");
    }
    Advance();
    auto table = ParseTableRef();
    if (!table) {
      return std::unexpected(table.error());
    }
    stmt.from = std::move(*table);
    if (Peek().kind == TokenKind::kSemicolon) {
      Advance();
    }
    if (Peek().kind != TokenKind::kEnd) {
      if (Peek().kind == TokenKind::kIdentifier) {
        return Unsupported(Peek(), "clause '" + Peek().text + "' is not supported yet");
      }
      return Syntax(Peek(), "unexpected " + std::string(ToString(Peek().kind)));
    }
    stmt.span = SourceSpan{.offset = begin, .length = Peek().span.offset - begin};
    return stmt;
  }

 private:
  std::expected<SelectItem, ParseError> ParseSelectItem() {
    const Token& start = Peek();
    if (start.IsKeyword("COUNT") && PeekAt(1).kind == TokenKind::kLeftParen &&
        PeekAt(2).kind == TokenKind::kStar && PeekAt(3).kind == TokenKind::kRightParen) {
      const std::size_t begin = start.span.offset;
      Advance(4);
      const std::size_t end = Previous().span.offset + Previous().span.length;
      const SourceSpan span{.offset = begin, .length = end - begin};
      return SelectItem{.expr = AggregateCall{.kind = AggKind::kCountStar, .arg = {}, .span = span},
                        .alias = {},
                        .span = span};
    }
    return Unsupported(start, "only COUNT(*) is supported yet");
  }

  std::expected<TableRef, ParseError> ParseTableRef() {
    const Token& t = Peek();
    switch (t.kind) {
      case TokenKind::kIdentifier:
      case TokenKind::kQuotedIdentifier: {
        TableRef ref{.kind = TableRef::Kind::kName,
                     .name = t.text,
                     .quoted = t.kind == TokenKind::kQuotedIdentifier,
                     .span = t.span};
        Advance();
        return ref;
      }
      case TokenKind::kString: {
        TableRef ref{
            .kind = TableRef::Kind::kPath, .name = t.text, .quoted = false, .span = t.span};
        Advance();
        return ref;
      }
      default:
        return Syntax(t, "expected a table name or a quoted file path");
    }
  }

  const Token& Peek() const { return PeekAt(0); }
  const Token& PeekAt(std::size_t ahead) const {
    const std::size_t idx = pos_ + ahead;
    return idx < tokens_.size() ? tokens_[idx] : tokens_.back();
  }
  const Token& Previous() const { return tokens_[pos_ - 1]; }
  void Advance(std::size_t count = 1) {
    for (std::size_t i = 0; i < count && tokens_[pos_].kind != TokenKind::kEnd; ++i) {
      ++pos_;
    }
  }

  static std::unexpected<ParseError> Syntax(const Token& at, std::string message) {
    return std::unexpected(ParseError{
        .kind = ParseError::Kind::kSyntax, .message = std::move(message), .span = at.span});
  }
  static std::unexpected<ParseError> Unsupported(const Token& at, std::string message) {
    return std::unexpected(ParseError{
        .kind = ParseError::Kind::kUnsupported, .message = std::move(message), .span = at.span});
  }

  std::vector<Token> tokens_;
  std::size_t pos_ = 0;
};

}  // namespace

std::expected<SelectStatement, ParseError> Parse(std::string_view text) {
  auto tokens = Tokenize(text);
  if (!tokens) {
    return std::unexpected(tokens.error());
  }
  return Parser(std::move(*tokens)).ParseStatement();
}

}  // namespace antb1::sql
