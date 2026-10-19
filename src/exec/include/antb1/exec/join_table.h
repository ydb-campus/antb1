#pragma once

#include <array>
#include <atomic>
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

#include "antb1/exec/key_filter.h"
#include "antb1/exec/memory_budget.h"
#include "antb1/exec/operator.h"
#include "antb1/plan/logical_plan.h"

// The hash table of a join's build side (docs/adr/0022-joins-and-query-blocks.md, "The join hash
// table"): built from the parts of the build input, on any number of threads, into the same table,
// then read by any number of probe threads without locks.
//
// A build runs in three steps. Each part of the input is appended to its own JoinBuildPart, on any
// thread (a part task): the part indexes its selected rows whose keys are not NULL, split into
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
// never match; the table counts them for the null-aware anti join, and with
// JoinBuildSpec::keep_null_keys it also retains their rows in null_chunks(), apart from everything
// a JoinRowRef addresses, for the join that must emit the build rows which did not match.
//
// A keyless build (JoinBuildSpec::Keyless, a one-row join's) holds every selected row as rows of
// one key: one hash, one partition, one slot, so unique() means at most one row. It has nothing to
// probe: its Find is Invalid.
//
// Errors: what a correct physical plan never sends (a key of a wrong type, a part added twice) is
// Invalid; passing the memory limit, or 4294967295 rows, is OutOfMemory, and so is std::bad_alloc.
// Memory: every container of a build is charged to the budget before it is allocated
// (MemoryReservation), and the build's Arrow buffers come from the pool given to Append; a finished
// table keeps its memory until it is destroyed. The budget must outlive the parts, the builder and
// the table. A probe (Find) is not charged for its hashes, as GROUP BY's routing is not.

namespace antb1::exec {

class PartitionLanes;

// The partitions of a build: a constant of its own, never the number of threads. Independent of
// GroupTable::kPartitions: a join partitions build rows, a GROUP BY its groups.
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
  // Invalid (a planner bug, never NotImplemented) without a schema or keys, for a key outside the
  // schema, for a DOUBLE or BOOLEAN key, or for a key whose column is not of its type
  // (plan::ToArrow).
  // With `keep_null_keys`, the build also retains the selected rows whose keys are NULL, apart from
  // the table it indexes (JoinTable::null_chunks): a join that emits the build rows which did not
  // match needs them, since a NULL key never matches (ADR 0026's C2c). Every other join leaves them
  // behind, so it costs nothing unless it is asked for.
  static arrow::Result<std::shared_ptr<const JoinBuildSpec>> Make(
      std::shared_ptr<arrow::Schema> schema, std::vector<plan::BoundColumn> keys,
      bool keep_null_keys = false);
  // The spec of a build without keys, a one-row join's. Invalid without a schema.
  static arrow::Result<std::shared_ptr<const JoinBuildSpec>> Keyless(
      std::shared_ptr<arrow::Schema> schema);

  [[nodiscard]] const std::shared_ptr<arrow::Schema>& schema() const { return schema_; }
  // Empty for a keyless spec.
  [[nodiscard]] const std::vector<plan::BoundColumn>& keys() const { return keys_; }
  // One key of a type the direct layout indexes: SMALLINT, INTEGER, BIGINT, USMALLINT, DATE or
  // TIMESTAMP.
  [[nodiscard]] bool direct_candidate() const { return direct_candidate_; }
  // Whether the build retains the rows whose keys are NULL (JoinTable::null_chunks).
  [[nodiscard]] bool keep_null_keys() const { return keep_null_keys_; }

 private:
  JoinBuildSpec(std::shared_ptr<arrow::Schema> schema, std::vector<plan::BoundColumn> keys,
                bool direct_candidate, bool keep_null_keys);

