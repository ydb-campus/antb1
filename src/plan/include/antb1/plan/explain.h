#pragma once

#include <string>

#include "antb1/plan/logical_plan.h"

namespace antb1::plan {

// Deterministic text rendering of a logical plan (`antb1 explain` and the EXPLAIN tests): an
// "Output:" line with the result columns, then one line per node, root first, each input indented
// by two more spaces, e.g.
//
//   Output: count_star():BIGINT sum(x):HUGEINT
//   Aggregate COUNT(*), SUM(x)
//     Filter x > 0 AND d >= DATE '2013-07-01'
//       Scan table=t source=parquet(files=1, rows=10) columns=[x, d]
//
// Column names that are not plain identifiers are double-quoted; strings and names show bytes
// outside printable ASCII as \xHH.
std::string Explain(const LogicalPlan& plan);

}  // namespace antb1::plan
