#include "../window.h"

#include <algorithm>
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
#include "antb1/exec/profile.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/sql_status.h"
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

plan::WindowCall Prefix(AggKind kind, LogicalType type, LogicalType result, bool partition = true) {
  auto call = Call(kind, type, result, partition);
  call.frame = plan::WindowCall::Frame::kRowsPrefix;
  call.frame_omitted = false;
  return call;
}

plan::WindowCall Rank(LogicalType type = LogicalType::kBigInt, bool descending = false,
                      bool nulls_first = false) {
  return {.kind = plan::WindowCall::Kind::kRank,
          .partition_by = {Column(0, "k", LogicalType::kBigInt)},
          .order_by = {{.column = Column(1, "v", type),
                        .descending = descending,
                        .nulls_first = nulls_first}},
          .name = "rank"};
}

TEST_F(WindowTest, RowsPrefixesShareRankLayoutsAndKeepInputIdentity) {
  const auto schema =
      arrow::schema({arrow::field("k", arrow::int64()), arrow::field("v", arrow::int64())});
  const auto batch =
      arrow::RecordBatch::Make(schema, 7,
                               {testing::Int64s({1, 2, 1, 1, 2, std::nullopt, std::nullopt}),
                                testing::Int64s({3, 8, 1, 3, std::nullopt, 9, 9})});
  auto source = std::make_unique<ScriptedSource>(
      schema, std::vector<Batch>{
                  {.data = batch->Slice(0, 3), .selection = testing::Bools({true, false, true})},
                  {.data = batch->Slice(3), .selection = {}}});
  const auto* observed = source.get();
  auto count = Prefix(AggKind::kCountStar, LogicalType::kBigInt, LogicalType::kBigInt);
  count.order_by = Rank().order_by;
  auto descending = count;
  descending.order_by = Rank(LogicalType::kBigInt, true, true).order_by;
  WindowOperator op(std::move(source),
                    {count, Rank(), descending, count,
                     Call(AggKind::kCountStar, LogicalType::kBigInt, LogicalType::kBigInt)});
  MemoryBudget budget(1024 * 1024);
  ExecContext ctx{.pool = &budget, .batch_size = 1, .budget = &budget};
  for (int run = 1; run <= 2; ++run) {
    {
      const auto result = Drain(op, ctx);
      ASSERT_TRUE(result.ok()) << result.status();
      EXPECT_EQ(testing::Int64Column(**result, 2),
                (std::vector<std::optional<int64_t>>{2, 1, 3, 1, 1, 2}));
      EXPECT_EQ(testing::Int64Column(**result, 3),
                (std::vector<std::optional<int64_t>>{2, 1, 2, 1, 1, 1}));
      EXPECT_EQ(testing::Int64Column(**result, 4),
                (std::vector<std::optional<int64_t>>{1, 3, 2, 1, 1, 2}));
      EXPECT_EQ(testing::Int64Column(**result, 5), testing::Int64Column(**result, 2));
      EXPECT_EQ(testing::Int64Column(**result, 6),
                (std::vector<std::optional<int64_t>>{3, 3, 3, 1, 2, 2}));
    }
    EXPECT_EQ(observed->opens(), run);
    EXPECT_EQ(observed->closes(), run);
    EXPECT_EQ(budget.bytes_allocated(), 0);
  }
}

