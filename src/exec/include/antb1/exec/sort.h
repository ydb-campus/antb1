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
  // < 0 if row i of `a` comes before row j of `b`, 0 if they tie, > 0 if it comes after.
  [[nodiscard]] int Compare(const KeyArrays& a, int64_t i, const KeyArrays& b, int64_t j) const;

 private:
  using CompareValues = int (*)(const arrow::Array& a, int64_t i, const arrow::Array& b, int64_t j);
  struct Key {
    int column = 0;
    CompareValues compare = nullptr;  // both values not NULL, ascending
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
  // Moves the rows of `other` (same schema and keep) behind this buffer's rows.
  arrow::Status Merge(SortBuffer& other, arrow::MemoryPool* pool);
  // Sorts the rows; afterwards num_rows() is at most keep.
  arrow::Status Sort(arrow::MemoryPool* pool);

  // The number of rows (after Sort: the sorted rows).
  [[nodiscard]] int64_t num_rows() const { return rows_; }
  // Rows [begin, end) of the sorted order as one batch; call after Sort.
  [[nodiscard]] arrow::Result<std::shared_ptr<arrow::RecordBatch>> Slice(
      int64_t begin, int64_t end, arrow::MemoryPool* pool) const;

 private:
  // Sorts and, with `keep`, keeps only the first rows (as one chunk) and remembers the last one.
  arrow::Status Compact(arrow::MemoryPool* pool);

  RowComparator comparator_;
  std::optional<int64_t> keep_;
  std::vector<std::shared_ptr<arrow::RecordBatch>> chunks_;
  std::vector<RowComparator::KeyArrays> chunk_keys_;  // per chunk
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
  int64_t batch_size_ = 1;
  std::unique_ptr<SortBuffer> buffer_;
  int64_t next_ = 0;  // the next sorted row to emit
  int64_t end_ = 0;   // one past the last sorted row to emit
  bool sorted_ = false;
};

}  // namespace antb1::exec
