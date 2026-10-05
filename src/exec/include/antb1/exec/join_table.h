#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <arrow/array/array_primitive.h>
#include <arrow/memory_pool.h>
#include <arrow/record_batch.h>
#include <arrow/result.h>
#include <arrow/status.h>
#include <arrow/type_fwd.h>

#include "antb1/exec/memory_budget.h"
#include "antb1/exec/operator.h"
#include "antb1/plan/logical_plan.h"

// The hash table of a join's build side (docs/adr/0022-joins-and-query-blocks.md, "The join hash
// table"): built from the parts of the build input, on any number of threads, into the same table,
// then read by any number of probe threads without locks.
//
// A build runs in three steps. Each part of the input is appended to its own JoinBuildPart, on any
// thread (a part task): the part keeps its selected rows whose keys are not NULL, split into
// kJoinPartitions partitions by the hash of their keys (KeyHashes modulo kJoinPartitions, the
// GROUP BY rule), with one Take per column. The consumer thread adds the parts to a
// JoinTableBuilder in any order; the builder hands them to the partitions in part order
// (PartitionLanes), so every partition lists its rows in (part, row) order. Finish then builds the
// partitions' tables in parallel. Every decision depends on the data only, never on the number of
// threads or the order in which parts arrive: the finished table is the same.
//
// Layouts. The rows of a key are always contiguous in rows() and in (part, row) order.
// - Direct, for one SMALLINT, INTEGER, BIGINT, USMALLINT, DATE or TIMESTAMP key whose values span
//   fewer than 8 times the rows (max - min < 8 * num_rows()): an array indexed by the key minus the
//   smallest key gives each key's rows.
// - Hashed otherwise: per partition, bit_ceil(rows) buckets on the hash bits above the partition's,
//   each listing the partition's distinct keys that fall in it (first seen first) with their 64-bit
//   hash and the range of their rows.
//
// Rows are kept as the parts appended them (the chunks), without a copy, and referenced as (chunk,
// row); a build of more than 4294967295 rows is OutOfMemory. NULL keys are never inserted, so they
// never match; the table counts them for the null-aware anti join.
//
// Errors: what a correct physical plan never sends (a key of a wrong type, a part added twice) is
// Invalid; passing the memory limit, or 4294967295 rows, is OutOfMemory, and so is std::bad_alloc.
// Memory: every container of a build is charged to the budget before it is allocated
// (MemoryReservation), and the build's Arrow buffers come from the pool given to Append; a finished
// table keeps its memory until it is destroyed. The budget must outlive the parts, the builder and
// the table. A probe (Find) is not charged for its hashes, as GROUP BY's routing is not.

namespace antb1::exec {

class PartitionLanes;

// The partitions of a build: a constant (GroupTable::kPartitions), never the number of threads.
inline constexpr std::size_t kJoinPartitions = 64;

// A build row: row `row` of JoinTable::chunks()[chunk].
struct JoinRowRef {
  std::uint32_t chunk = 0;
  std::uint32_t row = 0;
};

// The build rows a probe row matches: JoinTable::rows()[begin, end), empty ({0, 0}) for none.
struct JoinMatches {
  std::uint32_t begin = 0;
  std::uint32_t end = 0;
};

// The keys of a build: columns of its input's schema, which the planner has cast to one type on
// both sides of the join. Checked once and shared by every part of the build. Immutable.
class JoinBuildSpec {
 public:
  // Invalid (a planner bug, never NotImplemented) without keys, for a key outside the schema, for a
  // DOUBLE or BOOLEAN key, or for a key whose column is not of its type (plan::ToArrow).
  static arrow::Result<std::shared_ptr<const JoinBuildSpec>> Make(
      std::shared_ptr<arrow::Schema> schema, std::vector<plan::BoundColumn> keys);

  [[nodiscard]] const std::shared_ptr<arrow::Schema>& schema() const { return schema_; }
  [[nodiscard]] const std::vector<plan::BoundColumn>& keys() const { return keys_; }
  // One key of a type the direct layout indexes: SMALLINT, INTEGER, BIGINT, USMALLINT, DATE or
  // TIMESTAMP.
  [[nodiscard]] bool direct_candidate() const { return direct_candidate_; }

 private:
  JoinBuildSpec(std::shared_ptr<arrow::Schema> schema, std::vector<plan::BoundColumn> keys,
                bool direct_candidate);

