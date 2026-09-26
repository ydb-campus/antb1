// Exact literal folding for integer columns (docs/sql-subset.md, "Literals"): the folded predicate
// must accept exactly the column values that the exact comparison accepts, for every operator, at
// and around every type boundary.

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "antb1/common/int128.h"
#include "antb1/plan/literal.h"
#include "antb1/plan/logical_plan.h"

#include "plan_test_util.h"

namespace antb1::plan {
namespace {

using testing::BindSql;
using testing::MakeCatalog;
using testing::Nth;

constexpr std::array<CompareOp, 6> kOps = {CompareOp::kEq, CompareOp::kNe, CompareOp::kLt,
                                           CompareOp::kLe, CompareOp::kGt, CompareOp::kGe};

// A test literal: value = numerator / 10^scale exactly, or a huge value (beyond Int128) with a
// sign.
struct TestLiteral {
  std::string text;  // without the sign
  bool negative = false;
  Int128 numerator = 0;  // signed
  int scale = 0;
  bool huge = false;
};

Int128 Pow10(int n) {
  Int128 v = 1;
  for (int i = 0; i < n; ++i) {
    v *= 10;
  }
  return v;
}

// A literal written as an integer (or with a zero fraction) or as n + 1/2, n - 1/2 ...
TestLiteral Integer(Int128 value) {
  return TestLiteral{.text = Int128ToString(value < 0 ? -value : value),
                     .negative = value < 0,
                     .numerator = value,
                     .scale = 0};
}

TestLiteral WithFraction(Int128 value, std::string_view fraction) {
  // value.fraction, e.g. (-3, "5") is -3.5.
  const int scale = static_cast<int>(fraction.size());
  Int128 frac = 0;
  for (const char c : fraction) {
    frac = (frac * 10) + (c - '0');
  }
  const Int128 magnitude = ((value < 0 ? -value : value) * Pow10(scale)) + frac;
  return TestLiteral{
      .text = Int128ToString(value < 0 ? -value : value) + "." + std::string(fraction),
      .negative = value < 0,
      .numerator = value < 0 ? -magnitude : magnitude,
      .scale = scale};
}

// Exact three-way comparison of an integer with a literal.
int Compare(Int128 v, const TestLiteral& lit) {
  if (lit.huge) {
    return lit.negative ? 1 : -1;
  }
  const Int128 scaled = v * Pow10(lit.scale);  // |v| < 2^64, scale <= 20: no overflow
  if (scaled < lit.numerator) {
    return -1;
  }
  return scaled > lit.numerator ? 1 : 0;
}

bool Holds(CompareOp op, int cmp) {
  switch (op) {
    case CompareOp::kEq:
      return cmp == 0;
    case CompareOp::kNe:
      return cmp != 0;
    case CompareOp::kLt:
      return cmp < 0;
    case CompareOp::kLe:
      return cmp <= 0;
    case CompareOp::kGt:
      return cmp > 0;
    case CompareOp::kGe:
      return cmp >= 0;
  }
  return false;
}

bool Accepts(const FoldedComparison& folded, Int128 v) {
  switch (folded.kind) {
    case Predicate::Kind::kFalse:
      return false;
    case Predicate::Kind::kIsNotNull:
      return true;
    case Predicate::Kind::kCompare: {
      int cmp = 0;
      if (v < folded.value) {
        cmp = -1;
      } else if (v > folded.value) {
        cmp = 1;
      }
      return Holds(folded.op, cmp);
    }
  }
  return false;
}

// Boundary literals of a range: lo - 1, lo, lo + 1, hi - 1, hi, hi + 1, the halves around them,
// zero, +-1, +-0.5, and values beyond every type.
std::vector<TestLiteral> BoundaryLiterals(IntegerRange range) {
  std::vector<TestLiteral> out;
  for (const Int128 v : {range.min - 1, range.min, range.min + 1, range.max - 1, range.max,
                         range.max + 1, Int128{0}, Int128{1}, Int128{-1}, Int128{7}}) {
    out.push_back(Integer(v));
    out.push_back(WithFraction(v, "5"));
    out.push_back(WithFraction(v, "25"));
    out.push_back(WithFraction(v, "0"));
    out.push_back(WithFraction(v, "999999999999"));
  }
  out.push_back(TestLiteral{.text = "0.5", .numerator = 5, .scale = 1});
  out.push_back(TestLiteral{.text = "0.5", .negative = true, .numerator = -5, .scale = 1});
  out.push_back(TestLiteral{.text = "0.0", .negative = true, .numerator = 0, .scale = 1});
  out.push_back(TestLiteral{.text = "1" + std::string(40, '0'), .huge = true});
  out.push_back(TestLiteral{.text = "1" + std::string(40, '0'), .negative = true, .huge = true});
  out.push_back(TestLiteral{.text = "1e300", .huge = true});
  out.push_back(TestLiteral{.text = "2.5e300", .negative = true, .huge = true});
  return out;
}

void CheckValues(LogicalType type, const std::vector<Int128>& values) {
  const IntegerRange range = RangeOf(type);
  for (const TestLiteral& lit : BoundaryLiterals(range)) {
    const auto exact = ParseExactNumber(lit.text, lit.negative);
    if (!exact.has_value()) {
      ADD_FAILURE() << "not a number: " << lit.text;
      continue;
    }
    for (const CompareOp op : kOps) {
      const FoldedComparison folded = FoldIntegerComparison(op, *exact, range);
      if (folded.kind == Predicate::Kind::kCompare) {
        EXPECT_GE(folded.value, range.min) << lit.text;
        EXPECT_LE(folded.value, range.max) << lit.text;
      }
      for (const Int128 v : values) {
        ASSERT_EQ(Accepts(folded, v), Holds(op, Compare(v, lit)))
            << ToString(type) << " value " << Int128ToString(v) << " " << ToString(op) << " "
            << (lit.negative ? "-" : "") << lit.text;
      }
    }
  }
}

// Every value of the 16-bit types.
TEST(FoldingTest, SmallIntAllValues) {
  std::vector<Int128> values;
  for (int v = std::numeric_limits<int16_t>::min(); v <= std::numeric_limits<int16_t>::max(); ++v) {
    values.push_back(v);
  }
  CheckValues(LogicalType::kSmallInt, values);
}

TEST(FoldingTest, USmallIntAllValues) {
  std::vector<Int128> values;
  for (int v = 0; v <= int{std::numeric_limits<uint16_t>::max()}; ++v) {
    values.push_back(v);
  }
  CheckValues(LogicalType::kUSmallInt, values);
}

// Values around the boundaries and around zero for the wide types.
std::vector<Int128> ValuesNearBoundaries(IntegerRange range) {
  std::vector<Int128> values;
  for (Int128 d = 0; d < 20; ++d) {
    values.push_back(range.min + d);
    values.push_back(range.max - d);
    values.push_back(d - 10);
  }
  return values;
}

TEST(FoldingTest, IntegerNearBoundaries) {
  CheckValues(LogicalType::kInteger, ValuesNearBoundaries(RangeOf(LogicalType::kInteger)));
}

TEST(FoldingTest, BigIntNearBoundaries) {
  CheckValues(LogicalType::kBigInt, ValuesNearBoundaries(RangeOf(LogicalType::kBigInt)));
}

TEST(FoldingTest, Ranges) {
  EXPECT_EQ(RangeOf(LogicalType::kSmallInt).min, -32768);
  EXPECT_EQ(RangeOf(LogicalType::kSmallInt).max, 32767);
  EXPECT_EQ(RangeOf(LogicalType::kUSmallInt).min, 0);
  EXPECT_EQ(RangeOf(LogicalType::kUSmallInt).max, 65535);
  EXPECT_EQ(RangeOf(LogicalType::kInteger).min, std::numeric_limits<int32_t>::min());
  EXPECT_EQ(RangeOf(LogicalType::kInteger).max, std::numeric_limits<int32_t>::max());
  EXPECT_EQ(RangeOf(LogicalType::kBigInt).min, std::numeric_limits<int64_t>::min());
  EXPECT_EQ(RangeOf(LogicalType::kBigInt).max, std::numeric_limits<int64_t>::max());
  EXPECT_EQ(Int128ToString(RangeOf(LogicalType::kHugeInt).max), std::string(38, '9'));
  EXPECT_EQ(Int128ToString(RangeOf(LogicalType::kHugeInt).min), "-" + std::string(38, '9'));
}

// The folding table through the binder: SQL text in, the predicate the plan holds out.
struct FoldCase {
  std::string_view where;
  Predicate::Kind kind;
  CompareOp op = CompareOp::kEq;
  Int128 value = 0;
};

void PrintTo(const FoldCase& c, std::ostream* os) { *os << c.where; }

class FoldThroughBinderTest : public ::testing::TestWithParam<FoldCase> {};

TEST_P(FoldThroughBinderTest, Folds) {
  const FoldCase& c = GetParam();
  const Catalog catalog = MakeCatalog();
  const std::string sql = "SELECT COUNT(*) FROM t WHERE " + std::string(c.where);
  auto plan = BindSql(sql, catalog);
  ASSERT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
  const Predicate& p = std::get<FilterNode>(Nth(*plan, 1)).predicates.at(0);
  EXPECT_EQ(p.kind, c.kind);
  EXPECT_EQ(p.column.has_value(), c.kind != Predicate::Kind::kFalse);
  if (c.kind == Predicate::Kind::kCompare) {
    EXPECT_EQ(p.op, c.op);
    EXPECT_EQ(Int128ToString(std::get<Int128>(p.constant.value)), Int128ToString(c.value));
  }
}

using enum Predicate::Kind;

constexpr Int128 kI64Max = std::numeric_limits<int64_t>::max();
constexpr Int128 kI64Min = std::numeric_limits<int64_t>::min();

INSTANTIATE_TEST_SUITE_P(
    Binder, FoldThroughBinderTest,
    ::testing::Values(
        // SMALLINT boundaries.
        FoldCase{"i16 = 32767", kCompare, CompareOp::kEq, 32767}, FoldCase{"i16 = 32768", kFalse},
        FoldCase{"i16 <> 32768", kIsNotNull}, FoldCase{"i16 < 32768", kIsNotNull},
        FoldCase{"i16 <= 32768", kIsNotNull}, FoldCase{"i16 > 32768", kFalse},
        FoldCase{"i16 >= 32768", kFalse},
        FoldCase{"i16 >= -32768", kCompare, CompareOp::kGe, -32768},
        FoldCase{"i16 > -32769", kIsNotNull}, FoldCase{"i16 < -32769", kFalse},
        FoldCase{"-32769 < i16", kIsNotNull}, FoldCase{"-32769 = i16", kFalse},
        FoldCase{"i16 > 32767.5", kFalse},
        FoldCase{"i16 < 32767.5", kCompare, CompareOp::kLe, 32767},
        FoldCase{"i16 >= -32768.5", kCompare, CompareOp::kGe, -32768},
        FoldCase{"i16 <= -32768.5", kFalse},
        // USMALLINT: negative literals are below the range.
        FoldCase{"u16 >= 0", kCompare, CompareOp::kGe, 0}, FoldCase{"u16 >= -1", kIsNotNull},
        FoldCase{"u16 < 0", kCompare, CompareOp::kLt, 0}, FoldCase{"u16 < -0.5", kFalse},
        FoldCase{"u16 > -0.5", kCompare, CompareOp::kGe, 0},
        FoldCase{"u16 = -0.0", kCompare, CompareOp::kEq, 0},
        FoldCase{"u16 <= 65535", kCompare, CompareOp::kLe, 65535},
        FoldCase{"u16 <= 65536", kIsNotNull}, FoldCase{"u16 = 65536", kFalse},
        // INTEGER.
        FoldCase{"i32 > 2147483647", kCompare, CompareOp::kGt, 2147483647},
        FoldCase{"i32 > 2147483648", kFalse}, FoldCase{"i32 <= 2147483648", kIsNotNull},
        FoldCase{"i32 >= -2147483648", kCompare, CompareOp::kGe, -2147483648LL},
        FoldCase{"i32 < -2147483648.5", kFalse},
        // BIGINT: literals beyond int64 compare exactly (DuckDB widens to HUGEINT).
        FoldCase{"i64 = 9223372036854775807", kCompare, CompareOp::kEq, kI64Max},
        FoldCase{"i64 = 9223372036854775808", kFalse},
        FoldCase{"i64 < 9223372036854775808", kIsNotNull},
        FoldCase{"i64 >= -9223372036854775808", kCompare, CompareOp::kGe, kI64Min},
        FoldCase{"i64 > -9223372036854775809", kIsNotNull},
        FoldCase{"i64 > 9223372036854775807.5", kFalse},
        FoldCase{"i64 < 9223372036854775806.5", kCompare, CompareOp::kLe, kI64Max - 1},
        FoldCase{"i64 < 99999999999999999999999999999999999999999999", kIsNotNull},
        FoldCase{"i64 = -99999999999999999999999999999999999999999999.5", kFalse},
        // Decimal literals against integer columns: the nearest integer on the kept side.
        FoldCase{"i64 > 1.5", kCompare, CompareOp::kGe, 2},
        FoldCase{"i64 >= 1.5", kCompare, CompareOp::kGe, 2},
        FoldCase{"i64 < 1.5", kCompare, CompareOp::kLe, 1},
        FoldCase{"i64 <= 1.5", kCompare, CompareOp::kLe, 1}, FoldCase{"i64 = 1.5", kFalse},
        FoldCase{"i64 <> 1.5", kIsNotNull}, FoldCase{"i64 > -1.5", kCompare, CompareOp::kGe, -1},
        FoldCase{"i64 < -1.5", kCompare, CompareOp::kLe, -2},
        FoldCase{"i64 > 0.001", kCompare, CompareOp::kGe, 1},
        FoldCase{"i64 < -0.001", kCompare, CompareOp::kLe, -1},
        FoldCase{"i64 = 2.000", kCompare, CompareOp::kEq, 2}, FoldCase{"i64 = 25e-1", kFalse},
        FoldCase{"i64 = 250e-2", kFalse}, FoldCase{"i64 = 2500e-3", kFalse},
        FoldCase{"i64 = 2000e-3", kCompare, CompareOp::kEq, 2},
        FoldCase{"i64 > 1e3", kCompare, CompareOp::kGt, 1000},
        FoldCase{"i64 > 1.5e1", kCompare, CompareOp::kGt, 15},
        FoldCase{"i64 > 1.55e1", kCompare, CompareOp::kGe, 16}, FoldCase{"i64 < 1e19", kIsNotNull},
        FoldCase{"i64 > -1e19", kIsNotNull}, FoldCase{"i64 < 1e-5", kCompare, CompareOp::kLe, 0},
        FoldCase{"1.5 < i64", kCompare, CompareOp::kGe, 2},
        // HUGEINT: +-(10^38 - 1).
        FoldCase{"h = 99999999999999999999999999999999999999", kCompare, CompareOp::kEq,
                 RangeOf(LogicalType::kHugeInt).max},
        FoldCase{"h < 100000000000000000000000000000000000000", kIsNotNull},
        FoldCase{"h > -100000000000000000000000000000000000000", kIsNotNull},
        FoldCase{"h >= 1e38", kFalse}));

}  // namespace
}  // namespace antb1::plan
