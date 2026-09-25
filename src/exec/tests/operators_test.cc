#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <gtest/gtest.h>

#include "antb1/exec/filter.h"
#include "antb1/exec/limit.h"
#include "antb1/exec/operator.h"
#include "antb1/exec/project.h"
#include "antb1/exec/scalar_aggregate.h"
#include "antb1/exec/table_scan.h"
#include "antb1/plan/logical_plan.h"

#include "exec_test_util.h"

namespace antb1::exec {
namespace {

using plan::CompareOp;
using plan::LogicalType;
using plan::Predicate;
using testing::BigInt;
using testing::Bools;
using testing::Column;
using testing::Compare;
using testing::Int64Column;
using testing::Int64s;
using testing::MemoryTable;
using testing::ScriptedSource;
using testing::SingleInt64;
using testing::Strings;

class OperatorsTest : public testing::ExecTest {
 protected:
  // Columns x (BIGINT, with NULLs) and s (VARCHAR), 6 rows in batches of 4 and 2.
  static std::shared_ptr<MemoryTable> Table() {
    const auto schema =
        arrow::schema({arrow::field("x", arrow::int64()), arrow::field("s", arrow::binary())});
    arrow::RecordBatchVector batches = {
        arrow::RecordBatch::Make(schema, 4,
                                 {Int64s({1, std::nullopt, 3, 4}), Strings({"a", "b", "", "d"})}),
        arrow::RecordBatch::Make(schema, 2, {Int64s({5, -6}), Strings({std::nullopt, "f"})}),
    };
    return std::make_shared<MemoryTable>(schema, std::move(batches));
  }

  static std::unique_ptr<Operator> Scan(const std::shared_ptr<MemoryTable>& table,
                                        std::vector<int> fields = {0, 1}) {
    return std::make_unique<TableScanOperator>(table, std::move(fields));
  }

  static std::shared_ptr<arrow::Table> Run(Operator& op, int64_t batch_size = 1024) {
    ExecContext ctx{.pool = arrow::default_memory_pool(), .batch_size = batch_size};
    auto table = Drain(op, ctx);
    EXPECT_TRUE(table.ok()) << table.status().ToString();
    return table.ok() ? *table : nullptr;
  }

  static std::unique_ptr<ScriptedSource> Source(std::vector<Batch> batches) {
    const auto schema = arrow::schema({arrow::field("x", arrow::int64())});
    return std::make_unique<ScriptedSource>(schema, std::move(batches));
  }

