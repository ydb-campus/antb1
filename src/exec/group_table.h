#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <arrow/compute/exec.h>
#include <arrow/memory_pool.h>
#include <arrow/record_batch.h>
#include <arrow/result.h>
#include <arrow/status.h>
#include <arrow/type_fwd.h>

#include "antb1/exec/grouped_aggregate_state.h"
#include "antb1/exec/memory_budget.h"
#include "antb1/plan/logical_plan.h"

namespace arrow::compute {
class ExecContext;
class Grouper;
}  // namespace arrow::compute

namespace antb1::exec {

// A hash of every row's key values (the columns of `keys`, normalized as a grouper keeps them),
// the same on every run and build of this Arrow version: each value's bytes through Arrow's string
// hash (a fixed-width value's bytes, a binary value's bytes, a constant for NULL), combined over
// the keys. Without columns every row has the same hash.
arrow::Result<std::vector<std::uint64_t>> KeyHashes(const arrow::compute::ExecBatch& keys);

// The groups of a GROUP BY: a grouper over the keys (NULL is a key value; DOUBLE keys are
// normalized first, so -0.0 groups with 0.0 and every NaN with every other NaN), one
// GroupedAggregateState per call, and each group's key values as first seen, in chunks of new
// groups. GroupAggregateOperator consumes its whole input into one table;
// PartGroupAggregateOperator consumes each part into its own and merges them in part order
// (docs/adr/0013-parallel-execution.md). Groups are numbered by the grouper (deterministic for a
// given input, not the order of first appearance). Without keys every row is in one group. The
// states' own containers are charged to the budget. Single-threaded.
class GroupTable {
 public:
  // The most new groups a Merge adds as one chunk (one output batch).
  static constexpr std::size_t kMaxMergeChunk = std::size_t{64} * 1024;
  // The partitions of a partitioned merge: a constant, never the number of threads, so that the
  // result is the same for any number of threads.
  static constexpr std::size_t kPartitions = 64;

  // Checks the keys and calls against an input `input_width` columns wide. Without
  // `first_keys` the table keeps no first-seen key values (NextChunk is then unavailable): its
  // groups are read through uniques(), which holds the normalized keys.
  static arrow::Result<std::unique_ptr<GroupTable>> Make(
      std::vector<plan::BoundColumn> keys, std::vector<plan::AggregateCall> aggregates,
      int input_width, arrow::MemoryPool* pool, MemoryBudget* budget, bool first_keys = true);
  GroupTable(const GroupTable&) = delete;
  GroupTable& operator=(const GroupTable&) = delete;
  GroupTable(GroupTable&&) = delete;
  GroupTable& operator=(GroupTable&&) = delete;
  ~GroupTable();

  // Adds the rows of a batch without a selection.
  arrow::Status Consume(const arrow::RecordBatch& rows);
  // Adds the groups of `part` (a table over the same keys and calls, of the rows after this
  // table's): a group new to this table keeps part's first-seen key values; states merge. Only
  // for a table that keeps first-seen keys.
  arrow::Status Merge(const GroupTable& part);

  // Splits the groups into partitions by a hash of their (normalized) keys, after the last
  // Consume of a part table: kPartitions partitions, or one without keys.
  arrow::Status Partition();
  // Partition by the hash of the first `prefix` keys, except for the groups whose prefix hash is
  // in `heavy` (sorted), which go by the hash of all keys (the two-level aggregation,
  // docs/adr/0014-two-level-aggregation.md). A pure function of each group's keys, so every part
  // puts a group in the same partition.
  arrow::Status Partition(std::size_t prefix, std::span<const std::uint64_t> heavy);
  // The hash (KeyHashes) of each group's first `prefix` keys, in group order (with keys only).
  [[nodiscard]] arrow::Result<std::vector<std::uint64_t>> PrefixHashes(std::size_t prefix) const;
  // Each group's normalized keys, in group order (with keys only).
  [[nodiscard]] arrow::Result<arrow::compute::ExecBatch> uniques() const;
  // The state of call `i`.
  [[nodiscard]] const GroupedAggregateState& state(std::size_t i) const { return *states_.at(i); }
  [[nodiscard]] std::size_t num_partitions() const { return partition_groups_.size(); }
  // After Partition: the groups of partition `partition`, in group order.
  [[nodiscard]] const std::vector<std::uint32_t>& partition_groups(std::size_t partition) const {
    return partition_groups_.at(partition);
  }
  // Adds the groups of partition `partition` of `part` (partitioned) as Merge adds all of them.
  // Tables that merge different partitions of the same parts can run on different threads.
  arrow::Status MergePartition(const GroupTable& part, std::size_t partition);

  [[nodiscard]] std::uint32_t num_groups() const { return num_groups_; }
  // The next chunk of groups as one batch (the keys as first seen, then one column per call, in
  // `schema`), or nullptr after the last. Moves the keys out: call it after the last Consume or
  // Merge.
  arrow::Result<std::shared_ptr<arrow::RecordBatch>> NextChunk(
      const std::shared_ptr<arrow::Schema>& schema);

 private:
  GroupTable(std::vector<plan::BoundColumn> keys, std::vector<plan::AggregateCall> aggregates,
             arrow::MemoryPool* pool, bool first_keys);

  // Numbers `rows` groups new from num_groups_ on (a new chunk) and gives every state room.
  arrow::Status AddGroups(std::uint32_t after, std::vector<std::shared_ptr<arrow::Array>> keys);
  // Adds the groups new since num_groups_ (up to `after`) that part's groups from[i] become
  // (to[i]), with part's first-seen keys, in chunks of at most kMaxMergeChunk groups.
  arrow::Status AddPartGroups(const GroupTable& part, std::span<const std::uint32_t> from,
                              std::span<const std::uint32_t> to, std::uint32_t after);
  arrow::Status Account();

  std::vector<plan::BoundColumn> keys_;
  std::vector<plan::AggregateCall> aggregates_;
  arrow::MemoryPool* pool_;
  bool keep_first_keys_;
  std::unique_ptr<arrow::compute::ExecContext> kernels_;
  std::unique_ptr<arrow::compute::Grouper> grouper_;  // nullptr without keys
  std::vector<std::unique_ptr<GroupedAggregateState>> states_;
  std::vector<std::vector<std::shared_ptr<arrow::Array>>> first_keys_;  // per key, per chunk
  std::vector<std::uint32_t> chunk_groups_;  // the number of new groups of each chunk
  std::uint32_t num_groups_ = 0;
  std::size_t next_chunk_ = 0;  // NextChunk: the next chunk and the group it starts at
  std::uint32_t next_group_ = 0;
  MemoryReservation memory_;  // the states' and chunk_groups_' containers
  // After Partition: each partition's groups, and their unique normalized keys.
  std::vector<std::vector<std::uint32_t>> partition_groups_;
  std::vector<arrow::compute::ExecBatch> partition_uniques_;
};

}  // namespace antb1::exec
