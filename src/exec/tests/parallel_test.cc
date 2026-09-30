// Parallel execution over table parts (docs/adr/0013-parallel-execution.md): the part scheduler
// and the part operators the physical planner builds, on one thread and on a 4-thread pool.

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <arrow/compute/api.h>
#include <arrow/util/thread_pool.h>
#include <gtest/gtest.h>

#include "antb1/exec/filter.h"
#include "antb1/exec/group_aggregate.h"
#include "antb1/exec/memory_budget.h"
#include "antb1/exec/physical_planner.h"
#include "antb1/exec/profile.h"
#include "antb1/exec/scalar_aggregate.h"
#include "antb1/exec/sort.h"
#include "antb1/exec/table_scan.h"
#include "antb1/plan/logical_plan.h"

#include "../group_table.h"
#include "../part_operators.h"
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

  // The consumer frees what it holds before the part runs again alone, once.
  on_caller->clear();
  int retries = 0;
  PartScheduler<int64_t> hooked(10, task, pool.get(), 4);
  hooked.set_before_retry([&] {
    EXPECT_TRUE(on_caller->empty()) << "before the part runs again";
    ++retries;
  });
  for (int64_t part = 0; part < 10; ++part) {
    ASSERT_TRUE(hooked.Next().ok());
  }
  EXPECT_EQ(retries, 1);

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

// Whether two tables hold the same values in the same order, NaN equal to NaN.
bool SameRows(const arrow::Table& a, const arrow::Table& b) {
  if (!a.schema()->Equals(*b.schema()) || a.num_rows() != b.num_rows()) {
    return false;
  }
  const auto options = arrow::EqualOptions::Defaults().nans_equal(true);
  for (int c = 0; c < a.num_columns(); ++c) {
    if (!a.column(c)->Equals(*b.column(c), options)) {
      return false;
    }
  }
  return true;
}

// The rows of a table sorted by all its columns, for comparing results whose row order SQL
// leaves open.
std::shared_ptr<arrow::Table> SortedRows(const std::shared_ptr<arrow::Table>& table) {
  std::vector<arrow::compute::SortKey> keys;
  for (const auto& field : table->schema()->fields()) {
    keys.emplace_back(field->name());
  }
  const auto indices = arrow::compute::SortIndices(arrow::Datum(table),
                                                   arrow::compute::SortOptions(std::move(keys)));
  EXPECT_TRUE(indices.ok()) << indices.status().ToString();
  const auto sorted = arrow::compute::Take(table, *indices);
  EXPECT_TRUE(sorted.ok()) << sorted.status().ToString();
  return sorted->table();
}

// Grouping runs per part and merges the parts' groups in part order; sorting reads the part union.
// Both give the same result on any number of threads, byte for byte, and the serial operators'
// rows (the group order follows the parts' batches, which SQL leaves open).
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
  const auto same_as_serial = [&](const auto& shape, std::size_t width, bool ordered) {
    const auto serial = Run(PlanOf(shape(Scan(Table(/*split=*/false))), width), nullptr);
    ASSERT_TRUE(serial.ok()) << serial.status().ToString();
    const auto one_thread = Run(PlanOf(shape(Scan(Table())), width), nullptr);
    ASSERT_TRUE(one_thread.ok()) << one_thread.status().ToString();
    const auto four_threads = Run(PlanOf(shape(Scan(Table())), width), pool.get());
    ASSERT_TRUE(four_threads.ok()) << four_threads.status().ToString();
    EXPECT_TRUE((*four_threads)->Equals(**one_thread)) << (*four_threads)->ToString();
    if (ordered) {
      EXPECT_TRUE((*one_thread)->Equals(**serial)) << (*one_thread)->ToString();
    } else {
      EXPECT_TRUE(SortedRows(*one_thread)->Equals(*SortedRows(*serial)));
    }
  };
  same_as_serial(grouped, 2, /*ordered=*/false);
  same_as_serial(sorted, 1, /*ordered=*/true);
}

// ---- GROUP BY per part ----

// A table of `parts` parts: key k (DOUBLE, with -0.0, 0.0 and NaNs of both signs, and NULL),
// group g (BIGINT, NULL every seventh row), s (VARCHAR) and v (BIGINT).
std::shared_ptr<MemoryTable> GroupingTable(bool split) {
  const auto schema =
      arrow::schema({arrow::field("k", arrow::float64()), arrow::field("g", arrow::int64()),
                     arrow::field("s", arrow::binary()), arrow::field("v", arrow::int64()),
                     arrow::field("d", arrow::float64())});
  const std::vector<double> doubles = {-0.0, 1.5, 0.0, std::nan(""), -std::nan(""), 2.5, 1.5};
  arrow::RecordBatchVector batches;
  for (int64_t part = 0; part < 12; ++part) {
    arrow::DoubleBuilder k;
    std::vector<std::optional<int64_t>> g;
    std::vector<std::optional<std::string>> str;
    std::vector<std::optional<int64_t>> v;
    arrow::DoubleBuilder d;
    const int64_t rows = part % 4 == 3 ? 0 : 9;  // some parts are empty
    for (int64_t i = 0; i < rows; ++i) {
      const int64_t row = (part * 9) + i;
      EXPECT_TRUE(
          (row % 11 == 0 ? k.AppendNull() : k.Append(doubles[static_cast<std::size_t>(row % 7)]))
              .ok());
      g.push_back(row % 7 == 0 ? std::nullopt : std::optional<int64_t>((row * 5) % 13));
      str.emplace_back(std::string(1, static_cast<char>('a' + (row % 5))));
      v.emplace_back(row);
      EXPECT_TRUE(d.Append(static_cast<double>(row) * 0.1).ok());  // sums round by order
    }
    batches.push_back(
        arrow::RecordBatch::Make(schema, rows,
                                 {k.Finish().ValueOrDie(), Int64s(g), testing::Strings(str),
                                  Int64s(v), d.Finish().ValueOrDie()}));
  }
  if (!split) {
    auto combined = arrow::Table::FromRecordBatches(schema, batches).ValueOrDie();
    auto one = combined->CombineChunksToBatch().ValueOrDie();
    return std::make_shared<MemoryTable>(schema, arrow::RecordBatchVector{one}, false);
  }
  return std::make_shared<MemoryTable>(schema, std::move(batches), true);
}

// Grouping per part and merging the parts in order gives the groups of one pass (as a set of
// rows) for every aggregate and key type, NULL keys, no keys and empty parts; the same rows in the
// same order on any number of threads; and a DOUBLE key keeps its spelling from the earliest part.
TEST_F(PartOperatorsTest, GroupingPerPartMergesInPartOrder) {
  const auto pool = Pool();
  const auto k = Column(0, "k", LogicalType::kDouble);
  const auto g = Column(1, "g", LogicalType::kBigInt);
  const auto str = Column(2, "s", LogicalType::kVarchar);
  const auto v = Column(3, "v", LogicalType::kBigInt);
  const std::vector<plan::AggregateCall> calls = {
      {.kind = plan::AggKind::kCountStar, .arg = {}, .type = LogicalType::kBigInt},
      {.kind = plan::AggKind::kCount, .arg = g, .type = LogicalType::kBigInt},
      {.kind = plan::AggKind::kSum, .arg = v, .type = LogicalType::kHugeInt},
      {.kind = plan::AggKind::kMin, .arg = str, .type = LogicalType::kVarchar},
      {.kind = plan::AggKind::kMax, .arg = k, .type = LogicalType::kDouble},
      {.kind = plan::AggKind::kCountDistinct, .arg = str, .type = LogicalType::kBigInt}};
  for (const std::vector<plan::BoundColumn>& keys :
       {std::vector<plan::BoundColumn>{k}, std::vector<plan::BoundColumn>{g, str},
        std::vector<plan::BoundColumn>{}}) {
    const auto grouped = [&](const std::shared_ptr<MemoryTable>& table) {
      const auto scan =
          Node(plan::ScanNode{.table = table, .table_name = "t", .fields = {0, 1, 2, 3}});
      return PlanOf(
          Node(plan::GroupAggregateNode{.input = scan, .keys = keys, .aggregates = calls}),
          keys.size() + calls.size());
    };
    const auto serial = Run(grouped(GroupingTable(false)), nullptr);
    ASSERT_TRUE(serial.ok()) << serial.status().ToString();
    const auto one_thread = Run(grouped(GroupingTable(true)), nullptr);
    ASSERT_TRUE(one_thread.ok()) << one_thread.status().ToString();
    const auto four_threads = Run(grouped(GroupingTable(true)), pool.get());
    ASSERT_TRUE(four_threads.ok()) << four_threads.status().ToString();
    EXPECT_TRUE(SameRows(**four_threads, **one_thread)) << keys.size();
    EXPECT_TRUE(SameRows(*SortedRows(*one_thread), *SortedRows(*serial)))
        << keys.size() << "\n"
        << (*one_thread)->ToString() << "\n"
        << (*serial)->ToString();
  }
  // Row 2 (0.0) comes before row 7 (-0.0), row 3 (NaN) before row 4 (-NaN): the groups keep the
  // first spelling.
  const auto scan =
      Node(plan::ScanNode{.table = GroupingTable(true), .table_name = "t", .fields = {0}});
  const auto result =
      Run(PlanOf(Node(plan::GroupAggregateNode{.input = scan,
                                               .keys = {Column(0, "k", LogicalType::kDouble)},
                                               .aggregates = {{.kind = plan::AggKind::kCountStar,
                                                               .arg = {},
                                                               .type = LogicalType::kBigInt}}}),
                 2),
          pool.get());
  ASSERT_TRUE(result.ok()) << result.status().ToString();
  const auto keys = (*result)->column(0);
  int zeros = 0;
  int nans = 0;
  for (const auto& chunk : keys->chunks()) {
    const auto& values = static_cast<const arrow::DoubleArray&>(*chunk);
    for (int64_t i = 0; i < values.length(); ++i) {
      if (values.IsNull(i)) {
        continue;
      }
      if (values.Value(i) == 0) {
        ++zeros;
        EXPECT_FALSE(std::signbit(values.Value(i))) << "0.0 comes first";
      }
      if (std::isnan(values.Value(i))) {
        ++nans;
        EXPECT_FALSE(std::signbit(values.Value(i))) << "NaN comes before -NaN";
      }
    }
  }
  EXPECT_EQ(zeros, 1);
  EXPECT_EQ(nans, 1);
}

