#pragma once

#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <arrow/result.h>

#include "antb1/engine/session.h"

#include "engine.h"
#include "tables.h"

namespace antb1::slt {

// The rows of an antb1 result as canonical text (engine::FormatValue), NULL as std::nullopt.
// Handles columns of any chunk layout.
arrow::Result<ResultSet> ToResultSet(const engine::QueryResult& result);

// antb1 through engine::Session. Values are formatted with engine::FormatValue (the canonical
// text). Session-wide column overrides are the only mechanism the engine offers today, so the
// `clickbench` option applies to every table that has an EventDate column: all such tables must
// agree on it. `options` sets everything else (e.g. batch_size for the metamorphic tests).
class Antb1Engine final : public Engine {
 public:
  static std::expected<std::unique_ptr<Antb1Engine>, std::string> Make(
      const std::vector<TableDef>& tables, engine::SessionOptions options = {});

  [[nodiscard]] std::string_view name() const override { return "antb1"; }
  ExecResult Execute(const std::string& sql) override;

 private:
  explicit Antb1Engine(std::unique_ptr<engine::Session> session) : session_(std::move(session)) {}

  std::unique_ptr<engine::Session> session_;
};

}  // namespace antb1::slt
