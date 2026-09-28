#include "antb1/exec/compute.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <gtest/gtest.h>

#include "antb1/common/int128.h"
#include "antb1/plan/literal.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/types.h"

#include "exec_test_util.h"

namespace antb1::exec {
namespace {

using plan::ArithOp;
using plan::LogicalType;
using testing::Int16s;
using testing::Int64s;

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

TEST_F(ComputeTest, ConstantsFillEveryRow) {
  auto out = Eval(ConstantOf(7, LogicalType::kInteger), {Int16s({1, 2, 3})});
  ASSERT_TRUE(out.ok());
  EXPECT_EQ((*out)->ToString(), "[\n  7,\n  7,\n  7\n]");
  auto missing = Eval(ColumnAt(3, LogicalType::kSmallInt), {Int16s({1})});
  EXPECT_TRUE(missing.status().IsInvalid());
}

}  // namespace
}  // namespace antb1::exec
