#include "antb1/engine/session.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <arrow/api.h>
#include <arrow/compute/api.h>

#include "antb1/exec/operator.h"
#include "antb1/exec/physical_planner.h"
#include "antb1/io/parquet_table.h"
#include "antb1/plan/binder.h"
#include "antb1/plan/catalog.h"
#include "antb1/plan/explain.h"
#include "antb1/plan/optimizer.h"
#include "antb1/plan/sql_status.h"
#include "antb1/sql/parser.h"

namespace antb1::engine {
namespace {

using Clock = std::chrono::steady_clock;

io::ParquetTableOptions TableOptions(const SessionOptions& options,
                                     const std::shared_ptr<io::ParquetTable>& probe) {
  io::ParquetTableOptions table_options;
  for (const auto& [column, type] : options.default_overrides) {
    // Only apply session-wide overrides to tables that have the column (ASCII case-insensitive).
    const std::string wanted = plan::AsciiLower(column);
    const bool present =
        probe && std::ranges::any_of(probe->schema()->fields(), [&](const auto& field) {
          return plan::AsciiLower(field->name()) == wanted;
        });
    if (present) {
      table_options.overrides.push_back(io::ColumnOverride{.column = column, .type = type});
    }
  }
  return table_options;
}

arrow::Result<std::shared_ptr<plan::Table>> OpenParquet(const SessionOptions& options,
                                                        const std::vector<std::string>& paths) {
  ARROW_ASSIGN_OR_RAISE(auto probe, io::ParquetTable::Open(paths));
  auto table_options = TableOptions(options, probe);
  if (table_options.overrides.empty()) {
    return probe;
  }
  ARROW_ASSIGN_OR_RAISE(auto table, io::ParquetTable::Open(paths, table_options));
  return table;
}

arrow::Result<plan::LogicalPlan> ParseAndBind(std::string_view sql, const plan::Catalog& catalog,
                                              QueryTimings* timings) {
  const auto t0 = Clock::now();
  auto stmt = sql::Parse(sql);
  const auto t1 = Clock::now();
  if (!stmt) {
    return plan::ToArrowStatus(stmt.error());
  }
  auto bound = plan::Bind(*stmt, catalog);
  if (bound.ok()) {
    *bound = plan::Optimize(*bound);
  }
  if (timings != nullptr) {
    timings->parse = t1 - t0;
    timings->bind = Clock::now() - t1;  // bind and optimize
  }
  return bound;
}

}  // namespace

Session::Session(SessionOptions options)
    : options_(std::move(options)),
      catalog_([opts = options_](const std::string& path) { return OpenParquet(opts, {path}); }) {}

arrow::Result<std::unique_ptr<Session>> Session::Make(SessionOptions options) {
  ARROW_RETURN_NOT_OK(arrow::compute::Initialize());
  return std::unique_ptr<Session>(new Session(std::move(options)));
}

arrow::Status Session::RegisterParquet(const std::string& name,
                                       const std::vector<std::string>& paths) {
  ARROW_ASSIGN_OR_RAISE(auto table, OpenParquet(options_, paths));
  return catalog_.Register(name, table);
}

arrow::Result<QueryResult> Session::Execute(std::string_view sql) {
  QueryResult result;
  ARROW_ASSIGN_OR_RAISE(auto logical, ParseAndBind(sql, catalog_, &result.timings));
  const auto t0 = Clock::now();
  ARROW_ASSIGN_OR_RAISE(auto op, exec::BuildPhysicalPlan(logical));
  exec::ExecContext ctx{.pool = arrow::default_memory_pool(), .batch_size = options_.batch_size};
  ARROW_ASSIGN_OR_RAISE(auto table, exec::Drain(*op, ctx));
  for (const auto& col : logical.output) {
    result.names.push_back(col.name);
    result.types.push_back(col.type);
  }
  ARROW_ASSIGN_OR_RAISE(result.table, table->RenameColumns(result.names));
  result.timings.execute = Clock::now() - t0;
  return result;
}

arrow::Result<std::string> Session::Explain(std::string_view sql) {
  ARROW_ASSIGN_OR_RAISE(auto logical, ParseAndBind(sql, catalog_, nullptr));
  return plan::Explain(logical);
}

}  // namespace antb1::engine