TEST_F(WindowTest, RowsPrefixesCarryStateAcrossChunksAndResetPartitions) {
  constexpr int64_t kRows = 65539;
  arrow::Int64Builder keys;
  arrow::Int64Builder values;
  for (int64_t i = 0; i < kRows; ++i) {
    ASSERT_TRUE(keys.Append(i < 65537 ? 0 : 1).ok());
    if (i == 65535 || i == 65537) {
      ASSERT_TRUE(values.AppendNull().ok());
    } else {
      ASSERT_TRUE(values.Append(i == 0 ? 7 : 3).ok());
    }
  }
  const auto schema =
      arrow::schema({arrow::field("k", arrow::int64()), arrow::field("v", arrow::int64())});
  const auto batch = arrow::RecordBatch::Make(
      schema, kRows, {keys.Finish().ValueOrDie(), values.Finish().ValueOrDie()});
  auto source = std::make_unique<ScriptedSource>(
      schema, std::vector<Batch>{{.data = batch->Slice(0, 65535), .selection = {}},
                                 {.data = batch->Slice(65535), .selection = {}}});
  WindowOperator op(std::move(source),
                    {Prefix(AggKind::kCountStar, LogicalType::kBigInt, LogicalType::kBigInt),
                     Prefix(AggKind::kCount, LogicalType::kBigInt, LogicalType::kBigInt),
                     Prefix(AggKind::kMin, LogicalType::kBigInt, LogicalType::kBigInt),
                     Prefix(AggKind::kMax, LogicalType::kBigInt, LogicalType::kBigInt)});
  ExecContext ctx;
  ctx.batch_size = 65536;
  const auto result = Drain(op, ctx);
  ASSERT_TRUE(result.ok()) << result.status();
  const auto counts = testing::Int64Column(**result, 2);
  ASSERT_EQ(counts.size(), static_cast<std::size_t>(kRows));
  for (std::size_t i = 0; i < counts.size(); ++i) {
    EXPECT_EQ(counts[i], static_cast<int64_t>(i < 65537 ? i + 1 : i - 65536));
  }
  const auto valid_counts = testing::Int64Column(**result, 3);
  const auto minima = testing::Int64Column(**result, 4);
  const auto maxima = testing::Int64Column(**result, 5);
  // Both input and output chunk boundaries carry argument state, including the maximum in the
  // first input chunk. The new partition starts with NULL and resets all three aggregates.
  for (const std::size_t i : {0U, 65534U, 65535U, 65536U, 65537U, 65538U}) {
    SCOPED_TRACE(i);
    EXPECT_EQ(valid_counts[i], static_cast<int64_t>(i < 65535 ? i + 1 : i < 65537 ? i : i - 65537));
    EXPECT_EQ(minima[i], i == 65537 ? std::nullopt : std::optional<int64_t>(i == 0 ? 7 : 3));
    EXPECT_EQ(maxima[i], i == 65537 ? std::nullopt : std::optional<int64_t>(i < 65537 ? 7 : 3));
  }
}

// Exercise the executor path deterministically on the test thread.
class InlineWindowExecutor final : public arrow::internal::Executor {
 public:
  int GetCapacity() override { return 4; }

 protected:
  arrow::Status SpawnReal(arrow::internal::TaskHints, arrow::internal::FnOnce<void()> task,
                          arrow::StopToken, StopCallback&&) override {
    std::move(task)();
    return arrow::Status::OK();
  }
};

