#include "antb1/plan/type_fold.h"

#include <vector>

#include <gtest/gtest.h>

#include "antb1/plan/types.h"

namespace antb1::plan {
namespace {

using enum LogicalType::Id;

std::vector<ValueType> Types() {
  return {std::nullopt,
          LogicalType(kSmallInt),
          LogicalType(kUSmallInt),
          LogicalType(kInteger),
          LogicalType(kBigInt),
          LogicalType(kHugeInt),
          LogicalType::Decimal(4, 1),
          LogicalType::Decimal(18, 3),
          LogicalType::Decimal(38, 10),
          LogicalType(kDouble),
          LogicalType(kVarchar),
          LogicalType(kDate),
          LogicalType(kTimestamp),
          LogicalType(kBoolean)};
}

// NULL is the identity, on either side; NULL with NULL stays NULL.
TEST(TypeFoldTest, NullIsTheIdentity) {
  for (const ValueType& t : Types()) {
    EXPECT_EQ(CommonValueType(std::nullopt, t), t);
    EXPECT_EQ(CommonValueType(t, std::nullopt), t);
    EXPECT_TRUE(DuckDbCaseMixes(std::nullopt, t));
  }
  EXPECT_EQ(CommonValueType(std::nullopt, std::nullopt), std::nullopt);
}

// The fold is commutative. It is not associative, as DuckDB's is not: USMALLINT with SMALLINT is
// INTEGER, so the order of a fold can change a DECIMAL's width, and a UNION ALL folds left in
// written order.
TEST(TypeFoldTest, IsCommutativeNotAssociative) {
  for (const ValueType& a : Types()) {
    for (const ValueType& b : Types()) {
      EXPECT_EQ(CommonValueType(a, b), CommonValueType(b, a));
      EXPECT_EQ(DuckDbCaseMixes(a, b), DuckDbCaseMixes(b, a));
    }
  }
  const ValueType usmallint = LogicalType(kUSmallInt);
  const ValueType smallint = LogicalType(kSmallInt);
  const ValueType decimal = LogicalType::Decimal(4, 1);
  EXPECT_EQ(CommonValueType(CommonValueType(usmallint, smallint), decimal),
            LogicalType::Decimal(11, 1));
  EXPECT_EQ(CommonValueType(usmallint, CommonValueType(smallint, decimal)),
            LogicalType::Decimal(6, 1));
}

TEST(TypeFoldTest, CombinesAsDuckDbDoes) {
  const auto common = [](ValueType a, ValueType b) { return CommonValueType(a, b); };
  EXPECT_EQ(common(LogicalType(kUSmallInt), LogicalType(kSmallInt)), LogicalType(kInteger));
  EXPECT_EQ(common(LogicalType(kInteger), LogicalType(kBigInt)), LogicalType(kBigInt));
  EXPECT_EQ(common(LogicalType(kBigInt), LogicalType::Decimal(4, 1)), LogicalType::Decimal(20, 1));
  EXPECT_EQ(common(LogicalType(kUSmallInt), LogicalType::Decimal(4, 1)),
            LogicalType::Decimal(6, 1));
  EXPECT_EQ(common(LogicalType(kHugeInt), LogicalType::Decimal(4, 1)), LogicalType::Decimal(38, 1));
  EXPECT_EQ(common(LogicalType::Decimal(38, 10), LogicalType::Decimal(18, 3)),
            LogicalType::Decimal(38, 10));
  EXPECT_EQ(common(LogicalType(kHugeInt), LogicalType(kDouble)), LogicalType(kDouble));
  EXPECT_EQ(common(LogicalType::Decimal(4, 1), LogicalType(kDouble)), LogicalType(kDouble));
  EXPECT_EQ(common(LogicalType(kBoolean), LogicalType(kInteger)), LogicalType(kInteger));
  EXPECT_EQ(common(LogicalType(kBoolean), LogicalType(kDouble)), LogicalType(kDouble));
  EXPECT_EQ(common(LogicalType(kDate), LogicalType(kTimestamp)), LogicalType(kTimestamp));
  EXPECT_EQ(common(LogicalType(kDate), LogicalType(kInteger)), LogicalType(kDate));
  EXPECT_EQ(common(LogicalType(kVarchar), LogicalType(kDate)), LogicalType(kVarchar));
  EXPECT_EQ(common(LogicalType(kVarchar), LogicalType(kBoolean)), LogicalType(kVarchar));
}

TEST(TypeFoldTest, CaseRefusesTheMixesDuckDbsCaseRefuses) {
  EXPECT_TRUE(DuckDbCaseMixes(LogicalType(kInteger), LogicalType::Decimal(4, 1)));
  EXPECT_TRUE(DuckDbCaseMixes(LogicalType(kBoolean), LogicalType(kBigInt)));
  EXPECT_TRUE(DuckDbCaseMixes(LogicalType(kDate), LogicalType(kTimestamp)));
  EXPECT_FALSE(DuckDbCaseMixes(LogicalType(kVarchar), LogicalType(kInteger)));
  EXPECT_FALSE(DuckDbCaseMixes(LogicalType(kBoolean), LogicalType(kDouble)));
  EXPECT_FALSE(DuckDbCaseMixes(LogicalType(kBoolean), LogicalType::Decimal(4, 1)));
  EXPECT_FALSE(DuckDbCaseMixes(LogicalType(kDate), LogicalType(kInteger)));
  EXPECT_FALSE(DuckDbCaseMixes(LogicalType(kTimestamp), LogicalType(kBoolean)));
}

}  // namespace
}  // namespace antb1::plan
