#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine.h"
#include "tables.h"

namespace antb1::slt {

// The DuckDB oracle (C API, DUCKDB_API_NO_DEPRECATED, results via duckdb_fetch_chunk), locked down:
// threads=1, no extension autoinstall/autoload, no file access outside the fixtures directory, the
// directories of the table files and temp_dir (temp files), the late_materialization optimizer off
// (a DuckDB bug, duckdb_engine.cc), then lock_configuration=true. A memory_limit (bytes, at least
// 1) is DuckDB's memory_limit: beyond it DuckDB spills to temp_dir, and a query that still does not
// fit fails with an "Out of Memory Error"; without one, DuckDB's default applies (80% of the host's
// memory). Execute()
// runs one statement per call, and only SELECT, EXPLAIN, SET and LOAD: anything else (COPY, ATTACH,
// EXPORT, DDL, DML) is refused with a "Permission Error: ..." before it runs, so no record can
// write next to the shared fixtures or change the oracle's state. After a DuckDB internal error
// (IsDuckDbInternalError) Execute opens the database again, with the same setup: DuckDB
// invalidates a database whose query fails with one while it runs, and every later query would
// fail too. Every table is a view
//   CREATE VIEW <name> AS SELECT * [REPLACE (make_date(EventDate) AS EventDate)]
//     FROM read_parquet([<files>], binary_as_string=true)
// and binary_as_string is also on for `FROM '<path>'`. Values are converted to the canonical text.
class DuckDbEngine final : public Engine {
 public:
  // Which of DuckDB's optimizers run. The ones short of kAll make the oracles the differential test
  // asks where the oracle fails with an internal error (difftest.h).
  enum class Optimizers : std::uint8_t {
    kAll,          // every one but late_materialization (a DuckDB bug, duckdb_engine.cc)
    kNoJoinOrder,  // nor join_order: joins keep the order the query writes them in
    // filter_pushdown alone, which still makes a comma join's equalities hash joins, in the
    // written order: without it a comma join is a cross product
    kFilterPushdownOnly,
  };

  static std::expected<std::unique_ptr<DuckDbEngine>, std::string> Make(
      const std::vector<TableDef>& tables, const std::filesystem::path& fixtures_dir,
      const std::filesystem::path& temp_dir, std::optional<int64_t> memory_limit = std::nullopt,
      Optimizers optimizers = Optimizers::kAll);

  // The differential test's retries, in the order it asks them: kNoJoinOrder, then
  // kFilterPushdownOnly, each with a temp dir of its own under `temp_dir`.
  static std::expected<std::vector<std::unique_ptr<DuckDbEngine>>, std::string> MakeRetries(
      const std::vector<TableDef>& tables, const std::filesystem::path& fixtures_dir,
      const std::filesystem::path& temp_dir, std::optional<int64_t> memory_limit = std::nullopt);

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

  // One statement, checked as Execute's comment says.
  ExecResult Run(const std::string& sql);

  std::unique_ptr<Handles> handles_;
};

}  // namespace antb1::slt
