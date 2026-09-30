#pragma once

#include <cstdint>
#include <memory>

#include <arrow/array/array_primitive.h>
#include <arrow/memory_pool.h>
#include <arrow/record_batch.h>
#include <arrow/result.h>
#include <arrow/status.h>
#include <arrow/table.h>

// Pull-based, batch-at-a-time physical operators (docs/adr/0003-engine-architecture.md). Every
// operator instance is used by one thread and pulls its input in order. The pipelines below the
// first blocking operator run once per table part, on ExecContext::executor when there is one, and
// their results are combined in part order (docs/adr/0013-parallel-execution.md): the result does
// not depend on the number of threads. Operators that call Arrow compute kernels need
// arrow::compute::Initialize() (engine::Session::Make calls it).

namespace arrow::internal {
class Executor;
}  // namespace arrow::internal

namespace antb1::exec {

class MemoryBudget;
class ProfileNode;

struct ExecContext {
  // Every buffer of the query; the session's MemoryBudget when there is one.
  arrow::MemoryPool* pool = arrow::default_memory_pool();
  int64_t batch_size = int64_t{64} * 1024;
  // Runs the parts of a pipeline; nullptr runs them one after another on the calling thread.
  arrow::internal::Executor* executor = nullptr;
  // The executor's threads (1 without one): parts in flight are bounded by twice this.
  int threads = 1;
  // The budget `pool` counts against (nullptr: no limit, nothing to reserve). Operators reserve
  // the memory of their own containers here, and parts are started one at a time under pressure.
  MemoryBudget* budget = nullptr;
};

// Rows flowing from one operator to the next: the columns and, optionally, a selection. A selection
// is a boolean array of the batch's length without NULLs; a row takes part only where it is true.
// Filter only builds selections; aggregates consume them directly (COUNT(*) is a true count), while
// Project, Limit and Drain materialize the selected rows.
struct Batch {
  std::shared_ptr<arrow::RecordBatch> data;        // nullptr: the end of the stream
  std::shared_ptr<arrow::BooleanArray> selection;  // nullptr: every row

  [[nodiscard]] bool end() const { return data == nullptr; }
  // The number of rows that take part (0 at the end of the stream).
  [[nodiscard]] int64_t selected_rows() const;
};

class Operator {
 public:
  virtual ~Operator() = default;
  Operator() = default;
  Operator(const Operator&) = delete;
  Operator& operator=(const Operator&) = delete;
  Operator(Operator&&) = delete;
  Operator& operator=(Operator&&) = delete;

  // The schema of every batch's data. Field names are internal: the engine names the result.
  [[nodiscard]] virtual const std::shared_ptr<arrow::Schema>& output_schema() const = 0;
  // Prepares a (re)run; opens the inputs first.
  virtual arrow::Status Open(ExecContext& ctx) = 0;
  // The next batch, or an end() batch once the stream is exhausted.
  virtual arrow::Result<Batch> Next() = 0;
  // Releases the resources of the run; closes the inputs.
  virtual arrow::Status Close() = 0;

  // The node the operator adds its own metrics to (profile.h), set by the physical planner when
  // the query is profiled; nullptr otherwise.
  void set_profile(ProfileNode* profile) { profile_ = profile; }

 protected:
  [[nodiscard]] ProfileNode* profile() const { return profile_; }

 private:
  ProfileNode* profile_ = nullptr;
};

// The selected rows of a batch, without a selection (the data itself when every row is selected).
arrow::Result<std::shared_ptr<arrow::RecordBatch>> Materialize(const Batch& batch,
                                                               arrow::MemoryPool* pool);

// Opens op, pulls every batch into a table (materializing selections), closes op.
arrow::Result<std::shared_ptr<arrow::Table>> Drain(Operator& op, ExecContext& ctx);

}  // namespace antb1::exec
