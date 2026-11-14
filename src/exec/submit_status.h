#pragma once

#include <arrow/status.h>

// Private to exec: the status of a task that could not be submitted to Arrow's thread pool.

namespace antb1::exec {

// Arrow's thread pool starts a worker when a task needs one, and since Arrow 26 (apache/arrow
// GH-48137) a worker the operating system refuses to start, or a bad_alloc while starting it, is an
// UnknownError from Submit. That is a limit of the machine and not a bug, so a query that meets it
// fails as an ExecutionError, which the CLI reports with exit code 1 (an UnknownError is 70, a
// bug's). Every other status of a Submit comes back unchanged.
inline arrow::Status SubmitStatus(arrow::Status status) {
  if (status.IsUnknownError()) {
    return arrow::Status::ExecutionError("could not start a worker thread: ", status.message());
  }
  return status;
}

}  // namespace antb1::exec
