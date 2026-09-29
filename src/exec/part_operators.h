#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include <arrow/result.h>
#include <arrow/status.h>
#include <arrow/type_fwd.h>

#include "antb1/exec/operator.h"
#include "antb1/exec/sort.h"
#include "antb1/plan/logical_plan.h"

#include "aggregate_set.h"
#include "group_table.h"
#include "part_scheduler.h"

// The operators that run a pipeline once per table part (docs/adr/0013-parallel-execution.md). A
// part pipeline is a chain of streaming operators over a TableScanOperator of one part; the
// physical planner builds a fresh one per part, so parts share no operator state.

namespace antb1::exec {

// Builds the pipeline of one part. Called concurrently from several threads.
using PartPipeline = std::function<arrow::Result<std::unique_ptr<Operator>>(int64_t part)>;

// The batches of every part in part order: a source that stands for the pipeline's top. Parts
// run ahead on ExecContext::executor, at most 2 * threads of them at a time. With a row cap (a
// LIMIT above it needs at most limit + offset rows), each part stops after that many selected rows.
// Batches keep their selections; batches without selected rows are dropped.
class PartUnionOperator final : public Operator {
 public:
  PartUnionOperator(PartPipeline pipeline, int64_t num_parts, std::shared_ptr<arrow::Schema> schema,
                    std::optional<int64_t> row_cap);
  ~PartUnionOperator() override;

  [[nodiscard]] const std::shared_ptr<arrow::Schema>& output_schema() const override {
    return schema_;
  }
  arrow::Status Open(ExecContext& ctx) override;
  arrow::Result<Batch> Next() override;
  arrow::Status Close() override;

 private:
  using PartBatches = std::shared_ptr<std::vector<Batch>>;

  std::shared_ptr<const PartPipeline> pipeline_;
  int64_t num_parts_;
  std::shared_ptr<arrow::Schema> schema_;
  std::optional<int64_t> row_cap_;
  PartBatches current_;
  std::size_t next_ = 0;  // the next batch of current_
  std::unique_ptr<PartScheduler<PartBatches>> scheduler_;
};

// Global aggregation over a part pipeline: every part is aggregated into its own AggregateSet on
// the executor, and the sets are merged in part order. Output: as ScalarAggregateOperator.
class PartAggregateOperator final : public Operator {
 public:
  PartAggregateOperator(PartPipeline pipeline, int64_t num_parts, int input_width,
                        std::vector<plan::AggregateCall> aggregates);
  ~PartAggregateOperator() override;

  [[nodiscard]] const std::shared_ptr<arrow::Schema>& output_schema() const override {
    return schema_;
  }
  arrow::Status Open(ExecContext& ctx) override;
  arrow::Result<Batch> Next() override;
  arrow::Status Close() override;

 private:
  using PartStates = std::shared_ptr<AggregateSet>;

  std::shared_ptr<const PartPipeline> pipeline_;
  int64_t num_parts_;
  int input_width_;
  std::shared_ptr<const std::vector<plan::AggregateCall>> aggregates_;
  std::shared_ptr<arrow::Schema> schema_;
  arrow::MemoryPool* pool_ = arrow::default_memory_pool();
  bool done_ = true;
  std::unique_ptr<PartScheduler<PartStates>> scheduler_;
};

// Grouped aggregation over a part pipeline: every part is grouped into its own GroupTable on the
// executor and split into partitions by the hash of its keys (GroupTable::Partition). The parts are
// merged in part order into one table per partition; a part's partitions merge in parallel on the
// executor, each partition's table touched by one task at a time. Then the partitions' groups are
// emitted, partition by partition, as GroupAggregateOperator emits them. The partitions are a
// constant, so the result is the same for any number of threads. Output: as
// GroupAggregateOperator.
class PartGroupAggregateOperator final : public Operator {
 public:
  PartGroupAggregateOperator(PartPipeline pipeline, int64_t num_parts, int input_width,
                             std::vector<plan::BoundColumn> keys,
                             std::vector<plan::AggregateCall> aggregates,
                             std::shared_ptr<arrow::Schema> schema);
  ~PartGroupAggregateOperator() override;

  [[nodiscard]] const std::shared_ptr<arrow::Schema>& output_schema() const override {
    return schema_;
  }
  arrow::Status Open(ExecContext& ctx) override;
  arrow::Result<Batch> Next() override;
  arrow::Status Close() override;

 private:
  using PartTable = std::shared_ptr<GroupTable>;

  std::shared_ptr<const PartPipeline> pipeline_;
  int64_t num_parts_;
  int input_width_;
  std::vector<plan::BoundColumn> keys_;
  std::vector<plan::AggregateCall> aggregates_;
  std::shared_ptr<arrow::Schema> schema_;
  arrow::MemoryPool* pool_ = arrow::default_memory_pool();
  MemoryBudget* budget_ = nullptr;
  arrow::internal::Executor* executor_ = nullptr;
  // The merged groups of each partition (created with the first part), until emitted.
  std::vector<std::unique_ptr<GroupTable>> tables_;
  std::size_t next_table_ = 0;  // the partition being emitted
  bool opened_ = false;
  bool merged_ = false;

  // Merges a part's partitions into tables_, on the executor when there is one.
  arrow::Status MergePart(const GroupTable& part);
  std::unique_ptr<PartScheduler<PartTable>> scheduler_;
};

// ORDER BY with a LIMIT (top-N) over a part pipeline: every part keeps its first limit + offset
// rows of the order in its own SortBuffer on the executor, and the buffers are merged in part order
// into one, which keeps the first limit + offset rows again. Ties keep their input order, parts in
// part order: exactly the rows and order of SortOperator's top-N over the part union, for any
// number of threads. Output: the window [offset, offset + limit) of the order, in the pipeline's
// schema.
class PartTopNOperator final : public Operator {
 public:
  PartTopNOperator(PartPipeline pipeline, int64_t num_parts, std::shared_ptr<arrow::Schema> schema,
                   std::vector<plan::SortKey> keys, int64_t limit, int64_t offset);
  ~PartTopNOperator() override;

  [[nodiscard]] const std::shared_ptr<arrow::Schema>& output_schema() const override {
    return schema_;
  }
  arrow::Status Open(ExecContext& ctx) override;
  arrow::Result<Batch> Next() override;
  arrow::Status Close() override;

 private:
  // A part's first rows, and the reservation of the buffer's own containers, which lasts until
  // the part is merged (while it waits in the scheduler's window, too).
  struct PartRows {
    PartRows(SortBuffer rows, MemoryBudget* budget) : buffer(std::move(rows)) {
      memory.Reset(budget);
    }
    SortBuffer buffer;
    MemoryReservation memory;
  };
  using PartBuffer = std::shared_ptr<PartRows>;

  std::shared_ptr<const PartPipeline> pipeline_;
  int64_t num_parts_;
  std::shared_ptr<arrow::Schema> schema_;
  std::vector<plan::SortKey> keys_;
  int64_t limit_;
  int64_t offset_;
  arrow::MemoryPool* pool_ = arrow::default_memory_pool();
  int64_t batch_size_ = 1;
  std::unique_ptr<SortBuffer> merged_;  // the parts' first rows, until emitted
  MemoryReservation memory_;            // merged_'s own containers
  bool sorted_ = false;
  int64_t next_ = 0;  // the next sorted row to emit
  int64_t end_ = 0;   // one past the last
  std::unique_ptr<PartScheduler<PartBuffer>> scheduler_;
};

}  // namespace antb1::exec
