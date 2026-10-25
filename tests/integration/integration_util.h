#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <arrow/api.h>

#include "antb1/engine/session.h"
#include "antb1/exec/profile.h"

#include "fixtures.h"

// Helpers of the integration tests (tests/integration): engine::Session end to end over the
// Parquet fixtures of tools/fixturegen.

namespace antb1::integration {

inline std::string Fixture(std::string_view relative) {
  return (antb1::testing::FixturesDir() / relative).string();
}

// A new session (optionally with the ClickBench EventDate override).
inline std::unique_ptr<engine::Session> NewSession(bool clickbench = false) {
  engine::SessionOptions options;
  if (clickbench) {
    options.default_overrides.emplace_back("EventDate", plan::LogicalType::kDate);
  }
  auto session = engine::Session::Make(options);
  return session.ok() ? std::move(*session) : nullptr;
}

// The single BIGINT value of a COUNT(*) query.
inline arrow::Result<int64_t> Count(engine::Session& session, const std::string& sql) {
  ARROW_ASSIGN_OR_RAISE(auto result, session.Execute(sql));
  ARROW_ASSIGN_OR_RAISE(auto table, result.table->CombineChunks());
  if (table->num_rows() != 1 || table->num_columns() != 1 ||
      table->column(0)->type()->id() != arrow::Type::INT64) {
    return arrow::Status::Invalid("not a single BIGINT: ", table->ToString());
  }
  return std::static_pointer_cast<arrow::Int64Array>(table->column(0)->chunk(0))->Value(0);
}

inline std::string FromPath(const std::string& path) {
  return "SELECT COUNT(*) FROM '" + path + "'";
}

// The first node named `name` of `node`'s tree (depth first); nullptr if none.
inline const exec::ProfileNode* FindNode(const exec::ProfileNode& node, const std::string& name) {
  if (node.name() == name) {
    return &node;
  }
  for (const exec::ProfileNode* child : node.children()) {
    if (const exec::ProfileNode* found = FindNode(*child, name)) {
      return found;
    }
  }
  return nullptr;
}

// Metric `name` of `node`, if it has one. What a metric is for is a property of a run that no
// result shows, so a test that wants one of those asks here.
inline std::optional<int64_t> MetricOf(const exec::ProfileNode& node, const std::string& name) {
  for (const exec::ProfileMetric& metric : node.metrics()) {
    if (metric.name == name) {
      return metric.value;
    }
  }
  return std::nullopt;
}

}  // namespace antb1::integration
