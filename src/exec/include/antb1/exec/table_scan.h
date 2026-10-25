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
  //     sees a row whose key the build cannot hold (D1). Null when the scan applies none. Where the
  //     scan also has predicates pushed into it the two are applied together, as the conjunction a
  //     scan's one ScanFilter can be (exec::MakeBothScanFilters), so neither displaces the other.
  //   - `part_has_no_key`, that this part holds no key the build published, so the scan reads
  //     nothing at all -- no open, no decode, no filtering (D2). It needs no filter of any kind,
  //     which is why it survived even before the two could be applied together.
  struct KeyScan {
    std::shared_ptr<const KeyFilter> filter;
    int column = 0;  // a position in `fields`, of the probe's one key column
    bool part_has_no_key = false;
  };

  // `filter_only` (empty: none) marks, per field, a field read only so that a pushed predicate can
  // be applied to it (ADR 0020): the table reads and filters on it and never copies it, and this
  // operator puts a NullArray in its place.
  //
  // **Its position is kept, not removed.** Every operator above reads a column by the position
  // plan::ResolvePositions gave it, so removing one from the middle would move every later column
  // out from under them -- which is why output_schema() names the field with the `null` type
  // instead, as ADR 0016's late split already does for a column it defers. Nothing above reads the
  // column by construction, so nothing reads the placeholder; it costs no buffer, a NullArray
  // having none. Not combined with a late scan.
  TableScanOperator(std::shared_ptr<plan::Table> table, std::vector<int> fields,
                    std::optional<int64_t> part = std::nullopt,
                    std::optional<LateScan> late = std::nullopt,
                    std::vector<plan::Predicate> pushed = {},
                    std::optional<KeyScan> key_scan = std::nullopt,
                    std::vector<bool> filter_only = {});

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
  // The fields a filtered scan is asked for: the returned ones in their own order, then the ones
  // read only to filter on. `returned_` is how many of them the batches carry, so the predicates
  // are rebound to positions in this list rather than in fields_.
  std::vector<int> scan_fields_;
  std::vector<plan::Predicate> scan_pushed_;    // pushed_, rebound to scan_fields_
  std::shared_ptr<arrow::Schema> scan_schema_;  // the types of scan_fields_, for the filter
  std::vector<bool> filter_only_;               // per field of fields_
  int returned_ = 0;
  std::shared_ptr<arrow::Schema> schema_;
  std::unique_ptr<arrow::RecordBatchReader> reader_;
  int64_t offset_ = 0;       // with late_: the position in the part of the next batch's first row
  bool positioned_ = false;  // a narrow scan whose batches end with the rows' positions
  bool pruned_ = false;      // the part holds no published key: no reader was opened at all
  // The rows the key filter cleared, which only the filter can count (the operators above see the
  // survivors alone): shared with the filter, and reported on Close (ADR 0029's D3).
  std::shared_ptr<std::atomic<int64_t>> key_dropped_;
  // How many (column, piece) pairs had their predicate asked once per distinct value of a chunk's
  // dictionary rather than once per row: shared with the filter, reported on Close (ADR 0020).
  std::shared_ptr<std::atomic<int64_t>> dictionary_pieces_;
  arrow::MemoryPool* pool_ = arrow::default_memory_pool();

  // A batch of the returned fields as output_schema() describes it: the fields back at their own
  // positions, with a NullArray for each read only to filter on.
  arrow::Result<std::shared_ptr<arrow::RecordBatch>> WithPlaceholders(
      const arrow::RecordBatch& read) const;
  // The filter of the pushed predicates over the narrow scan's read fields.
  arrow::Result<std::shared_ptr<const plan::ScanFilter>> NarrowFilter(
      const LateScan& late, arrow::MemoryPool* pool) const;
  // A batch of the read fields (and with positioned_, the rows' positions) as the narrow scan's
  // output.
  arrow::Result<Batch> Narrow(const arrow::RecordBatch& read, const LateScan& late);
};

}  // namespace antb1::exec
