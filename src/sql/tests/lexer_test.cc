#include "antb1/sql/lexer.h"

#include <vector>

#include <gtest/gtest.h>

namespace antb1::sql {
namespace {

std::vector<TokenKind> Kinds(std::string_view text) {
  auto tokens = Tokenize(text);
  EXPECT_TRUE(tokens.has_value()) << (tokens ? "" : tokens.error().message);
  std::vector<TokenKind> kinds;
  if (tokens) {
    for (const auto& t : *tokens) {
      kinds.push_back(t.kind);
    }
  }
  return kinds;
}

TEST(LexerTest, TokenizesCountStar) {
  EXPECT_EQ(
      Kinds("SELECT COUNT(*) FROM events;"),
      (std::vector<TokenKind>{TokenKind::kIdentifier, TokenKind::kIdentifier, TokenKind::kLeftParen,
                              TokenKind::kStar, TokenKind::kRightParen, TokenKind::kIdentifier,
                              TokenKind::kIdentifier, TokenKind::kSemicolon, TokenKind::kEnd}));
}

TEST(LexerTest, OperatorsAndLiterals) {
  EXPECT_EQ(
      Kinds("a <> 1 != 2.5 <= 'x''y' >= \"Q\"\"\" < > = -3e2"),
      (std::vector<TokenKind>{
          TokenKind::kIdentifier, TokenKind::kNotEqual, TokenKind::kInteger, TokenKind::kNotEqual,
          TokenKind::kDecimal, TokenKind::kLessEqual, TokenKind::kString, TokenKind::kGreaterEqual,
          TokenKind::kQuotedIdentifier, TokenKind::kLess, TokenKind::kGreater, TokenKind::kEqual,
          TokenKind::kMinus, TokenKind::kDecimal, TokenKind::kEnd}));
}

TEST(LexerTest, UnescapesQuotes) {
  auto tokens = Tokenize(R"('it''s' "a""b")");
  ASSERT_TRUE(tokens.has_value());
  EXPECT_EQ((*tokens)[0].text, "it's");
  EXPECT_EQ((*tokens)[1].text, "a\"b");
}

TEST(LexerTest, SkipsComments) {
  EXPECT_EQ(
      Kinds("-- c\nx /* y */ z"),
      (std::vector<TokenKind>{TokenKind::kIdentifier, TokenKind::kIdentifier, TokenKind::kEnd}));
}

TEST(LexerTest, ReportsErrorsWithSpans) {
  auto r = Tokenize("SELECT 'abc");
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().span.offset, 7U);
  EXPECT_FALSE(Tokenize("a ! b").has_value());
  EXPECT_FALSE(Tokenize("/* open").has_value());
  EXPECT_FALSE(Tokenize("12abc").has_value());
}

TEST(LexerTest, KeywordsAreCaseInsensitive) {
  auto tokens = Tokenize("sElEcT");
  ASSERT_TRUE(tokens.has_value());
  EXPECT_TRUE((*tokens)[0].IsKeyword("SELECT"));
  EXPECT_FALSE((*tokens)[0].IsKeyword("SELEC"));
}

}  // namespace
}  // namespace antb1::sql
