#include "antb1/common/utf8.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace antb1 {
namespace {

std::string Bytes(std::initializer_list<unsigned> bytes) {
  std::string s;
  for (const unsigned b : bytes) {
    s += static_cast<char>(b);
  }
  return s;
}

TEST(Utf8, WellFormedBoundaries) {
  struct Case {
    std::string bytes;
    std::size_t length;
  };
  const std::vector<Case> cases = {
      {.bytes = Bytes({0xC2, 0x80}), .length = 2},              // U+0080
      {.bytes = Bytes({0xDF, 0xBF}), .length = 2},              // U+07FF
      {.bytes = Bytes({0xE0, 0xA0, 0x80}), .length = 3},        // U+0800
      {.bytes = Bytes({0xED, 0x9F, 0xBF}), .length = 3},        // U+D7FF
      {.bytes = Bytes({0xEE, 0x80, 0x80}), .length = 3},        // U+E000
      {.bytes = Bytes({0xEF, 0xBF, 0xBF}), .length = 3},        // U+FFFF
      {.bytes = Bytes({0xF0, 0x90, 0x80, 0x80}), .length = 4},  // U+10000
      {.bytes = Bytes({0xF4, 0x8F, 0xBF, 0xBF}), .length = 4},  // U+10FFFF
  };
  for (const auto& c : cases) {
    EXPECT_EQ(Utf8SequenceLength(c.bytes, 0), c.length);
    EXPECT_EQ(Utf8SequenceLength("x" + c.bytes + "y", 1), c.length);
  }
}

TEST(Utf8, IllFormedSequences) {
  const std::vector<std::string> cases = {
      Bytes({0x41}),                    // ASCII: callers handle it
      Bytes({0x80}),                    // a lone continuation byte
      Bytes({0xBF, 0x80}),              // a continuation byte as lead
      Bytes({0xC0, 0x80}),              // C0: always overlong
      Bytes({0xC1, 0xBF}),              // C1: always overlong
      Bytes({0xE0, 0x80, 0x80}),        // overlong 3-byte
      Bytes({0xE0, 0x9F, 0xBF}),        // overlong 3-byte
      Bytes({0xED, 0xA0, 0x80}),        // surrogate U+D800
      Bytes({0xED, 0xBF, 0xBF}),        // surrogate U+DFFF
      Bytes({0xF0, 0x80, 0x80, 0x80}),  // overlong 4-byte
      Bytes({0xF0, 0x8F, 0xBF, 0xBF}),  // overlong 4-byte
      Bytes({0xF4, 0x90, 0x80, 0x80}),  // above U+10FFFF
      Bytes({0xF5, 0x80, 0x80, 0x80}),  // F5..FF never lead
      Bytes({0xFF}),                    //
      Bytes({0xC2, 0x41}),              // bad second byte
      Bytes({0xE2, 0x82, 0x41}),        // bad third byte
      Bytes({0xF0, 0x90, 0x80, 0xC0}),  // bad fourth byte
      Bytes({0xC2}),                    // cut short at the end
      Bytes({0xE2, 0x82}),              //
      Bytes({0xF0, 0x90, 0x80}),        //
  };
  for (const auto& c : cases) {
    EXPECT_EQ(Utf8SequenceLength(c, 0), 0U) << testing::PrintToString(c);
  }
}

// Reference decoder: decode by the bit patterns, then reject by code point value.
std::size_t Reference(const std::string& s) {
  const auto b = [&](std::size_t k) {
    return static_cast<uint32_t>(static_cast<unsigned char>(s[k]));
  };
  std::size_t len = 0;
  uint32_t cp = 0;
  uint32_t min = 0;
  if ((b(0) & 0xE0U) == 0xC0U) {
    len = 2;
    cp = b(0) & 0x1FU;
    min = 0x80;
  } else if ((b(0) & 0xF0U) == 0xE0U) {
    len = 3;
    cp = b(0) & 0x0FU;
    min = 0x800;
  } else if ((b(0) & 0xF8U) == 0xF0U) {
    len = 4;
    cp = b(0) & 0x07U;
    min = 0x10000;
  } else {
    return 0;
  }
  if (s.size() < len) {
    return 0;
  }
  for (std::size_t k = 1; k < len; ++k) {
    if ((b(k) & 0xC0U) != 0x80U) {
      return 0;
    }
    cp = (cp << 6U) | (b(k) & 0x3FU);
  }
  const bool ok = cp >= min && cp <= 0x10FFFF && (cp < 0xD800 || cp > 0xDFFF);
  return ok ? len : 0;
}

TEST(Utf8, MatchesAReferenceDecoderExhaustively) {
  // Every 1- and 2-byte string with a non-ASCII lead, every 3-byte string whose lead can start a
  // 3- or 4-byte sequence, and 4-byte strings of those leads with boundary fourth bytes.
  std::string s;
  auto check = [&](std::size_t n) {
    const std::string prefix = s.substr(0, n);
    ASSERT_EQ(Utf8SequenceLength(prefix, 0), Reference(prefix)) << testing::PrintToString(prefix);
  };
  s.resize(4);
  for (unsigned a = 0x80; a <= 0xFF; ++a) {
    s[0] = static_cast<char>(a);
    check(1);
    for (unsigned b = 0; b <= 0xFF; ++b) {
      s[1] = static_cast<char>(b);
      check(2);
      if (a < 0xE0) {
        continue;
      }
      for (unsigned c = 0; c <= 0xFF; ++c) {
        s[2] = static_cast<char>(c);
        check(3);
        if (a < 0xF0) {
          continue;
        }
        for (const unsigned d : {0x00U, 0x41U, 0x7FU, 0x80U, 0xBFU, 0xC0U, 0xFFU}) {
          s[3] = static_cast<char>(d);
          check(4);
        }
      }
    }
  }
}

}  // namespace
}  // namespace antb1
