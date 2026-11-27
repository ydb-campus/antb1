#pragma once

#include <arrow/status.h>

namespace antb1::exec {

inline arrow::Status ScalarCardinalityError() {
  return arrow::Status::ExecutionError(
      "More than one row returned by a subquery used as an expression - scalar subqueries can only "
      "return a single row");
}

}  // namespace antb1::exec
