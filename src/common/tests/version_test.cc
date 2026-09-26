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

}  // namespace
}  // namespace antb1