TEST_F(PartOperatorsTest, GroupingErrorsFollowPartOrder) {
  const auto pool = Pool();
  for (arrow::internal::Executor* executor :
       {static_cast<arrow::internal::Executor*>(nullptr),
        static_cast<arrow::internal::Executor*>(pool.get())}) {
    const auto table = Table();
    table->FailPart(5);
    const auto grouped = Node(plan::GroupAggregateNode{
        .input = Scan(table),
        .keys = {Column(2, "y", LogicalType::kBigInt)},
        .aggregates = {
            {.kind = plan::AggKind::kCountStar, .arg = {}, .type = LogicalType::kBigInt}}});
    const auto failed = Run(PlanOf(grouped, 2), executor);
    EXPECT_TRUE(failed.status().IsIOError()) << failed.status().ToString();
    EXPECT_EQ(failed.status().message(), "part 5 is broken");
  }
}

// A grouped DOUBLE SUM or AVG adds per part, then the parts in order: the same bytes on any number
// of threads, and one running sum up to rounding.
TEST_F(PartOperatorsTest, GroupedDoubleSumsAddPerPart) {
  const auto pool = Pool();
  const auto d = Column(4, "d", LogicalType::kDouble);
  const auto grouped = [&](const std::shared_ptr<MemoryTable>& table) {
    const auto scan =
        Node(plan::ScanNode{.table = table, .table_name = "t", .fields = {0, 1, 2, 3, 4}});
    return PlanOf(
        Node(plan::GroupAggregateNode{
            .input = scan,
            .keys = {Column(1, "g", LogicalType::kBigInt)},
            .aggregates = {{.kind = plan::AggKind::kSum, .arg = d, .type = LogicalType::kDouble},
                           {.kind = plan::AggKind::kAvg, .arg = d, .type = LogicalType::kDouble}}}),
        3);
  };
  const auto serial = Run(grouped(GroupingTable(false)), nullptr);
  const auto one_thread = Run(grouped(GroupingTable(true)), nullptr);
  const auto four_threads = Run(grouped(GroupingTable(true)), pool.get());
  ASSERT_TRUE(serial.ok() && one_thread.ok() && four_threads.ok());
  EXPECT_TRUE(SameRows(**four_threads, **one_thread));
  // Per key, the sums agree with the running sums up to rounding.
  const auto sums_by_key = [](const arrow::Table& table) {
    std::map<std::optional<int64_t>, std::pair<double, double>> out;
    const auto combined = table.CombineChunksToBatch().ValueOrDie();
    const auto& keys = static_cast<const arrow::Int64Array&>(*combined->column(0));
    const auto& sums = static_cast<const arrow::DoubleArray&>(*combined->column(1));
    const auto& avgs = static_cast<const arrow::DoubleArray&>(*combined->column(2));
    for (int64_t i = 0; i < combined->num_rows(); ++i) {
      out[keys.IsNull(i) ? std::nullopt : std::optional(keys.Value(i))] = {sums.Value(i),
                                                                           avgs.Value(i)};
    }
    return out;
  };
  const auto parts = sums_by_key(**one_thread);
  const auto running = sums_by_key(**serial);
  ASSERT_EQ(parts.size(), running.size());
  for (const auto& [key, values] : parts) {
    const auto& expected = running.at(key);
    EXPECT_NEAR(values.first, expected.first, 1e-9 * std::abs(expected.first));
    EXPECT_NEAR(values.second, expected.second, 1e-9 * std::abs(expected.second));
  }
}

// A merge adds a part's new groups in chunks of at most GroupTable::kMaxMergeChunk, taking each
// group's keys from the part's chunk that holds them: no array grows with the part.
TEST_F(PartOperatorsTest, MergedGroupsComeInBoundedChunks) {
  const auto x = Column(0, "x", LogicalType::kBigInt);
  const auto s = Column(1, "s", LogicalType::kVarchar);
  const std::vector<plan::AggregateCall> calls = {
      {.kind = plan::AggKind::kCountStar, .arg = {}, .type = LogicalType::kBigInt}};
  const auto schema =
      arrow::schema({arrow::field("key0", arrow::int64()), arrow::field("key1", arrow::binary()),
                     arrow::field("agg0", arrow::int64())});
  auto part = GroupTable::Make({x, s}, calls, 2, arrow::default_memory_pool(), nullptr);
  ASSERT_TRUE(part.ok()) << part.status().ToString();
  constexpr int64_t kGroups = 150'000;
  constexpr int64_t kBatch = 50'000;
  for (int64_t start = 0; start < kGroups; start += kBatch) {  // three chunks of new groups
    std::vector<std::optional<int64_t>> xs;
    std::vector<std::optional<std::string>> ss;
    xs.reserve(kBatch);
    ss.reserve(kBatch);
    for (int64_t i = start; i < start + kBatch; ++i) {
      xs.emplace_back(kGroups - i);
      ss.emplace_back(std::to_string(i % 97));
    }
    const auto batch = arrow::RecordBatch::Make(
        arrow::schema({arrow::field("x", arrow::int64()), arrow::field("s", arrow::binary())}),
        kBatch, {Int64s(xs), testing::Strings(ss)});
    ASSERT_TRUE((*part)->Consume(*batch).ok());
  }
  auto merged = GroupTable::Make({x, s}, calls, 2, arrow::default_memory_pool(), nullptr);
  ASSERT_TRUE(merged.ok());
  ASSERT_TRUE((*merged)->Merge(**part).ok());
  EXPECT_EQ((*merged)->num_groups(), kGroups);
  const auto drain = [&](GroupTable& table) {
    arrow::RecordBatchVector out;
    while (true) {
      auto chunk = table.NextChunk(schema);
      EXPECT_TRUE(chunk.ok()) << chunk.status().ToString();
      if (!chunk.ok() || *chunk == nullptr) {
        return out;
      }
      EXPECT_TRUE((*chunk)->ValidateFull().ok());
      out.push_back(*chunk);
    }
  };
  const arrow::RecordBatchVector chunks = drain(**merged);
  std::vector<int64_t> sizes;
  sizes.reserve(chunks.size());
  for (const auto& chunk : chunks) {
    sizes.push_back(chunk->num_rows());
  }
  EXPECT_EQ(sizes, (std::vector<int64_t>{65536, 65536, 18928}));
  // The same groups, keys and counts as the part itself.
  const auto a = arrow::Table::FromRecordBatches(schema, chunks).ValueOrDie();
  const auto b = arrow::Table::FromRecordBatches(schema, drain(**part)).ValueOrDie();
  EXPECT_TRUE(SameRows(*SortedRows(a), *SortedRows(b)));
}

// The partition of a group depends only on its key: the same on every run and build (with this
// Arrow version), whatever the order of the groups or the number of threads. A change here changes
// the order of GROUP BY results; update the golden only on purpose.
TEST_F(PartOperatorsTest, PartitionsFollowTheKeyHash) {
  const auto partitions_of = [](const plan::BoundColumn& key,
                                const std::shared_ptr<arrow::Array>& column) {
    auto table = GroupTable::Make({key}, {}, 1, arrow::default_memory_pool(), nullptr);
    EXPECT_TRUE(table.ok());
    const auto batch = arrow::RecordBatch::Make(arrow::schema({arrow::field("k", column->type())}),
                                                column->length(), {column});
    EXPECT_TRUE((*table)->Consume(*batch).ok());
    EXPECT_TRUE((*table)->Partition().ok());
    EXPECT_EQ((*table)->num_partitions(), GroupTable::kPartitions);
    std::vector<std::size_t> out(static_cast<std::size_t>(column->length()));
    for (std::size_t p = 0; p < GroupTable::kPartitions; ++p) {
      for (const std::uint32_t group : (*table)->partition_groups(p)) {
        out.at(group) = p;
      }
    }
    return out;
  };
  const auto ints = partitions_of(Column(0, "k", LogicalType::kBigInt),
                                  Int64s({1, 2, 3, 1000000, -5, std::nullopt}));
  const auto strings =
      partitions_of(Column(0, "k", LogicalType::kVarchar),
                    testing::Strings({"a", "b", "", "https://example.org", std::nullopt}));
  std::string golden;
  for (const std::size_t p : ints) {
    golden += std::to_string(p) + " ";
  }
  golden += "| ";
  for (const std::size_t p : strings) {
    golden += std::to_string(p) + " ";
  }
  EXPECT_EQ(golden, "24 13 0 22 60 12 | 26 30 37 35 12 ");
}

