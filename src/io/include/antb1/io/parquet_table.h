#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <arrow/result.h>
#include <arrow/type_fwd.h>

#include "antb1/plan/table.h"
#include "antb1/plan/types.h"

namespace parquet {
class FileMetaData;
}  // namespace parquet

namespace antb1::io {

// Reinterprets a stored column as another logical type, e.g. ClickBench EventDate (uint16 days
// since 1970-01-01) as DATE. Supported: USMALLINT/INTEGER -> DATE.
struct ColumnOverride {
  std::string column;  // matched ASCII case-insensitively
  plan::LogicalType type = plan::LogicalType::kDate;
};

struct ParquetTableOptions {
  std::vector<ColumnOverride> overrides;
};

// One logical table over 1..N Parquet files with identical schemas (a glob expands to sorted
// files). Unannotated BYTE_ARRAY and UTF8 columns are both VARCHAR (arrow::binary(), compared
// byte-wise). Columns of unsupported types keep their storage type; binding a reference to them
// fails cleanly. All Parquet exceptions and read failures are converted to arrow IOError at this
// boundary.
class ParquetTable final : public plan::Table {
 public:
  static arrow::Result<std::shared_ptr<ParquetTable>> Open(const std::vector<std::string>& paths,
                                                           const ParquetTableOptions& options = {});

  const std::shared_ptr<arrow::Schema>& schema() const override { return schema_; }
  std::optional<int64_t> exact_row_count() const override { return num_rows_; }
  // One part per row group with rows, in file and row-group order.
  int64_t num_parts() const override;
  // The rows of the part's row group, from the footer.
  std::optional<int64_t> part_rows(int64_t part) const override;
  // From the row group's column chunk statistics in the footer: only for a primitive field whose
  // engine type is integer-valued (integers but HUGEINT, USMALLINT, DATE) and whose chunk has valid
  // statistics with min/max and a null count; std::nullopt otherwise (never skip).
  std::optional<plan::PartStats> part_stats(int64_t part, int field) const override;
  std::string Describe() const override;
  // A float column of the files (widened to double on read).
  bool StoredAsFloat(int field) const override;
  // The filtered path (docs/adr/0020-filter-pushdown.md) reads flat INT16, INT32, USMALLINT,
  // DATE, BIGINT, DOUBLE (also stored as FLOAT) and VARCHAR columns.
  [[nodiscard]] bool supports_scan_filter(const std::vector<int>& fields) const override;
  // The sum of the file sizes (total_bytes()).
  std::optional<int64_t> data_size() const override { return total_bytes_; }

  [[nodiscard]] const std::vector<std::string>& files() const { return files_; }
  [[nodiscard]] int64_t total_bytes() const { return total_bytes_; }

 protected:
  // Scan reads the files in order, one at a time and single-threaded (a parquet::arrow::FileReader
  // per file over the footer read at Open, every row group; a top-level field maps to its Parquet
  // leaf columns through the file's schema manifest). Batches have 1..batch_size rows and the
  // engine view of the requested fields in the requested order: utf8 and large strings become
  // binary without UTF-8 validation, float becomes double, and a USMALLINT or INTEGER column read
  // as DATE becomes date32. With no fields the batches only carry row counts. Invalid for a bad or
  // repeated field or batch_size < 1; IOError when a file cannot be read or changed after Open (its
  // size or footer differs). Pages, decoded columns and conversions are allocated from `pool`.
  arrow::Result<std::unique_ptr<arrow::RecordBatchReader>> DoScan(
      const std::vector<int>& fields, int64_t batch_size, arrow::MemoryPool* pool) const override;
  // ScanPart reads the part's row group as Scan reads a file.
  arrow::Result<std::unique_ptr<arrow::RecordBatchReader>> DoScanPart(
      int64_t part, const std::vector<int>& fields, int64_t batch_size,
      arrow::MemoryPool* pool) const override;
  // ScanPart with a filter: the row group through Parquet's column readers, the filter's columns
  // first (strings as views into the decoded pages), then only the rows that pass of every
  // column (filtered_scan.h).
  arrow::Result<std::unique_ptr<arrow::RecordBatchReader>> DoScanPartFiltered(
      int64_t part, const std::vector<int>& fields, int64_t batch_size, arrow::MemoryPool* pool,
      const std::shared_ptr<const plan::ScanFilter>& filter, bool positions) const override;

 private:
  // One row group of one file.
  struct Part {
    std::size_t file = 0;  // index into files_
    int row_group = 0;
    int64_t rows = 0;
  };

  ParquetTable() = default;

  // The footers read at Open are reused by every scan, so a scan never parses a footer and always
  // reads the row groups and types counted at Open; it checks that the file still ends with the
  // same footer bytes.
  std::vector<std::string> files_;
  std::vector<std::shared_ptr<parquet::FileMetaData>> metadata_;  // per file
  std::vector<int64_t> file_bytes_;                               // per file
  std::vector<std::shared_ptr<arrow::Buffer>> footers_;           // per file, as stored
  std::vector<Part> parts_;
  std::vector<int> leaf_of_field_;  // a top-level field's Parquet leaf column; -1 if nested
  std::shared_ptr<arrow::Schema> storage_schema_;  // as read from the first file
  std::shared_ptr<arrow::Schema> schema_;          // engine view
  int64_t num_rows_ = 0;
  int64_t total_bytes_ = 0;
};

}  // namespace antb1::io
