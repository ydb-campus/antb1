#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <arrow/record_batch.h>
#include <arrow/result.h>
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
  // batch_size rows and columns typed as in schema().
  virtual arrow::Result<std::unique_ptr<arrow::RecordBatchReader>> Scan(
      const std::vector<int>& fields, int64_t batch_size) const = 0;

  // One-line description for EXPLAIN, e.g. "parquet(3 files)".
  virtual std::string Describe() const = 0;
};

}  // namespace antb1::plan