TEST_F(WindowTest, BucketWorkersWriteNullablePrefixesToTheirOriginalBatchPositions) {
  constexpr int64_t kRows = (4 * 65536) + 13;
  arrow::Int64Builder keys;
  arrow::Int64Builder values;
  arrow::Int64Builder sequence;
  ASSERT_TRUE(keys.Reserve(kRows).ok());
  ASSERT_TRUE(values.Reserve(kRows).ok());
  ASSERT_TRUE(sequence.Reserve(kRows).ok());
  for (int64_t i = 0; i < kRows; ++i) {
    if (i % 97 == 0) {
      keys.UnsafeAppendNull();
    } else {
      keys.UnsafeAppend(i % 53);
    }
    if (i % 97 == 0 || i % 11 == 0) {
      values.UnsafeAppendNull();
    } else {
      values.UnsafeAppend(i % 271);
    }
    sequence.UnsafeAppend(i);
  }
  const auto schema =
      arrow::schema({arrow::field("k", arrow::int64()), arrow::field("v", arrow::int64()),
                     arrow::field("seq", arrow::int64())});
  const auto batch = arrow::RecordBatch::Make(
      schema, kRows,
      {keys.Finish().ValueOrDie(), values.Finish().ValueOrDie(), sequence.Finish().ValueOrDie()});
  auto sum = Prefix(AggKind::kSum, LogicalType::kBigInt, LogicalType::kHugeInt);
  auto max = Prefix(AggKind::kMax, LogicalType::kBigInt, LogicalType::kBigInt);
  sum.order_by = {{.column = Column(2, "seq", LogicalType::kBigInt), .descending = true}};
  max.order_by = sum.order_by;
  const auto run = [&](arrow::internal::Executor* executor, ProfileNode& profile) {
    auto source = std::make_unique<ScriptedSource>(
        schema, std::vector<Batch>{{.data = batch->Slice(0, 100003), .selection = {}},
                                   {.data = batch->Slice(100003), .selection = {}}});
    WindowOperator op(std::move(source), {sum, max});
    op.set_profile(&profile);
    ExecContext ctx{
        .batch_size = 509, .executor = executor, .threads = executor == nullptr ? 1 : 4};
    return Drain(op, ctx);
  };
  ProfileNode serial_profile;
  const auto serial = run(nullptr, serial_profile);
  ASSERT_TRUE(serial.ok()) << serial.status();
  InlineWindowExecutor executor;
  ProfileNode profile;
  const auto bucketed = run(&executor, profile);
  ASSERT_TRUE(bucketed.ok()) << bucketed.status();
  EXPECT_TRUE((*serial)->Equals(**bucketed));
  const auto metrics = profile.metrics();
  const auto direct = std::ranges::find(metrics, "direct_prefix_columns", &ProfileMetric::name);
  ASSERT_NE(direct, metrics.end());
  EXPECT_EQ(direct->value, 2);
  const auto packed = std::ranges::find(metrics, "packed_sort_rows", &ProfileMetric::name);
  ASSERT_NE(packed, metrics.end());
  EXPECT_GT(packed->value, 0);
}

TEST_F(WindowTest, RankPeersAcrossMasksBatchesAndSpecificationsKeepRowIdentity) {
  const auto schema =
      arrow::schema({arrow::field("k", arrow::int64()), arrow::field("v", arrow::int64())});
  const auto batch =
      arrow::RecordBatch::Make(schema, 8,
                               {testing::Int64s({1, 2, 1, 1, 2, std::nullopt, std::nullopt, 2}),
                                testing::Int64s({3, 5, 1, 3, std::nullopt, 9, 9, 5})});
  auto source = std::make_unique<ScriptedSource>(
      schema, std::vector<Batch>{
                  {.data = batch->Slice(0, 3), .selection = testing::Bools({true, false, true})},
                  {.data = batch->Slice(3, 2), .selection = {}},
                  {.data = batch->Slice(5), .selection = {}}});
  const auto* observed = source.get();
  auto global = Rank();
  global.partition_by.clear();
  auto unordered = Rank();
  unordered.order_by.clear();
  WindowOperator op(std::move(source),
                    {Rank(), Rank(), Rank(LogicalType::kBigInt, true, true), global, unordered,
                     Call(AggKind::kCountStar, LogicalType::kBigInt, LogicalType::kBigInt)});
  MemoryBudget budget(1024 * 1024);
  ExecContext ctx{.pool = &budget, .batch_size = 1, .budget = &budget};
  for (int run = 1; run <= 2; ++run) {
    {
      const auto output = Drain(op, ctx);
      ASSERT_TRUE(output.ok()) << output.status();
      const auto column = [&](int i) { return testing::Int64Column(**output, i); };
      EXPECT_EQ(column(2), (std::vector<std::optional<int64_t>>{2, 1, 2, 2, 1, 1, 1}));
      EXPECT_EQ(column(3), column(2));
      EXPECT_EQ(column(4), (std::vector<std::optional<int64_t>>{1, 3, 1, 1, 1, 1, 2}));
      EXPECT_EQ(column(5), (std::vector<std::optional<int64_t>>{2, 1, 2, 7, 5, 5, 4}));
      EXPECT_EQ(column(6), (std::vector<std::optional<int64_t>>{1, 1, 1, 1, 1, 1, 1}));
      EXPECT_EQ(column(7), (std::vector<std::optional<int64_t>>{3, 3, 3, 2, 2, 2, 2}));
    }
    EXPECT_EQ(observed->opens(), run);
    EXPECT_EQ(observed->closes(), run);
    EXPECT_EQ(observed->pulls(), run * 4);
    EXPECT_EQ(budget.bytes_allocated(), 0);
  }
  ASSERT_TRUE(op.Open(ctx).ok());
  {
    const auto first = op.Next();
    ASSERT_TRUE(first.ok()) << first.status();
    EXPECT_EQ(first->data->column(2), first->data->column(3));
  }
  ASSERT_TRUE(op.Close().ok());
  EXPECT_EQ(budget.bytes_allocated(), 0);
}

