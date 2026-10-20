#include "antb1/exec/compute.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <arrow/compute/cast.h>
#include <gtest/gtest.h>

#include "antb1/common/int128.h"
#include "antb1/common/narrow.h"
#include "antb1/exec/filter.h"
#include "antb1/exec/memory_budget.h"
#include "antb1/plan/literal.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/types.h"

#include "../decimal.h"
#include "../decimal_bytes.h"
#include "../parallel_compute.h"
#include "exec_test_util.h"

namespace antb1::exec {
namespace {

using plan::ArithOp;
using plan::LogicalType;
using testing::Decimals;
using testing::Int16s;
using testing::Int64s;
using testing::NarrowDecimalsFor;
using testing::UnscaledText;

class ComputeTest : public testing::ExecTest {};

plan::ExprPtr ColumnAt(int index, LogicalType type) {
  return std::make_shared<const plan::Expr>(
      plan::Expr{.node = plan::ColumnExpr{.index = index}, .type = type, .name = "c"});
}

plan::ExprPtr ConstantOf(Int128 value, LogicalType type) {
  return std::make_shared<const plan::Expr>(
      plan::Expr{.node = plan::ConstantExpr{.value = plan::Constant{.type = type, .value = value}},
                 .type = type,
                 .name = Int128ToString(value)});
}

plan::ExprPtr Arith(ArithOp op, plan::ExprPtr left, plan::ExprPtr right, LogicalType type) {
  return std::make_shared<const plan::Expr>(plan::Expr{
      .node = plan::ArithExpr{.op = op, .left = std::move(left), .right = std::move(right)},
      .type = type,
      .name = "e"});
}

std::shared_ptr<arrow::RecordBatch> BatchOf(const arrow::ArrayVector& columns) {
  arrow::FieldVector fields;
  for (std::size_t i = 0; i < columns.size(); ++i) {
    fields.push_back(arrow::field("f" + std::to_string(i), columns[i]->type()));
  }
  return arrow::RecordBatch::Make(arrow::schema(fields), columns.front()->length(), columns);
}

arrow::Result<std::shared_ptr<arrow::Array>> Eval(const plan::ExprPtr& expr,
                                                  const arrow::ArrayVector& columns) {
  return EvaluateExpr(*expr, *BatchOf(columns), arrow::default_memory_pool());
}

std::shared_ptr<arrow::Array> Doubles(const std::vector<std::optional<double>>& values) {
  return testing::ArrayOf<arrow::DoubleBuilder>(arrow::float64(), values);
}

// + - * compute in the expression's type and fail on an overflow, like DuckDB; NULL stays NULL.
TEST_F(ComputeTest, IntegerArithmeticIsCheckedInItsType) {
  const auto values = Int16s({1, std::nullopt, 32766});
  const auto plus_one = Arith(ArithOp::kAdd, ColumnAt(0, LogicalType::kSmallInt),
                              ConstantOf(1, LogicalType::kInteger), LogicalType::kSmallInt);
  auto sum = Eval(plus_one, {values});
  ASSERT_TRUE(sum.ok()) << sum.status().ToString();
  EXPECT_EQ((*sum)->ToString(), "[\n  2,\n  null,\n  32767\n]");
  auto overflow = Eval(plus_one, {Int16s({32767})});
  ASSERT_FALSE(overflow.ok());
  EXPECT_TRUE(overflow.status().IsExecutionError()) << overflow.status().ToString();
  EXPECT_EQ(overflow.status().message(), "Overflow in addition of SMALLINT");
  // The same values in INTEGER do not overflow.
  auto wide = Eval(Arith(ArithOp::kAdd, ColumnAt(0, LogicalType::kSmallInt),
                         ConstantOf(1, LogicalType::kInteger), LogicalType::kInteger),
                   {Int16s({32767})});
  ASSERT_TRUE(wide.ok()) << wide.status().ToString();
  EXPECT_EQ((*wide)->ToString(), "[\n  32768\n]");
  auto product = Eval(Arith(ArithOp::kMultiply, ColumnAt(0, LogicalType::kBigInt),
                            ConstantOf(2, LogicalType::kInteger), LogicalType::kBigInt),
                      {Int64s({std::numeric_limits<int64_t>::max()})});
  EXPECT_EQ(product.status().message(), "Overflow in multiplication of BIGINT");
  const auto negate = std::make_shared<const plan::Expr>(
      plan::Expr{.node = plan::NegateExpr{.operand = ColumnAt(0, LogicalType::kSmallInt)},
                 .type = LogicalType::kSmallInt,
                 .name = "-c"});
  auto negated = Eval(negate, {Int16s({-5, std::nullopt})});
  ASSERT_TRUE(negated.ok());
  EXPECT_EQ((*negated)->ToString(), "[\n  5,\n  null\n]");
  EXPECT_EQ(Eval(negate, {Int16s({-32768})}).status().message(),
            "Overflow in negation of SMALLINT");
}

// // truncates and % takes the sign of the dividend; both are NULL for a zero divisor, and the
// minimum divided by -1 overflows (DuckDB 1.5.5).
TEST_F(ComputeTest, IntegerDivisionAndModulo) {
  const auto l = Int64s({7, -7, 7, -7, 5, std::nullopt});
  const auto r = Int64s({2, 2, -3, -3, 0, 1});
  const auto divide = Arith(ArithOp::kIntegerDivide, ColumnAt(0, LogicalType::kBigInt),
                            ColumnAt(1, LogicalType::kBigInt), LogicalType::kBigInt);
  const auto modulo = Arith(ArithOp::kModulo, ColumnAt(0, LogicalType::kBigInt),
                            ColumnAt(1, LogicalType::kBigInt), LogicalType::kBigInt);
  auto quotient = Eval(divide, {l, r});
  ASSERT_TRUE(quotient.ok()) << quotient.status().ToString();
  EXPECT_EQ((*quotient)->ToString(), "[\n  3,\n  -3,\n  -2,\n  2,\n  null,\n  null\n]");
  auto remainder = Eval(modulo, {l, r});
  ASSERT_TRUE(remainder.ok()) << remainder.status().ToString();
  EXPECT_EQ((*remainder)->ToString(), "[\n  1,\n  -1,\n  1,\n  -1,\n  null,\n  null\n]");
  const auto min = Int64s({std::numeric_limits<int64_t>::min()});
  const auto minus_one = Int64s({-1});
  EXPECT_TRUE(Eval(divide, {min, minus_one}).status().IsExecutionError());
  EXPECT_TRUE(Eval(modulo, {min, minus_one}).status().IsExecutionError());
  // USMALLINT: no sign to overflow.
  const auto u = testing::ArrayOf<arrow::UInt16Builder>(
      arrow::uint16(), std::vector<std::optional<uint16_t>>{65535});
  auto unsigned_div = Eval(Arith(ArithOp::kIntegerDivide, ColumnAt(0, LogicalType::kUSmallInt),
                                 ConstantOf(2, LogicalType::kInteger), LogicalType::kUSmallInt),
                           {u});
  ASSERT_TRUE(unsigned_div.ok()) << unsigned_div.status().ToString();
  EXPECT_EQ((*unsigned_div)->ToString(), "[\n  32767\n]");
}

// / is DOUBLE division (IEEE: x / 0 is +-inf, 0 / 0 NaN); on DOUBLE, // divides but is NULL for a
// zero divisor and % is fmod (NaN for a zero divisor).
TEST_F(ComputeTest, DoubleDivision) {
  const auto ints = Int64s({1, -1, 0, 7});
  const auto zeros = Int64s({0, 0, 0, 2});
  auto divided = Eval(Arith(ArithOp::kDivide, ColumnAt(0, LogicalType::kBigInt),
                            ColumnAt(1, LogicalType::kBigInt), LogicalType::kDouble),
                      {ints, zeros});
  ASSERT_TRUE(divided.ok()) << divided.status().ToString();
  const auto& d = static_cast<const arrow::DoubleArray&>(**divided);
  EXPECT_TRUE(std::isinf(d.Value(0)) && d.Value(0) > 0);
  EXPECT_TRUE(std::isinf(d.Value(1)) && d.Value(1) < 0);
  EXPECT_TRUE(std::isnan(d.Value(2)));
  EXPECT_EQ(d.Value(3), 3.5);
  const auto x = Doubles({1.5, 1.5, std::nullopt});
  const auto y = Doubles({0.0, 2.0, 2.0});
  auto quotient = Eval(Arith(ArithOp::kIntegerDivide, ColumnAt(0, LogicalType::kDouble),
                             ColumnAt(1, LogicalType::kDouble), LogicalType::kDouble),
                       {x, y});
  ASSERT_TRUE(quotient.ok());
  EXPECT_EQ((*quotient)->ToString(), "[\n  null,\n  0.75,\n  null\n]");
  auto remainder = Eval(Arith(ArithOp::kModulo, ColumnAt(0, LogicalType::kDouble),
                              ColumnAt(1, LogicalType::kDouble), LogicalType::kDouble),
                        {x, y});
  ASSERT_TRUE(remainder.ok());
  const auto& m = static_cast<const arrow::DoubleArray&>(**remainder);
  EXPECT_TRUE(std::isnan(m.Value(0)));
  EXPECT_EQ(m.Value(1), 1.5);
  EXPECT_TRUE(m.IsNull(2));
  // A BIGINT beyond 2^53 rounds to the nearest double on its way to DOUBLE.
  auto rounded = Eval(Arith(ArithOp::kDivide, ColumnAt(0, LogicalType::kBigInt),
                            ConstantOf(1, LogicalType::kInteger), LogicalType::kDouble),
                      {Int64s({std::numeric_limits<int64_t>::max()})});
  ASSERT_TRUE(rounded.ok()) << rounded.status().ToString();
  EXPECT_EQ(static_cast<const arrow::DoubleArray&>(**rounded).Value(0), 9223372036854775808.0);
}

// HUGEINT (SUM results) adds, subtracts and multiplies exactly, inside decimal128(38, 0).
TEST_F(ComputeTest, HugeIntArithmetic) {
  const auto sums = ConstantOf(plan::RangeOf(LogicalType::kHugeInt).max, LogicalType::kHugeInt);
  const auto one = ConstantOf(1, LogicalType::kHugeInt);
  const auto rows = Int64s({1, 2});
  auto at_max = Eval(Arith(ArithOp::kSubtract, sums, one, LogicalType::kHugeInt), {rows});
  ASSERT_TRUE(at_max.ok()) << at_max.status().ToString();
  EXPECT_EQ((*at_max)->length(), 2);
  auto over = Eval(Arith(ArithOp::kAdd, sums, one, LogicalType::kHugeInt), {rows});
  EXPECT_EQ(over.status().message(), "Overflow in addition of HUGEINT");
  auto product = Eval(Arith(ArithOp::kMultiply, ConstantOf(3, LogicalType::kHugeInt),
                            ColumnAt(0, LogicalType::kBigInt), LogicalType::kHugeInt),
                      {rows});
  ASSERT_TRUE(product.ok()) << product.status().ToString();
  EXPECT_EQ((*product)->ToString(), "[\n  3,\n  6\n]");
}

plan::ExprPtr Named(const plan::ExprPtr& expr, std::string name) {
  plan::Expr copy = *expr;
  copy.name = std::move(name);
  return std::make_shared<const plan::Expr>(std::move(copy));
}

// A constant operand reaches Arrow's kernel as the one scalar value it is rather than as an array
// of copies of itself (ADR 0026's C11), so it has to answer and fail exactly as a *column* of that
// value does. Each case below is computed both ways and compared, which is the only way to see a
// difference the scalar path could introduce: the result of the two is otherwise identical.
TEST_F(ComputeTest, AConstantOperandAnswersAsAColumnOfItWould) {
  struct Case {
    std::string what;
    plan::ExprPtr constant;                // the operand as a constant
    std::shared_ptr<arrow::Array> column;  // the same value as a column, row for row
    std::shared_ptr<arrow::Array> values;  // the other operand
    LogicalType operand;                   // the other operand's type
    LogicalType result;
    ArithOp op = ArithOp::kAdd;
  };
  const LogicalType d2_1 = LogicalType::Decimal(2, 1);
  const std::vector<Case> cases = {
      // An INTEGER constant narrowed into a SMALLINT result, and a row it overflows.
      {.what = "narrowed, fits",
       .constant = ConstantOf(1, LogicalType::kInteger),
       .column = testing::Int32s({1, 1, 1}),
       .values = Int16s({1, std::nullopt, 32766}),
       .operand = LogicalType::kSmallInt,
       .result = LogicalType::kSmallInt},
      {.what = "narrowed, overflows the result",
       .constant = ConstantOf(1, LogicalType::kInteger),
       .column = testing::Int32s({1}),
       .values = Int16s({32767}),
       .operand = LogicalType::kSmallInt,
       .result = LogicalType::kSmallInt},
      // A constant that does not fit the result type at all: the cast fails, both ways.
      {.what = "does not fit the result type",
       .constant = ConstantOf(100000, LogicalType::kInteger),
       .column = testing::Int32s({100000}),
       .values = Int16s({1}),
       .operand = LogicalType::kSmallInt,
       .result = LogicalType::kSmallInt},
      // The same over no rows, which is the one place the two could part: there is no value to
      // convert, so neither raises. Taking the constant as a scalar would have raised on the
      // constant alone, which is why a conversion it fails goes back to the array path.
      {.what = "does not fit the result type, over no rows",
       .constant = ConstantOf(100000, LogicalType::kInteger),
       .column = testing::Int32s({}),
       .values = Int16s({}),
       .operand = LogicalType::kSmallInt,
       .result = LogicalType::kSmallInt},
      // A DECIMAL constant beside a DOUBLE column, which is how a decimal literal meets one
      // (`f + 1.5` is DOUBLE + DECIMAL(2,1)): the conversion is DuckDB's, not Arrow's cast.
      {.what = "a decimal literal into DOUBLE",
       .constant = ConstantOf(15, d2_1),
       .column = Decimals(d2_1, {"15", "15", "15"}),
       .values = Doubles({1.5, -0.25, std::nullopt}),
       .operand = LogicalType::kDouble,
       .result = LogicalType::kDouble},
      // A decimal past the exactly representable range, where DuckDB's conversion splits the value
      // and adds the parts rather than dividing the whole (DuckDbDecimalToReal). Taking the
      // constant through CastTo rather than Arrow's own cast is what keeps it converting as a
      // column of it would; the two agree on every value tried here, including this one, so this
      // case asserts the invariant rather than proving the routing necessary.
      {.what = "a decimal past the exactly representable range",
       .constant = ConstantOf((Int128{9007199254740992} * 10000000000) + 9999999999,
                              LogicalType::Decimal(38, 10)),
       .column = Decimals(LogicalType::Decimal(38, 10), {"90071992547409929999999999"}),
       .values = Doubles({1.0}),
       .operand = LogicalType::kDouble,
       .result = LogicalType::kDouble},
      {.what = "a BIGINT multiply that overflows",
       .constant = ConstantOf(2, LogicalType::kInteger),
       .column = testing::Int32s({2}),
       .values = Int64s({std::numeric_limits<int64_t>::max()}),
       .operand = LogicalType::kBigInt,
       .result = LogicalType::kBigInt,
       .op = ArithOp::kMultiply},
  };
  for (const Case& c : cases) {
    SCOPED_TRACE(c.what);
    auto with_constant =
        Eval(Arith(c.op, ColumnAt(0, c.operand), c.constant, c.result), {c.values});
    auto with_column =
        Eval(Arith(c.op, ColumnAt(0, c.operand), ColumnAt(1, c.constant->type), c.result),
             {c.values, c.column});
    EXPECT_EQ(with_constant.ok(), with_column.ok());
    if (with_constant.ok() && with_column.ok()) {
      EXPECT_EQ((*with_constant)->ToString(), (*with_column)->ToString());
    } else {
      EXPECT_EQ(with_constant.status().message(), with_column.status().message());
    }
  }
}

// DECIMAL + and - rescale both operands to the result's scale, * multiplies the unscaled values;
// only a width capped to 18 or 38 can overflow, with DuckDB 1.5.5's messages (ADR 0021 rule 7).
TEST_F(ComputeTest, DecimalArithmetic) {
  const LogicalType p = LogicalType::Decimal(15, 2);
  const LogicalType r = LogicalType::Decimal(9, 4);
  const auto price = Decimals(p, {"1700", "-25", std::nullopt, "999999999999999"});
  const auto rate = Decimals(r, {"5", "-10000", "1", "999999999"});
  // p + r is DECIMAL(18,4): 17.00 + 0.0005, -0.25 - 1.0000, NULL, 9999999999999.99 + 99999.9999.
  auto sum = Eval(Arith(ArithOp::kAdd, ColumnAt(0, p), ColumnAt(1, r), LogicalType::Decimal(18, 4)),
                  {price, rate});
  ASSERT_TRUE(sum.ok()) << sum.status().ToString();
  EXPECT_EQ(UnscaledText(**sum), "DECIMAL(18,4): 170005 -12500 null 100000000999999899");
  auto difference =
      Eval(Arith(ArithOp::kSubtract, ColumnAt(1, r), ColumnAt(0, p), LogicalType::Decimal(18, 4)),
           {price, rate});
  ASSERT_TRUE(difference.ok()) << difference.status().ToString();
  EXPECT_EQ(UnscaledText(**difference), "DECIMAL(18,4): -169995 -7500 null -99999998999999901");
  // p * r is DECIMAL(18,6), capped from 24: the last row needs 24 digits.
  const auto product =
      Arith(ArithOp::kMultiply, ColumnAt(0, p), ColumnAt(1, r), LogicalType::Decimal(18, 6));
  auto first = Eval(product, {price->Slice(0, 3), rate->Slice(0, 3)});
  ASSERT_TRUE(first.ok()) << first.status().ToString();
  EXPECT_EQ(UnscaledText(**first), "DECIMAL(18,6): 8500 250000 null");
  EXPECT_EQ(Eval(product, {price, rate}).status().message(),
            "Overflow in multiplication of DECIMAL(18) (999999999999999 * 999999999). You might "
            "want to add an explicit cast to a bigger decimal.");
  // A sliced operand past its first row: the values are read in place, from the array's own buffer
  // (ADR 0026's C9), so the offset has to reach the arithmetic -- the same rows answer the same
  // whether they are a whole array or the tail of one. The add rescales its left operand and reads
  // its right one as it is; the multiply reads both as they are.
  const auto tail_sum =
      Arith(ArithOp::kAdd, ColumnAt(0, p), ColumnAt(1, r), LogicalType::Decimal(18, 4));
  auto sliced = Eval(tail_sum, {price->Slice(1, 2), rate->Slice(1, 2)});
  ASSERT_TRUE(sliced.ok()) << sliced.status().ToString();
  EXPECT_EQ(UnscaledText(**sliced), "DECIMAL(18,4): -12500 null");
  auto sliced_product = Eval(product, {price->Slice(1, 2), rate->Slice(1, 2)});
  ASSERT_TRUE(sliced_product.ok()) << sliced_product.status().ToString();
  EXPECT_EQ(UnscaledText(**sliced_product), "DECIMAL(18,6): 250000 null");
  // A chain of operations runs in one pass over the rows (ADR 0026's C10), so the rows a NULL makes
  // NULL and the offset of a sliced operand both have to reach it: `(p + r) * r` over the tail of
  // three arrays, one of which is NULL in the middle, answers as the whole arrays do at those rows.
  const auto chain =
      Arith(ArithOp::kMultiply,
            Arith(ArithOp::kAdd, ColumnAt(0, p), ColumnAt(1, r), LogicalType::Decimal(18, 4)),
            ColumnAt(1, r), LogicalType::Decimal(38, 8));
  auto whole = Eval(chain, {price, rate});
  ASSERT_TRUE(whole.ok()) << whole.status().ToString();
  EXPECT_EQ(UnscaledText(**whole),
            "DECIMAL(38,8): 850025 125000000 null 100000000899999898000000101");
  auto tail = Eval(chain, {price->Slice(1, 3), rate->Slice(1, 3)});
  ASSERT_TRUE(tail.ok()) << tail.status().ToString();
  EXPECT_EQ(UnscaledText(**tail), "DECIMAL(38,8): 125000000 null 100000000899999898000000101");
  // The same at an offset that is a whole number of bytes, which is where taking one nullable
  // column's own bitmap for the rows would read bits of the wrong rows: twelve rows, NULL at row 9,
  // sliced from row 8.
  std::vector<std::optional<std::string>> many;
  std::vector<std::optional<std::string>> ones;
  for (int i = 0; i < 12; ++i) {
    many.emplace_back(i == 9 ? std::nullopt : std::optional(std::to_string(i * 100)));
    ones.emplace_back("1");
  }
  const auto long_p = Decimals(p, many);
  const auto long_r = Decimals(r, ones);
  auto aligned = Eval(chain, {long_p->Slice(8, 4), long_r->Slice(8, 4)});
  ASSERT_TRUE(aligned.ok()) << aligned.status().ToString();
  EXPECT_EQ(UnscaledText(**aligned), "DECIMAL(38,8): 80001 null 100001 110001");
  // An integer operand counts as DECIMAL(k,0): p + 7 is DECIMAL(16,2).
  auto plus_int = Eval(Arith(ArithOp::kAdd, ColumnAt(0, p), ConstantOf(7, LogicalType::kInteger),
                             LogicalType::Decimal(16, 2)),
                       {price});
  ASSERT_TRUE(plus_int.ok()) << plus_int.status().ToString();
  EXPECT_EQ(UnscaledText(**plus_int), "DECIMAL(16,2): 2400 675 null 1000000000000699");
  // Negation keeps the type.
  const auto negate = std::make_shared<const plan::Expr>(
      plan::Expr{.node = plan::NegateExpr{.operand = ColumnAt(0, p)}, .type = p, .name = "-c"});
  auto negated = Eval(negate, {price});
  ASSERT_TRUE(negated.ok()) << negated.status().ToString();
  EXPECT_EQ(UnscaledText(**negated), "DECIMAL(15,2): -1700 25 null -999999999999999");
  // A file may hold a value beyond the declared width: the 128-bit minimum fails, it is not UB.
  const LogicalType d38 = LogicalType::Decimal(38, 0);
  const auto minimum = Decimals(d38, {"-170141183460469231731687303715884105728"});
  const auto negate38 = std::make_shared<const plan::Expr>(
      plan::Expr{.node = plan::NegateExpr{.operand = ColumnAt(0, d38)}, .type = d38, .name = "-c"});
  EXPECT_EQ(Eval(negate38, {minimum}).status().message(), "Overflow in negation of DECIMAL(38,0)");
}

// Overflows at both caps and failed rescales, with DuckDB 1.5.5's messages: + and - check
// |v| <= 10^p - 1 (not the 64-bit range), * also beyond 128 bits; a rescaled operand that does not
// fit fails first, naming the column it comes from.
TEST_F(ComputeTest, DecimalArithmeticErrors) {
  const LogicalType d18 = LogicalType::Decimal(18, 0);
  const auto big = Decimals(d18, {"600000000000000000"});
  EXPECT_EQ(
      Eval(Arith(ArithOp::kAdd, ColumnAt(0, d18), ColumnAt(0, d18), d18), {big}).status().message(),
      "Overflow in addition of DECIMAL(18) (600000000000000000 + 600000000000000000). You "
      "might want to add an explicit cast to a bigger decimal.");
  const auto negative = Decimals(d18, {"-600000000000000000"});
  EXPECT_EQ(
      Eval(Arith(ArithOp::kSubtract, ColumnAt(0, d18), ColumnAt(1, d18), d18), {big, negative})
          .status()
          .message(),
      "Overflow in subtract of DECIMAL(18) (600000000000000000 - -600000000000000000). You "
      "might want to add an explicit cast to a bigger decimal.");
  const LogicalType d38 = LogicalType::Decimal(38, 0);
  const auto huge = Decimals(d38, {"60000000000000000000000000000000000000"});
  EXPECT_EQ(Eval(Arith(ArithOp::kAdd, ColumnAt(0, d38), ColumnAt(0, d38), d38), {huge})
                .status()
                .message(),
            "Overflow in addition of DECIMAL(38) (60000000000000000000000000000000000000 + "
            "60000000000000000000000000000000000000);");
  const auto twenty = Decimals(d38, {"-100000000000000000000"});
  EXPECT_EQ(Eval(Arith(ArithOp::kMultiply, ColumnAt(0, d38), ColumnAt(0, d38), d38), {twenty})
                .status()
                .message(),
            "Overflow in multiplication of DECIMAL(38) (-100000000000000000000 * "
            "-100000000000000000000). You might want to add an explicit cast to a decimal with a "
            "smaller scale.");
  // Beyond 128 bits, still DuckDB's overflow.
  const auto max38 = Decimals(d38, {std::string(38, '9')});
  EXPECT_TRUE(Eval(Arith(ArithOp::kMultiply, ColumnAt(0, d38), ColumnAt(0, d38), d38), {max38})
                  .status()
                  .message()
                  .starts_with("Overflow in multiplication of DECIMAL(38)"));
  // d38_0 + d38_10 is DECIMAL(38,10): the DECIMAL(38,0) operand does not fit after rescaling.
  const LogicalType d38_10 = LogicalType::Decimal(38, 10);
  const auto small = Decimals(d38_10, {"1"});
  const auto result = LogicalType::Decimal(38, 10);
  EXPECT_EQ(
      Eval(Arith(ArithOp::kAdd, Named(ColumnAt(0, d38), "d38_0"), ColumnAt(1, d38_10), result),
           {max38, small})
          .status()
          .message(),
      "Casting value \"99999999999999999999999999999999999999\" to type DECIMAL(38,10) "
      "failed: value is out of range! when casting from source column d38_0");
  // Not a column: no source column; a scaled value prints with its scale.
  const LogicalType d18_1 = LogicalType::Decimal(18, 1);
  const auto scaled = Decimals(d18_1, {"123456789012345675"});
  const auto cents = Decimals(LogicalType::Decimal(2, 2), {"1"});
  EXPECT_EQ(Eval(Arith(ArithOp::kAdd,
                       Arith(ArithOp::kMultiply, ColumnAt(0, d18_1),
                             ConstantOf(1, LogicalType::kSmallInt), LogicalType::Decimal(18, 1)),
                       ColumnAt(1, LogicalType::Decimal(2, 2)), LogicalType::Decimal(18, 2)),
                 {scaled, cents})
                .status()
                .message(),
            "Casting value \"12345678901234567.5\" to type DECIMAL(18,2) failed: value is out of "
            "range!");
  // A file may hold a value beyond the column's declared width, and at equal scales there is no
  // rescale to catch it (ADR 0026's C9), so the arithmetic checks the operands as it reads them.
  // Both of these are out of DECIMAL(19,0) by one, and their sum is 0: without the check the query
  // would answer 0 where DuckDB raises its conversion error.
  // 10^18 rather than 10^19, and the result DECIMAL(18,0) rather than (19,0): 10^18 is one past
  // DECIMAL(18,0)'s declared width and still INSIDE an int64, so a narrow column holds it and the
  // premise -- a file carrying a value beyond its declared width -- is a real input at both
  // physical widths. 10^19 was expressible only while a p<=18 column was 128 bits wide, and it
  // would have had to move to a 19-digit type, which is a different claim (ADR 0021's decimal64
  // item). This way the narrow step's own range test runs over it as well.
  const LogicalType d18_0 = LogicalType::Decimal(18, 0);
  const auto over = Decimals(d18_0, {"1000000000000000000"});
  const auto under = Decimals(d18_0, {"-1000000000000000000"});
  EXPECT_EQ(Eval(Arith(ArithOp::kAdd, Named(ColumnAt(0, d18_0), "wide"), ColumnAt(1, d18_0), d18_0),
                 {over, under})
                .status()
                .message(),
            "Casting value \"1000000000000000000\" to type DECIMAL(18,0) failed: value is out of "
            "range! when casting from source column wide");
  // The right operand is reported when the left one fits.
  EXPECT_EQ(Eval(Arith(ArithOp::kAdd, ColumnAt(1, d18_0), Named(ColumnAt(0, d18_0), "wide"), d18_0),
                 {over, Decimals(d18_0, {"1"})})
                .status()
                .message(),
            "Casting value \"1000000000000000000\" to type DECIMAL(18,0) failed: value is out of "
            "range! when casting from source column wide");
  // An integer operand that does not fit: DuckDB's integer cast message.
  const auto ints = testing::ArrayOf<arrow::Int32Builder>(
      arrow::int32(), std::vector<std::optional<int32_t>>{std::numeric_limits<int32_t>::min()});
  const LogicalType d18_10 = LogicalType::Decimal(18, 10);
  EXPECT_EQ(Eval(Arith(ArithOp::kAdd, Named(ColumnAt(0, LogicalType::kInteger), "i32"),
                       ColumnAt(1, d18_10), d18_10),
                 {ints, Decimals(d18_10, {"1"})})
                .status()
                .message(),
            "Could not cast value -2147483648 to DECIMAL(18,10) when casting from source column "
            "i32");
}

// The left operand of + and - is computed and rescaled before the right one is computed, as in
// DuckDB 1.5.5: here its failed rescale is reported, not the right operand's overflow.
TEST_F(ComputeTest, DecimalOperandsInDuckDbOrder) {
  const LogicalType e = LogicalType::Decimal(18, 4);
  Int128 big = 1;
  for (int i = 0; i < 32; ++i) {
    big *= 10;
  }
  const auto left = Arith(ArithOp::kMultiply, ColumnAt(0, LogicalType::kBigInt),
                          ConstantOf(big, LogicalType::kHugeInt), LogicalType::kHugeInt);
  const auto right =
      Arith(ArithOp::kMultiply, ColumnAt(1, e), ColumnAt(1, e), LogicalType::Decimal(18, 8));
  const auto sum = Arith(ArithOp::kAdd, left, right, LogicalType::Decimal(38, 8));
  EXPECT_EQ(Eval(sum, {Int64s({1}), Decimals(e, {"1200000000000000"})}).status().message(),
            "Could not cast value 100000000000000000000000000000000 to DECIMAL(38,8)");
  // The right operand alone overflows.
  EXPECT_TRUE(Eval(right, {Int64s({1}), Decimals(e, {"1200000000000000"})})
                  .status()
                  .message()
                  .starts_with("Overflow in multiplication of DECIMAL(18)"));
}

// / and // of a DECIMAL are DOUBLE (ADR 0021 rule 8): the DECIMAL converts as DuckDB converts it
// (beyond 2^53 div + mod / 10^s, not the nearest double), / follows IEEE and // is NULL for a zero
// divisor. % computes in the common DECIMAL (rule 9): the operands rescaled to its scale, the
// dividend's sign, NULL for a zero divisor.
TEST_F(ComputeTest, DecimalDivisionAndModulo) {
  const LogicalType p = LogicalType::Decimal(15, 2);
  const auto price = Decimals(p, {"1700", "-25", std::nullopt, "0", "-725"});
  const auto divisor = Int64s({3, 0, 1, 0, 2});
  auto divided = Eval(Arith(ArithOp::kDivide, ColumnAt(0, p), ColumnAt(1, LogicalType::kBigInt),
                            LogicalType::kDouble),
                      {price, divisor});
  ASSERT_TRUE(divided.ok()) << divided.status().ToString();
  const auto& d = static_cast<const arrow::DoubleArray&>(**divided);
  EXPECT_EQ(d.Value(0), 17.0 / 3);
  EXPECT_TRUE(std::isinf(d.Value(1)) && d.Value(1) < 0);
  EXPECT_TRUE(d.IsNull(2));
  EXPECT_TRUE(std::isnan(d.Value(3)));
  EXPECT_EQ(d.Value(4), -3.625);
  auto quotient = Eval(Arith(ArithOp::kIntegerDivide, ColumnAt(0, p),
                             ColumnAt(1, LogicalType::kBigInt), LogicalType::kDouble),
                       {price, divisor});
  ASSERT_TRUE(quotient.ok()) << quotient.status().ToString();
  EXPECT_EQ((*quotient)->ToString(),
            "[\n  5.666666666666667,\n  null,\n  null,\n  null,\n  -3.625\n]");
  // DuckDB's conversion, not the nearest double (1.99489722791017e+16).
  const LogicalType wide = LogicalType::Decimal(18, 1);
  auto converted = Eval(Arith(ArithOp::kDivide, ColumnAt(0, wide),
                              ConstantOf(1, LogicalType::kInteger), LogicalType::kDouble),
                        {Decimals(wide, {"199489722791016982"})});
  ASSERT_TRUE(converted.ok()) << converted.status().ToString();
  EXPECT_EQ(static_cast<const arrow::DoubleArray&>(**converted).Value(0), 1.9948972279101696e+16);
  // A HUGEINT (an integer SUM) converts through DuckDB's 128-bit formula too: 2^64 + 2^63 + 2049
  // is 2^64 + 2^63, where the nearest double is 2^64 + 2^63 + 4096 (ADR 0021 rule 13's value).
  auto huge = Eval(Arith(ArithOp::kDivide, ColumnAt(0, LogicalType::kHugeInt),
                         ConstantOf(1, LogicalType::kInteger), LogicalType::kDouble),
                   {Decimals(LogicalType::kHugeInt, {"27670116110564329473"})});
  ASSERT_TRUE(huge.ok()) << huge.status().ToString();
  EXPECT_EQ(static_cast<const arrow::DoubleArray&>(**huge).Value(0), 0x1.8p64);
  // p % 2 is DECIMAL(15,2): 17.00 % 2, -0.25 % 0 (NULL), NULL, 0.00 % 0 (NULL), -7.25 % 2.
  auto remainder = Eval(Arith(ArithOp::kModulo, ColumnAt(0, p), ColumnAt(1, LogicalType::kBigInt),
                              LogicalType::Decimal(21, 2)),
                        {price, divisor});
  ASSERT_TRUE(remainder.ok()) << remainder.status().ToString();
  EXPECT_EQ(UnscaledText(**remainder), "DECIMAL(21,2): 200 null null null -125");
  // Scales differ: 17.00 % 0.0005 and -0.25 % -0.0003 in DECIMAL(17,4) (p % r).
  const LogicalType r = LogicalType::Decimal(9, 4);
  auto mixed =
      Eval(Arith(ArithOp::kModulo, ColumnAt(0, p), ColumnAt(1, r), LogicalType::Decimal(17, 4)),
           {Decimals(p, {"1700", "-25"}), Decimals(r, {"5", "-3"})});
  ASSERT_TRUE(mixed.ok()) << mixed.status().ToString();
  EXPECT_EQ(UnscaledText(**mixed), "DECIMAL(17,4): 0 -1");
}

// A Compute appends its columns to the selected rows only: a row a filter dropped is never
// computed, so it cannot overflow.
TEST_F(ComputeTest, ComputesOnlyTheSelectedRows) {
  const auto values = Int16s({1, 32767, 3});
  auto schema = arrow::schema({arrow::field("v", arrow::int16())});
  const Batch batch{.data = arrow::RecordBatch::Make(schema, 3, {values}),
                    .selection = testing::Bools({true, false, true})};
  ComputeOperator op(std::make_unique<testing::ScriptedSource>(schema, std::vector<Batch>{batch}),
                     {Arith(ArithOp::kAdd, ColumnAt(0, LogicalType::kSmallInt),
                            ConstantOf(1, LogicalType::kInteger), LogicalType::kSmallInt)});
  EXPECT_EQ(op.output_schema()->num_fields(), 2);
  EXPECT_TRUE(op.output_schema()->field(1)->type()->Equals(*arrow::int16()));
  ExecContext ctx;
  auto table = Drain(op, ctx);
  ASSERT_TRUE(table.ok()) << table.status().ToString();
  ASSERT_EQ((*table)->num_rows(), 2);
  EXPECT_EQ((*table)->column(1)->chunk(0)->ToString(), "[\n  2,\n  4\n]");
}

// Two columns compare in their common type: a BIGINT with a DOUBLE in DOUBLE (a value beyond 2^53
// rounds, as in DuckDB), a SMALLINT with a USMALLINT in INTEGER; NULL rejects the row.
TEST_F(ComputeTest, FilterComparesTwoColumns) {
  const auto ints = Int64s({std::numeric_limits<int64_t>::min(), 1, 3, std::nullopt});
  const auto doubles = Doubles({0.0, 1.0, 2.5, 1.0});
  auto schema =
      arrow::schema({arrow::field("i", arrow::int64()), arrow::field("d", arrow::float64())});
  const Batch batch{.data = arrow::RecordBatch::Make(schema, 4, {ints, doubles}), .selection = {}};
  const auto compare = [&](plan::CompareOp op) {
    return plan::Predicate{.kind = plan::Predicate::Kind::kCompareColumns,
                           .column = testing::Column(0, "i", LogicalType::kBigInt),
                           .other = testing::Column(1, "d", LogicalType::kDouble),
                           .op = op,
                           .constant = {},
                           .values = {},
                           .span = {}};
  };
  FilterOperator less(std::make_unique<testing::ScriptedSource>(schema, std::vector<Batch>{batch}),
                      {compare(plan::CompareOp::kLt)});
  ExecContext ctx;
  auto rows = Drain(less, ctx);
  ASSERT_TRUE(rows.ok()) << rows.status().ToString();
  EXPECT_EQ(testing::Int64Column(**rows),
            (std::vector<std::optional<int64_t>>{std::numeric_limits<int64_t>::min()}));
  FilterOperator equal(std::make_unique<testing::ScriptedSource>(schema, std::vector<Batch>{batch}),
                       {compare(plan::CompareOp::kEq)});
  auto same = Drain(equal, ctx);
  ASSERT_TRUE(same.ok()) << same.status().ToString();
  EXPECT_EQ(testing::Int64Column(**same), (std::vector<std::optional<int64_t>>{1}));
  auto bad = compare(plan::CompareOp::kEq);
  bad.other = testing::Column(5, "nope", LogicalType::kDouble);
  FilterOperator outside(
      std::make_unique<testing::ScriptedSource>(schema, std::vector<Batch>{batch}), {bad});
  EXPECT_TRUE(outside.Open(ctx).IsInvalid());
}

plan::ExprPtr StringConstant(std::string value) {
  return std::make_shared<const plan::Expr>(
      plan::Expr{.node = plan::ConstantExpr{.value = plan::Constant{.type = LogicalType::kVarchar,
                                                                    .value = std::move(value)}},
                 .type = LogicalType::kVarchar,
                 .name = "'...'"});
}

plan::ExprPtr Call(plan::Function function, std::vector<plan::ExprPtr> args, LogicalType type) {
  return std::make_shared<const plan::Expr>(
      plan::Expr{.node = plan::FunctionExpr{.function = function, .args = std::move(args)},
                 .type = type,
                 .name = "f"});
}

// regexp_replace keeps running out of memory a memory error (not the execution error of an
// invalid pattern), so that the query fails with the `memory` kind.
TEST_F(ComputeTest, RegexpReplaceOutOfMemoryIsAMemoryError) {
  std::vector<std::optional<std::string>> long_values(64, std::string(std::size_t{64} * 1024, 'a'));
  const auto text = testing::Strings(long_values);
  const auto call = [](std::string pattern) {
    return Call(plan::Function::kRegexpReplace,
                {ColumnAt(0, LogicalType::kVarchar), StringConstant(std::move(pattern)),
                 StringConstant("b")},
                LogicalType::kVarchar);
  };
  MemoryBudget budget(int64_t{256} * 1024);  // far below the 4 MiB result
  const auto oom = EvaluateExpr(*call("a"), *BatchOf({text}), &budget);
  EXPECT_TRUE(oom.status().IsOutOfMemory()) << oom.status().ToString();
  const auto invalid = EvaluateExpr(*call("("), *BatchOf({text}), &budget);
  EXPECT_TRUE(invalid.status().IsExecutionError()) << invalid.status().ToString();
  EXPECT_EQ(budget.bytes_allocated(), 0);
}

// substring slices codepoints, and its start and length are columns like any other, so the kernel
// is the only place where a per-row window is exercised: NULL in any of the three arguments, a
// window clipped at either end, a negative start and a negative length, an empty value, a value
// whose first byte is a UTF-8 continuation byte (an unannotated Parquet BYTE_ARRAY is VARCHAR and
// may hold any bytes), and the two out-of-range errors, which no SQL-level test can reach for a
// NULL-valued row.
TEST_F(ComputeTest, SubstringSlicesCodepointsPerRow) {
  // "h\xC3\xA9llo" is 5 codepoints of 6 bytes; the fifth value starts with a continuation byte.
  const std::string continuation_first{'\x80', 'A'};  // no codepoint precedes the 0x80
  const auto text =
      testing::Strings({"h\xC3\xA9llo", std::nullopt, "ab", "", continuation_first, "ab"});
  const auto call = [&](const std::shared_ptr<arrow::Array>& start,
                        const std::shared_ptr<arrow::Array>& length) {
    std::vector<plan::ExprPtr> args{ColumnAt(0, LogicalType::kVarchar),
                                    ColumnAt(1, LogicalType::kBigInt)};
    if (length != nullptr) {
      args.push_back(ColumnAt(2, LogicalType::kBigInt));
    }
    arrow::ArrayVector columns{text, start};
    if (length != nullptr) {
      columns.push_back(length);
    }
    return Eval(Call(plan::Function::kSubstring, std::move(args), LogicalType::kVarchar), columns);
  };
  const auto values = [](const std::shared_ptr<arrow::Array>& array) {
    std::vector<std::optional<std::string>> out;
    const auto& binary = static_cast<const arrow::BinaryArray&>(*array);
    out.reserve(Narrow<std::size_t>(binary.length()));
    for (int64_t i = 0; i < binary.length(); ++i) {
      out.push_back(binary.IsNull(i) ? std::nullopt : std::optional(binary.GetString(i)));
    }
    return out;
  };
  using Values = std::vector<std::optional<std::string>>;
  // Row 1's value is NULL, row 5's start is NULL, row 3's window closes before it opens, and row 4
  // is empty; row 0 keeps two codepoints of three bytes and row 2 clips at the end.
  auto three = call(Int64s({1, 1, 2, 3, 1, std::nullopt}), Int64s({2, 2, 9, -1, 9, 1}));
  ASSERT_TRUE(three.ok()) << three.status().ToString();
  EXPECT_EQ(values(*three),
            (Values{"h\xC3\xA9", std::nullopt, "b", "", continuation_first, std::nullopt}));
  // A NULL length is a NULL result even where the value and the start are not.
  auto null_length = call(Int64s({1, 1, 1, 1, 1, 1}), Int64s({1, 1, std::nullopt, 1, 1, 1}));
  ASSERT_TRUE(null_length.ok()) << null_length.status().ToString();
  EXPECT_EQ(values(*null_length)[2], std::nullopt);
  // Without a length the window runs to the end of the value here, since 4294967295 covers it, and
  // a negative start counts from the end. The first byte of row 4 is kept: no byte of a value may
  // fall outside every codepoint, or a full window would lose it.
  auto two = call(Int64s({-2, 1, -9, 1, -2, 1}), nullptr);
  ASSERT_TRUE(two.ok()) << two.status().ToString();
  EXPECT_EQ(values(*two), (Values{"lo", std::nullopt, "ab", "", continuation_first, "ab"}));
  // The bounds DuckDB puts on a position, one message per side, raised even though the batch holds
  // a NULL value and a NULL start.
  for (const auto& [start, length, side] :
       std::vector<std::tuple<int64_t, int64_t, std::string>>{{4294967296, 1, "offset"},
                                                              {-4294967297, 1, "offset"},
                                                              {1, 4294967296, "length"},
                                                              {1, -4294967297, "length"}}) {
    const auto bad = call(Int64s({start, start, start, start, start, std::nullopt}),
                          Int64s({length, length, length, length, length, length}));
    ASSERT_FALSE(bad.ok()) << start << ", " << length;
    EXPECT_NE(bad.status().message().find("Substring " + side + " outside of supported range"),
              std::string::npos)
        << bad.status().ToString();
  }
}

// strlen counts bytes; regexp_replace replaces the first match with RE2 in UTF-8 mode (`.` is one
// character, not a newline), \0 and \1 in the replacement; NULL stays NULL; a bad pattern is an
// execution error (DuckDB 1.5.5).
TEST_F(ComputeTest, StringFunctions) {
  const auto text = testing::Strings({"h\xC3\xA9llo", std::nullopt, "a\nb", "", "abab"});
  auto lengths = Eval(
      Call(plan::Function::kStrlen, {ColumnAt(0, LogicalType::kVarchar)}, LogicalType::kBigInt),
      {text});
  ASSERT_TRUE(lengths.ok()) << lengths.status().ToString();
  EXPECT_EQ((*lengths)->ToString(), "[\n  6,\n  null,\n  3,\n  0,\n  4\n]");
  const auto replace = [&](std::string pattern, std::string replacement) {
    return Eval(Call(plan::Function::kRegexpReplace,
                     {ColumnAt(0, LogicalType::kVarchar), StringConstant(std::move(pattern)),
                      StringConstant(std::move(replacement))},
                     LogicalType::kVarchar),
                {text});
  };
  const auto values = [](const std::shared_ptr<arrow::Array>& array) {
    std::vector<std::optional<std::string>> out;
    const auto& binary = static_cast<const arrow::BinaryArray&>(*array);
    out.reserve(Narrow<std::size_t>(binary.length()));
    for (int64_t i = 0; i < binary.length(); ++i) {
      out.push_back(binary.IsNull(i) ? std::nullopt : std::optional(binary.GetString(i)));
    }
    return out;
  };
  using Values = std::vector<std::optional<std::string>>;
  auto dot = replace("h.l", "_");
  ASSERT_TRUE(dot.ok()) << dot.status().ToString();
  EXPECT_TRUE((*dot)->type()->Equals(*arrow::binary()));
  EXPECT_EQ(values(*dot), (Values{"_lo", std::nullopt, "a\nb", "", "abab"}));
  auto first = replace("(b)", "[\\1\\0]");
  ASSERT_TRUE(first.ok());
  EXPECT_EQ(values(*first), (Values{"h\xC3\xA9llo", std::nullopt, "a\n[bb]", "", "a[bb]ab"}));
  // Each distinct value is replaced once and the results go back to every row: repeated values,
  // NULLs and values without a match in one batch.
  const auto repeated =
      testing::Strings({"abab", std::nullopt, "abab", "xyz", "", "abab", std::nullopt, "xyz", "b"});
  auto once = Eval(
      Call(plan::Function::kRegexpReplace,
           {ColumnAt(0, LogicalType::kVarchar), StringConstant("(b)"), StringConstant("<\\1>")},
           LogicalType::kVarchar),
      {repeated});
  ASSERT_TRUE(once.ok()) << once.status().ToString();
  EXPECT_EQ(values(*once), (Values{"a<b>ab", std::nullopt, "a<b>ab", "xyz", "", "a<b>ab",
                                   std::nullopt, "xyz", "<b>"}));
  // A batch of NULLs only (no distinct value to replace) stays NULL.
  auto nulls = Eval(
      Call(plan::Function::kRegexpReplace,
           {ColumnAt(0, LogicalType::kVarchar), StringConstant("(b)"), StringConstant("<\\1>")},
           LogicalType::kVarchar),
      {testing::Strings({std::nullopt, std::nullopt})});
  ASSERT_TRUE(nulls.ok()) << nulls.status().ToString();
  EXPECT_EQ(values(*nulls), (Values{std::nullopt, std::nullopt}));
  auto newline = replace("a.b", "X");
  ASSERT_TRUE(newline.ok());
  EXPECT_EQ(values(*newline)[2], "a\nb") << "`.` does not match a newline";
  for (const char* invalid : {"\\1", "x\\", "\\q"}) {
    auto unchanged = replace("a", invalid);
    ASSERT_TRUE(unchanged.ok()) << invalid << ": " << unchanged.status().ToString();
    EXPECT_EQ(values(*unchanged), (Values{"h\xC3\xA9llo", std::nullopt, "a\nb", "", "abab"}))
        << "DuckDB leaves the text unchanged for the replacement " << invalid;
  }
  // Divergence D15: RE2 never matches a byte that is not UTF-8, which DuckDB cannot read at all.
  auto invalid_utf8 =
      Eval(Call(plan::Function::kRegexpReplace,
                {ColumnAt(0, LogicalType::kVarchar), StringConstant("[^a]"), StringConstant("X")},
                LogicalType::kVarchar),
           {testing::Strings({"a\xFF"
                              "b"})});
  ASSERT_TRUE(invalid_utf8.ok()) << invalid_utf8.status().ToString();
  EXPECT_EQ(values(*invalid_utf8), (Values{"a\xFF"
                                           "X"}));
  auto bad = replace("(", "x");
  EXPECT_TRUE(bad.status().IsExecutionError()) << bad.status().ToString();
  // Valid once wrapped in (...), but not alone: DuckDB rejects it.
  auto unbalanced = replace("a)|(b", "x");
  EXPECT_TRUE(unbalanced.status().IsExecutionError()) << unbalanced.status().ToString();
  // Assertions see the whole value: \B after the first byte of "abab", not at the match's end.
  auto context = replace("a\\B", "Z");
  ASSERT_TRUE(context.ok()) << context.status().ToString();
  EXPECT_EQ(values(*context)[4], "Zbab");
}

// A pattern anchored at ^ runs as the user wrote it, where any other pattern is wrapped in
// ^(\C*?)(...) so that RE2 does the unanchored search (see RegexpReplace). The two accept exactly
// the same values -- a leading ^ matches in one place only, so the lazy prefix can match nothing --
// and the wrapper costs RE2 the literal prefix it would otherwise reject a non-matching value on.
// Each case here is the anchored pattern against the wrapper written out by hand, which is what the
// evaluator used to build; the wrapper form is itself ^-anchored, so it runs as it always did.
TEST_F(ComputeTest, AnchoredRegexpReplaceMatchesTheWrappedForm) {
  const auto text = testing::Strings(
      {"abab", std::nullopt, "", "xyz", "abc", "a\nb", "aaa", "h\xC3\xA9llo", "ab", "\nab"});
  const auto replace = [&](std::string pattern, std::string replacement) {
    return Eval(Call(plan::Function::kRegexpReplace,
                     {ColumnAt(0, LogicalType::kVarchar), StringConstant(std::move(pattern)),
                      StringConstant(std::move(replacement))},
                     LogicalType::kVarchar),
                {text});
  };
  const auto values = [](const std::shared_ptr<arrow::Array>& array) {
    std::vector<std::optional<std::string>> out;
    const auto& binary = static_cast<const arrow::BinaryArray&>(*array);
    out.reserve(Narrow<std::size_t>(binary.length()));
    for (int64_t i = 0; i < binary.length(); ++i) {
      out.push_back(binary.IsNull(i) ? std::nullopt : std::optional(binary.GetString(i)));
    }
    return out;
  };
  struct Case {
    std::string pattern;      // anchored: the evaluator runs it bare
    std::string replacement;  // the user's groups
    std::string shifted;      // the same, groups + 2, for the hand-written wrapper
  };
  // Zero-width matches, an empty-only pattern, several groups, an assertion, and a non-ASCII value.
  const std::vector<Case> cases = {
      {.pattern = "^a(b)", .replacement = R"([\1])", .shifted = R"([\3])"},
      {.pattern = "^(.)", .replacement = R"(<\1>)", .shifted = R"(<\3>)"},
      {.pattern = "^x*", .replacement = "Z", .shifted = "Z"},
      {.pattern = "^$", .replacement = "E", .shifted = "E"},
      {.pattern = "^(a)(b)(c)", .replacement = R"(\3\2\1)", .shifted = R"(\5\4\3)"},
      {.pattern = R"(^a\B)", .replacement = "Q", .shifted = "Q"},
      {.pattern = R"(^\C)", .replacement = "B", .shifted = "B"},
      {.pattern = "^(?i)AB", .replacement = "i", .shifted = "i"},
      {.pattern = "^.*b", .replacement = "G", .shifted = "G"},
  };
  for (const Case& c : cases) {
    auto bare = replace(c.pattern, c.replacement);
    ASSERT_TRUE(bare.ok()) << c.pattern << ": " << bare.status().ToString();
    auto wrapped = replace("^(\\C*?)(" + c.pattern + ")", "\\1" + c.shifted);
    ASSERT_TRUE(wrapped.ok()) << c.pattern << ": " << wrapped.status().ToString();
    EXPECT_EQ(values(*bare), values(*wrapped)) << c.pattern << " -> " << c.replacement;
  }
}

// A pattern ending in `.*$` has that tail rewritten to `\C*$`, RE2's any byte, because the match is
// replaced whole and the tail's bytes are thrown away (see RegexpReplace). `.` is a character that
// is not a newline, so the two agree only on a value that is a whole number of characters and holds
// no newline; the rest run the pattern as written. Each case here is the rewritten pattern against
// `[^\n]*$`, which means what `.*$` means and is not rewritten, so it is the answer DuckDB gives.
TEST_F(ComputeTest, ARewrittenRegexpReplaceTailMatchesTheDotTail) {
  using Values = std::vector<std::optional<std::string>>;
  const auto replace = [&](const std::shared_ptr<arrow::Array>& text, std::string pattern,
                           std::string replacement) {
    return Eval(Call(plan::Function::kRegexpReplace,
                     {ColumnAt(0, LogicalType::kVarchar), StringConstant(std::move(pattern)),
                      StringConstant(std::move(replacement))},
                     LogicalType::kVarchar),
                {text});
  };
  const auto values = [](const std::shared_ptr<arrow::Array>& array) {
    std::vector<std::optional<std::string>> out;
    const auto& binary = static_cast<const arrow::BinaryArray&>(*array);
    out.reserve(Narrow<std::size_t>(binary.length()));
    for (int64_t i = 0; i < binary.length(); ++i) {
      out.push_back(binary.IsNull(i) ? std::nullopt : std::optional(binary.GetString(i)));
    }
    return out;
  };
  // A newline first, last and in the middle (the values the tails disagree on, so the ones that
  // must take the pattern as written), a value with no tail at all, an empty one, NULL, a
  // multi-byte character, and two URLs, that being the idiom a discarded tail comes from.
  const auto text = testing::Strings({"abab", std::nullopt, "", "a\nb", "\nab", "ab\n", "a", "za",
                                      "h\xC3\xA9llo", "https://www.example.com/p/q", "http://h/"});
  const std::vector<std::pair<std::string, std::string>> cases = {
      {R"(^[a-z]+://([^@/]+)/.*$)", R"(\1)"},  // keep a prefix group, drop the rest
      {"^a.*$", "X"},                          // anchored, the plain shape
      {"a.*$", "X"},                           // unanchored: rewritten inside the wrapper
      {R"(^(a).*$)", R"([\1])"},               // a group before the tail
      {"^x.*$", "X"},                          // matches nothing in the corpus
      {".*$", "X"},                            // the whole value
      {"^a|b.*$", "X"},                        // an alternation, so wrapped as written
      {R"(^a\\.*$)", "X"},                     // an escaped backslash, then a real tail
      {R"(^.\C.*$)", "X"},                     // `\C` lands the tail inside a character
      {R"(\B.*$)", "X"},                       // so does `\B`, which holds mid-character
  };
  for (const auto& [pattern, replacement] : cases) {
    auto rewritten = replace(text, pattern, replacement);
    ASSERT_TRUE(rewritten.ok()) << pattern << ": " << rewritten.status().ToString();
    auto as_written =
        replace(text, pattern.substr(0, pattern.size() - 3) + R"([^\n]*$)", replacement);
    ASSERT_TRUE(as_written.ok()) << pattern << ": " << as_written.status().ToString();
    EXPECT_EQ(values(*rewritten), values(*as_written)) << pattern << " -> " << replacement;
  }
  // A byte that is no character at all: `.` cannot match it, so the pattern does not match and
  // DuckDB returns the value unchanged (divergence D15), where `\C*$` would consume it. One such
  // value in a batch takes the rewrite off every value of it, which is why this is its own array.
  const auto invalid =
      testing::Strings({"a\xFF"
                        "b",
                        "ab"});
  auto bytes = replace(invalid, "^a.*$", "X");
  ASSERT_TRUE(bytes.ok()) << bytes.status().ToString();
  EXPECT_EQ(values(*bytes), (Values{"a\xFF"
                                    "b",
                                    "X"}));
  // Values with no bytes at all: the one distinct value is empty, and a buffer of no bytes can be a
  // null one, which neither check may read (as in KeyHashes, where that was a crash).
  auto empty = replace(testing::Strings({"", std::nullopt, ""}), "^.*$", "X");
  ASSERT_TRUE(empty.ok()) << empty.status().ToString();
  EXPECT_EQ(values(*empty), (Values{"X", std::nullopt, "X"}));
  // A character split across two values: the bytes validate concatenated, as the pair they form,
  // where neither value is a character on its own. The boundary test is what rejects them, so `.`
  // matches neither value and both come back unchanged.
  auto split = replace(testing::Strings({"\xC3", "\xA9"}), "^.*$", "X");
  ASSERT_TRUE(split.ok()) << split.status().ToString();
  EXPECT_EQ(values(*split), (Values{"\xC3", "\xA9"}));
  // The two halves of the pattern test: an escaped dot is a literal, and a `.*` without the anchor
  // is not the tail. Rewriting either would change what the pattern means, not only its speed.
  auto literal = replace(testing::Strings({"a", "a.", "a..", "ab"}), R"(^a\.*$)", "X");
  ASSERT_TRUE(literal.ok()) << literal.status().ToString();
  EXPECT_EQ(values(*literal), (Values{"X", "X", "X", "ab"}));
  auto unterminated = replace(testing::Strings({"a\nb", "ab", "za"}), "^a.*", "X");
  ASSERT_TRUE(unterminated.ok()) << unterminated.status().ToString();
  EXPECT_EQ(values(*unterminated), (Values{"X\nb", "X", "za"}));
}

// A condition leaf: `column <op> constant` over operand 0, or a folded kind.
plan::ExprPtr Condition(plan::Predicate::Kind kind, plan::CompareOp op, Int128 value,
                        plan::ExprPtr operand) {
  const LogicalType type = operand->type;
  plan::Predicate predicate{.kind = kind,
                            .column = plan::BoundColumn{.index = 0, .name = "o", .type = type},
                            .op = op,
                            .constant = plan::Constant{.type = type, .value = value},
                            .span = {}};
  return std::make_shared<const plan::Expr>(plan::Expr{
      .node =
          plan::PredicateExpr{.predicate = std::move(predicate), .operands = {std::move(operand)}},
      .type = LogicalType::kBoolean,
      .name = "p"});
}

plan::ExprPtr Bool(plan::BoolOp op, std::vector<plan::ExprPtr> args) {
  return std::make_shared<const plan::Expr>(
      plan::Expr{.node = plan::BoolExpr{.op = op, .args = std::move(args)},
                 .type = LogicalType::kBoolean,
                 .name = "b"});
}

// AND, OR and NOT in SQL's three-valued logic; a folded leaf (never true, always true for a value)
// is NULL for a NULL operand, so NOT keeps rejecting NULL.
TEST_F(ComputeTest, ConditionsAreThreeValued) {
  using Kind = plan::Predicate::Kind;
  const auto x = Int16s({1, 5, std::nullopt, -3});
  const auto column = ColumnAt(0, LogicalType::kSmallInt);
  const auto gt2 = Condition(Kind::kCompare, plan::CompareOp::kGt, 2, column);
  const auto lt0 = Condition(Kind::kCompare, plan::CompareOp::kLt, 0, column);
  const auto show = [&](const plan::ExprPtr& e) {
    auto out = Eval(e, {x});
    return out.ok() ? (*out)->ToString() : out.status().ToString();
  };
  EXPECT_EQ(show(gt2), "[\n  false,\n  true,\n  null,\n  false\n]");
  EXPECT_EQ(show(Bool(plan::BoolOp::kOr, {gt2, lt0})), "[\n  false,\n  true,\n  null,\n  true\n]");
  EXPECT_EQ(show(Bool(plan::BoolOp::kAnd, {gt2, lt0})),
            "[\n  false,\n  false,\n  null,\n  false\n]");
  EXPECT_EQ(show(Bool(plan::BoolOp::kNot, {gt2})), "[\n  true,\n  false,\n  null,\n  true\n]");
  // A later argument is computed only for the rows still undecided, as in DuckDB: x * x (SMALLINT)
  // overflows for 30000, which x > 100 already decides for OR and x < 100 for AND.
  const auto wide = Int16s({1, 30000, std::nullopt, -3});
  const auto square = Condition(Kind::kCompare, plan::CompareOp::kGt, 0,
                                Arith(ArithOp::kMultiply, column, column, LogicalType::kSmallInt));
  const auto above = Condition(Kind::kCompare, plan::CompareOp::kGt, 100, column);
  const auto below = Condition(Kind::kCompare, plan::CompareOp::kLt, 100, column);
  auto any = Eval(Bool(plan::BoolOp::kOr, {above, square}), {wide});
  ASSERT_TRUE(any.ok()) << any.status().ToString();
  EXPECT_EQ((*any)->ToString(), "[\n  true,\n  true,\n  null,\n  true\n]");
  auto both = Eval(Bool(plan::BoolOp::kAnd, {below, square}), {wide});
  ASSERT_TRUE(both.ok()) << both.status().ToString();
  EXPECT_EQ((*both)->ToString(), "[\n  true,\n  false,\n  null,\n  true\n]");
  auto unguarded = Eval(Bool(plan::BoolOp::kOr, {below, square}), {wide});
  EXPECT_TRUE(unguarded.status().IsExecutionError()) << "30000 * 30000 is computed here";
  const auto never = Condition(Kind::kFalse, plan::CompareOp::kEq, 0, column);
  EXPECT_EQ(show(Bool(plan::BoolOp::kNot, {never})), "[\n  true,\n  true,\n  null,\n  true\n]");
  const auto always = Condition(Kind::kIsNotNull, plan::CompareOp::kEq, 0, column);
  EXPECT_EQ(show(always), "[\n  true,\n  true,\n  null,\n  true\n]");
}

// CASE takes the first branch whose condition is true (NULL is not), else ELSE or NULL; a value is
// computed only for the rows its branch answers, so a guarded overflow never fails, and a later
// condition only for the rows no earlier branch took.
TEST_F(ComputeTest, CaseEvaluatesEachBranchOnItsRows) {
  using Kind = plan::Predicate::Kind;
  const auto x = Int16s({1, 30000, std::nullopt, -3, 200});
  const auto column = ColumnAt(0, LogicalType::kSmallInt);
  const auto small = Condition(Kind::kCompare, plan::CompareOp::kLt, 100, column);
  // x * 2 in SMALLINT: 30000 * 2 overflows, so only the rows below 100 may compute it.
  const auto doubled = Arith(ArithOp::kMultiply, column, ConstantOf(2, LogicalType::kInteger),
                             LogicalType::kSmallInt);
  const auto big = Condition(Kind::kCompare, plan::CompareOp::kGt, 10000, column);
  const auto make = [](std::vector<plan::ExprPtr> whens, std::vector<plan::ExprPtr> thens,
                       plan::ExprPtr otherwise, LogicalType type) {
    return std::make_shared<const plan::Expr>(
        plan::Expr{.node = plan::CaseExpr{.whens = std::move(whens),
                                          .thens = std::move(thens),
                                          .otherwise = std::move(otherwise)},
                   .type = type,
                   .name = "case"});
  };
  auto guarded = Eval(make({small, big}, {doubled, ConstantOf(-1, LogicalType::kInteger)},
                           ConstantOf(7, LogicalType::kInteger), LogicalType::kSmallInt),
                      {x});
  ASSERT_TRUE(guarded.ok()) << guarded.status().ToString();
  EXPECT_EQ((*guarded)->ToString(), "[\n  2,\n  -1,\n  7,\n  -6,\n  7\n]");
  auto no_else = Eval(make({big}, {column}, nullptr, LogicalType::kInteger), {x});
  ASSERT_TRUE(no_else.ok()) << no_else.status().ToString();
  EXPECT_TRUE((*no_else)->type()->Equals(*arrow::int32()));
  EXPECT_EQ((*no_else)->ToString(), "[\n  null,\n  30000,\n  null,\n  null,\n  null\n]");
  // The overflow still fails where the branch answers it.
  auto overflow = Eval(make({big}, {doubled}, nullptr, LogicalType::kSmallInt), {x});
  EXPECT_TRUE(overflow.status().IsExecutionError()) << overflow.status().ToString();
  // No row taken: every value computed for none.
  auto none = Eval(make({Condition(Kind::kFalse, plan::CompareOp::kEq, 0, column)}, {doubled},
                        column, LogicalType::kSmallInt),
                   {x});
  ASSERT_TRUE(none.ok()) << none.status().ToString();
  EXPECT_EQ((*none)->ToString(), (*x).ToString());
}

// A DECIMAL CASE casts each value to its type on the rows the value's branch takes, as DuckDB casts
// (ADR 0021 rule 10): up exactly, down rounded half away from zero (beyond 38 digits a value can
// gain a digit), an integer times 10^s. A value that does not fit fails with DuckDB's conversion
// error, which names the column it casts, and only where its branch answers.
TEST_F(ComputeTest, DecimalCaseValuesCastToTheCaseType) {
  using Kind = plan::Predicate::Kind;
  const auto make = [](plan::ExprPtr then, plan::ExprPtr otherwise, LogicalType type) {
    const auto first = Condition(Kind::kCompare, plan::CompareOp::kLt, 3,
                                 ColumnAt(0, LogicalType::kSmallInt));  // key < 3
    return std::make_shared<const plan::Expr>(plan::Expr{
        .node =
            plan::CaseExpr{
                .whens = {first}, .thens = {std::move(then)}, .otherwise = std::move(otherwise)},
        .type = type,
        .name = "case"});
  };
  const auto keys = Int16s({1, 2, 3, 4, 5});
  // DECIMAL(15,2) and DECIMAL(9,4) give DECIMAL(17,4), exactly that Arrow type; row 2's p is not
  // taken.
  const LogicalType p = LogicalType::Decimal(15, 2);
  const LogicalType r = LogicalType::Decimal(9, 4);
  auto up = Eval(make(ColumnAt(1, p), ColumnAt(2, r), LogicalType::Decimal(17, 4)),
                 {keys, Decimals(p, {"1700", "-25", "999999999999999", "1", std::nullopt}),
                  Decimals(r, {"1", "2", "3", "-99999999", std::nullopt})});
  ASSERT_TRUE(up.ok()) << up.status().ToString();
  EXPECT_EQ(UnscaledText(**up), "DECIMAL(17,4): 170000 -2500 3 -99999999 null");
  // DECIMAL(38,10) next to DECIMAL(38,0) is DECIMAL(38,0): 1.5, -2.5, 0.4999999999 and
  // -9999999999999999999999999999.5 round to 2, -3, 0 and -10^28.
  const LogicalType z = LogicalType::Decimal(38, 10);
  const LogicalType h = LogicalType::Decimal(38, 0);
  auto down = Eval(make(ColumnAt(1, z), ColumnAt(2, h), h),
                   {Int16s({1, 2, 1, 2, 5}),
                    Decimals(z, {"15000000000", "-25000000000", "4999999999",
                                 "-99999999999999999999999999995000000000", "0"}),
                    Decimals(h, {"0", "0", "0", "0", "-7"})});
  ASSERT_TRUE(down.ok()) << down.status().ToString();
  EXPECT_EQ(UnscaledText(**down), "DECIMAL(38,0): 2 -3 0 -10000000000000000000000000000 -7");
  // An integer that does not fit DECIMAL(38,30) fails where it is taken, named by its column (an
  // aggregate's output column too); a constant is not named.
  const LogicalType n = LogicalType::Decimal(38, 30);
  const auto bigint = Named(ColumnAt(1, LogicalType::kBigInt), "b");
  const arrow::ArrayVector rows = {Int16s({1, 5}), Int64s({123456789, 99}),
                                   Decimals(n, {"0", "1"})};
  auto named = Eval(make(bigint, ColumnAt(2, n), n), rows);
  EXPECT_TRUE(named.status().IsExecutionError()) << named.status().ToString();
  EXPECT_EQ(named.status().message(),
            "Could not cast value 123456789 to DECIMAL(38,30) when casting from source column b");
  auto constant =
      Eval(make(ConstantOf(-123456789, LogicalType::kInteger), ColumnAt(2, n), n), rows);
  EXPECT_EQ(constant.status().message(), "Could not cast value -123456789 to DECIMAL(38,30)");
  auto untaken = Eval(make(bigint, ColumnAt(2, n), n),
                      {Int16s({5, 6}), Int64s({123456789, 99}), Decimals(n, {"1", std::nullopt})});
  ASSERT_TRUE(untaken.ok()) << untaken.status().ToString();
  EXPECT_EQ(UnscaledText(**untaken), "DECIMAL(38,30): 1 null");
  // A HUGEINT (an integer SUM) keeps the integer message; DECIMAL(15,2) next to it is
  // DECIMAL(38,2).
  auto huge = Eval(
      make(Named(ColumnAt(1, LogicalType::kHugeInt), "sum(b)"), ColumnAt(2, p),
           LogicalType::Decimal(38, 2)),
      {Int16s({1}), Decimals(LogicalType::kHugeInt, {"10000000000000000000000000000000000000"}),
       Decimals(p, {"0"})});
  EXPECT_EQ(huge.status().message(),
            "Could not cast value 10000000000000000000000000000000000000 to DECIMAL(38,2) when "
            "casting from source column sum(b)");
  // A DECIMAL value beyond its declared width (a file can hold one) fails like any value that does
  // not fit, printed at its own scale, as in DuckDB (probed with such a file).
  const LogicalType tiny = LogicalType::Decimal(4, 2);
  auto beyond = Eval(
      make(ColumnAt(1, tiny), ColumnAt(2, LogicalType::Decimal(5, 2)), LogicalType::Decimal(5, 2)),
      {Int16s({1}), Decimals(tiny, {"12345678"}), Decimals(LogicalType::Decimal(5, 2), {"0"})});
  EXPECT_EQ(beyond.status().message(),
            "Casting value \"123456.78\" to type DECIMAL(5,2) failed: value is out of range! when "
            "casting from source column c");
}

plan::ExprPtr CastOf(plan::ExprPtr operand, LogicalType type) {
  return std::make_shared<const plan::Expr>(plan::Expr{
      .node = plan::CastExpr{.operand = std::move(operand)}, .type = type, .name = "cast"});
}

// A join key's cast (plan::CastExpr) keeps every value, the extremes of each type included, and
// NULL stays NULL: integers widen to integers, HUGEINT and DECIMALs, and DECIMALs to DECIMALs with
// more digits on either side of the point.
TEST_F(ComputeTest, CastWidensExactly) {
  const auto cast = [](const std::shared_ptr<arrow::Array>& values, LogicalType from,
                       LogicalType to) -> std::shared_ptr<arrow::Array> {
    auto out = Eval(CastOf(ColumnAt(0, from), to), {values});
    EXPECT_TRUE(out.ok()) << from << " to " << to << ": " << out.status().ToString();
    if (!out.ok()) {
      return nullptr;
    }
    EXPECT_TRUE((*out)->type()->Equals(*plan::ToArrow(to))) << (*out)->type()->ToString();
    return *out;
  };
  const auto int16 =
      cast(Int16s({-32768, std::nullopt, 32767}), LogicalType::kSmallInt, LogicalType::kInteger);
  ASSERT_NE(int16, nullptr);
  EXPECT_EQ(int16->ToString(), "[\n  -32768,\n  null,\n  32767\n]");
  const auto uint16 = cast(testing::UInt16s({65535, std::nullopt, 0}), LogicalType::kUSmallInt,
                           LogicalType::kInteger);
  ASSERT_NE(uint16, nullptr);
  EXPECT_EQ(uint16->ToString(), "[\n  65535,\n  null,\n  0\n]");
  constexpr int32_t kMin32 = std::numeric_limits<int32_t>::min();
  constexpr int32_t kMax32 = std::numeric_limits<int32_t>::max();
  const auto int32 = testing::Int32s({kMin32, std::nullopt, kMax32});
  const auto int64 = cast(int32, LogicalType::kInteger, LogicalType::kBigInt);
  ASSERT_NE(int64, nullptr);
  EXPECT_EQ(int64->ToString(), "[\n  -2147483648,\n  null,\n  2147483647\n]");
  const auto huge = cast(int32, LogicalType::kInteger, LogicalType::kHugeInt);
  ASSERT_NE(huge, nullptr);
  EXPECT_EQ(UnscaledText(*huge), "DECIMAL(38,0): -2147483648 null 2147483647");
  const auto cents = cast(Int64s({std::numeric_limits<int64_t>::min(), std::nullopt,
                                  std::numeric_limits<int64_t>::max()}),
                          LogicalType::kBigInt, LogicalType::Decimal(21, 2));
  ASSERT_NE(cents, nullptr);
  EXPECT_EQ(UnscaledText(*cents),
            "DECIMAL(21,2): -922337203685477580800 null 922337203685477580700");
  const auto wider = cast(Decimals(LogicalType::Decimal(4, 2), {"9999", std::nullopt, "-9999"}),
                          LogicalType::Decimal(4, 2), LogicalType::Decimal(5, 3));
  ASSERT_NE(wider, nullptr);
  EXPECT_EQ(UnscaledText(*wider), "DECIMAL(5,3): 99990 null -99990");
  const auto sums = cast(Decimals(LogicalType::kHugeInt, {std::string(38, '9'), std::nullopt}),
                         LogicalType::kHugeInt, LogicalType::Decimal(38, 0));
  ASSERT_NE(sums, nullptr);
  EXPECT_EQ(UnscaledText(*sums), "DECIMAL(38,0): " + std::string(38, '9') + " null");
}

// Anything but an exact widening is a malformed plan, and a DECIMAL value beyond its own declared
// width (a file can hold one) fails the cast with DuckDB's conversion error, naming the column.
TEST_F(ComputeTest, CastRejectsWhatIsNoExactWidening) {
  const auto narrowing =
      Eval(CastOf(ColumnAt(0, LogicalType::kBigInt), LogicalType::kInteger), {Int64s({1})});
  EXPECT_TRUE(narrowing.status().IsInvalid()) << narrowing.status().ToString();
  EXPECT_EQ(narrowing.status().message(), "a cast of BIGINT to INTEGER is not an exact widening");
  const LogicalType price = LogicalType::Decimal(15, 2);
  const auto scale =
      Eval(CastOf(ColumnAt(0, price), LogicalType::Decimal(15, 3)), {Decimals(price, {"1"})});
  EXPECT_EQ(scale.status().message(),
            "a cast of DECIMAL(15,2) to DECIMAL(15,3) is not an exact widening");
  const auto empty = std::make_shared<const plan::Expr>(
      plan::Expr{.node = plan::CastExpr{}, .type = LogicalType::kBigInt, .name = "cast"});
  EXPECT_TRUE(Eval(empty, {Int64s({1})}).status().IsInvalid());
  const LogicalType tiny = LogicalType::Decimal(4, 2);
  auto beyond =
      Eval(CastOf(ColumnAt(0, tiny), LogicalType::Decimal(5, 3)), {Decimals(tiny, {"12345678"})});
  EXPECT_TRUE(beyond.status().IsExecutionError()) << beyond.status().ToString();
  EXPECT_EQ(beyond.status().message(),
            "Casting value \"123456.78\" to type DECIMAL(5,3) failed: value is out of range! when "
            "casting from source column c");
}

// % of DECIMALs beyond 38 digits is DOUBLE, as DuckDB computes it: fmod of the operands converted
// by rule 8 (a BIGINT to its nearest double, a HUGEINT by DuckDB's 128-bit formula), with the
// dividend's sign (-0.0 too) and NULL for a zero divisor; with a DOUBLE operand a zero divisor
// gives NaN, as for any DOUBLE %.
TEST_F(ComputeTest, DecimalModuloBeyond38DigitsIsDouble) {
  const LogicalType h = LogicalType::Decimal(38, 0);
  const LogicalType half = LogicalType::Decimal(2, 1);
  auto wide =
      Eval(Arith(ArithOp::kModulo, ColumnAt(0, h), ColumnAt(1, half), LogicalType::kDouble),
           {Decimals(h, {"-4", "7", std::nullopt, "3", "99999999999999999999999999999999999999"}),
            Decimals(half, {"5", "25", "5", "0", "25"})});
  ASSERT_TRUE(wide.ok()) << wide.status().ToString();
  const auto& w = static_cast<const arrow::DoubleArray&>(**wide);
  EXPECT_EQ(w.Value(0), 0.0);
  EXPECT_TRUE(std::signbit(w.Value(0)));  // -4 % 0.5 is -0.0
  EXPECT_EQ(w.Value(1), 2.0);
  EXPECT_TRUE(w.IsNull(2));
  EXPECT_TRUE(w.IsNull(3));  // a zero divisor: NULL, not NaN
  EXPECT_EQ(w.Value(4), std::fmod(DuckDbDecimalToDouble(PowerOfTen(38) - 1, 38, 0), 2.5));
  // HUGEINT: 2^64 + 2^63 + 2049 converts to 2^64 + 2^63 (DuckDB's formula).
  auto huge =
      Eval(Arith(ArithOp::kModulo, ColumnAt(0, LogicalType::kHugeInt), ColumnAt(1, half),
                 LogicalType::kDouble),
           {Decimals(LogicalType::kHugeInt, {"27670116110564329473"}), Decimals(half, {"25"})});
  ASSERT_TRUE(huge.ok()) << huge.status().ToString();
  EXPECT_EQ(static_cast<const arrow::DoubleArray&>(**huge).Value(0), std::fmod(0x1.8p64, 2.5));
  // BIGINT % DECIMAL(38,30): 2^53 + 1 converts to 2^53.
  const LogicalType n = LogicalType::Decimal(38, 30);
  auto bigint =
      Eval(Arith(ArithOp::kModulo, ColumnAt(0, LogicalType::kBigInt), ColumnAt(1, n),
                 LogicalType::kDouble),
           {Int64s({9007199254740993, 5}), Decimals(n, {"1500000000000000000000000000000", "0"})});
  ASSERT_TRUE(bigint.ok()) << bigint.status().ToString();
  const auto& b = static_cast<const arrow::DoubleArray&>(**bigint);
  EXPECT_EQ(b.Value(0), std::fmod(0x1p53, 1.5));
  EXPECT_TRUE(b.IsNull(1));
  // With a DOUBLE operand: NaN for a zero divisor.
  const LogicalType p = LogicalType::Decimal(15, 2);
  auto with_double = Eval(Arith(ArithOp::kModulo, ColumnAt(0, LogicalType::kDouble), ColumnAt(1, p),
                                LogicalType::kDouble),
                          {Doubles({7.5}), Decimals(p, {"0"})});
  ASSERT_TRUE(with_double.ok()) << with_double.status().ToString();
  EXPECT_TRUE(std::isnan(static_cast<const arrow::DoubleArray&>(**with_double).Value(0)));
}

plan::ExprPtr Temporal(plan::Function function, plan::ExprPtr value, std::string text,
                       LogicalType type) {
  return std::make_shared<const plan::Expr>(plan::Expr{
      .node = plan::FunctionExpr{.function = function,
                                 .args = {std::move(value), StringConstant(std::move(text))}},
      .type = type,
      .name = "f"});
}

// epoch_ms is exact to the microsecond range and fails beyond it, as DuckDB's conversion does;
// EXTRACT gives the civil field and date_trunc floors (before 1970 too); a DATE is its midnight.
TEST_F(ComputeTest, TimestampFunctions) {
  const auto millis = Int64s({1'373'896'800'000, -61'000, std::nullopt});
  const auto epoch =
      Call(plan::Function::kEpochMs, {ColumnAt(0, LogicalType::kBigInt)}, LogicalType::kTimestamp);
  auto timestamps = Eval(epoch, {millis});
  ASSERT_TRUE(timestamps.ok()) << timestamps.status().ToString();
  EXPECT_TRUE((*timestamps)->type()->Equals(*arrow::timestamp(arrow::TimeUnit::MICRO)));
  const auto& ts = static_cast<const arrow::TimestampArray&>(**timestamps);
  EXPECT_EQ(ts.Value(0), 1'373'896'800'000'000);
  EXPECT_EQ(ts.Value(1), -61'000'000);
  EXPECT_TRUE(ts.IsNull(2));
  auto beyond = Eval(epoch, {Int64s({9'223'372'036'854'776})});
  EXPECT_TRUE(beyond.status().IsExecutionError()) << beyond.status().ToString();
  // DuckDB's range is asymmetric: it starts at 290309-12-22 (BC) 00:00:00.
  auto lowest = Eval(epoch, {Int64s({-9'223'372'022'400'000})});
  ASSERT_TRUE(lowest.ok()) << lowest.status().ToString();
  EXPECT_EQ(plan::FormatTimestamp(static_cast<const arrow::TimestampArray&>(**lowest).Value(0)),
            "290309-12-22 (BC) 00:00:00");
  auto below = Eval(epoch, {Int64s({-9'223'372'022'400'001})});
  EXPECT_TRUE(below.status().IsExecutionError()) << below.status().ToString();

  const auto column = ColumnAt(0, LogicalType::kTimestamp);
  const auto part = [&](std::string field) {
    auto out =
        Eval(Temporal(plan::Function::kExtract, column, std::move(field), LogicalType::kBigInt),
             {*timestamps});
    return out.ok() ? (*out)->ToString() : out.status().ToString();
  };
  EXPECT_EQ(part("minute"), "[\n  0,\n  58,\n  null\n]");
  EXPECT_EQ(part("second"), "[\n  0,\n  59,\n  null\n]");
  EXPECT_EQ(part("year"), "[\n  2013,\n  1969,\n  null\n]");
  const auto floor = [&](std::string unit) -> std::vector<std::string> {
    auto out =
        Eval(Temporal(plan::Function::kDateTrunc, column, std::move(unit), LogicalType::kTimestamp),
             {*timestamps});
    if (!out.ok()) {
      return {out.status().ToString()};
    }
    const auto& a = static_cast<const arrow::TimestampArray&>(**out);
    return {plan::FormatTimestamp(a.Value(0)), plan::FormatTimestamp(a.Value(1))};
  };
  EXPECT_EQ(floor("minute"),
            (std::vector<std::string>{"2013-07-15 14:00:00", "1969-12-31 23:58:00"}));
  EXPECT_EQ(floor("week"), (std::vector<std::string>{"2013-07-15 00:00:00", "1969-12-29 00:00:00"}))
      << "weeks start on Monday";
  EXPECT_EQ(floor("quarter"),
            (std::vector<std::string>{"2013-07-01 00:00:00", "1969-10-01 00:00:00"}));
  // Years past 32767 (a millisecond epoch read as seconds; the top of the range) and DATE
  // infinities: EXTRACT is NULL, date_trunc keeps the infinity.
  auto far = Eval(epoch, {Int64s({1'373'896'800'000'000, 9'223'372'036'854'000})});
  ASSERT_TRUE(far.ok()) << far.status().ToString();
  auto far_year =
      Eval(Temporal(plan::Function::kExtract, column, "year", LogicalType::kBigInt), {*far});
  ASSERT_TRUE(far_year.ok()) << far_year.status().ToString();
  EXPECT_EQ((*far_year)->ToString(), "[\n  45507,\n  294247\n]");
  auto far_week =
      Eval(Temporal(plan::Function::kDateTrunc, column, "week", LogicalType::kTimestamp), {*far});
  ASSERT_TRUE(far_week.ok()) << far_week.status().ToString();
  EXPECT_EQ(plan::FormatTimestamp(static_cast<const arrow::TimestampArray&>(**far_week).Value(1)),
            "294247-01-04 00:00:00");
  auto infinite = testing::ArrayOf<arrow::Date32Builder>(
      arrow::date32(), std::vector<std::optional<int32_t>>{std::numeric_limits<int32_t>::max(),
                                                           -std::numeric_limits<int32_t>::max()});
  const auto date_column = ColumnAt(0, LogicalType::kDate);
  auto infinite_year = Eval(
      Temporal(plan::Function::kExtract, date_column, "year", LogicalType::kBigInt), {infinite});
  ASSERT_TRUE(infinite_year.ok()) << infinite_year.status().ToString();
  EXPECT_EQ((*infinite_year)->ToString(), "[\n  null,\n  null\n]");
  auto infinite_day =
      Eval(Temporal(plan::Function::kDateTrunc, date_column, "day", LogicalType::kTimestamp),
           {infinite});
  ASSERT_TRUE(infinite_day.ok()) << infinite_day.status().ToString();
  const auto& infinite_days = static_cast<const arrow::TimestampArray&>(**infinite_day);
  EXPECT_EQ(plan::FormatTimestamp(infinite_days.Value(0)), "infinity");
  EXPECT_EQ(plan::FormatTimestamp(infinite_days.Value(1)), "-infinity");
  // A result outside the range fails, as DuckDB's conversion does.
  auto out_of_range = Eval(
      Temporal(plan::Function::kDateTrunc, column, "year", LogicalType::kTimestamp), {*lowest});
  EXPECT_TRUE(out_of_range.status().IsExecutionError()) << out_of_range.status().ToString();
  // Every other EXTRACT field and date_trunc unit, over values checked against DuckDB 1.5.5:
  // 2013-01-01 14:05:06.789123, 2021-01-03 (ISO week 53 of 2020), 1969-12-31 23:59:59.5,
  // 0001-01-01 (BC) (year 0) and 0011-01-01 (BC) (year -10).
  const auto samples = testing::ArrayOf<arrow::TimestampBuilder, int64_t>(
      arrow::timestamp(arrow::TimeUnit::MICRO),
      {1'357'049'106'789'123, 1'609'632'000'000'000, -500'000, -62'167'219'200'000'000,
       -62'482'752'000'000'000});
  const auto fields = [&](std::string field) {
    const LogicalType type = field == "epoch" ? LogicalType::kDouble : LogicalType::kBigInt;
    auto out = Eval(Temporal(plan::Function::kExtract, column, std::move(field), type), {samples});
    return out.ok() ? (*out)->ToString() : out.status().ToString();
  };
  const auto list = [](std::string_view values) { return "[\n  " + std::string(values) + "\n]"; };
  EXPECT_EQ(fields("quarter"), list("1,\n  1,\n  4,\n  1,\n  1"));
  EXPECT_EQ(fields("week"), list("1,\n  53,\n  1,\n  52,\n  1"));
  EXPECT_EQ(fields("isoyear"), list("2013,\n  2020,\n  1970,\n  -1,\n  -10"));
  EXPECT_EQ(fields("dow"), list("2,\n  0,\n  3,\n  6,\n  1"));
  EXPECT_EQ(fields("isodow"), list("2,\n  7,\n  3,\n  6,\n  1"));
  EXPECT_EQ(fields("doy"), list("1,\n  3,\n  365,\n  1,\n  1"));
  EXPECT_EQ(fields("millisecond"), list("6789,\n  0,\n  59500,\n  0,\n  0"));
  EXPECT_EQ(fields("microsecond"), list("6789123,\n  0,\n  59500000,\n  0,\n  0"));
  EXPECT_EQ(fields("decade"), list("201,\n  202,\n  196,\n  0,\n  -1"));
  EXPECT_EQ(fields("century"), list("21,\n  21,\n  20,\n  -1,\n  -1"));
  EXPECT_EQ(fields("millennium"), list("3,\n  3,\n  2,\n  -1,\n  -1"));
  EXPECT_EQ(fields("epoch"),
            list("1357049106.789123,\n  1609632000,\n  -0.5,\n  -6.21672192e+10,\n  "
                 "-6.2482752e+10"));
  const auto floors = [&](std::string unit) {
    auto out =
        Eval(Temporal(plan::Function::kDateTrunc, column, std::move(unit), LogicalType::kTimestamp),
             {samples});
    std::vector<std::string> text;
    if (!out.ok()) {
      return std::vector<std::string>{out.status().ToString()};
    }
    const auto& a = static_cast<const arrow::TimestampArray&>(**out);
    text.reserve(static_cast<std::size_t>(a.length()));
    for (int64_t i = 0; i < a.length(); ++i) {
      text.push_back(plan::FormatTimestamp(a.Value(i)));
    }
    return text;
  };
  using Texts = std::vector<std::string>;
  EXPECT_EQ(floors("millisecond"),
            (Texts{"2013-01-01 14:05:06.789", "2021-01-03 00:00:00", "1969-12-31 23:59:59.5",
                   "0001-01-01 (BC) 00:00:00", "0011-01-01 (BC) 00:00:00"}));
  EXPECT_EQ(floors("decade"),
            (Texts{"2010-01-01 00:00:00", "2020-01-01 00:00:00", "1960-01-01 00:00:00",
                   "0001-01-01 (BC) 00:00:00", "0011-01-01 (BC) 00:00:00"}))
      << "the year truncated toward zero, as DuckDB does";
  EXPECT_EQ(floors("century"),
            (Texts{"2000-01-01 00:00:00", "2000-01-01 00:00:00", "1900-01-01 00:00:00",
                   "0001-01-01 (BC) 00:00:00", "0001-01-01 (BC) 00:00:00"}));
  EXPECT_EQ(floors("millennium"),
            (Texts{"2000-01-01 00:00:00", "2000-01-01 00:00:00", "1000-01-01 00:00:00",
                   "0001-01-01 (BC) 00:00:00", "0001-01-01 (BC) 00:00:00"}));
  EXPECT_EQ(floors("isoyear"),
            (Texts{"2012-12-31 00:00:00", "2019-12-30 00:00:00", "1969-12-29 00:00:00",
                   "0002-01-04 (BC) 00:00:00", "0011-01-01 (BC) 00:00:00"}));
  // A DATE: the timestamp of its midnight.
  auto days = testing::ArrayOf<arrow::Date32Builder>(arrow::date32(),
                                                     std::vector<std::optional<int32_t>>{-1});
  auto month = Eval(Temporal(plan::Function::kDateTrunc, ColumnAt(0, LogicalType::kDate), "month",
                             LogicalType::kTimestamp),
                    {days});
  ASSERT_TRUE(month.ok()) << month.status().ToString();
  EXPECT_EQ(plan::FormatTimestamp(static_cast<const arrow::TimestampArray&>(**month).Value(0)),
            "1969-12-01 00:00:00");
  auto hour = Eval(Temporal(plan::Function::kExtract, ColumnAt(0, LogicalType::kDate), "hour",
                            LogicalType::kBigInt),
                   {days});
  ASSERT_TRUE(hour.ok()) << hour.status().ToString();
  EXPECT_EQ((*hour)->ToString(), "[\n  0\n]");
}

TEST_F(ComputeTest, ConstantsFillEveryRow) {
  auto out = Eval(ConstantOf(7, LogicalType::kInteger), {Int16s({1, 2, 3})});
  ASSERT_TRUE(out.ok());
  EXPECT_EQ((*out)->ToString(), "[\n  7,\n  7,\n  7\n]");
  auto missing = Eval(ColumnAt(3, LogicalType::kSmallInt), {Int16s({1})});
  EXPECT_TRUE(missing.status().IsInvalid());
}

// ---- reusing an earlier expression of the same Compute (ADR 0026's C10, one program per node)
// ----

// The columns a Compute appends for `exprs` over `columns`: ComputeBatch, which is what lets a
// later expression read an earlier one's values rather than compile the same subtree again.
arrow::Result<arrow::ArrayVector> Appended(const std::vector<plan::ExprPtr>& exprs,
                                           const arrow::ArrayVector& columns) {
  const std::shared_ptr<arrow::RecordBatch> in = BatchOf(columns);
  const auto schema = ComputedSchema(*in->schema(), exprs);
  ARROW_ASSIGN_OR_RAISE(const Batch out, ComputeBatch(Batch{.data = in, .selection = {}}, exprs,
                                                      schema, arrow::default_memory_pool()));
  arrow::ArrayVector appended;
  for (int i = in->num_columns(); i < out.data->num_columns(); ++i) {
    appended.push_back(out.data->column(i));
  }
  return appended;
}

// The shape a SUM-heavy grouped query computes: one expression is a whole subtree of the next, so
// the second reads the first instead of compiling it again. What is asserted is the values, since
// nothing observable reports that the reuse happened -- the evidence for that is the measurement.
TEST_F(ComputeTest, ASharedSubtreeAnswersAsItWouldAlone) {
  const LogicalType p = LogicalType::Decimal(15, 2);
  const auto price = Decimals(p, {"1050025", "200000", "0", "999", std::nullopt});
  const auto discount = Decimals(p, {"10", "0", "500", std::nullopt, "7"});
  const auto tax = Decimals(p, {"8", "100", std::nullopt, "2", "3"});
  const auto one = ConstantOf(100, p);

  // `price * (1 - discount)`, then that whole expression times `(1 + tax)`.
  const auto net =
      Arith(ArithOp::kMultiply, ColumnAt(0, p),
            Arith(ArithOp::kSubtract, one, ColumnAt(1, p), LogicalType::Decimal(16, 2)),
            LogicalType::Decimal(31, 4));
  const auto charged = Arith(ArithOp::kMultiply, net,
                             Arith(ArithOp::kAdd, one, ColumnAt(2, p), LogicalType::Decimal(16, 2)),
                             LogicalType::Decimal(38, 6));

  auto both = Appended({net, charged}, {price, discount, tax});
  ASSERT_TRUE(both.ok()) << both.status().ToString();
  ASSERT_EQ(both->size(), 2U);

  // Each against the same expression computed on its own, which is the path without any reuse.
  auto net_alone = Eval(net, {price, discount, tax});
  auto charged_alone = Eval(charged, {price, discount, tax});
  ASSERT_TRUE(net_alone.ok()) << net_alone.status().ToString();
  ASSERT_TRUE(charged_alone.ok()) << charged_alone.status().ToString();
  EXPECT_EQ(UnscaledText(*(*both)[0]), UnscaledText(**net_alone));
  EXPECT_EQ(UnscaledText(*(*both)[1]), UnscaledText(**charged_alone))
      << "reading the earlier expression must give what recomputing it gave, NULLs included";
}

// The same expression twice in one node: the second is the first, so it is reused at its root.
TEST_F(ComputeTest, ADuplicateExpressionAnswersTwice) {
  const LogicalType p = LogicalType::Decimal(15, 2);
  const auto a = Decimals(p, {"500", std::nullopt, "250"});
  const auto sum = Arith(ArithOp::kAdd, ColumnAt(0, p), ConstantOf(100, p), p);

  auto both = Appended({sum, sum}, {a});
  ASSERT_TRUE(both.ok()) << both.status().ToString();
  ASSERT_EQ(both->size(), 2U);
  EXPECT_EQ(UnscaledText(*(*both)[0]), UnscaledText(*(*both)[1]));
  auto alone = Eval(sum, {a});
  ASSERT_TRUE(alone.ok()) << alone.status().ToString();
  EXPECT_EQ(UnscaledText(*(*both)[0]), UnscaledText(**alone));
}

// A shared subtree that overflows must fail with the error the earlier expression raises, at its
// own operation: reuse must not move which expression of a node reports first, since DuckDB
// evaluates a select list in order and so does ComputeBatch.
TEST_F(ComputeTest, ANodeFailsOnItsFirstFailingExpression) {
  const LogicalType d38 = LogicalType::Decimal(38, 0);
  // Doubling it is past DECIMAL(38, 0); the shift is unsigned because Int128 is signed.
  const auto huge = static_cast<Int128>(UInt128{1} << 126U);
  const auto column = Decimals(d38, {Int128ToString(huge)});
  // Two expressions that fail DIFFERENTLY, so the message says which one ran first. Comparing a
  // node against one of its own expressions evaluated alone proves nothing -- it is the same code
  // over the same input either way, which is what the first version of this case did.
  const auto added = Arith(ArithOp::kAdd, ColumnAt(0, d38), ColumnAt(0, d38), d38);
  const auto multiplied = Arith(ArithOp::kMultiply, ColumnAt(0, d38), ColumnAt(0, d38), d38);

  auto add_first = Appended({added, multiplied}, {column});
  auto multiply_first = Appended({multiplied, added}, {column});
  ASSERT_FALSE(add_first.ok());
  ASSERT_FALSE(multiply_first.ok());
  EXPECT_NE(add_first.status().ToString(), multiply_first.status().ToString())
      << "both orders gave the same message, so this case cannot see which expression ran first";
  EXPECT_EQ(add_first.status().ToString(), Eval(added, {column}).status().ToString());
  EXPECT_EQ(multiply_first.status().ToString(), Eval(multiplied, {column}).status().ToString());
}

// A column a file filled past its declared width is still checked beside a reused operand. The
// first expression has to SUCCEED for this to test anything: ComputeBatch stops at its first
// failing expression, so an out-of-width value in the shared expression itself would mean the
// reusing expression is never compiled and the case passes while reaching none of the new code.
TEST_F(ComputeTest, AColumnBeyondItsDeclaredWidthIsStillCheckedBesideAReusedExpression) {
  const LogicalType narrow = LogicalType::Decimal(2, 1);
  const LogicalType wider = LogicalType::Decimal(3, 1);  // the step's range is +-999
  // Column 0 is inside its width; column 1 is the value a file holds that no DECIMAL(2,1) admits,
  // and that no DECIMAL(3,1) admits either, so the step above has something to catch.
  const auto in_width = Decimals(narrow, {"50"});
  const auto beyond = Decimals(narrow, {"1500"});
  const auto shared = Arith(ArithOp::kAdd, ColumnAt(0, narrow), ConstantOf(1, narrow), narrow);
  const auto with_column = Arith(ArithOp::kAdd, shared, ColumnAt(1, narrow), wider);

  auto alone = Eval(shared, {in_width, beyond});
  ASSERT_TRUE(alone.ok()) << "the shared expression must succeed, or the next one never compiles: "
                          << alone.status().ToString();
  auto both = Appended({shared, with_column}, {in_width, beyond});
  EXPECT_FALSE(both.ok())
      << "the out-of-width column beside a reused operand must still be checked";

  // And the same node without the reuse raises the same way, so this is the column's check and not
  // an artefact of reuse: `with_column` alone compiles the shared subtree itself.
  auto without_reuse = Appended({with_column}, {in_width, beyond});
  EXPECT_FALSE(without_reuse.ok());
  EXPECT_EQ(both.status().ToString(), without_reuse.status().ToString());
}

// The reused operand's own range test may only be skipped for an expression whose result the
// arithmetic has already checked. A DECIMAL negation is not one: it keeps its operand's type and
// tests only 128 bits, so it passes a file's out-of-width value straight through. Marking such an
// array `checked` turned a cast error into an answer of 0.
// At both physical widths, because the check lives where the operand is READ and the two widths are
// read by different arms of ReadDecimalOperand. The narrow arm widens into a vector, which is also
// what an INTEGER operand does -- and an integer's values are inside its physical type, so that arm
// used to mark every operand it took `checked`. A decimal64 column of DECIMAL(2,1) holds 1500 as
// easily as a decimal128 one, so marking it checked answered 0 here instead of raising: the hole
// #191 closed, reopened one width over, and caught only because this case runs at both.
void CheckAReusedNegationIsRangeChecked() {
  const LogicalType narrow = LogicalType::Decimal(2, 1);
  const LogicalType wider = LogicalType::Decimal(3, 1);  // +-999, which 1500 is outside
  const auto beyond = Decimals(narrow, {"1500"});
  const auto negated = std::make_shared<const plan::Expr>(plan::Expr{
      .node = plan::NegateExpr{.operand = ColumnAt(0, narrow)}, .type = narrow, .name = "n"});
  // -1500 - (-1500) is 0, which fits DECIMAL(3,1): only testing the OPERANDS catches this, and the
  // step's result test cannot. That is what makes it the case the wrong `checked` let through.
  const auto difference = Arith(ArithOp::kSubtract, negated, negated, wider);

  auto negation_alone = Eval(negated, {beyond});
  ASSERT_TRUE(negation_alone.ok())
      << "a negation keeps the value a file holds: " << negation_alone.status().ToString();

  auto reused = Appended({negated, difference}, {beyond});
  EXPECT_FALSE(reused.ok())
      << "a reused negation carries an out-of-width value and must be checked";

  // Without the reuse the program cannot compile the negation at all and the recursive path checks
  // the operands: the two must agree, which is what "reuse changes no answer" means here.
  auto alone = Eval(difference, {beyond});
  EXPECT_FALSE(alone.ok());
  EXPECT_EQ(reused.status().ToString(), alone.status().ToString());
}

TEST_F(ComputeTest, AReusedNegationIsRangeCheckedByTheStepThatReadsIt) {
  for (const bool narrowing_on : {false, true}) {
    const NarrowDecimalsFor narrowing(narrowing_on);
    SCOPED_TRACE(std::format("narrowing {}", narrowing_on));
    CheckAReusedNegationIsRangeChecked();
  }
}

// ---- a step computed in int64 where its range admits one (ADR 0021's decimal64 item, N1) ----

// The same arithmetic forced down the Int128 path, as the control. `Compile` covers only DECIMAL
// +, - and * over columns, constants and other such nodes, so a double negation of an operand --
// the same value -- makes it bail and evaluate the whole tree recursively in Int128. Without a
// control like this an equivalence test would compare the narrow path with itself.
//
// It is the RIGHT operand that is wrapped. A cast error names the column it came from, and
// wrapping the left operand would leave the control unable to name it, so the two paths would
// differ in the message for a reason that has nothing to do with the arithmetic.
plan::ExprPtr Negated(const plan::ExprPtr& operand, LogicalType type) {
  return std::make_shared<const plan::Expr>(
      plan::Expr{.node = plan::NegateExpr{.operand = operand}, .type = type, .name = "n"});
}
plan::ExprPtr ThroughInt128(ArithOp op, const plan::ExprPtr& left, const plan::ExprPtr& right,
                            LogicalType type) {
  return Arith(op, left, Negated(Negated(right, right->type), right->type), type);
}

// Every value a narrow step may legitimately hold fits an int64, and every value it may not must
// behave as the Int128 path behaves: the boundaries, one past them, a rescale that leaves int64,
// and a column holding what its declared width does not admit.
TEST_F(ComputeTest, ANarrowStepAgreesWithTheInt128PathAtItsBoundaries) {
  struct Case {
    std::string what;
    LogicalType operand;
    LogicalType result;
    ArithOp op;
    std::string left;
    std::string right;
  };
  const LogicalType d18 = LogicalType::Decimal(18, 0);
  const LogicalType d18s = LogicalType::Decimal(18, 2);
  const std::string max18 = "999999999999999999";  // 10^18 - 1, inside an int64
  const std::vector<Case> cases = {
      {.what = "ordinary values",
       .operand = d18,
       .result = d18,
       .op = ArithOp::kAdd,
       .left = "1234",
       .right = "5678"},
      {.what = "the result exactly at the range maximum",
       .operand = d18,
       .result = d18,
       .op = ArithOp::kAdd,
       .left = "999999999999999998",
       .right = "1"},
      {.what = "one past the range maximum",
       .operand = d18,
       .result = d18,
       .op = ArithOp::kAdd,
       .left = max18,
       .right = "1"},
      {.what = "the negative boundary",
       .operand = d18,
       .result = d18,
       .op = ArithOp::kSubtract,
       .left = "-999999999999999998",
       .right = "1"},
      {.what = "a product past the range",
       .operand = d18,
       .result = d18,
       .op = ArithOp::kMultiply,
       .left = "1000000000",
       .right = "1000000000"},
      // A column holding more than its declared width, which no int64 holds either: the operand
      // test has to send this row to the Int128 path rather than truncating it.
      // The operand type is 38 digits where the step's is 18: a p<=18 column cannot hold a value
      // past int64 once it is 8 bytes wide, so a wider OPERAND is how this stays a real shape at
      // both physical widths rather than a row that stops running when the default flips. The step
      // is still narrowed -- that follows the RESULT's range -- so the operand test is what has to
      // send this row to the Int128 path (ADR 0021's decimal64 item).
      {.what = "an operand past the step's width and past int64",
       .operand = LogicalType::Decimal(38, 0),
       .result = d18,
       .op = ArithOp::kAdd,
       .left = "99999999999999999999999999999",
       .right = "1"},
      // 2^64 + 5, whose low 64 bits are 5. The case above does not pin the operand test on its
      // own: truncating that value happens to land outside the step's range, so the range test
      // rejects it either way and a truncating read would pass unnoticed. This one truncates to a
      // value well INSIDE the range, so only a real test of whether it fits an int64 keeps it an
      // error rather than an answer of 6.
      {.what = "a value whose low 64 bits are inside the range",
       .operand = LogicalType::Decimal(38, 0),
       .result = d18,
       .op = ArithOp::kAdd,
       .left = "18446744073709551621",
       .right = "1"},
      // A rescale to scale 2 multiplies by 100, which leaves an int64 for a large operand.
      {.what = "a rescale that leaves int64",
       .operand = d18,
       .result = d18s,
       .op = ArithOp::kAdd,
       .left = max18,
       .right = "0"},
  };

  // At both representations: the narrow step is reached either way, and what changes is the width
  // its operands are READ from, which is a different arm of ReadDecimalOperand.
  for (const bool narrowing_on : {false, true}) {
    const NarrowDecimalsFor narrowing(narrowing_on);
    for (const Case& c : cases) {
      SCOPED_TRACE(std::format("narrowing {}: {}", narrowing_on, c.what));
      const auto left = Decimals(c.operand, {c.left});
      const auto right = Decimals(c.operand, {c.right});
      const auto l = ColumnAt(0, c.operand);
      const auto r = ColumnAt(1, c.operand);
      auto narrow = Eval(Arith(c.op, l, r, c.result), {left, right});
      auto wide = Eval(ThroughInt128(c.op, l, r, c.result), {left, right});
      ASSERT_EQ(narrow.ok(), wide.ok())
          << c.what
          << ": one path answered and the other did not\nnarrow: " << narrow.status().ToString()
          << "\nwide: " << wide.status().ToString();
      if (narrow.ok()) {
        EXPECT_EQ(UnscaledText(**narrow), UnscaledText(**wide)) << c.what;
      } else {
        // The message names the operation and the values, so the two paths must raise the same one:
        // the narrow path is required to stand aside rather than raise on its own account.
        EXPECT_EQ(narrow.status().ToString(), wide.status().ToString()) << c.what;
      }
    }
  }
}

// A step whose result needs more than 18 digits keeps the Int128 path, and still answers.
TEST_F(ComputeTest, AStepPast18DigitsIsNotNarrowed) {
  const LogicalType d38 = LogicalType::Decimal(38, 0);
  const std::string big = "12345678901234567890123456789";  // 29 digits, no int64 holds it
  const auto column = Decimals(d38, {big});
  auto sum = Eval(Arith(ArithOp::kAdd, ColumnAt(0, d38), ColumnAt(0, d38), d38), {column});
  ASSERT_TRUE(sum.ok()) << sum.status().ToString();
  EXPECT_EQ(UnscaledText(**sum), "DECIMAL(38,0): 24691357802469135780246913578");
}

// The guard every decimal read in exec goes through (src/exec/decimal_bytes.h). An array of
// another physical type must abort rather than be reinterpreted: a static_cast of a decimal64
// array to a Decimal128Array reads 8-byte values as 16-byte ones and says nothing, which is the
// failure ADR 0021's N3 would otherwise introduce in a release build.
TEST(DecimalBytesDeathTest, AnArrayOfAnotherPhysicalTypeIsNotReinterpreted) {
  const auto ints = Int64s({1, 2, 3});
  EXPECT_DEATH(AsDecimals(*ints), "check failed");
}

// And the type a builder is given is re-derived and checked, so a builder can never be constructed
// for a width this code does not write. Both callers build the 38-digit result of an exact SUM --
// HUGEINT or DECIMAL(38,s) -- which is decimal128 at either setting of the narrowing switch, so
// the helper's check is the permanent statement that this builder is for that type and no other.
TEST(DecimalBytesTest, TheBuilderTypeIsTheSumsOwn) {
  for (const bool narrowing_on : {false, true}) {
    const NarrowDecimalsFor narrowing(narrowing_on);
    SCOPED_TRACE(std::format("narrowing {}", narrowing_on));
    for (const LogicalType type :
         {LogicalType(LogicalType::kHugeInt), LogicalType::Decimal(38, 4)}) {
      EXPECT_TRUE(DecimalsType(type)->Equals(*arrow::decimal128(38, type.scale())))
          << ToString(type) << " gave " << DecimalsType(type)->ToString();
    }
  }
}

// A type the SUM builder is not for aborts rather than handing back a width this code would then
// write 16 bytes into. Only reachable with the switch on, which is what makes it worth pinning:
// without the check a release build would construct a Decimal128Builder over 8-byte slots.
TEST(DecimalBytesDeathTest, TheBuilderTypeRefusesANarrowDecimal) {
  const NarrowDecimalsFor narrowing(true);
  EXPECT_DEATH(DecimalsType(LogicalType::Decimal(15, 2)), "check failed");
}

// ---- the same program at the narrow physical width (ADR 0021's N3, the program path) ----

// ---- the same program at the narrow physical width (ADR 0021's N3, the program path) ----
// Both arms call the same Decimals, under NarrowDecimalsFor: with the helper width-aware the
// two arms are literally the same code at two settings, which is what these cases claim.

// The program's own shape -- a chain of DECIMAL arithmetic over columns -- at both widths. The
// operands differ in physical type and the result type differs with them, so what is compared is
// the unscaled values.
TEST_F(ComputeTest, ANarrowProgramAnswersAsTheWideOneDoes) {
  const LogicalType money = LogicalType::Decimal(15, 2);
  const LogicalType step1 = LogicalType::Decimal(16, 2);
  const LogicalType result = LogicalType::Decimal(18, 4);
  // The last price is there so that a product needs more than 32 bits: 999999999999 * 100 is
  // 99999999999900, and without it every value of this case fits an int32 and a narrow append that
  // truncated to one would answer correctly anyway.
  const std::vector<std::optional<std::string>> prices = {"1050025",    "200000", "0",
                                                          std::nullopt, "5",      "999999999999"};
  const std::vector<std::optional<std::string>> discounts = {"10", "0",          "500",
                                                             "7",  std::nullopt, "0"};
  // price * (1 - discount), which is the shape that made this item worth doing.
  const auto expr =
      Arith(ArithOp::kMultiply, ColumnAt(0, money),
            Arith(ArithOp::kSubtract, ConstantOf(100, money), ColumnAt(1, money), step1), result);

  std::string wide;
  {
    const NarrowDecimalsFor narrowing(false);
    auto out = Eval(expr, {Decimals(money, prices), Decimals(money, discounts)});
    ASSERT_TRUE(out.ok()) << out.status().ToString();
    EXPECT_EQ((*out)->type_id(), arrow::Type::DECIMAL128);
    wide = UnscaledText(**out);
  }
  {
    const NarrowDecimalsFor narrowing(true);
    auto out = Eval(expr, {Decimals(money, prices), Decimals(money, discounts)});
    ASSERT_TRUE(out.ok()) << out.status().ToString();
    EXPECT_EQ((*out)->type_id(), arrow::Type::DECIMAL64)
        << "a result of 18 digits must be built narrow with the switch on";
    // The type alone does not say the values were WRITTEN narrow -- it is whatever type the builder
    // was handed. Eight bytes a value is what says it, and a wide builder handed a narrow type
    // would reserve sixteen.
    EXPECT_EQ((*out)->data()->buffers[1]->size(),
              static_cast<std::int64_t>(sizeof(std::int64_t)) * (*out)->length())
        << "the values buffer is not eight bytes a row";
    EXPECT_EQ(UnscaledText(**out), wide) << "the narrow program answered differently";
  }
}

// A result past 18 digits stays decimal128 even with the switch on, and still answers: the width
// band is the result type's, not the switch's.
TEST_F(ComputeTest, AWideResultStaysWideWithNarrowingOn) {
  const NarrowDecimalsFor narrowing(true);
  const LogicalType d38 = LogicalType::Decimal(38, 0);
  const auto column = Decimals(d38, {"123456789012345678901234567"});
  auto out = Eval(Arith(ArithOp::kAdd, ColumnAt(0, d38), ColumnAt(0, d38), d38), {column});
  ASSERT_TRUE(out.ok()) << out.status().ToString();
  EXPECT_EQ((*out)->type_id(), arrow::Type::DECIMAL128);
  EXPECT_EQ(UnscaledText(**out), "DECIMAL(38,0): 246913578024691357802469134");
}

// The recursive fallbacks at the narrow width (ADR 0021's decimal64 item, step 2b). Each of these
// shapes makes Compile bail, so the program path is not involved: a negation, a CASE whose branch
// type is its own, a cast, and a chain whose operand the program cannot read.
TEST_F(ComputeTest, TheRecursiveFallbacksAnswerAtTheNarrowWidth) {
  const LogicalType money = LogicalType::Decimal(15, 2);
  const LogicalType wider = LogicalType::Decimal(16, 2);
  const std::vector<std::optional<std::string>> values = {"1050025", std::nullopt, "-7",
                                                          "999999999999"};

  // A negation keeps its operand's type, and Compile covers no NegateExpr, so this is the
  // recursive path at whatever width the type asks for.
  const auto negated = std::make_shared<const plan::Expr>(plan::Expr{
      .node = plan::NegateExpr{.operand = ColumnAt(0, money)}, .type = money, .name = "n"});
  // An arithmetic node whose operand is that negation: DecimalArith, not the program.
  const auto sum = Arith(ArithOp::kAdd, negated, ConstantOf(100, money), wider);

  for (const plan::ExprPtr& expr : {negated, sum}) {
    std::string wide;
    arrow::Type::type wide_id{};
    {
      const NarrowDecimalsFor narrowing(false);
      auto out = Eval(expr, {Decimals(money, values)});
      ASSERT_TRUE(out.ok()) << out.status().ToString();
      wide = UnscaledText(**out);
      wide_id = (*out)->type_id();
    }
    EXPECT_EQ(wide_id, arrow::Type::DECIMAL128);
    const NarrowDecimalsFor narrowing(true);
    auto out = Eval(expr, {Decimals(money, values)});
    ASSERT_TRUE(out.ok()) << out.status().ToString();
    EXPECT_EQ((*out)->type_id(), arrow::Type::DECIMAL64) << ToString(expr->type);
    EXPECT_EQ((*out)->data()->buffers[1]->size(),
              static_cast<std::int64_t>(sizeof(std::int64_t)) * (*out)->length())
        << "the values buffer is not eight bytes a row for " << ToString(expr->type);
    EXPECT_EQ(UnscaledText(**out), wide) << "the narrow fallback answered differently";
  }
}

// A DECIMAL cast to DOUBLE reads either width, and gives DuckDB's value either way.
TEST_F(ComputeTest, ADecimalToDoubleReadsEitherWidth) {
  const LogicalType money = LogicalType::Decimal(15, 2);
  const std::vector<std::optional<std::string>> values = {"1050025", std::nullopt, "-7"};
  // Each array under its own guard, and the physical types asserted to DIFFER: two arms built at
  // the same width would compare an array against itself and pass whatever ReadUnscaled did.
  const auto column = [&](bool narrowing_on) {
    const NarrowDecimalsFor narrowing(narrowing_on);
    return Decimals(money, values);
  };
  const auto wide_column = column(false);
  const auto narrow_column = column(true);
  ASSERT_EQ(wide_column->type_id(), arrow::Type::DECIMAL128);
  ASSERT_EQ(narrow_column->type_id(), arrow::Type::DECIMAL64);
  auto wide = DecimalToDouble(*wide_column, arrow::default_memory_pool());
  auto narrow = DecimalToDouble(*narrow_column, arrow::default_memory_pool());
  ASSERT_TRUE(wide.ok()) << wide.status().ToString();
  ASSERT_TRUE(narrow.ok()) << narrow.status().ToString();
  EXPECT_TRUE((*wide)->Equals(**narrow)) << (*wide)->ToString() << "\nagainst\n"
                                         << (*narrow)->ToString();
}

// A DOUBLE constant.
plan::ExprPtr DoubleOf(double value) {
  return std::make_shared<const plan::Expr>(plan::Expr{
      .node =
          plan::ConstantExpr{.value = plan::Constant{.type = LogicalType::kDouble, .value = value}},
      .type = LogicalType::kDouble,
      .name = "d"});
}

// Reaching that conversion is a separate thing from doing it right, and this is the test that fails
// when a DECIMAL expression typed DOUBLE converts the column with Arrow's cast instead: the case
// above calls DecimalToDouble itself, so it passes either way.
//
// The two conversions differ, and the case asserts that they still do, because the whole of this
// test rests on the one value below being one they disagree about (ADR 0021 rule 8).
TEST_F(ComputeTest, ADecimalConvertsToDoubleByDuckDbsRuleAtEitherWidth) {
  const LogicalType d18_4 = LogicalType::Decimal(18, 4);
  const std::vector<std::optional<std::string>> values = {"50005", std::nullopt, "-50005"};
  // 5.0005 is DuckDB's 5.0004999999999997 (the unscaled value divided by 10^4 in DOUBLE) and
  // Arrow's 5.0005000000000006 (multiplied by 10^-4). Without this the case proves nothing.
  {
    const NarrowDecimalsFor narrowing(false);
    auto arrows = arrow::compute::Cast(Decimals(d18_4, {"50005"}), arrow::float64());
    ASSERT_TRUE(arrows.ok()) << arrows.status().ToString();
    ASSERT_NE(static_cast<const arrow::DoubleArray&>(*arrows->make_array()).Value(0),
              DuckDbDecimalToDouble(50005, 18, 4))
        << "Arrow's cast now agrees with DuckDB's rule at this value: the case cannot tell the two "
           "conversions apart any more and needs another one";
  }
  // The shape the switch-on run failed on: a DECIMAL column in an expression typed DOUBLE, which
  // is what makes CastTo convert it. Times one, so the conversion is the whole of the answer.
  const auto expr =
      Arith(ArithOp::kMultiply, ColumnAt(0, d18_4), DoubleOf(1.0), LogicalType::kDouble);
  // Each arm names its width and the two are asserted to DIFFER: an unguarded wide arm inherits
  // whatever the default is, and in a build where it is on both arms would be the same array and
  // the comparison below would hold for any conversion at all.
  const auto column = [&](bool narrowing_on) {
    const NarrowDecimalsFor narrowing(narrowing_on);
    return Decimals(d18_4, values);
  };
  const auto wide_column = column(false);
  const auto narrow_column = column(true);
  ASSERT_EQ(wide_column->type_id(), arrow::Type::DECIMAL128);
  ASSERT_EQ(narrow_column->type_id(), arrow::Type::DECIMAL64);

  arrow::Result<std::shared_ptr<arrow::Array>> wide;
  {
    const NarrowDecimalsFor narrowing(false);
    wide = Eval(expr, {wide_column});
  }
  ASSERT_TRUE(wide.ok()) << wide.status().ToString();
  EXPECT_EQ(static_cast<const arrow::DoubleArray&>(**wide).Value(0),
            DuckDbDecimalToDouble(50005, 18, 4));
  const NarrowDecimalsFor narrowing(true);
  auto narrow = Eval(expr, {narrow_column});
  ASSERT_TRUE(narrow.ok()) << narrow.status().ToString();
  EXPECT_TRUE((*wide)->Equals(**narrow)) << (*wide)->ToString() << "\nagainst\n"
                                         << (*narrow)->ToString();
}

// An operand a rescale rewrote must be read from what the rescale wrote, at either width. The
// operand shapes that read an array in place -- 16 bytes a row for decimal128, 8 for decimal64 --
// are both cleared when the rescale copies the values out; leaving one set makes the arithmetic
// read the value the rescale replaced and answer with the wrong scale, silently.
//
// A modulo is the sharp shape: it rescales the narrower operand and then divides by it, so an
// un-rescaled divisor is 100x too small and the remainder is wrong rather than merely unscaled.
TEST_F(ComputeTest, ARescaledOperandIsReadRescaledAtEitherWidth) {
  const LogicalType money = LogicalType::Decimal(15, 2);  // the operand a rescale moves
  const LogicalType finer = LogicalType::Decimal(18, 4);  // the result's scale
  // 4.0004 % 10.50 is 4.0004: the dividend is smaller, so the answer is the dividend -- but only
  // if the divisor really was rescaled to 105000. Un-rescaled it is 1050, and the answer is 0.0024.
  const std::vector<std::optional<std::string>> dividend = {"40004", "-30003", std::nullopt};
  const std::vector<std::optional<std::string>> divisor = {"1050", "1050", "1050"};
  const auto expr = Arith(ArithOp::kModulo, ColumnAt(0, finer), ColumnAt(1, money), finer);

  for (const bool narrowing_on : {false, true}) {
    const NarrowDecimalsFor narrowing(narrowing_on);
    SCOPED_TRACE(std::format("narrowing {}", narrowing_on));
    const auto left = Decimals(finer, dividend);
    const auto right = Decimals(money, divisor);
    ASSERT_EQ(left->type_id(), narrowing_on ? arrow::Type::DECIMAL64 : arrow::Type::DECIMAL128);
    auto out = Eval(expr, {left, right});
    ASSERT_TRUE(out.ok()) << out.status().ToString();
    EXPECT_EQ(UnscaledText(**out), "DECIMAL(18,4): 40004 -30003 null");
  }
}

// A CASE over DECIMALs scatters each branch's values into the result, through a kernel Arrow has
// for decimal128 and not for decimal64 (ADR 0021's decimal64 item). Two branches and an ELSE, so
// the scatter runs more than once and over a result that already holds values.
TEST_F(ComputeTest, ANarrowDecimalCaseScattersAsTheWideOneDoes) {
  using Kind = plan::Predicate::Kind;
  const LogicalType money = LogicalType::Decimal(15, 2);
  const auto keys = Int16s({1, 2, 3, 4, 5, std::nullopt});
  const auto first = Condition(Kind::kCompare, plan::CompareOp::kLt, 3,
                               ColumnAt(0, LogicalType::kSmallInt));  // key < 3
  const auto second = Condition(Kind::kCompare, plan::CompareOp::kLt, 5,
                                ColumnAt(0, LogicalType::kSmallInt));  // key < 5
  const auto expr = std::make_shared<const plan::Expr>(
      plan::Expr{.node = plan::CaseExpr{.whens = {first, second},
                                        .thens = {ColumnAt(1, money), ColumnAt(2, money)},
                                        .otherwise = ColumnAt(3, money)},
                 .type = money,
                 .name = "case"});
  // A NULL in each branch's column, and a NULL key, whose conditions are both NULL and so take
  // neither branch -- it falls to the ELSE, which is the row that says the ELSE scatters too.
  const std::vector<std::optional<std::string>> a = {"1700", std::nullopt, "1", "2", "3", "4"};
  const std::vector<std::optional<std::string>> b = {"5", "6", "-25", std::nullopt, "7", "8"};
  const std::vector<std::optional<std::string>> c = {"9", "10", "11", "12", std::nullopt, "13"};

  std::string wide;
  {
    const NarrowDecimalsFor narrowing(false);
    auto out = Eval(expr, {keys, Decimals(money, a), Decimals(money, b), Decimals(money, c)});
    ASSERT_TRUE(out.ok()) << out.status().ToString();
    wide = UnscaledText(**out);
    EXPECT_EQ(wide, "DECIMAL(15,2): 1700 null -25 null null 13")
        << "the wide arm answered something else";
  }
  const NarrowDecimalsFor narrowing(true);
  auto out = Eval(expr, {keys, Decimals(money, a), Decimals(money, b), Decimals(money, c)});
  ASSERT_TRUE(out.ok()) << out.status().ToString();
  EXPECT_EQ((*out)->type_id(), arrow::Type::DECIMAL64);
  EXPECT_EQ((*out)->data()->buffers[1]->size(),
            static_cast<std::int64_t>(sizeof(std::int64_t)) * (*out)->length())
      << "the values buffer is not eight bytes a row: the scatter widened the result";
  EXPECT_EQ(UnscaledText(**out), wide) << "the narrow CASE answered differently";
}

}  // namespace
}  // namespace antb1::exec
