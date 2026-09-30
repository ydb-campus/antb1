#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include <arrow/memory_pool.h>
#include <arrow/result.h>
#include <arrow/status.h>
#include <arrow/type_fwd.h>
#include <arrow/util/future.h>
#include <arrow/util/thread_pool.h>

#include "antb1/exec/memory_budget.h"
#include "antb1/exec/operator.h"
#include "antb1/plan/logical_plan.h"

namespace antb1::exec {

// The output schema of a Compute: the input fields, then one field per expression.
std::shared_ptr<arrow::Schema> ComputedSchema(const arrow::Schema& input,
                                              const std::vector<plan::ExprPtr>& exprs);

// The selected rows of `in` with one column per expression appended (ComputeOperator's batch).
arrow::Result<Batch> ComputeBatch(const Batch& in, const std::vector<plan::ExprPtr>& exprs,
                                  const std::shared_ptr<arrow::Schema>& schema,
                                  arrow::MemoryPool* pool);

// ComputeOperator for a Compute outside a part pipeline (over an aggregation or a sort, whose
// batches arrive on the consumer's thread; docs/adr/0013-parallel-execution.md). With an
// executor it reads its input ahead and computes several batches at a time on the executor,
// returning them strictly in input order, so its output, and its first error in batch order, are
// those of ComputeOperator. A batch read ahead but never returned (a Limit above stopped) never
// reports its error, as ComputeOperator never computes it. Without an executor it is
// ComputeOperator.
//
// Memory, as the part scheduler (part_scheduler.h): the batches in flight start at `threads`;
// every batch taken under pressure (MemoryBudget::under_pressure) halves the window, every other
// one widens it by one; under pressure no batch starts while another is in flight. A batch that
// runs out of memory on a worker does not fail the query yet: the batches ahead are dropped
// (they are computed again when reached), the window becomes 1, and the batch is computed again
// alone on the consumer's thread; only that failure fails the query. A task's std::bad_alloc is an
// OutOfMemory status. Results are released on the consumer's thread (Next, Close, the destructor),
// never on a worker.
class ParallelComputeOperator final : public Operator {
 public:
  ParallelComputeOperator(std::unique_ptr<Operator> input, std::vector<plan::ExprPtr> exprs);
  ~ParallelComputeOperator() override;
  ParallelComputeOperator(const ParallelComputeOperator&) = delete;
  ParallelComputeOperator& operator=(const ParallelComputeOperator&) = delete;
  ParallelComputeOperator(ParallelComputeOperator&&) = delete;
  ParallelComputeOperator& operator=(ParallelComputeOperator&&) = delete;

  [[nodiscard]] const std::shared_ptr<arrow::Schema>& output_schema() const override {
    return shared_->schema;
  }
  arrow::Status Open(ExecContext& ctx) override;
  arrow::Result<Batch> Next() override;
  arrow::Status Close() override;

  // The batches computed at a time now (with an executor).
  [[nodiscard]] std::size_t window() const { return window_; }

 private:
  // What the tasks share with the operator; it outlives the operator if it has to.
  struct Shared {
    std::vector<plan::ExprPtr> exprs;
    std::shared_ptr<arrow::Schema> schema;
    std::mutex mutex;
    std::unordered_map<int64_t, arrow::Result<Batch>> slots;  // computed, not yet taken

    // Computes `batch`; a std::bad_alloc is an OutOfMemory status.
    arrow::Result<Batch> Compute(const Batch& batch, arrow::MemoryPool* pool) const;
    // Computes batch `id` on a worker and leaves its result in a slot, so that the worker's
    // copy of the task holds neither the input nor the result once the future completes.
    void ComputeIntoSlot(int64_t id, Batch batch, arrow::MemoryPool* pool);
    arrow::Result<Batch> TakeSlot(int64_t id);
  };

  // An input batch read ahead, until its result is taken (kept to compute it again).
  struct Entry {
    int64_t id = 0;
    Batch input;
    std::optional<arrow::Future<>> done;  // empty: not submitted
  };

  // Starts computing `entry` on the executor; its future completes once its result is in a slot.
  arrow::Result<arrow::Future<>> Submit(Entry& entry);
  // Submits the entries not in flight and reads the input ahead, while the window allows.
  arrow::Status Fill();
  // Waits for the batches in flight and releases their results; the entries stay, unsubmitted.
  void Drop();

  std::unique_ptr<Operator> input_;
  std::shared_ptr<Shared> shared_;
  arrow::MemoryPool* pool_ = arrow::default_memory_pool();
  arrow::internal::Executor* executor_ = nullptr;
  const MemoryBudget* budget_ = nullptr;
  std::size_t max_window_ = 1;
  std::size_t window_ = 1;
  std::size_t in_flight_ = 0;
  int64_t next_id_ = 0;
  bool input_done_ = false;
  arrow::Status input_error_;  // the input's failure, returned after the batches read before it
  std::deque<Entry> entries_;  // in input order
};

}  // namespace antb1::exec
