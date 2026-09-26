#pragma once

#include <string>
#include <string_view>

namespace antb1 {

// Project version from CMake's project(VERSION ...).
std::string_view Version();

// The compiler that built antb1, e.g. "Clang 23.1.0" or "GCC 15.1.0".
std::string CompilerVersion();

// CMake's build type of this build (e.g. "Release", "Debug"); "unknown" if none was set.
std::string_view BuildType();

}  // namespace antb1
