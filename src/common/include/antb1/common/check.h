#pragma once

#include <cstdio>
#include <cstdlib>
#include <print>
#include <source_location>

// Invariant checks. ANTB1_CHECK is always on; ANTB1_DCHECK compiles away in NDEBUG builds.
// Use them for programming errors only; user-facing errors travel as std::expected / arrow::Status.

namespace antb1::internal {

[[noreturn]] inline void CheckFailed(const char* expr,
                                     std::source_location loc = std::source_location::current()) {
  std::println(stderr, "antb1: check failed: {} at {}:{} ({})", expr, loc.file_name(), loc.line(),
               loc.function_name());
  std::abort();
}

}  // namespace antb1::internal

#define ANTB1_CHECK(cond)                    \
  do {                                       \
    if (!(cond)) [[unlikely]] {              \
      ::antb1::internal::CheckFailed(#cond); \
    }                                        \
  } while (false)

#ifdef NDEBUG
#define ANTB1_DCHECK(cond) \
  do {                     \
  } while (false)
#else
#define ANTB1_DCHECK(cond) ANTB1_CHECK(cond)
#endif
