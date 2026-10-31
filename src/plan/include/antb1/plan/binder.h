#pragma once

#include <arrow/result.h>

#include "antb1/plan/catalog.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/plan_options.h"
#include "antb1/sql/ast.h"

namespace antb1::plan {

// Resolves names and types against the catalog and builds the logical plan (its shape is in
// logical_plan.h) with every WHERE literal folded exactly into the type of its column
// (docs/sql-subset.md). One Scan of every field per FROM item, named by rule 1 of ADR 0022 (its
// alias, else a table's name as written, else a path's file name), joined as inner joins in the
// order the footer statistics choose; each item's own conjuncts filter its own branch, a
// cross-relation equality of one key type is a join key and anything else over two relations is a
// join's residual. The binder refers to columns by their ids, and plan::ResolvePositions sets every
// position at the end (ADR 0022). The plan is not optimized yet (see plan::Optimize). Errors carry
// a SqlErrorDetail with the span of the offending AST node: kBind for SQL that is wrong for the
// tables (unknown or ambiguous column, a qualifier no FROM item is named by, `*` over two items of
// one name sharing a column name, a `t.*` that names no FROM item of its own block or two of them,
// aggregates mixed with columns, SUM/AVG of a non-number, a literal of the wrong type),
// kUnsupported for a column of an unsupported type, FROM 'path' without a path opener, more than
// 256 relations, a join graph no key connects (a cross product, but of one-row inputs), a `t.*`
// over a STRUCT column, an ORDER BY position over an EXISTS block's star, and an ORDER BY name
// only the select list makes unambiguous.
arrow::Result<LogicalPlan> Bind(const sql::SelectStatement& stmt, const Catalog& catalog,
                                const BindOptions& options = {});

}  // namespace antb1::plan