// A partition merge that fails on the executor (here: a HUGEINT sum past its range, reached only
// when two parts' sums are added) fails the query with the same error on any number of threads;
// a bad partition is Invalid; the operator is Invalid after Close.
TEST_F(PartOperatorsTest, PartitionMergeErrors) {
  const auto pool = Pool();
  const auto schema = arrow::schema(
      {arrow::field("k", arrow::int64()), arrow::field("h", arrow::decimal128(38, 0))});
  arrow::RecordBatchVector batches;
  for (int part = 0; part < 2; ++part) {
    arrow::Decimal128Builder h(arrow::decimal128(38, 0));
    ASSERT_TRUE(h.Append(arrow::Decimal128("60000000000000000000000000000000000000")).ok());
    batches.push_back(arrow::RecordBatch::Make(schema, 1, {Int64s({1}), h.Finish().ValueOrDie()}));
  }
  const auto table = std::make_shared<MemoryTable>(schema, batches, /*split=*/true);
  const auto plan =
      PlanOf(Node(plan::GroupAggregateNode{
                 .input = Node(plan::ScanNode{.table = table, .table_name = "t", .fields = {0, 1}}),
                 .keys = {Column(0, "k", LogicalType::kBigInt)},
                 .aggregates = {{.kind = plan::AggKind::kSum,
                                 .arg = Column(1, "h", LogicalType::kHugeInt),
                                 .type = LogicalType::kHugeInt}}}),
             2);
  const auto serial = Run(plan, nullptr);
  const auto parallel = Run(plan, pool.get());
  EXPECT_TRUE(serial.status().IsExecutionError()) << serial.status().ToString();
  EXPECT_EQ(parallel.status().ToString(), serial.status().ToString());

  auto part = GroupTable::Make({Column(0, "k", LogicalType::kBigInt)}, {}, 2,
                               arrow::default_memory_pool(), nullptr);
  ASSERT_TRUE(part.ok());
  ASSERT_TRUE((*part)->Consume(*batches[0]).ok());
  ASSERT_TRUE((*part)->Partition().ok());
  auto merged = GroupTable::Make({Column(0, "k", LogicalType::kBigInt)}, {}, 2,
                                 arrow::default_memory_pool(), nullptr);
  ASSERT_TRUE(merged.ok());
  EXPECT_TRUE((*merged)->MergePartition(**part, GroupTable::kPartitions).IsInvalid());

  auto op = BuildPhysicalPlan(plan);
  ASSERT_TRUE(op.ok());
  ExecContext ctx;
  ASSERT_TRUE((*op)->Open(ctx).ok());
  ASSERT_TRUE((*op)->Close().ok());
  EXPECT_TRUE((*op)->Next().status().IsInvalid());
}

// A merge failure (a HUGEINT sum overflowing when part 1 merges into part 0) and a failed read of
// part i: the query reports the earlier part's failure, the same on any number of threads.
TEST_F(PartOperatorsTest, MergeAndReadFailuresFollowPartOrder) {
  const auto pool = Pool();
  const auto schema = arrow::schema(
      {arrow::field("k", arrow::int64()), arrow::field("h", arrow::decimal128(38, 0))});
  arrow::RecordBatchVector batches;
  for (int part = 0; part < 6; ++part) {
    arrow::Decimal128Builder h(arrow::decimal128(38, 0));
    ASSERT_TRUE(h.Append(arrow::Decimal128("60000000000000000000000000000000000000")).ok());
    batches.push_back(arrow::RecordBatch::Make(schema, 1, {Int64s({1}), h.Finish().ValueOrDie()}));
  }
  for (const int64_t failing : {0, 1, 3, 5}) {
    const auto table = std::make_shared<MemoryTable>(schema, batches, /*split=*/true);
    table->FailPart(failing);
    const auto plan = PlanOf(
        Node(plan::GroupAggregateNode{
            .input = Node(plan::ScanNode{.table = table, .table_name = "t", .fields = {0, 1}}),
            .keys = {Column(0, "k", LogicalType::kBigInt)},
            .aggregates = {{.kind = plan::AggKind::kSum,
                            .arg = Column(1, "h", LogicalType::kHugeInt),
                            .type = LogicalType::kHugeInt}}}),
        2);
    const auto serial = Run(plan, nullptr);
    const auto parallel = Run(plan, pool.get());
    // Parts 0 and 1 fail to read before part 1 would merge; later reads come after the overflow.
    if (failing <= 1) {
      EXPECT_FALSE(serial.status().IsExecutionError()) << serial.status().ToString();
    } else {
      EXPECT_TRUE(serial.status().IsExecutionError())
          << failing << ": " << serial.status().ToString();
    }
    EXPECT_EQ(parallel.status().ToString(), serial.status().ToString()) << failing;
  }
}

// ORDER BY ... LIMIT over parts keeps each part's first rows and merges them in part order: the
// window and its tie order are the serial top-N's, byte for byte, on any number of threads, for
// windows inside, across and past the rows; errors come in part order; memory is budgeted.
TEST_F(PartOperatorsTest, TopNKeepsEachPartsFirstRows) {
  const auto pool = Pool();
  const auto y = Column(2, "y", LogicalType::kBigInt);  // NULL every fifth row: ties at NULL
  const auto keys = [&](bool descending, bool nulls_first) {
    return std::vector<plan::SortKey>{
        {.column = y, .descending = descending, .nulls_first = nulls_first}};
  };
  const auto top = [&](const std::shared_ptr<MemoryTable>& table, int64_t limit, int64_t offset,
                       bool descending, bool nulls_first) {
    const auto sort =
        Node(plan::SortNode{.input = Scan(table), .keys = keys(descending, nulls_first)});
    return PlanOf(Node(plan::LimitNode{.input = sort, .limit = limit, .offset = offset}), 3);
  };
  // The reference: the serial top-N SortOperator over one scan of the whole table.
  const auto serial_top = [&](int64_t limit, int64_t offset, bool descending, bool nulls_first) {
    SortOperator op(std::make_unique<TableScanOperator>(Table(false), std::vector<int>{0, 1, 2}),
                    keys(descending, nulls_first), limit, offset);
    ExecContext ctx{.pool = arrow::default_memory_pool(), .batch_size = 3};
    return Drain(op, ctx);
  };
  for (const auto& [limit, offset] :
       std::vector<std::pair<int64_t, int64_t>>{{1, 0},
                                                {5, 3},
                                                {40, 20},
                                                {30, 125},
                                                {10, 200},
                                                {std::numeric_limits<int64_t>::max(), 110}}) {
    for (const bool descending : {false, true}) {
      for (const bool nulls_first : {false, true}) {
        const auto serial = serial_top(limit, offset, descending, nulls_first);
        const auto one_thread = Run(top(Table(), limit, offset, descending, nulls_first), nullptr);
        const auto four_threads =
            Run(top(Table(), limit, offset, descending, nulls_first), pool.get());
        ASSERT_TRUE(serial.ok() && one_thread.ok() && four_threads.ok())
            << serial.status().ToString() << one_thread.status().ToString()
            << four_threads.status().ToString();
        EXPECT_TRUE(SameRows(**one_thread, **serial)) << limit << " " << offset;
        EXPECT_TRUE(SameRows(**four_threads, **one_thread)) << limit << " " << offset;
      }
    }
  }
  for (arrow::internal::Executor* executor :
       {static_cast<arrow::internal::Executor*>(nullptr),
        static_cast<arrow::internal::Executor*>(pool.get())}) {
    const auto table = Table();
    table->FailPart(4);
    const auto failed = Run(top(table, 5, 0, false, false), executor);
    EXPECT_TRUE(failed.status().IsIOError()) << failed.status().ToString();
    EXPECT_EQ(failed.status().message(), "part 4 is broken");

    MemoryBudget tiny(512);
    auto op = BuildPhysicalPlan(top(Table(), 50, 0, false, false));
    ASSERT_TRUE(op.ok());
    ExecContext ctx{.pool = &tiny,
                    .batch_size = 3,
                    .executor = executor,
                    .threads = executor == nullptr ? 1 : kThreads,
                    .budget = &tiny};
    const auto oom = Drain(**op, ctx);
    EXPECT_TRUE(oom.status().IsOutOfMemory()) << oom.status().ToString();
    op->reset();
    EXPECT_EQ(tiny.bytes_allocated(), 0);
  }
}

