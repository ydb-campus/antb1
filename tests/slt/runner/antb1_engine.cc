#include "antb1_engine.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
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
    case plan::LogicalType::kTimestamp:
    case plan::LogicalType::kBoolean:  // never a result column
      return ColumnClass::kText;
  }
  return ColumnClass::kText;
}

}  // namespace

arrow::Result<ResultSet> ToResultSet(const engine::QueryResult& result) {
  const arrow::Table& table = *result.table;
  if (std::cmp_not_equal(table.num_columns(), result.types.size())) {
    return arrow::Status::Invalid("result has ", table.num_columns(), " columns but ",
                                  result.types.size(), " types");
  }
  ResultSet out;
  for (const auto type : result.types) {
    out.classes.push_back(ClassOf(type));
    out.type_names.emplace_back(plan::ToString(type));
  }
  const auto nrows = static_cast<std::size_t>(table.num_rows());
  out.rows.assign(nrows, std::vector<std::optional<std::string>>(result.types.size()));
  // Chunk by chunk: CombineChunks keeps a binary column over 2 GiB in several chunks.
  for (int c = 0; c < table.num_columns(); ++c) {
    const auto uc = static_cast<std::size_t>(c);
    const auto& column = *table.column(c);
    if (column.length() != table.num_rows()) {
      return arrow::Status::Invalid("column ", c, " has ", column.length(),
                                    " rows but the result has ", table.num_rows());
    }
    std::size_t row = 0;
    for (const auto& chunk : column.chunks()) {
      for (int64_t i = 0; i < chunk->length(); ++i, ++row) {
        if (!chunk->IsNull(i)) {
          out.rows[row][uc] = engine::FormatValue(*chunk, i, result.types[uc]);
        }
      }
    }
  }
  return out;
}

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

namespace {

class SameResultEngine final : public Engine {
 public:
  SameResultEngine(std::unique_ptr<Engine> engine, std::unique_ptr<Engine> reference,
                   std::string reference_name)
      : engine_(std::move(engine)),
        reference_(std::move(reference)),
        reference_name_(std::move(reference_name)) {}

  [[nodiscard]] std::string_view name() const override { return engine_->name(); }

  ExecResult Execute(const std::string& sql) override {
    ExecResult result = engine_->Execute(sql);
    const ExecResult reference = reference_->Execute(sql);
    const std::optional<std::string> difference = Difference(result, reference);
    if (!difference.has_value()) {
      return result;
    }
    return std::unexpected(EngineError{
        .kind = "internal",
        .message = std::format("internal: the result differs from the result with {}: {}",
                               reference_name_, *difference),
        .unsupported = false,
        .internal = true});
  }

 private:
  // Where two results differ (no values), or std::nullopt if they are identical.
  static std::optional<std::string> Difference(const ExecResult& a, const ExecResult& b) {
    if (a.has_value() != b.has_value()) {
      return a.has_value() ? "only the reference failed" : "only this run failed";
    }
    if (!a.has_value()) {
      if (a.error().kind != b.error().kind || a.error().message != b.error().message) {
        return std::format("other errors ({} vs {})", a.error().kind, b.error().kind);
      }
      return std::nullopt;
    }
    if (a->classes != b->classes || a->type_names != b->type_names) {
      return "other column types";
    }
    if (a->rows.size() != b->rows.size()) {
      return std::format("{} rows vs {}", a->rows.size(), b->rows.size());
    }
    for (std::size_t row = 0; row < a->rows.size(); ++row) {
      if (a->rows[row] != b->rows[row]) {
        return std::format("row {} differs", row);
      }
    }
    return std::nullopt;
  }

  std::unique_ptr<Engine> engine_;
  std::unique_ptr<Engine> reference_;
  std::string reference_name_;
};

}  // namespace

std::unique_ptr<Engine> MakeSameResultEngine(std::unique_ptr<Engine> engine,
                                             std::unique_ptr<Engine> reference,
                                             std::string reference_name) {
  return std::make_unique<SameResultEngine>(std::move(engine), std::move(reference),
                                            std::move(reference_name));
}

}  // namespace antb1::slt
