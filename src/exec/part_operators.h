#pragma once

#include <atomic>
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
#include "outer_groups.h"
#include "part_scheduler.h"
#include "partition_lanes.h"

// The operators that run a pipeline once per table part (docs/adr/0013-parallel-execution.md). A
// part pipeline is a chain of streaming operators over a TableScanOperator of one part; the
// physical planner builds a fresh one per part, so parts share no operator state.

namespace antb1::exec {

// Builds the pipeline of one part. Called concurrently from several threads.
using PartPipeline = std::function<arrow::Result<std::unique_ptr<Operator>>(int64_t part)>;

// The context of a part's pipeline: a part runs single-threaded, its operators never see the
// executor.
ExecContext PartContext(const ExecContext& ctx);

// The number of parts that may run ahead of the consumer: 2 * threads with an executor, else 1.
int64_t PartWindow(const ExecContext& ctx);

// Opens the part's pipeline, hands every batch with selected rows to `consume` until it returns
// false or the pipeline ends, and closes the pipeline. Once `stop` is set the part is abandoned
// (its status is dropped by the scheduler).
arrow::Status RunPart(const PartPipeline& pipeline, int64_t part, ExecContext ctx,
                      const std::atomic<bool>& stop,
                      const std::function<arrow::Result<bool>(Batch)>& consume);

// The sink of a part pipeline: an operator that runs the pipeline's parts (PartScheduler) and
// combines their results in part order. Once it has every part's result, so that no part of its
// pipeline runs any more, it calls its parts-done callback (if one is set), once per run, on the
// consumer thread: the builds a probe pipeline reads are released then
// (docs/adr/0022-joins-and-query-blocks.md). A run that fails, or that its consumer stops early,
// does not call it.
class PartSink : public Operator {
 public:
  void set_parts_done(std::function<void()> parts_done) { parts_done_ = std::move(parts_done); }

 protected:
  void PartsDone() const {
    if (parts_done_) {
      parts_done_();
    }
  }

 private:
  std::function<void()> parts_done_;
};

// The batches of every part in part order: a source that stands for the pipeline's top. Parts
// run ahead on ExecContext::executor, at most 2 * threads of them at a time. With a row cap (a
// LIMIT above it needs at most limit + offset rows), each part stops after that many selected rows.
// Batches keep their selections; batches without selected rows are dropped. Its parts are done
// once it has handed on the last part's batches.
class PartUnionOperator final : public PartSink {
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
  bool ended_ = false;    // every part's batches were handed on
  std::unique_ptr<PartScheduler<PartBatches>> scheduler_;
};

// Global aggregation over a part pipeline: every part is aggregated into its own AggregateSet on
// the executor, and the sets are merged in part order. Output: as ScalarAggregateOperator.
class PartAggregateOperator final : public PartSink {
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

// The top-N above a grouped aggregation (Limit over Sort directly over it): the sort keys, over the
// aggregation's output, and the rows the top-N needs (limit + offset).
struct PartitionTopN {
  std::vector<plan::SortKey> keys;
  int64_t keep = 0;
};

// The first `keep` rows of `rows` in `comparator`'s order (a stable sort: ties keep their order),
// as one batch; `rows` unchanged when they hold at most `keep` rows (a stable sort above gives the
// same order). The sort buffer's own containers are reserved on `budget` while it works; nothing
// is left reserved or allocated when it fails.
arrow::Result<std::vector<std::shared_ptr<arrow::RecordBatch>>> KeepFirstRows(
    std::vector<std::shared_ptr<arrow::RecordBatch>> rows, const RowComparator& comparator,
    int64_t keep, arrow::MemoryPool* pool, MemoryBudget* budget);

// Grouped aggregation over a part pipeline: every part is grouped into its own GroupTable on the
// executor and split into partitions by the hash of its keys (GroupTable::Partition). The parts are
// merged in part order into one table per partition, each partition in its own lane on the
// executor (PartitionLanes): a partition never waits for another. Then the partitions' rows are
// built in parallel, as many partitions at a time as threads (one under memory pressure), and
// emitted partition by partition, as GroupAggregateOperator emits them. The partitions are a
// constant, so the result is the same for any number of threads. Output: as
// GroupAggregateOperator. With a PartitionTopN (a top-N above it), each partition keeps only its
// first `keep` rows in the top-N's order (a stable sort, SortBuffer) as it builds them, in
// parallel: the top-N above then reads at most 64 * keep rows, and picks the rows and the order it
// would have picked from all of them (ties stay in partition order, then in group order).
class PartGroupAggregateOperator final : public PartSink {
 public:
  PartGroupAggregateOperator(PartPipeline pipeline, int64_t num_parts, int input_width,
                             std::vector<plan::BoundColumn> keys,
                             std::vector<plan::AggregateCall> aggregates,
                             std::shared_ptr<arrow::Schema> schema,
                             std::optional<PartitionTopN> top_n = std::nullopt);
  ~PartGroupAggregateOperator() override;

