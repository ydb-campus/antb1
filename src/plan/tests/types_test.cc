#include "antb1/plan/types.h"

#include <arrow/api.h>
#include <gtest/gtest.h>

namespace antb1::plan {
namespace {

TEST(TypesTest, RoundTripsThroughArrow) {
  for (auto t : {LogicalType::kSmallInt, LogicalType::kInteger, LogicalType::kBigInt,
                 LogicalType::kUSmallInt, LogicalType::kHugeInt, LogicalType::kDouble,
                 LogicalType::kVarchar, LogicalType::kDate}) {
    auto back = FromArrow(*ToArrow(t));
    ASSERT_TRUE(back.ok()) << ToString(t);
    EXPECT_EQ(*back, t);
  }
}

TEST(TypesTest, MapsStorageTypes) {
  EXPECT_EQ(*FromArrow(*arrow::utf8()), LogicalType::kVarchar);
  EXPECT_EQ(*FromArrow(*arrow::float32()), LogicalType::kDouble);
  EXPECT_TRUE(FromArrow(*arrow::list(arrow::int32())).status().IsNotImplemented());
  EXPECT_TRUE(FromArrow(*arrow::decimal128(10, 2)).status().IsNotImplemented());
}

TEST(TypesTest, Classification) {
  EXPECT_TRUE(IsInteger(LogicalType::kUSmallInt));
  EXPECT_FALSE(IsInteger(LogicalType::kDouble));
  EXPECT_TRUE(IsNumeric(LogicalType::kDouble));
  EXPECT_FALSE(IsNumeric(LogicalType::kDate));
}

}  // namespace
}  // namespace antb1::plan
