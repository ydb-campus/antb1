#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <arrow/memory_pool.h>
#include <arrow/record_batch.h>
#include <arrow/result.h>
#include <arrow/status.h>
#include <arrow/type_fwd.h>

namespace antb1::plan {

// A scannable table as seen by the planner and executor. Implemented by io::ParquetTable (and by
// in-memory tables in tests). exec never includes io: it scans through this interface.
class Table {
 public:
  virtual ~Table() = default;

  // Engine view of the columns: every field type is ToArrow(<logical type>) (VARCHAR = binary).
  virtual const std::shared_ptr<arrow::Schema>& schema() const = 0;

  // Exact row count without scanning (e.g. from Parquet footers), if known.
  virtual std::optional<int64_t> exact_row_count() const = 0;

  // Scans the given top-level fields (indices into schema()) in order; batches have at most
  // batch_size rows and columns typed as in schema(). Their buffers are allocated from `pool`
  // (the query's memory budget).
  arrow::Result<std::unique_ptr<arrow::RecordBatchReader>> Scan(
      const std::vector<int>& fields, int64_t batch_size,
      arrow::MemoryPool* pool = arrow::default_memory_pool()) const {
    return DoScan(fields, batch_size, pool);
  }

  // The table split into parts, the units of parallel work (ADR 0013): scanning parts 0 ..
  // num_parts() - 1 one after another gives the rows of Scan in the same order. A Parquet part is
  // one row group of one file. A table without rows may have no parts. By default the whole
  // table is one part.
  virtual int64_t num_parts() const { return 1; }

  // Rows of part `part` without scanning, if known; std::nullopt for a part out of range.
  virtual std::optional<int64_t> part_rows(int64_t part) const {
    return part == 0 ? exact_row_count() : std::nullopt;
  }

  // Scans the rows of part `part` as Scan does (same fields, batches, pool and errors). Invalid
  // for a part out of range.
  arrow::Result<std::unique_ptr<arrow::RecordBatchReader>> ScanPart(
      int64_t part, const std::vector<int>& fields, int64_t batch_size,
      arrow::MemoryPool* pool = arrow::default_memory_pool()) const {
    return DoScanPart(part, fields, batch_size, pool);
  }

  // Whether top-level field `field` (engine type DOUBLE) is stored as FLOAT and widened on read.
  // The binder compares such a column with a number the way DuckDB compares a FLOAT column
  // (plan::DuckDbFloatOf).
  virtual bool StoredAsFloat(int field) const {
    static_cast<void>(field);
    return false;
  }

  // One-line description for EXPLAIN, e.g. "parquet(3 files)".
  virtual std::string Describe() const = 0;

  // Bytes of the table's storage (e.g. the sum of its Parquet file sizes), if known; reported by
  // `antb1 bench` as ClickBench's data_size.
  virtual std::optional<int64_t> data_size() const { return std::nullopt; }

 protected:
  // Scan and ScanPart. A table that does not split itself keeps the default DoScanPart.
  virtual arrow::Result<std::unique_ptr<arrow::RecordBatchReader>> DoScan(
      const std::vector<int>& fields, int64_t batch_size, arrow::MemoryPool* pool) const = 0;
  virtual arrow::Result<std::unique_ptr<arrow::RecordBatchReader>> DoScanPart(
      int64_t part, const std::vector<int>& fields, int64_t batch_size,
      arrow::MemoryPool* pool) const {
    if (part != 0) {
      return arrow::Status::Invalid("scan of part ", part, " of a table with 1 part");
    }
    return DoScan(fields, batch_size, pool);
  }
};

}  // namespace antb1::plan
