#include "antb1/common/int128.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>

#include <gtest/gtest.h>

namespace antb1 {
namespace {

TEST(Int128Test, ToStringHandlesExtremes) {
  EXPECT_EQ(Int128ToString(0), "0");
  EXPECT_EQ(Int128ToString(-42), "-42");
  EXPECT_EQ(Int128ToString(kInt128Max), "170141183460469231731687303715884105727");
  EXPECT_EQ(Int128ToString(kInt128Min), "-170141183460469231731687303715884105728");
}

TEST(Int128Test, ToInt64ChecksRange) {
  EXPECT_EQ(Int128ToInt64(static_cast<Int128>(INT64_MAX)), INT64_MAX);
  EXPECT_EQ(Int128ToInt64(static_cast<Int128>(INT64_MIN)), INT64_MIN);
  EXPECT_FALSE(Int128ToInt64(static_cast<Int128>(INT64_MAX) + 1).has_value());
  EXPECT_FALSE(Int128ToInt64(static_cast<Int128>(INT64_MIN) - 1).has_value());
}

TEST(Int128Test, CheckedAddDetectsOverflow) {
  EXPECT_EQ(CheckedAdd(1, 2), Int128{3});
  EXPECT_FALSE(CheckedAdd(kInt128Max, 1).has_value());
  EXPECT_FALSE(CheckedAdd(kInt128Min, -1).has_value());
}

TEST(Int128Test, ExactDivideMatchesDoubleForSmallValues) {
  EXPECT_DOUBLE_EQ(ExactDivideToDouble(7, 2), 3.5);
  EXPECT_DOUBLE_EQ(ExactDivideToDouble(-7, 2), -3.5);
  // Sum of two INT64_MAX values averaged must not overflow.
  const Int128 sum = static_cast<Int128>(INT64_MAX) * 2;
  EXPECT_DOUBLE_EQ(ExactDivideToDouble(sum, 2), static_cast<double>(INT64_MAX));
}

// Parses a decimal integer of up to 39 digits (test data only).
Int128 Big(const std::string& text) {
  Int128 value = 0;
  const bool negative = text.starts_with('-');
  for (const char c : text.substr(negative ? 1 : 0)) {
    value = (value * 10) + (c - '0');
  }
  return negative ? -value : value;
}

TEST(Int128Test, PowerOfTen) {
  EXPECT_EQ(PowerOfTen(0), Int128{1});
  EXPECT_EQ(PowerOfTen(18), Int128{1'000'000'000'000'000'000});
  EXPECT_EQ(Int128ToString(PowerOfTen(38)), "1" + std::string(38, '0'));
}

// a / 10^sa against b / 10^sb, exactly: equal values of different scales, a difference in the last
// digit, signs, and scalings that overflow 128 bits (the scaled side's sign decides), with the
// extremes of Int128 (a file can hold a value beyond its declared width).
TEST(Int128Test, CompareScaled) {
  struct Case {
    Int128 a;
    int a_scale;
    Int128 b;
    int b_scale;
    int expected;
  };
  const Int128 max38 = PowerOfTen(38) - 1;
  for (const Case& c : {
           Case{.a = 123, .a_scale = 2, .b = 1230, .b_scale = 3, .expected = 0},
           Case{.a = 123, .a_scale = 2, .b = 1229, .b_scale = 3, .expected = 1},
           Case{.a = -123, .a_scale = 2, .b = -1231, .b_scale = 3, .expected = 1},
           Case{.a = -123, .a_scale = 2, .b = -1229, .b_scale = 3, .expected = -1},
           Case{.a = 0, .a_scale = 0, .b = 0, .b_scale = 38, .expected = 0},
           Case{.a = 5, .a_scale = 0, .b = 49999, .b_scale = 4, .expected = 1},
           Case{.a = 7, .a_scale = 0, .b = 7, .b_scale = 0, .expected = 0},
           Case{.a = 1, .a_scale = 38, .b = 0, .b_scale = 0, .expected = 1},
           // max38 * 10^10 overflows: 10^38 - 1 is far above any DECIMAL(38,10) value.
           Case{.a = max38, .a_scale = 0, .b = max38, .b_scale = 10, .expected = 1},
           Case{.a = -max38, .a_scale = 0, .b = max38, .b_scale = 10, .expected = -1},
           Case{.a = max38, .a_scale = 10, .b = -max38, .b_scale = 0, .expected = 1},
           Case{.a = max38, .a_scale = 10, .b = max38, .b_scale = 0, .expected = -1},
           Case{.a = kInt128Min, .a_scale = 0, .b = kInt128Min, .b_scale = 38, .expected = -1},
           Case{.a = kInt128Max, .a_scale = 38, .b = kInt128Max, .b_scale = 0, .expected = -1},
           Case{.a = kInt128Min, .a_scale = 2, .b = kInt128Min, .b_scale = 2, .expected = 0},
           Case{.a = kInt128Max, .a_scale = 1, .b = kInt128Min, .b_scale = 0, .expected = 1},
       }) {
    SCOPED_TRACE(Int128ToString(c.a) + "e-" + std::to_string(c.a_scale) + " vs " +
                 Int128ToString(c.b) + "e-" + std::to_string(c.b_scale));
    EXPECT_EQ(CompareScaled(c.a, c.a_scale, c.b, c.b_scale), c.expected);
    EXPECT_EQ(CompareScaled(c.b, c.b_scale, c.a, c.a_scale), -c.expected);
  }
}

// A DECIMAL cast to another scale, as DuckDB 1.5.5 casts it: up exactly (an overflow of 128 bits is
// std::nullopt), down rounded half away from zero, also by 38 digits and at the extremes.
TEST(Int128Test, Rescale) {
  struct Case {
    Int128 value;
    int from;
    int to;
    std::optional<Int128> expected;
  };
  const Int128 max38 = PowerOfTen(38) - 1;
  for (const Case& c : {
           Case{.value = 15, .from = 1, .to = 0, .expected = 2},
           Case{.value = -15, .from = 1, .to = 0, .expected = -2},
           Case{.value = -25, .from = 1, .to = 0, .expected = -3},
           Case{.value = 24, .from = 1, .to = 0, .expected = 2},
           Case{.value = -4, .from = 1, .to = 0, .expected = 0},
           Case{.value = 0, .from = 5, .to = 0, .expected = 0},
           Case{.value = 5, .from = 11, .to = 10, .expected = 1},
           Case{.value = -5, .from = 11, .to = 10, .expected = -1},
           Case{.value = 1250, .from = 3, .to = 1, .expected = 13},
           Case{.value = 1249, .from = 3, .to = 1, .expected = 12},
           // 9999999999999999999999999999.9999999999 and -9999999999999999999999999999.5 to
           // scale 0.
           Case{.value = max38, .from = 10, .to = 0, .expected = PowerOfTen(28)},
           Case{.value = -(PowerOfTen(38) - (PowerOfTen(9) * 5)),
                .from = 10,
                .to = 0,
                .expected = -PowerOfTen(28)},
           Case{.value = (PowerOfTen(38) / 2) - 1, .from = 38, .to = 0, .expected = 0},
           Case{.value = PowerOfTen(38) / 2, .from = 38, .to = 0, .expected = 1},
           Case{.value = -(PowerOfTen(38) / 2), .from = 38, .to = 0, .expected = -1},
           // The minimum's tenth, ...0572.8, rounds away from zero.
           Case{.value = kInt128Min, .from = 1, .to = 0, .expected = (kInt128Min / 10) - 1},
           Case{.value = 123, .from = 2, .to = 5, .expected = 123000},
           Case{.value = -1, .from = 0, .to = 38, .expected = -PowerOfTen(38)},
           Case{.value = max38, .from = 0, .to = 0, .expected = max38},
           Case{.value = kInt128Max / 10, .from = 0, .to = 1, .expected = (kInt128Max / 10) * 10},
           Case{.value = max38, .from = 0, .to = 1, .expected = std::nullopt},
           Case{.value = kInt128Min, .from = 0, .to = 1, .expected = std::nullopt},
       }) {
    SCOPED_TRACE(Int128ToString(c.value) + "e-" + std::to_string(c.from) + " to scale " +
                 std::to_string(c.to));
    const std::optional<Int128> got = Rescale(c.value, c.from, c.to);
    ASSERT_EQ(got.has_value(), c.expected.has_value());
    if (got.has_value() && c.expected.has_value()) {
      EXPECT_EQ(Int128ToString(*got), Int128ToString(*c.expected));
    }
  }
}

// DuckDB 1.5.5's cast of a DECIMAL to DOUBLE (ADR 0021 rule 8), bit for bit: within 2^53 (or at a
// width up to 4, or scale 0) one division; beyond it div + mod / 10^scale, which is not the
// nearest double in the five cases from DuckDB (checked with CAST(<literal> AS DOUBLE)).
TEST(Int128Test, DuckDbDecimalToDouble) {
  struct Case {
    std::string unscaled;
    int width;
    int scale;
    double expected;
  };
  for (const Case& c : {
           Case{.unscaled = "125", .width = 4, .scale = 3, .expected = 0.125},
           Case{.unscaled = "-25", .width = 2, .scale = 1, .expected = -2.5},
           Case{.unscaled = "9007199254740992",
                .width = 17,
                .scale = 1,
                .expected = 900719925474099.2},
           Case{.unscaled = "1152921504606846977", .width = 19, .scale = 0, .expected = 0x1p60},
           Case{.unscaled = "199489722791016982",
                .width = 18,
                .scale = 1,
                .expected = 1.9948972279101696e+16},
           Case{.unscaled = "60719098953061412540",
                .width = 20,
                .scale = 3,
                .expected = 6.071909895306141e+16},
           Case{.unscaled = "-949662803139880935336384",
                .width = 25,
                .scale = 8,
                .expected = -9496628031398808.0},
           Case{.unscaled = "-494847612481479776948244721282",
                .width = 30,
                .scale = 12,
                .expected = -4.9484761248147974e+17},
           Case{.unscaled = "-92675172664100886869829230387221166619",
                .width = 38,
                .scale = 2,
                .expected = -9.26751726641009e+35},
       }) {
    EXPECT_EQ(DuckDbDecimalToDouble(Big(c.unscaled), c.width, c.scale), c.expected) << c.unscaled;
  }
}

// DuckDB 1.5.5's AVG of a DECIMAL, bit for bit, from sums of random sets where the correctly
// rounded mean differs in the last bit (each answer checked against DuckDB on x86-64). long double
// is the platform's, as in DuckDB's own build: x87 80-bit on x86-64 Linux, 64-bit on arm64 macOS.
TEST(Int128Test, DuckDbDecimalAverage) {
  struct Case {
    std::string sum;
    std::int64_t count;
    int width;
    int scale;
    double x87;       // 64-bit mantissa
    double double64;  // long double == double
  };
  for (const Case& c : {
           Case{.sum = "-6027685297180875454426411241489759",
                .count = 2,
                .width = 34,
                .scale = 4,
                .x87 = -3.013842648590438e+29,
                .double64 = -3.013842648590438e+29},
           Case{.sum = "-13223336620658",
                .count = 5,
                .width = 13,
                .scale = 5,
                .x87 = -26446673.241315998,
                .double64 = -26446673.241316},
           Case{.sum = "-128063935436827839243246709275367",
                .count = 2,
                .width = 32,
                .scale = 3,
                .x87 = -6.403196771841392e+28,
                .double64 = -6.403196771841392e+28},
           Case{.sum = "506931365701444",
                .count = 4,
                .width = 15,
                .scale = 6,
                .x87 = 126732841.42536101,
                .double64 = 126732841.425361},
           Case{.sum = "-3150176280006674350035463",
                .count = 4,
                .width = 25,
                .scale = 3,
                .x87 = -7.875440700016687e+20,
                .double64 = -7.875440700016685e+20},
           // The halves lower + upper * 2^64, not a direct conversion (ADR 0021 rule 13).
           Case{.sum = "33273625953879217449636401730702882204",
                .count = 1,
                .width = 38,
                .scale = 0,
                .x87 = 3.327362595387922e+37,
                .double64 = 3.327362595387922e+37},
           Case{.sum = "-96429140781460569102326976360742939557",
                .count = 1,
                .width = 38,
                .scale = 0,
                .x87 = -9.642914078146058e+37,
                .double64 = -9.642914078146058e+37},
           Case{.sum = "27670116110564329473",
                .count = 1,
                .width = 38,
                .scale = 0,
                .x87 = 27670116110564327424.0,
                .double64 = 27670116110564327424.0},
           // A 16-bit DECIMAL (width up to 4) averages in double.
           Case{.sum = "-1999",
                .count = 3,
                .width = 4,
                .scale = 2,
                .x87 = -6.663333333333333,
                .double64 = -6.663333333333333},
       }) {
    const double expected = std::numeric_limits<long double>::digits == 64 ? c.x87 : c.double64;
    EXPECT_EQ(DuckDbDecimalAverage(Big(c.sum), c.count, c.width, c.scale), expected)
        << c.sum << " / " << c.count;
  }
}

}  // namespace
}  // namespace antb1