TEST_F(WindowTest, RankUsesEverySupportedOrderingTypeAndNullDirection) {
  const std::vector<std::pair<LogicalType, std::shared_ptr<arrow::Array>>> inputs{
      {LogicalType::kSmallInt, testing::Int16s({2, 1, 2, std::nullopt, 3, 1, std::nullopt})},
      {LogicalType::kInteger, testing::Int32s({2, 1, 2, std::nullopt, 3, 1, std::nullopt})},
      {LogicalType::kBigInt, testing::Int64s({2, 1, 2, std::nullopt, 3, 1, std::nullopt})},
      {LogicalType::kUSmallInt, testing::UInt16s({2, 1, 2, std::nullopt, 3, 1, std::nullopt})},
      {LogicalType::kDouble,
       testing::ArrayOf<arrow::DoubleBuilder>(
           arrow::float64(),
           std::vector<std::optional<double>>{2, 1, 2, std::nullopt, 3, 1, std::nullopt})},
      {LogicalType::kHugeInt,
       testing::Decimals(38, 0, {"2", "1", "2", std::nullopt, "3", "1", std::nullopt})},
      {LogicalType::Decimal(12, 2),
       testing::Decimals(12, 2, {"2", "1", "2", std::nullopt, "3", "1", std::nullopt})},
      {LogicalType::Decimal(30, 7),
       testing::Decimals(30, 7, {"2", "1", "2", std::nullopt, "3", "1", std::nullopt})},
      {LogicalType::kVarchar,
       testing::Strings({"b", "a", "b", std::nullopt, "c", "a", std::nullopt})},
      {LogicalType::kDate, testing::Dates({2, 1, 2, std::nullopt, 3, 1, std::nullopt})},
      {LogicalType::kTimestamp, testing::Timestamps({2, 1, 2, std::nullopt, 3, 1, std::nullopt})}};
  for (const auto& [type, values] : inputs) {
    SCOPED_TRACE(values->type()->ToString());
    const auto schema =
        arrow::schema({arrow::field("k", arrow::int64()), arrow::field("v", values->type())});
    const auto batch =
        arrow::RecordBatch::Make(schema, 7, {testing::Int64s({1, 1, 1, 1, 1, 1, 1}), values});
    auto source = std::make_unique<ScriptedSource>(
        schema, std::vector<Batch>{{.data = batch->Slice(0, 2), .selection = {}},
                                   {.data = batch->Slice(2), .selection = {}}});
    WindowOperator op(std::move(source), {Rank(type), Rank(type, true, true)});
    ExecContext ctx;
    ctx.batch_size = 2;
    const auto output = Drain(op, ctx);
    ASSERT_TRUE(output.ok()) << output.status();
    EXPECT_EQ(testing::Int64Column(**output, 2),
              (std::vector<std::optional<int64_t>>{3, 1, 3, 6, 5, 1, 6}));
    EXPECT_EQ(testing::Int64Column(**output, 3),
              (std::vector<std::optional<int64_t>>{4, 6, 4, 1, 3, 6, 1}));
  }
}

