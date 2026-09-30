#include "allocator.h"

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#ifdef __linux__
#include <unistd.h>
#endif

namespace antb1::cli {
namespace {

#ifdef __linux__
constexpr std::string_view kPurgeDelay = "MIMALLOC_PURGE_DELAY";

// The value of `name` in this process's environment, or nullptr (reads `environ`, which nothing
// changes while main starts: no other thread runs yet).
const char* FromEnviron(std::string_view name) {
  for (char* const* entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
    const std::string_view var(*entry);
    if (var.size() > name.size() && var.starts_with(name) && var[name.size()] == '=') {
      return *entry + name.size() + 1;
    }
  }
  return nullptr;
}
#endif

}  // namespace

bool NeedsAllocatorRestart(const EnvLookup& env) {
#ifdef __linux__
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

void RestartForAllocator(char* const* argv) {
#ifdef __linux__
  if (argv == nullptr || argv[0] == nullptr || !NeedsAllocatorRestart(&FromEnviron)) {
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
  // The environment plus the setting, for the new process only: if the exec fails, this process
  // goes on unchanged (with mimalloc's defaults).
  std::string setting = std::string(kPurgeDelay) + "=-1";
  std::vector<char*> env;
  for (char* const* entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
    env.push_back(*entry);
  }
  env.push_back(setting.data());
  env.push_back(nullptr);
  execve("/proc/self/exe", argv, env.data());
#else
  static_cast<void>(argv);
#endif
}

}  // namespace antb1::cli
