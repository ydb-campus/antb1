#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include <arrow/memory_pool.h>
#include <arrow/record_batch.h>
#include <arrow/result.h>
#include <arrow/status.h>
#include <arrow/type_fwd.h>

#include "antb1/common/int128.h"
#include "antb1/exec/memory_budget.h"
#include "antb1/exec/operator.h"
#include "antb1/plan/logical_plan.h"

// ORDER BY: a row comparator with DuckDB's order, a sort buffer that also keeps only the first rows
// (top-N), and the Sort operator built on them.

namespace antb1::exec {

// Compares rows of batches with one schema by plan::SortKey: VARCHAR by bytes, DOUBLE with -0.0
// equal to 0.0 and NaN above every number (NaN equal to NaN), NULLs first or last whatever the
// direction, later keys breaking ties of earlier ones.
class RowComparator {
 public:
  // The key columns of one batch, in key order; valid while the batch lives.
  using KeyArrays = std::vector<const arrow::Array*>;

  // Fails if a key is outside the schema or its column has no engine type (plan::ToArrow).
  static arrow::Result<RowComparator> Make(std::shared_ptr<arrow::Schema> schema,
                                           const std::vector<plan::SortKey>& keys);

  [[nodiscard]] const std::shared_ptr<arrow::Schema>& schema() const { return schema_; }
  [[nodiscard]] KeyArrays KeysOf(const arrow::RecordBatch& batch) const;
  // < 0 if row i of `a` comes before row j of `b`, 0 if they tie, > 0 if it comes after, by the
  // keys from `first_key` on.
  [[nodiscard]] int Compare(const KeyArrays& a, int64_t i, const KeyArrays& b, int64_t j,
                            std::size_t first_key = 0) const;

  // The first key of a row as two numbers that order rows as the key does: a group (0: NULL first,
  // 1: a value, 2: NULL last) and, for a value, 64 bits of it in the key's direction. Rows with a
  // smaller (group, bits) come first; rows with equal ones need Compare, from key 1 on when
  // prefix_is_exact() (the bits are the whole value), else from key 0.
  struct Prefix {
    std::uint64_t bits = 0;
    std::uint8_t group = 0;
  };
  [[nodiscard]] Prefix PrefixOf(const KeyArrays& keys, int64_t row) const;
  [[nodiscard]] bool prefix_is_exact() const { return keys_.front().exact; }

 private:
  using CompareValues = int (*)(const arrow::Array& a, int64_t i, const arrow::Array& b, int64_t j);
  using PrefixBits = std::uint64_t (*)(const arrow::Array& a, int64_t i);
  struct Key {
    int column = 0;
    CompareValues compare = nullptr;  // both values not NULL, ascending
    PrefixBits prefix = nullptr;      // a value not NULL, ascending
    bool exact = false;               // `prefix` orders every value (not only its first bits)
    bool descending = false;
    bool nulls_first = false;
  };

  RowComparator(std::shared_ptr<arrow::Schema> schema, std::vector<Key> keys)
      : schema_(std::move(schema)), keys_(std::move(keys)) {}

  std::shared_ptr<arrow::Schema> schema_;
  std::vector<Key> keys_;
};

// Rows added in input order, sorted stably (tied rows keep their input order). With `keep`, only
// the first `keep` rows of that order are wanted: rows that cannot be among them are dropped as
// they arrive, so memory stays O(keep) plus one batch.
//
// Merge appends the rows of another buffer as if they came after this buffer's rows, so buffers
// filled from consecutive parts of the input (for example row groups read by different threads)
// and merged in input order give exactly the single-buffer result.
class SortBuffer {
 public:
  struct RowRef {
    std::uint32_t chunk = 0;
    std::uint32_t row = 0;
  };

  SortBuffer(RowComparator comparator, std::optional<int64_t> keep);

  // Rows with the comparator's schema, without a selection.
  arrow::Status Add(std::shared_ptr<arrow::RecordBatch> rows, arrow::MemoryPool* pool);
  // Moves the rows of `other` (another buffer of the same schema and keep) behind this buffer's
  // rows. `other` is left empty and unsorted, like a new buffer, also when adding its rows fails; a
  // merge it refuses (of a buffer into itself, or of another schema or keep) changes neither.
  arrow::Status Merge(SortBuffer& other, arrow::MemoryPool* pool);
  // Sorts the rows; afterwards num_rows() is at most keep.
  arrow::Status Sort(arrow::MemoryPool* pool);
  // Establishes the bound ThresholdKey() reads, so that a caller can ask for it between Adds: with
  // `keep` and at least that many rows, the last of the first `keep` rows of the order so far. A
  // no-op without `keep`, with fewer rows, or when one is already established.
  arrow::Status Tighten(arrow::MemoryPool* pool);

