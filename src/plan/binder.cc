#include "antb1/plan/binder.h"

#include <memory>
#include <string>
#include <variant>

#include <arrow/result.h>

#include "antb1/plan/sql_status.h"

namespace antb1::plan {
namespace {

arrow::Result<std::shared_ptr<Table>> ResolveTable(const sql::TableRef& ref,
                                                   const Catalog& catalog) {
  if (ref.kind == sql::TableRef::Kind::kPath) {
    auto table = catalog.OpenPath(ref.name);
    if (!table.ok()) {
      if (table.status().IsNotImplemented()) {
        return UnsupportedError(table.status().message(), ref.span);
      }
      return table.status();  // I/O errors keep their code (exit 3)
    }
    return table;
  }
  auto table = catalog.Find(ref.name);
  if (!table) {
    return BindError("table '" + ref.name + "' does not exist", ref.span);
  }
  return table;
}

}  // namespace

arrow::Result<LogicalPlan> Bind(const sql::SelectStatement& stmt, const Catalog& catalog) {
  ARROW_ASSIGN_OR_RAISE(auto table, ResolveTable(stmt.from, catalog));
  if (stmt.star || stmt.items.size() != 1 || !stmt.where.empty() || stmt.limit.has_value()) {
    return UnsupportedError("only SELECT COUNT(*) FROM <table> is supported yet", stmt.span);
  }
  const auto* agg = std::get_if<sql::AggregateCall>(&stmt.items[0].expr);
  if (agg == nullptr || agg->kind != sql::AggKind::kCountStar) {
    return UnsupportedError("only COUNT(*) is supported yet", stmt.items[0].span);
  }
  if (!table->exact_row_count().has_value()) {
    return UnsupportedError("COUNT(*) needs a table with a known row count", stmt.from.span);
  }
  LogicalPlan plan;
  plan.root = std::make_shared<const LogicalNode>(
      RowCountNode{.table = table, .table_name = stmt.from.name});
  plan.output.push_back(OutputColumn{.name = stmt.items[0].alias.value_or("count_star()"),
                                     .type = LogicalType::kBigInt});
  return plan;
}

}  // namespace antb1::plan
