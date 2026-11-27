#pragma once

#include <arrow/status.h>

namespace antb1::exec {

struct ExecContext;

// Validate this run's scan context after all readers have closed, using the query executor.
arrow::Status FinishScans(const ExecContext& ctx);

}  // namespace antb1::exec
