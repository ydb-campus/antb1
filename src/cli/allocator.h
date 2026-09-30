#pragma once

#include <functional>
#include <string_view>

// Private to the cli module: the allocator setting of the antb1 process
// (docs/adr/0017-allocator-keeps-memory.md).

namespace antb1::cli {

// Looks up an environment variable: its value, or nullptr when it is not set.
using EnvLookup = std::function<const char*(std::string_view name)>;

// Whether the process must restart with MIMALLOC_PURGE_DELAY=-1 so that mimalloc, Arrow's default
// memory pool, keeps the memory a query frees instead of returning it to the system: on Linux, when
// the variable is unset (a value the user set wins, and a restarted process has it) and Arrow's
// default pool is mimalloc (ARROW_DEFAULT_MEMORY_POOL unset or "mimalloc").
bool NeedsAllocatorRestart(const EnvLookup& env);

// Restarts the process (the same executable, arguments and open files) with MIMALLOC_PURGE_DELAY=-1
// when NeedsAllocatorRestart: mimalloc reads its options when Arrow is loaded, before main, so a
// process cannot change them for itself. Returns only when no restart is needed or it failed; the
// process then runs with mimalloc's defaults.
void RestartForAllocator(char** argv);

}  // namespace antb1::cli
