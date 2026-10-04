#include "antb1/common/int128.h"

#include <cstdint>
#include <limits>
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