  std::shared_ptr<arrow::Schema> schema_;
  std::vector<plan::BoundColumn> keys_;
  bool direct_candidate_ = false;
};

// One part of a build's input, partitioned as it is appended. Single-threaded: a part task appends
// to it on any thread, then hands it to the consumer (JoinTableBuilder::Add), which only reads it.
class JoinBuildPart {
 public:
  // `budget` (nullptr: none) is charged for the part's own containers.
  JoinBuildPart(std::shared_ptr<const JoinBuildSpec> spec, MemoryBudget* budget);
  JoinBuildPart(const JoinBuildPart&) = delete;
  JoinBuildPart& operator=(const JoinBuildPart&) = delete;
  JoinBuildPart(JoinBuildPart&&) = delete;
  JoinBuildPart& operator=(JoinBuildPart&&) = delete;
  ~JoinBuildPart();

  // Keeps the selected rows of `batch` whose keys are not NULL, every column, partitioned (Arrow
  // buffers from `pool`). Invalid for an end batch, another schema or a selection of another
  // length. On failure the part is unchanged.
  arrow::Status Append(const Batch& batch, arrow::MemoryPool* pool);

  [[nodiscard]] const std::shared_ptr<const JoinBuildSpec>& spec() const { return spec_; }
  // The selected rows appended, NULL keys included.
  [[nodiscard]] int64_t input_rows() const { return input_rows_; }
  // The selected rows with a NULL in some key (not kept).
  [[nodiscard]] int64_t null_key_rows() const { return null_key_rows_; }
  // The rows kept.
  [[nodiscard]] int64_t num_rows() const { return num_rows_; }

 private:
  friend class JoinTableBuilder;
  friend class JoinTable;

  // The kept rows of one appended batch, in partition order.
  struct Piece {
    std::shared_ptr<arrow::RecordBatch> rows;  // every column
    std::vector<std::uint64_t> hashes;         // of each row
    // Partition p holds rows [start[p], start[p + 1]).
    std::array<std::uint32_t, kJoinPartitions + 1> start{};
  };

  arrow::Status AppendRows(const Batch& batch, arrow::MemoryPool* pool);
  // The bytes of pieces_ (at `capacity`) and of the pieces' hashes.
  [[nodiscard]] int64_t MemoryUsage(std::size_t capacity) const;

  std::shared_ptr<const JoinBuildSpec> spec_;
  MemoryBudget* budget_;
  std::vector<Piece> pieces_;
  int64_t input_rows_ = 0;
  int64_t null_key_rows_ = 0;
  int64_t num_rows_ = 0;
  int64_t min_key_ = 0;  // a direct candidate with rows: its smallest and largest key
  int64_t max_key_ = 0;
  int64_t hash_bytes_ = 0;    // the capacity of the pieces' hashes, in bytes
  MemoryReservation memory_;  // pieces_ and the hashes
};

class JoinTableBuilder;

// A finished build, immutable: Find and every accessor may be called from any number of threads at
// once.
class JoinTable {
 public:
  enum class Layout : std::uint8_t { kDirect, kHashed };

  JoinTable(const JoinTable&) = delete;
  JoinTable& operator=(const JoinTable&) = delete;
  JoinTable(JoinTable&&) = delete;
  JoinTable& operator=(JoinTable&&) = delete;
  ~JoinTable();

  // The matches of every probe row: out[i] for row i of `keys` (one array per build key, of its
  // type, all of one length), the build rows whose keys equal row i's, in (part, row) order. A row
  // that is not selected (`selection`: nullptr for every row), has a NULL key or matches nothing
  // gets an empty range ({0, 0}). Probe buffers come from `pool` (a NULL bitmap and, when at most a
  // quarter of the rows are kept, their positions and a copy of their keys), except the hashed
  // layout's hashes: a temporary std::vector that no budget sees. Invalid for keys of another
  // count, type or length, a selection of another length or a short `out`.
  arrow::Status Find(std::span<const std::shared_ptr<arrow::Array>> keys,
                     const arrow::BooleanArray* selection, arrow::MemoryPool* pool,
                     std::span<JoinMatches> out) const;