// ORDER BY ... LIMIT over a partitioned GROUP BY: each partition keeps only its first rows in the
// top-N's order (PartitionTopN), and the result, ties in their order included, is the top-N of
// every group: rows [offset, offset + limit) of the stably sorted groups, on any number of threads.
TEST_F(PartOperatorsTest, GroupedTopNIsTheTopNOfAllGroups) {
  const auto pool = Pool();
  // Many groups per partition (so that a partition drops rows): 12 parts of 250 rows, x = 0..2999,
  // y = x % 1500 (NULL where it is a multiple of 7), so 1 to 3 rows per group and many ties.
  const auto table = [] {
    const auto schema =
        arrow::schema({arrow::field("x", arrow::int64()), arrow::field("d", arrow::float64()),
                       arrow::field("y", arrow::int64())});
    arrow::RecordBatchVector batches;
    for (int64_t part = 0; part < 12; ++part) {
      std::vector<std::optional<int64_t>> xs;
      std::vector<std::optional<int64_t>> ys;
      arrow::DoubleBuilder d;
      for (int64_t i = 0; i < 250; ++i) {
        const int64_t v = (part * 250) + i;
        xs.emplace_back(v);
        ys.emplace_back((v % 1500) % 7 == 0 ? std::nullopt : std::optional((v * 7) % 1500));
        EXPECT_TRUE(d.Append(0.5).ok());
      }
      batches.push_back(
          arrow::RecordBatch::Make(schema, 250, {Int64s(xs), d.Finish().ValueOrDie(), Int64s(ys)}));
    }
    return std::make_shared<MemoryTable>(schema, std::move(batches), /*split=*/true);
  };
  const auto x = Column(0, "x", LogicalType::kBigInt);
  const auto y = Column(2, "y", LogicalType::kBigInt);
  const auto grouped = [&](const std::shared_ptr<MemoryTable>& t) {
    return Node(plan::GroupAggregateNode{
        .input = Scan(t),
        .keys = {y},
        .aggregates = {{.kind = plan::AggKind::kCountStar, .arg = {}, .type = LogicalType::kBigInt},
                       {.kind = plan::AggKind::kMin, .arg = x, .type = LogicalType::kBigInt}}});
  };
  // Over the aggregation's output: y 0, COUNT(*) 1, MIN(x) 2. COUNT(*) is 1 to 3 (the NULL group
  // more): most groups tie on it.
  const auto key = Column(0, "y", LogicalType::kBigInt);
  const auto count = Column(1, "c", LogicalType::kBigInt);
  const auto low = Column(2, "m", LogicalType::kBigInt);
  // COUNT(*) ascending: every group but the NULL one ties, so the first rows all come from the
  // first partition, which must keep all of them.
  const std::vector<std::vector<plan::SortKey>> orders = {
      {{.column = count}},
      {{.column = count, .descending = true}},
      {{.column = count}, {.column = low, .descending = true}},
      {{.column = key, .nulls_first = true}},
      {{.column = key, .descending = true}}};
  for (const auto& order : orders) {
    const auto sorted =
        Run(PlanOf(Node(plan::SortNode{.input = grouped(table()), .keys = order}), 3), pool.get());
    ASSERT_TRUE(sorted.ok()) << sorted.status().ToString();
    for (const auto& [limit, offset] : std::vector<std::pair<int64_t, int64_t>>{
             {1, 0}, {3, 2}, {10, 0}, {40, 30}, {50, 1300}, {10, 2000}}) {
      const auto plan =
          PlanOf(Node(plan::LimitNode{
                     .input = Node(plan::SortNode{.input = grouped(table()), .keys = order}),
                     .limit = limit,
                     .offset = offset}),
                 3);
      const int64_t begin = std::min(offset, (*sorted)->num_rows());
      const auto expected = (*sorted)->Slice(begin, limit);
      for (arrow::internal::Executor* executor :
           {static_cast<arrow::internal::Executor*>(nullptr),
            static_cast<arrow::internal::Executor*>(pool.get())}) {
        const auto result = Run(plan, executor);
        ASSERT_TRUE(result.ok()) << result.status().ToString();
        EXPECT_TRUE(SameRows(**result, *expected))
            << order.size() << " keys, limit " << limit << " offset " << offset << "\n"
            << (*result)->ToString() << "\nexpected\n"
            << expected->ToString();
      }
    }
  }
  // Out of memory: the query fails and every byte comes back.
  for (arrow::internal::Executor* executor :
       {static_cast<arrow::internal::Executor*>(nullptr),
        static_cast<arrow::internal::Executor*>(pool.get())}) {
    MemoryBudget tiny(512);
    auto op = BuildPhysicalPlan(
        PlanOf(Node(plan::LimitNode{
                   .input = Node(plan::SortNode{.input = grouped(table()), .keys = orders.front()}),
                   .limit = 5}),
               3));
    ASSERT_TRUE(op.ok());
    ExecContext ctx{.pool = &tiny,
                    .batch_size = 3,
                    .executor = executor,
                    .threads = executor == nullptr ? 1 : kThreads,
                    .budget = &tiny};
    const auto oom = Drain(**op, ctx);
    EXPECT_TRUE(oom.status().IsOutOfMemory()) << oom.status().ToString();
    op->reset();
    EXPECT_EQ(tiny.bytes_allocated(), 0);
  }
}

// A global aggregation of only COUNT(DISTINCT x) is planned as a GROUP BY of x (merged in parallel,
// partitioned) and COUNT(x) over the groups: the counts of the serial COUNT(DISTINCT) state, for
// DOUBLE (-0.0 with 0.0, one NaN), BIGINT with NULLs, VARCHAR, repeated calls and no rows, on any
// number of threads.
TEST_F(PartOperatorsTest, CountDistinctAloneIsAParallelGroupBy) {
  const auto pool = Pool();
  const std::vector<plan::BoundColumn> columns = {Column(0, "k", LogicalType::kDouble),
                                                  Column(1, "g", LogicalType::kBigInt),
                                                  Column(2, "s", LogicalType::kVarchar)};
  for (const plan::BoundColumn& column : columns) {
    const std::vector<plan::AggregateCall> calls = {
        {.kind = plan::AggKind::kCountDistinct, .arg = column, .type = LogicalType::kBigInt},
        {.kind = plan::AggKind::kCountDistinct, .arg = column, .type = LogicalType::kBigInt}};
    for (const bool empty : {false, true}) {
      const auto table = [&](bool split) {
        if (!empty) {
          return GroupingTable(split);
        }
        const auto full = GroupingTable(false);
        return std::make_shared<MemoryTable>(full->schema(), arrow::RecordBatchVector{}, split);
      };
      ScalarAggregateOperator serial(
          std::make_unique<TableScanOperator>(table(false), std::vector<int>{0, 1, 2, 3, 4}),
          calls);
      ExecContext ctx{.pool = arrow::default_memory_pool(), .batch_size = 3};
      const auto expected = Drain(serial, ctx);
      ASSERT_TRUE(expected.ok()) << expected.status().ToString();
      const auto plan =
          PlanOf(Node(plan::AggregateNode{
                     .input = Node(plan::ScanNode{
                         .table = table(true), .table_name = "t", .fields = {0, 1, 2, 3, 4}}),
                     .aggregates = calls}),
                 2);
      for (arrow::internal::Executor* executor :
           {static_cast<arrow::internal::Executor*>(nullptr),
            static_cast<arrow::internal::Executor*>(pool.get())}) {
        const auto result = Run(plan, executor);
        ASSERT_TRUE(result.ok()) << result.status().ToString();
        EXPECT_TRUE(SameRows(**result, **expected)) << column.name << " empty=" << empty << "\n"
                                                    << (*result)->ToString() << "\n"
                                                    << (*expected)->ToString();
      }
    }
  }
}

// ---- two-level aggregation (docs/adr/0014-two-level-aggregation.md) ----

// 16 parts of 40 rows (every fifth part empty): k (BIGINT, 1 in a third of the rows: a heavy key;
// NULL every ninth), s (VARCHAR, 4 values), day (DATE), x (BIGINT, NULL every 13th), y (VARCHAR,
// NULL every 17th), v (BIGINT, negative and positive).
std::shared_ptr<MemoryTable> TwoLevelTable(bool split, bool empty = false) {
  const auto schema =
      arrow::schema({arrow::field("k", arrow::int64()), arrow::field("s", arrow::binary()),
                     arrow::field("day", arrow::date32()), arrow::field("x", arrow::int64()),
                     arrow::field("y", arrow::binary()), arrow::field("v", arrow::int64())});
  arrow::RecordBatchVector batches;
  for (int64_t part = 0; part < (empty ? 0 : 16); ++part) {
    std::vector<std::optional<int64_t>> k;
    std::vector<std::optional<std::string>> str;
    arrow::Date32Builder day;
    std::vector<std::optional<int64_t>> x;
    std::vector<std::optional<std::string>> y;
    std::vector<std::optional<int64_t>> v;
    const int64_t rows = part % 5 == 4 ? 0 : 40;
    for (int64_t i = 0; i < rows; ++i) {
      const int64_t r = (part * 40) + i;
      std::optional<int64_t> key = r % 3 == 0 ? 1 : (r * 7) % 23;
      if (r % 9 == 0) {
        key.reset();
      }
      k.push_back(key);
      str.emplace_back(std::string(1, static_cast<char>('a' + (r % 4))));
      EXPECT_TRUE(day.Append(static_cast<int32_t>(r % 5)).ok());
      x.push_back(r % 13 == 0 ? std::nullopt : std::optional<int64_t>((r * 31) % 97));
      y.push_back(r % 17 == 0 ? std::nullopt : std::optional<std::string>(std::to_string(r % 29)));
      v.emplace_back(r - 200);
    }
    batches.push_back(
        arrow::RecordBatch::Make(schema, rows,
                                 {Int64s(k), testing::Strings(str), day.Finish().ValueOrDie(),
                                  Int64s(x), testing::Strings(y), Int64s(v)}));
  }
  if (!split) {
    auto combined = arrow::Table::FromRecordBatches(schema, batches).ValueOrDie();
    auto one = combined->CombineChunksToBatch().ValueOrDie();
    return std::make_shared<MemoryTable>(schema, arrow::RecordBatchVector{one}, false);
  }
  return std::make_shared<MemoryTable>(schema, std::move(batches), true);
}

// The two-level operator over every part of `table`, sampling its first `sample_parts` parts.
arrow::Result<std::shared_ptr<arrow::Table>> RunTwoLevel(
    const std::shared_ptr<MemoryTable>& table, int64_t sample_parts,
    const std::vector<plan::BoundColumn>& keys, const std::vector<plan::AggregateCall>& calls,
    arrow::internal::Executor* executor) {
  const std::vector<int> fields = {0, 1, 2, 3, 4, 5};
  PartPipeline pipeline = [table,
                           fields](int64_t part) -> arrow::Result<std::unique_ptr<Operator>> {
    return std::make_unique<TableScanOperator>(table, fields, part);
  };
  std::shared_ptr<arrow::Schema> schema;
  if (keys.empty()) {
    schema = ScalarAggregateOperator(std::make_unique<TableScanOperator>(table, fields), calls)
                 .output_schema();
  } else {
    schema = GroupAggregateOperator(std::make_unique<TableScanOperator>(table, fields), keys, calls)
                 .output_schema();
  }
  PartTwoLevelAggregateOperator op(std::move(pipeline), table->num_parts(), sample_parts, 6, keys,
                                   calls, schema, keys.empty());
  ExecContext ctx{.pool = arrow::default_memory_pool(),
                  .batch_size = 3,
                  .executor = executor,
                  .threads = executor == nullptr ? 1 : kThreads};
  return Drain(op, ctx);
}

