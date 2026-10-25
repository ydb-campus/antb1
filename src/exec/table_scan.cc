#include "antb1/exec/table_scan.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <arrow/api.h>

#include "antb1/exec/profile.h"
#include "antb1/exec/scan_filter.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/table.h"

namespace antb1::exec {

namespace {

std::shared_ptr<arrow::Schema> FieldsOf(const plan::Table& table, const std::vector<int>& fields,
                                        const std::optional<LateScan>& late) {
  const arrow::Schema& schema = *table.schema();
  arrow::FieldVector out;
  out.reserve(fields.size());
  for (std::size_t i = 0; i < fields.size(); ++i) {
    const int f = fields[i];
    // An index outside the schema makes Open fail (plan::Table::Scan validates the request).
    std::shared_ptr<arrow::Field> field =
        f >= 0 && f < schema.num_fields() ? schema.field(f) : arrow::field("?", arrow::null());
    if (late.has_value() && i < late->late.size() && late->late[i]) {
      field = std::cmp_equal(late->row_id, i) ? arrow::field("row_id", arrow::int64(), false)
                                              : arrow::field(field->name(), arrow::null());
    }
    out.push_back(std::move(field));
  }
  return arrow::schema(std::move(out));
}

}  // namespace

TableScanOperator::TableScanOperator(std::shared_ptr<plan::Table> table, std::vector<int> fields,
                                     std::optional<int64_t> part, std::optional<LateScan> late,
                                     std::vector<plan::Predicate> pushed,
                                     std::optional<KeyScan> key_scan, std::vector<bool> filter_only)
    : table_(std::move(table)),
      fields_(std::move(fields)),
      part_(part),
      late_(std::move(late)),
      pushed_(std::move(pushed)),
      key_scan_(std::move(key_scan)),
      filter_only_(std::move(filter_only)),
      schema_(FieldsOf(*table_, fields_, late_)) {
  for (std::size_t i = 0; i < fields_.size(); ++i) {
    if (!late_.has_value() || i >= late_->late.size() || !late_->late[i]) {
      read_.push_back(fields_[i]);
    }
  }
  // The request a filtered scan gets: the fields it returns first, in their own order, then the
  // fields it reads only to filter on. Ordered that way because a table is told how many of the
  // fields it returns and not which, and because the returned ones keep the positions every
  // operator above already resolved. The predicates are rebound once here rather than per Open --
  // the inverse of NarrowFilter, which rebinds them to a list narrower than the output.
  const auto marked = [&](std::size_t i) { return i < filter_only_.size() && filter_only_[i]; };
  scan_position_.assign(fields_.size(), -1);
  arrow::FieldVector scan_fields;
  for (const bool pass : {false, true}) {
    for (std::size_t i = 0; i < fields_.size(); ++i) {
      if (marked(i) != pass) {
        continue;
      }
      scan_position_[i] = static_cast<int>(scan_fields_.size());
      scan_fields_.push_back(fields_[i]);
      const arrow::Schema& table_schema = *table_->schema();
      const int f = fields_[i];
      scan_fields.push_back(f >= 0 && f < table_schema.num_fields()
                                ? table_schema.field(f)
                                : arrow::field("?", arrow::null()));
      returned_ += pass ? 0 : 1;
    }
  }
  scan_schema_ = arrow::schema(std::move(scan_fields));
  scan_pushed_ = pushed_;
  for (plan::Predicate& p : scan_pushed_) {
    if (p.column.has_value()) {
      p.column->index = ScanPositionOf(p.column->index);
    }
  }
}

int TableScanOperator::ScanPositionOf(int field) const {
  // A field's place in the list a filtered scan is asked for. **Everything that names a field by
  // position goes through here**, which is the whole of the risk in reordering that list: a key
  // filter's column (ADR 0029) is never dropped -- the join reads it -- but it moves when a field
  // before it is read only to filter on, and a stale index there filters on another column and
  // changes the answer.
  if (field < 0 || std::cmp_greater_equal(field, scan_position_.size())) {
    return field;
  }
  return scan_position_[static_cast<std::size_t>(field)];
}

arrow::Status TableScanOperator::Open(ExecContext& ctx) {
  offset_ = 0;
  pool_ = ctx.pool;
  positioned_ = false;
  if ((!pushed_.empty() || key_scan_.has_value()) && !part_.has_value()) {
    return arrow::Status::Invalid("only the scan of a part applies predicates");
  }
  // A part holding no key the build published matches nothing, so it is not read at all: no reader
  // is opened, nothing is decoded, and Next() ends straight away (ADR 0029's D2). Before the
  // branches below because it makes all of them moot -- including the narrow scan's.
  pruned_ = key_scan_.has_value() && key_scan_->part_has_no_key;
  if (pruned_) {
    return arrow::Status::OK();
  }
  if (late_.has_value()) {
    const LateScan& late = late_.value();
    if (!part_.has_value() || late.late.size() != fields_.size() || late.row_id < 0 ||
        std::cmp_greater_equal(late.row_id, fields_.size()) ||
        !late.late[static_cast<std::size_t>(late.row_id)] || late.ordinal < 0 ||
        late.ordinal >= kRowIdParts) {
      return arrow::Status::Invalid("a narrow scan of a part needs its late columns and row id");
    }
    if (pushed_.empty()) {
      ARROW_ASSIGN_OR_RAISE(reader_, table_->ScanPart(*part_, read_, ctx.batch_size, ctx.pool));
    } else {
      // The predicates read output columns; the scan reads only the early ones (read_), and
      // reports the positions of the rows that pass for their row ids.
      ARROW_ASSIGN_OR_RAISE(auto filter, NarrowFilter(late, ctx.pool));
      ARROW_ASSIGN_OR_RAISE(reader_, table_->ScanPart(*part_, read_, ctx.batch_size, ctx.pool,
                                                      filter, /*positions=*/true));
      positioned_ = true;
    }
  } else if (part_.has_value()) {
    // The predicates pushed into the scan (ADR 0020) and the key filter a join above published
    // (ADR 0029's D1), in whichever combination this scan has: a scan used to take one or the
    // other, because it takes one ScanFilter, and takes their conjunction now.
    std::shared_ptr<const plan::ScanFilter> filter;
    std::shared_ptr<const plan::ScanFilter> keys;
    if (!pushed_.empty()) {
      // Counted, and reported, only where a predicate is on a VARCHAR column: that is the only
      // place a dictionary is read, so elsewhere the number would be a zero on every scan of every
      // plan rather than an answer to a question anyone asked.
      const bool on_strings = std::ranges::any_of(scan_pushed_, [&](const plan::Predicate& p) {
        return p.column.has_value() && p.column->index >= 0 &&
               p.column->index < scan_schema_->num_fields() &&
               scan_schema_->field(p.column->index)->type()->id() == arrow::Type::BINARY;
      });
      if (on_strings) {
        dictionary_pieces_ = std::make_shared<std::atomic<int64_t>>(0);
      }
      ARROW_ASSIGN_OR_RAISE(
          filter, MakeScanFilter(scan_pushed_, *scan_schema_, ctx.pool, dictionary_pieces_));
    }
    if (key_scan_.has_value() && key_scan_->filter != nullptr) {
      key_dropped_ = std::make_shared<std::atomic<int64_t>>(0);
      ARROW_ASSIGN_OR_RAISE(
          keys,
          MakeKeyScanFilter(key_scan_->filter, ScanPositionOf(key_scan_->column), key_dropped_));
    }
    if (filter == nullptr) {
      filter = std::move(keys);
    } else if (keys != nullptr) {
      ARROW_ASSIGN_OR_RAISE(filter, MakeBothScanFilters(std::move(filter), std::move(keys)));
    }
    ARROW_ASSIGN_OR_RAISE(reader_,
                          table_->ScanPart(*part_, scan_fields_, ctx.batch_size, ctx.pool, filter,
                                           /*positions=*/false, returned_));
  } else {
    ARROW_ASSIGN_OR_RAISE(reader_, table_->Scan(fields_, ctx.batch_size, ctx.pool));
  }
  return arrow::Status::OK();
}

arrow::Result<Batch> TableScanOperator::Next() {
  if (pruned_) {
    return Batch{};  // the part holds no published key, so it has no rows for the join
  }
  if (reader_ == nullptr) {
    return arrow::Status::Invalid("table scan: Next() before Open()");
  }
  std::shared_ptr<arrow::RecordBatch> batch;
  ARROW_RETURN_NOT_OK(reader_->ReadNext(&batch));
  if (batch == nullptr) {
    return Batch{};
  }
  if (late_.has_value()) {
    return Narrow(*batch, late_.value());
  }
  if (std::cmp_less(returned_, fields_.size())) {
    ARROW_ASSIGN_OR_RAISE(batch, WithPlaceholders(*batch));
  }
  if (!batch->schema()->Equals(*schema_, /*check_metadata=*/false)) {
    // Same types under other names (e.g. an in-memory table); a type mismatch is an error.
    ARROW_ASSIGN_OR_RAISE(batch, batch->ReplaceSchema(schema_));
  }
  return Batch{.data = std::move(batch), .selection = {}};
}

arrow::Result<std::shared_ptr<arrow::RecordBatch>> TableScanOperator::WithPlaceholders(
    const arrow::RecordBatch& read) const {
  // `read` holds the returned fields in their own order; this puts them back at the positions
  // output_schema() gives them, with an all-NULL array of the field's **declared** type where a
  // field was read only to filter on.
  //
  // **Declared, not the `null` type ADR 0016's late split uses.** A late column is re-read by a
  // node that knows it is a placeholder; this one can flow anywhere a real column can -- into a
  // join build, which may carry it past the join and gather it -- and a reader that meets the
  // `null` type there has no way to proceed. An ANTB1_CHECK in the gather said so on the first
  // run, which is the one place this could have been got wrong quietly. The saving is unaffected:
  // what cost the scan was the bytes and a view a row, and neither is here.
  arrow::ArrayVector columns;
  columns.reserve(fields_.size());
  int next = 0;
  for (std::size_t i = 0; i < fields_.size(); ++i) {
    if (i < filter_only_.size() && filter_only_[i]) {
      ARROW_ASSIGN_OR_RAISE(std::shared_ptr<arrow::Array> absent,
                            arrow::MakeArrayOfNull(schema_->field(static_cast<int>(i))->type(),
                                                   read.num_rows(), pool_));
      columns.push_back(std::move(absent));
      continue;
    }
    if (next >= read.num_columns()) {
      return arrow::Status::Invalid("a filtered scan returned ", read.num_columns(),
                                    " columns for ", returned_, " returned fields");
    }
    columns.push_back(read.column(next++));
  }
  return arrow::RecordBatch::Make(schema_, read.num_rows(), std::move(columns));
}

arrow::Result<std::shared_ptr<const plan::ScanFilter>> TableScanOperator::NarrowFilter(
    const LateScan& late, arrow::MemoryPool* pool) const {
  std::vector<int> read_index(fields_.size(), -1);  // per output column: its position in read_
  arrow::FieldVector read_fields;
  for (std::size_t i = 0; i < fields_.size(); ++i) {
    if (!late.late[i]) {
      read_index[i] = static_cast<int>(read_fields.size());
      read_fields.push_back(schema_->field(static_cast<int>(i)));
    }
  }
  std::vector<plan::Predicate> rebound = pushed_;
  for (plan::Predicate& p : rebound) {
    if (!p.column.has_value() || p.column->index < 0 ||
        std::cmp_greater_equal(p.column->index, fields_.size()) ||
        read_index[static_cast<std::size_t>(p.column->index)] < 0) {
      return arrow::Status::Invalid("a narrow scan applies predicates on its early columns only");
    }
    p.column->index = read_index[static_cast<std::size_t>(p.column->index)];
  }
  return MakeScanFilter(rebound, *arrow::schema(std::move(read_fields)), pool);
}

arrow::Result<Batch> TableScanOperator::Narrow(const arrow::RecordBatch& read,
                                               const LateScan& late) {
  const int64_t rows = read.num_rows();
  // A filtered scan reports the rows' positions in the part (increasing); else they follow on.
  const arrow::Int64Array* positions = nullptr;
  bool in_range = offset_ + rows <= kRowIdParts * 2;
  if (positioned_) {
    if (std::cmp_not_equal(read.num_columns(), read_.size() + 1) ||
        read.column(read.num_columns() - 1)->type_id() != arrow::Type::INT64 ||
        read.column(read.num_columns() - 1)->null_count() > 0) {
      return arrow::Status::Invalid("a filtered narrow scan without row positions");
    }
    positions = &static_cast<const arrow::Int64Array&>(*read.column(read.num_columns() - 1));
    in_range =
        rows == 0 || (positions->Value(0) >= 0 && positions->Value(rows - 1) < kRowIdParts * 2);
  }
  if (!in_range) {
    return arrow::Status::Invalid("a part of more than 2^32 rows has no row ids");
  }
  arrow::ArrayVector columns;
  columns.reserve(fields_.size());
  int next = 0;  // the next column read
  for (std::size_t i = 0; i < fields_.size(); ++i) {
    if (!late.late[i]) {
      columns.push_back(read.column(next++));
    } else if (std::cmp_equal(late.row_id, i)) {
      arrow::Int64Builder ids(pool_);
      ARROW_RETURN_NOT_OK(ids.Reserve(rows));
      for (int64_t r = 0; r < rows; ++r) {
        ids.UnsafeAppend(
            RowId(late.ordinal, positions != nullptr ? positions->Value(r) : offset_ + r));
      }
      ARROW_ASSIGN_OR_RAISE(auto array, ids.Finish());
      columns.push_back(std::move(array));
    } else {
      columns.push_back(std::make_shared<arrow::NullArray>(rows));
    }
  }
  offset_ += rows;
  return Batch{.data = arrow::RecordBatch::Make(schema_, rows, std::move(columns)),
               .selection = {}};
}

arrow::Status TableScanOperator::Close() {
  // What the dynamic join filter did to this part, which nothing above the scan can see: the rows
  // the key filter cleared, or that the part was skipped whole (ADR 0029's D3). On Close so that it
  // is reported once a part, after every batch of it.
  if (profile() != nullptr && key_dropped_ != nullptr) {
    profile()->Add("key_dropped", MetricUnit::kCount,
                   key_dropped_->load(std::memory_order_relaxed));
  }
  // Which question each pushed predicate was asked, and how many columns were read for a predicate
  // and nothing else: neither is visible in a result, and both are the whole of what this part of
  // ADR 0020 does.
  if (profile() != nullptr && dictionary_pieces_ != nullptr) {
    profile()->Add("dict_pieces", MetricUnit::kCount,
                   dictionary_pieces_->load(std::memory_order_relaxed));
  }
  if (profile() != nullptr && std::cmp_less(returned_, scan_fields_.size())) {
    profile()->Max("cols_filter_only", MetricUnit::kCount,
                   static_cast<int64_t>(scan_fields_.size()) - returned_);
  }
  if (reader_ == nullptr) {
    return arrow::Status::OK();
  }
  const arrow::Status status = reader_->Close();
  reader_.reset();
  return status;
}

}  // namespace antb1::exec