  // The inserted rows, each key's rows together and in (part, row) order.
  [[nodiscard]] std::span<const JoinRowRef> rows() const { return rows_; }
  // The parts' rows (every column of the build input) that JoinRowRef::chunk indexes.
  [[nodiscard]] const std::vector<std::shared_ptr<arrow::RecordBatch>>& chunks() const {
    return chunks_;
  }
  [[nodiscard]] const JoinBuildSpec& spec() const { return *spec_; }
  [[nodiscard]] Layout layout() const { return layout_; }
  // The rows inserted (rows().size()).
  [[nodiscard]] int64_t num_rows() const { return static_cast<int64_t>(rows_.size()); }
  // The selected rows of the build input, NULL keys included.
  [[nodiscard]] int64_t input_rows() const { return input_rows_; }
  // The selected rows with a NULL in some key (never inserted).
  [[nodiscard]] bool has_null() const { return null_key_rows_ > 0; }
  [[nodiscard]] int64_t null_key_rows() const { return null_key_rows_; }
  // No key value repeats among the inserted rows (true when none is inserted).
  [[nodiscard]] bool unique() const { return unique_; }
  // The build input had no selected row.
  [[nodiscard]] bool empty() const { return input_rows_ == 0; }

 private:
  friend class JoinTableBuilder;

  // One key column of a batch, addressed by the batch's row: a fixed-width value is `width` bytes
  // at values + row * width; a VARCHAR value is values[offsets[row], offsets[row + 1]).
  struct KeyView {
    const std::uint8_t* values = nullptr;   // fixed width: row 0's value; VARCHAR: the data buffer
    const std::int32_t* offsets = nullptr;  // VARCHAR only: row 0's offset
  };
  // A distinct key of a partition: its hash and its rows, rows()[begin, end).
  struct KeySlot {
    std::uint64_t hash = 0;
    std::uint32_t begin = 0;
    std::uint32_t end = 0;
  };
  // A run of a part's rows in one partition: rows [begin, end) of chunks_[chunk], whose hashes
  // start at `hashes` (in the part, which outlives the run).
  struct Segment {
    const std::uint64_t* hashes = nullptr;
    std::uint32_t chunk = 0;
    std::uint32_t begin = 0;
    std::uint32_t end = 0;
  };
  // Per partition, its runs in part order.
  using Segments = std::array<std::vector<Segment>, kJoinPartitions>;
  // A partition of the hashed layout: bucket b lists slots[buckets[b], buckets[b + 1]).
  struct Directory {
    std::uint64_t mask = 0;  // the number of buckets - 1
    std::vector<std::uint32_t> buckets;
    std::vector<KeySlot> slots;
    MemoryReservation memory;  // buckets and slots
  };

  JoinTable(std::shared_ptr<const JoinBuildSpec> spec, MemoryBudget* budget);

  // Builds the table from the parts (in part order) and each partition's runs of rows.
  arrow::Status Build(std::span<const std::shared_ptr<const JoinBuildPart>> parts,
                      const Segments& segments, int64_t min_key, int64_t max_key,
                      arrow::internal::Executor* executor);
  arrow::Status BuildDirect(const Segments& segments, arrow::internal::Executor* executor);
  arrow::Status BuildHashed(const Segments& segments, arrow::internal::Executor* executor);
  // The directory of partition `partition` from its runs, its rows going to rows_[first, ...);
  // sets `repeats` when a key has several rows.
  arrow::Status BuildDirectory(std::size_t partition, std::span<const Segment> segments,
                               std::uint32_t first, bool& repeats);
  // Puts each key's entries of a bucket (rows_[first, first + hashes.size()), with their hashes)
  // together, the keys in the order they first appear, every key's rows in their order; the
  // number of keys. `heads` and `scratch` are the caller's, reused from bucket to bucket.
  arrow::Result<std::size_t> GroupBucket(std::uint32_t first, std::span<std::uint64_t> hashes,
                                         std::vector<std::uint32_t>& heads,
                                         MemoryReservation& scratch);
  // The view of a key column of `width` bytes (0: VARCHAR), whatever its offset.
  static KeyView ViewOf(const arrow::ArrayData& data, int width);
  [[nodiscard]] arrow::Status DoFind(std::span<const std::shared_ptr<arrow::Array>> keys,
                                     const arrow::BooleanArray* selection, arrow::MemoryPool* pool,
                                     std::span<JoinMatches> out) const;
  // The views of chunk `chunk`'s keys.
  [[nodiscard]] std::span<const KeyView> ChunkKeys(std::uint32_t chunk) const;
  // Whether the keys of row `row` of `keys` (one view per key) equal those of build row `ref`.
  [[nodiscard]] bool KeysEqual(std::span<const KeyView> keys, int64_t row, JoinRowRef ref) const;
  [[nodiscard]] int64_t MemoryUsage() const;

