#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <arrow/memory_pool.h>
#include <arrow/record_batch.h>
#include <arrow/result.h>
#include <arrow/status.h>
#include <arrow/type_fwd.h>

#include "antb1/common/int128.h"

namespace antb1::plan {

class ScanContext;

// Table-specific state shared by the scans of one execution. It must not retain a query's
// memory pool or open files: readers own those resources and may outlive their context.
class TableScanState {
 public:
  virtual ~TableScanState() = default;
  // Independent final checks, called after all readers/workers have closed and before their rows
  // become a successful result. Different pieces may run concurrently. Any piece, including an
  // already successful one, must be safe to repeat after another piece runs out of memory.
  // Retain no allocation made from the pool after the call returns.
  virtual int64_t num_finish_parts() const { return 1; }
  virtual arrow::Status FinishPart(int64_t part, arrow::MemoryPool* /*pool*/) {
    return part == 0 ? arrow::Status::OK()
                     : arrow::Status::Invalid("finish of part ", part, " of a scan with 1 part");
  }

  // Direct callers use serial validation. The executor can schedule the same pieces on its
  // bounded query executor, including a serial retry when parallel checks run out of memory.
  arrow::Status Finish(arrow::MemoryPool* pool) {
    const int64_t parts = num_finish_parts();
    if (parts < 0) {
      return arrow::Status::Invalid("negative number of scan finish parts: ", parts);
    }
    for (int64_t part = 0; part < parts; ++part) {
      ARROW_RETURN_NOT_OK(FinishPart(part, pool));
    }
    return arrow::Status::OK();
  }
};

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
  // VARCHAR, and only when every data page of the column chunk is dictionary-encoded: the chunk's
  // distinct values, and one index into them per row. What they are for is evaluating a predicate
  // once per distinct value instead of once per row
  // ([ADR 0020](../../../../../docs/adr/0020-filter-pushdown.md)).
  //
  // Empty when the chunk has a plain-encoded data page, which a writer produces once its
  // dictionary outgrows its page limit. A NULL row's index is not meaningful -- it has no value --
  // so `validity` decides a row before its index is read.
  //
  // **A VARCHAR column arrives in one of two shapes, and a reader has to accept both.** Either
  // `strings` holds one view per row, with these two filled as well where the encoding allows it;
  // or `strings` is **empty** and these two are the only values there are. The second shape is
  // what a column nothing above the scan reads looks like: its views exist only to be compared, so
  // where the dictionary can answer the predicate the table does not spend a write a row on them.
  // A table produces that shape only when `dictionary` is no larger than `rows`, which is the
  // condition under which a predicate is known to take the dictionary; a reader that would fall
  // back to the per-row path for any other reason must not be given it.
  std::span<const std::string_view> dictionary;
  std::span<const std::int32_t> indices;
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

  // Called once per table in a ScanContext, before any of its readers use the returned state.
  // Tables without shared scan preparation need no state. This creates state only, without
  // reading a file: a pruned part must not open or validate its input.
  virtual std::shared_ptr<TableScanState> MakeScanState() const { return nullptr; }

  // Scans the given top-level fields (indices into schema()) in order; batches have at most
  // batch_size rows and columns typed as in schema(). Their buffers are allocated from `pool`
  // (the query's memory budget).
  arrow::Result<std::unique_ptr<arrow::RecordBatchReader>> Scan(
      const std::vector<int>& fields, int64_t batch_size,
      arrow::MemoryPool* pool = arrow::default_memory_pool(),
      const std::shared_ptr<ScanContext>& context = nullptr) const {
    return DoScan(fields, batch_size, pool, context);
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
      arrow::MemoryPool* pool = arrow::default_memory_pool(),
      const std::shared_ptr<ScanContext>& context = nullptr) const {
    return DoScanPart(part, fields, batch_size, pool, context);
  }

