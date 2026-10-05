#include "partition_lanes.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <utility>
#include <vector>

#include <arrow/result.h>
#include <arrow/status.h>
#include <arrow/util/future.h>
#include <arrow/util/thread_pool.h>

#include "antb1/exec/memory_budget.h"

namespace antb1::exec {

arrow::Status ForEach(arrow::internal::Executor* executor, std::size_t n,
                      const std::function<arrow::Status(std::size_t)>& fn) {
  const auto guarded = [&fn](std::size_t i) -> arrow::Status {
    try {
      return fn(i);
    } catch (const std::bad_alloc&) {
      return arrow::Status::OutOfMemory("out of memory in a parallel task");
    }
  };
  if (executor == nullptr || n <= 1) {
    for (std::size_t i = 0; i < n; ++i) {
      ARROW_RETURN_NOT_OK(guarded(i));
    }
    return arrow::Status::OK();
  }
  std::vector<arrow::Future<>> tasks;
  // The tasks use `guarded` and `fn`: every task submitted ends before ForEach returns, however it
  // returns (a std::bad_alloc while a status is made included). Waiting allocates nothing.
  struct WaitAll {
    const std::vector<arrow::Future<>>& futures;
    ~WaitAll() {
      for (const arrow::Future<>& future : futures) {
        future.Wait();
      }
    }
  };
  const WaitAll wait_all{tasks};
  tasks.reserve(n);
  // A Submit that throws std::bad_alloc (Arrow allocates the task) has started nothing, like one
  // that fails: no task runs from that one on. A failed Submit's status moves: no allocation.
  arrow::Status submitted;
  for (std::size_t i = 0; i < n && submitted.ok(); ++i) {
    try {
      arrow::Result<arrow::Future<>> task = executor->Submit([&guarded, i] { return guarded(i); });
      if (task.ok()) {
        tasks.push_back(*std::move(task));  // reserved: nothing allocated, nothing thrown
      } else {
        submitted = std::move(task).status();
      }
    } catch (const std::bad_alloc&) {
      submitted = arrow::Status::OutOfMemory("out of memory while submitting a parallel task");
    }
  }
  for (const arrow::Future<>& task : tasks) {
    ARROW_RETURN_NOT_OK(task.status());  // waits for the task
  }
  return submitted;
}

PartitionLanes::PartitionLanes(std::size_t lanes, arrow::internal::Executor* executor,
                               int64_t max_pending, const MemoryBudget* budget)
    : executor_(executor),
      max_pending_(max_pending < 1 ? 1 : max_pending),
      budget_(budget),
      lanes_(lanes) {}

PartitionLanes::~PartitionLanes() {
  std::unique_lock lock(mu_);
  changed_.wait(lock, [this] { return running_ == 0; });
}

arrow::Status PartitionLanes::Run(const Merge& merge, std::size_t lane) {
  try {
    return merge(lane);
  } catch (const std::bad_alloc&) {
    return arrow::Status::OutOfMemory("out of memory while merging partitions");
  }
}

void PartitionLanes::Fail(int64_t part, std::size_t lane, arrow::Status status) {
  if (!failure_.has_value() || part < failure_->part ||
      (part == failure_->part && lane < failure_->lane)) {
    failure_ = Failure{.part = part, .lane = lane, .status = std::move(status)};
  }
}

void PartitionLanes::Done(Part& part) {
  if (--part.remaining == 0) {
    part.merge = nullptr;  // releases what the merge holds
    --pending_;
    changed_.notify_all();
  }
}

arrow::Status PartitionLanes::Add(int64_t part, Merge merge) {
  if (executor_ == nullptr) {  // every lane in order, here
    {
      const std::scoped_lock lock(mu_);
      if (failure_.has_value()) {
        return failure_->status;
      }
    }
    for (std::size_t lane = 0; lane < lanes_.size(); ++lane) {
      arrow::Status status = Run(merge, lane);
      if (!status.ok()) {
        const std::scoped_lock lock(mu_);
        Fail(part, lane, status);
        return status;
      }
    }
    return arrow::Status::OK();
  }
  std::unique_lock lock(mu_);
  changed_.wait(lock, [this] {
    const bool pressure = budget_ != nullptr && budget_->under_pressure();
    return failure_.has_value() || pending_ < (pressure ? 1 : max_pending_);
  });
  if (failure_.has_value()) {
    lock.unlock();
    return Finish();
  }
  if (lanes_.empty()) {
    return arrow::Status::OK();
  }
  std::shared_ptr<Part> queued;
  try {
    queued = std::make_shared<Part>(
        Part{.part = part, .merge = std::move(merge), .remaining = lanes_.size()});
  } catch (const std::bad_alloc&) {  // no lane has the part, so none may merge a later one
    Fail(part, 0, arrow::Status::OutOfMemory("out of memory while queuing a part"));
    lock.unlock();
    return Finish();
  }
  ++pending_;
  bool failed = false;
  for (std::size_t lane = 0; lane < lanes_.size(); ++lane) {
    Lane& state = lanes_[lane];
    if (state.failed) {
      Done(*queued);
      continue;
    }
    arrow::Status status = Queue(lane, queued);
    if (!status.ok()) {  // the lane fails as if it had failed to merge the part
      failed = true;
      state.failed = true;
      Fail(part, lane, std::move(status));
      Done(*queued);
      while (!state.queue.empty()) {  // the lane stops: its other parts are dropped
        Done(*state.queue.front());
        state.queue.pop_front();
      }
    }
  }
  if (failed) {
    lock.unlock();
    return Finish();
  }
  return arrow::Status::OK();
}

arrow::Status PartitionLanes::Queue(std::size_t lane, const std::shared_ptr<Part>& part) {
  Lane& state = lanes_[lane];
  try {
    state.queue.push_back(part);
  } catch (const std::bad_alloc&) {
    return arrow::Status::OutOfMemory("out of memory while queuing a part");
  }
  if (state.running) {
    return arrow::Status::OK();
  }
  // A Submit that throws std::bad_alloc (Arrow allocates the task) has started nothing, like one
  // that fails. The task waits for mu_, held here, so the lane is marked running before it runs.
  arrow::Status submitted;
  try {
    submitted = executor_->Submit([this, lane] { Drain(lane); }).status();
  } catch (const std::bad_alloc&) {
    submitted = arrow::Status::OutOfMemory("out of memory while starting a partition lane");
  }
  if (!submitted.ok()) {
    state.queue.pop_back();  // the queue of a lane without a task held nothing else
    return submitted;
  }
  state.running = true;
  ++running_;
  return arrow::Status::OK();
}

void PartitionLanes::Drain(std::size_t lane) {
  std::unique_lock lock(mu_);
  Lane& state = lanes_[lane];
  while (!state.queue.empty()) {
    const std::shared_ptr<Part> part = state.queue.front();
    state.queue.pop_front();
    lock.unlock();
    arrow::Status status = Run(part->merge, lane);
    lock.lock();
    if (!status.ok()) {
      state.failed = true;
      Fail(part->part, lane, std::move(status));
      while (!state.queue.empty()) {  // the lane stops: its other parts are dropped
        Done(*state.queue.front());
        state.queue.pop_front();
      }
    }
    Done(*part);
  }
  state.running = false;
  --running_;
  changed_.notify_all();
}

arrow::Status PartitionLanes::Finish() {
  Wait();
  const std::scoped_lock lock(mu_);
  return failure_.has_value() ? failure_->status : arrow::Status::OK();
}

void PartitionLanes::Wait() {
  std::unique_lock lock(mu_);
  changed_.wait(lock, [this] { return running_ == 0; });
}

bool PartitionLanes::failed() const {
  const std::scoped_lock lock(mu_);
  return failure_.has_value();
}

int64_t PartitionLanes::pending() const {
  const std::scoped_lock lock(mu_);
  return pending_;
}

}  // namespace antb1::exec