TEST_F(WindowTest, RankPartitionAndPeerEqualityNormalizeNaNsAndSignedZeros) {
  const auto values = testing::ArrayOf<arrow::DoubleBuilder>(
      arrow::float64(), std::vector<std::optional<double>>{std::nan("1"), -0.0, std::nullopt,
                                                           std::nan("2"), 0.0, 3.0});
  const auto schema =
      arrow::schema({arrow::field("k", arrow::float64()), arrow::field("v", arrow::float64())});
  const auto batch = arrow::RecordBatch::Make(schema, 6, {values, values});
  auto source = std::make_unique<ScriptedSource>(
      schema, std::vector<Batch>{{.data = batch->Slice(0, 3), .selection = {}},
                                 {.data = batch->Slice(3), .selection = {}}});
  auto global = Rank(LogicalType::kDouble);
  global.partition_by.clear();
  auto partitioned = global;
  partitioned.partition_by = {Column(0, "k", LogicalType::kDouble)};
  WindowOperator op(std::move(source), {global, partitioned});
  ExecContext ctx;
  const auto result = Drain(op, ctx);
  ASSERT_TRUE(result.ok()) << result.status();
  EXPECT_EQ(testing::Int64Column(**result, 2),
            (std::vector<std::optional<int64_t>>{4, 1, 6, 4, 1, 3}));
  EXPECT_EQ(testing::Int64Column(**result, 3),
            (std::vector<std::optional<int64_t>>{1, 1, 1, 1, 1, 1}));
}

TEST_F(WindowTest, RankPeersSpanLargeInputAndOutputChunks) {
  constexpr int64_t kRows = 65539;
  arrow::Int64Builder values;
  for (int64_t i = 0; i < kRows; ++i) {
    ASSERT_TRUE(values.Append(i < kRows - 2 ? 7 : 9).ok());
  }
  const auto schema = arrow::schema({arrow::field("v", arrow::int64())});
  const auto first = arrow::RecordBatch::Make(schema, kRows, {values.Finish().ValueOrDie()});
  const auto last = arrow::RecordBatch::Make(schema, 3, {testing::Int64s({7, 9, 5})});
  auto source = std::make_unique<ScriptedSource>(
      schema,
      std::vector<Batch>{{.data = first, .selection = {}}, {.data = last, .selection = {}}});
  auto call = Rank();
  call.partition_by.clear();
  call.order_by.front().column.index = 0;
  WindowOperator op(std::move(source), {call});
  ExecContext ctx;
  ctx.batch_size = 65536;
  const auto output = Drain(op, ctx);
  ASSERT_TRUE(output.ok()) << output.status();
  const auto ranks = testing::Int64Column(**output, 1);
  ASSERT_EQ(ranks.size(), static_cast<std::size_t>(kRows + 3));
  for (std::size_t i = 0; i < static_cast<std::size_t>(kRows - 2); ++i) {
    EXPECT_EQ(ranks[i], 2);
  }
  EXPECT_EQ(ranks[65537], 65540);
  EXPECT_EQ(ranks[65538], 65540);
  EXPECT_EQ(ranks[65539], 2);
  EXPECT_EQ(ranks[65540], 65540);
  EXPECT_EQ(ranks[65541], 1);
}

TEST_F(WindowTest, InvalidBoundRankColumnsAreRejectedBeforeFastPaths) {
  const auto schema = arrow::schema({arrow::field("k", arrow::int64())});
  for (const bool empty : {false, true}) {
    for (const auto& key :
         {Column(-1, "bad", LogicalType::kBigInt), Column(1, "bad", LogicalType::kBigInt),
          Column(0, "bad", LogicalType::kVarchar)}) {
      for (const bool ordered : {false, true}) {
        const auto batch = arrow::RecordBatch::Make(schema, 1, {testing::Int64s({1})});
        std::vector<Batch> batches;
        if (!empty) {
          batches.push_back({.data = batch, .selection = {}});
        }
        auto source = std::make_unique<ScriptedSource>(schema, std::move(batches));
        const auto* observed = source.get();
        auto call = Rank();
        call.partition_by.clear();
        call.order_by.clear();
        if (ordered) {
          call.order_by = {{.column = key}};
        } else {
          call.partition_by = {key};
        }
        WindowOperator op(std::move(source), {call});
        ExecContext ctx;
        EXPECT_TRUE(op.Open(ctx).IsInvalid());
        EXPECT_EQ(observed->opens(), 0);
        EXPECT_TRUE(op.Close().ok());
      }
    }
  }
}

