// The memory limit: MemoryBudget, MemoryReservation, the memory the sinks report, operators that
// run out of their budget, and parts started one at a time under pressure.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <latch>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <arrow/util/thread_pool.h>
#include <gtest/gtest.h>

#include "antb1/exec/grouped_aggregate_state.h"
#include "antb1/exec/memory_budget.h"
#include "antb1/exec/physical_planner.h"
#include "antb1/exec/sort.h"
#include "antb1/plan/logical_plan.h"

#include "exec_test_util.h"

namespace antb1::exec {
namespace {

using plan::LogicalType;
using testing::Column;
using testing::Int64s;
using testing::MemoryTable;

constexpr int64_t kGiB = int64_t{1024} * 1024 * 1024;

// ---- MemoryBudget ----

TEST(MemoryBudgetTest, CountsBuffersAndReservationsAgainstTheLimit) {
  MemoryBudget budget(1000);
  EXPECT_EQ(budget.limit(), 1000);
  uint8_t* a = nullptr;
  ASSERT_TRUE(budget.Allocate(600, &a).ok());
  EXPECT_EQ(budget.bytes_allocated(), 600);
  EXPECT_TRUE(budget.under_pressure());  // above half the limit
  uint8_t* b = nullptr;
  const arrow::Status failed = budget.Allocate(500, &b);
  EXPECT_TRUE(failed.IsOutOfMemory()) << failed.ToString();
  EXPECT_NE(failed.message().find("memory limit of 1.00 KB"), std::string::npos);
  EXPECT_EQ(budget.bytes_allocated(), 600);  // nothing left behind
  EXPECT_TRUE(budget.Reserve(400).ok());
  EXPECT_TRUE(budget.Reserve(1).IsOutOfMemory());
  budget.Release(400);
  // Growing past the limit fails and keeps the buffer; shrinking gives memory back.
  EXPECT_TRUE(budget.Reallocate(600, 1200, &a).IsOutOfMemory());
  EXPECT_EQ(budget.bytes_allocated(), 600);
  ASSERT_TRUE(budget.Reallocate(600, 100, &a).ok());
  EXPECT_EQ(budget.bytes_allocated(), 100);
  EXPECT_FALSE(budget.under_pressure());
  budget.Free(a, 100);
  EXPECT_EQ(budget.bytes_allocated(), 0);
  EXPECT_EQ(budget.max_memory(), 1000);
  EXPECT_EQ(budget.num_allocations(), 2);
  EXPECT_EQ(budget.total_bytes_allocated(), 600);
  EXPECT_FALSE(budget.backend_name().empty());
  EXPECT_TRUE(budget.Reserve(0).ok());

  MemoryBudget unlimited(std::nullopt);
  EXPECT_TRUE(unlimited.Reserve(kGiB * 1024 * 1024).ok());
  EXPECT_FALSE(unlimited.under_pressure());
}

// Threads allocating at once never pass the limit together: 4 threads each hold 20 KiB until all
// have tried (a latch), so exactly 64 of their 80 allocations of 1 KiB fit a 64 KiB limit; then
// they churn allocations and frees, and the peak never passes the limit.
TEST(MemoryBudgetTest, ConcurrentAllocationsNeverPassTheLimit) {
  constexpr int64_t kLimit = int64_t{64} * 1024;
  constexpr int kThreads = 4;
  MemoryBudget budget(kLimit);
  std::atomic<int64_t> held_first = 0;
  std::latch tried(kThreads);
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&] {
      std::vector<uint8_t*> held;
      for (int i = 0; i < 20; ++i) {
        uint8_t* p = nullptr;
        if (budget.Allocate(1024, &p).ok()) {
          held.push_back(p);
        }
      }
      held_first += static_cast<int64_t>(held.size());
      tried.arrive_and_wait();
      for (int i = 0; i < 2000; ++i) {
        uint8_t* p = nullptr;
        if (budget.Allocate(1024, &p).ok()) {
          held.push_back(p);
        }
        if (held.size() > 20) {
          budget.Free(held.back(), 1024);
          held.pop_back();
          budget.Free(held.front(), 1024);
          held.erase(held.begin());
        }
      }
      for (uint8_t* q : held) {
        budget.Free(q, 1024);
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  EXPECT_EQ(held_first.load(), kLimit / 1024);
  EXPECT_LE(budget.max_memory(), kLimit);
  EXPECT_EQ(budget.bytes_allocated(), 0);
}

TEST(MemoryBudgetTest, ReservationsFollowTheirContainer) {
  MemoryBudget budget(1000);
  {
    MemoryReservation reservation;
    EXPECT_TRUE(reservation.Resize(5000).ok());  // no budget: only counted
    EXPECT_EQ(reservation.bytes(), 5000);
    reservation.Reset(&budget);
    EXPECT_EQ(reservation.bytes(), 0);
    EXPECT_TRUE(reservation.Resize(300).ok());
    EXPECT_TRUE(reservation.Resize(700).ok());
    EXPECT_EQ(budget.bytes_allocated(), 700);
    EXPECT_TRUE(reservation.Resize(1200).IsOutOfMemory());
    EXPECT_EQ(reservation.bytes(), 700);
    EXPECT_TRUE(reservation.Resize(200).ok());
    EXPECT_EQ(budget.bytes_allocated(), 200);
  }
  EXPECT_EQ(budget.bytes_allocated(), 0);
}

TEST(MemoryBudgetTest, FormatsBytes) {
  EXPECT_EQ(FormatBytes(0), "0 bytes");
  EXPECT_EQ(FormatBytes(999), "999 bytes");
  EXPECT_EQ(FormatBytes(1500), "1.50 KB");
  EXPECT_EQ(FormatBytes(int64_t{4} * 1000 * 1000 * 1000), "4.00 GB");
}

// ---- memory the sinks report ----

TEST(SortBufferMemoryTest, ReportsRowReferences) {
  const auto schema = arrow::schema({arrow::field("x", arrow::int64())});
  auto comparator = RowComparator::Make(
      schema, {{.column = Column(0, "x", LogicalType::kBigInt), .descending = false}});
  ASSERT_TRUE(comparator.ok()) << comparator.status().ToString();
  SortBuffer buffer(std::move(*comparator), std::nullopt);
  const int64_t empty = buffer.memory_usage();
  std::vector<std::optional<int64_t>> values;
  values.reserve(1000);
  for (int64_t i = 0; i < 1000; ++i) {
    values.emplace_back(1000 - i);
  }
  ASSERT_TRUE(buffer
                  .Add(arrow::RecordBatch::Make(schema, 1000, {Int64s(values)}),
                       arrow::default_memory_pool())
                  .ok());
  EXPECT_GE(buffer.memory_usage(), empty);
  EXPECT_EQ(buffer.sort_memory(), 1000 * 32);
  ASSERT_TRUE(buffer.Sort(arrow::default_memory_pool()).ok());
  EXPECT_GE(buffer.memory_usage(), 1000 * 8);  // the sorted row references
}

TEST(GroupedStateMemoryTest, GrowsWithGroupsAndVarcharValues) {
  for (const plan::AggKind kind :
       {plan::AggKind::kCountStar, plan::AggKind::kCount, plan::AggKind::kSum, plan::AggKind::kAvg,
        plan::AggKind::kMin, plan::AggKind::kMax, plan::AggKind::kCountDistinct}) {
    const bool sum = kind == plan::AggKind::kSum || kind == plan::AggKind::kAvg;
    const std::optional<LogicalType> input =
        kind == plan::AggKind::kCountStar ? std::nullopt : std::optional(LogicalType::kBigInt);
    LogicalType result = LogicalType::kBigInt;
    if (kind == plan::AggKind::kSum) {
      result = LogicalType::kHugeInt;
    } else if (kind == plan::AggKind::kAvg) {
      result = LogicalType::kDouble;
    }
    auto state = MakeGroupedAggregateState(kind, input, result);
    ASSERT_TRUE(state.ok()) << state.status().ToString();
    (*state)->Resize(1000);
    const int64_t small = (*state)->memory_usage();
    EXPECT_GE(small, sum ? 1000 * 16 : 1000) << plan::ToString(kind);
    (*state)->Resize(5000);
    EXPECT_GT((*state)->memory_usage(), small) << plan::ToString(kind);
  }
  // A VARCHAR MIN holds its values: long ones count on top of the vectors.
  auto state =
      MakeGroupedAggregateState(plan::AggKind::kMin, LogicalType::kVarchar, LogicalType::kVarchar);
  ASSERT_TRUE(state.ok());
  (*state)->Resize(2);
  const int64_t empty = (*state)->memory_usage();
  const auto values = testing::Strings({std::string(1000, 'x'), std::string(500, 'y'), "z"});
  const std::vector<std::uint32_t> ids = {0, 1, 1};
  ASSERT_TRUE((*state)->Consume(values.get(), ids).ok());
  // Both long values are held ("z" is not below the 500-byte value).
  EXPECT_GE((*state)->memory_usage(), empty + 1500);
  EXPECT_LT((*state)->memory_usage(), empty + 1600);
}

// ---- operators on a budget ----

class MemoryLimitTest : public testing::ExecTest {
 protected:
  // 20 parts of 50 rows: x = 0..999, s = a 100-byte string per row.
  static std::shared_ptr<MemoryTable> Table() {
    const auto schema =
        arrow::schema({arrow::field("x", arrow::int64()), arrow::field("s", arrow::binary())});
    arrow::RecordBatchVector batches;
    for (int64_t part = 0; part < 20; ++part) {
      std::vector<std::optional<int64_t>> x;
      std::vector<std::optional<std::string>> s;
      for (int64_t i = 0; i < 50; ++i) {
        const int64_t v = (part * 50) + i;
        x.emplace_back(v);
        s.emplace_back(std::string(100, static_cast<char>('a' + (v % 26))) + std::to_string(v));
      }
      batches.push_back(arrow::RecordBatch::Make(schema, 50, {Int64s(x), testing::Strings(s)}));
    }
    return std::make_shared<MemoryTable>(schema, std::move(batches), /*split=*/true);
  }

  static plan::LogicalNodePtr Node(plan::LogicalNode node) {
    return std::make_shared<const plan::LogicalNode>(std::move(node));
  }

  static plan::LogicalPlan PlanOf(plan::LogicalNodePtr root, std::size_t width) {
    plan::LogicalPlan plan{.root = std::move(root), .output = {}};
    for (std::size_t i = 0; i < width; ++i) {
      plan.output.push_back({.name = "c" + std::to_string(i), .type = LogicalType::kBigInt});
    }
    return plan;
  }

  static arrow::Result<std::shared_ptr<arrow::Table>> Run(const plan::LogicalPlan& plan,
                                                          MemoryBudget& budget,
                                                          arrow::internal::Executor* executor) {
    ARROW_ASSIGN_OR_RAISE(auto op, BuildPhysicalPlan(plan));
    ExecContext ctx{.pool = &budget,
                    .batch_size = 16,
                    .executor = executor,
                    .threads = executor == nullptr ? 1 : 4,
                    .budget = &budget};
    return Drain(*op, ctx);
  }

  // Every shape of sink: a grouping (VARCHAR MIN per group), a sort, a projection, an aggregate.
  static std::vector<plan::LogicalPlan> Plans(const std::shared_ptr<MemoryTable>& table) {
    const auto x = Column(0, "x", LogicalType::kBigInt);
    const auto s = Column(1, "s", LogicalType::kVarchar);
    const auto scan = Node(plan::ScanNode{.table = table, .table_name = "t", .fields = {0, 1}});
    std::vector<plan::LogicalPlan> plans;
    plans.push_back(
        PlanOf(Node(plan::GroupAggregateNode{.input = scan,
                                             .keys = {x},
                                             .aggregates = {{.kind = plan::AggKind::kMin,
                                                             .arg = s,
                                                             .type = LogicalType::kVarchar}}}),
               2));
    plans.push_back(PlanOf(
        Node(plan::ProjectNode{.input = Node(plan::SortNode{
                                   .input = scan, .keys = {{.column = s, .descending = true}}}),
                               .columns = {x}}),
        1));
    // About half of every batch's rows (the strings cycle through the alphabet), so the
    // projection copies them.
    const auto filter = Node(plan::FilterNode{
        .input = scan,
        .predicates = {testing::Compare(
            s, plan::CompareOp::kGe,
            plan::Constant{.type = LogicalType::kVarchar, .value = std::string("n")})}});
    plans.push_back(PlanOf(Node(plan::ProjectNode{.input = filter, .columns = {s}}), 1));
    plans.push_back(
        PlanOf(Node(plan::AggregateNode{.input = scan,
                                        .aggregates = {{.kind = plan::AggKind::kCountDistinct,
                                                        .arg = s,
                                                        .type = LogicalType::kBigInt}}}),
               1));
    // Two levels (ADR 0014): inner tables by x and s, outer groups by x.
    plans.push_back(PlanOf(
        Node(plan::GroupAggregateNode{
            .input = scan,
            .keys = {x},
            .aggregates =
                {{.kind = plan::AggKind::kCountDistinct, .arg = s, .type = LogicalType::kBigInt},
                 {.kind = plan::AggKind::kMax, .arg = s, .type = LogicalType::kVarchar}}}),
        3));
    return plans;
  }
};

// With a tiny limit every sink fails with OutOfMemory and gives everything back; with a big one
// the results are those without a limit.
TEST_F(MemoryLimitTest, SinksFailCleanlyPastTheLimit) {
  auto pool = arrow::internal::ThreadPool::Make(4);
  ASSERT_TRUE(pool.ok());
  for (arrow::internal::Executor* executor :
       {static_cast<arrow::internal::Executor*>(nullptr),
        static_cast<arrow::internal::Executor*>(pool->get())}) {
    const auto plans = Plans(Table());
    for (std::size_t i = 0; i < plans.size(); ++i) {
      MemoryBudget unlimited(std::nullopt);
      const auto expected = Run(plans[i], unlimited, executor);
      ASSERT_TRUE(expected.ok()) << i << ": " << expected.status().ToString();
      EXPECT_GT(unlimited.max_memory(), 20000) << i;

      MemoryBudget tiny(4096);
      const auto failed = Run(plans[i], tiny, executor);
      EXPECT_TRUE(failed.status().IsOutOfMemory()) << i << ": " << failed.status().ToString();
      EXPECT_EQ(tiny.bytes_allocated(), 0) << i;

      MemoryBudget big(kGiB);
      const auto result = Run(plans[i], big, executor);
      ASSERT_TRUE(result.ok()) << i << ": " << result.status().ToString();
      EXPECT_TRUE((*result)->Equals(**expected)) << i;
    }
  }
}

// Above half of the limit (under pressure) the sinks still give the unlimited results: parts and
// merges go one at a time, and a GROUP BY builds its rows one partition at a time.
TEST_F(MemoryLimitTest, SinksGiveTheSameResultsUnderPressure) {
  auto pool = arrow::internal::ThreadPool::Make(4);
  ASSERT_TRUE(pool.ok());
  for (arrow::internal::Executor* executor :
       {static_cast<arrow::internal::Executor*>(nullptr),
        static_cast<arrow::internal::Executor*>(pool->get())}) {
    const auto plans = Plans(Table());
    for (std::size_t i = 0; i < plans.size(); ++i) {
      MemoryBudget unlimited(std::nullopt);
      const auto expected = Run(plans[i], unlimited, executor);
      ASSERT_TRUE(expected.ok()) << i << ": " << expected.status().ToString();
      MemoryBudget pressed(kGiB);
      ASSERT_TRUE(pressed.Reserve((kGiB / 2) + 1).ok());
      ASSERT_TRUE(pressed.under_pressure());
      const auto result = Run(plans[i], pressed, executor);
      ASSERT_TRUE(result.ok()) << i << ": " << result.status().ToString();
      EXPECT_TRUE((*result)->Equals(**expected)) << i;
      pressed.Release((kGiB / 2) + 1);
    }
  }
}

// Under pressure a part starts only when no other is in flight: a LIMIT met in the first part
// reads at most the next one, whatever the window.
TEST_F(MemoryLimitTest, PartsStartOneAtATimeUnderPressure) {
  auto pool = arrow::internal::ThreadPool::Make(4);
  ASSERT_TRUE(pool.ok());
  const auto table = Table();
  const auto scan = Node(plan::ScanNode{.table = table, .table_name = "t", .fields = {0}});
  const auto limit = Node(plan::LimitNode{.input = scan, .limit = 3});
  MemoryBudget budget(kGiB);
  ASSERT_TRUE(budget.Reserve((kGiB / 2) + 1).ok());  // past half the limit
  const auto result = Run(PlanOf(limit, 1), budget, pool->get());
  ASSERT_TRUE(result.ok()) << result.status().ToString();
  EXPECT_EQ((*result)->num_rows(), 3);
  const std::vector<int64_t> scanned = table->scanned_parts();
  ASSERT_FALSE(scanned.empty());
  EXPECT_LE(scanned.back(), 1);
}

}  // namespace
}  // namespace antb1::exec