  std::shared_ptr<const JoinBuildSpec> spec_;
  MemoryBudget* budget_;
  std::vector<int> widths_;  // per key: the bytes of a value, 0 for VARCHAR
  std::vector<std::shared_ptr<arrow::RecordBatch>> chunks_;
  std::vector<KeyView> chunk_keys_;  // chunk c's keys at [c * keys, (c + 1) * keys)
  std::vector<JoinRowRef> rows_;
  Layout layout_ = Layout::kHashed;
  int64_t input_rows_ = 0;
  int64_t null_key_rows_ = 0;
  bool unique_ = true;
  // Direct: key min_key_ + i has the rows rows_[offsets_[i], offsets_[i + 1]), for i <= span_.
  int64_t min_key_ = 0;
  std::uint64_t span_ = 0;
  std::vector<std::uint32_t> offsets_;
  // Hashed: one directory per partition (the partition is the hash modulo kJoinPartitions).
  std::array<Directory, kJoinPartitions> directories_;
  MemoryReservation memory_;  // widths_, chunks_, chunk_keys_, rows_ and offsets_
};

// Assembles the parts of a build into a JoinTable. Consumer thread only (it waits for the
// executor's tasks); the parts' merges run on the executor.
class JoinTableBuilder {
 public:
  // A build of `num_parts` parts of `spec`. The executor (nullptr: the calling thread) merges the
  // parts into the partitions and builds them; at most `max_pending` parts wait to be merged (one
  // under memory pressure). Invalid without a spec or for a negative number of parts.
  static arrow::Result<std::unique_ptr<JoinTableBuilder>> Make(
      std::shared_ptr<const JoinBuildSpec> spec, int64_t num_parts,
      arrow::internal::Executor* executor, int64_t max_pending, MemoryBudget* budget);
  JoinTableBuilder(const JoinTableBuilder&) = delete;
  JoinTableBuilder& operator=(const JoinTableBuilder&) = delete;
  JoinTableBuilder(JoinTableBuilder&&) = delete;
  JoinTableBuilder& operator=(JoinTableBuilder&&) = delete;
  // Waits for the merges still running.
  ~JoinTableBuilder();

  // Part `part` (0 <= part < num_parts), appended to the end; parts may come in any order. Invalid
  // for a part outside the build, of another spec or added twice, and after Finish. Once a merge
  // or a release failed, that failure.
  arrow::Status Add(int64_t part, std::shared_ptr<const JoinBuildPart> rows);
  // Waits for the merges of the parts added so far: the failure of the smallest (part, partition).
  arrow::Status Merged();
  // The table, once every part is added: a merge failure first, then Invalid for a missing part or
  // a second call; OutOfMemory past the budget.
  arrow::Result<std::shared_ptr<const JoinTable>> Finish();

 private:
  JoinTableBuilder(std::shared_ptr<const JoinBuildSpec> spec, int64_t num_parts,
                   arrow::internal::Executor* executor, MemoryBudget* budget);

  // Hands part `part` (the next in order) to the partitions.
  arrow::Status Release(int64_t part);
  // Lane `partition`: the part's rows there, its chunks numbered from `first_chunk`.
  arrow::Status Merge(std::size_t partition, const JoinBuildPart& part, std::uint32_t first_chunk);

  std::shared_ptr<const JoinBuildSpec> spec_;
  int64_t num_parts_;
  arrow::internal::Executor* executor_;
  MemoryBudget* budget_;
  std::vector<std::shared_ptr<const JoinBuildPart>> parts_;  // by part; held until Finish
  int64_t next_part_ = 0;                                    // the next part to release
  std::uint64_t num_chunks_ = 0;                             // of the released parts
  std::uint64_t num_rows_ = 0;
  int64_t input_rows_ = 0;
  int64_t null_key_rows_ = 0;
  int64_t min_key_ = 0;  // over the released parts with rows (a direct candidate)
  int64_t max_key_ = 0;
  bool finished_ = false;
  arrow::Status failed_;      // a failed release
  MemoryReservation memory_;  // parts_
  // Per partition, its runs of rows in part order; each partition is written by its lane only.
  JoinTable::Segments segments_;
  std::array<MemoryReservation, kJoinPartitions> segment_memory_;
  std::unique_ptr<PartitionLanes> lanes_;  // last: destroyed first, it waits for the merges
};

}  // namespace antb1::exec
