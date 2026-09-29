// Parallel execution over table parts (docs/adr/0013-parallel-execution.md): the part scheduler
// and the part operators the physical planner builds, on one thread and on a 4-thread pool.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <arrow/util/thread_pool.h>
#include <gtest/gtest.h>

#include "antb1/exec/memory_budget.h"
#include "antb1/exec/physical_planner.h"
#include "antb1/plan/logical_plan.h"

#include "../part_scheduler.h"
#include "exec_test_util.h"

namespace antb1::exec {
namespace {

using plan::LogicalType;
using testing::BigInt;
using testing::Column;
using testing::Int64Column;
using testing::Int64s;
using testing::MemoryTable;

constexpr int kThreads = 4;

std::shared_ptr<arrow::internal::ThreadPool> Pool() {
  auto pool = arrow::internal::ThreadPool::Make(kThreads);
  EXPECT_TRUE(pool.ok()) << pool.status().ToString();
  return *pool;
}

// ---- PartScheduler ----

TEST(PartSchedulerTest, ResultsComeInPartOrder) {
  const auto pool = Pool();
  for (arrow::internal::Executor* executor :
       {static_cast<arrow::internal::Executor*>(nullptr),
        static_cast<arrow::internal::Executor*>(pool.get())}) {
    PartScheduler<int64_t> scheduler(
        20,
        [](int64_t part, const std::atomic<bool>&) -> arrow::Result<int64_t> { return part * 10; },
        executor, 3);
    for (int64_t part = 0; part < 20; ++part) {
      ASSERT_FALSE(scheduler.done());
      auto result = scheduler.Next();
      ASSERT_TRUE(result.ok()) << result.status().ToString();
      EXPECT_EQ(*result, part * 10);
    }
    EXPECT_TRUE(scheduler.done());
    EXPECT_TRUE(scheduler.Next().status().IsInvalid());
  }
}

// Parts start only within the window ahead of the consumer: at most `window` parts are submitted
// beyond the ones taken. Without an executor, a part runs only when Next() reaches it.
TEST(PartSchedulerTest, PartsStartWithinTheWindow) {
  const auto pool = Pool();
  auto started = std::make_shared<std::atomic<int64_t>>(0);
  const auto task = [started](int64_t part, const std::atomic<bool>&) -> arrow::Result<int64_t> {
    ++*started;
    return part;
  };
  {
    PartScheduler<int64_t> scheduler(20, task, pool.get(), 3);
    ASSERT_TRUE(scheduler.Next().ok());
    EXPECT_LE(started->load(), 1 + 3);
    ASSERT_TRUE(scheduler.Next().ok());
    EXPECT_LE(started->load(), 2 + 3);
  }
  *started = 0;
  PartScheduler<int64_t> inline_scheduler(20, task, nullptr, 3);
  ASSERT_TRUE(inline_scheduler.Next().ok());
  ASSERT_TRUE(inline_scheduler.Next().ok());
  EXPECT_EQ(started->load(), 2);
}

// The first failed part in part order decides the error, whatever finishes first; the scheduler
// then stops.
TEST(PartSchedulerTest, FirstErrorInPartOrder) {
  const auto pool = Pool();
  for (arrow::internal::Executor* executor :
       {static_cast<arrow::internal::Executor*>(nullptr),
        static_cast<arrow::internal::Executor*>(pool.get())}) {
    PartScheduler<int64_t> scheduler(
        10,
        [](int64_t part, const std::atomic<bool>&) -> arrow::Result<int64_t> {
          if (part == 3 || part == 5) {
            return arrow::Status::IOError("part ", part);
          }
          return part;
        },
        executor, 8);
    for (int64_t part = 0; part < 3; ++part) {
      auto result = scheduler.Next();
      ASSERT_TRUE(result.ok()) << result.status().ToString();
      EXPECT_EQ(*result, part);
    }
    const auto failed = scheduler.Next();
    EXPECT_TRUE(failed.status().IsIOError()) << failed.status().ToString();
    EXPECT_EQ(failed.status().message(), "part 3");
    EXPECT_TRUE(scheduler.done());
  }
}

// Stopping (here: destroying) the scheduler makes the running parts see `stop` and waits for them.
TEST(PartSchedulerTest, StopWaitsForRunningParts) {
  const auto pool = Pool();
  auto started = std::make_shared<std::atomic<int64_t>>(0);
  auto stopped = std::make_shared<std::atomic<int64_t>>(0);
  {
    PartScheduler<int64_t> scheduler(
        10,
        [started, stopped](int64_t part, const std::atomic<bool>& stop) -> arrow::Result<int64_t> {
          if (part == 0) {
            return part;
          }
          ++*started;
          while (!stop) {
            std::this_thread::yield();
          }
          ++*stopped;
          return arrow::Status::Cancelled("stopped");
        },
        pool.get(), 4);
    auto first = scheduler.Next();
    ASSERT_TRUE(first.ok()) << first.status().ToString();
  }
  EXPECT_EQ(started->load(), stopped->load());
  EXPECT_LE(started->load(), 4);
}

// A part that runs out of memory on the pool is run again alone on the calling thread, the window
// drops to one part and widens again; the results are the same, in order. Without an executor,
// running out of memory is the query's error.
TEST(PartSchedulerTest, OutOfMemoryInParallelFallsBackToOnePartAtATime) {
  const auto pool = Pool();
  const std::thread::id caller = std::this_thread::get_id();
  auto on_caller = std::make_shared<std::vector<int64_t>>();
  const auto task = [caller, on_caller](int64_t part,
                                        const std::atomic<bool>&) -> arrow::Result<int64_t> {
    if (std::this_thread::get_id() == caller) {
      on_caller->push_back(part);
      return part;
    }
    if (part == 3) {
      return arrow::Status::OutOfMemory("too much at once");
    }
    return part;
  };
  PartScheduler<int64_t> scheduler(10, task, pool.get(), 4);
  for (int64_t part = 0; part < 10; ++part) {
    auto result = scheduler.Next();
    ASSERT_TRUE(result.ok()) << part << ": " << result.status().ToString();
    EXPECT_EQ(*result, part);
  }
  EXPECT_EQ(*on_caller, std::vector<int64_t>{3});
  EXPECT_EQ(scheduler.window(), 4);  // 1 after part 3, then widened by the 6 parts after it

  PartScheduler<int64_t> inline_scheduler(
      10,
      [](int64_t part, const std::atomic<bool>&) -> arrow::Result<int64_t> {
        return part == 3 ? arrow::Result<int64_t>(arrow::Status::OutOfMemory("too much"))
                         : arrow::Result<int64_t>(part);
      },
      nullptr, 4);
  for (int64_t part = 0; part < 3; ++part) {
    ASSERT_TRUE(inline_scheduler.Next().ok());
  }
  EXPECT_TRUE(inline_scheduler.Next().status().IsOutOfMemory());
}

// Every part the consumer takes under memory pressure halves the window, down to one part; every
// part taken without pressure widens it again by one. The results are the same, in order.
TEST(PartSchedulerTest, PressureNarrowsTheWindow) {
  const auto pool = Pool();
  MemoryBudget budget(1000);
  PartScheduler<int64_t> scheduler(
      20, [](int64_t part, const std::atomic<bool>&) -> arrow::Result<int64_t> { return part; },
      pool.get(), 8, &budget);
  const auto next = [&](int64_t expected) {
    auto result = scheduler.Next();
    ASSERT_TRUE(result.ok()) << expected << ": " << result.status().ToString();
    EXPECT_EQ(*result, expected);
  };
  next(0);
  EXPECT_EQ(scheduler.window(), 8);
  ASSERT_TRUE(budget.Reserve(600).ok());  // past half the limit
  int64_t part = 1;
  for (const int64_t window : {4, 2, 1, 1}) {
    next(part++);
    EXPECT_EQ(scheduler.window(), window);
  }
  budget.Release(600);
  for (const int64_t window : {2, 3, 4}) {
    next(part++);
    EXPECT_EQ(scheduler.window(), window);
  }
  while (part < 20) {
    next(part++);
  }
}

// ---- part operators through the physical planner ----

class PartOperatorsTest : public testing::ExecTest {
 protected:
  // 20 parts of 7 rows: x = 0..139, d = x / 10 (DOUBLE), y = x, NULL where x is a multiple of 5.
  static std::shared_ptr<MemoryTable> Table(bool split = true) {
    const auto schema =
        arrow::schema({arrow::field("x", arrow::int64()), arrow::field("d", arrow::float64()),
                       arrow::field("y", arrow::int64())});
    arrow::RecordBatchVector batches;
    for (int64_t part = 0; part < kParts; ++part) {
      std::vector<std::optional<int64_t>> x;
      std::vector<std::optional<int64_t>> y;
      arrow::DoubleBuilder d;
      for (int64_t i = 0; i < kRows; ++i) {
        const int64_t v = (part * kRows) + i;
        x.emplace_back(v);
        y.emplace_back(v % 5 == 0 ? std::nullopt : std::optional(v));
        EXPECT_TRUE(d.Append(static_cast<double>(v) / 10).ok());
      }
      batches.push_back(
          arrow::RecordBatch::Make(schema, kRows, {Int64s(x), d.Finish().ValueOrDie(), Int64s(y)}));
    }
    return std::make_shared<MemoryTable>(schema, std::move(batches), split);
  }

