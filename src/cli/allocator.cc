#include "allocator.h"

#include <array>
#include <cstddef>
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

bool IsSameProgram(std::string_view exe, std::string_view argv0) {
  const auto base = [](std::string_view path) {
    const std::size_t slash = path.rfind('/');
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
  };
  return !base(argv0).empty() && base(exe) == base(argv0);
}

void RestartForAllocator(char** argv) {
#if defined(__linux__)
  const EnvLookup env = [](std::string_view name) {
    return std::getenv(std::string(name).c_str());
  };
  if (argv == nullptr || argv[0] == nullptr || !NeedsAllocatorRestart(env)) {
    return;
  }
  // Only a directly started antb1 restarts: run through the dynamic loader or an emulator,
  // /proc/self/exe is that program, which would read argv differently.
  std::array<char, 4096> exe{};
  const ssize_t length = readlink("/proc/self/exe", exe.data(), exe.size() - 1);
  if (length <= 0 ||
      !IsSameProgram(std::string_view(exe.data(), static_cast<std::size_t>(length)), argv[0])) {
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
