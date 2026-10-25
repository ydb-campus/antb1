#include "../cpu_guard.h"

#include <array>
#include <cstddef>
#include <string>

#include <gtest/gtest.h>

#include "antb1/common/narrow.h"
#include "antb1/common/version.h"

#include <unistd.h>

namespace antb1::cli {
namespace {

constexpr CpuFeatures kAll{
    .avx2 = true, .bmi2 = true, .fma = true, .lse = true, .rdm = true, .crc32 = true};

TEST(CpuGuardTest, NamesTheFirstMissingFeatureOfARaisedBaseline) {
  EXPECT_EQ(MissingCpuFeature("-march=x86-64-v3", kAll), "");
  EXPECT_EQ(MissingCpuFeature("-march=armv8.2-a", kAll), "");
  CpuFeatures cpu = kAll;
  cpu.fma = false;
  EXPECT_EQ(MissingCpuFeature("-march=x86-64-v3", cpu), "FMA");
  cpu.bmi2 = false;
  EXPECT_EQ(MissingCpuFeature("-march=x86-64-v3", cpu), "BMI2");
  cpu.avx2 = false;
  EXPECT_EQ(MissingCpuFeature("-march=x86-64-v3", cpu), "AVX2");
  EXPECT_EQ(MissingCpuFeature("-march=armv8.2-a", cpu), "") << "x86 features do not matter on Arm";
  cpu = kAll;
  cpu.crc32 = false;
  EXPECT_EQ(MissingCpuFeature("-march=armv8.2-a", cpu), "CRC32");
  cpu.rdm = false;
  EXPECT_EQ(MissingCpuFeature("-march=armv8.2-a", cpu), "RDM");
  cpu.lse = false;
  EXPECT_EQ(MissingCpuFeature("-march=armv8.2-a", cpu), "LSE atomics");
}

TEST(CpuGuardTest, DefaultAndBaseBaselinesNeedNothing) {
  for (const auto* baseline : {"", "-march=x86-64", "-march=armv8-a"}) {
    EXPECT_EQ(MissingCpuFeature(baseline, CpuFeatures{}), "") << baseline;
    EXPECT_TRUE(GuardsCpuBaseline(baseline)) << baseline;
  }
  EXPECT_FALSE(GuardsCpuBaseline("-march=x86-64-v4"))
      << "a raised baseline the guard does not know";
}

TEST(CpuGuardTest, GuardsTheBaselineOfThisBuild) {
  // A new default baseline in cmake/static/toolchain.cmake must come with its check in
  // cpu_guard.cc.
  EXPECT_TRUE(GuardsCpuBaseline(CpuBaseline())) << CpuBaseline();
}

TEST(CpuGuardTest, ReportsAMissingFeatureAndExitsWith70) {
  std::array<int, 2> fds{};
  ASSERT_EQ(pipe(fds.data()), 0);
  CpuFeatures cpu = kAll;
  cpu.bmi2 = false;
  EXPECT_EQ(ReportMissingCpuFeature("-march=x86-64-v3", cpu, fds[1]), 70);
  EXPECT_EQ(ReportMissingCpuFeature("-march=x86-64-v3", kAll, fds[1]), 0) << "writes nothing";
  EXPECT_EQ(ReportMissingCpuFeature("", CpuFeatures{}, fds[1]), 0) << "writes nothing";
  close(fds[1]);
  std::string text;
  std::array<char, 256> buffer{};
  for (ssize_t n; (n = read(fds[0], buffer.data(), buffer.size())) > 0;) {
    text.append(buffer.data(), Narrow<std::size_t>(n));
  }
  close(fds[0]);
  EXPECT_EQ(
      text,
      "antb1: this CPU lacks BMI2, which this build (-march=x86-64-v3) needs; use a build for an "
      "older CPU\n");
}

}  // namespace
}  // namespace antb1::cli