  // Preserve the original explicit-null-filter call when the context overload is also viable.
  arrow::Result<std::unique_ptr<arrow::RecordBatchReader>> ScanPart(int64_t part,
                                                                    const std::vector<int>& fields,
                                                                    int64_t batch_size,
                                                                    arrow::MemoryPool* pool,
                                                                    std::nullptr_t) const {
    return DoScanPart(part, fields, batch_size, pool, nullptr);
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
  //
  // `returned` is how many of `fields`, from the first, the batches carry; the rest are read and
  // filtered on and **never put in a batch**. std::nullopt (the default) returns every field.
  // A column a predicate reads and nothing above the scan does is otherwise copied into every
  // batch for no reader at all, which on a VARCHAR column is the greater part of the scan's work
  // ([ADR 0020](../../../../../docs/adr/0020-filter-pushdown.md)); the caller knows which those
  // are and a table cannot. Invalid for a `returned` larger than `fields` or negative, and the
  // filter's columns may name any field, returned or not. The `position` column of `positions`
  // follows the returned ones.
  arrow::Result<std::unique_ptr<arrow::RecordBatchReader>> ScanPart(
      int64_t part, const std::vector<int>& fields, int64_t batch_size, arrow::MemoryPool* pool,
      const std::shared_ptr<const ScanFilter>& filter, bool positions = false,
      std::optional<int> returned = std::nullopt,
      const std::shared_ptr<ScanContext>& context = nullptr) const {
    if (returned.has_value() && (*returned < 0 || std::cmp_greater(*returned, fields.size()))) {
      return arrow::Status::Invalid("a scan returns ", *returned, " of ", fields.size(), " fields");
    }
    if (filter == nullptr) {
      if (positions) {
        return arrow::Status::Invalid("a scan reports row positions only with a filter");
      }
      if (returned.has_value() && std::cmp_less(*returned, fields.size())) {
        return arrow::Status::Invalid("a scan reads a field it does not return only to filter on");
      }
      return DoScanPart(part, fields, batch_size, pool, context);
    }
    return DoScanPartFiltered(part, fields, batch_size, pool, filter, positions,
                              returned.value_or(static_cast<int>(fields.size())), context);
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
      const std::vector<int>& fields, int64_t batch_size, arrow::MemoryPool* pool,
      const std::shared_ptr<ScanContext>& context) const = 0;
  virtual arrow::Result<std::unique_ptr<arrow::RecordBatchReader>> DoScanPart(
      int64_t part, const std::vector<int>& fields, int64_t batch_size, arrow::MemoryPool* pool,
      const std::shared_ptr<ScanContext>& context) const {
    if (part != 0) {
      return arrow::Status::Invalid("scan of part ", part, " of a table with 1 part");
    }
    return DoScan(fields, batch_size, pool, context);
  }
  // ScanPart with a filter, for tables that support it.
  virtual arrow::Result<std::unique_ptr<arrow::RecordBatchReader>> DoScanPartFiltered(
      int64_t /*part*/, const std::vector<int>& /*fields*/, int64_t /*batch_size*/,
      arrow::MemoryPool* /*pool*/, const std::shared_ptr<const ScanFilter>& /*filter*/,
      bool /*positions*/, int /*returned*/, const std::shared_ptr<ScanContext>& /*context*/) const {
    return arrow::Status::NotImplemented("this table cannot filter while it scans");
  }
};

// A fresh context belongs to one execution, shared by its ordinary, filtered and late scans.
// Without a context each Table scan performs its own preparation. A context must not be reused
// for another execution: external files are validated again even when a session reuses tables.
// Call Finish after closing every reader, before exposing a successful result. Shared preparation
// may use cheaper checks between its first validation and this final validation.
// Tables referenced here must outlive the context; their operators already own them during a run.
class ScanContext {
 public:
  // A runner must finish/join every task before returning, including on failure. It must preserve
  // part-order errors and use the supplied pool. An empty runner validates serially.
  using FinishRunner = std::function<arrow::Status(TableScanState&, arrow::MemoryPool*)>;

  arrow::Result<std::shared_ptr<TableScanState>> Get(const Table& table) {
    try {
      const std::scoped_lock lock(mutex_);
      const auto found = states_.find(&table);
      if (found != states_.end()) {
        return found->second;
      }
      auto state = table.MakeScanState();
      states_.emplace(&table, state);
      return state;
    } catch (const std::bad_alloc&) {
      return arrow::Status::OutOfMemory("out of memory preparing a table scan");
    }
  }

  arrow::Status Finish(arrow::MemoryPool* pool, const FinishRunner& runner = {}) {
    try {
      std::vector<std::shared_ptr<TableScanState>> states;
      {
        const std::scoped_lock lock(mutex_);
        states.reserve(states_.size());
        for (const auto& [table, state] : states_) {
          static_cast<void>(table);
          if (state != nullptr) {
            states.push_back(state);
          }
        }
      }
      for (const auto& state : states) {
        ARROW_RETURN_NOT_OK(runner ? runner(*state, pool) : state->Finish(pool));
      }
      return arrow::Status::OK();
    } catch (const std::bad_alloc&) {
      return arrow::Status::OutOfMemory("out of memory validating completed table scans");
    }
  }

 private:
  std::mutex mutex_;
  std::unordered_map<const Table*, std::shared_ptr<TableScanState>> states_;
};

}  // namespace antb1::plan
