// A SUM and an AVG over the same column, sharing one accumulator.
//
// Both hold the same per-group sums and the same per-group counts, and differ only in how they
// finalize: SUM emits the sums, AVG divides them by the counts. Built as two states they add every
// row twice -- a null test, a 16-byte load and a checked 128-bit add each -- for a column the
// engine has already summed. A grouped query that asks for both over two of its columns pays that
// twice over nearly 600 million rows.
//
// The alias carries no accumulator of its own: it consumes nothing, merges nothing, charges no
// memory, and finalizes through the state that does the work. Internal to exec on purpose -- the
// decision needs every aggregate call of one table at once, which is knowledge GroupTable has and
// MakeGroupedAggregateState, which sees one call, does not.

#ifndef ANTB1_EXEC_GROUPED_SUM_ALIAS_H_
#define ANTB1_EXEC_GROUPED_SUM_ALIAS_H_

#include <memory>

#include "antb1/exec/grouped_aggregate_state.h"

namespace antb1::exec {

// A state that finalizes `owner`'s accumulator as AVG (`average`) or as SUM, and does nothing
// else: `owner` must be the state that consumes the column, and must outlive the result.
//
// Null when `owner` is not a state whose accumulator serves both, which is the only answer a
// caller has to handle: it then makes a state of its own, as it did before. A temporal AVG keeps
// its own state this way, since it holds something else entirely.
std::unique_ptr<GroupedAggregateState> MakeSumAlias(const GroupedAggregateState& owner,
                                                    bool average);

}  // namespace antb1::exec

#endif  // ANTB1_EXEC_GROUPED_SUM_ALIAS_H_