TEST_F(WindowTest, InvalidAndDeferredRankCallsAreRejectedBeforeOpeningInput) {
  const auto schema =
      arrow::schema({arrow::field("k", arrow::boolean()), arrow::field("v", arrow::int64())});
  for (int variant = 0; variant < 6; ++variant) {
    SCOPED_TRACE(variant);
    auto call = Rank();
    call.partition_by.clear();
    switch (variant) {
      case 0:
        call.frame = plan::WindowCall::Frame::kWholePartition;
        break;
      case 1:
        call.frame_omitted = false;
        break;
      case 2:
        call.arguments = {Column(1, "v", LogicalType::kBigInt)};
        break;
      case 3:
        call.type = LogicalType::kInteger;
        break;
      case 4:
        call.partition_by = {Column(0, "k", LogicalType::kBoolean)};
        break;
      case 5:
        call.order_by = {{.column = Column(0, "k", LogicalType::kBoolean)}};
        break;
      default:
        FAIL() << "unhandled rank test variant";
    }
    auto source = std::make_unique<ScriptedSource>(schema, std::vector<Batch>{});
    const auto* observed = source.get();
    WindowOperator op(std::move(source), {call});
    ExecContext ctx;
    const auto status = op.Open(ctx);
    if (variant == 2 || variant == 3) {
      EXPECT_TRUE(status.IsInvalid()) << status;
    } else {
      const auto detail = plan::GetSqlError(status);
      ASSERT_NE(detail, nullptr) << status;
      EXPECT_EQ(detail->kind(), plan::SqlErrorDetail::Kind::kUnsupported);
      EXPECT_NE(status.message().find(variant < 2 ? "W8" : "W11b"), std::string::npos);
    }
    EXPECT_EQ(observed->opens(), 0);
    EXPECT_TRUE(op.Close().ok());
  }
}

TEST_F(WindowTest, InvalidRowsPrefixCallsAreRejectedBeforeOpeningInput) {
  const auto schema =
      arrow::schema({arrow::field("k", arrow::boolean()), arrow::field("v", arrow::int64())});
  for (int variant = 0; variant < 7; ++variant) {
    SCOPED_TRACE(variant);
    auto call = Prefix(AggKind::kSum, LogicalType::kBigInt, LogicalType::kHugeInt, false);
    switch (variant) {
      case 0:
        call.aggregate = AggKind::kSumCounts;
        break;
      case 1:
        call.aggregate = AggKind::kSingleValue;
        break;
      case 2:
        call.type = LogicalType::kInteger;
        break;
      case 3:
        call.arguments.clear();
        break;
      case 4:
        call.arguments.front().index = 2;
        break;
      case 5:
        call.arguments = {Column(0, "k", LogicalType::kBoolean)};
        break;
      case 6:
        call.frame_omitted = true;
        break;
      default:
        FAIL() << "unhandled prefix test variant";
    }
    auto source = std::make_unique<ScriptedSource>(schema, std::vector<Batch>{});
    const auto* observed = source.get();
    WindowOperator op(std::move(source), {call});
    ExecContext ctx;
    const auto status = op.Open(ctx);
    if (variant < 5) {
      EXPECT_TRUE(status.IsInvalid()) << status;
    } else {
      const auto detail = plan::GetSqlError(status);
      ASSERT_NE(detail, nullptr) << status;
      EXPECT_EQ(detail->kind(), plan::SqlErrorDetail::Kind::kUnsupported);
      EXPECT_NE(status.message().find(variant == 5 ? "W11b" : "W6"), std::string::npos);
    }
    EXPECT_EQ(observed->opens(), 0);
    EXPECT_TRUE(op.Close().ok());
  }
}