  static Batch Ints(const std::vector<std::optional<int64_t>>& values,
                    std::shared_ptr<arrow::BooleanArray> selection = nullptr) {
    const auto schema = arrow::schema({arrow::field("x", arrow::int64())});
    const auto n = static_cast<int64_t>(values.size());
    return Batch{.data = arrow::RecordBatch::Make(schema, n, {Int64s(values)}),
                 .selection = std::move(selection)};
  }
};

TEST_F(OperatorsTest, TableScanReadsTheFieldsInBatches) {
  const auto table = Table();
  TableScanOperator scan(table, {1, 0});
  EXPECT_EQ(scan.output_schema()->field(0)->name(), "s");
  ExecContext ctx{.pool = arrow::default_memory_pool(), .batch_size = 3};
  ASSERT_TRUE(scan.Open(ctx).ok());
  std::vector<int64_t> sizes;
  while (true) {
    auto batch = scan.Next();
    ASSERT_TRUE(batch.ok()) << batch.status().ToString();
    if (batch->end()) {
      break;
    }
    EXPECT_EQ(batch->selection, nullptr);
    sizes.push_back(batch->data->num_rows());
  }
  EXPECT_EQ(sizes, (std::vector<int64_t>{3, 1, 2}));
  ASSERT_TRUE(scan.Close().ok());
  ASSERT_TRUE(scan.Close().ok()) << "closing twice is fine";
  EXPECT_TRUE(scan.Next().status().IsInvalid()) << "Next() after Close()";
  // No fields: row counts only.
  TableScanOperator counts(table, {});
  const auto rows = Run(counts);
  EXPECT_EQ(rows->num_columns(), 0);
  EXPECT_EQ(rows->num_rows(), 6);
  // A bad field fails at Open.
  TableScanOperator bad(table, {7});
  EXPECT_TRUE(bad.Open(ctx).IsInvalid());
}

TEST_F(OperatorsTest, TableScanRenamesButNeverRetypes) {
  // A table whose batches carry other field names (e.g. an in-memory table) is renamed; other types
  // are an error.
  const auto declared = arrow::schema({arrow::field("x", arrow::int64())});
  const auto named_other = arrow::schema({arrow::field("y", arrow::int64())});
  const auto typed_other = arrow::schema({arrow::field("x", arrow::int16())});
  auto renamed = std::make_shared<MemoryTable>(
      declared, arrow::RecordBatchVector{arrow::RecordBatch::Make(named_other, 1, {Int64s({1})})});
  TableScanOperator scan(renamed, {0});
  EXPECT_EQ(Run(scan)->schema()->field(0)->name(), "x");
  auto retyped = std::make_shared<MemoryTable>(
      declared,
      arrow::RecordBatchVector{arrow::RecordBatch::Make(typed_other, 1, {testing::Int16s({1})})});
  TableScanOperator bad(retyped, {0});
  ExecContext ctx;
  EXPECT_FALSE(Drain(bad, ctx).ok());
}

TEST_F(OperatorsTest, FilterSelectsWithoutCopying) {
  FilterOperator filter(Scan(Table()),
                        {Compare(Column(0, "x", LogicalType::kBigInt), CompareOp::kGt, BigInt(2))});
  ExecContext ctx;
  ASSERT_TRUE(filter.Open(ctx).ok());
  auto first = filter.Next();
  ASSERT_TRUE(first.ok()) << first.status().ToString();
  ASSERT_NE(first->selection, nullptr);
  EXPECT_EQ(first->data->num_rows(), 4) << "the data is the input batch";
  EXPECT_EQ(first->selection->null_count(), 0) << "NULL comparisons are normalized to false";
  EXPECT_EQ(first->selected_rows(), 2);
  auto second = filter.Next();
  ASSERT_TRUE(second.ok());
  EXPECT_EQ(second->selected_rows(), 1);
  ASSERT_TRUE(filter.Next()->end());
  ASSERT_TRUE(filter.Close().ok());
}

TEST_F(OperatorsTest, FilterComparisonsAndNulls) {
  const auto x = Column(0, "x", LogicalType::kBigInt);
  const std::vector<std::pair<CompareOp, std::vector<std::optional<int64_t>>>> cases = {
      {CompareOp::kEq, {3}},        {CompareOp::kNe, {1, 4, 5, -6}}, {CompareOp::kLt, {1, -6}},
      {CompareOp::kLe, {1, 3, -6}}, {CompareOp::kGt, {4, 5}},        {CompareOp::kGe, {3, 4, 5}},
  };
  for (const auto& [op, expected] : cases) {
    FilterOperator filter(Scan(Table()), {Compare(x, op, BigInt(3))});
    EXPECT_EQ(Int64Column(*Run(filter)), expected) << plan::ToString(op);
  }
  // IS NOT NULL (a folded always-true comparison) keeps every non-NULL value.
  FilterOperator not_null(Scan(Table()), {Predicate{.kind = Predicate::Kind::kIsNotNull,
                                                    .column = x,
                                                    .op = CompareOp::kEq,
                                                    .constant = {},
                                                    .span = {}}});
  EXPECT_EQ(Int64Column(*Run(not_null)), (std::vector<std::optional<int64_t>>{1, 3, 4, 5, -6}));
}

TEST_F(OperatorsTest, FilterConjunctionOverColumnsOfEveryType) {
  const auto s = Column(1, "s", LogicalType::kVarchar);
  FilterOperator filter(
      Scan(Table()),
      {Compare(Column(0, "x", LogicalType::kBigInt), CompareOp::kGe, BigInt(1)),
       Compare(s, CompareOp::kLt, plan::Constant{.type = LogicalType::kVarchar, .value = "c"})});
  // x >= 1 AND s < 'c': 1/'a' passes, NULL/'b' and 5/NULL are rejected, 3/'' passes.
  EXPECT_EQ(Int64Column(*Run(filter)), (std::vector<std::optional<int64_t>>{1, 3}));
}

TEST_F(OperatorsTest, FilterSkipsEmptyBatchesAndDropsAllTrueSelections) {
  auto source = Source({Ints({1, 2}), Ints({7, 8}), Ints({9})});
  const auto* raw = source.get();
  FilterOperator filter(std::move(source),
                        {Compare(Column(0, "x", LogicalType::kBigInt), CompareOp::kGt, BigInt(5))});
  ExecContext ctx;
  ASSERT_TRUE(filter.Open(ctx).ok());
  auto batch = filter.Next();
  ASSERT_TRUE(batch.ok());
  EXPECT_EQ(batch->data->num_rows(), 2) << "the batch without a selected row was skipped";
  EXPECT_EQ(batch->selection, nullptr) << "every row passes";
  EXPECT_EQ(raw->pulls(), 2);
}

TEST_F(OperatorsTest, FilterCombinesAnIncomingSelection) {
  auto source = Source({Ints({1, 2, 3, std::nullopt}, Bools({false, true, true, true}))});
  FilterOperator filter(std::move(source),
                        {Compare(Column(0, "x", LogicalType::kBigInt), CompareOp::kNe, BigInt(3))});
  EXPECT_EQ(Int64Column(*Run(filter)), (std::vector<std::optional<int64_t>>{2}));
}

TEST_F(OperatorsTest, FalseFilterReadsNothing) {
  auto source = Source({Ints({1})});
  const auto* raw = source.get();
  FilterOperator filter(
      std::move(source),
      {Compare(Column(0, "x", LogicalType::kBigInt), CompareOp::kGt, BigInt(0)),
       Predicate{
           .kind = Predicate::Kind::kFalse, .column = {}, .op = {}, .constant = {}, .span = {}}});
  EXPECT_EQ(Run(filter)->num_rows(), 0);
  EXPECT_EQ(raw->pulls(), 0);
  EXPECT_EQ(raw->opens(), 1);
  EXPECT_EQ(raw->closes(), 1);
}

TEST_F(OperatorsTest, FilterWithoutPredicatesPassesBatchesOn) {
  FilterOperator filter(Scan(Table()), {});
  EXPECT_EQ(Run(filter)->num_rows(), 6);
}

TEST_F(OperatorsTest, FilterRejectsMalformedPredicatesAtOpen) {
  ExecContext ctx;
  FilterOperator outside(
      Scan(Table()), {Compare(Column(5, "x", LogicalType::kBigInt), CompareOp::kEq, BigInt(1))});
  EXPECT_TRUE(outside.Open(ctx).IsInvalid());
  FilterOperator retyped(
      Scan(Table()), {Compare(Column(1, "s", LogicalType::kVarchar), CompareOp::kEq, BigInt(1))});
  EXPECT_TRUE(retyped.Open(ctx).IsInvalid()) << "a BIGINT constant for a VARCHAR column";
  FilterOperator wrong_value(Scan(Table()),
                             {Compare(Column(0, "x", LogicalType::kBigInt), CompareOp::kEq,
                                      plan::Constant{.type = LogicalType::kBigInt, .value = 1.5})});
  EXPECT_TRUE(wrong_value.Open(ctx).IsInvalid());
}

TEST_F(OperatorsTest, ProjectReordersRepeatsAndMaterializes) {
  FilterOperator* filter_view = nullptr;
  auto filter = std::make_unique<FilterOperator>(
      Scan(Table()), std::vector<Predicate>{
                         Compare(Column(0, "x", LogicalType::kBigInt), CompareOp::kNe, BigInt(4))});
  filter_view = filter.get();
  ASSERT_NE(filter_view, nullptr);
  ProjectOperator project(std::move(filter), {1, 0, 0});
  ASSERT_EQ(project.output_schema()->num_fields(), 3);
  EXPECT_EQ(project.output_schema()->field(0)->name(), "s");
  const auto result = Run(project);
  EXPECT_EQ(result->num_rows(), 4);
  EXPECT_EQ(Int64Column(*result, 1), (std::vector<std::optional<int64_t>>{1, 3, 5, -6}));
  EXPECT_EQ(Int64Column(*result, 2), Int64Column(*result, 1));
  ExecContext ctx;
  ProjectOperator bad(Scan(Table()), {2});
  EXPECT_TRUE(bad.Open(ctx).IsInvalid());
}

TEST_F(OperatorsTest, LimitStopsPullingEarly) {
  for (const auto& [limit, rows, pulls] : std::vector<std::tuple<int64_t, int64_t, int>>{
           {0, 0, 0}, {1, 1, 1}, {2, 2, 1}, {3, 3, 2}, {4, 4, 2}, {5, 5, 3}, {9, 5, 4}}) {
    auto source = Source({Ints({1, 2}), Ints({3, 4}), Ints({5})});
    const auto* raw = source.get();
    LimitOperator op(std::move(source), limit);
    const auto result = Run(op);
    EXPECT_EQ(result->num_rows(), rows) << "LIMIT " << limit;
    EXPECT_EQ(raw->pulls(), pulls) << "LIMIT " << limit;
  }
  auto source = Source({Ints({1, 2, 3}, Bools({false, true, true})), Ints({4})});
  LimitOperator selected(std::move(source), 3);
  EXPECT_EQ(Int64Column(*Run(selected)), (std::vector<std::optional<int64_t>>{2, 3, 4}));
  ExecContext ctx;
  LimitOperator negative(Source({}), -1);
  EXPECT_TRUE(negative.Open(ctx).IsInvalid());
}

TEST_F(OperatorsTest, ScalarAggregateConsumesSelections) {
  const auto x = Column(0, "x", LogicalType::kBigInt);
  auto filter = std::make_unique<FilterOperator>(
      Scan(Table()), std::vector<Predicate>{Compare(x, CompareOp::kLt, BigInt(5))});
  ScalarAggregateOperator aggregate(
      std::move(filter),
      {plan::AggregateCall{
           .kind = plan::AggKind::kCountStar, .arg = {}, .type = LogicalType::kBigInt, .span = {}},
       plan::AggregateCall{
           .kind = plan::AggKind::kSum, .arg = x, .type = LogicalType::kHugeInt, .span = {}},
       plan::AggregateCall{.kind = plan::AggKind::kMax,
                           .arg = Column(1, "s", LogicalType::kVarchar),
                           .type = LogicalType::kVarchar,
                           .span = {}}});
  const auto result = Run(aggregate);
  ASSERT_EQ(result->num_rows(), 1);
  ASSERT_EQ(result->num_columns(), 3);
  EXPECT_EQ(SingleInt64(*result), 4);  // 1, 3, 4, -6
  EXPECT_EQ(result->column(1)->chunk(0)->GetScalar(0).ValueOrDie()->ToString(), "2");
  EXPECT_EQ(result->column(2)->chunk(0)->GetScalar(0).ValueOrDie()->ToString(), "f");
}

TEST_F(OperatorsTest, ScalarAggregateOverNoRows) {
  const auto x = Column(0, "x", LogicalType::kBigInt);
  ScalarAggregateOperator aggregate(
      Source({}),
      {plan::AggregateCall{
           .kind = plan::AggKind::kCount, .arg = x, .type = LogicalType::kBigInt, .span = {}},
       plan::AggregateCall{
           .kind = plan::AggKind::kAvg, .arg = x, .type = LogicalType::kDouble, .span = {}}});
  const auto result = Run(aggregate);
  ASSERT_EQ(result->num_rows(), 1);
  EXPECT_EQ(SingleInt64(*result), 0);
  EXPECT_TRUE(result->column(1)->chunk(0)->IsNull(0));
  ExecContext ctx;
  ScalarAggregateOperator unopened(
      Source({}),
      {plan::AggregateCall{
          .kind = plan::AggKind::kCountStar, .arg = {}, .type = LogicalType::kBigInt, .span = {}}});
  EXPECT_TRUE(unopened.Next().status().IsInvalid());
  ScalarAggregateOperator outside(Source({}),
                                  {plan::AggregateCall{.kind = plan::AggKind::kMin,
                                                       .arg = Column(3, "y", LogicalType::kBigInt),
                                                       .type = LogicalType::kBigInt,
                                                       .span = {}}});
  EXPECT_TRUE(outside.Open(ctx).IsInvalid());
  ScalarAggregateOperator mistyped(
      Source({}),
      {plan::AggregateCall{
          .kind = plan::AggKind::kSum, .arg = x, .type = LogicalType::kBigInt, .span = {}}});
  EXPECT_TRUE(mistyped.Open(ctx).IsInvalid());
}

TEST_F(OperatorsTest, DrainMaterializesAndClosesAfterFailures) {
  auto source = Source({Ints({1, 2, 3}, Bools({true, false, true})), Ints({4})});
  EXPECT_EQ(Int64Column(*Run(*source)), (std::vector<std::optional<int64_t>>{1, 3, 4}));
  auto failing = Source({Ints({1}), Ints({2})});
  failing->FailAt(1);
  ExecContext ctx;
  EXPECT_TRUE(Drain(*failing, ctx).status().IsIOError());
  EXPECT_EQ(failing->closes(), 1) << "Drain closes the operator after a failure too";
}

TEST_F(OperatorsTest, MaterializeKeepsRowCountsOfBatchesWithoutColumns) {
  const auto empty_schema = arrow::schema(arrow::FieldVector{});
  const Batch batch{.data = arrow::RecordBatch::Make(empty_schema, 3, arrow::ArrayVector{}),
                    .selection = Bools({true, false, true})};
  auto rows = Materialize(batch, arrow::default_memory_pool());
  ASSERT_TRUE(rows.ok());
  EXPECT_EQ((*rows)->num_rows(), 2);
  EXPECT_EQ(Batch{}.selected_rows(), 0);
  auto end = Materialize(Batch{}, arrow::default_memory_pool());
  ASSERT_TRUE(end.ok());
  EXPECT_EQ(*end, nullptr);
}

}  // namespace
}  // namespace antb1::exec