  std::shared_ptr<arrow::Schema> schema_;
  std::vector<plan::BoundColumn> keys_;
  bool direct_candidate_ = false;
  bool keep_null_keys_ = false;
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
  // buffers from `pool`); with JoinBuildSpec::keep_null_keys, the selected rows whose keys are NULL
  // are kept too, in their own batches that no partition indexes. Invalid for an end batch, another
  // schema or a selection of another length. On failure the part is unchanged.
  arrow::Status Append(const Batch& batch, arrow::MemoryPool* pool);

  [[nodiscard]] const std::shared_ptr<const JoinBuildSpec>& spec() const { return spec_; }
  // The selected rows appended, NULL keys included.
  [[nodiscard]] int64_t input_rows() const { return input_rows_; }
  // The selected rows with a NULL in some key (never indexed; retained only with
  // JoinBuildSpec::keep_null_keys).
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
  // The rows of `batch` that are selected and have a NULL key, appended to null_pieces_; nothing
  // when there is none. `remaining` is how many there are (the selected rows less the kept ones),
  // which is known before the rows are found.
  arrow::Status AppendNullKeys(const Batch& batch, int64_t remaining, arrow::MemoryPool* pool);
  // The bytes of pieces_ and null_pieces_ (each at its own capacity) and of the pieces' hashes.
  // One function, taking both, because MemoryReservation::Resize is absolute: a term added at one
  // call site would be dropped by the next Append.
  [[nodiscard]] int64_t MemoryUsage(std::size_t capacity, std::size_t null_capacity) const;

  std::shared_ptr<const JoinBuildSpec> spec_;
  MemoryBudget* budget_;
  std::vector<Piece> pieces_;
  // The retained rows whose keys are NULL, in the order they were appended; empty unless the spec
  // asks for them. They are batches rather than Pieces: nothing hashes, partitions or indexes them,
  // and the tail that emits them slices them whole.
  std::vector<std::shared_ptr<arrow::RecordBatch>> null_pieces_;
  int64_t input_rows_ = 0;
  int64_t null_key_rows_ = 0;
  int64_t num_rows_ = 0;
  int64_t min_key_ = 0;  // a direct candidate with rows: its smallest and largest key
  int64_t max_key_ = 0;
  int64_t hash_bytes_ = 0;    // the capacity of the pieces' hashes, in bytes
  MemoryReservation memory_;  // pieces_, null_pieces_ and the hashes
};

class JoinTableBuilder;

// Which build rows of a JoinTable have matched a probe row: one bit per rows() position, which is
// the position JoinMatches already carries, so marking needs no index of its own. A join that
// builds on the side it preserves (docs/adr/0026-join-performance.md, C2b) allocates one, every
// probe thread marks into it, and the join reads it once its probe pipeline is done; no other join
// allocates one, so a join that does not need marks pays nothing.
class JoinMarks {
 public:
  JoinMarks(const JoinMarks&) = delete;
  JoinMarks& operator=(const JoinMarks&) = delete;
  JoinMarks(JoinMarks&&) = delete;
  JoinMarks& operator=(JoinMarks&&) = delete;
  ~JoinMarks() = default;

  // `rows` bits, every one clear, charged to `budget` (nullptr: no budget). OutOfMemory past the
  // limit. `rows` must not be negative.
  static arrow::Result<std::unique_ptr<JoinMarks>> Make(int64_t rows, MemoryBudget* budget);

  // Marks build row `row`, from any number of threads at once and any number of times: the write
  // is an OR, so a part that PartScheduler reruns and then discards marks the same bits again and
  // leaves the same answer. `row` must be below num_rows().
  void Mark(std::uint32_t row);
  // Whether build row `row` is marked. Only for a reader that the parts-done signal orders after
  // every Mark: a concurrent Mark races it. `row` must be below num_rows().
  [[nodiscard]] bool marked(std::uint32_t row) const;
  // The bits (the build's num_rows()).
  [[nodiscard]] int64_t num_rows() const { return rows_; }

 private:
  JoinMarks() = default;

