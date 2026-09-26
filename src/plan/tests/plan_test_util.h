#pragma once

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
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

namespace antb1::plan::testing {

// A table with a schema and an optional exact row count; it cannot be scanned.
class FakeTable final : public Table {
 public:
  FakeTable(std::shared_ptr<arrow::Schema> schema, std::optional<int64_t> rows)
      : schema_(std::move(schema)), rows_(rows) {}

  const std::shared_ptr<arrow::Schema>& schema() const override { return schema_; }
  std::optional<int64_t> exact_row_count() const override { return rows_; }
  arrow::Result<std::unique_ptr<arrow::RecordBatchReader>> Scan(
      const std::vector<int>& /*fields*/, int64_t /*batch_size*/) const override {
    return arrow::Status::NotImplemented("fake tables cannot be scanned");
  }
  std::string Describe() const override { return "fake"; }

 private:
  std::shared_ptr<arrow::Schema> schema_;
  std::optional<int64_t> rows_;
};

// Every engine type, an unsupported column and two names that need quoting:
//   i16 SMALLINT, i32 INTEGER, i64 BIGINT, u16 USMALLINT, h HUGEINT, d DOUBLE, s VARCHAR,
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
// "dup" (columns "a" and "A") and "ok" (i16 and s only).
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
    const LogicalNodePtr* input = InputOf(*node);
    if (input == nullptr || *input == nullptr) {
      std::abort();
    }
    node = input->get();
  }
  return *node;
}

}  // namespace antb1::plan::testing
