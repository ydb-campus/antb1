#include "antb1/plan/logical_plan.h"

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
#include "antb1/plan/table.h"
#include "antb1/plan/types.h"

#include "plan_test_util.h"

namespace antb1::plan {
namespace {

using testing::AllTypesSchema;
using testing::FakeTable;

// A table that does not split itself is one part: the whole table.
TEST(TableTest, WholeTableIsOnePartByDefault) {
  const FakeTable known(AllTypesSchema(), 100);
  EXPECT_EQ(known.num_parts(), 1);
  EXPECT_EQ(known.part_rows(0), 100);
  EXPECT_EQ(known.part_rows(1), std::nullopt);
  EXPECT_EQ(known.part_rows(-1), std::nullopt);
  const FakeTable unknown(AllTypesSchema(), std::nullopt);
  EXPECT_EQ(unknown.part_rows(0), std::nullopt);
  // Part 0 is Scan (which a FakeTable refuses); any other part is out of range.
  EXPECT_TRUE(known.ScanPart(0, {0}, 8).status().IsNotImplemented());
  EXPECT_TRUE(known.ScanPart(1, {0}, 8).status().IsInvalid());
  EXPECT_TRUE(known.ScanPart(-1, {0}, 8).status().IsInvalid());
}

TEST(LogicalPlanTest, OperatorAndAggregateNames) {
  EXPECT_EQ(ToString(CompareOp::kEq), "=");
  EXPECT_EQ(ToString(CompareOp::kNe), "<>");
  EXPECT_EQ(ToString(CompareOp::kLt), "<");
  EXPECT_EQ(ToString(CompareOp::kLe), "<=");
  EXPECT_EQ(ToString(CompareOp::kGt), ">");
  EXPECT_EQ(ToString(CompareOp::kGe), ">=");
  EXPECT_EQ(ToString(AggKind::kCountStar), "COUNT");
  EXPECT_EQ(ToString(AggKind::kCount), "COUNT");
  EXPECT_EQ(ToString(AggKind::kSum), "SUM");
  EXPECT_EQ(ToString(AggKind::kAvg), "AVG");
  EXPECT_EQ(ToString(AggKind::kMin), "MIN");
  EXPECT_EQ(ToString(AggKind::kMax), "MAX");
}

ExprPtr Column(int index, LogicalType type = LogicalType::kSmallInt) {
  return std::make_shared<const Expr>(
      Expr{.node = ColumnExpr{.index = index}, .type = type, .name = "c"});
}

// A column of the binder's and the optimizer's plans: one with an id.
ExprPtr ColumnWithId(std::uint32_t id, int index) {
  return std::make_shared<const Expr>(Expr{.node = ColumnExpr{.index = index, .id = ColumnId{id}},
                                           .type = LogicalType::kSmallInt,
                                           .name = "c"});
}

ExprPtr Condition(int index, CompareOp op, Int128 value) {
  Predicate p{.kind = Predicate::Kind::kCompare,
              .column = BoundColumn{.index = 0, .name = "o", .type = LogicalType::kSmallInt},
              .op = op,
              .constant = Constant{.type = LogicalType::kSmallInt, .value = value},
              .span = {}};
  return std::make_shared<const Expr>(
      Expr{.node = PredicateExpr{.predicate = std::move(p), .operands = {Column(index)}},
           .type = LogicalType::kBoolean,
           .name = "p"});
}

ExprPtr Bool(BoolOp op, std::vector<ExprPtr> args) {
  return std::make_shared<const Expr>(Expr{.node = BoolExpr{.op = op, .args = std::move(args)},
                                           .type = LogicalType::kBoolean,
                                           .name = "b"});
}

ExprPtr Case(std::vector<ExprPtr> whens, std::vector<ExprPtr> thens, ExprPtr otherwise) {
  return std::make_shared<const Expr>(Expr{.node = CaseExpr{.whens = std::move(whens),
                                                            .thens = std::move(thens),
                                                            .otherwise = std::move(otherwise)},
                                           .type = LogicalType::kSmallInt,
                                           .name = "case"});
}

// Structure, not names: conditions compare their predicate (kind, operator, constants) and
// operands; CASE its branches and ELSE (a missing one too); every child is mapped and read.
TEST(LogicalPlanTest, ConditionAndCaseExpressions) {
  const auto gt1 = Condition(0, CompareOp::kGt, 1);
  EXPECT_TRUE(SameExpr(*gt1, *Condition(0, CompareOp::kGt, 1)));
  EXPECT_FALSE(SameExpr(*gt1, *Condition(0, CompareOp::kGe, 1)));
  EXPECT_FALSE(SameExpr(*gt1, *Condition(0, CompareOp::kGt, 2)));
  EXPECT_FALSE(SameExpr(*gt1, *Condition(1, CompareOp::kGt, 1)));
  const auto any = Bool(BoolOp::kOr, {gt1, Condition(1, CompareOp::kLt, 0)});
  EXPECT_TRUE(SameExpr(*any, *Bool(BoolOp::kOr, {gt1, Condition(1, CompareOp::kLt, 0)})));
  EXPECT_FALSE(SameExpr(*any, *Bool(BoolOp::kAnd, {gt1, Condition(1, CompareOp::kLt, 0)})));
  EXPECT_FALSE(SameExpr(*any, *Bool(BoolOp::kOr, {gt1})));
  const auto with_else = Case({any}, {Column(2)}, Column(3));
  const auto without_else = Case({any}, {Column(2)}, nullptr);
  EXPECT_TRUE(SameExpr(*with_else, *Case({any}, {Column(2)}, Column(3))));
  EXPECT_FALSE(SameExpr(*with_else, *without_else));
  EXPECT_TRUE(SameExpr(*without_else, *Case({any}, {Column(2)}, nullptr)));
  EXPECT_FALSE(SameExpr(*with_else, *Case({gt1}, {Column(2)}, Column(3))));

  std::vector<int> read;
  CollectColumns(*with_else, read);
  EXPECT_EQ(read, (std::vector<int>{0, 1, 2, 3}));
  const auto mapped = MapColumns(without_else, [](ColumnExpr column) {
    column.index += 10;
    return column;
  });
  read.clear();
  CollectColumns(*mapped, read);
  EXPECT_EQ(read, (std::vector<int>{10, 11, 12}));
  EXPECT_EQ(std::get<CaseExpr>(mapped->node).otherwise, nullptr);
}

// Columns with ids are the same column when their ids are: their positions are not compared (they
// are set from the ids at the end of Bind and Optimize). Only columns without ids, those of the
// executor's positional plans, compare by position; a column with an id is never one without.
TEST(LogicalPlanTest, ColumnsAreTheSameByTheirIds) {
  EXPECT_TRUE(SameExpr(*ColumnWithId(7, 0), *ColumnWithId(7, 3)));
  EXPECT_FALSE(SameExpr(*ColumnWithId(7, 0), *ColumnWithId(8, 0)));
  EXPECT_FALSE(SameExpr(*ColumnWithId(7, 0), *Column(0)));
  EXPECT_FALSE(SameExpr(*Column(0), *ColumnWithId(7, 0)));
  EXPECT_TRUE(SameExpr(*Column(2), *Column(2)));
  EXPECT_FALSE(SameExpr(*Column(2), *Column(3)));
  const auto with_ids = Case({ColumnWithId(5, 0)}, {ColumnWithId(6, 1)}, ColumnWithId(7, 2));
  EXPECT_TRUE(
      SameExpr(*with_ids, *Case({ColumnWithId(5, 9)}, {ColumnWithId(6, 9)}, ColumnWithId(7, 9))));
  EXPECT_FALSE(
      SameExpr(*with_ids, *Case({ColumnWithId(5, 0)}, {ColumnWithId(6, 1)}, ColumnWithId(8, 2))));
}

// The ids of what an expression reads: a PredicateExpr's operands, never the operand-local columns
// of its predicate (which have no id).
TEST(LogicalPlanTest, CollectsTheIdsAnExpressionReads) {
  Predicate p{.kind = Predicate::Kind::kCompareColumns,
              .column = BoundColumn{.index = 0, .name = "o", .type = LogicalType::kSmallInt},
              .other = BoundColumn{.index = 1, .name = "p", .type = LogicalType::kSmallInt},
              .op = CompareOp::kLt,
              .span = {}};
  const auto condition = std::make_shared<const Expr>(
      Expr{.node = PredicateExpr{.predicate = std::move(p),
                                 .operands = {ColumnWithId(4, 0), ColumnWithId(9, 1)}},
           .type = LogicalType::kBoolean,
           .name = "p"});
  std::vector<ColumnId> ids;
  CollectColumnIds(*Case({condition}, {ColumnWithId(2, 5)}, ColumnWithId(4, 0)), ids);
  EXPECT_EQ(ids, (std::vector<ColumnId>{ColumnId{4}, ColumnId{9}, ColumnId{2}, ColumnId{4}}));
}

TEST(LogicalPlanTest, FunctionAndTypeNames) {
  EXPECT_EQ(ToString(Function::kStrlen), "strlen");
  EXPECT_EQ(ToString(Function::kRegexpReplace), "regexp_replace");
  EXPECT_EQ(ToString(Function::kEpochMs), "epoch_ms");
  EXPECT_EQ(ToString(Function::kExtract), "extract");
  EXPECT_EQ(ToString(Function::kDateTrunc), "date_trunc");
  EXPECT_EQ(ToString(LogicalType::kTimestamp), "TIMESTAMP");
  EXPECT_EQ(ToString(LogicalType::kBoolean), "BOOLEAN");
  EXPECT_TRUE(ToArrow(LogicalType::kTimestamp)->Equals(*arrow::timestamp(arrow::TimeUnit::MICRO)));
  EXPECT_TRUE(ToArrow(LogicalType::kBoolean)->Equals(*arrow::boolean()));
  // Neither is a column type: a Parquet timestamp or boolean column stays unsupported.
  EXPECT_FALSE(FromArrow(*arrow::timestamp(arrow::TimeUnit::MICRO)).ok());
  EXPECT_FALSE(FromArrow(*arrow::boolean()).ok());
  auto timestamp = ToArrowScalar(Constant{.type = LogicalType::kTimestamp, .value = Int128{-1}});
  ASSERT_TRUE(timestamp.ok()) << timestamp.status().ToString();
  EXPECT_EQ(static_cast<const arrow::TimestampScalar&>(**timestamp).value, -1);
  EXPECT_EQ(ToString(Constant{.type = LogicalType::kTimestamp, .value = Int128{-1}}),
            "TIMESTAMP '1969-12-31 23:59:59.999999'");
  EXPECT_FALSE(ToArrowScalar(Constant{.type = LogicalType::kTimestamp, .value = kInt128Max}).ok());
}

TEST(LogicalPlanTest, ConstantText) {
  EXPECT_EQ(ToString(Constant{.type = LogicalType::kSmallInt, .value = Int128{-7}}), "-7");
  EXPECT_EQ(ToString(Constant{.type = LogicalType::kHugeInt, .value = kInt128Min}),
            "-170141183460469231731687303715884105728");
  EXPECT_EQ(ToString(Constant{.type = LogicalType::kDate, .value = Int128{19000}}),
            "DATE '2022-01-08'");
  EXPECT_EQ(ToString(Constant{.type = LogicalType::kDate, .value = kInt128Max}),
            "DATE <170141183460469231731687303715884105727 days>");
  EXPECT_EQ(ToString(Constant{.type = LogicalType::kDouble, .value = 1.5}), "1.5");
  EXPECT_EQ(ToString(Constant{.type = LogicalType::kDouble, .value = 0.1}), "0.1");
  EXPECT_EQ(ToString(Constant{.type = LogicalType::kDouble, .value = -0.0}), "-0");
  EXPECT_EQ(ToString(Constant{.type = LogicalType::kDouble,
                              .value = std::numeric_limits<double>::infinity()}),
            "inf");
  EXPECT_EQ(ToString(Constant{.type = LogicalType::kVarchar, .value = std::string("O'Brien")}),
            "'O''Brien'");
  EXPECT_EQ(ToString(Constant{.type = LogicalType::kVarchar,
                              .value = std::string("a\tb\\c\x7f\xC3\xA9\0", 9)}),
            R"('a\x09b\x5Cc\x7F\xC3\xA9\x00')");
  EXPECT_EQ(ToString(Constant{.type = LogicalType::kVarchar, .value = std::string()}), "''");
  // A DECIMAL is its unscaled value printed with the type's scale, as DuckDB prints it.
  EXPECT_EQ(ToString(Constant{.type = LogicalType::Decimal(15, 2), .value = Int128{-25}}), "-0.25");
  EXPECT_EQ(ToString(Constant{.type = LogicalType::Decimal(15, 2), .value = Int128{1700}}),
            "17.00");
  EXPECT_EQ(ToString(Constant{.type = LogicalType::Decimal(3, 3), .value = Int128{500}}), ".500");
  EXPECT_EQ(ToString(Constant{.type = LogicalType::Decimal(38, 0), .value = Int128{-7}}), "-7");
}

TEST(LogicalPlanTest, ArrowScalars) {
  const auto scalar = [](const Constant& c) {
    auto s = ToArrowScalar(c);
    EXPECT_TRUE(s.ok()) << s.status().ToString();
    return s.ok() ? *s : nullptr;
  };
  auto s = scalar({.type = LogicalType::kSmallInt, .value = Int128{-32768}});
  ASSERT_NE(s, nullptr);
  EXPECT_TRUE(s->Equals(arrow::Int16Scalar(-32768)));
  s = scalar({.type = LogicalType::kInteger, .value = Int128{7}});
  ASSERT_NE(s, nullptr);
  EXPECT_TRUE(s->Equals(arrow::Int32Scalar(7)));
  s = scalar({.type = LogicalType::kBigInt, .value = Int128{std::numeric_limits<int64_t>::max()}});
  ASSERT_NE(s, nullptr);
  EXPECT_TRUE(s->Equals(arrow::Int64Scalar(std::numeric_limits<int64_t>::max())));
  s = scalar({.type = LogicalType::kUSmallInt, .value = Int128{65535}});
  ASSERT_NE(s, nullptr);
  EXPECT_TRUE(s->Equals(arrow::UInt16Scalar(65535)));
  s = scalar({.type = LogicalType::kDate, .value = Int128{15887}});
  ASSERT_NE(s, nullptr);
  EXPECT_TRUE(s->Equals(arrow::Date32Scalar(15887)));
  s = scalar({.type = LogicalType::kDouble, .value = -2.5});
  ASSERT_NE(s, nullptr);
  EXPECT_TRUE(s->Equals(arrow::DoubleScalar(-2.5)));
  s = scalar({.type = LogicalType::kVarchar, .value = std::string("x\0y", 3)});
  ASSERT_NE(s, nullptr);
  EXPECT_TRUE(s->Equals(arrow::BinaryScalar(arrow::Buffer::FromString(std::string("x\0y", 3)))));
  const Int128 big = -static_cast<Int128>(UInt128{1} << 100U);
  s = scalar({.type = LogicalType::kHugeInt, .value = big});
  ASSERT_NE(s, nullptr);
  ASSERT_TRUE(s->type->Equals(*arrow::decimal128(38, 0)));
  EXPECT_EQ(static_cast<const arrow::Decimal128Scalar&>(*s).value.ToIntegerString(),
            Int128ToString(big));
  // A DECIMAL constant carries its own (p,s), so it compares with the column at the same scale.
  s = scalar({.type = LogicalType::Decimal(15, 2), .value = Int128{-1234}});
  ASSERT_NE(s, nullptr);
  ASSERT_TRUE(s->type->Equals(*arrow::decimal128(15, 2)));
  EXPECT_EQ(static_cast<const arrow::Decimal128Scalar&>(*s).value.ToIntegerString(), "-1234");
}

TEST(LogicalPlanTest, ArrowScalarErrors) {
  const auto invalid = [](const Constant& c) { return ToArrowScalar(c).status().IsInvalid(); };
  EXPECT_TRUE(invalid({.type = LogicalType::kSmallInt, .value = Int128{32768}}));
  EXPECT_TRUE(invalid({.type = LogicalType::kUSmallInt, .value = Int128{-1}}));
  EXPECT_TRUE(invalid({.type = LogicalType::kBigInt, .value = kInt128Max}));
  EXPECT_TRUE(invalid({.type = LogicalType::kHugeInt, .value = kInt128Max}));
  // A DECIMAL(p,s) holds at most p digits: 10^15 is outside DECIMAL(15,2).
  EXPECT_TRUE(
      invalid({.type = LogicalType::Decimal(15, 2), .value = Int128{1'000'000'000'000'000}}));
  EXPECT_FALSE(
      invalid({.type = LogicalType::Decimal(15, 2), .value = Int128{999'999'999'999'999}}));
  EXPECT_TRUE(
      invalid({.type = LogicalType::kDate, .value = Int128{1'099'511'627'776}}));  // 2^40 days
  EXPECT_TRUE(invalid({.type = LogicalType::kDouble, .value = Int128{1}}));
  EXPECT_TRUE(invalid({.type = LogicalType::kVarchar, .value = Int128{1}}));
  EXPECT_TRUE(invalid({.type = LogicalType::kBigInt, .value = 1.0}));
  EXPECT_TRUE(invalid({.type = LogicalType::kDouble, .value = std::string("1")}));
}

TEST(LogicalPlanTest, NodeNamesSpansAndInputs) {
  const auto table = std::make_shared<FakeTable>(testing::AllTypesSchema(), 1);
  const auto span = [](std::size_t offset) { return SourceSpan{.offset = offset, .length = 1}; };
  const LogicalNodePtr scan =
      std::make_shared<const LogicalNode>(ScanNode{.table = table, .span = span(1)});
  const LogicalNodePtr other =
      std::make_shared<const LogicalNode>(ScanNode{.table = table, .span = span(0)});
  const std::vector<std::pair<LogicalNode, std::string_view>> nodes = {
      {ScanNode{.table = table, .span = span(1)}, "Scan"},
      {FilterNode{.input = scan, .span = span(2)}, "Filter"},
      {ComputeNode{.input = scan, .span = span(3)}, "Compute"},
      {ProjectNode{.input = scan, .span = span(4)}, "Project"},
      {AggregateNode{.input = scan, .span = span(5)}, "Aggregate"},
      {GroupAggregateNode{.input = scan, .span = span(6)}, "GroupAggregate"},
      {SortNode{.input = scan, .span = span(7)}, "Sort"},
      {LimitNode{.input = scan, .span = span(8)}, "Limit"},
      {RowCountNode{.table = table, .span = span(9)}, "RowCount"},
      {JoinNode{.left = scan, .right = other, .span = span(10)}, "Join"},
  };
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    const auto& [node, name] = nodes[i];
    EXPECT_EQ(NodeName(node), name);
    EXPECT_EQ(SpanOf(node), span(i + 1)) << name;
    const std::vector<LogicalNodePtr> inputs = InputsOf(node);
    if (name == "Scan" || name == "RowCount") {
      EXPECT_TRUE(inputs.empty()) << name;
    } else if (name == "Join") {
      EXPECT_EQ(inputs, (std::vector<LogicalNodePtr>{scan, other})) << "left, then right";
    } else {
      EXPECT_EQ(inputs, std::vector<LogicalNodePtr>{scan}) << name;
    }

    // WithInputs: a copy of the node, its span and its other fields kept, over new inputs.
    std::vector<LogicalNodePtr> replaced;
    replaced.reserve(inputs.size());
    for (std::size_t k = 0; k < inputs.size(); ++k) {
      replaced.push_back(
          std::make_shared<const LogicalNode>(ScanNode{.table = table, .span = span(20 + k)}));
    }
    const LogicalNodePtr copy = WithInputs(node, replaced);
    ASSERT_NE(copy, nullptr) << name;
    EXPECT_EQ(NodeName(*copy), name);
    EXPECT_EQ(SpanOf(*copy), span(i + 1)) << name;
    EXPECT_EQ(InputsOf(*copy), replaced) << name;
  }
}

TEST(LogicalPlanDeathTest, WithInputsNeedsOneInputPerChild) {
  const auto table = std::make_shared<FakeTable>(testing::AllTypesSchema(), 1);
  const LogicalNodePtr scan = std::make_shared<const LogicalNode>(ScanNode{.table = table});
  EXPECT_DEATH(WithInputs(JoinNode{.left = scan, .right = scan}, {scan}), "inputs.size");
  EXPECT_DEATH(WithInputs(LimitNode{.input = scan}, {}), "inputs.size");
  EXPECT_DEATH(WithInputs(ScanNode{.table = table}, {scan}), "inputs.size");
}

TEST(LogicalPlanTest, JoinKindsAndBuildSidesHaveNames) {
  EXPECT_EQ(ToString(JoinKind::kInner), "INNER");
  EXPECT_EQ(ToString(JoinKind::kLeft), "LEFT");
  EXPECT_EQ(ToString(JoinKind::kSemi), "SEMI");
  EXPECT_EQ(ToString(JoinKind::kAnti), "ANTI");
  EXPECT_EQ(ToString(JoinKind::kNullAwareAnti), "NULL-AWARE ANTI");
  EXPECT_EQ(ToString(JoinKind::kOneRow), "ONE-ROW");
  EXPECT_EQ(ToString(BuildSide::kLeft), "left");
  EXPECT_EQ(ToString(BuildSide::kRight), "right");
}

// The defaults of plan::Table for scans with a filter: no table filters unless it says so; a scan
// without a filter is the plain scan.
TEST(TableTest, FilteredScansAreOptIn) {
  class KeepAll final : public ScanFilter {
   public:
    [[nodiscard]] const std::vector<int>& columns() const override { return columns_; }
    arrow::Status Apply(int /*column*/, const ScanValues& /*values*/, int64_t /*offset*/,
                        std::uint8_t* /*selected*/) const override {
      return arrow::Status::OK();
    }

   private:
    std::vector<int> columns_{0};
  };
  const testing::FakeTable table(testing::AllTypesSchema(), 10);
  EXPECT_FALSE(table.supports_scan_filter({0}));
  const auto plain = table.ScanPart(0, {0}, 10, arrow::default_memory_pool(), nullptr);
  EXPECT_TRUE(plain.status().IsNotImplemented());
  EXPECT_EQ(plain.status().message(), "fake tables cannot be scanned");  // forwarded to DoScan
  const auto filtered =
      table.ScanPart(0, {0}, 10, arrow::default_memory_pool(), std::make_shared<KeepAll>());
  EXPECT_TRUE(filtered.status().IsNotImplemented());
  EXPECT_EQ(filtered.status().message(), "this table cannot filter while it scans");
}

}  // namespace
}  // namespace antb1::plan
