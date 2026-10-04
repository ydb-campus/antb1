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
    case Predicate::Kind::kLike:  // integer folding never produces LIKE, IN or two columns
    case Predicate::Kind::kNotLike:
    case Predicate::Kind::kCompareColumns:
    case Predicate::Kind::kIn:
    case Predicate::Kind::kNotIn:
    case Predicate::Kind::kIsTrue:
      return false;
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
class FoldDecimalTest : public ::testing::TestWithParam<FoldCase> {};
class FoldHavingTest : public ::testing::TestWithParam<FoldCase> {};

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
        // DECIMAL(38,0): +-(10^38 - 1); a fraction is folded like into an integer column.
        FoldCase{"h = 99999999999999999999999999999999999999", kCompare, CompareOp::kEq,
                 RangeOf(LogicalType::Decimal(38, 0)).max},
        FoldCase{"h <= -99999999999999999999999999999999999999", kCompare, CompareOp::kLe,
                 RangeOf(LogicalType::Decimal(38, 0)).min},
        FoldCase{"h < 1.5", kCompare, CompareOp::kLe, 1}, FoldCase{"h = 1.5", kFalse}));

// A literal folds exactly into a DECIMAL column's scale, as its unscaled value (ADR 0021 rule 11):
// a fraction beyond the scale gives the nearest value on the kept side, and a literal beyond the
// precision makes the comparison a constant (divergence D14).
TEST_P(FoldDecimalTest, Folds) {
  const FoldCase& c = GetParam();
  const Catalog catalog = MakeCatalog();
  const std::string sql = "SELECT COUNT(*) FROM dec WHERE " + std::string(c.where);
  auto plan = BindSql(sql, catalog);
  ASSERT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
  // The Filter is below the Aggregate, and below a Compute for a computed operand.
  int depth = 1;
  while (!std::holds_alternative<FilterNode>(Nth(*plan, depth))) {
    ++depth;
  }
  const Predicate& p = std::get<FilterNode>(Nth(*plan, depth)).predicates.at(0);
  EXPECT_EQ(p.kind, c.kind);
  EXPECT_EQ(p.column.has_value(), c.kind != Predicate::Kind::kFalse);
  if (c.kind == Predicate::Kind::kCompare) {
    EXPECT_EQ(p.op, c.op);
    EXPECT_EQ(Int128ToString(std::get<Int128>(p.constant.value)), Int128ToString(c.value));
    ASSERT_TRUE(p.column.has_value());
    if (p.column.has_value()) {
      EXPECT_EQ(p.constant.type, p.column->type);
    }
  }
}