  // Relaxed throughout: a set bit must be visible to the one thread that reads the marks after the
  // probe pipeline is done, and the parts-done signal already orders that read after every Mark.
  // Nothing reads a mark to decide anything while probes run, so no mark orders any other write.
  static constexpr std::memory_order kOrder = std::memory_order_relaxed;

  std::vector<std::atomic<std::uint64_t>> words_;
  int64_t rows_ = 0;
  MemoryReservation memory_;
};

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
  // layout's hashes: a temporary std::vector that no budget sees. Invalid for a keyless build, keys
  // of another count, type or length, a selection of another length or a short `out`.
  arrow::Status Find(std::span<const std::shared_ptr<arrow::Array>> keys,
                     const arrow::BooleanArray* selection, arrow::MemoryPool* pool,
                     std::span<JoinMatches> out) const;

  // The inserted rows, each key's rows together and in (part, row) order.
  [[nodiscard]] std::span<const JoinRowRef> rows() const { return rows_; }
  // The parts' rows (every column of the build input) that JoinRowRef::chunk indexes.
  [[nodiscard]] const std::vector<std::shared_ptr<arrow::RecordBatch>>& chunks() const {
    return chunks_;
  }
  // The retained rows whose keys are NULL, in part order; empty unless
  // JoinBuildSpec::keep_null_keys asked for them. They are in no key's range, Find never returns
  // one, and no JoinRowRef addresses them -- a ref's `chunk` indexes chunks() and nothing else,
  // which is what keeps the two stores apart. The join that emits the build rows which did not
  // match emits these whole (ADR 0026's C2c), since a NULL key matches nothing.
  [[nodiscard]] const std::vector<std::shared_ptr<arrow::RecordBatch>>& null_chunks() const {
    return null_chunks_;
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
  std::vector<std::shared_ptr<arrow::RecordBatch>> null_chunks_;  // not indexed by any JoinRowRef
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

  // Part `part` (0 <= part < num_parts), appended to the end; parts may come in any order. Once the
  // build failed, its failure (as Merged), before any other check: a release that fails, and a
  // merge on the executor once its failure is known, wait for the other merges first. Otherwise
  // Invalid for a part outside the build, of another spec or added twice, and after Finish.
  arrow::Status Add(int64_t part, std::shared_ptr<const JoinBuildPart> rows);
  // Waits for the merges of the parts added so far: the build's failure, which every later call
  // returns too. It is the failure of the earliest part, as in the serial order: the merges' (of
  // the smallest (part, partition)), else a failed release's. A failed release hands no later part
  // to the merges, so a merge failure is never of a later part.
  arrow::Status Merged();
  // The table, once every part is added: Invalid for a second call; otherwise the build's failure
  // first (as Merged), then Invalid for a missing part; OutOfMemory past the budget.
  arrow::Result<std::shared_ptr<const JoinTable>> Finish();

  // A Bloom filter over the key hash of every row the parts kept, for the scan of a probe side to
  // reject rows that can match nothing (docs/adr/0029-dynamic-join-filters.md, D1). Null -- not an
  // error -- when the spec's keys are not the one integer or DATE column a scan can hash the same
  // way, or when the filter does not fit the budget.
  //
  // Must be called **before** Finish, which frees the parts: the per-row hashes live in them and
  // nowhere else, and the table keeps none for the direct layout at all. Rows whose key is NULL are
  // not in it, which is right -- a NULL key matches nothing on either side.
  arrow::Result<std::shared_ptr<const KeyFilter>> MakeKeyFilter() const;

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
  arrow::Status failed_;  // the build's failure: the merges', else a failed release's
  // The failure of a release that std::bad_alloc cuts short, made ahead: nothing to allocate then.
  arrow::Status out_of_memory_;
  MemoryReservation memory_;  // parts_
  // Per partition, its runs of rows in part order; each partition is written by its lane only.
  JoinTable::Segments segments_;
  std::array<MemoryReservation, kJoinPartitions> segment_memory_;
  std::unique_ptr<PartitionLanes> lanes_;  // last: destroyed first, it waits for the merges
};

}  // namespace antb1::exec
