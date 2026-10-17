#pragma once

#include <cstdint>
#include <vector>

#include "antb1/common/int128.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/table.h"

// Skipping table parts by their statistics (plan::Table::part_stats), two ways. Both leave the
// result unchanged, and both skip nothing when the statistics are unknown.
//
// PartMayMatch and KeptParts skip before the query runs: a part none of whose rows can pass the
// WHERE conjunction. PartMayBeat skips while a top-N runs: a part none of whose rows can reach the
// rows it keeps, which depends on the rows read so far and so cannot be decided when the plan is
// made.

namespace antb1::exec {

// Whether part `part` may hold a row that passes every predicate. `fields` maps a predicate's
// column index (a column of the scan's output) to the table field (plan::ScanNode::fields).
bool PartMayMatch(const plan::Table& table, int64_t part, const std::vector<int>& fields,
                  const std::vector<plan::Predicate>& predicates);

// The parts that may match, in part order.
std::vector<int64_t> KeptParts(const plan::Table& table, const std::vector<int>& fields,
                               const std::vector<plan::Predicate>& predicates);

// Whether a part with these statistics of a top-N's first sort key may hold a row that comes before
// `bound` in `key`'s order -- the value of that key in the worst row the top-N keeps so far
// (SortBuffer::ThresholdKey). A part that may not holds nothing the top-N can use, so it need not
// be read at all.
//
// A part is ruled out only when it is *strictly* worse on the first key: one whose key equals
// `bound` may still come first on a later key, and SortBuffer::Add would take such a row. So
// equality always keeps the part.
bool PartMayBeat(const plan::PartStats& stats, const plan::SortKey& key, const Int128& bound);

}  // namespace antb1::exec
