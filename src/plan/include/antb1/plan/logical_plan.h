#pragma once

#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "antb1/plan/table.h"
#include "antb1/plan/types.h"

// Logical plan: a tree of immutable nodes. New operators are added as new node structs in the
// LogicalNode variant; std::visit over it is exhaustive, so exec/explain fail to compile until
// handled.

namespace antb1::plan {

struct OutputColumn {
  std::string name;
  LogicalType type = LogicalType::kBigInt;
};

// COUNT(*) without WHERE, answered from table metadata (Table::exact_row_count()).
struct RowCountNode {
  std::shared_ptr<Table> table;
  std::string table_name;
};

using LogicalNode = std::variant<RowCountNode>;

struct LogicalPlan {
  std::shared_ptr<const LogicalNode> root;
  std::vector<OutputColumn> output;
};

}  // namespace antb1::plan
