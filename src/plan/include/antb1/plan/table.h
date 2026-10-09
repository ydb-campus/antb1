#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <arrow/memory_pool.h>
#include <arrow/record_batch.h>
#include <arrow/result.h>
#include <arrow/status.h>
#include <arrow/type_fwd.h>

#include "antb1/common/int128.h"

namespace antb1::plan {

// What a part's footer says about one integer-valued column (integers, USMALLINT, DATE as days
// since 1970-01-01), for skipping parts that cannot hold a matching row.
struct PartStats {
  std::optional<Int128> min;  // over the non-NULL values; empty when every value is NULL
  std::optional<Int128> max;
  int64_t null_count = 0;
  int64_t rows = 0;
};

// The values of one column of a scan, as a table decoded them and before it copies them into a
// batch (docs/adr/0020-filter-pushdown.md): a column of a fixed-width engine type as an Arrow array
// of that type; a VARCHAR column as one view per row into the table's own buffers (an empty view
// where the row is NULL) and the rows' validity. The views are valid only during the call they are
// passed to.
struct ScanValues {
  std::shared_ptr<arrow::Array> array;        // fixed-width columns; nullptr for VARCHAR
  std::span<const std::string_view> strings;  // VARCHAR: one view per row
  const std::uint8_t* validity = nullptr;     // VARCHAR: a bit per row, 1 where not NULL;
                                              // nullptr: no row is NULL
  int64_t rows = 0;
};

// Predicates a table applies while it scans, before it copies the values it decoded (filter
// pushdown, docs/adr/0020-filter-pushdown.md). Every predicate reads one column, and the rows that
// pass are those for which every predicate is true (SQL's WHERE: NULL fails). Implemented by exec,
// applied by tables that support it (Table::supports_scan_filter). Thread-safe: the parts of a
// query apply one filter concurrently.
class ScanFilter {
 public:
  virtual ~ScanFilter() = default;

  // The columns it reads: positions in the scan's field list, each once.
  [[nodiscard]] virtual const std::vector<int>& columns() const = 0;

  // Clears the bits of `selected` (one bit per row of the batch, bit `offset` for values' first
  // row) of the rows that fail the predicates of column `column` (a position of columns()). A
  // table may call it with the values of a column in several pieces, and may skip rows whose bit
  // is already clear.
  virtual arrow::Status Apply(int column, const ScanValues& values, int64_t offset,
                              std::uint8_t* selected) const = 0;
};

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

  // Whether ScanPart can apply a ScanFilter while it scans these top-level fields.
  [[nodiscard]] virtual bool supports_scan_filter(const std::vector<int>& fields) const {
    static_cast<void>(fields);
    return false;
  }

  // Scans the rows of part `part` that pass `filter` (whose columns are positions in `fields`), as
  // ScanPart does, but only those rows, in the same order: the same batches but for the rows left
  // out (a batch may lose every row and is then not returned). With `positions`, the batches have
  // one more column after the fields, `position` (INT64, not null): each row's position in the
  // part, counting every row of the part (they increase). NotImplemented where
  // supports_scan_filter(fields) is false; Invalid for `positions` without a filter.
  arrow::Result<std::unique_ptr<arrow::RecordBatchReader>> ScanPart(
      int64_t part, const std::vector<int>& fields, int64_t batch_size, arrow::MemoryPool* pool,
      const std::shared_ptr<const ScanFilter>& filter, bool positions = false) const {
    if (filter == nullptr) {
      if (positions) {
        return arrow::Status::Invalid("a scan reports row positions only with a filter");
      }
      return DoScanPart(part, fields, batch_size, pool);
    }
    return DoScanPartFiltered(part, fields, batch_size, pool, filter, positions);
  }

  // The exact statistics of top-level field `field` in part `part`, if known and integer-valued;
  // std::nullopt (never skip) by default and for anything not known exactly.
  virtual std::optional<PartStats> part_stats(int64_t part, int field) const {
    static_cast<void>(part);
    static_cast<void>(field);
    return std::nullopt;
  }

  // The number of distinct non-NULL values of top-level field `field` in part `part` that the
  // writer recorded (Parquet's distinct_count, which DuckDB stores for dictionary-encoded column
  // chunks), if any: a hint for the join order's estimates (ADR 0022), never used for an answer.
  // std::nullopt by default and for a part or field out of range.
  virtual std::optional<int64_t> part_distinct_count(int64_t part, int field) const {
    static_cast<void>(part);
    static_cast<void>(field);
    return std::nullopt;
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
  // ScanPart with a filter, for tables that support it.
  virtual arrow::Result<std::unique_ptr<arrow::RecordBatchReader>> DoScanPartFiltered(
      int64_t /*part*/, const std::vector<int>& /*fields*/, int64_t /*batch_size*/,
      arrow::MemoryPool* /*pool*/, const std::shared_ptr<const ScanFilter>& /*filter*/,
      bool /*positions*/) const {
    return arrow::Status::NotImplemented("this table cannot filter while it scans");
  }
};

}  // namespace antb1::plan
