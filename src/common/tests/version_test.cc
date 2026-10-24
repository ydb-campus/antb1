#include "antb1/common/version.h"

#include <regex>
#include <string>

#include <gtest/gtest.h>

namespace antb1 {
namespace {

TEST(VersionTest, VersionCompilerAndBuildType) {
  EXPECT_TRUE(std::regex_match(std::string(Version()), std::regex(R"(\d+\.\d+\.\d+.*)")))
      << Version();
  EXPECT_TRUE(std::regex_match(CompilerVersion(), std::regex(R"((Clang|GCC) \d+\.\d+\.\d+)")))
      << CompilerVersion();
  EXPECT_FALSE(BuildType().empty());
}

TEST(VersionTest, BuildIdentity) {
  EXPECT_TRUE(std::regex_match(std::string(GitSha()), std::regex(R"(([0-9a-f]{40}(-dirty)?)?)")))
      << GitSha();
  EXPECT_TRUE(BuildFlavor() == "dynamic" || BuildFlavor() == "static-thinlto") << BuildFlavor();
  // Only the standalone build raises the CPU baseline (cmake/static/toolchain.cmake).
  EXPECT_TRUE(CpuBaseline().empty() ||
              (BuildFlavor() == "static-thinlto" && CpuBaseline().starts_with("-m")))
      << CpuBaseline();
}

}  // namespace
}  // namespace antb1
