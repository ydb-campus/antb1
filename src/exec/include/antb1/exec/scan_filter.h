#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include <arrow/memory_pool.h>
#include <arrow/result.h>
#include <arrow/type_fwd.h>

#include "antb1/exec/key_filter.h"
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

// The plan::ScanFilter of a hash join's key filter over field `column` of the scan
// (docs/adr/0029-dynamic-join-filters.md, D1): it clears the rows whose key the build certainly
// does not hold, so the join never sees them and their other columns are never materialised. The
// rows it keeps are exactly a superset of the rows the join would have kept, so the answer is the
// one the join alone would give.
//
// `column` is a position in the scan's field list, of the one key column, which must be a
// fixed-width type (plan::ScanValues carries VARCHAR as views, which this does not hash). Invalid
// without a filter.
//
// `dropped` (nullptr: not counted) is added to by the rows it clears, for the scan to report what
// the filter did (D3). It is counted here and not above because the operator over the scan is
// handed the survivors alone and so cannot see the difference; one add per call and not per row,
// and atomic because a ScanFilter is documented to be applied by several parts at once.
arrow::Result<std::shared_ptr<const plan::ScanFilter>> MakeKeyScanFilter(
    std::shared_ptr<const KeyFilter> filter, int column,
    std::shared_ptr<std::atomic<int64_t>> dropped = nullptr);

}  // namespace antb1::exec
