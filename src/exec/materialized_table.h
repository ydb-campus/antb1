#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <arrow/record_batch.h>
#include <arrow/result.h>
#include <arrow/status.h>
#include <arrow/type.h>

#include "antb1/exec/memory_budget.h"
#include "antb1/plan/shared_sub_plans.h"
#include "antb1/plan/table.h"

// A table whose rows are held in memory: a sub-plan computed once and read by several consumers
// (docs/adr/0030-shared-sub-plans.md, E2).

namespace antb1::exec {

class ProfileNode;

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
  // The planner estimates fixed-width storage. Enforce the cap on actual batches as they arrive.
  static constexpr int64_t kMaxBytes = plan::kMaxSharedResultBytes;

  explicit MaterializedTable(std::shared_ptr<arrow::Schema> schema) : schema_(std::move(schema)) {}

  // The profile of the operator tree that fills this table. Scans of the table point at it.
  void set_shared_profile(ProfileNode* node) { shared_profile_ = node; }
  [[nodiscard]] ProfileNode* shared_profile() const { return shared_profile_; }
  // 1-based index among the shared results of one plan. 0 until the planner assigns it.
  // EXPLAIN prints it so every consumer of this result can be matched to the others.
  void set_shared_id(std::size_t id) { shared_id_ = id; }
  [[nodiscard]] std::size_t shared_id() const { return shared_id_; }

  // Charges what it holds to `budget` from now on (nullptr: no budget).
  void Reset(MemoryBudget* budget);
  // False if retaining the batch would exceed the cap or the query budget.
  [[nodiscard]] arrow::Result<bool> TryAppend(std::shared_ptr<arrow::RecordBatch> batch);
  // Says the rows are all here. Until this, a scan is an error rather than an empty answer, and
  // exact_row_count() is unknown rather than zero.
  void Finish();
  // Gives the rows and the bytes back.
  void Release();

  const std::shared_ptr<arrow::Schema>& schema() const override { return schema_; }
  std::optional<int64_t> exact_row_count() const override;
  std::string Describe() const override {
    return shared_id_ == 0 ? std::string("shared") : "shared#" + std::to_string(shared_id_);
  }

 protected:
  arrow::Result<std::unique_ptr<arrow::RecordBatchReader>> DoScan(
      const std::vector<int>& fields, int64_t batch_size, arrow::MemoryPool* pool,
      const std::shared_ptr<plan::ScanContext>& context) const override;

 private:
  friend class MaterializedGroupPartition;
  arrow::Result<std::unique_ptr<arrow::RecordBatchReader>> ProjectBatches(
      const std::vector<int>& fields, int64_t batch_size, int key = -1,
      const arrow::Scalar* value = nullptr) const;

  std::shared_ptr<arrow::Schema> schema_;
  ProfileNode* shared_profile_ = nullptr;
  std::size_t shared_id_ = 0;
  // Guards the batches against the consumer threads: filling happens before any of them starts,
  // but a scan is const and may run on any of them, so the handover needs the lock to be visible.
  mutable std::mutex mutex_;
  arrow::RecordBatchVector batches_;
  int64_t rows_ = 0;
  bool filled_ = false;
  MemoryReservation memory_;
};

// Each emitted batch of a split aggregate belongs to one reader. Keep its boundaries and
// order, so callers see the same stream that their original grouped aggregate produced.
class MaterializedGroupPartition final : public plan::Table {
 public:
  MaterializedGroupPartition(std::shared_ptr<MaterializedTable> table, int key,
                             std::shared_ptr<arrow::Scalar> value)
      : table_(std::move(table)), key_(key), value_(std::move(value)) {}

  const std::shared_ptr<arrow::Schema>& schema() const override { return table_->schema(); }
  std::optional<int64_t> exact_row_count() const override { return std::nullopt; }
  std::string Describe() const override {
    const std::size_t id = table_->shared_id();
    return id == 0 ? std::string("shared-group-partition")
                   : "shared-group-partition#" + std::to_string(id);
  }
  [[nodiscard]] ProfileNode* shared_profile() const { return table_->shared_profile(); }

 protected:
  arrow::Result<std::unique_ptr<arrow::RecordBatchReader>> DoScan(
      const std::vector<int>& fields, int64_t batch_size, arrow::MemoryPool* /*pool*/,
      const std::shared_ptr<plan::ScanContext>& /*context*/) const override {
    return table_->ProjectBatches(fields, batch_size, key_, value_.get());
  }

 private:
  std::shared_ptr<MaterializedTable> table_;
  int key_;
  std::shared_ptr<arrow::Scalar> value_;
};

}  // namespace antb1::exec
