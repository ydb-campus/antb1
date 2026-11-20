#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>
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

#include "grouped_aggregate_program.h"
#include "key_table.h"
#include "pool_vector.h"

namespace arrow::compute {
class ExecContext;
}  // namespace arrow::compute

namespace antb1::exec {

// Private measurement control for ADR 0019. Ordinary operators use the separate ID/update path.
enum class GroupTableMode : std::uint8_t { kSeparate, kBatch };

// A hash of every row's key values (the columns of `keys`, normalized as a key table keeps them),
// the same on every run and build of this Arrow version: each value's bytes through Arrow's string
// hash (a fixed-width value's bytes, a binary value's bytes, a constant for NULL), combined over
// the keys. Without columns every row has the same hash.
arrow::Result<std::vector<std::uint64_t>> KeyHashes(const arrow::compute::ExecBatch& keys);

// Canonical routing hashes for valid row indices into normalized key arrays.
arrow::Status SelectedGroupHashes(const arrow::compute::ExecBatch& keys,
                                  std::span<const std::uint32_t> rows,
                                  std::span<std::uint64_t> hashes);

// The columns `columns` of `keys`, in that order.
arrow::compute::ExecBatch ProjectKeys(const arrow::compute::ExecBatch& keys,
                                      std::span<const std::size_t> columns);

// Rows of keys split by the hash of all their columns (KeyHashes), as GroupTable::Partition splits
// a table's groups over `partitions` (a power of two) partitions: per partition, its rows (indices
// into `keys`, ascending) and their keys, in that order.
struct KeyBuckets {
  std::vector<arrow::compute::ExecBatch> keys;
  std::vector<std::vector<std::uint32_t>> rows;
};
arrow::Result<KeyBuckets> SplitByKeys(const arrow::compute::ExecBatch& keys, std::size_t partitions,
                                      arrow::MemoryPool* pool);

// The groups of a GROUP BY: a key table over the keys (key_table.h; NULL is a key value; DOUBLE
// keys are normalized first, so -0.0 groups with 0.0 and every NaN with every other NaN), one
// GroupedAggregateState per call, and each group's key values as first seen, in chunks of new
// groups. GroupAggregateOperator consumes its whole input into one table;
// PartGroupAggregateOperator consumes each part into its own and merges them in part order
// (docs/adr/0013-parallel-execution.md). Groups are numbered by the key table (deterministic for a
// given input; arrow's Grouper does not number them in the order of first appearance). Without keys
// every row is in one group. The states' own containers are charged to the budget, and the key
// table allocates from the budget's pool. Single-threaded.
class GroupTable {
 public:
  // The most new groups a Merge adds as one chunk (one output batch).
  static constexpr std::size_t kMaxMergeChunk = std::size_t{64} * 1024;
  // The most key bytes NextChunk joins from several chunks into one batch.
  static constexpr int64_t kMaxJoinedKeyBytes = int64_t{16} * 1024 * 1024;
  // The partitions a table's groups merge through: a constant, never the number of threads, so
  // that the result is the same for any number of threads (docs/adr/0013-parallel-execution.md).
  // 32 rather than 64 because the fixed cost of a partition is paid once per part per partition --
  // a merge call, a slice of the uniques and a lane table -- and halving it is worth 5-10% on the
  // grouped queries, at the same ratio on 4 threads as on 14. A table may be made with another
  // count: the two-level aggregation keeps 64, whose inner tables grow as partitions shrink.
  static constexpr std::size_t kPartitions = 32;