  [[nodiscard]] const std::shared_ptr<arrow::Schema>& output_schema() const override {
    return schema_;
  }
  arrow::Status Open(ExecContext& ctx) override;
  arrow::Result<Batch> Next() override;
  arrow::Status Close() override;

 private:
  // A part's groups: its own table (partitioned), and, when the part stopped aggregating on its own
  // (its first batch hardly reduced), the rest of its rows routed to the partitions.
  struct PartGroups {
    std::shared_ptr<GroupTable> table;
    std::vector<std::vector<std::shared_ptr<arrow::RecordBatch>>> rows;  // per partition, or empty
  };
  using PartTable = std::shared_ptr<PartGroups>;

  std::shared_ptr<const PartPipeline> pipeline_;
  int64_t num_parts_;
  int input_width_;
  std::vector<plan::BoundColumn> keys_;
  std::vector<plan::AggregateCall> aggregates_;
  std::shared_ptr<arrow::Schema> schema_;
  std::optional<PartitionTopN> top_n_;
  std::optional<RowComparator> comparator_;  // with top_n_: its order, over schema_
  arrow::MemoryPool* pool_ = arrow::default_memory_pool();
  MemoryBudget* budget_ = nullptr;
  arrow::internal::Executor* executor_ = nullptr;
  // The merged groups of each partition (created with the first part), until emitted.
  std::vector<std::unique_ptr<GroupTable>> tables_;
  // After the merge: every partition's rows, built in parallel, until emitted.
  std::vector<std::vector<std::shared_ptr<arrow::RecordBatch>>> rows_;
  std::size_t next_table_ = 0;  // the partition being emitted
  std::size_t next_chunk_ = 0;  // and its next batch
  std::size_t built_ = 0;       // the partitions whose rows are built
  std::size_t threads_ = 1;
  bool opened_ = false;
  bool merged_ = false;

  // Runs every part and merges it into tables_, each partition in its own lane (PartitionLanes).
  arrow::Status Merge();
  // Builds the rows of the next partitions (from built_ on) in parallel.
  arrow::Status BuildRows();
  int64_t window_ = 1;
  std::unique_ptr<PartScheduler<PartTable>> scheduler_;
};

// Whether PartTwoLevelAggregateOperator runs an aggregation by `keys` (none: a global one) with
// `calls`: some COUNT(DISTINCT) call whose column is not a key, no DOUBLE key, and every other call
// independent of the order its rows merge in (COUNT, integer SUM and AVG, DATE and TIMESTAMP AVG,
// MIN and MAX but of DOUBLE), so that the result is the serial one exactly.
bool TwoLevelAggregation(const std::vector<plan::BoundColumn>& keys,
                         const std::vector<plan::AggregateCall>& calls);

// The input rows (by part_rows) the two-level aggregation samples its heavy keys from.
inline constexpr int64_t kTwoLevelSampleRows = int64_t{4} * 1000 * 1000;

// An aggregation with COUNT(DISTINCT) over a part pipeline in two levels
// (docs/adr/0014-two-level-aggregation.md): an inner GROUP BY of the keys K and each distinct
// column x (one inner table per column, plus a plain table by K for the other calls), then an outer
// GROUP BY K over the inner groups, which counts each column's inner groups with a non-NULL x.
// Every part groups into its own inner tables on the executor. The first `sample_parts` parts
// decide the heavy keys (a K with a large share of the sample's inner groups, HeavyHitters). Every
// table splits into GroupTable::kPartitions partitions by the hash of K, but the inner groups of a
// heavy K by the hash of K and x, so that no partition holds most of the pairs. Parts merge in part
// order, each partition in its own lane (PartitionLanes); then each partition groups its inner
// groups by K in
// parallel. The groups of a light K are complete in their partition; those of a heavy K are merged
// across the partitions, in partition order. Output: the light groups partition by partition, then
// the heavy ones; the columns of GroupAggregateOperator (`global`: of ScalarAggregateOperator, one
// row even without input). The same for any number of threads.
class PartTwoLevelAggregateOperator final : public PartSink {
 public:
  PartTwoLevelAggregateOperator(PartPipeline pipeline, int64_t num_parts, int64_t sample_parts,
                                int input_width, std::vector<plan::BoundColumn> keys,
                                std::vector<plan::AggregateCall> aggregates,
                                std::shared_ptr<arrow::Schema> schema, bool global);
  ~PartTwoLevelAggregateOperator() override;

  [[nodiscard]] const std::shared_ptr<arrow::Schema>& output_schema() const override {
    return schema_;
  }
  arrow::Status Open(ExecContext& ctx) override;
  arrow::Result<Batch> Next() override;
  arrow::Status Close() override;