// The two levels give the rows of the serial GroupAggregateOperator (as a set) and, without keys,
// of ScalarAggregateOperator: COUNT(DISTINCT) of two columns (one of them twice, NULLs skipped)
// with COUNT(*), COUNT, integer SUM and AVG, VARCHAR MIN and BIGINT MAX, by a skewed BIGINT key
// (heavy, with NULL), two keys, a VARCHAR key, a DATE key and none; for a sample of no part, some
// parts and every part; with no rows. The same rows in the same order on any number of threads.
TEST_F(PartOperatorsTest, TwoLevelAggregationIsTheSerialOne) {
  const auto pool = Pool();
  const auto k = Column(0, "k", LogicalType::kBigInt);
  const auto str = Column(1, "s", LogicalType::kVarchar);
  const auto day = Column(2, "day", LogicalType::kDate);
  const auto x = Column(3, "x", LogicalType::kBigInt);
  const auto y = Column(4, "y", LogicalType::kVarchar);
  const auto v = Column(5, "v", LogicalType::kBigInt);
  const std::vector<plan::AggregateCall> calls = {
      {.kind = plan::AggKind::kCountDistinct, .arg = x, .type = LogicalType::kBigInt},
      {.kind = plan::AggKind::kCountStar, .arg = {}, .type = LogicalType::kBigInt},
      {.kind = plan::AggKind::kCountDistinct, .arg = y, .type = LogicalType::kBigInt},
      {.kind = plan::AggKind::kSum, .arg = v, .type = LogicalType::kHugeInt},
      {.kind = plan::AggKind::kCountDistinct, .arg = x, .type = LogicalType::kBigInt},
      {.kind = plan::AggKind::kCount, .arg = y, .type = LogicalType::kBigInt},
      {.kind = plan::AggKind::kAvg, .arg = v, .type = LogicalType::kDouble},
      {.kind = plan::AggKind::kMin, .arg = str, .type = LogicalType::kVarchar},
      {.kind = plan::AggKind::kMax, .arg = v, .type = LogicalType::kBigInt}};
  const std::vector<int> fields = {0, 1, 2, 3, 4, 5};
  for (const std::vector<plan::BoundColumn>& keys :
       {std::vector<plan::BoundColumn>{k}, std::vector<plan::BoundColumn>{k, str},
        std::vector<plan::BoundColumn>{str}, std::vector<plan::BoundColumn>{day},
        std::vector<plan::BoundColumn>{}}) {
    ASSERT_TRUE(TwoLevelAggregation(keys, calls));
    for (const bool empty : {false, true}) {
      ExecContext ctx{.pool = arrow::default_memory_pool(), .batch_size = 3};
      std::unique_ptr<Operator> serial;
      auto scan = std::make_unique<TableScanOperator>(TwoLevelTable(false, empty), fields);
      if (keys.empty()) {
        serial = std::make_unique<ScalarAggregateOperator>(std::move(scan), calls);
      } else {
        serial = std::make_unique<GroupAggregateOperator>(std::move(scan), keys, calls);
      }
      const auto expected = Drain(*serial, ctx);
      ASSERT_TRUE(expected.ok()) << expected.status().ToString();
      for (const int64_t sample : {0, 3, 16}) {
        const auto one_thread =
            RunTwoLevel(TwoLevelTable(true, empty), sample, keys, calls, nullptr);
        ASSERT_TRUE(one_thread.ok()) << one_thread.status().ToString();
        const auto four_threads =
            RunTwoLevel(TwoLevelTable(true, empty), sample, keys, calls, pool.get());
        ASSERT_TRUE(four_threads.ok()) << four_threads.status().ToString();
        EXPECT_TRUE(SameRows(**four_threads, **one_thread)) << keys.size() << " " << sample;
        EXPECT_TRUE(SameRows(*SortedRows(*one_thread), *SortedRows(*expected)))
            << keys.size() << " sample " << sample << " empty " << empty << "\n"
            << (*one_thread)->ToString() << "\n"
            << (*expected)->ToString();
      }
    }
  }
}

// Which aggregations run in two levels: some COUNT(DISTINCT) of a column that is not a key, no
// DOUBLE key, and only calls whose merge order cannot change their result.
TEST_F(PartOperatorsTest, TwoLevelAggregationNeedsOrderIndependentCalls) {
  const auto k = Column(0, "k", LogicalType::kBigInt);
  const auto d = Column(1, "d", LogicalType::kDouble);
  const auto x = Column(2, "x", LogicalType::kBigInt);
  const auto h = Column(3, "h", LogicalType::kHugeInt);
  const auto ts = Column(4, "ts", LogicalType::kTimestamp);
  const auto call = [](plan::AggKind kind, std::optional<plan::BoundColumn> arg, LogicalType type) {
    return plan::AggregateCall{.kind = kind, .arg = std::move(arg), .type = type};
  };
  const auto distinct = call(plan::AggKind::kCountDistinct, x, LogicalType::kBigInt);
  EXPECT_TRUE(TwoLevelAggregation({k}, {distinct}));
  EXPECT_TRUE(TwoLevelAggregation({}, {distinct}));
  EXPECT_TRUE(
      TwoLevelAggregation({k}, {distinct, call(plan::AggKind::kAvg, ts, LogicalType::kTimestamp),
                                call(plan::AggKind::kCount, d, LogicalType::kBigInt),
                                call(plan::AggKind::kCountDistinct, d, LogicalType::kBigInt)}));
  EXPECT_FALSE(
      TwoLevelAggregation({k}, {call(plan::AggKind::kCountStar, {}, LogicalType::kBigInt)}))
      << "no COUNT(DISTINCT)";
  EXPECT_FALSE(TwoLevelAggregation({x}, {distinct})) << "the distinct column is a key";
  EXPECT_FALSE(TwoLevelAggregation({d}, {distinct})) << "a DOUBLE key";
  EXPECT_FALSE(
      TwoLevelAggregation({k}, {distinct, call(plan::AggKind::kSum, d, LogicalType::kDouble)}));
  EXPECT_FALSE(
      TwoLevelAggregation({k}, {distinct, call(plan::AggKind::kAvg, d, LogicalType::kDouble)}));
  EXPECT_FALSE(
      TwoLevelAggregation({k}, {distinct, call(plan::AggKind::kMin, d, LogicalType::kDouble)}));
  EXPECT_FALSE(
      TwoLevelAggregation({k}, {distinct, call(plan::AggKind::kSum, h, LogicalType::kHugeInt)}));
}

// A two-level partition sends a group by the hash of its first keys, or by the hash of all its
// keys when the first keys' hash is heavy: a pure function of the keys, so the same group goes to
// the same partition from every part.
TEST_F(PartOperatorsTest, HeavyKeysSpreadOverThePartitions) {
  const auto k = Column(0, "k", LogicalType::kBigInt);
  const auto x = Column(1, "x", LogicalType::kBigInt);
  const auto schema =
      arrow::schema({arrow::field("k", arrow::int64()), arrow::field("x", arrow::int64())});
  std::vector<std::optional<int64_t>> ks;
  std::vector<std::optional<int64_t>> xs;
  for (int64_t i = 0; i < 400; ++i) {
    ks.emplace_back(i % 4 == 0 ? 7 : i % 5);  // 7 in a quarter of the rows
    xs.emplace_back(i);
  }
  const auto batch = arrow::RecordBatch::Make(schema, 400, {Int64s(ks), Int64s(xs)});
  const auto partitions_of = [&](std::span<const std::uint64_t> heavy) {
    auto table = GroupTable::Make({k, x}, {}, 2, arrow::default_memory_pool(), nullptr,
                                  /*first_keys=*/false);
    EXPECT_TRUE(table.ok());
    EXPECT_TRUE((*table)->Consume(*batch).ok());
    EXPECT_TRUE((*table)->Partition(1, heavy).ok());
    // Per partition, the distinct values of k in it.
    std::vector<std::set<int64_t>> keys((*table)->num_partitions());
    const auto uniques = (*table)->uniques();
    EXPECT_TRUE(uniques.ok());
    const auto values =
        std::static_pointer_cast<arrow::Int64Array>(uniques->values[0].make_array());
    for (std::size_t p = 0; p < keys.size(); ++p) {
      for (const std::uint32_t g : (*table)->partition_groups(p)) {
        keys[p].insert(values->Value(g));
      }
    }
    return keys;
  };
  const auto count = [](const std::vector<std::set<int64_t>>& keys, int64_t key) {
    return std::ranges::count_if(keys, [&](const auto& in) { return in.contains(key); });
  };
  const auto light = partitions_of({});
  for (int64_t key : {0, 1, 2, 3, 4, 7}) {
    EXPECT_EQ(count(light, key), 1) << key;  // every key in one partition
  }
  // The hash of k = 7 alone, as the table computes it.
  auto single = GroupTable::Make({k}, {}, 2, arrow::default_memory_pool(), nullptr, false);
  ASSERT_TRUE(single.ok());
  const auto seven = arrow::RecordBatch::Make(schema, 1, {Int64s({7}), Int64s({0})});
  ASSERT_TRUE((*single)->Consume(*seven).ok());
  const auto hash = (*single)->PrefixHashes(1);
  ASSERT_TRUE(hash.ok());
  const auto spread = partitions_of(*hash);
  EXPECT_GT(count(spread, 7), 30) << "100 (7, x) groups over 64 partitions";
  for (int64_t key : {0, 1, 2, 3, 4}) {
    EXPECT_EQ(count(spread, key), 1) << key;
  }
  EXPECT_EQ(partitions_of(*hash), spread) << "the same partitions every time";
  // Without outer keys (a global aggregation) the empty prefix is heavy: groups spread by x.
  auto by_x = GroupTable::Make({x}, {}, 2, arrow::default_memory_pool(), nullptr, false);
  ASSERT_TRUE(by_x.ok());
  ASSERT_TRUE((*by_x)->Consume(*batch).ok());
  const auto empty_prefix = KeyHashes(arrow::compute::ExecBatch({}, 1));
  ASSERT_TRUE(empty_prefix.ok());
  ASSERT_TRUE((*by_x)->Partition(0, *empty_prefix).ok());
  EXPECT_GT(
      std::ranges::count_if(std::views::iota(std::size_t{0}, (*by_x)->num_partitions()),
                            [&](std::size_t p) { return !(*by_x)->partition_groups(p).empty(); }),
      50)
      << "400 values of x over 64 partitions";
}

// ---- rows routed past the parts' own tables ----

