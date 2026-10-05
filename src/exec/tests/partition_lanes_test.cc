#include "../partition_lanes.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <utility>
#include <vector>

#include <arrow/status.h>
#include <arrow/util/thread_pool.h>
#include <gtest/gtest.h>

#include "antb1/exec/memory_budget.h"

#include "exec_test_util.h"

namespace antb1::exec {
namespace {

constexpr int kThreads = 4;

std::shared_ptr<arrow::internal::ThreadPool> Pool() {
  auto pool = arrow::internal::ThreadPool::Make(kThreads);
  EXPECT_TRUE(pool.ok()) << pool.status().ToString();
  return *pool;
}

std::vector<arrow::internal::Executor*> Executors(arrow::internal::ThreadPool* pool) {
  return {nullptr, pool};
}

// Every lane merges every part, in the order the parts were added, on one thread and on a pool.
TEST(PartitionLanesTest, EveryLaneMergesThePartsInOrder) {
  const auto pool = Pool();
  for (arrow::internal::Executor* executor : Executors(pool.get())) {
    constexpr std::size_t kLanes = 8;
    std::vector<std::vector<int64_t>> seen(kLanes);  // each lane writes only its own
    {
      PartitionLanes lanes(kLanes, executor, 3);
      for (int64_t part = 0; part < 50; ++part) {
        ASSERT_TRUE(lanes
                        .Add(part,
                             [&seen, part](std::size_t lane) {
                               seen[lane].push_back(part);
                               return arrow::Status::OK();
                             })
                        .ok());
      }
      ASSERT_TRUE(lanes.Finish().ok());
      EXPECT_EQ(lanes.pending(), 0);
    }
    for (std::size_t lane = 0; lane < kLanes; ++lane) {
      ASSERT_EQ(seen[lane].size(), 50U) << lane;
      for (int64_t part = 0; part < 50; ++part) {
        EXPECT_EQ(seen[lane][static_cast<std::size_t>(part)], part) << lane;
      }
    }
  }
}

// Without an executor a part merges into lane 0, 1, ... before Add returns.
TEST(PartitionLanesTest, WithoutAnExecutorLanesMergeInOrderOnTheCaller) {
  std::vector<std::pair<int64_t, std::size_t>> order;
  PartitionLanes lanes(3, nullptr, 2);
  for (int64_t part = 0; part < 4; ++part) {
    ASSERT_TRUE(lanes
                    .Add(part,
                         [&order, part](std::size_t lane) {
                           order.emplace_back(part, lane);
                           return arrow::Status::OK();
                         })
                    .ok());
    EXPECT_EQ(order.size(), static_cast<std::size_t>((part + 1) * 3));
  }
  ASSERT_TRUE(lanes.Finish().ok());
  std::vector<std::pair<int64_t, std::size_t>> expected;
  for (int64_t part = 0; part < 4; ++part) {
    for (std::size_t lane = 0; lane < 3; ++lane) {
      expected.emplace_back(part, lane);
    }
  }
  EXPECT_EQ(order, expected);
}

// A lane that waits holds back no other lane: lane 1 merges parts 0..3 while lane 0 is still in
// part 0.
TEST(PartitionLanesTest, ASlowLaneHoldsBackNoOther) {
  const auto pool = Pool();
  std::mutex mu;
  std::condition_variable cv;
  int64_t lane1_done = -1;
  PartitionLanes lanes(2, pool.get(), 8);
  for (int64_t part = 0; part < 4; ++part) {
    ASSERT_TRUE(lanes
                    .Add(part,
                         [&, part](std::size_t lane) {
                           std::unique_lock lock(mu);
                           if (lane == 1) {
                             lane1_done = part;
                             cv.notify_all();
                           } else if (part == 0) {
                             cv.wait(lock, [&] { return lane1_done == 3; });
                           }
                           return arrow::Status::OK();
                         })
                    .ok());
  }
  ASSERT_TRUE(lanes.Finish().ok());
  EXPECT_EQ(lane1_done, 3);
}

// At most `max_pending` parts wait to be merged, and one under memory pressure; a part's merge
// function (and what it holds) is released once every lane has merged it.
TEST(PartitionLanesTest, PendingPartsAreBounded) {
  const auto pool = Pool();
  MemoryBudget calm(std::nullopt);
  MemoryBudget pressed(1000);
  ASSERT_TRUE(pressed.Reserve(600).ok());
  for (const auto& [budget, bound] :
       std::vector<std::pair<const MemoryBudget*, int64_t>>{{&calm, 3}, {&pressed, 1}}) {
    std::atomic<int64_t> most = 0;
    auto held = std::make_shared<int>(0);
    {
      PartitionLanes lanes(4, pool.get(), 3, budget);
      for (int64_t part = 0; part < 40; ++part) {
        ASSERT_TRUE(lanes
                        .Add(part,
                             [&lanes, &most, held](std::size_t) {
                               int64_t now = lanes.pending();
                               int64_t seen = most.load();
                               while (now > seen && !most.compare_exchange_weak(seen, now)) {
                               }
                               return arrow::Status::OK();
                             })
                        .ok());
        EXPECT_LE(lanes.pending(), bound);
      }
      ASSERT_TRUE(lanes.Finish().ok());
      EXPECT_EQ(held.use_count(), 1) << "every merge function released";
    }
    EXPECT_LE(most.load(), bound);
    EXPECT_GE(most.load(), 1);
  }
  pressed.Release(600);
}

// The failure of the smallest (part, lane) wins, whatever the timing; nothing is queued after a
// failure is known; std::bad_alloc becomes OutOfMemory.
TEST(PartitionLanesTest, TheEarliestFailureWins) {
  const auto pool = Pool();
  for (arrow::internal::Executor* executor : Executors(pool.get())) {
    for (int run = 0; run < 20; ++run) {
      PartitionLanes lanes(8, executor, 4);
      std::atomic<int64_t> last_added = -1;
      arrow::Status added;
      for (int64_t part = 0; part < 30 && added.ok(); ++part) {
        last_added = part;
        added = lanes.Add(part, [part](std::size_t lane) -> arrow::Status {
          if (part == 9 && lane == 0) {
            return arrow::Status::Invalid("part 9 lane 0");
          }
          if (part == 7 && lane == 2) {
            return arrow::Status::Invalid("part 7 lane 2");
          }
          if (part == 7 && lane == 6) {
            return arrow::Status::IOError("part 7 lane 6");
          }
          return arrow::Status::OK();
        });
      }
      const arrow::Status finished = lanes.Finish();
      EXPECT_TRUE(finished.IsInvalid()) << finished.ToString();
      EXPECT_EQ(finished.message(), "part 7 lane 2");
      EXPECT_FALSE(added.ok());
      EXPECT_EQ(added.message(), "part 7 lane 2");
      EXPECT_LT(last_added.load(), 30);
    }
    PartitionLanes lanes(2, executor, 2);
    ASSERT_TRUE(lanes.Add(0, [](std::size_t) -> arrow::Status { throw std::bad_alloc(); }).ok() ||
                executor == nullptr);
    EXPECT_TRUE(lanes.Finish().IsOutOfMemory());
  }
}

// A lane whose task cannot be submitted (the pool is shut down) fails Add with the executor's
// status; the part is dropped, nothing waits for it.
TEST(PartitionLanesTest, AFailedSubmitFailsTheLane) {
  auto pool = arrow::internal::ThreadPool::Make(1);
  ASSERT_TRUE(pool.ok());
  ASSERT_TRUE((*pool)->Shutdown().ok());
  PartitionLanes lanes(3, pool->get(), 2);
  EXPECT_FALSE(lanes.Add(0, [](std::size_t) { return arrow::Status::OK(); }).ok());
  EXPECT_EQ(lanes.pending(), 0);
  EXPECT_FALSE(lanes.Finish().ok());
  EXPECT_FALSE(lanes.Add(1, [](std::size_t) { return arrow::Status::OK(); }).ok());
}

// A lane whose task cannot be started because Submit throws std::bad_alloc fails with OutOfMemory:
// Add returns it once the other lanes have merged the part, nothing is queued after it, and the
// lanes finish and go without waiting for a task that never ran.
TEST(PartitionLanesTest, ASubmitThatThrowsFailsTheLane) {
  const auto pool = Pool();
  constexpr std::size_t kLanes = 4;
  testing::ThrowingExecutor executor(pool.get(), /*throw_at=*/2);  // lane 2's task for part 0
  std::vector<std::vector<int64_t>> seen(kLanes);                  // each lane writes only its own
  const auto merge = [&seen](int64_t part) {
    return [&seen, part](std::size_t lane) {
      seen[lane].push_back(part);
      return arrow::Status::OK();
    };
  };
  {
    PartitionLanes lanes(kLanes, &executor, 2);
    const arrow::Status added = lanes.Add(0, merge(0));
    EXPECT_TRUE(added.IsOutOfMemory()) << added.ToString();
    EXPECT_EQ(lanes.pending(), 0);
    const arrow::Status next = lanes.Add(1, merge(1));
    EXPECT_TRUE(next.IsOutOfMemory()) << next.ToString();
    const arrow::Status finished = lanes.Finish();
    EXPECT_TRUE(finished.IsOutOfMemory()) << finished.ToString();
  }
  EXPECT_EQ(seen, (std::vector<std::vector<int64_t>>{{0}, {0}, {}, {0}}));
  EXPECT_EQ(executor.spawns(), static_cast<int>(kLanes));
}

// ForEach runs the tasks on the executor (or here) and reports the first failure in index order,
// whatever the timing; without an executor the tasks after it do not run. std::bad_alloc becomes
// OutOfMemory, and a task that cannot be submitted fails ForEach.
TEST(PartitionLanesTest, ForEachReportsTheFirstFailureInIndexOrder) {
  const auto pool = Pool();
  for (arrow::internal::Executor* executor : Executors(pool.get())) {
    for (int run = 0; run < 10; ++run) {
      std::vector<int> ran(20, 0);  // each task writes only its own
      const arrow::Status status = ForEach(executor, ran.size(), [&ran](std::size_t i) {
        ran[i] = 1;
        if (i == 13) {
          return arrow::Status::IOError("task 13");
        }
        if (i == 7) {
          return arrow::Status::Invalid("task 7");
        }
        return arrow::Status::OK();
      });
      EXPECT_TRUE(status.IsInvalid()) << status.ToString();
      EXPECT_EQ(status.message(), "task 7");
      EXPECT_EQ(ran[19], executor == nullptr ? 0 : 1);
    }
    const arrow::Status oom = ForEach(executor, 3, [](std::size_t i) -> arrow::Status {
      if (i == 1) {
        throw std::bad_alloc();
      }
      return arrow::Status::OK();
    });
    EXPECT_TRUE(oom.IsOutOfMemory()) << oom.ToString();
    EXPECT_TRUE(
        ForEach(executor, 0, [](std::size_t) { return arrow::Status::Invalid("none"); }).ok());
  }
  auto stopped = arrow::internal::ThreadPool::Make(1);
  ASSERT_TRUE(stopped.ok());
  ASSERT_TRUE((*stopped)->Shutdown().ok());
  EXPECT_FALSE(ForEach(stopped->get(), 2, [](std::size_t) { return arrow::Status::OK(); }).ok());
}

// A Submit that throws std::bad_alloc starts nothing: ForEach fails with OutOfMemory once the tasks
// it has submitted have ended (they still run when it throws), the later tasks never run, and a
// task before it that fails comes first.
TEST(PartitionLanesTest, ForEachWaitsForItsTasksWhenASubmitThrows) {
  const auto pool = Pool();
  constexpr std::size_t kTasks = 8;
  constexpr std::size_t kThrowAt = 6;  // more tasks before it than threads: some wait to start
  for (const bool fail : {false, true}) {
    SCOPED_TRACE(fail);
    std::mutex mu;
    std::condition_variable cv;
    bool thrown = false;
    testing::ThrowingExecutor executor(pool.get(), static_cast<int>(kThrowAt), [&] {
      const std::scoped_lock lock(mu);
      thrown = true;
      cv.notify_all();
    });
    std::vector<int> ended(kTasks, 0);  // each task writes only its own
    const arrow::Status status = ForEach(&executor, kTasks, [&](std::size_t i) {
      std::unique_lock lock(mu);
      cv.wait(lock, [&] { return thrown; });  // no task ends before the submit throws
      ended[i] = 1;
      return fail && i == 4 ? arrow::Status::Invalid("task 4") : arrow::Status::OK();
    });
    if (fail) {
      EXPECT_TRUE(status.IsInvalid()) << status.ToString();
    } else {
      EXPECT_TRUE(status.IsOutOfMemory()) << status.ToString();
    }
    for (std::size_t i = 0; i < kTasks; ++i) {
      EXPECT_EQ(ended[i], i < kThrowAt ? 1 : 0) << i;
    }
    EXPECT_EQ(executor.spawns(), static_cast<int>(kThrowAt) + 1);
  }
}

}  // namespace
}  // namespace antb1::exec
