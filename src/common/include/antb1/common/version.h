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

// The commit this build was made from (its sha, "-dirty" appended for uncommitted changes); empty
// when the build does not record one, as in every dev and CI preset (scripts/static.sh records it
// in the standalone binary).
std::string_view GitSha();

// How the binary was built: "dynamic" (conda's shared Arrow, every preset but `static`) or
// "static-thinlto" (the standalone binary: static Arrow, -O3, ThinLTO;
// docs/adr/0033-standalone-static-binary.md).
std::string_view BuildFlavor();

// The CPU baseline compiled into the binary, e.g. "-march=x86-64-v3"; empty for the compiler's
// default.
std::string_view CpuBaseline();

}  // namespace antb1