  // Checks the keys and calls against an input `input_width` columns wide. Without
  // `first_keys` the table keeps no first-seen key values (NextChunk is then unavailable): its
  // groups are read through uniques(), which holds the normalized keys. `key_table` picks the key
  // table (MakeKeyTable); operators leave it to the engine.
  static arrow::Result<std::unique_ptr<GroupTable>> Make(
      std::vector<plan::BoundColumn> keys, std::vector<plan::AggregateCall> aggregates,
      int input_width, arrow::MemoryPool* pool, MemoryBudget* budget, bool first_keys = true,
      std::size_t partitions = kPartitions, const KeyTableOptions& key_table = {},
      GroupTableMode mode = GroupTableMode::kSeparate);
  // Fixed-key batch program with query-budgeted per-table metadata for matched local layouts.
  static arrow::Result<std::unique_ptr<GroupTable>> MakeLocal(
      const std::vector<plan::BoundColumn>& keys, const std::vector<plan::AggregateCall>& calls,
      int input_width, arrow::MemoryPool* pool, MemoryBudget* budget, bool first_keys = true,
      std::size_t partitions = kPartitions,
      const KeyTableOptions& options = {.kind = KeyTableKind::kFixed});
  GroupTable(const GroupTable&) = delete;
  GroupTable& operator=(const GroupTable&) = delete;
  GroupTable(GroupTable&&) = delete;
  GroupTable& operator=(GroupTable&&) = delete;
  ~GroupTable();

  // Adds the rows of a batch without a selection. With `group_ids`, each row's group is written
  // there too (a set operation's starters, docs/adr/0039-set-operations.md); without, nothing is
  // copied.
  arrow::Status Consume(const arrow::RecordBatch& rows,
                        std::vector<std::uint32_t>* group_ids = nullptr);
  // Updates a batch-program table from the selected rows of the original payload arrays.
  arrow::Status ConsumeSelected(const arrow::RecordBatch& rows,
                                const arrow::BooleanArray* selection);
  // The group of each row of a batch without a selection, null where no group has its keys: adds
  // no group and touches no state (the inputs of a set operation that only look up). DOUBLE keys
  // are normalized first, as Consume normalizes them. For a table with keys.
  arrow::Result<std::shared_ptr<arrow::UInt32Array>> Lookup(const arrow::RecordBatch& rows);
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
  // The rows of `rows` (a batch of this table's input) split as Partition splits their groups:
  // per partition, the rows whose keys' group Partition() puts there, in input order.
  // For a table with keys; DOUBLE keys are normalized for the hash, as the key table sees them.
  arrow::Result<std::vector<std::shared_ptr<arrow::RecordBatch>>> RouteRows(
      const arrow::RecordBatch& rows) const;
  // The hash (KeyHashes) of each group's first `prefix` keys, in group order (with keys only).
  [[nodiscard]] arrow::Result<std::vector<std::uint64_t>> PrefixHashes(std::size_t prefix) const;
  // Each group's normalized keys, in group order (with keys only).
  [[nodiscard]] arrow::Result<KeySnapshot> uniques() const;
  // The state of call `i`.
  [[nodiscard]] const GroupedAggregateState& state(std::size_t i) const { return *states_.at(i); }
  [[nodiscard]] bool uses_batch_program() const { return program_.has_value(); }
  [[nodiscard]] bool supports_local_dispatch() const {
    return program_ && program_->supports_local_dispatch();
  }
  [[nodiscard]] std::size_t num_partitions() const;
  // After Partition: the groups of partition `partition`, in group order.
  [[nodiscard]] std::span<const std::uint32_t> partition_groups(std::size_t partition) const;
  // Adds the groups of partition `partition` of `part` (partitioned) as Merge adds all of them.
  // Tables that merge different partitions of the same parts can run on different threads: they
  // read part only through its partitions' groups and keys, its first-seen keys and its states,
  // either from the immutable stored-key handoff or from the partitions materialized on the
  // producer. A lane never calls GetUniques on a shared Arrow Grouper.
  arrow::Status MergePartition(const GroupTable& part, std::size_t partition);

  // Folds groups `from` of `source` -- a table with the same calls over keys that include this
  // table's -- into the groups of their keys here, many source groups to one (the second level of
  // grouping sets, docs/adr/0038-grouping-sets.md). Row i of `keys` is the keys of group from[i],
  // projected to this table's (ProjectKeys of source's uniques(), possibly split by SplitByKeys).
  // A new group's keys are the normalized keys of the first source group that makes it: the key
  // values themselves for every type but DOUBLE, so never project a DOUBLE key. Only after
  // source's last Consume or Merge; source is read, not changed, so tables that merge disjoint
  // groups of one source can run on different threads.
  arrow::Status MergeProjected(const GroupTable& source, const arrow::compute::ExecBatch& keys,
                               std::span<const std::uint32_t> from);
  // Adds the one group of a table without keys if it has none: the empty grouping set has a row
  // even over no input, as a global aggregation does.
  arrow::Status EnsureGroup();

