#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include <arrow/memory_pool.h>
#include <arrow/record_batch.h>
#include <arrow/result.h>
#include <arrow/type_fwd.h>
#include <parquet/metadata.h>

#include "antb1/plan/table.h"

#include "scan_segment.h"

// Private to io: a scan of one row group that applies a plan::ScanFilter while it decodes
// (docs/adr/0020-filter-pushdown.md).

namespace antb1::io {

// How one column is read on the filtered path: its Parquet physical type and the engine type it
// becomes.
struct FilteredColumn {
  enum class Physical : std::uint8_t { kInt32, kInt64, kFloat, kDouble, kByteArray };
  int leaf = 0;  // the Parquet leaf column
  Physical physical = Physical::kInt32;
  std::shared_ptr<arrow::DataType> engine;  // the engine type it is read as
};

// The filtered path's reading of a top-level field whose storage type (as Arrow reads it) is
// `storage` and engine type `engine`, at Parquet leaf `leaf` of `metadata`: flat INT16, INT32,
// UINT16 and DATE32 (from INT32), INT64, FLOAT and DOUBLE (as DOUBLE), and BINARY and STRING (as
// BINARY); std::nullopt for anything else (nested, repeated, other types), which the filtered path
// does not read.
std::optional<FilteredColumn> FilteredColumnOf(const parquet::FileMetaData& metadata, int leaf,
                                               const arrow::DataType& storage,
                                               const arrow::DataType& engine);

// Reads row group `segment.row_groups[0]` of `columns` (the scan's fields, in order) in batches
// of at most `batch_size` rows and returns only the rows that pass `filter`: for every batch, the
// filter's columns are decoded first and the filter applied to them as they are decoded (strings
// as views into the decoded pages, not copied); then only the rows that pass are copied into the
// batch, for every column. A batch that loses every row is not returned. Batches have `schema`,
// and with `positions` one more column, `position` (INT64, not null): each row's position in the
// row group (plan::Table::ScanPart).
arrow::Result<std::unique_ptr<arrow::RecordBatchReader>> MakeFilteredScan(
    Segment segment, std::vector<FilteredColumn> columns, std::shared_ptr<arrow::Schema> schema,
    int64_t batch_size, arrow::MemoryPool* pool, std::shared_ptr<const plan::ScanFilter> filter,
    bool positions = false);

}  // namespace antb1::io
