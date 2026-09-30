#include "../parallel_compute.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <arrow/util/thread_pool.h>
#include <gtest/gtest.h>

#include "antb1/common/int128.h"
#include "antb1/exec/compute.h"
#include "antb1/exec/operator.h"
#include "antb1/plan/logical_plan.h"

#include "exec_test_util.h"

namespace antb1::exec {
namespace {

using plan::LogicalType;
using testing::Bools;
using testing::Int64s;
using testing::ScriptedSource;

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

// `count` batches of 10 rows (values 10 * b + i); every third batch has a selection, and the
// batches in `overflowing` hold kMax in a selected row.
std::vector<Batch> Batches(std::size_t count, const std::vector<std::size_t>& overflowing = {}) {
  std::vector<Batch> batches;
  for (std::size_t b = 0; b < count; ++b) {
    std::vector<std::optional<int64_t>> values;
    std::vector<std::optional<bool>> selected;
    for (int64_t i = 0; i < 10; ++i) {
      values.emplace_back(static_cast<int64_t>(b) * 10 + i);
      selected.emplace_back(i % 2 == 0);
    }
    values[3] = std::nullopt;
    if (std::ranges::find(overflowing, b) != overflowing.end()) {
      values[4] = kMax;
    }
    batches.push_back(
        Batch{.data = arrow::RecordBatch::Make(Schema(), 10, arrow::ArrayVector{Int64s(values)}),
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

}  // namespace
}  // namespace antb1::exec
