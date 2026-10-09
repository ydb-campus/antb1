#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <arrow/api.h>

#include "antb1/common/narrow.h"
#include "antb1/plan/binder.h"
#include "antb1/plan/catalog.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/sql_status.h"
#include "antb1/plan/table.h"
#include "antb1/sql/parser.h"

// In-memory tables and helpers shared by the plan unit tests.

namespace antb1::plan {

// gtest prints a column id as EXPLAIN-like text: #7.
inline void PrintTo(ColumnId id, std::ostream* os) { *os << '#' << std::to_underlying(id); }

}  // namespace antb1::plan

namespace antb1::plan::testing {

// A table with a schema, an optional exact row count and the fields stored as FLOAT; it cannot be
// scanned. By default it is one part without statistics or distinct-count hints; WithParts,
// WithStats and WithDistinctCount give it parts and their footer statistics.
class FakeTable final : public Table {
 public:
  FakeTable(std::shared_ptr<arrow::Schema> schema, std::optional<int64_t> rows,
            std::vector<int> float_fields = {})
      : schema_(std::move(schema)), rows_(rows), float_fields_(std::move(float_fields)) {}

  // Parts of these rows (the exact row count stays as constructed).
  FakeTable& WithParts(std::vector<int64_t> rows) {
    parts_ = std::move(rows);
    return *this;
  }
  // part_stats(part, field).
  FakeTable& WithStats(int64_t part, int field, PartStats stats) {
    stats_.insert_or_assign({part, field}, stats);
    return *this;
  }
  // part_distinct_count(part, field).
  FakeTable& WithDistinctCount(int64_t part, int field, int64_t count) {
    distinct_counts_.insert_or_assign({part, field}, count);
    return *this;
  }

  const std::shared_ptr<arrow::Schema>& schema() const override { return schema_; }
  std::optional<int64_t> exact_row_count() const override { return rows_; }
  int64_t num_parts() const override {
    return parts_.has_value() ? std::ssize(*parts_) : Table::num_parts();
  }
  std::optional<int64_t> part_rows(int64_t part) const override {
    if (!parts_.has_value()) {
      return Table::part_rows(part);
    }
    return part >= 0 && part < std::ssize(*parts_)
               ? std::optional((*parts_)[Narrow<std::size_t>(part)])
               : std::nullopt;
  }
  std::optional<PartStats> part_stats(int64_t part, int field) const override {
    const auto it = stats_.find({part, field});
    return it != stats_.end() ? std::optional(it->second) : std::nullopt;
  }
  std::optional<int64_t> part_distinct_count(int64_t part, int field) const override {
    const auto it = distinct_counts_.find({part, field});
    return it != distinct_counts_.end() ? std::optional(it->second) : std::nullopt;
  }
  bool StoredAsFloat(int field) const override {
    return std::ranges::contains(float_fields_, field);
  }
  std::string Describe() const override { return "fake"; }

 protected:
  arrow::Result<std::unique_ptr<arrow::RecordBatchReader>> DoScan(
      const std::vector<int>& /*fields*/, int64_t /*batch_size*/,
      arrow::MemoryPool* /*pool*/) const override {
    return arrow::Status::NotImplemented("fake tables cannot be scanned");
  }