INSTANTIATE_TEST_SUITE_P(
    Binder, FoldDecimalTest,
    ::testing::Values(
        // DECIMAL(15,2): the unscaled value is the number times 100.
        FoldCase{"p = 1.5", kCompare, CompareOp::kEq, 150},
        FoldCase{"p = 1.50", kCompare, CompareOp::kEq, 150},
        FoldCase{"p < 2", kCompare, CompareOp::kLt, 200},
        FoldCase{"p > -0.07", kCompare, CompareOp::kGt, -7},
        FoldCase{"p <= 12.345", kCompare, CompareOp::kLe, 1234},
        FoldCase{"p < 12.345", kCompare, CompareOp::kLe, 1234},
        FoldCase{"p >= 12.345", kCompare, CompareOp::kGe, 1235},
        FoldCase{"p > 12.345", kCompare, CompareOp::kGe, 1235},
        FoldCase{"p > -12.345", kCompare, CompareOp::kGe, -1234}, FoldCase{"p = 12.345", kFalse},
        FoldCase{"p <> 12.345", kIsNotNull}, FoldCase{"p = 0.001", kFalse},
        FoldCase{"p > 0.001", kCompare, CompareOp::kGe, 1},
        FoldCase{"12.345 < p", kCompare, CompareOp::kGe, 1235},
        // The precision bounds the unscaled value: +-(10^15 - 1) for DECIMAL(15,2).
        FoldCase{"p <= 9999999999999.99", kCompare, CompareOp::kLe, 999999999999999},
        FoldCase{"p < 10000000000000", kIsNotNull}, FoldCase{"p > 10000000000000", kFalse},
        FoldCase{"p = 9999999999999.995", kFalse},
        FoldCase{"p < 9999999999999.995", kCompare, CompareOp::kLe, 999999999999999},
        FoldCase{"p > -10000000000000", kIsNotNull},
        // DECIMAL(9,4) and DECIMAL(38,10).
        FoldCase{"r = 0.0001", kCompare, CompareOp::kEq, 1},
        FoldCase{"r < 99999.99995", kCompare, CompareOp::kLe, 999999999},
        FoldCase{"r < 100000", kIsNotNull},
        FoldCase{"z = 1234567890123456789012345678.0123456789", kCompare, CompareOp::kEq,
                 (Int128{1234567890123456789} * Int128{1'000'000'000'000'000'000} * 10) +
                     Int128{123456780123456789}},
        FoldCase{"z > 10000000000000000000000000000", kFalse},
        // Computed operands fold into their own type: p * q is DECIMAL(18,4), -p DECIMAL(15,2).
        FoldCase{"p * q > 100", kCompare, CompareOp::kGt, 1000000},
        FoldCase{"p * q <= 0.00001", kCompare, CompareOp::kLe, 0},
        FoldCase{"-p < -1.5", kCompare, CompareOp::kLt, -150},
        FoldCase{"p + i >= 99999999999999.995", kFalse},
        // A HUGEINT or UHUGEINT literal folds as well, where DuckDB fails to cast it to its capped
        // common type DECIMAL(38,s) (divergence D13).
        FoldCase{"p < 100000000000000000000000000000000000000", kIsNotNull},
        FoldCase{"p = 170141183460469231731687303715884105728", kFalse},
        FoldCase{"z > -170141183460469231731687303715884105728", kIsNotNull},
        FoldCase{"p <> 99999999999999999999999999999999999999", kIsNotNull}));

// Numbers that DuckDB reads as DOUBLE (an exponent, or a decimal of more than 38 digits) are
// rounded to the nearest double first, then folded exactly (divergence D7).
INSTANTIATE_TEST_SUITE_P(
    ApproximateNumbers, FoldThroughBinderTest,
    ::testing::Values(
        FoldCase{"i16 = 1.0000000000000000000001e0", kCompare, CompareOp::kEq, 1},
        FoldCase{"i16 = 1.00000000000000000000000000000000000000001", kCompare, CompareOp::kEq, 1},
        FoldCase{"i16 < 1.00000000000000000000000000000000000000001", kCompare, CompareOp::kLt, 1},
        FoldCase{"i16 <= 0.99999999999999999999999999999999999999999", kCompare, CompareOp::kLe, 1},
        FoldCase{"i16 > -0.99999999999999999999999999999999999999999", kCompare, CompareOp::kGt,
                 -1},
        FoldCase{"i16 > 1.5e0", kCompare, CompareOp::kGe, 2}, FoldCase{"i16 = 1.5e0", kFalse},
        FoldCase{"u16 < 65535.9999999999999999999999999999999999999", kIsNotNull},  // < 65536
        FoldCase{"i64 = 1e3", kCompare, CompareOp::kEq, 1000},
        FoldCase{"i64 > 9007199254740993e0", kCompare, CompareOp::kGt, 9007199254740992},
        FoldCase{"i64 >= 9223372036854775808e0", kFalse}, FoldCase{"i64 < 1e400", kIsNotNull},
        FoldCase{"i64 > -1e400", kIsNotNull}, FoldCase{"i64 = 1e-400", kCompare, CompareOp::kEq, 0},
        // At most 38 digits: an exact DECIMAL, as in DuckDB.
        FoldCase{"i16 = 1.0000000000000000000000000000000000001", kFalse},
        FoldCase{"i16 < 1.0000000000000000000000000000000000001", kCompare, CompareOp::kLe, 1}));

