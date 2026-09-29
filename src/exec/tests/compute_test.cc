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
#include <gtest/gtest.h>

#include "antb1/common/int128.h"
#include "antb1/common/narrow.h"
#include "antb1/exec/filter.h"
#include "antb1/exec/memory_budget.h"
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

}  // namespace
}  // namespace antb1::exec
