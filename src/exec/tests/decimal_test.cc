#include "../decimal.h"

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
#include "antb1/exec/compute.h"
#include "antb1/exec/filter.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/types.h"

#include "exec_test_util.h"

namespace antb1::exec {
namespace {

using Kind = plan::Predicate::Kind;
using plan::CompareOp;
using plan::LogicalType;
using testing::Column;
using testing::Int16s;
using testing::Int64s;

class DecimalTest : public testing::ExecTest {};

std::shared_ptr<arrow::Array> Decimals(int precision, int scale,
                                       const std::vector<std::optional<Int128>>& unscaled) {
  arrow::Decimal128Builder builder(arrow::decimal128(precision, scale));
  for (const auto& v : unscaled) {
    // The builder does not check the precision: a file can hold such values too.
    EXPECT_TRUE((v.has_value() ? builder.Append(FromInt128(*v)) : builder.AppendNull()).ok());
  }
  return builder.Finish().ValueOrDie();
}

std::shared_ptr<arrow::Array> Doubles(const std::vector<std::optional<double>>& values) {
  return testing::ArrayOf<arrow::DoubleBuilder>(arrow::float64(), values);
}

std::vector<std::optional<bool>> BoolValues(const arrow::Array& array) {
  const auto& bools = static_cast<const arrow::BooleanArray&>(array);
  std::vector<std::optional<bool>> out;
  for (int64_t i = 0; i < bools.length(); ++i) {
    out.push_back(bools.IsNull(i) ? std::nullopt : std::optional(bools.Value(i)));
  }
  return out;
}

std::shared_ptr<arrow::RecordBatch> BatchOf(const arrow::ArrayVector& columns) {
  arrow::FieldVector fields;
  for (std::size_t i = 0; i < columns.size(); ++i) {
    fields.push_back(arrow::field("c" + std::to_string(i), columns[i]->type()));
  }
  return arrow::RecordBatch::Make(arrow::schema(fields), columns.front()->length(), columns);
}

// The three-valued result of one predicate over the batch, as PredicateEvaluator gives it (empty,
// with a test failure, if the predicate does not evaluate).
std::vector<std::optional<bool>> Evaluated(const plan::Predicate& predicate,
                                           const arrow::RecordBatch& batch) {
  auto evaluator = PredicateEvaluator::Make(predicate, *batch.schema());
  if (!evaluator.ok()) {
    ADD_FAILURE() << evaluator.status().ToString();
    return {};
  }
  auto result = evaluator->Evaluate(batch, arrow::default_memory_pool(), /*kleene=*/true);
  if (!result.ok()) {
    ADD_FAILURE() << result.status().ToString();
    return {};
  }
  return BoolValues(*result->make_array());
}

plan::Predicate Columns(plan::BoundColumn left, CompareOp op, plan::BoundColumn right) {
  return plan::Predicate{.kind = Kind::kCompareColumns,
                         .column = std::move(left),
                         .other = std::move(right),
                         .op = op,
                         .constant = {},
                         .values = {},
                         .span = {}};
}

plan::Constant Double(double v) { return plan::Constant{.type = LogicalType::kDouble, .value = v}; }

constexpr std::optional<bool> kNull = std::nullopt;

// DuckDB's conversion of a DECIMAL to DOUBLE (ADR 0021 rule 8) at the array's own width, values
// from DuckDB 1.5.5: beyond 2^53 it is not the nearest double, and a 128-bit value of -2^53 - 2
// becomes -2^53 through DuckDB's formula for an upper half of -1.
TEST_F(DecimalTest, DecimalToDoubleIsDuckDbsConversion) {
  const auto wide = Decimals(
      38, 10,
      {Int128{90071992547409935} * 1'000'000'000, std::nullopt, Int128{15} * 1'000'000'000});
  auto converted = DecimalToDouble(*wide, arrow::default_memory_pool());
  ASSERT_TRUE(converted.ok()) << converted.status().ToString();
  const auto& doubles = static_cast<const arrow::DoubleArray&>(**converted);
  EXPECT_EQ(doubles.Value(0), 0x1p53);
  EXPECT_TRUE(doubles.IsNull(1));
  EXPECT_EQ(doubles.Value(2), 1.5);
  const auto narrow = Decimals(18, 1, {Int128{199489722791016982}});
  EXPECT_EQ(static_cast<const arrow::DoubleArray&>(
                **DecimalToDouble(*narrow, arrow::default_memory_pool()))
                .Value(0),
            1.9948972279101696e+16);
  const auto hugeint = Decimals(38, 0, {Int128{-9007199254740994}, Int128{9007199254740994}});
  auto halves = DecimalToDouble(*hugeint, arrow::default_memory_pool());
  ASSERT_TRUE(halves.ok()) << halves.status().ToString();
  const auto& halves_doubles = static_cast<const arrow::DoubleArray&>(**halves);
  EXPECT_EQ(halves_doubles.Value(0), -0x1p53);
  EXPECT_EQ(halves_doubles.Value(1), 9007199254740994.0);
  EXPECT_TRUE(DecimalToDouble(*Int64s({1}), arrow::default_memory_pool()).status().IsInvalid());
}

// DECIMALs of different scales and integers compare exactly by value, never through a rounding
// cast, also beyond 38 digits where DuckDB fails (divergence D13) and at the Int128 extremes a
// file can hold; NULL gives NULL.
TEST_F(DecimalTest, CompareExactIsExactAcrossScalesAndIntegers) {
  const auto pool = arrow::default_memory_pool();
  const auto compare = [&](const arrow::Array& l, const arrow::Array& r, CompareOp op) {
    auto result = CompareExact(l, r, op, pool);
    EXPECT_TRUE(result.ok()) << result.status().ToString();
    return result.ok() ? BoolValues(**result) : std::vector<std::optional<bool>>{};
  };
  // DECIMAL(15,2) against DECIMAL(5,3): 1.23 = 1.230, 1.23 > 1.229, -1.23 > -1.231.
  const auto p = Decimals(15, 2, {123, 123, -123, std::nullopt, 0});
  const auto rate = Decimals(5, 3, {1230, 1229, -1231, 7, std::nullopt});
  EXPECT_EQ(compare(*p, *rate, CompareOp::kEq),
            (std::vector<std::optional<bool>>{true, false, false, kNull, kNull}));
  EXPECT_EQ(compare(*p, *rate, CompareOp::kGt),
            (std::vector<std::optional<bool>>{false, true, true, kNull, kNull}));
  EXPECT_EQ(compare(*rate, *p, CompareOp::kGe),
            (std::vector<std::optional<bool>>{true, false, false, kNull, kNull}));
  // A DECIMAL(38,10) against BIGINT: 2^63 - 1 + 10^-10 is above the BIGINT maximum.
  const Int128 scale10 = PowerOfTen(10);
  const auto z = Decimals(38, 10,
                          {(Int128{std::numeric_limits<int64_t>::max()} * scale10) + 1,
                           Int128{std::numeric_limits<int64_t>::min()} * scale10});
  const auto b = Int64s({std::numeric_limits<int64_t>::max(), std::numeric_limits<int64_t>::min()});
  EXPECT_EQ(compare(*z, *b, CompareOp::kGt), (std::vector<std::optional<bool>>{true, false}));
  EXPECT_EQ(compare(*b, *z, CompareOp::kEq), (std::vector<std::optional<bool>>{false, true}));
  // SMALLINT, USMALLINT and INTEGER sides, and a DECIMAL(38,0) (a HUGEINT array too).
  const auto small = Int16s({-5, 7});
  const auto halves = Decimals(3, 1, {-50, 71});
  EXPECT_EQ(compare(*small, *halves, CompareOp::kEq),
            (std::vector<std::optional<bool>>{true, false}));
  const auto u16 = testing::ArrayOf<arrow::UInt16Builder>(
      arrow::uint16(), std::vector<std::optional<uint16_t>>{65535, 0});
  EXPECT_EQ(compare(*u16, *halves, CompareOp::kLt),
            (std::vector<std::optional<bool>>{false, true}));
  const auto i32 = testing::ArrayOf<arrow::Int32Builder>(
      arrow::int32(), std::vector<std::optional<int32_t>>{-2147483647, 3});
  const auto hugeint = Decimals(38, 0, {Int128{-2147483647}, Int128{2}});
  EXPECT_EQ(compare(*i32, *hugeint, CompareOp::kNe),
            (std::vector<std::optional<bool>>{false, true}));
  // Past 38 digits: 10^38 - 1 against DECIMAL(38,10) values, and values beyond the declared width
  // up to the Int128 extremes, where scaling up overflows (the sign decides).
  const Int128 max38 = PowerOfTen(38) - 1;
  const auto d38_0 = Decimals(38, 0, {max38, -max38, kInt128Min, kInt128Max, 0});
  const auto d38_10 = Decimals(38, 10, {max38, max38, kInt128Min, kInt128Min, kInt128Max});
  EXPECT_EQ(compare(*d38_0, *d38_10, CompareOp::kGt),
            (std::vector<std::optional<bool>>{true, false, false, true, false}));
  EXPECT_EQ(compare(*d38_10, *d38_0, CompareOp::kLe),
            (std::vector<std::optional<bool>>{true, false, false, true, false}));
  const auto same = Decimals(38, 38, {kInt128Min});
  EXPECT_EQ(compare(*same, *same, CompareOp::kEq), (std::vector<std::optional<bool>>{true}));
  // Every operator.
  const auto one = Decimals(2, 1, {10, 10, 10});
  const auto ints = Int64s({0, 1, 2});
  for (const auto& [op, expected] : {
           std::pair{CompareOp::kEq, std::vector<std::optional<bool>>{false, true, false}},
           std::pair{CompareOp::kNe, std::vector<std::optional<bool>>{true, false, true}},
           std::pair{CompareOp::kLt, std::vector<std::optional<bool>>{false, false, true}},
           std::pair{CompareOp::kLe, std::vector<std::optional<bool>>{false, true, true}},
           std::pair{CompareOp::kGt, std::vector<std::optional<bool>>{true, false, false}},
           std::pair{CompareOp::kGe, std::vector<std::optional<bool>>{true, true, false}},
       }) {
    EXPECT_EQ(compare(*one, *ints, op), expected) << static_cast<int>(op);
  }
  EXPECT_TRUE(
      CompareExact(*one, *Doubles({1.0, 1.0, 1.0}), CompareOp::kEq, pool).status().IsInvalid());
  EXPECT_TRUE(CompareExact(*one, *ints->Slice(0, 2), CompareOp::kEq, pool).status().IsInvalid());
}

// Two columns: a DECIMAL against another DECIMAL or an integer compares exactly, against a DOUBLE
// in DOUBLE after DuckDB's conversion (a HUGEINT too, which Arrow's cast converted differently);
// mixed integer columns keep Arrow's kernels.
TEST_F(DecimalTest, PredicateEvaluatorComparesColumnsOfMixedTypes) {
  // DECIMAL(18,1) 19948972279101698.2 converts to 19948972279101696 (the nearest double would be
  // 19948972279101700); DECIMAL(38,10) 9007199254740993.5 to 2^53.
  const auto narrow = Decimals(18, 1, {Int128{199489722791016982}, std::nullopt});
  const auto wide = Decimals(38, 10, {Int128{90071992547409935} * 1'000'000'000, 0});
  const auto doubles = Doubles({1.9948972279101696e+16, 0x1p53});
  const auto batch = BatchOf({narrow, wide, doubles});
  const auto n = Column(0, "n", LogicalType::Decimal(18, 1));
  const auto w = Column(1, "w", LogicalType::Decimal(38, 10));
  const auto d = Column(2, "d", LogicalType::kDouble);
  EXPECT_EQ(Evaluated(Columns(n, CompareOp::kEq, d), *batch),
            (std::vector<std::optional<bool>>{true, kNull}));
  EXPECT_EQ(Evaluated(Columns(d, CompareOp::kLt, w), *batch),
            (std::vector<std::optional<bool>>{false, false}));
  EXPECT_EQ(Evaluated(Columns(n, CompareOp::kGt, w), *batch),
            (std::vector<std::optional<bool>>{true, kNull}));
  // HUGEINT (an integer SUM) -2^53 - 2 against the DOUBLE -2^53 - 2: DuckDB converts the HUGEINT
  // to -2^53, so = is false and > is true.
  const auto sums = BatchOf({Decimals(38, 0, {Int128{-9007199254740994}}),
                             Doubles({-9007199254740994.0}), Int64s({-9007199254740994})});
  const auto sum = Column(0, "s", LogicalType::kHugeInt);
  const auto avg = Column(1, "a", LogicalType::kDouble);
  EXPECT_EQ(Evaluated(Columns(sum, CompareOp::kEq, avg), *sums),
            (std::vector<std::optional<bool>>{false}));
  EXPECT_EQ(Evaluated(Columns(sum, CompareOp::kGt, avg), *sums),
            (std::vector<std::optional<bool>>{true}));
  // HUGEINT against BIGINT: exactly.
  EXPECT_EQ(Evaluated(Columns(sum, CompareOp::kEq, Column(2, "b", LogicalType::kBigInt)), *sums),
            (std::vector<std::optional<bool>>{true}));
  // SMALLINT against USMALLINT and INTEGER against BIGINT stay on Arrow's kernels.
  const auto ints =
      BatchOf({Int16s({-1, 5}),
               testing::ArrayOf<arrow::UInt16Builder>(
                   arrow::uint16(), std::vector<std::optional<uint16_t>>{65535, 5}),
               testing::ArrayOf<arrow::Int32Builder>(
                   arrow::int32(), std::vector<std::optional<int32_t>>{2147483647, 1}),
               Int64s({2147483648, 1})});
  EXPECT_EQ(Evaluated(Columns(Column(0, "i16", LogicalType::kSmallInt), CompareOp::kLt,
                              Column(1, "u16", LogicalType::kUSmallInt)),
                      *ints),
            (std::vector<std::optional<bool>>{true, false}));
  EXPECT_EQ(Evaluated(Columns(Column(2, "i32", LogicalType::kInteger), CompareOp::kLt,
                              Column(3, "i64", LogicalType::kBigInt)),
                      *ints),
            (std::vector<std::optional<bool>>{true, false}));
}

// A DOUBLE constant or IN list on a DECIMAL column compares in DOUBLE, the column converted as
// DuckDB converts it (ADR 0021 rule 11): 9007199254740993.5 equals 2^53, -9007199254740994 in 128
// bits equals -2^53. NOT IN is NULL for NULL. A DOUBLE constant on any other column, and an IN
// list that mixes DOUBLE values with others, are rejected.
TEST_F(DecimalTest, PredicateEvaluatorComparesDecimalsWithDoubles) {
  const auto values = Decimals(
      38, 10,
      {Int128{90071992547409935} * 1'000'000'000, Int128{-9007199254740994} * PowerOfTen(10),
       std::nullopt, Int128{15} * 1'000'000'000});
  const auto batch = BatchOf({values, Decimals(38, 0, {1, 2, 3, 4})});
  const auto w = Column(0, "w", LogicalType::Decimal(38, 10));
  const auto compare = [&](CompareOp op, double constant) {
    return Evaluated(testing::Compare(w, op, Double(constant)), *batch);
  };
  EXPECT_EQ(compare(CompareOp::kEq, 0x1p53),
            (std::vector<std::optional<bool>>{true, false, kNull, false}));
  EXPECT_EQ(compare(CompareOp::kGt, 0x1p53),
            (std::vector<std::optional<bool>>{false, false, kNull, false}));
  EXPECT_EQ(compare(CompareOp::kEq, -0x1p53),
            (std::vector<std::optional<bool>>{false, true, kNull, false}));
  EXPECT_EQ(compare(CompareOp::kLe, 1.5),
            (std::vector<std::optional<bool>>{false, true, kNull, true}));
  const auto in = [&](bool negated) {
    plan::Predicate p{.kind = negated ? Kind::kNotIn : Kind::kIn, .column = w, .span = {}};
    p.values = {Double(0x1p53), Double(1.5)};
    return Evaluated(p, *batch);
  };
  EXPECT_EQ(in(false), (std::vector<std::optional<bool>>{true, false, kNull, true}));
  EXPECT_EQ(in(true), (std::vector<std::optional<bool>>{false, true, kNull, false}));
  // HUGEINT never gets a DOUBLE constant from the binder (divergence D7 folds it exactly).
  const auto hugeint = Column(1, "h", LogicalType::kHugeInt);
  EXPECT_TRUE(PredicateEvaluator::Make(testing::Compare(hugeint, CompareOp::kEq, Double(1.0)),
                                       *batch->schema())
                  .status()
                  .IsInvalid());
  plan::Predicate mixed{.kind = Kind::kIn, .column = w, .span = {}};
  mixed.values = {Double(1.5),
                  plan::Constant{.type = LogicalType::Decimal(38, 10), .value = Int128{1}}};
  EXPECT_TRUE(PredicateEvaluator::Make(mixed, *batch->schema()).status().IsInvalid());
  EXPECT_TRUE(
      PredicateEvaluator::Make(
          testing::Compare(w, CompareOp::kEq,
                           plan::Constant{.type = LogicalType::Decimal(15, 2), .value = Int128{1}}),
          *batch->schema())
          .status()
          .IsInvalid());
}

// The same comparisons inside an expression (CASE WHEN, OR): Compute evaluates each condition's
// operands into a batch of their own and prepares the predicate there.
TEST_F(DecimalTest, ConditionsInsideExpressions) {
  const auto operand = [](int index, LogicalType type) {
    return std::make_shared<const plan::Expr>(
        plan::Expr{.node = plan::ColumnExpr{.index = index}, .type = type, .name = "c"});
  };
  const auto condition = [](plan::Predicate predicate, std::vector<plan::ExprPtr> operands) {
    return std::make_shared<const plan::Expr>(plan::Expr{
        .node =
            plan::PredicateExpr{.predicate = std::move(predicate), .operands = std::move(operands)},
        .type = LogicalType::kBoolean,
        .name = "p"});
  };
  const auto p = Decimals(15, 2, {150, 199, std::nullopt});
  const auto rate = Decimals(5, 3, {1500, 2000, 1});
  const auto rows = BatchOf({p, rate});
  const LogicalType p_type = LogicalType::Decimal(15, 2);
  const LogicalType rate_type = LogicalType::Decimal(5, 3);
  // p = rate, exactly.
  const auto equal =
      condition(Columns(Column(0, "o0", p_type), CompareOp::kEq, Column(1, "o1", rate_type)),
                {operand(0, p_type), operand(1, rate_type)});
  // p IN (1e0, 1.99): a DOUBLE list.
  plan::Predicate in{.kind = Kind::kIn, .column = Column(0, "o0", p_type), .span = {}};
  in.values = {Double(1.0), Double(1.99)};
  const auto in_double = condition(in, {operand(0, p_type)});
  const auto either = std::make_shared<const plan::Expr>(
      plan::Expr{.node = plan::BoolExpr{.op = plan::BoolOp::kOr, .args = {equal, in_double}},
                 .type = LogicalType::kBoolean,
                 .name = "or"});
  auto result = EvaluateExpr(*either, *rows, arrow::default_memory_pool());
  ASSERT_TRUE(result.ok()) << result.status().ToString();
  EXPECT_EQ(BoolValues(**result), (std::vector<std::optional<bool>>{true, true, kNull}));
  // CASE WHEN p = rate THEN 1 ELSE 0 END.
  const auto one = [](int64_t v) {
    return std::make_shared<const plan::Expr>(
        plan::Expr{.node = plan::ConstantExpr{.value = plan::Constant{.type = LogicalType::kInteger,
                                                                      .value = Int128{v}}},
                   .type = LogicalType::kInteger,
                   .name = std::to_string(v)});
  };
  const auto case_expr = std::make_shared<const plan::Expr>(
      plan::Expr{.node = plan::CaseExpr{.whens = {equal}, .thens = {one(1)}, .otherwise = one(0)},
                 .type = LogicalType::kInteger,
                 .name = "case"});
  auto chosen = EvaluateExpr(*case_expr, *rows, arrow::default_memory_pool());
  ASSERT_TRUE(chosen.ok()) << chosen.status().ToString();
  EXPECT_EQ((*chosen)->ToString(), "[\n  1,\n  0,\n  0\n]");
}

}  // namespace
}  // namespace antb1::exec