 private:
  // A part's inner tables: one per distinct column, then the plain table if there are plain calls.
  // A sampled part also has the hash of K of every group of its distinct tables, table after
  // table (computed on its worker).
  struct InnerPart {
    std::vector<std::unique_ptr<GroupTable>> tables;
    std::vector<std::uint64_t> key_hashes;
  };
  using PartTables = std::shared_ptr<InnerPart>;

  // Runs every part and both levels.
  arrow::Status Aggregate();
  // The merge of a part's partition p into tables_[*][p], for PartitionLanes.
  PartitionLanes::Merge MergeOf(PartTables inner);
  // The outer groups of partition `partition`: the light ones as output batches (rows_), the
  // heavy ones left for the merge across partitions.
  arrow::Status Outer(std::size_t partition);

  std::shared_ptr<const PartPipeline> pipeline_;
  int64_t num_parts_;
  int64_t sample_parts_;
  int input_width_;
  std::vector<plan::BoundColumn> keys_;
  std::vector<plan::AggregateCall> aggregates_;
  std::shared_ptr<arrow::Schema> schema_;
  bool global_;
  std::vector<plan::BoundColumn> distinct_;   // the distinct columns, in order of first use
  std::vector<plan::AggregateCall> plain_;    // the other calls
  std::vector<OuterSlot> slots_;              // per call: its count or plain state
  std::vector<plan::LogicalType> key_types_;  // K's
  arrow::MemoryPool* pool_ = arrow::default_memory_pool();
  MemoryBudget* budget_ = nullptr;
  arrow::internal::Executor* executor_ = nullptr;
  ExecContext part_ctx_;
  int64_t window_ = 1;
  bool opened_ = false;
  bool aggregated_ = false;
  std::vector<std::uint64_t> heavy_;  // the hashes of the heavy K, sorted
  // Per inner table, per partition: the merged inner groups, until their partition's outer step.
  std::vector<std::vector<std::unique_ptr<GroupTable>>> tables_;
  // Per partition: the outer groups and which of them are heavy, until merged.
  std::vector<std::unique_ptr<OuterGroups>> outer_;
  std::vector<std::vector<std::uint32_t>> heavy_groups_;
  // The output: per partition, its light groups' batches; then the heavy K's groups.
  std::vector<std::vector<std::shared_ptr<arrow::RecordBatch>>> rows_;
  std::unique_ptr<OuterGroups> heavy_table_;
  std::size_t next_partition_ = 0;  // emitting: the partition (rows_.size(): heavy_table_),
  std::size_t next_rows_ = 0;       // its next batch,
  std::uint32_t next_group_ = 0;    // or the first heavy group of the next chunk
};

// ORDER BY with a LIMIT (top-N) over a part pipeline: every part keeps its first limit + offset
// rows of the order in its own SortBuffer on the executor, and the buffers are merged in part order
// into one, which keeps the first limit + offset rows again. Ties keep their input order, parts in
// part order: exactly the rows and order of SortOperator's top-N over the part union, for any
// number of threads. Output: the window [offset, offset + limit) of the order, in the pipeline's
// schema.
// Late materialization of a top-N (docs/adr/0016-late-materialization.md): the part pipelines scan
// narrowly (LateScan), so their rows hold NULL placeholders in the late columns and row ids in one
// of them; the top-N keeps and orders those rows; then the late columns of the window's rows are
// read from their parts (one task per part and column, on the executor) and put in place.
struct LateColumns {
  std::shared_ptr<plan::Table> table;
  std::vector<int> slots;   // the late output columns
  std::vector<int> fields;  // the table field of each
  int row_id = -1;          // the slot that carries the row ids through the pipeline
  // The table part of each part number (the parts the pipeline reads, in order).
  std::shared_ptr<const std::vector<int64_t>> parts;
  std::shared_ptr<arrow::Schema> narrow_schema;  // of the narrow pipeline's rows
};

class PartTopNOperator final : public PartSink {
 public:
  PartTopNOperator(PartPipeline pipeline, int64_t num_parts, std::shared_ptr<arrow::Schema> schema,
                   std::vector<plan::SortKey> keys, int64_t limit, int64_t offset,
                   std::optional<LateColumns> late = std::nullopt);
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
  std::optional<LateColumns> late_;
  arrow::internal::Executor* executor_ = nullptr;
  std::unique_ptr<SortBuffer> merged_;  // the parts' first rows, until emitted
  MemoryReservation memory_;            // merged_'s own containers
  bool sorted_ = false;
  int64_t next_ = 0;  // the next sorted row to emit
  int64_t end_ = 0;   // one past the last
  // With late_: the window's rows with their late columns, emitted from next_ - offset on.
  std::shared_ptr<arrow::RecordBatch> window_;
  int64_t window_start_ = 0;
  std::unique_ptr<PartScheduler<PartBuffer>> scheduler_;

  // The window's narrow rows with their late columns read from their parts.
  arrow::Result<std::shared_ptr<arrow::RecordBatch>> Fetch(const arrow::RecordBatch& window,
                                                           const LateColumns& late);
};

}  // namespace antb1::exec