  static plan::LogicalNodePtr Node(plan::LogicalNode node) {
    return std::make_shared<const plan::LogicalNode>(std::move(node));
  }

  static plan::LogicalNodePtr Scan(const std::shared_ptr<MemoryTable>& table) {
    return Node(plan::ScanNode{.table = table, .table_name = "t", .fields = {0, 1, 2}});
  }

  static plan::LogicalPlan PlanOf(plan::LogicalNodePtr root, std::size_t width) {
    plan::LogicalPlan plan{.root = std::move(root), .output = {}};
    for (std::size_t i = 0; i < width; ++i) {
      plan.output.push_back({.name = "c" + std::to_string(i), .type = LogicalType::kBigInt});
    }
    return plan;
  }

  // Runs the plan on `executor` (nullptr: one thread) in batches of 3 rows.
  static arrow::Result<std::shared_ptr<arrow::Table>> Run(const plan::LogicalPlan& plan,
                                                          arrow::internal::Executor* executor) {
    ARROW_ASSIGN_OR_RAISE(auto op, BuildPhysicalPlan(plan));
    ExecContext ctx{.pool = arrow::default_memory_pool(),
                    .batch_size = 3,
                    .executor = executor,
                    .threads = executor == nullptr ? 1 : kThreads};
    return Drain(*op, ctx);
  }

