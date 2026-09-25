#include "antb1/common/int128.h"

#include <cstdint>

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

}  // namespace
}  // namespace antb1