// 12 parts of 40 rows (every fourth empty): u (BIGINT, unique per row: parts that do not reduce),
// k (BIGINT, 5 values), d (DOUBLE key with -0.0, 0.0, NaN, NULL), s (VARCHAR), v (BIGINT).
std::shared_ptr<MemoryTable> RoutedTable(bool split) {
  const auto schema =
      arrow::schema({arrow::field("u", arrow::int64()), arrow::field("k", arrow::int64()),
                     arrow::field("d", arrow::float64()), arrow::field("s", arrow::binary()),
                     arrow::field("v", arrow::int64())});
  const std::vector<double> doubles = {-0.0, 0.0, std::nan(""), 1.5, -std::nan("")};
  arrow::RecordBatchVector batches;
  for (int64_t part = 0; part < 12; ++part) {
    std::vector<std::optional<int64_t>> u;
    std::vector<std::optional<int64_t>> k;
    arrow::DoubleBuilder d;
    std::vector<std::optional<std::string>> str;
    std::vector<std::optional<int64_t>> v;
    const int64_t rows = part % 4 == 3 ? 0 : 40;
    for (int64_t i = 0; i < rows; ++i) {
      const int64_t r = (part * 40) + i;
      u.push_back(r % 17 == 0 ? std::nullopt : std::optional<int64_t>(r));
      k.emplace_back(r % 5);
      EXPECT_TRUE(
          (r % 13 == 0 ? d.AppendNull() : d.Append(doubles[static_cast<std::size_t>(r % 5)])).ok());
      str.emplace_back("s" + std::to_string(r % 7));
      v.emplace_back(r % 11);
    }
    batches.push_back(arrow::RecordBatch::Make(
        schema, rows,
        {Int64s(u), Int64s(k), d.Finish().ValueOrDie(), testing::Strings(str), Int64s(v)}));
  }
  if (!split) {
    auto combined = arrow::Table::FromRecordBatches(schema, batches).ValueOrDie();
    auto one = combined->CombineChunksToBatch().ValueOrDie();
    return std::make_shared<MemoryTable>(schema, arrow::RecordBatchVector{one}, false);
  }
  return std::make_shared<MemoryTable>(schema, std::move(batches), true);
}

// A part whose first batch hardly reduces sends its other rows straight to the partitions: the
// groups are those of the serial GROUP BY (as a set) and the same on any number of threads, byte
// for byte, for unique and repeated keys, DOUBLE keys (-0.0 with 0.0, one NaN), NULL keys and every
// order-independent call; a DOUBLE SUM keeps the parts' own tables. The parts that routed rows are
// counted exactly.
TEST_F(PartOperatorsTest, RoutedRowsGiveTheSameGroups) {
  const auto pool = Pool();
  const auto u = Column(0, "u", LogicalType::kBigInt);
  const auto k = Column(1, "k", LogicalType::kBigInt);
  const auto d = Column(2, "d", LogicalType::kDouble);
  const auto str = Column(3, "s", LogicalType::kVarchar);
  const auto v = Column(4, "v", LogicalType::kBigInt);
  const std::vector<plan::AggregateCall> calls = {
      {.kind = plan::AggKind::kCountStar, .arg = {}, .type = LogicalType::kBigInt},
      {.kind = plan::AggKind::kCount, .arg = u, .type = LogicalType::kBigInt},
      {.kind = plan::AggKind::kSum, .arg = v, .type = LogicalType::kHugeInt},
      {.kind = plan::AggKind::kMin, .arg = str, .type = LogicalType::kVarchar},
      {.kind = plan::AggKind::kMax, .arg = d, .type = LogicalType::kDouble},
      {.kind = plan::AggKind::kCountDistinct, .arg = v, .type = LogicalType::kBigInt}};
  const plan::AggregateCall double_sum{
      .kind = plan::AggKind::kSum, .arg = d, .type = LogicalType::kDouble};
  const auto run =
      [&](const std::shared_ptr<MemoryTable>& table, const std::vector<plan::BoundColumn>& keys,
          const std::vector<plan::AggregateCall>& aggregates, arrow::internal::Executor* executor,
          ProfileNode* profile, const std::vector<plan::Predicate>& filter) {
        plan::LogicalNodePtr input =
            Node(plan::ScanNode{.table = table, .table_name = "t", .fields = {0, 1, 2, 3, 4}});
        if (!filter.empty()) {
          input = Node(plan::FilterNode{.input = input, .predicates = filter});
        }
        const auto plan = PlanOf(
            Node(plan::GroupAggregateNode{.input = input, .keys = keys, .aggregates = aggregates}),
            keys.size() + aggregates.size());
        auto op = BuildPhysicalPlan(plan, profile);
        EXPECT_TRUE(op.ok()) << op.status().ToString();
        ExecContext ctx{.pool = arrow::default_memory_pool(),
                        .batch_size = 8,
                        .executor = executor,
                        .threads = executor == nullptr ? 1 : kThreads};
        return Drain(**op, ctx);
      };
  const auto raw_parts = [](const ProfileNode& root) -> int64_t {
    for (const ProfileMetric& metric : root.metrics()) {
      if (metric.name == "raw_parts") {
        return metric.value;
      }
    }
    return 0;
  };
  struct Case {
    std::vector<plan::BoundColumn> keys;
    std::vector<plan::AggregateCall> calls;
    int64_t routed;  // parts that route rows: 40 rows in batches of 8, 9 parts with rows
    std::vector<plan::Predicate> filter;
  };
  // A selective filter leaves batches of a row or two: the decision waits for 8 rows (a batch), so
  // few keys never route, whatever the first batch looks like.
  const std::vector<plan::Predicate> selective = {
      testing::Compare(v, plan::CompareOp::kEq, BigInt(3))};
  std::vector<plan::AggregateCall> with_double_sum = calls;
  with_double_sum.push_back(double_sum);
  const std::vector<Case> cases = {{.keys = {u}, .calls = calls, .routed = 9},
                                   {.keys = {u, k}, .calls = calls, .routed = 9},
                                   {.keys = {k}, .calls = calls, .routed = 0},
                                   {.keys = {d}, .calls = calls, .routed = 0},
                                   {.keys = {d, u}, .calls = calls, .routed = 9},
                                   {.keys = {str, u}, .calls = calls, .routed = 9},
                                   {.keys = {u}, .calls = with_double_sum, .routed = 0},
                                   {.keys = {k}, .calls = calls, .routed = 0, .filter = selective},
                                   {.keys = {u}, .calls = calls, .routed = 0, .filter = selective}};
  for (const Case& c : cases) {
    ExecContext ctx{.pool = arrow::default_memory_pool(), .batch_size = 8};
    std::unique_ptr<Operator> source =
        std::make_unique<TableScanOperator>(RoutedTable(false), std::vector<int>{0, 1, 2, 3, 4});
    if (!c.filter.empty()) {
      source = std::make_unique<FilterOperator>(std::move(source), c.filter);
    }
    GroupAggregateOperator serial(std::move(source), c.keys, c.calls);
    const auto expected = Drain(serial, ctx);
    ASSERT_TRUE(expected.ok()) << expected.status().ToString();
    ProfileNode one_profile;
    const auto one = run(RoutedTable(true), c.keys, c.calls, nullptr, &one_profile, c.filter);
    ProfileNode four_profile;
    const auto four = run(RoutedTable(true), c.keys, c.calls, pool.get(), &four_profile, c.filter);
    ASSERT_TRUE(one.ok()) << one.status().ToString();
    ASSERT_TRUE(four.ok()) << four.status().ToString();
    EXPECT_TRUE(SameRows(**four, **one)) << c.keys.size() << " keys";
    EXPECT_TRUE(SameRows(*SortedRows(*one), *SortedRows(*expected)))
        << c.keys.size() << " keys, " << c.calls.size() << " calls\n"
        << (*one)->ToString() << "\n"
        << (*expected)->ToString();
    EXPECT_EQ(raw_parts(one_profile), c.routed) << c.keys.size() << " keys";
    EXPECT_EQ(raw_parts(four_profile), c.routed) << c.keys.size() << " keys";
  }
}

// Routed rows go to the partition that Partition() gives their keys' group, DOUBLE keys
// normalized (-0.0 with 0.0, every NaN together).
TEST_F(PartOperatorsTest, RoutedRowsFollowTheirGroupsPartition) {
  const auto table = RoutedTable(true);
  const std::vector<plan::BoundColumn> keys = {Column(2, "d", LogicalType::kDouble),
                                               Column(1, "k", LogicalType::kBigInt)};
  auto reader = table->ScanPart(0, {0, 1, 2, 3, 4}, 1024);
  ASSERT_TRUE(reader.ok());
  std::shared_ptr<arrow::RecordBatch> rows;
  ASSERT_TRUE((*reader)->ReadNext(&rows).ok());
  auto grouped = GroupTable::Make(keys, {}, 5, arrow::default_memory_pool(), nullptr);
  ASSERT_TRUE(grouped.ok());
  ASSERT_TRUE((*grouped)->Consume(*rows).ok());
  ASSERT_TRUE((*grouped)->Partition().ok());
  const auto routed = (*grouped)->RouteRows(*rows);
  ASSERT_TRUE(routed.ok()) << routed.status().ToString();
  ASSERT_EQ(routed->size(), GroupTable::kPartitions);
  int64_t total = 0;
  for (std::size_t p = 0; p < GroupTable::kPartitions; ++p) {
    const auto& part_rows = (*routed)[p];
    total += part_rows->num_rows();
    // Every routed row's group is one of the groups Partition() put in partition p.
    auto check = GroupTable::Make(keys, {}, 5, arrow::default_memory_pool(), nullptr);
    ASSERT_TRUE(check.ok());
    if (part_rows->num_rows() == 0) {
      continue;
    }
    ASSERT_TRUE((*check)->Consume(*part_rows).ok());
    EXPECT_EQ((*check)->num_groups(), (*grouped)->partition_groups(p).size()) << p;
  }
  EXPECT_EQ(total, rows->num_rows());
}

// ---- late materialization of top-N (docs/adr/0016-late-materialization.md) ----

