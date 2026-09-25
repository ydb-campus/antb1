#include "antb1/common/version.h"

#include <format>
#include <string>
#include <string_view>

namespace antb1 {

std::string_view Version() { return ANTB1_VERSION_STRING; }

std::string CompilerVersion() {
#ifdef __clang__
  return std::format("Clang {}.{}.{}", __clang_major__, __clang_minor__, __clang_patchlevel__);
#elifdef __GNUC__
  return std::format("GCC {}.{}.{}", __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
#else
  return "unknown";
#endif
}

std::string_view BuildType() {
  constexpr std::string_view kBuildType = ANTB1_BUILD_TYPE;
  if constexpr (kBuildType.empty()) {
    return "unknown";
  } else {
    return kBuildType;
  }
}

}  // namespace antb1
