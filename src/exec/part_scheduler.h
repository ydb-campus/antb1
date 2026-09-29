#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <utility>

#include <arrow/result.h>
#include <arrow/status.h>
#include <arrow/util/future.h>
#include <arrow/util/thread_pool.h>

namespace antb1::exec {

// Runs a task for every part 0 .. num_parts - 1 of a table and hands the results back strictly in
// part order (docs/adr/0013-parallel-execution.md). With an executor, parts run ahead of the
// consumer, at most `window` of them submitted or finished but not yet taken; without one, each
// part runs on the calling thread when Next() reaches it. Either way the results, and so anything
// combined from them in order, do not depend on the number of threads.
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

  PartScheduler(int64_t num_parts, Task task, arrow::internal::Executor* executor, int64_t window)
      : shared_(std::make_shared<Shared>(std::move(task))),
        num_parts_(num_parts),
        executor_(executor),
        window_(window < 1 ? 1 : window) {}

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
      ARROW_RETURN_NOT_OK(Submit());  // keep the window full while the consumer works
    }
    return result;
  }

  // No more parts: running tasks see `stop`, and are waited for.
  void Stop() {
    stopped_ = true;
    shared_->stop = true;
    for (const arrow::Future<T>& future : in_flight_) {
      future.Wait();
    }
    in_flight_.clear();
  }

 private:
  struct Shared {
    explicit Shared(Task t) : task(std::move(t)) {}
    Task task;
    std::atomic<bool> stop = false;
  };

  // The result of part next_result_.
  arrow::Result<T> Take() {
    if (executor_ == nullptr) {
      return shared_->task(next_result_, shared_->stop);
    }
    ARROW_RETURN_NOT_OK(Submit());  // at least this part
    arrow::Future<T> future = std::move(in_flight_.front());
    in_flight_.pop_front();
    return future.MoveResult();
  }

  // Submits parts until the window is full.
  arrow::Status Submit() {
    while (!stopped_ && next_submit_ < num_parts_ && std::cmp_less(in_flight_.size(), window_)) {
      const int64_t part = next_submit_;
      // The task holds the shared state, so it outlives the scheduler if it has to.
      ARROW_ASSIGN_OR_RAISE(arrow::Future<T> future, executor_->Submit([shared = shared_, part]() {
        return shared->task(part, shared->stop);
      }));
      in_flight_.push_back(std::move(future));
      ++next_submit_;
    }
    return arrow::Status::OK();
  }

  std::shared_ptr<Shared> shared_;
  int64_t num_parts_;
  arrow::internal::Executor* executor_;
  int64_t window_;
  int64_t next_submit_ = 0;  // with an executor: the next part to submit
  int64_t next_result_ = 0;  // the next part whose result Next() returns
  bool stopped_ = false;
  // With an executor: parts next_result_ .. next_submit_ - 1, submitted and not yet taken.
  std::deque<arrow::Future<T>> in_flight_;
};

}  // namespace antb1::exec
