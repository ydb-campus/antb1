#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <arrow/record_batch.h>
#include <arrow/result.h>
#include <arrow/status.h>
#include <arrow/type.h>

#include "antb1/exec/memory_budget.h"
#include "antb1/plan/table.h"

// A table whose rows are held in memory: a sub-plan computed once and read by several consumers
// (docs/adr/0030-shared-sub-plans.md, E2).

namespace antb1::exec {

// Made empty, filled once, then read. The planner makes it and rewrites each consumer to a scan of
// it; the operator that owns it fills it before the consumers run and releases it with the query.
//
// **One part, deliberately.** The physical planner reads num_parts() while it *builds* operators
// (PartsOf), which is before anything has run and therefore before this can hold a row -- so a
// chunk-per-part table would hand its consumers zero parts and they would read nothing. One part
// read at Open is the shape that works, and it costs the consumers part-parallelism over a result
// that is small by the gate that chose it (E2 shares a sub-plan only where it reduces).
class MaterializedTable final : public plan::Table {
 public:
  // The most one shared result may hold. The gate that chose to share cannot predict this: it
  // admits a sub-plan that *reduces*, and nothing records an aggregate's output cardinality, so the
  // size is known only as the rows arrive. Hence a cap enforced here rather than a prediction made
  // there -- and a query whose shared result passes it fails **naming the shared sub-plan**, which
  // is a legible failure rather than the generic out-of-memory the budget alone would give.
  //
  // Generous against what sharing is for: the one result of this workload that is worth sharing is
  // about 24 MB, and a reducing sub-plan that produces a thousand times that is not the case this
  // was built for.
  static constexpr int64_t kMaxBytes = int64_t{256} * 1024 * 1024;

  explicit MaterializedTable(std::shared_ptr<arrow::Schema> schema) : schema_(std::move(schema)) {}

  // Charges what it holds to `budget` from now on (nullptr: no budget).
  void Reset(MemoryBudget* budget);
  // Appends `batch`, charging its bytes. Called on one thread, before any consumer scans.
  arrow::Status Append(std::shared_ptr<arrow::RecordBatch> batch);
  // Says the rows are all here. Until this, a scan is an error rather than an empty answer, and
  // exact_row_count() is unknown rather than zero.
  void Finish();
  // Gives the rows and the bytes back.
  void Release();

  const std::shared_ptr<arrow::Schema>& schema() const override { return schema_; }
  std::optional<int64_t> exact_row_count() const override;
  std::string Describe() const override { return "shared"; }

 protected:
  arrow::Result<std::unique_ptr<arrow::RecordBatchReader>> DoScan(
      const std::vector<int>& fields, int64_t batch_size, arrow::MemoryPool* pool) const override;

 private:
  std::shared_ptr<arrow::Schema> schema_;
  // Guards the batches against the consumer threads: filling happens before any of them starts,
  // but a scan is const and may run on any of them, so the handover needs the lock to be visible.
  mutable std::mutex mutex_;
  arrow::RecordBatchVector batches_;
  int64_t rows_ = 0;
  bool filled_ = false;
  MemoryReservation memory_;
};

}  // namespace antb1::exec