// 16 parts of 25 rows (every sixth part empty): k (BIGINT, ties: r % 13, NULL every 11th), s
// (VARCHAR, NULL every 7th), d (DOUBLE), day (DATE), v (BIGINT, r).
std::shared_ptr<MemoryTable> WideTable(bool split) {
  const auto schema =
      arrow::schema({arrow::field("k", arrow::int64()), arrow::field("s", arrow::binary()),
                     arrow::field("d", arrow::float64()), arrow::field("day", arrow::date32()),
                     arrow::field("v", arrow::int64())});
  arrow::RecordBatchVector batches;
  for (int64_t part = 0; part < 16; ++part) {
    std::vector<std::optional<int64_t>> k;
    std::vector<std::optional<std::string>> str;
    arrow::DoubleBuilder d;
    arrow::Date32Builder day;
    std::vector<std::optional<int64_t>> v;
    const int64_t rows = part % 6 == 5 ? 0 : 25;
    for (int64_t i = 0; i < rows; ++i) {
      const int64_t r = (part * 25) + i;
      k.push_back(r % 11 == 0 ? std::nullopt : std::optional<int64_t>(r % 13));
      str.push_back(r % 7 == 0 ? std::nullopt
                               : std::optional<std::string>("s" + std::to_string(r)));
      EXPECT_TRUE(d.Append(static_cast<double>(r) / 4).ok());
      EXPECT_TRUE(day.Append(static_cast<int32_t>(r % 100)).ok());
      v.emplace_back(r);
    }
    batches.push_back(
        arrow::RecordBatch::Make(schema, rows,
                                 {Int64s(k), testing::Strings(str), d.Finish().ValueOrDie(),
                                  day.Finish().ValueOrDie(), Int64s(v)}));
  }
  if (!split) {
    auto combined = arrow::Table::FromRecordBatches(schema, batches).ValueOrDie();
    auto one = combined->CombineChunksToBatch().ValueOrDie();
    return std::make_shared<MemoryTable>(schema, arrow::RecordBatchVector{one}, false);
  }
  return std::make_shared<MemoryTable>(schema, std::move(batches), true);
}

// A narrow scan of a part reads only the early columns: a late column is a NullArray of the
// batch's length, the row-id column carries RowId(ordinal, position in the part), across batches;
// a row-id column that is not late, or no part, is invalid.
TEST_F(PartOperatorsTest, NarrowScanCarriesRowIds) {
  const auto table = WideTable(true);
  const std::vector<int> fields = {0, 1, 2, 3, 4};
  TableScanOperator narrow(
      table, fields, 2,
      LateScan{.late = {false, true, true, false, true}, .row_id = 1, .ordinal = 7});
  TableScanOperator plain(table, fields, 2);
  ExecContext ctx{.pool = arrow::default_memory_pool(), .batch_size = 3};
  const auto narrowed = Drain(narrow, ctx);
  const auto full = Drain(plain, ctx);
  ASSERT_TRUE(narrowed.ok()) << narrowed.status().ToString();
  ASSERT_TRUE(full.ok());
  ASSERT_EQ((*narrowed)->num_rows(), 25);
  EXPECT_EQ((*narrowed)->schema()->field(1)->type()->id(), arrow::Type::INT64);
  EXPECT_EQ((*narrowed)->schema()->field(2)->type()->id(), arrow::Type::NA);
  EXPECT_EQ((*narrowed)->schema()->field(4)->type()->id(), arrow::Type::NA);
  EXPECT_TRUE((*narrowed)->column(0)->Equals(*(*full)->column(0)));
  EXPECT_TRUE((*narrowed)->column(3)->Equals(*(*full)->column(3)));
  std::vector<std::optional<int64_t>> ids;
  ids.reserve(25);
  for (int64_t r = 0; r < 25; ++r) {
    ids.emplace_back(RowId(7, r));
  }
  EXPECT_EQ(Int64Column(**narrowed, 1), ids);
  EXPECT_EQ((*narrowed)->column(2)->null_count(), 25);

  TableScanOperator early_id(table, fields, 2,
                             LateScan{.late = {false, true, true, false, true}, .row_id = 0});
  EXPECT_TRUE(early_id.Open(ctx).IsInvalid());
  TableScanOperator whole(table, fields, std::nullopt,
                          LateScan{.late = {false, true, true, false, true}, .row_id = 1});
  EXPECT_TRUE(whole.Open(ctx).IsInvalid());
}

// A late top-N needs its late columns: without them, with a row-id slot out of range or not
// BIGINT, Open is invalid.
TEST_F(PartOperatorsTest, LateTopNChecksItsColumns) {
  const auto table = WideTable(true);
  const std::vector<int> fields = {0, 1, 2, 3, 4};
  const auto schema = TableScanOperator(table, fields).output_schema();
  const auto narrow_schema =
      TableScanOperator(table, fields, 0,
                        LateScan{.late = {false, true, true, true, true}, .row_id = 1})
          .output_schema();
  const PartPipeline pipeline = [table,
                                 fields](int64_t part) -> arrow::Result<std::unique_ptr<Operator>> {
    return std::make_unique<TableScanOperator>(table, fields, part);
  };
  const LateColumns good{
      .table = table,
      .slots = {1, 2, 3, 4},
      .fields = {1, 2, 3, 4},
      .row_id = 1,
      .parts = std::make_shared<const std::vector<int64_t>>(std::vector<int64_t>{0, 1, 2}),
      .narrow_schema = narrow_schema};
  std::vector<LateColumns> bad(4, good);
  bad[0].slots.clear();
  bad[0].fields.clear();
  bad[1].row_id = 9;
  bad[2].row_id = 2;  // DOUBLE in the table's schema: not a row-id column
  bad[2].narrow_schema = schema;
  bad[3].parts = nullptr;
  ExecContext ctx{.pool = arrow::default_memory_pool(), .batch_size = 3};
  for (std::size_t i = 0; i < bad.size(); ++i) {
    PartTopNOperator op(pipeline, 3, schema, {{.column = Column(0, "k", LogicalType::kBigInt)}}, 2,
                        0, bad[i]);
    EXPECT_TRUE(op.Open(ctx).IsInvalid()) << i;
  }
  PartTopNOperator ok(pipeline, 3, schema, {{.column = Column(0, "k", LogicalType::kBigInt)}}, 2, 0,
                      good);
  EXPECT_TRUE(ok.Open(ctx).ok());
  EXPECT_TRUE(ok.Close().ok());
}

// A top-N whose other columns are fetched late gives the rows of the plain top-N (one part, so no
// late materialization), byte for byte, on any number of threads: every column type with NULLs,
// ties on the key, a filter, a computed key, windows inside, across and past the rows, and no
// row at all. Failures of a part or of the late fetch pass through.
TEST_F(PartOperatorsTest, TopNFetchesLateColumns) {
  const auto pool = Pool();
  const auto k = Column(0, "k", LogicalType::kBigInt);
  const auto v = Column(4, "v", LogicalType::kBigInt);
  const auto shapes = [&](const std::shared_ptr<MemoryTable>& table) {
    const auto scan =
        Node(plan::ScanNode{.table = table, .table_name = "t", .fields = {0, 1, 2, 3, 4}});
    const auto filter = Node(plan::FilterNode{
        .input = scan, .predicates = {testing::Compare(v, plan::CompareOp::kGe, BigInt(120))}});
    const auto none = Node(plan::FilterNode{
        .input = scan, .predicates = {testing::Compare(v, plan::CompareOp::kLt, BigInt(0))}});
    const auto compute = Node(plan::ComputeNode{
        .input = scan,
        .exprs = {std::make_shared<const plan::Expr>(plan::Expr{
            .node = plan::ColumnExpr{.index = 4}, .type = LogicalType::kBigInt, .name = "v2"})}});
    return std::vector<std::pair<plan::LogicalNodePtr, std::vector<plan::SortKey>>>{
        {scan, {{.column = k, .descending = false, .nulls_first = false}}},
        {scan, {{.column = k, .descending = true, .nulls_first = true}}},
        {filter, {{.column = k, .descending = true, .nulls_first = false}}},
        {none, {{.column = k}}},
        {compute, {{.column = Column(5, "v2", LogicalType::kBigInt), .descending = true}}}};
  };
  const auto wide = shapes(WideTable(true));
  const auto one = shapes(WideTable(false));
  int late_plans = 0;  // the rule applies when limit + offset <= half of the parts read
  for (std::size_t shape = 0; shape < wide.size(); ++shape) {
    for (const auto& [limit, offset] :
         std::vector<std::pair<int64_t, int64_t>>{{1, 0}, {3, 0}, {4, 2}, {6, 2}, {2, 7}}) {
      const auto top = [&](const auto& input) {
        const std::size_t width =
            input.first == wide[4].first || input.first == one[4].first ? 6 : 5;
        return PlanOf(Node(plan::LimitNode{
                          .input = Node(plan::SortNode{.input = input.first, .keys = input.second}),
                          .limit = limit,
                          .offset = offset}),
                      width);
      };
      const auto expected = Run(top(one[shape]), nullptr);
      ASSERT_TRUE(expected.ok()) << expected.status().ToString();
      ProfileNode root;
      auto late = BuildPhysicalPlan(top(wide[shape]), &root);
      ASSERT_TRUE(late.ok()) << late.status().ToString();
      late_plans += root.detail().contains(" late=") ? 1 : 0;
      for (arrow::internal::Executor* executor :
           {static_cast<arrow::internal::Executor*>(nullptr),
            static_cast<arrow::internal::Executor*>(pool.get())}) {
        const auto result = Run(top(wide[shape]), executor);
        ASSERT_TRUE(result.ok()) << result.status().ToString();
        EXPECT_TRUE(SameRows(**result, **expected))
            << "shape " << shape << " limit " << limit << " offset " << offset << "\n"
            << (*result)->ToString() << "\n"
            << (*expected)->ToString();
      }
    }
  }
  EXPECT_GE(late_plans, 15) << "of 25 plans";
  const auto top_of = [&](const std::shared_ptr<MemoryTable>& table) {
    return PlanOf(
        Node(plan::LimitNode{.input = Node(plan::SortNode{.input = shapes(table)[0].first,
                                                          .keys = shapes(table)[0].second}),
                             .limit = 2,
                             .offset = 0}),
        5);
  };
  for (arrow::internal::Executor* executor :
       {static_cast<arrow::internal::Executor*>(nullptr),
        static_cast<arrow::internal::Executor*>(pool.get())}) {
    const auto broken_part = WideTable(true);
    broken_part->FailPart(3);
    EXPECT_TRUE(Run(top_of(broken_part), executor).status().IsIOError());
    const auto broken_fetch = WideTable(true);
    broken_fetch->FailField(1);  // a late column: read by the fetch only
    const auto fetched = Run(top_of(broken_fetch), executor);
    EXPECT_TRUE(fetched.status().IsIOError()) << fetched.status().ToString();
    EXPECT_NE(fetched.status().message().find("field 1"), std::string::npos);
  }
}