TEST_F(WindowTest, RankLayoutAndResultsRespectBudgetAndReleaseOnFailure) {
  const auto schema = arrow::schema({arrow::field("v", arrow::int64())});
  std::vector<std::optional<int64_t>> values(257, 7);
  const auto batch = arrow::RecordBatch::Make(schema, 257, {testing::Int64s(values)});
  auto call = Rank();
  call.partition_by.clear();
  call.order_by.front().column.index = 0;
  bool failed_during_layout = false;
  bool succeeded = false;
  for (int64_t limit = 2048; limit <= 16384; limit += 256) {
    auto source = std::make_unique<ScriptedSource>(
        schema, std::vector<Batch>{{.data = batch, .selection = {}}});
    const auto* observed = source.get();
    WindowOperator op(std::move(source), {call, call});
    MemoryBudget budget(limit);
    ExecContext ctx{.pool = &budget, .batch_size = 13, .budget = &budget};
    {
      const auto result = Drain(op, ctx);
      if (result.ok()) {
        EXPECT_EQ(testing::Int64Column(**result, 1), (std::vector<std::optional<int64_t>>(257, 1)));
        succeeded = true;
      } else {
        EXPECT_TRUE(result.status().IsOutOfMemory()) << result.status();
        failed_during_layout = failed_during_layout || observed->pulls() == 2;
      }
    }
    EXPECT_EQ(budget.bytes_allocated(), 0);
    EXPECT_LE(budget.max_memory(), limit);
    EXPECT_EQ(observed->closes(), 1);
  }
  EXPECT_TRUE(failed_during_layout);
  EXPECT_TRUE(succeeded);
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
      WindowOperator op(std::move(source), {Call(kind, type, result), Prefix(kind, type, result)});
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
        auto prefix_expected = expected[group];
        if (row == 0) {
          auto first =
              MakeAggregateState(
                  kind, kind == AggKind::kCountStar ? std::nullopt : std::optional(type), result)
                  .ValueOrDie();
          ASSERT_TRUE(first->Consume(*values->Slice(0, 1), nullptr).ok());
          prefix_expected = first->Finalize(ctx.pool).ValueOrDie();
        }
        EXPECT_TRUE(combined->column(3)->chunk(0)->Slice(row, 1)->ApproxEquals(prefix_expected))
            << combined->column(3)->ToString() << " expected " << prefix_expected->ToString();
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
    WindowOperator op(
        std::move(source),
        {Call(AggKind::kCountStar, LogicalType::kBigInt, LogicalType::kBigInt, false), Rank(),
         Prefix(AggKind::kCountStar, LogicalType::kBigInt, LogicalType::kBigInt, false)});
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
                      {Call(AggKind::kMin, LogicalType::kVarchar, LogicalType::kVarchar),
                       Prefix(AggKind::kMax, LogicalType::kVarchar, LogicalType::kVarchar),
                       Rank(LogicalType::kVarchar)});
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
  for (unsigned variant = 0; variant < 4; ++variant) {
    const bool average = (variant & 1U) != 0;
    const bool prefix = (variant & 2U) != 0;
    auto source = std::make_unique<ScriptedSource>(
        schema, std::vector<Batch>{{.data = batch, .selection = {}}});
    auto call = Call(average ? AggKind::kAvg : AggKind::kSum, type,
                     average ? LogicalType::kDouble : LogicalType::kHugeInt);
    if (prefix) {
      call.frame = plan::WindowCall::Frame::kRowsPrefix;
      call.frame_omitted = false;
    }
    WindowOperator op(std::move(source), {call});
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

TEST_F(WindowTest, RowsPrefixOverflowIsDetectedEvenWhenTheFinalSumFits) {
  const auto type = LogicalType::kHugeInt;
  const auto schema =
      arrow::schema({arrow::field("k", arrow::int64()), arrow::field("v", plan::ToArrow(type))});
  const std::string large = "6" + std::string(37, '0');
  const auto batch = arrow::RecordBatch::Make(
      schema, 3,
      {testing::Int64s({1, 1, 1}), testing::Decimals(38, 0, {large, large, "-" + large})});
  auto source = std::make_unique<ScriptedSource>(
      schema, std::vector<Batch>{{.data = batch, .selection = {}}});
  WindowOperator op(std::move(source), {Prefix(AggKind::kSum, type, type)});
  MemoryBudget budget(std::nullopt);
  ExecContext ctx{.pool = &budget, .batch_size = 1, .budget = &budget};
  ASSERT_TRUE(op.Open(ctx).ok());
  EXPECT_TRUE(op.Next().status().IsExecutionError());
  EXPECT_EQ(budget.bytes_allocated(), 0);
  EXPECT_TRUE(op.Close().ok());
}

}  // namespace
}  // namespace antb1::exec
