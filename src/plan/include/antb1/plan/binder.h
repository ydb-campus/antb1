#pragma once

#include <arrow/result.h>

#include "antb1/plan/catalog.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/sql/ast.h"

namespace antb1::plan {

// Resolves names and types against the catalog and builds the logical plan. Errors carry a
// SqlErrorDetail (kBind or kUnsupported) with the span of the offending AST node.
arrow::Result<LogicalPlan> Bind(const sql::SelectStatement& stmt, const Catalog& catalog);

}  // namespace antb1::plan
