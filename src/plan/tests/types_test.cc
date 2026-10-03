#include "antb1/plan/types.h"

#include <array>
#include <cstddef>
#include <sstream>
#include <string_view>
#include <type_traits>

#include <arrow/api.h>
#include <gtest/gtest.h>

namespace antb1::plan {
namespace {

struct NamedType {
  LogicalType::Id id;
  std::string_view name;
};

// Every type with its name, in declaration order (DECIMAL as its id alone, which no column has: a
// DECIMAL is made with LogicalType::Decimal).
constexpr std::array<NamedType, 11> kTypes = {{
    {.id = LogicalType::kSmallInt, .name = "SMALLINT"},
    {.id = LogicalType::kInteger, .name = "INTEGER"},
    {.id = LogicalType::kBigInt, .name = "BIGINT"},
    {.id = LogicalType::kUSmallInt, .name = "USMALLINT"},
    {.id = LogicalType::kHugeInt, .name = "HUGEINT"},
    {.id = LogicalType::kDecimal, .name = "DECIMAL(0,0)"},
    {.id = LogicalType::kDouble, .name = "DOUBLE"},
    {.id = LogicalType::kVarchar, .name = "VARCHAR"},
    {.id = LogicalType::kDate, .name = "DATE"},
    {.id = LogicalType::kTimestamp, .name = "TIMESTAMP"},
    {.id = LogicalType::kBoolean, .name = "BOOLEAN"},
}};

// True for the last id only. The switch has no default, so -Wswitch makes a new id update it, and
// the static_assert below then makes the new id a row of kTypes.
constexpr bool IsLastId(LogicalType::Id id) {
  switch (id) {
    case LogicalType::kSmallInt:
    case LogicalType::kInteger:
    case LogicalType::kBigInt:
    case LogicalType::kUSmallInt:
    case LogicalType::kHugeInt:
    case LogicalType::kDecimal:
    case LogicalType::kDouble:
    case LogicalType::kVarchar:
    case LogicalType::kDate:
    case LogicalType::kTimestamp:
      return false;
    case LogicalType::kBoolean:
      return true;
  }
  return false;
}

// Row i holds id i, and the last row holds the last id: the table has every id once.
constexpr bool ListsEveryIdInOrder() {
  for (std::size_t i = 0; i < kTypes.size(); ++i) {
    if (kTypes[i].id != static_cast<LogicalType::Id>(i)) {
      return false;
    }
  }
  return IsLastId(kTypes.back().id);
}
static_assert(ListsEveryIdInOrder());

// An id converts to a type implicitly, a type to an id only through id(): a switch must name it.
static_assert(std::is_convertible_v<LogicalType::Id, LogicalType>);
static_assert(!std::is_convertible_v<LogicalType, LogicalType::Id>);
static_assert(std::is_trivially_copyable_v<LogicalType>);
static_assert(LogicalType{}.id() == LogicalType::kSmallInt);
static_assert(LogicalType(LogicalType::kDate) == LogicalType::kDate);
static_assert(LogicalType(LogicalType::kDate) != LogicalType(LogicalType::kTimestamp));

TEST(TypesTest, RoundTripsThroughArrow) {
  // Not `auto`: that would be an Id, and EXPECT_EQ would compare the ids only.
  for (const LogicalType t : std::to_array<LogicalType>(
           {LogicalType::kSmallInt, LogicalType::kInteger, LogicalType::kBigInt,
            LogicalType::kUSmallInt, LogicalType::kDouble, LogicalType::kVarchar,
            LogicalType::kDate, LogicalType::Decimal(15, 2), LogicalType::Decimal(38, 0)})) {
    auto back = FromArrow(*ToArrow(t));
    ASSERT_TRUE(back.ok()) << ToString(t);
    EXPECT_EQ(*back, t);
  }
  // HUGEINT (integer SUM) is stored as decimal128(38,0), which a column of a table reads as
  // DECIMAL(38,0) (ADR 0021 rule 2): no table column is HUGEINT.
  EXPECT_EQ(*FromArrow(*ToArrow(LogicalType::kHugeInt)), LogicalType::Decimal(38, 0));
}

TEST(TypesTest, MapsStorageTypes) {
  EXPECT_EQ(*FromArrow(*arrow::utf8()), LogicalType::kVarchar);
  EXPECT_EQ(*FromArrow(*arrow::float32()), LogicalType::kDouble);
  EXPECT_TRUE(FromArrow(*arrow::list(arrow::int32())).status().IsNotImplemented());
}

// Every decimal of at most 38 digits is DECIMAL(p, s), whatever its Arrow width; DECIMAL(38, 0) is
// no HUGEINT (ADR 0021 rule 2).
TEST(TypesTest, MapsDecimals) {
  EXPECT_EQ(*FromArrow(*arrow::decimal128(15, 2)), LogicalType::Decimal(15, 2));
  EXPECT_EQ(*FromArrow(*arrow::decimal128(38, 0)), LogicalType::Decimal(38, 0));
  EXPECT_NE(*FromArrow(*arrow::decimal128(38, 0)), LogicalType(LogicalType::kHugeInt));
  EXPECT_EQ(*FromArrow(*arrow::decimal32(9, 2)), LogicalType::Decimal(9, 2));
  EXPECT_EQ(*FromArrow(*arrow::decimal64(18, 18)), LogicalType::Decimal(18, 18));
  EXPECT_EQ(*FromArrow(*arrow::decimal256(38, 10)), LogicalType::Decimal(38, 10));
  EXPECT_TRUE(FromArrow(*arrow::decimal256(39, 0)).status().IsNotImplemented());
  EXPECT_TRUE(FromArrow(*arrow::decimal128(10, -2)).status().IsNotImplemented());
  const LogicalType decimal = LogicalType::Decimal(15, 2);
  EXPECT_EQ(decimal.width(), 15);
  EXPECT_EQ(decimal.scale(), 2);
  EXPECT_EQ(ToString(decimal), "DECIMAL(15,2)");
  EXPECT_TRUE(ToArrow(decimal)->Equals(*arrow::decimal128(15, 2)));
  EXPECT_EQ(*FromArrow(*ToArrow(decimal)), decimal);
  // Another precision or scale is another type, the same id.
  EXPECT_NE(decimal, LogicalType::Decimal(15, 3));
  EXPECT_NE(decimal, LogicalType::Decimal(16, 2));
  EXPECT_TRUE(decimal == LogicalType::kDecimal);
  // Never an integer or a number for the binder's checks: those contexts reject DECIMAL.
  EXPECT_FALSE(IsInteger(decimal));
  EXPECT_FALSE(IsNumeric(decimal));
  EXPECT_FALSE(IsInteger(LogicalType::Decimal(38, 0)));
}

TEST(TypesTest, Classification) {
  EXPECT_TRUE(IsInteger(LogicalType::kUSmallInt));
  EXPECT_FALSE(IsInteger(LogicalType::kDouble));
  EXPECT_TRUE(IsNumeric(LogicalType::kDouble));
  EXPECT_FALSE(IsNumeric(LogicalType::kDate));
}

TEST(TypesTest, NamesEveryType) {
  for (const NamedType& named : kTypes) {
    EXPECT_EQ(ToString(named.id), named.name);
  }
}

TEST(TypesTest, TypesWithoutParametersHaveNoWidthOrScale) {
  for (const NamedType& named : kTypes) {
    const LogicalType type = named.id;
    EXPECT_EQ(type.id(), named.id);
    EXPECT_EQ(type.width(), 0) << named.name;
    EXPECT_EQ(type.scale(), 0) << named.name;
  }
}

TEST(TypesTest, DefaultsToSmallInt) {
  const LogicalType type;
  EXPECT_EQ(type, LogicalType(LogicalType::kSmallInt));
  EXPECT_EQ(type.width(), 0);
  EXPECT_EQ(type.scale(), 0);
}

TEST(TypesTest, ComparesWithTypesAndIds) {
  for (const NamedType& left : kTypes) {
    for (const NamedType& right : kTypes) {
      const bool same = &left == &right;
      const LogicalType left_type = left.id;
      const LogicalType right_type = right.id;
      EXPECT_EQ(left_type == right_type, same) << left.name << " " << right.name;
      EXPECT_EQ(left_type != right_type, !same) << left.name << " " << right.name;
      EXPECT_EQ(left_type == right.id, same) << left.name << " " << right.name;
      EXPECT_EQ(left_type != right.id, !same) << left.name << " " << right.name;
      EXPECT_EQ(left.id == right_type, same) << left.name << " " << right.name;
      EXPECT_EQ(left.id != right_type, !same) << left.name << " " << right.name;
    }
  }
}

TEST(TypesTest, PrintsItsName) {
  std::ostringstream os;
  os << LogicalType(LogicalType::kHugeInt) << " " << LogicalType::kVarchar;
  EXPECT_EQ(os.str(), "HUGEINT VARCHAR");
  // gtest failure messages show the name, for a type and for an id.
  EXPECT_EQ(testing::PrintToString(LogicalType(LogicalType::kDate)), "DATE");
  EXPECT_EQ(testing::PrintToString(LogicalType::kBoolean), "BOOLEAN");
}

}  // namespace
}  // namespace antb1::plan
