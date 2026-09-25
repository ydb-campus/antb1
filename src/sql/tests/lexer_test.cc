#include "antb1/sql/lexer.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "antb1/sql/error.h"
#include "antb1/sql/token.h"

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

// The single token of `text` (followed by kEnd).
Token Single(std::string_view text) {
  auto tokens = Tokenize(text);
  EXPECT_TRUE(tokens.has_value()) << text;
  if (!tokens || tokens->size() != 2) {
    ADD_FAILURE() << "expected exactly one token in: " << text;
    return {};
  }
  return (*tokens)[0];
}

ParseError LexError(std::string_view text) {
  auto tokens = Tokenize(text);
  EXPECT_FALSE(tokens.has_value()) << text;
  return tokens ? ParseError{} : tokens.error();
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
      Kinds(R"(a <> 1 != 2.5 <= 'x''y' >= "Q""" < > = -3e2 :: ||)"),
      (std::vector<TokenKind>{TokenKind::kIdentifier, TokenKind::kNotEqual, TokenKind::kInteger,
                              TokenKind::kNotEqual, TokenKind::kDecimal, TokenKind::kLessEqual,
                              TokenKind::kString, TokenKind::kGreaterEqual,
                              TokenKind::kQuotedIdentifier, TokenKind::kLess, TokenKind::kGreater,
                              TokenKind::kEqual, TokenKind::kMinus, TokenKind::kDecimal,
                              TokenKind::kDoubleColon, TokenKind::kConcat, TokenKind::kEnd}));
}

TEST(LexerTest, EveryPunctuator) {
  struct Case {
    std::string_view text;
    TokenKind kind;
  };
  for (const Case& c : {Case{.text = "*", .kind = TokenKind::kStar},
                        Case{.text = ",", .kind = TokenKind::kComma},
                        Case{.text = "(", .kind = TokenKind::kLeftParen},
                        Case{.text = ")", .kind = TokenKind::kRightParen},
                        Case{.text = ";", .kind = TokenKind::kSemicolon},
                        Case{.text = ".", .kind = TokenKind::kDot},
                        Case{.text = "+", .kind = TokenKind::kPlus},
                        Case{.text = "-", .kind = TokenKind::kMinus},
                        Case{.text = "/", .kind = TokenKind::kSlash},
                        Case{.text = "%", .kind = TokenKind::kPercent},
                        Case{.text = "=", .kind = TokenKind::kEqual},
                        Case{.text = "<>", .kind = TokenKind::kNotEqual},
                        Case{.text = "!=", .kind = TokenKind::kNotEqual},
                        Case{.text = "<", .kind = TokenKind::kLess},
                        Case{.text = "<=", .kind = TokenKind::kLessEqual},
                        Case{.text = ">", .kind = TokenKind::kGreater},
                        Case{.text = ">=", .kind = TokenKind::kGreaterEqual},
                        Case{.text = "::", .kind = TokenKind::kDoubleColon},
                        Case{.text = "||", .kind = TokenKind::kConcat},
                        Case{.text = "[", .kind = TokenKind::kLeftBracket},
                        Case{.text = "{", .kind = TokenKind::kLeftBrace},
                        Case{.text = "$1", .kind = TokenKind::kParameter},
                        Case{.text = "$name", .kind = TokenKind::kParameter},
                        Case{.text = "$", .kind = TokenKind::kParameter}}) {
    const Token token = Single(c.text);
    EXPECT_EQ(token.kind, c.kind) << c.text;
    EXPECT_EQ(token.text, c.text);
    EXPECT_EQ(token.span, (SourceSpan{.offset = 0, .length = c.text.size()}));
  }
}

TEST(LexerTest, OperatorsSplitWithoutSpaces) {
  EXPECT_EQ(Kinds("a<=-1"),
            (std::vector<TokenKind>{TokenKind::kIdentifier, TokenKind::kLessEqual,
                                    TokenKind::kMinus, TokenKind::kInteger, TokenKind::kEnd}));
  EXPECT_EQ(Kinds("a<-1"),
            (std::vector<TokenKind>{TokenKind::kIdentifier, TokenKind::kLess, TokenKind::kMinus,
                                    TokenKind::kInteger, TokenKind::kEnd}));
  EXPECT_EQ(Kinds("<>="), (std::vector<TokenKind>{TokenKind::kOperator, TokenKind::kEnd}));
  EXPECT_EQ(Kinds("a::b||c"),
            (std::vector<TokenKind>{TokenKind::kIdentifier, TokenKind::kDoubleColon,
                                    TokenKind::kIdentifier, TokenKind::kConcat,
                                    TokenKind::kIdentifier, TokenKind::kEnd}));
  const ParseError error = LexError(":::");
  EXPECT_EQ(error.message, "unexpected character ':'");
  EXPECT_EQ(error.span, (SourceSpan{.offset = 2, .length = 1}));
}

// PostgreSQL's operator rule (DuckDB uses the same lexer): the longest run of operator characters
// is one token; a trailing '+'/'-' is split off unless the run holds one of ~ ! @ # % ^ & | ` ?.
TEST(LexerTest, OperatorsFollowPostgresRules) {
  struct Case {
    std::string_view text;
    std::vector<std::string_view> tokens;  // texts of the tokens before kEnd
  };
  for (const Case& c : {
           Case{.text = "a<=-1", .tokens = {"a", "<=", "-", "1"}},
           Case{.text = "a<>-.5", .tokens = {"a", "<>", "-", ".5"}},
           Case{.text = "a=-1", .tokens = {"a", "=", "-", "1"}},
           Case{.text = "a>=+-1", .tokens = {"a", ">=", "+", "-", "1"}},
           Case{.text = "a*-1", .tokens = {"a", "*", "-", "1"}},
           Case{.text = "a!=-1", .tokens = {"a", "!=-", "1"}},  // one operator, as in DuckDB
           Case{.text = "a%-1", .tokens = {"a", "%-", "1"}},
           Case{.text = "a=--c\n1", .tokens = {"a", "=", "1"}},  // cut before a comment
           Case{.text = "a=/*c*/1", .tokens = {"a", "=", "1"}},
           Case{.text = "a<-/*c*/1", .tokens = {"a", "<", "-", "1"}},
           Case{.text = "*/**/", .tokens = {"*"}},
           Case{.text = "a==1", .tokens = {"a", "==", "1"}},
           Case{.text = "a<<2>>3", .tokens = {"a", "<<", "2", ">>", "3"}},
           Case{.text = "a->'k'", .tokens = {"a", "->", "k"}},
           Case{.text = "~a !~ b ^ c & d | e # f @ g ` ?",
                .tokens = {"~", "a", "!~", "b", "^", "c", "&", "d", "|", "e", "#", "f", "@", "g",
                           "`", "?"}},
       }) {
    auto tokens = Tokenize(c.text);
    ASSERT_TRUE(tokens.has_value()) << c.text << ": " << tokens.error().message;
    ASSERT_EQ(tokens->size(), c.tokens.size() + 1) << c.text;
    for (std::size_t i = 0; i < c.tokens.size(); ++i) {
      EXPECT_EQ((*tokens)[i].text, c.tokens[i]) << c.text << " token " << i;
    }
  }
  EXPECT_EQ(Single("!=-").kind, TokenKind::kOperator);
  EXPECT_EQ(Single("~").kind, TokenKind::kOperator);
  EXPECT_EQ(Single("|").kind, TokenKind::kOperator);
  EXPECT_EQ(Single("<=").kind, TokenKind::kLessEqual);
  EXPECT_EQ(Single("!=").kind, TokenKind::kNotEqual);
}

TEST(LexerTest, NumberForms) {
  struct Case {
    std::string_view text;
    TokenKind kind;
  };
  for (const Case& c : {Case{.text = "0", .kind = TokenKind::kInteger},
                        Case{.text = "007", .kind = TokenKind::kInteger},
                        Case{.text = "9223372036854775808", .kind = TokenKind::kInteger},
                        Case{.text = "1.5", .kind = TokenKind::kDecimal},
                        Case{.text = ".5", .kind = TokenKind::kDecimal},
                        Case{.text = "5.", .kind = TokenKind::kDecimal},
                        Case{.text = "1e3", .kind = TokenKind::kDecimal},
                        Case{.text = "1E+3", .kind = TokenKind::kDecimal},
                        Case{.text = "2.5e-3", .kind = TokenKind::kDecimal},
                        Case{.text = "1.e5", .kind = TokenKind::kDecimal},
                        Case{.text = ".5E10", .kind = TokenKind::kDecimal}}) {
    const Token token = Single(c.text);
    EXPECT_EQ(token.kind, c.kind) << c.text;
    EXPECT_EQ(token.text, c.text);
  }
  // A dot not followed by a digit ends the number; a second dot starts a new one.
  EXPECT_EQ(Kinds("1.2.3"),
            (std::vector<TokenKind>{TokenKind::kDecimal, TokenKind::kDecimal, TokenKind::kEnd}));
  EXPECT_EQ(Kinds("1..2"),
            (std::vector<TokenKind>{TokenKind::kDecimal, TokenKind::kDecimal, TokenKind::kEnd}));
}

TEST(LexerTest, InvalidNumbers) {
  for (const std::string_view text :
       {"1e", "1e+", "12abc", "0x", "0xZ", "0b2", "1.5e3x", "1.a", "1e+x", "5_", "5__0", "1AND"}) {
    const ParseError error = LexError(text);
    EXPECT_EQ(error.kind, ParseError::Kind::kSyntax) << text;
    EXPECT_EQ(error.message, "invalid number literal") << text;
    EXPECT_EQ(error.span.offset, 0U) << text;
  }
  const ParseError error = LexError("a = 12abc");
  EXPECT_EQ(error.span, (SourceSpan{.offset = 4, .length = 3}));
}

// Integer forms DuckDB accepts but the subset does not: kUnsupported covering the literal.
TEST(LexerTest, UnsupportedNumberForms) {
  struct Case {
    std::string_view text;
    std::size_t length;
    std::string_view message;
  };
  for (const Case& c : {
           Case{.text = "0x1F ", .length = 4, .message = "hexadecimal, octal and binary"},
           Case{.text = "0XffZ,", .length = 5, .message = "hexadecimal, octal and binary"},
           Case{.text = "0o17", .length = 4, .message = "hexadecimal, octal and binary"},
           Case{.text = "0b101)", .length = 5, .message = "hexadecimal, octal and binary"},
           Case{.text = "1_000 ", .length = 5, .message = "digit separators"},
           Case{.text = "1.5_0", .length = 5, .message = "digit separators"},
       }) {
    const ParseError error = LexError(c.text);
    EXPECT_EQ(error.kind, ParseError::Kind::kUnsupported) << c.text;
    EXPECT_EQ(error.span, (SourceSpan{.offset = 0, .length = c.length})) << c.text;
    EXPECT_TRUE(error.message.starts_with(c.message)) << error.message;
    EXPECT_TRUE(error.message.ends_with(kUnsupportedHint)) << error.message;
  }
}

TEST(LexerTest, UnescapesQuotes) {
  auto tokens = Tokenize(R"('it''s' "a""b" '' '''' "x")");
  ASSERT_TRUE(tokens.has_value());
  ASSERT_EQ(tokens->size(), 6U);
  EXPECT_EQ((*tokens)[0].text, "it's");
  EXPECT_EQ((*tokens)[0].span, (SourceSpan{.offset = 0, .length = 7}));
  EXPECT_EQ((*tokens)[1].kind, TokenKind::kQuotedIdentifier);
  EXPECT_EQ((*tokens)[1].text, R"(a"b)");
  EXPECT_EQ((*tokens)[2].kind, TokenKind::kString);
  EXPECT_EQ((*tokens)[2].text, "");
  EXPECT_EQ((*tokens)[3].text, "'");
  EXPECT_EQ((*tokens)[4].text, "x");
}

TEST(LexerTest, QuotedTextKeepsEveryByte) {
  const std::string text =
      std::string("'a\0b\n-- c /* d */ \xff'", 20) + " \"\xd0\xb8\xd0\xbc\xd1\x8f\"";
  auto tokens = Tokenize(text);
  ASSERT_TRUE(tokens.has_value()) << tokens.error().message;
  ASSERT_EQ(tokens->size(), 3U);
  EXPECT_EQ((*tokens)[0].text, std::string("a\0b\n-- c /* d */ \xff", 18));
  EXPECT_EQ((*tokens)[1].kind, TokenKind::kQuotedIdentifier);
  EXPECT_EQ((*tokens)[1].text, "\xd0\xb8\xd0\xbc\xd1\x8f");
}

TEST(LexerTest, ZeroLengthQuotedIdentifierIsAnError) {
  const ParseError error = LexError(R"(SELECT "" FROM t)");
  EXPECT_EQ(error.message, "zero-length quoted identifier");
  EXPECT_EQ(error.span, (SourceSpan{.offset = 7, .length = 2}));
}

TEST(LexerTest, SkipsComments) {
  EXPECT_EQ(
      Kinds("-- c\nx /* y */ z"),
      (std::vector<TokenKind>{TokenKind::kIdentifier, TokenKind::kIdentifier, TokenKind::kEnd}));
  EXPECT_EQ(Kinds("x -- trailing comment without newline"),
            (std::vector<TokenKind>{TokenKind::kIdentifier, TokenKind::kEnd}));
  EXPECT_EQ(Kinds("a/**/b/*\n multi\n line */c"),
            (std::vector<TokenKind>{TokenKind::kIdentifier, TokenKind::kIdentifier,
                                    TokenKind::kIdentifier, TokenKind::kEnd}));
  EXPECT_EQ(Kinds("/* -- not a line comment */ a /* '' */"),
            (std::vector<TokenKind>{TokenKind::kIdentifier, TokenKind::kEnd}));
  EXPECT_EQ(Kinds("a--b\nc"), (std::vector<TokenKind>{TokenKind::kIdentifier,
                                                      TokenKind::kIdentifier, TokenKind::kEnd}));
  EXPECT_EQ(Kinds("/* a /* nested */ still a comment */ b"),
            (std::vector<TokenKind>{TokenKind::kIdentifier, TokenKind::kEnd}));
  EXPECT_EQ(
      Kinds("/*/**/*/ b /***/ c /*/ */"),
      (std::vector<TokenKind>{TokenKind::kIdentifier, TokenKind::kIdentifier, TokenKind::kEnd}));
  EXPECT_EQ(Kinds("'-- in a string' '/* too */'"),
            (std::vector<TokenKind>{TokenKind::kString, TokenKind::kString, TokenKind::kEnd}));
}

// A line comment ends at \n or \r (PostgreSQL and DuckDB): text after a bare \r is not comment.
TEST(LexerTest, LineCommentsEndAtCarriageReturn) {
  for (const std::string_view text : {"a -- c\rb", "a -- c\r\nb", "a --\rb", "a -- c\nb"}) {
    auto tokens = Tokenize(text);
    ASSERT_TRUE(tokens.has_value()) << text;
    ASSERT_EQ(tokens->size(), 3U) << text;
    EXPECT_EQ((*tokens)[1].text, "b") << text;
    EXPECT_EQ((*tokens)[1].span.offset, text.size() - 1) << text;
  }
}

TEST(LexerTest, WhitespaceVariants) {
  auto tokens = Tokenize(" \t\r\n\v\fa\t\r\n\v\f b \f");
  ASSERT_TRUE(tokens.has_value());
  ASSERT_EQ(tokens->size(), 3U);
  EXPECT_EQ((*tokens)[0].span, (SourceSpan{.offset = 6, .length = 1}));
  EXPECT_EQ((*tokens)[1].span, (SourceSpan{.offset = 13, .length = 1}));
  EXPECT_EQ((*tokens)[2].kind, TokenKind::kEnd);
  EXPECT_EQ((*tokens)[2].span, (SourceSpan{.offset = 16, .length = 0}));
  EXPECT_EQ(Kinds(""), (std::vector<TokenKind>{TokenKind::kEnd}));
  EXPECT_EQ(Kinds("  -- only a comment"), (std::vector<TokenKind>{TokenKind::kEnd}));
}

TEST(LexerTest, IdentifiersAreAscii) {
  const Token token = Single("_Event_Date2");
  EXPECT_EQ(token.kind, TokenKind::kIdentifier);
  EXPECT_EQ(token.text, "_Event_Date2");
  // Well-formed UTF-8 outside quotes is a name DuckDB accepts but the subset does not.
  ParseError error = LexError("abc \xd0\xb8");
  EXPECT_EQ(error.kind, ParseError::Kind::kUnsupported);
  EXPECT_EQ(error.span, (SourceSpan{.offset = 4, .length = 2}));
  EXPECT_EQ(error.message,
            "unquoted non-ASCII names are not supported (double-quote the name); see "
            "docs/sql-subset.md");
  EXPECT_EQ(LexError("\xe2\x82\xac").span.length, 3U);
  EXPECT_EQ(LexError("\xf0\x9f\x98\x80").span.length, 4U);
  EXPECT_EQ(LexError("\xc2\xa0").kind, ParseError::Kind::kUnsupported);  // no-break space
  // Anything else is not UTF-8 at all: a syntax error at the first bad byte.
  for (const std::string_view bad :
       {"\xff", "\x80", "\xc0\x80", "\xc3\x28", "\xe0\x80\x80", "\xed\xa0\x80", "\xf4\x90\x80\x80",
        "\xf5\x80\x80\x80", "\xe2\x82"}) {
    error = LexError(bad);
    EXPECT_EQ(error.kind, ParseError::Kind::kSyntax) << testing::PrintToString(bad);
    EXPECT_EQ(error.span, (SourceSpan{.offset = 0, .length = 1}));
    EXPECT_TRUE(error.message.ends_with("(invalid UTF-8)")) << error.message;
  }
}

TEST(LexerTest, ReportsErrorsWithSpans) {
  ParseError error = LexError("SELECT 'abc");
  EXPECT_EQ(error.message, "unterminated string literal");
  EXPECT_EQ(error.span, (SourceSpan{.offset = 7, .length = 4}));
  error = LexError(R"(SELECT "abc)");
  EXPECT_EQ(error.message, "unterminated quoted identifier");
  EXPECT_EQ(error.span, (SourceSpan{.offset = 7, .length = 4}));
  error = LexError("a /* open");
  EXPECT_EQ(error.message, "unterminated block comment");
  EXPECT_EQ(error.span, (SourceSpan{.offset = 2, .length = 7}));
  error = LexError("/*/");
  EXPECT_EQ(error.message, "unterminated block comment");
  error = LexError("a /* /* nested */ open");
  EXPECT_EQ(error.message, "unterminated block comment");
  EXPECT_EQ(error.span, (SourceSpan{.offset = 2, .length = 20}));
  error = LexError("a \\ b");
  EXPECT_EQ(error.message, "unexpected character '\\'");
  EXPECT_EQ(error.span, (SourceSpan{.offset = 2, .length = 1}));
  EXPECT_EQ(LexError("a : b").message, "unexpected character ':'");
  EXPECT_EQ(LexError("[a]").message, "unexpected character ']'");
  EXPECT_EQ(LexError("{a}").message, "unexpected character '}'");
  EXPECT_EQ(LexError(std::string_view("a\0", 2)).message, "unexpected byte 0x00");
  EXPECT_EQ(LexError("\x7f").message, "unexpected byte 0x7F");
  EXPECT_EQ(LexError("\xff").message, "unexpected byte 0xFF (invalid UTF-8)");
}

TEST(LexerTest, EveryByteIsHandled) {
  for (int byte = 0; byte < 256; ++byte) {
    const std::string text(1, static_cast<char>(byte));
    auto tokens = Tokenize(text);
    if (tokens) {
      EXPECT_EQ(tokens->back().kind, TokenKind::kEnd);
    } else {
      EXPECT_LE(tokens.error().span.offset + tokens.error().span.length, text.size()) << byte;
      EXPECT_FALSE(tokens.error().message.empty());
    }
  }
}

TEST(LexerTest, KeywordsAreCaseInsensitive) {
  auto tokens = Tokenize(R"(sElEcT "SELECT")");
  ASSERT_TRUE(tokens.has_value());
  EXPECT_TRUE((*tokens)[0].IsKeyword("SELECT"));
  EXPECT_TRUE((*tokens)[0].IsKeyword("select"));
  EXPECT_FALSE((*tokens)[0].IsKeyword("SELEC"));
  EXPECT_FALSE((*tokens)[0].IsKeyword("SELECTS"));
  EXPECT_FALSE((*tokens)[1].IsKeyword("SELECT"));  // quoted identifiers are never keywords
}

TEST(LexerTest, StreamingLexerStaysAtEnd) {
  Lexer lexer("a");
  auto first = lexer.Next();
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first->kind, TokenKind::kIdentifier);
  for (int i = 0; i < 3; ++i) {
    auto end = lexer.Next();
    ASSERT_TRUE(end.has_value());
    EXPECT_EQ(end->kind, TokenKind::kEnd);
    EXPECT_EQ(end->span, (SourceSpan{.offset = 1, .length = 0}));
  }
  Lexer broken("'x");
  EXPECT_FALSE(broken.Next().has_value());
  auto after_error = broken.Next();
  ASSERT_TRUE(after_error.has_value());
  EXPECT_EQ(after_error->kind, TokenKind::kEnd);
}

TEST(LexerTest, ToStringNamesEveryKind) {
  for (int k = 0; k <= static_cast<int>(TokenKind::kEnd); ++k) {
    EXPECT_NE(ToString(static_cast<TokenKind>(k)), "?") << k;
  }
  EXPECT_EQ(ToString(TokenKind::kNotEqual), "'<>'");
  EXPECT_EQ(ToString(TokenKind::kEnd), "end of input");
}

TEST(LexerTest, LargeInputs) {
  constexpr std::size_t kSize = std::size_t{1} << 20U;
  auto parens = Tokenize(std::string(kSize, '('));
  ASSERT_TRUE(parens.has_value());
  EXPECT_EQ(parens->size(), kSize + 1);
  auto spaces = Tokenize(std::string(kSize, ' '));
  ASSERT_TRUE(spaces.has_value());
  EXPECT_EQ(spaces->size(), 1U);
  const Token ident = Single(std::string(kSize, 'x'));
  EXPECT_EQ(ident.text.size(), kSize);
  const Token quotes = Single(std::string(kSize, '\''));  // '' pairs are escaped quotes
  EXPECT_EQ(quotes.text, std::string((kSize - 2) / 2, '\''));
  EXPECT_EQ(LexError("/*" + std::string(kSize, '*')).message, "unterminated block comment");
}

}  // namespace
}  // namespace antb1::sql
