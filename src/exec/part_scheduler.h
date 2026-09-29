#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <unordered_map>
#include <utility>

#include <arrow/result.h>
#include <arrow/status.h>
#include <arrow/util/future.h>
#include <arrow/util/thread_pool.h>

#include "antb1/exec/memory_budget.h"

namespace antb1::exec {

// Runs a task for every part 0 .. num_parts - 1 of a table and hands the results back strictly in
// part order (docs/adr/0013-parallel-execution.md). With an executor, parts run ahead of the
// consumer, at most `window` of them submitted or finished but not yet taken; without one, each
// part runs on the calling thread when Next() reaches it. Either way the results, and so anything
// combined from them in order, do not depend on the number of threads.
//
// Memory: the window adapts to a budget (MemoryBudget::under_pressure): every part the consumer
// takes under pressure halves it, every part taken without pressure widens it by one, up to
// `window`; under pressure no new part is submitted while another is in flight. A part that runs
// out of memory while parts run in parallel does not fail the query yet: the parts ahead of it are
// stopped and dropped (freeing their memory; they run again when reached), the window becomes 1,
// and the part runs again alone on the calling thread. Only the schedule changes, never the
// results: a part computes the same result whenever it runs.
//
// Errors: Next() returns the first failed part in part order, when it reaches it, and stops the
// parts after it. A part the consumer never reaches (it stopped early) never reports its error.
//
// A task sees `stop` become true once the scheduler is stopped (the consumer is done or failed);
// it should check it between batches and may then return any status, which is dropped. Stop() and
// the destructor wait for every running task, so tasks may use what the scheduler's owner owns.
template <class T>
class PartScheduler {
 public:
  using Task = std::function<arrow::Result<T>(int64_t part, const std::atomic<bool>& stop)>;

  PartScheduler(int64_t num_parts, Task task, arrow::internal::Executor* executor, int64_t window,
                const MemoryBudget* budget = nullptr)
      : shared_(std::make_shared<Shared>(std::move(task))),
        num_parts_(num_parts),
        executor_(executor),
        max_window_(window < 1 ? 1 : window),
        window_(max_window_),
        budget_(budget) {}

  PartScheduler(const PartScheduler&) = delete;
  PartScheduler& operator=(const PartScheduler&) = delete;
  PartScheduler(PartScheduler&&) = delete;
  PartScheduler& operator=(PartScheduler&&) = delete;
  ~PartScheduler() { Stop(); }

  // Whether every part's result has been taken (or the scheduler was stopped).
  [[nodiscard]] bool done() const { return stopped_ || next_result_ >= num_parts_; }

  // The result of the next part in order. Invalid once done().
  arrow::Result<T> Next() {
    if (done()) {
      return arrow::Status::Invalid("part scheduler: no parts left");
    }
    arrow::Result<T> result = Take();
    ++next_result_;
    if (!result.ok()) {
      Stop();
      return result;
    }
    if (executor_ != nullptr) {
      if (budget_ != nullptr && budget_->under_pressure()) {
        window_ = std::max<int64_t>(1, window_ / 2);
      } else {
        window_ = std::min(max_window_, window_ + 1);
      }
      ARROW_RETURN_NOT_OK(Submit());  // keep the window full while the consumer works
    }
    return result;
  }

  // The parts that may be in flight now (with an executor).
  [[nodiscard]] int64_t window() const { return window_; }

  // No more parts: running tasks see `stop`, and are waited for; their results are released here.
  void Stop() {
    stopped_ = true;
    shared_->stop = true;
    Drop();
  }

 private:
  struct Shared {
    explicit Shared(Task t) : task(std::move(t)) {}
    // The task of `part`; running out of memory outside the budget (std::bad_alloc) is an
    // OutOfMemory status, never an exception on an executor thread.
    arrow::Result<T> Run(int64_t part) {
      try {
        return task(part, stop);
      } catch (const std::bad_alloc&) {
        return arrow::Status::OutOfMemory("out of memory while reading part ", part);
      }
    }
    // Runs part `part` on an executor thread and leaves its result in a slot, so that the
    // executor's copy of the task's future holds no result: the scheduler's thread takes the
    // result (Take) or releases it (Drop), never an executor thread after the future completes.
    void RunIntoSlot(int64_t part) {
      arrow::Result<T> result = Run(part);
      const std::scoped_lock lock(mutex);
      slots.insert_or_assign(part, std::move(result));
    }
    arrow::Result<T> TakeSlot(int64_t part) {
      const std::scoped_lock lock(mutex);
      auto it = slots.find(part);
      if (it == slots.end()) {
        return arrow::Status::Invalid("part scheduler: no result for part ", part);
      }
      arrow::Result<T> result = std::move(it->second);
      slots.erase(it);
      return result;
    }
    void ClearSlots() {
      std::unordered_map<int64_t, arrow::Result<T>> dropped;
      {
        const std::scoped_lock lock(mutex);
        dropped.swap(slots);
      }
    }

    Task task;
    std::atomic<bool> stop = false;
    std::mutex mutex;
    std::unordered_map<int64_t, arrow::Result<T>> slots;  // finished parts not yet taken
  };

  // The result of part next_result_.
  arrow::Result<T> Take() {
    if (executor_ == nullptr) {
      return shared_->Run(next_result_);
    }
    ARROW_RETURN_NOT_OK(Submit());  // at least this part
    const arrow::Future<> future = std::move(in_flight_.front());
    in_flight_.pop_front();
    ARROW_RETURN_NOT_OK(future.status());
    arrow::Result<T> result = shared_->TakeSlot(next_result_);
    if (result.ok() || !result.status().IsOutOfMemory()) {
      return result;
    }
    // Out of memory in parallel: drop the parts ahead (their memory with them; they run again
    // when reached) and run this part alone.
    shared_->stop = true;
    Drop();
    shared_->stop = false;  // no task runs any more
    next_submit_ = next_result_ + 1;
    window_ = 1;
    return shared_->Run(next_result_);
  }

  // Waits for the parts in flight and releases their results on this thread.
  void Drop() {
    for (const arrow::Future<>& future : in_flight_) {
      future.Wait();
    }
    in_flight_.clear();
    shared_->ClearSlots();
  }

  // Submits parts until the window is full.
  arrow::Status Submit() {
    while (!stopped_ && next_submit_ < num_parts_ && std::cmp_less(in_flight_.size(), window_)) {
      if (!in_flight_.empty() && budget_ != nullptr && budget_->under_pressure()) {
        break;
      }
      const int64_t part = next_submit_;
      // The task holds the shared state, so it outlives the scheduler if it has to.
      ARROW_ASSIGN_OR_RAISE(arrow::Future<> future, executor_->Submit([shared = shared_, part]() {
        shared->RunIntoSlot(part);
        return arrow::Status::OK();
      }));
      in_flight_.push_back(std::move(future));
      ++next_submit_;
    }
    return arrow::Status::OK();
  }

  std::shared_ptr<Shared> shared_;
  int64_t num_parts_;
  arrow::internal::Executor* executor_;
  int64_t max_window_;
  int64_t window_;  // max_window_ halved under pressure, widened again without
  const MemoryBudget* budget_;
  int64_t next_submit_ = 0;  // with an executor: the next part to submit
  int64_t next_result_ = 0;  // the next part whose result Next() returns
  bool stopped_ = false;
  // With an executor: parts next_result_ .. next_submit_ - 1, submitted and not yet taken.
  std::deque<arrow::Future<>> in_flight_;
};

}  // namespace antb1::exec
