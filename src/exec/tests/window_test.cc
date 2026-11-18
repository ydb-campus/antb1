#include "../window.h"

#include <array>
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

#include "antb1/exec/aggregate_state.h"
#include "antb1/exec/memory_budget.h"
#include "antb1/exec/operator.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/types.h"

#include "exec_test_util.h"

namespace antb1::exec {
namespace {

using plan::AggKind;
using plan::LogicalType;
using testing::Column;
using testing::ScriptedSource;
class WindowTest : public testing::ExecTest {};

plan::WindowCall Call(AggKind kind, LogicalType type, LogicalType result, bool partition = true) {
  plan::WindowCall call;
  call.aggregate = kind;
  if (kind != AggKind::kCountStar) {
    call.arguments = {Column(1, "v", type)};
  }
  if (partition) {
    call.partition_by = {Column(0, "k", LogicalType::kBigInt)};
  }
  call.frame = plan::WindowCall::Frame::kWholePartition;
  call.type = result;
  call.name = "w";
  return call;
}

TEST_F(WindowTest, AllAggregateTypesMatchScalarPartitionsAcrossBatchesAndMasks) {
  const std::vector<std::pair<LogicalType, std::shared_ptr<arrow::Array>>> inputs{
      {LogicalType::kSmallInt, testing::Int16s({3, -8, 11, std::nullopt, 5, std::nullopt})},
      {LogicalType::kInteger, testing::Int32s({3, -8, 11, std::nullopt, 5, std::nullopt})},
      {LogicalType::kBigInt, testing::Int64s({3, -8, 11, std::nullopt, 5, std::nullopt})},
      {LogicalType::kUSmallInt, testing::UInt16s({3, 8, 11, std::nullopt, 5, std::nullopt})},
      {LogicalType::kDouble,
       testing::ArrayOf<arrow::DoubleBuilder>(
           arrow::float64(),
           std::vector<std::optional<double>>{3.5, -8.0, 11.0, std::nullopt, 5.0, std::nullopt})},
      {LogicalType::kHugeInt,
       testing::Decimals(38, 0, {"3", "-8", "11", std::nullopt, "5", std::nullopt})},
      {LogicalType::Decimal(12, 2),
       testing::Decimals(12, 2, {"3", "-8", "11", std::nullopt, "5", std::nullopt})},
      {LogicalType::Decimal(30, 7),
       testing::Decimals(30, 7, {"3", "-8", "11", std::nullopt, "5", std::nullopt})},
      {LogicalType::kVarchar, testing::Strings({"a", "z", "bbb", std::nullopt, "c", std::nullopt})},
      {LogicalType::kDate, testing::Dates({3, -8, 11, std::nullopt, 5, std::nullopt})},
      {LogicalType::kTimestamp, testing::Timestamps({3, -8, 11, std::nullopt, 5, std::nullopt})}};
  for (const auto& [type, values] : inputs) {
    for (const auto kind : {AggKind::kCountStar, AggKind::kCount, AggKind::kSum, AggKind::kAvg,
                            AggKind::kMin, AggKind::kMax, AggKind::kStddevSamp, AggKind::kStddevPop,
                            AggKind::kVarSamp, AggKind::kVarPop}) {
      const bool numeric = type != LogicalType::kVarchar && type != LogicalType::kDate &&
                           type != LogicalType::kTimestamp && type != LogicalType::kBoolean;
      const bool temporal_avg =
          kind == AggKind::kAvg && (type == LogicalType::kDate || type == LogicalType::kTimestamp);
      if (!numeric && !temporal_avg && kind != AggKind::kCountStar && kind != AggKind::kCount &&
          kind != AggKind::kMin && kind != AggKind::kMax) {
        continue;
      }
      LogicalType result = LogicalType::kDouble;
      if (temporal_avg) {
        result = LogicalType::kTimestamp;
      } else if (kind == AggKind::kCount || kind == AggKind::kCountStar) {
        result = LogicalType::kBigInt;
      } else if (kind == AggKind::kMin || kind == AggKind::kMax) {
        result = type;
      } else if (kind == AggKind::kSum) {
        if (type.id() == LogicalType::kDecimal) {
          result = LogicalType::Decimal(38, type.scale());
        } else if (type != LogicalType::kDouble) {
          result = LogicalType::kHugeInt;
        }
      }
      SCOPED_TRACE(values->type()->ToString());
      SCOPED_TRACE(static_cast<int>(kind));
      auto schema =
          arrow::schema({arrow::field("k", arrow::int64()), arrow::field("v", values->type())});
      auto batch = arrow::RecordBatch::Make(
          schema, 6, {testing::Int64s({1, 2, 1, 2, std::nullopt, std::nullopt}), values});
      // Drop row 1, NULL selection drops row 5. Three partitions remain, one entirely NULL.
      std::vector<Batch> batches{
          {.data = batch->Slice(0, 3), .selection = testing::Bools({true, false, true})},
          {.data = batch->Slice(3, 3), .selection = testing::Bools({true, true, std::nullopt})}};
      auto source = std::make_unique<ScriptedSource>(schema, std::move(batches));
      WindowOperator op(std::move(source), {Call(kind, type, result)});
      ExecContext ctx;
      ctx.batch_size = 1;
      auto output = Drain(op, ctx);
      ASSERT_TRUE(output.ok()) << output.status();
      ASSERT_EQ((*output)->num_rows(), 4);
      auto combined = (*output)->CombineChunks().ValueOrDie();
      const auto& actual = combined->column(2)->chunk(0);
      const std::array<std::vector<std::optional<bool>>, 3> masks{
          std::vector<std::optional<bool>>{true, false, true, false, false, false},
          std::vector<std::optional<bool>>{false, false, false, true, false, false},
          std::vector<std::optional<bool>>{false, false, false, false, true, false}};
      std::array<std::shared_ptr<arrow::Array>, 3> expected;
      for (std::size_t i = 0; i < masks.size(); ++i) {
        auto state = MakeAggregateState(
            kind, kind == AggKind::kCountStar ? std::nullopt : std::optional(type), result);
        ASSERT_TRUE(state.ok()) << state.status();
        ASSERT_TRUE((*state)->Consume(*values, testing::Bools(masks[i]).get()).ok());
        expected[i] = (*state)->Finalize(ctx.pool).ValueOrDie();
      }
      for (int64_t row = 0; row < 4; ++row) {
        const auto group = row < 2 ? 0U : static_cast<std::size_t>(row - 1);
        EXPECT_TRUE(actual->Slice(row, 1)->ApproxEquals(expected[group]))
            << actual->ToString() << " expected " << expected[group]->ToString();
      }
    }
  }
}

TEST_F(WindowTest, MultipleSpecificationsOneReadAndReopen) {
  const auto schema =
      arrow::schema({arrow::field("k", arrow::int64()), arrow::field("v", arrow::int64())});
  const auto batch = arrow::RecordBatch::Make(
      schema, 4, {testing::Int64s({1, 2, 1, std::nullopt}), testing::Int64s({3, 4, 5, 6})});
  auto source = std::make_unique<ScriptedSource>(
      schema, std::vector<Batch>{{.data = batch, .selection = {}}});
  const auto* observed = source.get();
  WindowOperator op(std::move(source),
                    {Call(AggKind::kCountStar, LogicalType::kBigInt, LogicalType::kBigInt),
                     Call(AggKind::kMax, LogicalType::kBigInt, LogicalType::kBigInt, false),
                     Call(AggKind::kMin, LogicalType::kBigInt, LogicalType::kBigInt)});
  MemoryBudget budget(4 * 1024 * 1024);
  ExecContext ctx{.pool = &budget, .batch_size = 2, .budget = &budget};
  EXPECT_FALSE(op.Next().ok());
  for (int run = 1; run <= 2; ++run) {
    {
      auto result = Drain(op, ctx);
      ASSERT_TRUE(result.ok()) << result.status();
      EXPECT_EQ(testing::Int64Column(**result, 2),
                (std::vector<std::optional<int64_t>>{2, 1, 2, 1}));
      EXPECT_EQ(testing::Int64Column(**result, 3),
                (std::vector<std::optional<int64_t>>{6, 6, 6, 6}));
      EXPECT_EQ(testing::Int64Column(**result, 4),
                (std::vector<std::optional<int64_t>>{3, 4, 3, 6}));
    }
    EXPECT_EQ(observed->opens(), run);
    EXPECT_EQ(observed->closes(), run);
    EXPECT_EQ(observed->pulls(), run * 2);
    EXPECT_EQ(budget.bytes_allocated(), 0);
  }
  ASSERT_TRUE(op.Open(ctx).ok());
  ASSERT_TRUE(op.Next().ok());
  ASSERT_TRUE(op.Close().ok());
  ASSERT_TRUE(op.Close().ok());
  EXPECT_EQ(budget.bytes_allocated(), 0);
}

TEST_F(WindowTest, EmptyInputHasNoGlobalRowAndFailedInputReleasesMemory) {
  const auto schema =
      arrow::schema({arrow::field("k", arrow::int64()), arrow::field("v", arrow::int64())});
  const auto batch =
      arrow::RecordBatch::Make(schema, 2, {testing::Int64s({1, 1}), testing::Int64s({3, 4})});
  for (const auto& failure : {arrow::Status::OK(), arrow::Status::IOError("read failure"),
                              arrow::Status::Cancelled("cancelled input")}) {
    auto source = std::make_unique<ScriptedSource>(
        schema, std::vector<Batch>{
                    {.data = batch, .selection = testing::Bools({!failure.ok(), !failure.ok()})}});
    const auto* observed = source.get();
    if (!failure.ok()) {
      source->FailAt(1, failure);
    }
    WindowOperator op(std::move(source), {Call(AggKind::kCountStar, LogicalType::kBigInt,
                                               LogicalType::kBigInt, false)});
    MemoryBudget budget(1024 * 1024);
    ExecContext ctx{.pool = &budget, .budget = &budget};
    ASSERT_TRUE(op.Open(ctx).ok());
    auto result = op.Next();
    if (!failure.ok()) {
      EXPECT_EQ(result.status().code(), failure.code()) << result.status();
    } else {
      ASSERT_TRUE(result.ok()) << result.status();
      EXPECT_TRUE(result->end());
      EXPECT_TRUE(op.Next()->end());
    }
    EXPECT_EQ(observed->closes(), 1);
    EXPECT_EQ(budget.bytes_allocated(), 0);
    ASSERT_TRUE(op.Close().ok());
  }
}

TEST_F(WindowTest, ExternalRetainedBuffersAndMappingScratchRespectBudget) {
  const auto schema =
      arrow::schema({arrow::field("k", arrow::int64()), arrow::field("v", arrow::binary())});
  const auto batch = arrow::RecordBatch::Make(
      schema, 3,
      {testing::Int64s({1, 2, 1}), testing::Strings({std::string(8192, 'a'), "b", "c"})});
  bool succeeded = false;
  for (const int64_t limit :
       {int64_t{64}, int64_t{8192}, int64_t{16384}, int64_t{65536}, int64_t{1048576}}) {
    auto source = std::make_unique<ScriptedSource>(
        schema, std::vector<Batch>{{.data = batch, .selection = {}}});
    WindowOperator op(std::move(source),
                      {Call(AggKind::kMin, LogicalType::kVarchar, LogicalType::kVarchar)});
    MemoryBudget budget(limit);
    ExecContext ctx{.pool = &budget, .batch_size = 1, .budget = &budget};
    {
      const auto result = Drain(op, ctx);
      if (result.ok()) {
        EXPECT_EQ((*result)->num_rows(), 3);
        succeeded = true;
      } else {
        EXPECT_TRUE(result.status().IsOutOfMemory()) << result.status();
      }
    }
    EXPECT_EQ(budget.bytes_allocated(), 0);
    EXPECT_LE(budget.max_memory(), limit);
  }
  EXPECT_TRUE(succeeded);
}

TEST_F(WindowTest, DoublePartitionKeysNormalizeNaNsAndSignedZeros) {
  const auto keys = testing::ArrayOf<arrow::DoubleBuilder>(
      arrow::float64(), std::vector<std::optional<double>>{std::nan("1"), -0.0, std::nullopt,
                                                           std::nan("2"), 0.0, 3.0});
  const auto schema = arrow::schema({arrow::field("k", arrow::float64())});
  const auto batch = arrow::RecordBatch::Make(schema, 6, {keys});
  auto source = std::make_unique<ScriptedSource>(
      schema, std::vector<Batch>{{.data = batch->Slice(0, 3), .selection = {}},
                                 {.data = batch->Slice(3), .selection = {}}});
  auto call = Call(AggKind::kCountStar, LogicalType::kBigInt, LogicalType::kBigInt);
  call.partition_by = {Column(0, "k", LogicalType::kDouble)};
  WindowOperator op(std::move(source), {call});
  ExecContext ctx;
  const auto result = Drain(op, ctx);
  ASSERT_TRUE(result.ok()) << result.status();
  EXPECT_EQ(testing::Int64Column(**result, 1),
            (std::vector<std::optional<int64_t>>{2, 2, 1, 2, 2, 1}));
}

TEST_F(WindowTest, GroupIdsAndFinalizedChunksCrossTheBatchBoundary) {
  constexpr int64_t kRows = 65539;
  arrow::Int64Builder keys;
  for (int64_t i = 0; i < kRows; ++i) {
    ASSERT_TRUE(keys.Append(i).ok());
  }
  const auto schema = arrow::schema({arrow::field("k", arrow::int64())});
  const auto batch = arrow::RecordBatch::Make(schema, kRows, {keys.Finish().ValueOrDie()});
  auto source = std::make_unique<ScriptedSource>(
      schema, std::vector<Batch>{{.data = batch, .selection = {}},
                                 {.data = batch->Slice(65535), .selection = {}}});
  WindowOperator op(std::move(source),
                    {Call(AggKind::kCountStar, LogicalType::kBigInt, LogicalType::kBigInt)});
  ExecContext ctx;
  const auto result = Drain(op, ctx);
  ASSERT_TRUE(result.ok()) << result.status();
  const auto counts = testing::Int64Column(**result, 1);
  ASSERT_EQ(counts.size(), static_cast<std::size_t>(kRows + 4));
  for (std::size_t i = 0; i < counts.size(); ++i) {
    EXPECT_EQ(counts[i], i < 65535 ? 1 : 2) << i;
  }
}

TEST_F(WindowTest, FinalizeErrorsPrecedeFirstOutputAndAverageUsesItsOwnFinalizer) {
  const auto type = LogicalType::kHugeInt;
  const auto schema =
      arrow::schema({arrow::field("k", arrow::int64()), arrow::field("v", plan::ToArrow(type))});
  const std::string large = "6" + std::string(37, '0');
  const auto batch = arrow::RecordBatch::Make(
      schema, 3, {testing::Int64s({1, 2, 2}), testing::Decimals(38, 0, {"1", large, large})});
  for (const bool average : {false, true}) {
    auto source = std::make_unique<ScriptedSource>(
        schema, std::vector<Batch>{{.data = batch, .selection = {}}});
    WindowOperator op(std::move(source),
                      {Call(average ? AggKind::kAvg : AggKind::kSum, type,
                            average ? LogicalType::kDouble : LogicalType::kHugeInt)});
    MemoryBudget budget(std::nullopt);
    ExecContext ctx{.pool = &budget, .batch_size = 1, .budget = &budget};
    ASSERT_TRUE(op.Open(ctx).ok());
    {
      auto first = op.Next();
      if (average) {
        ASSERT_TRUE(first.ok()) << first.status();
        EXPECT_FALSE(first->end());
      } else {
        EXPECT_TRUE(first.status().IsExecutionError()) << first.status();
        EXPECT_EQ(budget.bytes_allocated(), 0);
      }
    }
    ASSERT_TRUE(op.Close().ok());
    EXPECT_EQ(budget.bytes_allocated(), 0);
  }
}

}  // namespace
}  // namespace antb1::exec
