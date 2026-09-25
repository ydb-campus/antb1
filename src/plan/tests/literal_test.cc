#include "antb1/plan/literal.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "antb1/common/int128.h"

namespace antb1::plan {
namespace {

// "huge", or floor(|value|) followed by "+" when the value is not an integer; "invalid" when the
// text is not a number.
std::string Summary(const std::optional<ExactNumber>& n) {
  if (!n.has_value()) {
    return "invalid";
  }
  if (n->huge) {
    return "huge";
  }
  return Int128ToString(n->magnitude) + (n->fraction ? "+" : "");
}

TEST(LiteralTest, ParseExactNumber) {
  const std::vector<std::pair<std::string_view, std::string_view>> cases = {
      {"0", "0"},
      {"000", "0"},
      {"0.000", "0"},
      {"42", "42"},
      {"0042", "42"},
      {"1.5", "1+"},
      {".5", "0+"},
      {"5.", "5"},
      {"5.0", "5"},
      {"2.50", "2+"},
      {"1e3", "1000"},
      {"1E+3", "1000"},
      {"1.e2", "100"},
      {"2.5E-3", "0+"},
      {"123e-1", "12+"},
      {"120e-1", "12"},
      {"0.0001e4", "1"},
      {"12345678901234567890", "12345678901234567890"},
      {"99999999999999999999999999999999999999", "99999999999999999999999999999999999999"},
      {"99999999999999999999999999999999999999.99", "99999999999999999999999999999999999999+"},
      {"100000000000000000000000000000000000000", "huge"},
      {"1e38", "huge"},
      {"0.0001e42", "huge"},
      {"9.99e37", "99900000000000000000000000000000000000"},
      {"1e1000000000000000000000", "huge"},
      {"1e-1000000000000000000000", "0+"},
      {"0e1000000000000000000000", "0"},
      {"00000000000000000000000000000000000000000001", "1"},
      {"0.00000000000000000000000000000000000000000001", "0+"},
  };
  for (const auto& [text, expected] : cases) {
    const auto n = ParseExactNumber(text, /*negative=*/false);
    EXPECT_EQ(Summary(n), expected) << text;
    EXPECT_FALSE(n.value_or(ExactNumber{.negative = true}).negative) << text;
  }
}

TEST(LiteralTest, SignOfZeroIsDropped) {
  EXPECT_TRUE(ParseExactNumber("1.5", true).value_or(ExactNumber{}).negative);
  EXPECT_FALSE(ParseExactNumber("0.0", true).value_or(ExactNumber{.negative = true}).negative);
  EXPECT_FALSE(ParseExactNumber("0e5", true).value_or(ExactNumber{.negative = true}).negative);
}

TEST(LiteralTest, RejectsNonNumbers) {
  for (const std::string_view text :
       {"", ".", "e5", ".e5", "1e", "1e+", "1e-", "1x", "-1", "+1", "1.2.3", "1e5.5", " 1", "1 ",
        "0x10", "1_000", "inf", "nan"}) {
    EXPECT_FALSE(ParseExactNumber(text, false).has_value()) << text;
    EXPECT_FALSE(ParseDoubleLiteral(text, false).has_value()) << text;
  }
}

TEST(LiteralTest, ParseDoubleLiteral) {
  EXPECT_EQ(ParseDoubleLiteral("1.5", false), 1.5);
  EXPECT_EQ(ParseDoubleLiteral("1.5", true), -1.5);
  EXPECT_EQ(ParseDoubleLiteral(".5", false), 0.5);
  EXPECT_EQ(ParseDoubleLiteral("5.", false), 5.0);
  EXPECT_EQ(ParseDoubleLiteral("1e3", false), 1000.0);
  EXPECT_EQ(ParseDoubleLiteral("2.5E-3", false), 0.0025);
  EXPECT_EQ(ParseDoubleLiteral("0.1", false), 0.1);
  EXPECT_EQ(ParseDoubleLiteral("123456789.125", false), 123456789.125);
  // Correctly rounded (ties to even).
  EXPECT_EQ(ParseDoubleLiteral("9007199254740993", false), 9007199254740992.0);
  EXPECT_EQ(ParseDoubleLiteral("9007199254740995", false), 9007199254740996.0);
  EXPECT_EQ(ParseDoubleLiteral("18446744073709551616", false), 18446744073709551616.0);
  // Beyond the double range, as in DuckDB.
  EXPECT_EQ(ParseDoubleLiteral("1e400", false), std::numeric_limits<double>::infinity());
  EXPECT_EQ(ParseDoubleLiteral("1e400", true), -std::numeric_limits<double>::infinity());
  EXPECT_EQ(ParseDoubleLiteral("1" + std::string(400, '0'), false),
            std::numeric_limits<double>::infinity());
  EXPECT_EQ(ParseDoubleLiteral("1e-400", false), 0.0);
  const double negative_zero = ParseDoubleLiteral("1e-400", true).value_or(1.0);
  EXPECT_EQ(negative_zero, 0.0);
  EXPECT_TRUE(std::signbit(negative_zero));
}

TEST(LiteralTest, ParseDate) {
  EXPECT_EQ(ParseDate("1970-01-01"), 0);
  EXPECT_EQ(ParseDate("1970-01-02"), 1);
  EXPECT_EQ(ParseDate("1969-12-31"), -1);
  EXPECT_EQ(ParseDate("2013-07-01"), 15887);
  EXPECT_EQ(ParseDate("2022-01-08"), 19000);
  EXPECT_EQ(ParseDate("2000-02-29"), 11016);
  EXPECT_EQ(ParseDate("2100-12-31"), 47846);
  EXPECT_EQ(ParseDate("0000-01-01"), -719528);
  EXPECT_EQ(ParseDate("0001-01-01"), -719162);
  EXPECT_EQ(ParseDate("9999-12-31"), 2932896);
  for (const std::string_view text :
       {"",           "2013-07-1",   "2013-7-01",   "13-07-01",      "2013-07-015",
        "2013/07/01", "2013-07-01 ", " 2013-07-01", "2013-07-01T00", "2013-13-01",
        "2013-00-01", "2013-01-00",  "2013-01-32",  "2013-02-29",    "1900-02-29",
        "2013-04-31", "+013-07-01",  "-013-07-01",  "2013-0a-01",    "abcd-ef-gh",
        "2013--7-01"}) {
    EXPECT_FALSE(ParseDate(text).has_value()) << text;
  }
}

TEST(LiteralTest, FormatDateRoundTrips) {
  for (const std::string_view text :
       {"1970-01-01", "1969-12-31", "2022-01-08", "2000-02-29", "0000-01-01", "9999-12-31"}) {
    const auto days = ParseDate(text);
    EXPECT_EQ(days.has_value() ? FormatDate(*days) : "invalid", text);
  }
  for (int32_t days = -719528; days <= 2932896; days += 997) {  // 0000-01-01 .. 9999-12-31
    EXPECT_EQ(ParseDate(FormatDate(days)), days) << days;
  }
}

TEST(LiteralTest, FoldIntegerComparisonTable) {
  const IntegerRange i16 = RangeOf(LogicalType::kSmallInt);
  const auto fold = [&](CompareOp op, std::string_view text, bool negative) {
    const auto n = ParseExactNumber(text, negative);
    EXPECT_TRUE(n.has_value()) << text;
    return FoldIntegerComparison(op, n.value_or(ExactNumber{}), i16);
  };
  using enum Predicate::Kind;
  // In range: unchanged.
  auto f = fold(CompareOp::kLt, "5", false);
  EXPECT_EQ(f.kind, kCompare);
  EXPECT_EQ(f.op, CompareOp::kLt);
  EXPECT_EQ(f.value, 5);
  // Non-integers: the nearest integer on the kept side.
  f = fold(CompareOp::kGt, "4.5", false);
  EXPECT_EQ(f.kind, kCompare);
  EXPECT_EQ(f.op, CompareOp::kGe);
  EXPECT_EQ(f.value, 5);
  f = fold(CompareOp::kLe, "4.5", true);
  EXPECT_EQ(f.kind, kCompare);
  EXPECT_EQ(f.op, CompareOp::kLe);
  EXPECT_EQ(f.value, -5);
  EXPECT_EQ(fold(CompareOp::kEq, "4.5", false).kind, kFalse);
  EXPECT_EQ(fold(CompareOp::kNe, "4.5", false).kind, kIsNotNull);
  // Outside the range.
  EXPECT_EQ(fold(CompareOp::kEq, "40000", false).kind, kFalse);
  EXPECT_EQ(fold(CompareOp::kNe, "40000", true).kind, kIsNotNull);
  EXPECT_EQ(fold(CompareOp::kLt, "40000", false).kind, kIsNotNull);
  EXPECT_EQ(fold(CompareOp::kLe, "40000", true).kind, kFalse);
  EXPECT_EQ(fold(CompareOp::kGt, "40000", true).kind, kIsNotNull);
  EXPECT_EQ(fold(CompareOp::kGe, "40000", false).kind, kFalse);
  EXPECT_EQ(fold(CompareOp::kGe, "1e50", true).kind, kIsNotNull);
  EXPECT_EQ(fold(CompareOp::kLe, "1e50", false).kind, kIsNotNull);
}

}  // namespace
}  // namespace antb1::plan