 private:
  std::shared_ptr<arrow::Schema> schema_;
  std::optional<int64_t> rows_;
  std::vector<int> float_fields_;
  std::optional<std::vector<int64_t>> parts_;
  std::map<std::pair<int64_t, int>, PartStats> stats_;
  std::map<std::pair<int64_t, int>, int64_t> distinct_counts_;
};

// Every engine type, an unsupported column and two names that need quoting:
//   i16 SMALLINT, i32 INTEGER, i64 BIGINT, u16 USMALLINT, h DECIMAL(38,0), d DOUBLE, s VARCHAR,
//   dt DATE, bad list<int32>, "Mixed Case" INTEGER, "from" INTEGER.
inline std::shared_ptr<arrow::Schema> AllTypesSchema() {
  return arrow::schema({
      arrow::field("i16", arrow::int16()),
      arrow::field("i32", arrow::int32()),
      arrow::field("i64", arrow::int64()),
      arrow::field("u16", arrow::uint16()),
      arrow::field("h", arrow::decimal128(38, 0)),
      arrow::field("d", arrow::float64()),
      arrow::field("s", arrow::binary()),
      arrow::field("dt", arrow::date32()),
      arrow::field("bad", arrow::list(arrow::int32())),
      arrow::field("Mixed Case", arrow::int32()),
      arrow::field("from", arrow::int32()),
  });
}

// A catalog with "t" (AllTypesSchema, 100 rows), "u" (the same schema, unknown row count),
// "dup" (columns "a" and "A"), "ok" (i16 and s only) and "dec" (DECIMALs: p DECIMAL(15,2),
// q DECIMAL(15,2), r DECIMAL(9,4), z DECIMAL(38,10), i INTEGER, s16 SMALLINT, u16 USMALLINT,
// b BIGINT, e DECIMAL(18,8), g DECIMAL(18,10), f DOUBLE).
inline Catalog MakeCatalog() {
  Catalog catalog;
  const auto must = [](const arrow::Status& status) {
    if (!status.ok()) {
      std::abort();
    }
  };
  must(catalog.Register("t", std::make_shared<FakeTable>(AllTypesSchema(), 100)));
  must(catalog.Register("u", std::make_shared<FakeTable>(AllTypesSchema(), std::nullopt)));
  must(catalog.Register(
      "dup", std::make_shared<FakeTable>(arrow::schema({arrow::field("a", arrow::int32()),
                                                        arrow::field("A", arrow::int64())}),
                                         3)));
  must(catalog.Register(
      "ok", std::make_shared<FakeTable>(arrow::schema({arrow::field("i16", arrow::int16()),
                                                       arrow::field("s", arrow::binary())}),
                                        7)));
  must(catalog.Register(
      "dec",
      std::make_shared<FakeTable>(
          arrow::schema(
              {arrow::field("p", arrow::decimal128(15, 2)),
               arrow::field("q", arrow::decimal128(15, 2)),
               arrow::field("r", arrow::decimal128(9, 4)),
               arrow::field("z", arrow::decimal128(38, 10)), arrow::field("i", arrow::int32()),
               arrow::field("s16", arrow::int16()), arrow::field("u16", arrow::uint16()),
               arrow::field("b", arrow::int64()), arrow::field("e", arrow::decimal128(18, 8)),
               arrow::field("g", arrow::decimal128(18, 10)), arrow::field("f", arrow::float64())}),
          10)));
  return catalog;
}

inline arrow::Result<LogicalPlan> BindSql(std::string_view sql, const Catalog& catalog) {
  auto stmt = sql::Parse(sql);
  if (!stmt) {
    return ToArrowStatus(stmt.error());
  }
  return Bind(*stmt, catalog);
}

// The query text an error's span points at ("" without a SqlErrorDetail).
inline std::string SpanText(std::string_view sql, const arrow::Status& status) {
  const auto detail = GetSqlError(status);
  if (detail == nullptr) {
    return "";
  }
  return std::string(sql.substr(detail->span().offset, detail->span().length));
}

// Walks single-input nodes from the root: Nth(plan, 0) is the root.
inline const LogicalNode& Nth(const LogicalPlan& plan, int depth) {
  const LogicalNode* node = plan.root.get();
  for (int i = 0; i < depth; ++i) {
    const std::vector<LogicalNodePtr> inputs = InputsOf(*node);
    if (inputs.size() != 1 || inputs[0] == nullptr) {
      std::abort();
    }
    node = inputs[0].get();
  }
  return *node;
}

// Walks single-input nodes from `node`: Down(node, 0) is `node` itself, and nullptr once a node
// has no input or two (a Join), so a test says where it stopped instead of aborting.
inline const LogicalNode* Down(const LogicalNode& node, int depth) {
  const LogicalNode* at = &node;
  for (int i = 0; i < depth && at != nullptr; ++i) {
    const std::vector<LogicalNodePtr> inputs = InputsOf(*at);
    at = inputs.size() == 1 ? inputs[0].get() : nullptr;
  }
  return at;
}

// The outermost Join of a plan, reached through single-input nodes; nullptr if there is none.
inline const JoinNode* FirstJoin(const LogicalPlan& plan) {
  for (const LogicalNode* node = plan.root.get(); node != nullptr;) {
    if (const auto* join = std::get_if<JoinNode>(node)) {
      return join;
    }
    const std::vector<LogicalNodePtr> inputs = InputsOf(*node);
    node = inputs.size() == 1 ? inputs[0].get() : nullptr;
  }
  return nullptr;
}

// The table a branch scans, through its single-input nodes: "" if it reaches no Scan.
inline std::string ScannedTable(const LogicalNode& node) {
  for (const LogicalNode* at = &node; at != nullptr;) {
    if (const auto* scan = std::get_if<ScanNode>(at)) {
      return scan->table_name;
    }
    const std::vector<LogicalNodePtr> inputs = InputsOf(*at);
    at = inputs.size() == 1 ? inputs[0].get() : nullptr;
  }
  return "";
}

}  // namespace antb1::plan::testing
