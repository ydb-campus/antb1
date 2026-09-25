#pragma once

#include <cstdint>
#include <memory>

#include <arrow/array/array_primitive.h>
#include <arrow/memory_pool.h>
#include <arrow/record_batch.h>
#include <arrow/result.h>
#include <arrow/status.h>
#include <arrow/table.h>

// Pull-based, batch-at-a-time physical operators (docs/adr/0003-engine-architecture.md). Execution
// is single-threaded and deterministic: every operator pulls its input in order. Operators that
// call Arrow compute kernels need arrow::compute::Initialize() (engine::Session::Make calls it).

namespace antb1::exec {

struct ExecContext {
  arrow::MemoryPool* pool = arrow::default_memory_pool();
  int64_t batch_size = int64_t{64} * 1024;
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
};

// The selected rows of a batch, without a selection (the data itself when every row is selected).
arrow::Result<std::shared_ptr<arrow::RecordBatch>> Materialize(const Batch& batch,
                                                               arrow::MemoryPool* pool);

// Opens op, pulls every batch into a table (materializing selections), closes op.
arrow::Result<std::shared_ptr<arrow::Table>> Drain(Operator& op, ExecContext& ctx);

}  // namespace antb1::exec
