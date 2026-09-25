#pragma once

#include <arrow/result.h>

#include "antb1/plan/catalog.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/sql/ast.h"

namespace antb1::plan {

// Resolves names and types against the catalog and builds the logical plan
//
//   [Limit] <- Project | Aggregate <- [Filter] <- Scan (every field of the table)
//
// with every WHERE literal folded exactly into the type of its column (docs/sql-subset.md). The
// plan is not optimized yet (see plan::Optimize). Errors carry a SqlErrorDetail with the span of
// the offending AST node: kBind for SQL that is wrong for the table (unknown or ambiguous column,
// aggregates mixed with columns, SUM/AVG of a non-number, a literal of the wrong type),
// kUnsupported for a column of an unsupported type or FROM 'path' without a path opener.
arrow::Result<LogicalPlan> Bind(const sql::SelectStatement& stmt, const Catalog& catalog);

}  // namespace antb1::plan
