#include "partition_lanes.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <utility>

#include <arrow/status.h>
#include <arrow/util/thread_pool.h>

#include "antb1/exec/memory_budget.h"

namespace antb1::exec {

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
    return arrow::Status::OutOfMemory("out of memory while merging groups");
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
  auto queued = std::make_shared<Part>(
      Part{.part = part, .merge = std::move(merge), .remaining = lanes_.size()});
  ++pending_;
  for (std::size_t lane = 0; lane < lanes_.size(); ++lane) {
    Lane& state = lanes_[lane];
    if (state.failed) {
      Done(*queued);
      continue;
    }
    state.queue.push_back(queued);
    if (state.running) {
      continue;
    }
    state.running = true;
    ++running_;
    auto task = executor_->Submit([this, lane] { Drain(lane); });
    if (!task.ok()) {  // nothing runs the lane: drop its queue
      state.running = false;
      --running_;
      state.failed = true;
      Fail(part, lane, task.status());
      while (!state.queue.empty()) {
        Done(*state.queue.front());
        state.queue.pop_front();
      }
      changed_.notify_all();
    }
  }
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

int64_t PartitionLanes::pending() const {
  const std::scoped_lock lock(mu_);
  return pending_;
}

}  // namespace antb1::exec
