#include "antb1_engine.h"

#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <arrow/api.h>

#include "antb1/engine/format.h"
#include "antb1/engine/session.h"
#include "antb1/plan/sql_status.h"
#include "antb1/plan/types.h"

#include "engine.h"
#include "tables.h"

namespace antb1::slt {
namespace {

// The same kinds as the CLI's error report (src/cli/cli.cc) and exit codes.
EngineError FromStatus(const arrow::Status& status) {
  EngineError error;
  if (const auto detail = plan::GetSqlError(status)) {
    switch (detail->kind()) {
      case plan::SqlErrorDetail::Kind::kParse:
        error.kind = "parse";
        break;
      case plan::SqlErrorDetail::Kind::kUnsupported:
        error.kind = "unsupported";
        error.unsupported = true;
        break;
      case plan::SqlErrorDetail::Kind::kBind:
        error.kind = "bind";
        break;
    }
  } else if (status.IsIOError()) {
    error.kind = "io";
  } else if (status.IsExecutionError() || status.IsInvalid()) {
    error.kind = "execution";
  } else {
    error.kind = "internal";
    error.internal = true;
  }
  error.message = std::format("{}: {}", error.kind, status.message());
  return error;
}

ColumnClass ClassOf(plan::LogicalType type) {
  switch (type) {
    case plan::LogicalType::kSmallInt:
    case plan::LogicalType::kInteger:
    case plan::LogicalType::kBigInt:
    case plan::LogicalType::kUSmallInt:
    case plan::LogicalType::kHugeInt:
      return ColumnClass::kInteger;
    case plan::LogicalType::kDouble:
      return ColumnClass::kReal;
    case plan::LogicalType::kVarchar:
    case plan::LogicalType::kDate:
      return ColumnClass::kText;
  }
  return ColumnClass::kText;
}

arrow::Result<ResultSet> ToResultSet(const engine::QueryResult& result) {
  ResultSet out;
  for (const auto type : result.types) {
    out.classes.push_back(ClassOf(type));
    out.type_names.emplace_back(plan::ToString(type));
  }
  ARROW_ASSIGN_OR_RAISE(auto table, result.table->CombineChunks());
  if (std::cmp_not_equal(table->num_columns(), result.types.size())) {
    return arrow::Status::Invalid("result has ", table->num_columns(), " columns but ",
                                  result.types.size(), " types");
  }
  for (int64_t r = 0; r < table->num_rows(); ++r) {
    std::vector<std::optional<std::string>> row;
    for (int c = 0; c < table->num_columns(); ++c) {
      const auto& column = *table->column(c)->chunk(0);
      if (column.IsNull(r)) {
        row.emplace_back(std::nullopt);
      } else {
        row.emplace_back(engine::FormatValue(column, r, result.types[static_cast<std::size_t>(c)]));
      }
    }
    out.rows.push_back(std::move(row));
  }
  return out;
}

}  // namespace

std::expected<std::unique_ptr<Antb1Engine>, std::string> Antb1Engine::Make(
    const std::vector<TableDef>& tables, engine::SessionOptions options) {
  bool any_clickbench = false;
  for (const auto& t : tables) {
    any_clickbench = any_clickbench || t.clickbench;
  }
  if (any_clickbench) {
    options.default_overrides.emplace_back("EventDate", plan::LogicalType::kDate);
  }
  auto session = engine::Session::Make(std::move(options));
  if (!session.ok()) {
    return std::unexpected(session.status().ToString());
  }
  for (const auto& t : tables) {
    if (auto status = (*session)->RegisterParquet(t.name, t.files); !status.ok()) {
      return std::unexpected(
          std::format("antb1: cannot register table '{}': {}", t.name, status.ToString()));
    }
    const auto table = (*session)->catalog().Find(t.name);
    const bool has_event_date =
        table != nullptr && table->schema()->GetFieldIndex("EventDate") >= 0;
    if (any_clickbench && has_event_date && !t.clickbench) {
      return std::unexpected(
          std::format("table '{}' has an EventDate column but no `clickbench` option, while other "
                      "tables have it; "
                      "engine::Session only supports session-wide column overrides",
                      t.name));
    }
  }
  return std::unique_ptr<Antb1Engine>(new Antb1Engine(std::move(*session)));
}

ExecResult Antb1Engine::Execute(const std::string& sql) {
  auto result = session_->Execute(sql);
  if (!result.ok()) {
    return std::unexpected(FromStatus(result.status()));
  }
  auto rows = ToResultSet(*result);
  if (!rows.ok()) {
    return std::unexpected(EngineError{.kind = "internal",
                                       .message = "internal: " + rows.status().ToString(),
                                       .unsupported = false,
                                       .internal = true});
  }
  return std::move(*rows);
}

}  // namespace antb1::slt