// A date cast of a string literal is the DATE literal (days since 1970-01-01), in either spelling
// and on either side.
INSTANTIATE_TEST_SUITE_P(
    DateCasts, FoldThroughBinderTest,
    ::testing::Values(FoldCase{"dt = DATE '2013-07-01'", kCompare, CompareOp::kEq, 15887},
                      FoldCase{"dt = CAST('2013-07-01' AS DATE)", kCompare, CompareOp::kEq, 15887},
                      FoldCase{"dt = '2013-07-01'::DATE", kCompare, CompareOp::kEq, 15887},
                      FoldCase{"dt >= cast('1970-01-01' as Date)", kCompare, CompareOp::kGe, 0},
                      FoldCase{"dt < '1969-12-31' :: date", kCompare, CompareOp::kLt, -1},
                      FoldCase{"CAST('2024-01-31' AS DATE) < dt", kCompare, CompareOp::kGt, 19753},
                      FoldCase{"'2020-01-02'::DATE >= dt", kCompare, CompareOp::kLe, 18263},
                      FoldCase{"dt <> ('2020-01-02')::DATE", kCompare, CompareOp::kNe, 18263},
                      FoldCase{"(('2020-01-02'::DATE)) = dt", kCompare, CompareOp::kEq, 18263}));

// HAVING folds into the aggregate's type the same way: an integer SUM is HUGEINT, with the range
// +-(10^38 - 1), and MIN and MAX of a DECIMAL keep the column's (p,s).
TEST_P(FoldHavingTest, Folds) {
  const FoldCase& c = GetParam();
  const Catalog catalog = MakeCatalog();
  const std::string sql = "SELECT COUNT(*) FROM dec HAVING " + std::string(c.where);
  auto plan = BindSql(sql, catalog);
  ASSERT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
  const Predicate& p = std::get<FilterNode>(Nth(*plan, 1)).predicates.at(0);
  EXPECT_EQ(p.kind, c.kind);
  EXPECT_EQ(p.column.has_value(), c.kind != Predicate::Kind::kFalse);
  if (c.kind == Predicate::Kind::kCompare) {
    EXPECT_EQ(p.op, c.op);
    EXPECT_EQ(Int128ToString(std::get<Int128>(p.constant.value)), Int128ToString(c.value));
    ASSERT_TRUE(p.column.has_value());
    if (p.column.has_value()) {
      EXPECT_EQ(p.constant.type, p.column->type);
    }
  }
}

INSTANTIATE_TEST_SUITE_P(
    Binder, FoldHavingTest,
    ::testing::Values(
        // HUGEINT (an integer SUM): +-(10^38 - 1).
        FoldCase{"SUM(i) = 99999999999999999999999999999999999999", kCompare, CompareOp::kEq,
                 RangeOf(LogicalType::kHugeInt).max},
        FoldCase{"SUM(i) < 100000000000000000000000000000000000000", kIsNotNull},
        FoldCase{"SUM(i) > -100000000000000000000000000000000000000", kIsNotNull},
        // 1e38 is a DOUBLE (below 10^38): SUM(i) >= 99999999999999997748809823456034029568.
        FoldCase{"SUM(i) >= 1e38", kCompare, CompareOp::kGe, static_cast<Int128>(1e38)},
        FoldCase{"SUM(i) >= 1.0000000000000001e38", kFalse},
        // MIN and MAX of DECIMAL(15,2) and DECIMAL(38,10): the column's scale.
        FoldCase{"MAX(p) < 12.345", kCompare, CompareOp::kLe, 1234},
        FoldCase{"MIN(p) >= -12.345", kCompare, CompareOp::kGe, -1234},
        FoldCase{"MIN(p) <> 0.001", kIsNotNull}, FoldCase{"MAX(z) = 0.00000000001", kFalse},
        FoldCase{"MAX(z) > 0.00000000001", kCompare, CompareOp::kGe, 1},
        // SUM of a DECIMAL(p,s) is DECIMAL(38,s).
        FoldCase{"SUM(p) > 1.5", kCompare, CompareOp::kGt, 150},
        FoldCase{"SUM(p) <= 12.345", kCompare, CompareOp::kLe, 1234},
        FoldCase{"SUM(r) = 0.00005", kFalse},
        FoldCase{"SUM(p) < 1000000000000000000000000000000000000", kIsNotNull},
        FoldCase{"SUM(p) > 170141183460469231731687303715884105727", kFalse},
        FoldCase{"MIN(z) >= -100000000000000000000000000000000000000", kIsNotNull}));

}  // namespace
}  // namespace antb1::plan