  [[nodiscard]] std::uint32_t num_groups() const { return num_groups_; }
  // OK when NextChunk would finalize every group without an error; otherwise the first call's error
  // (GroupedAggregateState::CheckFinalize).
  [[nodiscard]] arrow::Status CheckFinalize() const;
  // The next groups as one batch (the keys as first seen, then one column per call, in `schema`),
  // or nullptr after the last: a chunk of new groups, joined with the chunks after it up to
  // kMaxMergeChunk groups and kMaxJoinedKeyBytes of keys. Moves the keys out: call it after the
  // last Consume or Merge.
  arrow::Result<std::shared_ptr<arrow::RecordBatch>> NextChunk(
      const std::shared_ptr<arrow::Schema>& schema);

 private:
  friend class PartitionedGroupTables;
  friend class CardinalityLocalGroups;
  arrow::Result<arrow::compute::ExecBatch> NormalizedKeys(const arrow::RecordBatch& rows) const;
  GroupTable(std::vector<plan::BoundColumn> keys, std::vector<plan::AggregateCall> aggregates,
             arrow::MemoryPool* pool, bool first_keys);

  // Numbers `rows` groups new from num_groups_ on (a new chunk) and gives every state room.
  arrow::Status AddGroups(std::uint32_t after, std::vector<std::shared_ptr<arrow::Array>> keys);
  // Adds the groups new since num_groups_ (up to `after`) that part's groups from[i] become
  // (to[i]), with part's first-seen keys, in chunks of at most kMaxMergeChunk groups.
  arrow::Status AddPartGroups(const GroupTable& part, std::span<const std::uint32_t> from,
                              std::span<const std::uint32_t> to, std::uint32_t after);
  arrow::Status AddPartKeyRows(const GroupTable& part, std::span<const std::uint32_t> source);
  arrow::Status ResizeStates(std::uint32_t after);
  // The same groups, from the batch of part's keys this table has just consumed (row i the keys of
  // the part group that became to[i]) rather than from part's first_keys_: what a part that keeps
  // no first-seen keys has handed over already. One Take a column a chunk, where AddPartGroups
  // needs two and a Concatenate to gather the same values out of part's chunks.
  arrow::Status AddUniqueGroups(const arrow::compute::ExecBatch& keys,
                                std::span<const std::uint32_t> to, std::uint32_t after);
  // Adds one new group per position, in order, with the keys of row positions[i] of `keys`, in
  // chunks of at most kMaxMergeChunk groups.
  arrow::Status AddKeyRows(const arrow::compute::ExecBatch& keys,
                           std::span<const std::uint32_t> positions);
  arrow::Status Account();
  // Step 3 is available only for a fixed stored-key layout and a supported aggregate program.
  // A failed chunk can leave earlier chunks applied. A scheduler retry uses a fresh table.
  template <class Body>
  arrow::Status Update(const Body& body) {
    if (!program_) {
      return body();
    }
    ARROW_RETURN_NOT_OK(program_error_);
    if (frozen_) {
      return arrow::Status::Invalid("update of a frozen group table");
    }
    program_error_ = KeyTableNoBadAlloc(body);
    return program_error_;
  }
  arrow::Status MergeTable(const GroupTable& part);
  arrow::Status MergePartitionGroups(const GroupTable& part, std::size_t partition);
  arrow::Status MergeProjectedGroups(const GroupTable& source,
                                     const arrow::compute::ExecBatch& keys,
                                     std::span<const std::uint32_t> from);
  arrow::Status ConsumeRows(const arrow::RecordBatch& rows, std::vector<std::uint32_t>* group_ids);
  arrow::Status ConsumeProgram(const arrow::RecordBatch& rows,
                               const arrow::compute::ExecBatch& keys,
                               std::span<const std::uint32_t> selection = {});
  arrow::Status MergeStoredChunks(const GroupTable& source,
                                  std::optional<std::span<const std::uint32_t>> selection);
  arrow::Status MergeStoredGroups(const GroupTable& source, std::span<const std::uint32_t> from);
  arrow::Status FreezePartitions();
  std::uint32_t OutputRows(std::size_t& end_chunk) const;
  arrow::Status AppendFirstKeyColumns(std::size_t end_chunk, arrow::ArrayVector& columns);
  arrow::Result<std::shared_ptr<arrow::RecordBatch>> NextOutputChunk(
      const std::shared_ptr<arrow::Schema>& schema);
  const StoredKeyTable* StoredKeys() const;
  [[nodiscard]] bool KeepsKeyChunks() const { return keep_first_keys_ && !stored_output_; }
  // The partition of a key hash. The count is a power of two (checked in Make), so this masks
  // rather than divides: RouteRows runs it once a row, where a constant count let the compiler do
  // the same thing and a runtime one would not.
  [[nodiscard]] std::size_t PartitionOf(std::uint64_t hash) const {
    return static_cast<std::size_t>(hash) & (partitions_ - 1);
  }