  // The first sort key's value in the last kept row: what a row has to beat to enter the top-N, so
  // a part whose values are all worse holds nothing this buffer wants (exec::PartMayBeat). Empty
  // until a compaction or Tighten has established one, when the key is not integer-valued (only
  // those have part statistics to compare with) and when its value in that row is NULL.
  [[nodiscard]] std::optional<Int128> ThresholdKey() const;

  // The number of rows (after Sort: the sorted rows).
  [[nodiscard]] int64_t num_rows() const { return rows_; }
  // The bytes held outside Arrow buffers (the rows' batches are counted by their pool): the
  // per-chunk vectors (each chunk's key arrays), the per-column lists of every chunk's arrays and
  // the sorted row references.
  [[nodiscard]] int64_t memory_usage() const;
  // At most the bytes Sort holds on top of memory_usage() while it sorts.
  [[nodiscard]] int64_t sort_memory() const;
  // Rows [begin, end) of the sorted order as one batch, each column gathered from its arrays (taken
  // once per chunk, as it arrived). Invalid unless the rows are sorted: after Sort, and after a
  // compaction (Tighten, or an Add with `keep` past its slack), until the next Add of rows.
  [[nodiscard]] arrow::Result<std::shared_ptr<arrow::RecordBatch>> Slice(
      int64_t begin, int64_t end, arrow::MemoryPool* pool) const;

 private:
  // Sorts and, with `keep`, keeps only the first rows (as one chunk) and remembers the last one.
  arrow::Status Compact(arrow::MemoryPool* pool);
  // The only places chunks_, chunk_keys_ and column_arrays_ change, so that they stay in step.
  // ClearChunks empties all three and returns the chunks it took.
  void PushChunk(std::shared_ptr<arrow::RecordBatch> rows, RowComparator::KeyArrays keys);
  std::vector<std::shared_ptr<arrow::RecordBatch>> ClearChunks();

  RowComparator comparator_;
  std::optional<int64_t> keep_;
  std::vector<std::shared_ptr<arrow::RecordBatch>> chunks_;
  std::vector<RowComparator::KeyArrays> chunk_keys_;  // per chunk
  // Per column, its array in every chunk, in chunk order: the batches' own
  // (arrow::RecordBatch::columns), taken once as each chunk arrives, so that Slice gathers without
  // asking every chunk for every column again (a lock and a reference count each).
  std::vector<std::vector<const arrow::Array*>> column_arrays_;
  int64_t rows_ = 0;
  std::vector<RowRef> order_;        // after Sort
  std::optional<RowRef> threshold_;  // with keep: the last kept row after a compaction
  bool sorted_ = false;
};

// ORDER BY: reads its whole input, then emits it sorted (stably) in batches of the context's batch
// size. With a limit (ORDER BY .. LIMIT, a top-N) it emits only the rows [offset, offset + limit)
// of that order and keeps just limit + offset rows while it reads. Output: the input columns.
class SortOperator final : public Operator {
 public:
  SortOperator(std::unique_ptr<Operator> input, std::vector<plan::SortKey> keys,
               std::optional<int64_t> limit = std::nullopt, int64_t offset = 0);

  [[nodiscard]] const std::shared_ptr<arrow::Schema>& output_schema() const override {
    return input_->output_schema();
  }
  arrow::Status Open(ExecContext& ctx) override;
  arrow::Result<Batch> Next() override;
  arrow::Status Close() override;

 private:
  std::unique_ptr<Operator> input_;
  std::vector<plan::SortKey> keys_;
  std::optional<int64_t> limit_;
  int64_t offset_ = 0;
  arrow::MemoryPool* pool_ = arrow::default_memory_pool();
  MemoryReservation memory_;  // the buffer's own containers
  int64_t batch_size_ = 1;
  std::unique_ptr<SortBuffer> buffer_;
  int64_t next_ = 0;  // the next sorted row to emit
  int64_t end_ = 0;   // one past the last sorted row to emit
  bool sorted_ = false;
};

}  // namespace antb1::exec
