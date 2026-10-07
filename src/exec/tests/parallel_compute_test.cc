#include "../parallel_compute.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <arrow/util/thread_pool.h>
#include <gtest/gtest.h>

#include "antb1/common/int128.h"
#include "antb1/exec/compute.h"
#include "antb1/exec/memory_budget.h"
#include "antb1/exec/operator.h"
#include "antb1/plan/logical_plan.h"

#include "exec_test_util.h"

namespace antb1::exec {
namespace {

using plan::LogicalType;
using testing::Bools;
using testing::Int64s;
using testing::ScriptedSource;
using testing::WorkerFailingPool;

class ParallelComputeTest : public testing::ExecTest {};

constexpr int64_t kMax = std::numeric_limits<int64_t>::max();

// c0 + 1 (BIGINT): an overflow on kMax.
std::vector<plan::ExprPtr> PlusOne() {
  auto column = std::make_shared<const plan::Expr>(
      plan::Expr{.node = plan::ColumnExpr{.index = 0}, .type = LogicalType::kBigInt, .name = "c"});
  auto one = std::make_shared<const plan::Expr>(
      plan::Expr{.node = plan::ConstantExpr{.value = plan::Constant{.type = LogicalType::kBigInt,
                                                                    .value = Int128{1}}},
                 .type = LogicalType::kBigInt,
                 .name = "1"});
  return {std::make_shared<const plan::Expr>(
      plan::Expr{.node = plan::ArithExpr{.op = plan::ArithOp::kAdd, .left = column, .right = one},
                 .type = LogicalType::kBigInt,
                 .name = "c + 1"})};
}

std::shared_ptr<arrow::Schema> Schema() {
  return arrow::schema({arrow::field("c", arrow::int64())});
}

// `count` batches of `rows` rows (values rows * b + i); every third batch has a selection, and the
// batches in `overflowing` hold kMax in a selected row.
std::vector<Batch> Batches(std::size_t count, const std::vector<std::size_t>& overflowing = {},
                           int64_t rows = 10) {
  std::vector<Batch> batches;
  for (std::size_t b = 0; b < count; ++b) {
    std::vector<std::optional<int64_t>> values;
    std::vector<std::optional<bool>> selected;
    for (int64_t i = 0; i < rows; ++i) {
      values.emplace_back((static_cast<int64_t>(b) * rows) + i);
      selected.emplace_back(i % 2 == 0);
    }
    values[3] = std::nullopt;
    if (std::ranges::find(overflowing, b) != overflowing.end()) {
      values[4] = kMax;
    }
    batches.push_back(
        Batch{.data = arrow::RecordBatch::Make(Schema(), rows, arrow::ArrayVector{Int64s(values)}),
              .selection = b % 3 == 0 ? Bools(selected) : nullptr});
  }
  return batches;
}

struct Output {
  std::vector<std::shared_ptr<arrow::RecordBatch>> batches;
  arrow::Status status;  // the first error, after the batches returned before it
};

// Every batch an operator returns, until the end or its first error.
Output Drain(Operator& op, arrow::internal::Executor* executor) {
  ExecContext ctx{.executor = executor, .threads = executor == nullptr ? 1 : 4};
  Output run;
  run.status = op.Open(ctx);
  while (run.status.ok()) {
    arrow::Result<Batch> batch = op.Next();
    if (!batch.ok()) {
      run.status = batch.status();
      break;
    }
    if (batch->end()) {
      break;
    }
    run.batches.push_back(batch->data);
  }
  EXPECT_TRUE(op.Close().ok());
  return run;
}

// The batches and the first error of ComputeOperator over the same input.
Output Serial(std::vector<Batch> batches, std::optional<std::size_t> fail_at = std::nullopt) {
  auto source = std::make_unique<ScriptedSource>(Schema(), std::move(batches));
  if (fail_at.has_value()) {
    source->FailAt(*fail_at);
  }
  ComputeOperator op(std::move(source), PlusOne());
  return Drain(op, nullptr);
}

void ExpectSame(const Output& parallel, const Output& serial) {
  ASSERT_EQ(parallel.batches.size(), serial.batches.size());
  for (std::size_t i = 0; i < serial.batches.size(); ++i) {
    EXPECT_TRUE(parallel.batches[i]->Equals(*serial.batches[i])) << "batch " << i;
  }
  EXPECT_EQ(parallel.status.ToString(), serial.status.ToString());
}

TEST_F(ParallelComputeTest, ReturnsComputeOperatorsBatchesInOrder) {
  auto pool = arrow::internal::ThreadPool::Make(4);
  ASSERT_TRUE(pool.ok());
  for (arrow::internal::Executor* executor :
       {static_cast<arrow::internal::Executor*>(nullptr),
        static_cast<arrow::internal::Executor*>(pool->get())}) {
    ParallelComputeOperator op(std::make_unique<ScriptedSource>(Schema(), Batches(50)), PlusOne());
    const Output run = Drain(op, executor);
    ASSERT_TRUE(run.status.ok()) << run.status.ToString();
    ExpectSame(run, Serial(Batches(50)));
    EXPECT_EQ(run.batches.size(), 50U);
  }
}

// The first failing batch in input order fails the query, after the batches before it; the
// batches computed ahead of it, even failing ones, are never returned.
TEST_F(ParallelComputeTest, FailsOnTheFirstFailingBatchInOrder) {
  auto pool = arrow::internal::ThreadPool::Make(4);
  ASSERT_TRUE(pool.ok());
  ParallelComputeOperator op(std::make_unique<ScriptedSource>(Schema(), Batches(30, {7, 9, 20})),
                             PlusOne());
  const Output run = Drain(op, pool->get());
  EXPECT_FALSE(run.status.ok());
  EXPECT_EQ(run.batches.size(), 7U);
  ExpectSame(run, Serial(Batches(30, {7, 9, 20})));
}

// The input's own failure comes after the batches read before it, and after their errors.
TEST_F(ParallelComputeTest, ReturnsTheInputsFailureAfterTheBatchesBeforeIt) {
  auto pool = arrow::internal::ThreadPool::Make(4);
  ASSERT_TRUE(pool.ok());
  for (const auto& overflowing : {std::vector<std::size_t>{}, std::vector<std::size_t>{3}}) {
    auto source = std::make_unique<ScriptedSource>(Schema(), Batches(10, overflowing));
    source->FailAt(5);
    ParallelComputeOperator op(std::move(source), PlusOne());
    const Output run = Drain(op, pool->get());
    ExpectSame(run, Serial(Batches(10, overflowing), 5));
    EXPECT_EQ(run.status.IsIOError(), overflowing.empty()) << run.status.ToString();
  }
}

// A consumer that stops early (a Limit) closes the operator with batches still in flight.
TEST_F(ParallelComputeTest, ClosesWithBatchesInFlight) {
  auto pool = arrow::internal::ThreadPool::Make(4);
  ASSERT_TRUE(pool.ok());
  auto source = std::make_unique<ScriptedSource>(Schema(), Batches(100, {50}));
  const ScriptedSource* scripted = source.get();
  ParallelComputeOperator op(std::move(source), PlusOne());
  ExecContext ctx{.executor = pool->get(), .threads = 4};
  ASSERT_TRUE(op.Open(ctx).ok());
  auto first = op.Next();
  ASSERT_TRUE(first.ok()) << first.status().ToString();
  EXPECT_FALSE(first->end());
  EXPECT_LE(scripted->pulls(), 5);  // the window (4 threads) and no more
  EXPECT_TRUE(op.Close().ok());
  // Opened again, it starts over.
  const Output again = Drain(op, pool->get());
  EXPECT_EQ(again.batches.size(), 50U);
  EXPECT_FALSE(again.status.ok());
}

// Under memory pressure one batch is computed at a time; the output is the same.
TEST_F(ParallelComputeTest, ComputesOneBatchAtATimeUnderPressure) {
  auto pool = arrow::internal::ThreadPool::Make(4);
  ASSERT_TRUE(pool.ok());
  MemoryBudget budget(1000);
  ASSERT_TRUE(budget.Reserve(600).ok());
  ASSERT_TRUE(budget.under_pressure());
  auto source = std::make_unique<ScriptedSource>(Schema(), Batches(20));
  const ScriptedSource* scripted = source.get();
  ParallelComputeOperator op(std::move(source), PlusOne());
  ExecContext ctx{.executor = pool->get(), .threads = 4, .budget = &budget};
  ASSERT_TRUE(op.Open(ctx).ok());
  const Output serial = Serial(Batches(20));
  for (std::size_t i = 0; i < serial.batches.size(); ++i) {
    auto batch = op.Next();
    ASSERT_TRUE(batch.ok()) << batch.status().ToString();
    ASSERT_FALSE(batch->end());
    EXPECT_TRUE(batch->data->Equals(*serial.batches[i])) << "batch " << i;
    EXPECT_LE(static_cast<std::size_t>(scripted->pulls()), i + 2);  // never more than one ahead
    EXPECT_LE(op.window(), i == 0 ? 2U : 1U);  // halved by every batch taken under pressure
  }
  auto end = op.Next();
  ASSERT_TRUE(end.ok());
  EXPECT_TRUE(end->end());
  EXPECT_TRUE(op.Close().ok());
  budget.Release(600);
}

// Out of memory alone: the query fails with OutOfMemory and every byte comes back.
TEST_F(ParallelComputeTest, OutOfMemoryAloneFailsAndFreesEverything) {
  auto pool = arrow::internal::ThreadPool::Make(4);
  ASSERT_TRUE(pool.ok());
  MemoryBudget tiny(64);
  {
    ParallelComputeOperator op(std::make_unique<ScriptedSource>(Schema(), Batches(20, {}, 1000)),
                               PlusOne());
    ExecContext ctx{.pool = &tiny, .executor = pool->get(), .threads = 4, .budget = &tiny};
    ASSERT_TRUE(op.Open(ctx).ok());
    auto batch = op.Next();
    ASSERT_FALSE(batch.ok());
    EXPECT_TRUE(batch.status().IsOutOfMemory()) << batch.status().ToString();
    EXPECT_TRUE(op.Close().ok());
  }
  EXPECT_EQ(tiny.bytes_allocated(), 0);
}

// Every batch runs out of memory on a worker, also once the window is 1 and nothing else is in
// flight: each is computed again alone, and the output is ComputeOperator's.
TEST_F(ParallelComputeTest, OutOfMemoryOnAWorkerComputesTheBatchAgainAlone) {
  auto pool = arrow::internal::ThreadPool::Make(4);
  ASSERT_TRUE(pool.ok());
  const Output serial = Serial(Batches(12, {}, 1000));
  MemoryBudget budget(std::nullopt);
  WorkerFailingPool failing(&budget);
  {
    ParallelComputeOperator op(std::make_unique<ScriptedSource>(Schema(), Batches(12, {}, 1000)),
                               PlusOne());
    ExecContext ctx{.pool = &failing, .executor = pool->get(), .threads = 4, .budget = &budget};
    ASSERT_TRUE(op.Open(ctx).ok());
    for (std::size_t i = 0; i < serial.batches.size(); ++i) {
      auto batch = op.Next();
      ASSERT_TRUE(batch.ok()) << "batch " << i << ": " << batch.status().ToString();
      ASSERT_FALSE(batch->end());
      EXPECT_TRUE(batch->data->Equals(*serial.batches[i])) << "batch " << i;
      EXPECT_EQ(op.window(), 1U);
    }
    auto end = op.Next();
    ASSERT_TRUE(end.ok());
    EXPECT_TRUE(end->end());
    EXPECT_TRUE(op.Close().ok());
  }
  EXPECT_GE(failing.failures(), 12);  // every batch failed on a worker first
  EXPECT_EQ(budget.bytes_allocated(), 0);
}

// Out of memory alone, the batch computed again also fails: the query fails with it.
TEST_F(ParallelComputeTest, OutOfMemoryAloneFailsAfterTheRetry) {
  auto pool = arrow::internal::ThreadPool::Make(4);
  ASSERT_TRUE(pool.ok());
  MemoryBudget tiny(64);
  WorkerFailingPool failing(&tiny);
  {
    ParallelComputeOperator op(std::make_unique<ScriptedSource>(Schema(), Batches(4, {}, 1000)),
                               PlusOne());
    ExecContext ctx{.pool = &failing, .executor = pool->get(), .threads = 4, .budget = &tiny};
    ASSERT_TRUE(op.Open(ctx).ok());
    auto batch = op.Next();
    ASSERT_FALSE(batch.ok());
    EXPECT_TRUE(batch.status().IsOutOfMemory()) << batch.status().ToString();
    EXPECT_TRUE(op.Close().ok());
  }
  EXPECT_EQ(tiny.bytes_allocated(), 0);
}

}  // namespace
}  // namespace antb1::exec
