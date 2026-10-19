#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include <arrow/record_batch.h>

#include "antb1/exec/key_filter.h"
#include "antb1/exec/operator.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/table.h"

namespace antb1::exec {

// The narrow scan of a part for late materialization (docs/adr/0016-late-materialization.md): the
// late output columns are not read; each is a NullArray of the batch's length, except `row_id`,
// which carries every row's id instead: RowId(ordinal, the row's position in the part).
struct LateScan {
  std::vector<bool> late;  // per output column
  int row_id = -1;         // a late column
  int64_t ordinal = 0;     // the part's number in the row ids
};

// The row id of the row at `offset` (< kRowIdParts) in part number `ordinal` (< kRowIdParts) of a
// narrow scan, and back.
constexpr int64_t kRowIdParts = 2147483648;  // 2^31: parts, and 2^32 rows per part, row ids name
constexpr int64_t RowId(int64_t ordinal, int64_t offset) {
  return (ordinal * kRowIdParts * 2) + offset;
}
constexpr int64_t RowIdOrdinal(int64_t id) { return id / (kRowIdParts * 2); }
constexpr int64_t RowIdOffset(int64_t id) { return id % (kRowIdParts * 2); }

// Reads the given top-level fields of a table (plan::Table::Scan), or of one of its parts
// (plan::Table::ScanPart), in batches of at most ExecContext::batch_size rows. Only the referenced
// fields are decoded; with no fields the batches carry row counts only. Output: the fields, in the
// given order. With `late` (a part only), the late fields are not read (LateScan). With `pushed`
// (a part only), the table applies those WHERE predicates on the fields while it scans
// (PushableToScan, docs/adr/0020-filter-pushdown.md): only the rows that pass are returned. A
// narrow scan applies them on its early fields only, and takes its row ids from the positions the
// table reports.
class TableScanOperator final : public Operator {
 public:
  // What a hash join's build says about this part of the scan
  // (docs/adr/0029-dynamic-join-filters.md), over one field of it:
  //
  //   - `filter`, the key filter the scan drops rows with as it decodes them, so the join never
  //     sees a row whose key the build cannot hold (D1). Null when the scan applies none: it takes
  //     one filter, and pushed predicates have the first claim on it.
  //   - `part_has_no_key`, that this part holds no key the build published, so the scan reads
  //     nothing at all -- no open, no decode, no filtering (D2). Independent of the filter slot,
  //     which is why a scan with pushed predicates can still carry this.
  struct KeyScan {
    std::shared_ptr<const KeyFilter> filter;
    int column = 0;  // a position in `fields`, of the probe's one key column
    bool part_has_no_key = false;
  };

  TableScanOperator(std::shared_ptr<plan::Table> table, std::vector<int> fields,
                    std::optional<int64_t> part = std::nullopt,
                    std::optional<LateScan> late = std::nullopt,
                    std::vector<plan::Predicate> pushed = {},
                    std::optional<KeyScan> key_scan = std::nullopt);

  [[nodiscard]] const std::shared_ptr<arrow::Schema>& output_schema() const override {
    return schema_;
  }
  arrow::Status Open(ExecContext& ctx) override;
  arrow::Result<Batch> Next() override;
  arrow::Status Close() override;

 private:
  std::shared_ptr<plan::Table> table_;
  std::vector<int> fields_;
  std::optional<int64_t> part_;
  std::optional<LateScan> late_;
  std::vector<plan::Predicate> pushed_;
  std::optional<KeyScan> key_scan_;
  std::vector<int> read_;  // the fields read: fields_ but the late ones
  std::shared_ptr<arrow::Schema> schema_;
  std::unique_ptr<arrow::RecordBatchReader> reader_;
  int64_t offset_ = 0;       // with late_: the position in the part of the next batch's first row
  bool positioned_ = false;  // a narrow scan whose batches end with the rows' positions
  bool pruned_ = false;      // the part holds no published key: no reader was opened at all
  // The rows the key filter cleared, which only the filter can count (the operators above see the
  // survivors alone): shared with the filter, and reported on Close (ADR 0029's D3).
  std::shared_ptr<std::atomic<int64_t>> key_dropped_;
  arrow::MemoryPool* pool_ = arrow::default_memory_pool();

  // The filter of the pushed predicates over the narrow scan's read fields.
  arrow::Result<std::shared_ptr<const plan::ScanFilter>> NarrowFilter(
      const LateScan& late, arrow::MemoryPool* pool) const;
  // A batch of the read fields (and with positioned_, the rows' positions) as the narrow scan's
  // output.
  arrow::Result<Batch> Narrow(const arrow::RecordBatch& read, const LateScan& late);
};

}  // namespace antb1::exec
