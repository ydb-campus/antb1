#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <arrow/api.h>

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
// scanned.
class FakeTable final : public Table {
 public:
  FakeTable(std::shared_ptr<arrow::Schema> schema, std::optional<int64_t> rows,
            std::vector<int> float_fields = {})
      : schema_(std::move(schema)), rows_(rows), float_fields_(std::move(float_fields)) {}

  const std::shared_ptr<arrow::Schema>& schema() const override { return schema_; }
  std::optional<int64_t> exact_row_count() const override { return rows_; }
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
// q DECIMAL(15,2), r DECIMAL(9,4), z DECIMAL(38,10), i INTEGER).
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
  must(catalog.Register("dec", std::make_shared<FakeTable>(
                                   arrow::schema({arrow::field("p", arrow::decimal128(15, 2)),
                                                  arrow::field("q", arrow::decimal128(15, 2)),
                                                  arrow::field("r", arrow::decimal128(9, 4)),
                                                  arrow::field("z", arrow::decimal128(38, 10)),
                                                  arrow::field("i", arrow::int32())}),
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

}  // namespace antb1::plan::testing
