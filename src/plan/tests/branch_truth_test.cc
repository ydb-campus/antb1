#include "antb1/plan/branch_truth.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "antb1/common/int128.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/types.h"

namespace antb1::plan {
namespace {

BoundColumn Col(LogicalType type, ColumnId id = ColumnId{1}) {
  return BoundColumn{.index = 0, .id = id, .name = "c", .type = type};
}

Constant Number(Int128 value, LogicalType type = LogicalType::kInteger) {
  return Constant{.type = type, .value = value};
}

Constant Text(std::string value) {
  return Constant{.type = LogicalType::kVarchar, .value = std::move(value)};
}

// column <op> constant, the column of the constant's type unless given.
Predicate Compare(CompareOp op, const Constant& constant,
                  std::optional<LogicalType> column = std::nullopt) {
  return Predicate{.kind = Predicate::Kind::kCompare,
                   .column = Col(column.value_or(constant.type)),
                   .op = op,
                   .constant = constant};
}

Predicate CompareColumns(CompareOp op, LogicalType left, LogicalType right) {
  return Predicate{.kind = Predicate::Kind::kCompareColumns,
                   .column = Col(left),
                   .other = Col(right, ColumnId{2}),
                   .op = op};
}

Predicate InList(Predicate::Kind kind, LogicalType column, std::vector<Constant> values) {
  return Predicate{.kind = kind, .column = Col(column), .values = std::move(values)};
}

constexpr BranchTruth kNever = BranchTruth::kNever;
constexpr BranchTruth kAlways = BranchTruth::kAlways;
constexpr BranchTruth kUnknown = BranchTruth::kUnknown;

// Every operator on each side of a constant and on it: an INTEGER 5 against 4, 5 and 6.
TEST(BranchTruthTest, DecidesAComparisonByEveryOperator) {
  struct Case {
    CompareOp op;
    BranchTruth below;  // 5 <op> 6
    BranchTruth equal;  // 5 <op> 5
    BranchTruth above;  // 5 <op> 4
  };
  for (const Case& c :
       {Case{.op = CompareOp::kEq, .below = kNever, .equal = kAlways, .above = kNever},
        Case{.op = CompareOp::kNe, .below = kAlways, .equal = kNever, .above = kAlways},
        Case{.op = CompareOp::kLt, .below = kAlways, .equal = kNever, .above = kNever},
        Case{.op = CompareOp::kLe, .below = kAlways, .equal = kAlways, .above = kNever},
        Case{.op = CompareOp::kGt, .below = kNever, .equal = kNever, .above = kAlways},
        Case{.op = CompareOp::kGe, .below = kNever, .equal = kAlways, .above = kAlways}}) {
    SCOPED_TRACE(std::string(ToString(c.op)));
    const Constant five = Number(5);
    EXPECT_EQ(TruthOver(Compare(c.op, Number(6)), five, std::nullopt), c.below);
    EXPECT_EQ(TruthOver(Compare(c.op, Number(5)), five, std::nullopt), c.equal);
    EXPECT_EQ(TruthOver(Compare(c.op, Number(4)), five, std::nullopt), c.above);
  }
  // The extremes of the widest integer.
  const LogicalType hugeint = LogicalType::kHugeInt;
  EXPECT_EQ(TruthOver(Compare(CompareOp::kLt, Number(kInt128Max, hugeint)),
                      Number(kInt128Min, hugeint), std::nullopt),
            kAlways);
}

// Numbers compare by value whatever their types and scales, as exec compares a DECIMAL with an
// integer or another DECIMAL: 1.50 is 1.5, and the INTEGER 1 is the DECIMAL 1.00.
TEST(BranchTruthTest, ComparesNumbersExactlyAcrossScales) {
  const LogicalType d32 = LogicalType::Decimal(3, 2);
  const LogicalType d21 = LogicalType::Decimal(2, 1);
  const LogicalType d122 = LogicalType::Decimal(12, 2);
  EXPECT_EQ(TruthOver(CompareColumns(CompareOp::kEq, d32, d21), Number(150, d32), Number(15, d21)),
            kAlways);
  EXPECT_EQ(TruthOver(CompareColumns(CompareOp::kLt, d32, d21), Number(149, d32), Number(15, d21)),
            kAlways);
  EXPECT_EQ(TruthOver(CompareColumns(CompareOp::kEq, LogicalType::kInteger, d122), Number(1),
                      Number(100, d122)),
            kAlways);
  // Scaling the smaller scale up overflows: the magnitude decides.
  const LogicalType d380 = LogicalType::Decimal(38, 0);
  const LogicalType d3810 = LogicalType::Decimal(38, 10);
  const Int128 huge = PowerOfTen(37);
  EXPECT_EQ(
      TruthOver(CompareColumns(CompareOp::kGt, d380, d3810), Number(huge, d380), Number(1, d3810)),
      kAlways);
  // A comparison with a constant is decided at the column's own type only.
  EXPECT_EQ(
      TruthOver(Compare(CompareOp::kEq, Number(1, LogicalType::kBigInt), LogicalType::kInteger),
                Number(1), std::nullopt),
      kUnknown);
  EXPECT_EQ(TruthOver(Compare(CompareOp::kEq, Number(15, d21)), Number(150, d32), std::nullopt),
            kUnknown)
      << "a branch constant of another scale than the column";
}

// Text compares by unsigned bytes, as DuckDB and Arrow's binary kernels do: a byte above 0x7f is
// greater than every ASCII byte, a trailing space makes a string greater, the empty string is the
// least.
TEST(BranchTruthTest, ComparesTextByUnsignedBytes) {
  EXPECT_EQ(TruthOver(Compare(CompareOp::kGt, Text("z")), Text("\xD0\x96"), std::nullopt), kAlways)
      << "a Cyrillic letter above an ASCII one";
  EXPECT_EQ(TruthOver(Compare(CompareOp::kLt, Text("\x80")), Text("\x7f"), std::nullopt), kAlways);
  EXPECT_EQ(TruthOver(Compare(CompareOp::kGt, Text("XL")), Text("XL "), std::nullopt), kAlways);
  EXPECT_EQ(TruthOver(Compare(CompareOp::kLt, Text("a")), Text(""), std::nullopt), kAlways);
  EXPECT_EQ(TruthOver(Compare(CompareOp::kEq, Text("a")), Text("A"), std::nullopt), kNever);
  EXPECT_EQ(TruthOver(Compare(CompareOp::kEq, Text(std::string("a\0b", 3))),
                      Text(std::string("a\0b", 3)), std::nullopt),
            kAlways);
}

// DATE and TIMESTAMP compare by their integers, never with each other.
TEST(BranchTruthTest, ComparesDatesAndTimestampsByTheirIntegers) {
  const LogicalType date = LogicalType::kDate;
  const LogicalType timestamp = LogicalType::kTimestamp;
  EXPECT_EQ(
      TruthOver(Compare(CompareOp::kLt, Number(20'000, date)), Number(19'999, date), std::nullopt),
      kAlways);
  EXPECT_EQ(
      TruthOver(Compare(CompareOp::kEq, Number(7, timestamp)), Number(8, timestamp), std::nullopt),
      kNever);
  EXPECT_EQ(TruthOver(CompareColumns(CompareOp::kEq, date, timestamp), Number(0, date),
                      Number(0, timestamp)),
            kUnknown);
}

TEST(BranchTruthTest, DecidesInAndNotInByAnEqualValueOfTheColumnsType) {
  const auto in = [](Predicate::Kind kind, std::vector<Constant> values) {
    return InList(kind, LogicalType::kVarchar, std::move(values));
  };
  const Constant b = Text("b");
  EXPECT_EQ(TruthOver(in(Predicate::Kind::kIn, {Text("a"), Text("b")}), b, std::nullopt), kAlways);
  EXPECT_EQ(TruthOver(in(Predicate::Kind::kIn, {Text("a"), Text("c")}), b, std::nullopt), kNever);
  EXPECT_EQ(TruthOver(in(Predicate::Kind::kNotIn, {Text("a"), Text("b")}), b, std::nullopt),
            kNever);
  EXPECT_EQ(TruthOver(in(Predicate::Kind::kNotIn, {Text("a"), Text("c")}), b, std::nullopt),
            kAlways);
  EXPECT_EQ(TruthOver(in(Predicate::Kind::kIn, {Text("a"), Number(2)}), b, std::nullopt), kUnknown)
      << "a value of another type than the column";
  EXPECT_EQ(TruthOver(in(Predicate::Kind::kIn, {Number(2), Text("b")}), b, std::nullopt), kUnknown)
      << "exec rejects such a list, so an equal value decides nothing";
  EXPECT_EQ(TruthOver(in(Predicate::Kind::kNotIn, {Text("b"), Number(2)}), b, std::nullopt),
            kUnknown);
  EXPECT_EQ(TruthOver(in(Predicate::Kind::kIn, {}), b, std::nullopt), kUnknown);
}

TEST(BranchTruthTest, AConstantIsNeverNullAndFalseIsNeverTrue) {
  const Predicate not_null{.kind = Predicate::Kind::kIsNotNull, .column = Col(LogicalType::kDate)};
  EXPECT_EQ(TruthOver(not_null, Number(0, LogicalType::kDate), std::nullopt), kAlways);
  EXPECT_EQ(TruthOver(not_null, std::nullopt, std::nullopt), kUnknown);
  EXPECT_EQ(TruthOver(Predicate{.kind = Predicate::Kind::kFalse}, std::nullopt, std::nullopt),
            kNever);
}

// A NULL value (the NULL item of a branch, ADR 0037's NL3) makes no predicate that reads it true:
// a comparison, IN and NOT IN, LIKE and IS TRUE are NULL, IS NOT NULL is false.
TEST(BranchTruthTest, ANullValueMakesNoPredicateTrue) {
  const Constant null{.type = LogicalType::kInteger, .value = std::monostate{}};
  for (const CompareOp op : {CompareOp::kEq, CompareOp::kNe, CompareOp::kLt, CompareOp::kGe}) {
    EXPECT_EQ(TruthOver(Compare(op, Number(5)), null, std::nullopt), kNever);
  }
  EXPECT_EQ(TruthOver(CompareColumns(CompareOp::kEq, LogicalType::kInteger, LogicalType::kInteger),
                      Number(5), null),
            kNever);
  EXPECT_EQ(TruthOver(InList(Predicate::Kind::kIn, LogicalType::kInteger, {Number(5)}), null,
                      std::nullopt),
            kNever);
  EXPECT_EQ(TruthOver(InList(Predicate::Kind::kNotIn, LogicalType::kInteger, {Number(5)}), null,
                      std::nullopt),
            kNever);
  EXPECT_EQ(TruthOver(Predicate{.kind = Predicate::Kind::kIsNotNull,
                                .column = Col(LogicalType::kInteger)},
                      null, std::nullopt),
            kNever);
  const Constant null_text{.type = LogicalType::kVarchar, .value = std::monostate{}};
  EXPECT_EQ(TruthOver(Predicate{.kind = Predicate::Kind::kLike,
                                .column = Col(LogicalType::kVarchar),
                                .constant = Text("a%")},
                      null_text, std::nullopt),
            kNever);
  const Constant null_bool{.type = LogicalType::kBoolean, .value = std::monostate{}};
  EXPECT_EQ(
      TruthOver(Predicate{.kind = Predicate::Kind::kIsTrue, .column = Col(LogicalType::kBoolean)},
                null_bool, std::nullopt),
      kNever);
}

// What is not decided exactly here stays a condition: LIKE (its matcher is exec's), a computed
// condition, DOUBLE (NaN, -0.0, DuckDB's conversions) on either side, a constant missing or whose
// value is not of its type.
TEST(BranchTruthTest, LeavesWhatItCannotDecideUnknown) {
  const Predicate like{
      .kind = Predicate::Kind::kLike, .column = Col(LogicalType::kVarchar), .constant = Text("a%")};
  EXPECT_EQ(TruthOver(like, Text("abc"), std::nullopt), kUnknown);
  Predicate not_like = like;
  not_like.kind = Predicate::Kind::kNotLike;
  EXPECT_EQ(TruthOver(not_like, Text("abc"), std::nullopt), kUnknown);
  const Predicate is_true{.kind = Predicate::Kind::kIsTrue, .column = Col(LogicalType::kBoolean)};
  EXPECT_EQ(TruthOver(is_true, std::nullopt, std::nullopt), kUnknown);

  const Constant one_double{.type = LogicalType::kDouble, .value = 1.0};
  EXPECT_EQ(TruthOver(Compare(CompareOp::kEq, one_double), one_double, std::nullopt), kUnknown);
  const LogicalType d21 = LogicalType::Decimal(2, 1);
  EXPECT_EQ(TruthOver(Compare(CompareOp::kEq, one_double, d21), Number(10, d21), std::nullopt),
            kUnknown)
      << "a DOUBLE constant on a DECIMAL column";

  for (const Predicate& p :
       {Compare(CompareOp::kEq, Number(1)),
        CompareColumns(CompareOp::kEq, LogicalType::kInteger, LogicalType::kInteger),
        InList(Predicate::Kind::kIn, LogicalType::kInteger, {Number(1)})}) {
    EXPECT_EQ(TruthOver(p, std::nullopt, Number(1)), kUnknown) << "no constant for the column";
  }
  EXPECT_EQ(TruthOver(CompareColumns(CompareOp::kEq, LogicalType::kInteger, LogicalType::kInteger),
                      Number(1), std::nullopt),
            kUnknown)
      << "no constant for the other column";

  const Constant mistyped{.type = LogicalType::kVarchar, .value = Int128{1}};
  EXPECT_EQ(TruthOver(Compare(CompareOp::kEq, Text("1")), mistyped, std::nullopt), kUnknown)
      << "a value that is not of its type";
}

TEST(BranchTruthTest, NamesItsAnswers) {
  EXPECT_EQ(ToString(kNever), "never");
  EXPECT_EQ(ToString(kAlways), "always");
  EXPECT_EQ(ToString(kUnknown), "unknown");
}

}  // namespace
}  // namespace antb1::plan
