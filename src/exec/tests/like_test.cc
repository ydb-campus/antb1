#include "antb1/exec/like.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <arrow/api.h>
#include <gtest/gtest.h>

#include "exec_test_util.h"

namespace antb1::exec {
namespace {

bool Like(std::string_view text, std::string_view pattern) {
  return LikePattern(pattern).Matches(text);
}

// The cases checked on DuckDB 1.5.5.
TEST(LikePatternTest, DuckDbSemantics) {
  EXPECT_TRUE(Like("\xC3\xA9", "_")) << "_ is one UTF-8 character";
  EXPECT_FALSE(Like("\xC3\xA9", "__"));
  EXPECT_TRUE(Like("a\\b", "a\\b")) << "no escape character";
  EXPECT_FALSE(Like("ab", "a\\b"));
  EXPECT_FALSE(Like("a%", "a\\%"));
  EXPECT_FALSE(Like("a_c", "a\\_c"));
  EXPECT_FALSE(Like("ABC", "abc")) << "case-sensitive";
  EXPECT_FALSE(Like("abc", ""));
  EXPECT_TRUE(Like("", ""));
  EXPECT_TRUE(Like("", "%"));
  EXPECT_FALSE(Like("", "_"));
  EXPECT_TRUE(Like("x", "%%%"));
  EXPECT_TRUE(Like("a\nb", "a_b"));
  EXPECT_TRUE(Like("x\x01y", "x_y"));
}

TEST(LikePatternTest, SegmentsWithoutUnderscore) {
  EXPECT_TRUE(Like("www.shop.example", "%shop%"));
  EXPECT_FALSE(Like("www.shp.example", "%shop%"));
  EXPECT_TRUE(Like("shop", "%shop%"));
  EXPECT_TRUE(Like("abc", "abc"));
  EXPECT_FALSE(Like("abcd", "abc"));
  EXPECT_TRUE(Like("abcd", "ab%"));
  EXPECT_TRUE(Like("abcd", "%cd"));
  EXPECT_FALSE(Like("abcd", "%bc"));
  EXPECT_TRUE(Like("aXbXc", "a%b%c"));
  EXPECT_FALSE(Like("acb", "a%b%c"));
  EXPECT_TRUE(Like("aa", "a%a")) << "prefix and suffix may not overlap";
  EXPECT_FALSE(Like("a", "a%a"));
  EXPECT_TRUE(Like("abab", "ab%ab"));
  EXPECT_FALSE(Like("aba", "ab%ab"));
  EXPECT_TRUE(Like("xabyabz", "%ab%ab%"));
  EXPECT_FALSE(Like("xabz", "%ab%ab%"));
}

// splitmix64 with a fixed seed: deterministic test data.
class Rng {
 public:
  explicit Rng(std::uint64_t seed) : state_(seed) {}
  std::uint64_t Next() {
    std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31U);
  }
  std::size_t Below(std::size_t n) { return Next() % n; }

 private:
  std::uint64_t state_;
};

// The characters of a text: well-formed UTF-8 sequences of these tests' alphabet, or single bytes.
std::vector<std::string> Characters(std::string_view text) {
  std::vector<std::string> out;
  for (std::size_t i = 0; i < text.size();) {
    const auto byte = static_cast<unsigned char>(text[i]);
    std::size_t length = 1;
    if ((byte == 0xC3 || byte == 0xD0) && i + 1 < text.size() &&
        (static_cast<unsigned char>(text[i + 1]) & 0xC0U) == 0x80U) {
      length = 2;
    }
    out.emplace_back(text.substr(i, length));
    i += length;
  }
  return out;
}

// LIKE by its definition, over characters: `%` any run of them, `_` one, anything else itself.
bool Reference(const std::vector<std::string>& text, std::size_t t,
               const std::vector<std::string>& pattern, std::size_t p) {
  if (p == pattern.size()) {
    return t == text.size();
  }
  if (pattern[p] == "%") {
    for (std::size_t k = t; k <= text.size(); ++k) {
      if (Reference(text, k, pattern, p + 1)) {
        return true;
      }
    }
    return false;
  }
  if (t == text.size()) {
    return false;
  }
  return (pattern[p] == "_" || pattern[p] == text[t]) && Reference(text, t + 1, pattern, p + 1);
}

// Random texts (with 2-byte UTF-8 characters and invalid bytes) and patterns agree with the
// definition, with and without `_`.
TEST(LikePatternTest, MatchesTheDefinition) {
  const std::vector<std::string> text_alphabet = {"a",  "b",    "\xC3\xA9", "\xD0\x9F",
                                                  "\\", "\xFF", "\xC3"};
  const std::vector<std::string> pattern_alphabet = {"a",  "b", "\xC3\xA9", "\xD0\x9F",
                                                     "\\", "%", "_"};
  Rng rng(1);
  for (int round = 0; round < 20000; ++round) {
    std::string text;
    for (std::size_t n = rng.Below(7); n > 0; --n) {
      text += text_alphabet[rng.Below(text_alphabet.size())];
    }
    std::string pattern;
    for (std::size_t n = rng.Below(6); n > 0; --n) {
      pattern += pattern_alphabet[rng.Below(pattern_alphabet.size())];
    }
    const bool expected = Reference(Characters(text), 0, Characters(pattern), 0);
    ASSERT_EQ(Like(text, pattern), expected) << "text '" << text << "' pattern '" << pattern << "'";
  }
}

TEST(LikePatternTest, EvaluateKeepsNullsAndNegates) {
  const auto values = std::static_pointer_cast<arrow::BinaryArray>(
      testing::Strings({"books", std::nullopt, "games", ""}));
  const LikePattern pattern("%oo%");
  const auto like = pattern.Evaluate(*values, false, arrow::default_memory_pool());
  ASSERT_TRUE(like.ok()) << like.status().ToString();
  EXPECT_EQ((*like)->ToString(), "[\n  true,\n  null,\n  false,\n  false\n]");
  const auto not_like = pattern.Evaluate(*values, true, arrow::default_memory_pool());
  ASSERT_TRUE(not_like.ok()) << not_like.status().ToString();
  EXPECT_EQ((*not_like)->ToString(), "[\n  false,\n  null,\n  true,\n  true\n]");
}

}  // namespace
}  // namespace antb1::exec