  std::optional<MemoryReservation> local_metadata_;  // owned by a dispatched lane through handoff
  MemoryReservation memory_;  // outlives the state and key metadata containers it charges
  std::vector<plan::BoundColumn> keys_;
  std::vector<plan::AggregateCall> aggregates_;
  arrow::MemoryPool* pool_;
  bool keep_first_keys_;
  std::size_t partitions_ = kPartitions;  // this table's partition count
  std::unique_ptr<arrow::compute::ExecContext> kernels_;
  std::unique_ptr<KeyTable> key_table_;  // nullptr without keys
  std::vector<std::unique_ptr<GroupedAggregateState>> states_;
  std::optional<GroupedAggregateProgram> program_;
  // A failed streaming update may already have applied earlier chunks; only a fresh part can retry.
  arrow::Status program_error_;
  StoredKeyTable* stored_keys_ = nullptr;  // mutable until FreezePartitions hands over key_table_
  bool stored_output_ = false;             // first-seen keys equal the stored representation
  std::vector<std::vector<std::shared_ptr<arrow::Array>>> first_keys_;  // per key, per chunk
  std::vector<std::uint32_t> chunk_groups_;  // the number of new groups of each chunk
  std::uint32_t num_groups_ = 0;
  std::size_t next_chunk_ = 0;  // NextChunk: the next chunk and the group it starts at
  std::uint32_t next_group_ = 0;
  // After Partition: each partition's groups, and their unique normalized keys.
  std::vector<std::vector<std::uint32_t>> partition_groups_;
  std::vector<arrow::compute::ExecBatch> partition_uniques_;
  using StoredPartition = std::vector<std::uint32_t, arrow::stl::allocator<std::uint32_t>>;
  struct FrozenGroups {
    std::unique_ptr<const KeyTable> owner;
    const StoredKeyTable* keys;
    std::vector<StoredPartition, arrow::stl::allocator<StoredPartition>> partitions;
  };
  std::optional<FrozenGroups> frozen_;
};

// A worker's independently owned local tables, populated by stable lane dispatch. Finish releases
// the dispatch scratch. Each destination can then take its own immutable partition concurrently.
class PartitionedGroupTables {
 public:
  static arrow::Result<std::unique_ptr<PartitionedGroupTables>> Make(
      const std::vector<plan::BoundColumn>& keys, const std::vector<plan::AggregateCall>& calls,
      int input_width, arrow::MemoryPool* pool, MemoryBudget* budget, bool first_keys = true,
      std::size_t partitions = GroupTable::kPartitions,
      const KeyTableOptions& options = {.kind = KeyTableKind::kFixed});
  arrow::Status Consume(const arrow::RecordBatch& rows,
                        const arrow::BooleanArray* selection = nullptr,
                        std::chrono::nanoseconds* dispatch_elapsed = nullptr);
  [[nodiscard]] std::uint64_t num_groups() const { return groups_; }
  arrow::Status Finish();
  // After Finish. Calls on different lanes are safe concurrently.
  std::unique_ptr<const GroupTable> ReleasePartition(std::size_t lane);

