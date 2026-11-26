#pragma once

#include <set>

#include "antb1/plan/plan_options.h"

namespace antb1::plan {

// Place mergeable grouped aggregates on the scan branches of an inner join's UNION ALL probe.
LogicalNodePtr RewriteUnionAggregates(const LogicalNodePtr& root, const BindOptions& options,
                                      ColumnId& last_id,
                                      const std::set<const LogicalNode*>& preserve);

}  // namespace antb1::plan