// ---- skipping parts by their statistics ----

// A Filter on the scan skips the parts whose statistics rule every row out: with exact statistics
// of a sorted, contiguous column (x = 0..139 in parts of 7), exactly the parts holding a matching
// row are read. The results equal a read of every part (statistics off), on any number of
// threads.
TEST_F(PartOperatorsTest, FiltersSkipPartsByTheirStatistics) {
  const auto pool = Pool();
  const auto x = Column(0, "x", LogicalType::kBigInt);
  const auto matches = [](const plan::Predicate& predicate, int64_t v) {
    const auto c = static_cast<int64_t>(std::get<Int128>(predicate.constant.value));
    if (predicate.kind == plan::Predicate::Kind::kIn) {
      return std::ranges::any_of(predicate.values, [&](const plan::Constant& value) {
        return static_cast<int64_t>(std::get<Int128>(value.value)) == v;
      });
    }
    switch (predicate.op) {
      case plan::CompareOp::kEq:
        return v == c;
      case plan::CompareOp::kNe:
        return v != c;
      case plan::CompareOp::kLt:
        return v < c;
      case plan::CompareOp::kLe:
        return v <= c;
      case plan::CompareOp::kGt:
        return v > c;
      case plan::CompareOp::kGe:
        return v >= c;
    }
    return false;
  };
  std::vector<plan::Predicate> predicates;
  for (const plan::CompareOp op :
       {plan::CompareOp::kEq, plan::CompareOp::kNe, plan::CompareOp::kLt, plan::CompareOp::kLe,
        plan::CompareOp::kGt, plan::CompareOp::kGe}) {
    for (const int64_t c : {-1, 0, 6, 7, 69, 139, 140}) {
      predicates.push_back(testing::Compare(x, op, BigInt(c)));
    }
  }
  plan::Predicate in{.kind = plan::Predicate::Kind::kIn, .column = x};
  in.values = {BigInt(3), BigInt(50), BigInt(500)};
  predicates.push_back(in);
  plan::Predicate outside{.kind = plan::Predicate::Kind::kIn, .column = x};
  outside.values = {BigInt(-5), BigInt(1000)};
  predicates.push_back(outside);
  for (const plan::Predicate& predicate : predicates) {
    const auto plan_over = [&](const std::shared_ptr<MemoryTable>& table) {
      const auto filter = Node(plan::FilterNode{.input = Scan(table), .predicates = {predicate}});
      return PlanOf(Node(plan::ProjectNode{.input = filter, .columns = {x}}), 1);
    };
    std::vector<int64_t> expected;
    for (int64_t part = 0; part < kParts; ++part) {
      for (int64_t v = part * kRows; v < (part + 1) * kRows; ++v) {
        if (matches(predicate, v)) {
          expected.push_back(part);
          break;
        }
      }
    }
    const auto unpruned_table = Table();
    unpruned_table->set_stats(false);
    const auto unpruned = Run(plan_over(unpruned_table), nullptr);
    ASSERT_TRUE(unpruned.ok());
    EXPECT_EQ(unpruned_table->scanned_parts().size(), static_cast<std::size_t>(kParts));
    for (arrow::internal::Executor* executor :
         {static_cast<arrow::internal::Executor*>(nullptr),
          static_cast<arrow::internal::Executor*>(pool.get())}) {
      const auto table = Table();
      const auto result = Run(plan_over(table), executor);
      ASSERT_TRUE(result.ok()) << result.status().ToString();
      EXPECT_TRUE(SameRows(**result, **unpruned));
      EXPECT_EQ(table->scanned_parts(), expected) << "op " << static_cast<int>(predicate.op)
                                                  << " kind " << static_cast<int>(predicate.kind);
    }
  }
}

// Parts where the column is NULL on every row match no comparison, IN or IS NOT NULL; a folded
// FALSE matches no part (a global aggregate over no part still gives its one row).
TEST_F(PartOperatorsTest, NullPartsAndFalseSkipEverything) {
  const auto schema = arrow::schema({arrow::field("z", arrow::int64())});
  arrow::RecordBatchVector batches = {
      arrow::RecordBatch::Make(schema, 3, {Int64s({1, 2, 3})}),
      arrow::RecordBatch::Make(schema, 3, {Int64s({std::nullopt, std::nullopt, std::nullopt})}),
      arrow::RecordBatch::Make(schema, 2, {Int64s({5, std::nullopt})})};
  const auto z = Column(0, "z", LogicalType::kBigInt);
  const auto count = [&](const std::shared_ptr<MemoryTable>& table,
                         std::vector<plan::Predicate> predicates) {
    const auto scan = Node(plan::ScanNode{.table = table, .table_name = "t", .fields = {0}});
    const auto filter = Node(plan::FilterNode{.input = scan, .predicates = std::move(predicates)});
    return Run(PlanOf(Node(plan::AggregateNode{.input = filter,
                                               .aggregates = {{.kind = plan::AggKind::kCountStar,
                                                               .arg = {},
                                                               .type = LogicalType::kBigInt}}}),
                      1),
               nullptr);
  };
  plan::Predicate not_null{.kind = plan::Predicate::Kind::kIsNotNull, .column = z};
  for (const auto& [predicates, rows, parts] :
       std::vector<std::tuple<std::vector<plan::Predicate>, int64_t, std::vector<int64_t>>>{
           {{testing::Compare(z, plan::CompareOp::kGe, BigInt(0))}, 4, {0, 2}},
           {{not_null}, 4, {0, 2}},
           {{testing::Compare(z, plan::CompareOp::kEq, BigInt(5))}, 1, {2}},
           {{plan::Predicate{.kind = plan::Predicate::Kind::kFalse}}, 0, {}}}) {
    const auto table = std::make_shared<MemoryTable>(schema, batches, /*split=*/true);
    const auto result = count(table, predicates);
    ASSERT_TRUE(result.ok()) << result.status().ToString();
    EXPECT_EQ(Int64Column(**result), (std::vector<std::optional<int64_t>>{rows}));
    EXPECT_EQ(table->scanned_parts(), parts);
  }
}

// A predicate's column is a column of the scan's output, mapped to its table field through the
// scan's fields: with the scan reading fields {2, 0} (y, x), a filter on column 1 (x) skips by x's
// statistics, and a filter on column 0 (y) by y's. A Filter above a Compute is not used.
TEST_F(PartOperatorsTest, SkippingMapsScanColumnsToTableFields) {
  const auto count = [&](const std::shared_ptr<MemoryTable>& table,
                         const plan::LogicalNodePtr& filtered) {
    auto result =
        Run(PlanOf(Node(plan::AggregateNode{.input = filtered,
                                            .aggregates = {{.kind = plan::AggKind::kCountStar,
                                                            .arg = {},
                                                            .type = LogicalType::kBigInt}}}),
                   1),
            nullptr);
    EXPECT_TRUE(result.ok()) << result.status().ToString();
    return std::pair(Int64Column(**result).at(0).value_or(-1), table->scanned_parts());
  };
  const auto scan_of = [&](const std::shared_ptr<MemoryTable>& table) {
    return Node(plan::ScanNode{.table = table, .table_name = "t", .fields = {2, 0}});
  };
  {
    const auto table = Table();  // x in part p is 7p .. 7p + 6
    const auto filter =
        Node(plan::FilterNode{.input = scan_of(table),
                              .predicates = {testing::Compare(Column(1, "x", LogicalType::kBigInt),
                                                              plan::CompareOp::kLt, BigInt(14))}});
    const auto [rows, parts] = count(table, filter);
    EXPECT_EQ(rows, 14);
    EXPECT_EQ(parts, (std::vector<int64_t>{0, 1}));
  }
  {
    const auto table = Table();  // y = x, NULL every fifth row
    const auto filter =
        Node(plan::FilterNode{.input = scan_of(table),
                              .predicates = {testing::Compare(Column(0, "y", LogicalType::kBigInt),
                                                              plan::CompareOp::kGe, BigInt(133))}});
    const auto [rows, parts] = count(table, filter);
    EXPECT_EQ(rows, 6);  // 133 .. 139 without 135
    EXPECT_EQ(parts, (std::vector<int64_t>{19}));
  }
  {
    const auto table = Table();
    const auto compute = Node(plan::ComputeNode{
        .input = scan_of(table),
        .exprs = {std::make_shared<const plan::Expr>(
            plan::Expr{.node = plan::ColumnExpr{.index = 1}, .type = LogicalType::kBigInt})}});
    const auto filter =
        Node(plan::FilterNode{.input = compute,
                              .predicates = {testing::Compare(Column(2, "x2", LogicalType::kBigInt),
                                                              plan::CompareOp::kLt, BigInt(14))}});
    const auto [rows, parts] = count(table, filter);
    EXPECT_EQ(rows, 14);
    EXPECT_EQ(parts.size(), static_cast<std::size_t>(kParts)) << "a Filter above a Compute";
  }
}

}  // namespace
}  // namespace antb1::exec
