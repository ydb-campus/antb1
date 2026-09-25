#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <arrow/result.h>
#include <arrow/type_fwd.h>

#include "antb1/plan/table.h"
#include "antb1/plan/types.h"

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
  // Reads the files in order, one at a time and single-threaded (a parquet::arrow::FileReader per
  // file, every row group; a top-level field maps to its Parquet leaf columns through the file's
  // schema manifest). Batches have 1..batch_size rows and the engine view of the requested fields
  // in the requested order: utf8 and large strings become binary without UTF-8 validation, float
  // becomes double, and a USMALLINT or INTEGER column read as DATE becomes date32. With no fields
  // the batches only carry row counts. Invalid for a bad or repeated field or batch_size < 1;
  // IOError when a file cannot be read or no longer matches the schema read at Open.
  arrow::Result<std::unique_ptr<arrow::RecordBatchReader>> Scan(const std::vector<int>& fields,
                                                                int64_t batch_size) const override;
  std::string Describe() const override;

  [[nodiscard]] const std::vector<std::string>& files() const { return files_; }
  [[nodiscard]] int64_t total_bytes() const { return total_bytes_; }

 private:
  ParquetTable() = default;

  std::vector<std::string> files_;
  std::shared_ptr<arrow::Schema> storage_schema_;  // as read from the first file
  std::shared_ptr<arrow::Schema> schema_;          // engine view
  int64_t num_rows_ = 0;
  int64_t total_bytes_ = 0;
};

}  // namespace antb1::io
