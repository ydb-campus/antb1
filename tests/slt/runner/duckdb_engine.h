#pragma once

#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "engine.h"
#include "tables.h"

namespace antb1::slt {

// The DuckDB oracle (C API, DUCKDB_API_NO_DEPRECATED, results via duckdb_fetch_chunk), locked down:
// threads=1, no extension autoinstall/autoload, no file access outside the fixtures directory, temp
// files under temp_dir, then lock_configuration=true. Execute() runs one statement per call, and
// only SELECT, EXPLAIN, SET and LOAD: anything else (COPY, ATTACH, EXPORT, DDL, DML) is refused
// with a "Permission Error: ..." before it runs, so no record can write next to the shared fixtures
// or change the oracle's state. Every table is a view
//   CREATE VIEW <name> AS SELECT * [REPLACE (make_date(EventDate) AS EventDate)]
//     FROM read_parquet([<files>], binary_as_string=true)
// and binary_as_string is also on for `FROM '<path>'`. Values are converted to the canonical text.
class DuckDbEngine final : public Engine {
 public:
  static std::expected<std::unique_ptr<DuckDbEngine>, std::string> Make(
      const std::vector<TableDef>& tables, const std::filesystem::path& fixtures_dir,
      const std::filesystem::path& temp_dir);

  DuckDbEngine(const DuckDbEngine&) = delete;
  DuckDbEngine& operator=(const DuckDbEngine&) = delete;
  DuckDbEngine(DuckDbEngine&&) = delete;
  DuckDbEngine& operator=(DuckDbEngine&&) = delete;
  ~DuckDbEngine() override;

  [[nodiscard]] std::string_view name() const override { return "duckdb"; }
  ExecResult Execute(const std::string& sql) override;

  // The DuckDB library version, e.g. "v1.5.5".
  static std::string Version();

 private:
  struct Handles;
  explicit DuckDbEngine(std::unique_ptr<Handles> handles);

  std::unique_ptr<Handles> handles_;
};

}  // namespace antb1::slt
