#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <arrow/result.h>
#include <arrow/status.h>
#include <arrow/table.h>

#include "antb1/plan/catalog.h"
#include "antb1/plan/types.h"

namespace antb1::engine {

struct SessionOptions {
  int64_t batch_size = int64_t{64} * 1024;
  // Column type overrides applied to every table opened by this session, e.g. {"EventDate", kDate}
  // for ClickBench (`antb1 --clickbench`).
  std::vector<std::pair<std::string, plan::LogicalType>> default_overrides;
};

struct QueryTimings {
  std::chrono::nanoseconds parse{0};
  std::chrono::nanoseconds bind{0};
  std::chrono::nanoseconds execute{0};
};

struct QueryResult {
  std::shared_ptr<arrow::Table> table;
  std::vector<std::string> names;
  std::vector<plan::LogicalType> types;
  QueryTimings timings;
};

// Entry point of the engine: owns the catalog, parses/binds/executes SQL. Not thread-safe.
class Session {
 public:
  // Calls arrow::compute::Initialize() (required since Arrow 21 for kernels such as "sum").
  static arrow::Result<std::unique_ptr<Session>> Make(SessionOptions options = {});

  // Registers a Parquet table over files/globs (see io::ParquetTable).
  arrow::Status RegisterParquet(const std::string& name, const std::vector<std::string>& paths);

  arrow::Result<QueryResult> Execute(std::string_view sql);
  arrow::Result<std::string> Explain(std::string_view sql);

  [[nodiscard]] const plan::Catalog& catalog() const { return catalog_; }

 private:
  explicit Session(SessionOptions options);

  SessionOptions options_;
  plan::Catalog catalog_;
};

}  // namespace antb1::engine
