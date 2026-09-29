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
// executor, and the tables are merged in part order into one, whose groups are then emitted as
// GroupAggregateOperator emits them. Output: as GroupAggregateOperator.
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
  std::unique_ptr<GroupTable> table_;  // the merged groups, until emitted
  bool merged_ = false;
  std::unique_ptr<PartScheduler<PartTable>> scheduler_;
};

}  // namespace antb1::exec
