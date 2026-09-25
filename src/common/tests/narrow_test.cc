#include "antb1/common/narrow.h"

#include <cstdint>

#include <gtest/gtest.h>

namespace antb1 {
namespace {

TEST(NarrowTest, TryNarrowRejectsLoss) {
  EXPECT_EQ(TryNarrow<int16_t>(int64_t{123}), int16_t{123});
  EXPECT_FALSE(TryNarrow<int16_t>(int64_t{40000}).has_value());
  EXPECT_FALSE(TryNarrow<uint32_t>(-1).has_value());
}

TEST(NarrowTest, NarrowKeepsValue) { EXPECT_EQ(Narrow<int>(int64_t{7}), 7); }

TEST(NarrowDeathTest, NarrowAbortsOnLoss) { EXPECT_DEATH(Narrow<int8_t>(1000), "check failed"); }

}  // namespace
}  // namespace antb1
