#include "allocator.h"

#include <cstdlib>
#include <string>
#include <string_view>

#if defined(__linux__)
#include <unistd.h>
#endif

namespace antb1::cli {
namespace {

constexpr const char* kPurgeDelay = "MIMALLOC_PURGE_DELAY";

}  // namespace

bool NeedsAllocatorRestart(const EnvLookup& env) {
#if defined(__linux__)
  if (env(kPurgeDelay) != nullptr) {
    return false;
  }
  const char* pool = env("ARROW_DEFAULT_MEMORY_POOL");
  return pool == nullptr || std::string_view(pool) == "mimalloc";
#else
  static_cast<void>(env);
  return false;
#endif
}

void RestartForAllocator(char** argv) {
#if defined(__linux__)
  const EnvLookup env = [](std::string_view name) {
    return std::getenv(std::string(name).c_str());
  };
  if (argv == nullptr || argv[0] == nullptr || !NeedsAllocatorRestart(env)) {
    return;
  }
  // Set before the exec, so that a failed exec never loops: the process goes on without it.
  if (setenv(kPurgeDelay, "-1", /*overwrite=*/0) != 0) {
    return;
  }
  execv("/proc/self/exe", argv);
#else
  static_cast<void>(argv);
#endif
}

}  // namespace antb1::cli
