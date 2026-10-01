#pragma once

#include <memory>
#include <vector>

#include <arrow/memory_pool.h>
#include <arrow/result.h>
#include <arrow/type_fwd.h>

#include "antb1/plan/logical_plan.h"
#include "antb1/plan/table.h"

// WHERE predicates that a scan applies while it decodes (filter pushdown,
// docs/adr/0020-filter-pushdown.md).

namespace antb1::exec {

// Whether a scan can apply `predicate`, one of the WHERE conjunction directly over it: a predicate
// that reads one column, compared with constants (`<op>`, [NOT] IN, [NOT] LIKE, IS NOT NULL).
// Comparisons of two columns and IS TRUE (of a computed column) are not, nor FALSE, with which the
// Filter ends the stream before reading anything.
[[nodiscard]] bool PushableToScan(const plan::Predicate& predicate);

// The plan::ScanFilter of `predicates` (every one PushableToScan) over the scan's output `schema`:
// a row passes when every predicate is true, as in FilterOperator. Fixed-width columns are
// evaluated with PredicateEvaluator (its kernels allocate from `pool`), VARCHAR views in place with
// the same results: LIKE as LikePattern, `<op>` and IN bytewise, NULL fails.
arrow::Result<std::shared_ptr<const plan::ScanFilter>> MakeScanFilter(
    const std::vector<plan::Predicate>& predicates, const arrow::Schema& schema,
    arrow::MemoryPool* pool);

}  // namespace antb1::exec
