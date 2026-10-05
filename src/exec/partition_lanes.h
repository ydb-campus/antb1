#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include <arrow/status.h>
#include <arrow/util/thread_pool.h>

#include "antb1/exec/memory_budget.h"

namespace antb1::exec {

// Runs fn(0) .. fn(n - 1) on the executor (here, one after another, without one) and waits for all
// of them: the first failure in index order decides the status (without an executor, the tasks
// after it do not run). std::bad_alloc in a task becomes OutOfMemory. A task that cannot be
// submitted (Submit fails, or throws std::bad_alloc: OutOfMemory) is a failure at its index, and
// no later task starts; the tasks already submitted still end before ForEach returns, or throws
// (std::bad_alloc while even a status cannot be made). Called from the consumer thread, never from
// a task of the executor: it waits for the tasks.
arrow::Status ForEach(arrow::internal::Executor* executor, std::size_t n,
                      const std::function<arrow::Status(std::size_t)>& fn);

// The merge of parts into partitioned state (docs/adr/0013-parallel-execution.md): one lane per
// partition, each merging the parts in part order on its own, so that a slow partition holds back
// no other. Add hands a part to every lane and returns; a lane runs on the executor while it has
// parts queued (one task per lane at a time), and a part's merge function (with whatever it holds)
// is released once every lane has merged it. Without an executor, Add merges the part into lane 0,
// 1, ... on the calling thread. Either way each lane sees exactly the parts in the order added, so
// the merged state is the same for any number of threads.
//
// Memory: Add waits while `max_pending` parts are not merged by every lane, and while one is under
// memory pressure (MemoryBudget::under_pressure), which bounds the parts held for merging.
//
// Errors: a lane stops at its first failure and drops the rest of its queue; the others go on.
// Once a failure is known, Add queues nothing more and returns Finish(). Finish() waits for every
// lane and returns the failure of the smallest (part, lane): every part up to the first failure
// seen was given to every lane, so which failure wins does not depend on the timing.
// std::bad_alloc in a merge becomes OutOfMemory. A lane that cannot take a part (its task cannot
// be submitted, or std::bad_alloc while the part is queued: OutOfMemory) fails as if it had failed
// to merge the part, every lane does when the part cannot be queued at all, and Add then returns
// Finish(): no task is left running and no lane waits for one.
//
// Add and Finish are called from one thread (the consumer), never from the executor's threads:
// they may wait for the lanes. The destructor waits for every lane.
class PartitionLanes {
 public:
  // Merges the part into lane `lane`.
  using Merge = std::function<arrow::Status(std::size_t lane)>;

  PartitionLanes(std::size_t lanes, arrow::internal::Executor* executor, int64_t max_pending,
                 const MemoryBudget* budget = nullptr);
  PartitionLanes(const PartitionLanes&) = delete;
  PartitionLanes& operator=(const PartitionLanes&) = delete;
  PartitionLanes(PartitionLanes&&) = delete;
  PartitionLanes& operator=(PartitionLanes&&) = delete;
  ~PartitionLanes();

  // Queues part `part` (parts come in increasing order) on every lane.
  arrow::Status Add(int64_t part, Merge merge);
  // Waits for every lane to merge every part added; the failure of the smallest (part, lane).
  arrow::Status Finish();
  // Waits for every lane to merge every part added (their memory is then released).
  void Wait();
  // Whether a failure is known, without waiting: Finish() then returns one.
  [[nodiscard]] bool failed() const;

  // Parts added and not yet merged by every lane (for tests).
  [[nodiscard]] int64_t pending() const;

 private:
  struct Part {
    int64_t part = 0;
    Merge merge;
    std::size_t remaining = 0;  // lanes yet to merge it
  };
  struct Lane {
    std::deque<std::shared_ptr<Part>> queue;
    bool running = false;  // a task drains the queue
    bool failed = false;
  };
  struct Failure {
    int64_t part = 0;
    std::size_t lane = 0;
    arrow::Status status;
  };

  // Under mu_: queues `part` on lane `lane` and starts a task for the lane unless one runs. On
  // failure the lane is as it was: the part is not queued and no task was started.
  arrow::Status Queue(std::size_t lane, const std::shared_ptr<Part>& part);
  // Runs lane `lane`'s queue until it is empty or the lane fails.
  void Drain(std::size_t lane);
  // Under mu_: a lane is done with `part` (merged, failed or dropped).
  void Done(Part& part);
  // Under mu_: records a failure of `part` in `lane`, keeping the smallest (part, lane).
  void Fail(int64_t part, std::size_t lane, arrow::Status status);
  static arrow::Status Run(const Merge& merge, std::size_t lane);

  arrow::internal::Executor* executor_;
  int64_t max_pending_;
  const MemoryBudget* budget_;
  mutable std::mutex mu_;
  std::condition_variable changed_;
  std::vector<Lane> lanes_;
  int64_t pending_ = 0;
  std::size_t running_ = 0;  // lanes with a task
  std::optional<Failure> failure_;
};

}  // namespace antb1::exec