 private:
  friend class CardinalityLocalGroups;
  arrow::Status CreateLane(std::size_t lane);
  // The frozen source's groups are disjoint and these destination lanes are empty.
  arrow::Status Seed(const GroupTable& source);
  PartitionedGroupTables(int input_width, arrow::MemoryPool* pool, const KeyTableOptions& options,
                         MemoryReservation metadata)
      : metadata_(std::move(metadata)),
        input_width_(input_width),
        options_(options),
        lanes_(arrow::stl::allocator<std::unique_ptr<GroupTable>>(pool)),
        rows_(pool),
        hashes_(pool) {
    options_.table_metadata = nullptr;  // a lane constructs with its own metadata reservation
  }
  arrow::Status Dispatch(const arrow::RecordBatch& rows, const arrow::BooleanArray* selection,
                         std::chrono::nanoseconds* dispatch_elapsed);
  MemoryReservation metadata_;
  int input_width_;
  KeyTableOptions options_;
  std::vector<std::unique_ptr<GroupTable>, arrow::stl::allocator<std::unique_ptr<GroupTable>>>
      lanes_;
  std::unique_ptr<GroupTable> spare_;  // first populated lane reuses the eligibility table
  GroupTable* prototype_ = nullptr;    // borrowed only by the producer, cleared by Finish
  PoolVector<std::uint32_t> rows_;     // selected indices followed by the stable lane order
  PoolVector<std::uint64_t> hashes_;
  std::uint64_t groups_ = 0;
  arrow::Status error_;
};

// Private observation threshold for the single-to-partitioned local layout experiment.
inline constexpr std::uint32_t kLocalPromotionGroups = 32U * 1024;

// Start one fixed local table and observe its actual distinct groups. A later nonempty input
// tile may promote it: move the accumulated state of each group to its owning lane before
// consuming new rows. Finish returns the same handoff as the selected local layout.
class CardinalityLocalGroups {
 public:
  static arrow::Result<std::unique_ptr<CardinalityLocalGroups>> Make(
      const std::vector<plan::BoundColumn>& keys, const std::vector<plan::AggregateCall>& calls,
      int input_width, arrow::MemoryPool* pool, MemoryBudget* budget, bool first_keys,
      std::size_t partitions, const KeyTableOptions& options, std::uint32_t threshold);
  arrow::Status Consume(const arrow::RecordBatch& rows,
                        const arrow::BooleanArray* selection = nullptr,
                        std::chrono::nanoseconds* dispatch_elapsed = nullptr,
                        std::chrono::nanoseconds* promotion_elapsed = nullptr);
  arrow::Status Finish(std::unique_ptr<GroupTable>& single,
                       std::unique_ptr<PartitionedGroupTables>& partitioned);
  [[nodiscard]] bool promoted() const { return promoted_groups_ != 0; }
  [[nodiscard]] std::uint32_t promoted_groups() const { return promoted_groups_; }
  [[nodiscard]] std::uint64_t num_groups() const;

 private:
  CardinalityLocalGroups(MemoryReservation metadata, int input_width, KeyTableOptions options,
                         std::uint32_t threshold, bool can_promote)
      : metadata_(std::move(metadata)),
        input_width_(input_width),
        options_(options),
        threshold_(threshold),
        can_promote_(can_promote) {
    options_.table_metadata = nullptr;
  }
  arrow::Status Promote();
  MemoryReservation metadata_;
  int input_width_;
  KeyTableOptions options_;
  std::uint32_t threshold_;
  bool can_promote_;
  std::uint32_t promoted_groups_ = 0;
  std::uint64_t finished_groups_ = 0;
  std::unique_ptr<GroupTable> single_;
  std::unique_ptr<PartitionedGroupTables> partitioned_;
  arrow::Status error_;
};

}  // namespace antb1::exec
