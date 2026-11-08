#include "antb1/plan/branch_truth.h"

#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <arrow/api.h>
#include <gtest/gtest.h>

#include "antb1/common/int128.h"
#include "antb1/exec/filter.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/types.h"

#include "exec_test_util.h"

namespace antb1::exec {
namespace {

using plan::BranchTruth;
using plan::CompareOp;
using plan::Constant;
using plan::LogicalType;
using plan::Predicate;

constexpr std::array kOps = {CompareOp::kEq, CompareOp::kNe, CompareOp::kLt,
                             CompareOp::kLe, CompareOp::kGt, CompareOp::kGe};

// A batch of one row holding `values`, one column each, of their types (plan::ToArrow); nullptr,
// with a test failure, for a value Arrow does not hold.
std::shared_ptr<arrow::RecordBatch> OneRow(const std::vector<Constant>& values) {
  arrow::FieldVector fields;
  arrow::ArrayVector columns;
  for (std::size_t i = 0; i < values.size(); ++i) {
    auto scalar = plan::ToArrowScalar(values[i]);
    if (!scalar.ok()) {
      ADD_FAILURE() << plan::ToString(values[i]) << ": " << scalar.status().ToString();
      return nullptr;
    }
    auto array = arrow::MakeArrayFromScalar(**scalar, 1);
    if (!array.ok()) {
      ADD_FAILURE() << plan::ToString(values[i]) << ": " << array.status().ToString();
      return nullptr;
    }
    fields.push_back(arrow::field("c" + std::to_string(i), (*array)->type()));
    columns.push_back(*array);
  }
  return arrow::RecordBatch::Make(arrow::schema(fields), 1, columns);
}

// Whether exec's PredicateEvaluator keeps the batch's one row: true only where the predicate is
// true (a NULL rejects the row, as a filter does).
bool Kept(const Predicate& predicate, const arrow::RecordBatch& batch) {
  auto evaluator = PredicateEvaluator::Make(predicate, *batch.schema());
  EXPECT_TRUE(evaluator.ok()) << evaluator.status().ToString();
  if (!evaluator.ok()) {
    return false;
  }
  auto result = evaluator->Evaluate(batch, arrow::default_memory_pool(), /*kleene=*/false);
  EXPECT_TRUE(result.ok()) << result.status().ToString();
  if (!result.ok()) {
    return false;
  }
  if (result->is_scalar()) {
    const auto& value = static_cast<const arrow::BooleanScalar&>(*result->scalar());
    return value.is_valid && value.value;
  }
  if (!result->is_array()) {
    ADD_FAILURE() << "a result of kind " << result->ToString();
    return false;
  }
  const std::shared_ptr<arrow::Array> array = result->make_array();
  const auto& bools = static_cast<const arrow::BooleanArray&>(*array);
  return bools.IsValid(0) && bools.Value(0);
}

plan::BoundColumn Col(int index, LogicalType type) {
  return plan::BoundColumn{.index = index,
                           .id = plan::ColumnId{static_cast<std::uint32_t>(index + 1)},
                           .name = "c" + std::to_string(index),
                           .type = type};
}

// What TruthOver decided against what the filter does with a row holding the constants: always
// is kept, never is dropped. Returns whether it decided at all.
bool Agrees(const Predicate& predicate, const std::vector<Constant>& row) {
  const std::optional<Constant> other =
      row.size() > 1 ? std::optional<Constant>(row[1]) : std::nullopt;
  const BranchTruth truth = plan::TruthOver(predicate, row[0], other);
  if (truth == BranchTruth::kUnknown) {
    return false;
  }
  const std::shared_ptr<arrow::RecordBatch> batch = OneRow(row);
  if (batch == nullptr) {
    return true;  // the failure is reported
  }
  EXPECT_EQ(Kept(predicate, *batch), truth == BranchTruth::kAlways)
      << plan::ToString(predicate.kind == Predicate::Kind::kCompare ? predicate.op : CompareOp::kEq)
      << " over " << plan::ToString(row[0]) << ": TruthOver says " << plan::ToString(truth);
  return true;
}

// The kernels need arrow::compute::Initialize, which ExecTest does.
class BranchTruthAgreesWithTheFilter : public testing::ExecTest {};

Constant Of(LogicalType type, Int128 value) { return Constant{.type = type, .value = value}; }

Constant Text(std::string value) {
  return Constant{.type = LogicalType::kVarchar, .value = std::move(value)};
}

// Values of each type that TruthOver decides, from the edges of its range to the middle.
std::vector<std::vector<Constant>> ValuesOfEveryType() {
  const auto of = [](LogicalType type, const std::vector<Int128>& values) {
    std::vector<Constant> out;
    out.reserve(values.size());
    for (const Int128 v : values) {
      out.push_back(Of(type, v));
    }
    return out;
  };
  const Int128 hugeint_max = PowerOfTen(38) - 1;
  return {
      of(LogicalType::kSmallInt, {-32'768, -1, 0, 1, 32'767}),
      of(LogicalType::kInteger,
         {std::numeric_limits<int32_t>::min(), -5, 0, 5, std::numeric_limits<int32_t>::max()}),
      of(LogicalType::kBigInt,
         {std::numeric_limits<int64_t>::min(), -5, 0, 5, std::numeric_limits<int64_t>::max()}),
      of(LogicalType::kUSmallInt, {0, 1, 65'535}),
      of(LogicalType::kHugeInt, {-hugeint_max, -1, 0, 1, hugeint_max}),
      of(LogicalType::Decimal(9, 2), {-999'999'999, -150, 0, 150, 999'999'999}),
      of(LogicalType::Decimal(30, 4),
         {-(PowerOfTen(30) - 1), -15'000, 0, 15'000, PowerOfTen(30) - 1}),
      of(LogicalType::kDate, {-719'528, 0, 19'000, 20'000}),
      of(LogicalType::kTimestamp, {-1, 0, 1, 1'700'000'000'000'000}),
      {Text(""), Text("a"), Text("a "), Text("b"), Text("z"), Text("XL"), Text("XL "), Text("\x7f"),
       Text("\x80"), Text("\xD0\x96"), Text(std::string("a\0b", 3))},
  };
}

// column <op> constant for every pair of values of a type and every operator.
TEST_F(BranchTruthAgreesWithTheFilter, ComparisonsWithAConstant) {
  for (const std::vector<Constant>& values : ValuesOfEveryType()) {
    SCOPED_TRACE(plan::ToString(values.front().type));
    int decided = 0;
    for (const Constant& value : values) {
      for (const Constant& constant : values) {
        for (const CompareOp op : kOps) {
          const Predicate predicate{.kind = Predicate::Kind::kCompare,
                                    .column = Col(0, value.type),
                                    .op = op,
                                    .constant = constant};
          decided += Agrees(predicate, {value}) ? 1 : 0;
        }
      }
    }
    EXPECT_EQ(decided, static_cast<int>(values.size() * values.size() * kOps.size()))
        << "every comparison of a type with itself is decided";
  }
}

// column [NOT] IN (two values) for every value and pair of values of a type.
TEST_F(BranchTruthAgreesWithTheFilter, InLists) {
  for (const std::vector<Constant>& values : ValuesOfEveryType()) {
    SCOPED_TRACE(plan::ToString(values.front().type));
    for (const Constant& value : values) {
      for (std::size_t a = 0; a < values.size(); ++a) {
        for (std::size_t b = a; b < values.size(); ++b) {
          for (const Predicate::Kind kind : {Predicate::Kind::kIn, Predicate::Kind::kNotIn}) {
            const Predicate predicate{
                .kind = kind, .column = Col(0, value.type), .values = {values[a], values[b]}};
            EXPECT_TRUE(Agrees(predicate, {value}));
          }
        }
      }
    }
  }
}

// column <op> other for every pair of values of a type as two columns of that type, and for pairs
// of numbers of different types or scales, compared by value: every comparison is decided.
TEST_F(BranchTruthAgreesWithTheFilter, ComparisonsOfTwoColumns) {
  std::vector<std::pair<std::vector<Constant>, std::vector<Constant>>> pairs;
  for (const std::vector<Constant>& values : ValuesOfEveryType()) {
    pairs.emplace_back(values, values);
  }
  const auto of = [](LogicalType type, const std::vector<Int128>& values) {
    std::vector<Constant> out;
    out.reserve(values.size());
    for (const Int128 v : values) {
      out.push_back(Of(type, v));
    }
    return out;
  };
  const Int128 hugeint_max = PowerOfTen(38) - 1;
  const Int128 int64_min = std::numeric_limits<int64_t>::min();
  const Int128 int64_max = std::numeric_limits<int64_t>::max();
  const Int128 int32_min = std::numeric_limits<int32_t>::min();
  const Int128 int32_max = std::numeric_limits<int32_t>::max();
  const LogicalType d122 = LogicalType::Decimal(12, 2);
  const LogicalType d92 = LogicalType::Decimal(9, 2);
  pairs.emplace_back(of(LogicalType::kInteger, {-1, 1, 2}), of(d122, {-100, 100, 150, 200}));
  pairs.emplace_back(of(LogicalType::Decimal(3, 2), {149, 150, 151}),
                     of(LogicalType::Decimal(2, 1), {14, 15, 16}));
  pairs.emplace_back(of(LogicalType::Decimal(30, 4), {-15'000, 15'000, PowerOfTen(30) - 1}),
                     of(LogicalType::Decimal(38, 0), {-2, 1, 2, PowerOfTen(26)}));
  pairs.emplace_back(of(LogicalType::kSmallInt, {-32'768, -1, 0, 7, 32'767}),
                     of(LogicalType::kUSmallInt, {0, 7, 65'535}));
  pairs.emplace_back(of(LogicalType::kInteger, {int32_min, -5, 5, int32_max}),
                     of(LogicalType::kBigInt, {int64_min, -5, 5, int32_max, int64_max}));
  pairs.emplace_back(of(LogicalType::kHugeInt, {-hugeint_max, int64_min, -5, 5, hugeint_max}),
                     of(LogicalType::kBigInt, {int64_min, -5, 5, int64_max}));
  pairs.emplace_back(of(LogicalType::kUSmallInt, {0, 7, 65'535}),
                     of(d92, {0, 700, 6'553'500, 6'553'501}));
  int cases = 0;
  int decided = 0;
  for (const auto& [lefts, rights] : pairs) {
    SCOPED_TRACE(plan::ToString(lefts.front().type) + " against " +
                 plan::ToString(rights.front().type));
    for (const Constant& left : lefts) {
      for (const Constant& right : rights) {
        for (const CompareOp op : kOps) {
          const Predicate predicate{.kind = Predicate::Kind::kCompareColumns,
                                    .column = Col(0, left.type),
                                    .other = Col(1, right.type),
                                    .op = op};
          ++cases;
          decided += Agrees(predicate, {left, right}) ? 1 : 0;
        }
      }
    }
  }
  EXPECT_EQ(decided, cases) << "every comparison of two numbers, dates, timestamps or texts";
}

// column IS [NOT] DISTINCT FROM other (ADR 0037's OJ5b) for every pair of values of a type and its
// NULL, as two columns of that type, and for numbers of different types or scales with their NULLs:
// every pair is decided, NULL against NULL as not distinct and against a value as distinct.
TEST_F(BranchTruthAgreesWithTheFilter, DistinctFromOfTwoColumns) {
  std::vector<std::pair<std::vector<Constant>, std::vector<Constant>>> pairs;
  for (std::vector<Constant> values : ValuesOfEveryType()) {
    values.push_back(Constant{.type = values.front().type, .value = std::monostate{}});
    pairs.emplace_back(values, values);
  }
  const auto of = [](LogicalType type, const std::vector<Int128>& values) {
    std::vector<Constant> out;
    out.reserve(values.size() + 1);
    for (const Int128 v : values) {
      out.push_back(Of(type, v));
    }
    out.push_back(Constant{.type = type, .value = std::monostate{}});
    return out;
  };
  pairs.emplace_back(of(LogicalType::kInteger, {-1, 1, 2}),
                     of(LogicalType::Decimal(12, 2), {-100, 100, 150, 200}));
  pairs.emplace_back(of(LogicalType::Decimal(3, 2), {149, 150, 151}),
                     of(LogicalType::Decimal(2, 1), {14, 15, 16}));
  pairs.emplace_back(of(LogicalType::kSmallInt, {-32'768, -1, 0, 7, 32'767}),
                     of(LogicalType::kUSmallInt, {0, 7, 65'535}));
  int cases = 0;
  int decided = 0;
  for (const auto& [lefts, rights] : pairs) {
    SCOPED_TRACE(plan::ToString(lefts.front().type) + " against " +
                 plan::ToString(rights.front().type));
    for (const Constant& left : lefts) {
      for (const Constant& right : rights) {
        for (const Predicate::Kind kind :
             {Predicate::Kind::kIsDistinct, Predicate::Kind::kIsNotDistinct}) {
          const Predicate predicate{
              .kind = kind, .column = Col(0, left.type), .other = Col(1, right.type)};
          ++cases;
          decided += Agrees(predicate, {left, right}) ? 1 : 0;
        }
      }
    }
  }
  EXPECT_EQ(decided, cases) << "every pair of values and NULLs";
}

// DOUBLE is compared by the filter alone (TruthOver decides none): NaN is not distinct from NaN
// and -0.0 not from 0.0, as in DuckDB, where `=` follows IEEE (divergence D10); a DOUBLE with an
// integer or a DECIMAL compares in DOUBLE; NULL is not distinct from NULL only. Never NULL.
TEST_F(BranchTruthAgreesWithTheFilter, DistinctFromOfDoubles) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const auto dbl = [](double v) { return Constant{.type = LogicalType::kDouble, .value = v}; };
  const Constant null_double{.type = LogicalType::kDouble, .value = std::monostate{}};
  struct Case {
    Constant left;
    Constant right;
    bool not_distinct;
  };
  const std::vector<Case> cases = {
      {.left = dbl(nan), .right = dbl(nan), .not_distinct = true},
      {.left = dbl(-0.0), .right = dbl(0.0), .not_distinct = true},
      {.left = dbl(1.5), .right = dbl(1.5), .not_distinct = true},
      {.left = dbl(nan), .right = dbl(1.5), .not_distinct = false},
      {.left = dbl(nan), .right = null_double, .not_distinct = false},
      {.left = null_double, .right = null_double, .not_distinct = true},
      {.left = dbl(2.0), .right = Of(LogicalType::kInteger, 2), .not_distinct = true},
      {.left = Of(LogicalType::kBigInt, 3), .right = dbl(2.0), .not_distinct = false},
      {.left = dbl(1.5), .right = Of(LogicalType::Decimal(9, 2), 150), .not_distinct = true},
      {.left = Of(LogicalType::Decimal(9, 2), 151), .right = dbl(1.5), .not_distinct = false},
      {.left = Of(LogicalType::kInteger, 2),
       .right = Constant{.type = LogicalType::kDouble, .value = std::monostate{}},
       .not_distinct = false},
  };
  for (const Case& c : cases) {
    SCOPED_TRACE(plan::ToString(c.left) + " and " + plan::ToString(c.right));
    const std::shared_ptr<arrow::RecordBatch> batch = OneRow({c.left, c.right});
    ASSERT_NE(batch, nullptr);
    for (const Predicate::Kind kind :
         {Predicate::Kind::kIsDistinct, Predicate::Kind::kIsNotDistinct}) {
      const Predicate predicate{
          .kind = kind, .column = Col(0, c.left.type), .other = Col(1, c.right.type)};
      EXPECT_EQ(Kept(predicate, *batch),
                c.not_distinct == (kind == Predicate::Kind::kIsNotDistinct));
      // Never NULL, inside an expression too.
      auto evaluator = PredicateEvaluator::Make(predicate, *batch->schema());
      ASSERT_TRUE(evaluator.ok()) << evaluator.status().ToString();
      auto result = evaluator->Evaluate(*batch, arrow::default_memory_pool(), /*kleene=*/true);
      ASSERT_TRUE(result.ok()) << result.status().ToString();
      EXPECT_EQ(result->make_array()->null_count(), 0);
    }
  }
}

}  // namespace
}  // namespace antb1::exec