  static constexpr int64_t kParts = 20;
  static constexpr int64_t kRows = 7;
};

TEST_F(PartOperatorsTest, AggregatesAreTheSameOnAnyNumberOfThreads) {
  const auto pool = Pool();
  const auto aggregate = [](const plan::LogicalNodePtr& input) {
    const auto y = Column(2, "y", LogicalType::kBigInt);
    return Node(plan::AggregateNode{
        .input = input,
        .aggregates = {
            {.kind = plan::AggKind::kCountStar, .arg = {}, .type = LogicalType::kBigInt},
            {.kind = plan::AggKind::kCount, .arg = y, .type = LogicalType::kBigInt},
            {.kind = plan::AggKind::kMin, .arg = y, .type = LogicalType::kBigInt},
            {.kind = plan::AggKind::kMax, .arg = y, .type = LogicalType::kBigInt},
            {.kind = plan::AggKind::kCountDistinct, .arg = y, .type = LogicalType::kBigInt}}});
  };
  const auto serial = Run(PlanOf(aggregate(Scan(Table(/*split=*/false))), 5), nullptr);
  ASSERT_TRUE(serial.ok()) << serial.status().ToString();
  for (arrow::internal::Executor* executor :
       {static_cast<arrow::internal::Executor*>(nullptr),
        static_cast<arrow::internal::Executor*>(pool.get())}) {
    const auto table = Table();
    const auto result = Run(PlanOf(aggregate(Scan(table)), 5), executor);
    ASSERT_TRUE(result.ok()) << result.status().ToString();
    EXPECT_TRUE((*result)->Equals(**serial)) << (*result)->ToString();
    EXPECT_EQ(Int64Column(**result, 0), (std::vector<std::optional<int64_t>>{140}));
    EXPECT_EQ(Int64Column(**result, 1), (std::vector<std::optional<int64_t>>{112}));
    EXPECT_EQ(Int64Column(**result, 3), (std::vector<std::optional<int64_t>>{139}));
    EXPECT_EQ(table->scanned_parts().size(), static_cast<std::size_t>(kParts));
  }
}

// DOUBLE SUM and AVG add per part, then the part sums in part order: the same on any number of
// threads, and possibly other than one running sum by rounding.
TEST_F(PartOperatorsTest, DoubleSumsAddPerPartThenInOrder) {
  const auto pool = Pool();
  const auto d = Column(1, "d", LogicalType::kDouble);
  const auto aggregate = Node(plan::AggregateNode{
      .input = Scan(Table()),
      .aggregates = {{.kind = plan::AggKind::kSum, .arg = d, .type = LogicalType::kDouble},
                     {.kind = plan::AggKind::kAvg, .arg = d, .type = LogicalType::kDouble}}});
  double expected = 0;
  for (int64_t part = 0; part < kParts; ++part) {
    double sum = 0;
    for (int64_t i = 0; i < kRows; ++i) {
      sum += static_cast<double>((part * kRows) + i) / 10;
    }
    expected += sum;
  }
  for (arrow::internal::Executor* executor :
       {static_cast<arrow::internal::Executor*>(nullptr),
        static_cast<arrow::internal::Executor*>(pool.get())}) {
    const auto result = Run(PlanOf(aggregate, 2), executor);
    ASSERT_TRUE(result.ok()) << result.status().ToString();
    const auto& sum = static_cast<const arrow::DoubleArray&>(*(*result)->column(0)->chunk(0));
    const auto& avg = static_cast<const arrow::DoubleArray&>(*(*result)->column(1)->chunk(0));
    EXPECT_EQ(sum.Value(0), expected);
    EXPECT_EQ(avg.Value(0), expected / static_cast<double>(kParts * kRows));
  }
}

TEST_F(PartOperatorsTest, ProjectionsKeepPartOrder) {
  const auto pool = Pool();
  const auto table = Table();
  const auto filter =
      Node(plan::FilterNode{.input = Scan(table),
                            .predicates = {testing::Compare(Column(2, "y", LogicalType::kBigInt),
                                                            plan::CompareOp::kGe, BigInt(0))}});
  const auto project =
      Node(plan::ProjectNode{.input = filter, .columns = {Column(0, "x", LogicalType::kBigInt)}});
  std::vector<std::optional<int64_t>> expected;
  for (int64_t v = 0; v < kParts * kRows; ++v) {
    if (v % 5 != 0) {
      expected.emplace_back(v);
    }
  }
  for (arrow::internal::Executor* executor :
       {static_cast<arrow::internal::Executor*>(nullptr),
        static_cast<arrow::internal::Executor*>(pool.get())}) {
    const auto result = Run(PlanOf(project, 1), executor);
    ASSERT_TRUE(result.ok()) << result.status().ToString();
    EXPECT_EQ(Int64Column(**result), expected);
  }
}

// A LIMIT met by the first parts stops scheduling: on one thread no later part is scanned, on the
// pool only the parts within the window (2 * threads beyond the taken ones).
TEST_F(PartOperatorsTest, LimitStopsSchedulingParts) {
  const auto pool = Pool();
  for (arrow::internal::Executor* executor :
       {static_cast<arrow::internal::Executor*>(nullptr),
        static_cast<arrow::internal::Executor*>(pool.get())}) {
    const auto table = Table();
    const auto limit = Node(plan::LimitNode{.input = Scan(table), .limit = 4, .offset = 1});
    const auto project =
        Node(plan::ProjectNode{.input = limit, .columns = {Column(0, "x", LogicalType::kBigInt)}});
    const auto result = Run(PlanOf(project, 1), executor);
    ASSERT_TRUE(result.ok()) << result.status().ToString();
    EXPECT_EQ(Int64Column(**result), (std::vector<std::optional<int64_t>>{1, 2, 3, 4}));
    const std::vector<int64_t> scanned = table->scanned_parts();
    ASSERT_FALSE(scanned.empty());
    if (executor == nullptr) {
      EXPECT_EQ(scanned, std::vector<int64_t>{0});
    } else {
      EXPECT_LE(scanned.back(), 2 * kThreads);
    }
  }
}

// The first failing part in part order decides the error; a part after the rows a LIMIT needs is
// never reported.
TEST_F(PartOperatorsTest, ErrorsFollowPartOrder) {
  const auto pool = Pool();
  for (arrow::internal::Executor* executor :
       {static_cast<arrow::internal::Executor*>(nullptr),
        static_cast<arrow::internal::Executor*>(pool.get())}) {
    const auto table = Table();
    table->FailPart(3);
    const auto aggregate = Node(plan::AggregateNode{
        .input = Scan(table),
        .aggregates = {
            {.kind = plan::AggKind::kCountStar, .arg = {}, .type = LogicalType::kBigInt}}});
    const auto failed = Run(PlanOf(aggregate, 1), executor);
    EXPECT_TRUE(failed.status().IsIOError()) << failed.status().ToString();
    EXPECT_EQ(failed.status().message(), "part 3 is broken");

    const auto limit = Node(plan::LimitNode{.input = Scan(table), .limit = 2});
    const auto project =
        Node(plan::ProjectNode{.input = limit, .columns = {Column(0, "x", LogicalType::kBigInt)}});
    const auto limited = Run(PlanOf(project, 1), executor);
    ASSERT_TRUE(limited.ok()) << limited.status().ToString();
    EXPECT_EQ(Int64Column(**limited), (std::vector<std::optional<int64_t>>{0, 1}));
  }
}

// Blocking operators other than a global aggregate read the part union: grouping and sorting see
// the parts' rows in part order, so their results are the same on any number of threads.
TEST_F(PartOperatorsTest, GroupingAndSortingReadThePartsInOrder) {
  const auto pool = Pool();
  const auto y = Column(2, "y", LogicalType::kBigInt);
  const auto x = Column(0, "x", LogicalType::kBigInt);
  const auto grouped = [&](const plan::LogicalNodePtr& input) {
    return Node(plan::GroupAggregateNode{
        .input = input,
        .keys = {y},
        .aggregates = {
            {.kind = plan::AggKind::kCountStar, .arg = {}, .type = LogicalType::kBigInt}}});
  };
  const auto sorted = [&](const plan::LogicalNodePtr& input) {
    const auto sort =
        Node(plan::SortNode{.input = input, .keys = {{.column = y, .descending = true}}});
    const auto top = Node(plan::LimitNode{.input = sort, .limit = 30});
    return Node(plan::ProjectNode{.input = top, .columns = {x}});
  };
  const auto same_as_serial = [&](const auto& shape, std::size_t width) {
    const auto serial = Run(PlanOf(shape(Scan(Table(/*split=*/false))), width), nullptr);
    ASSERT_TRUE(serial.ok()) << serial.status().ToString();
    for (arrow::internal::Executor* executor :
         {static_cast<arrow::internal::Executor*>(nullptr),
          static_cast<arrow::internal::Executor*>(pool.get())}) {
      const auto result = Run(PlanOf(shape(Scan(Table())), width), executor);
      ASSERT_TRUE(result.ok()) << result.status().ToString();
      EXPECT_TRUE((*result)->Equals(**serial)) << (*result)->ToString();
    }
  };
  same_as_serial(grouped, 2);
  same_as_serial(sorted, 1);
}

}  // namespace
}  // namespace antb1::exec
