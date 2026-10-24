#pragma once

#include <string_view>

namespace antb1::cli {

// The raised CPU baselines of the standalone build (cmake/static/toolchain.cmake), as
// antb1::CpuBaseline() names them.
inline constexpr std::string_view kCpuBaselineX86V3 = "-march=x86-64-v3";
inline constexpr std::string_view kCpuBaselineArmV82 = "-march=armv8.2-a";

// What the CPU offers of the features that a raised CPU baseline compiles in.
struct CpuFeatures {
  bool avx2 = false;   // x86-64-v3
  bool bmi2 = false;   // x86-64-v3
  bool fma = false;    // x86-64-v3
  bool lse = false;    // armv8.2-a: large system extension atomics (ARMv8.1)
  bool rdm = false;    // armv8.2-a: rounding doubling multiply-accumulate (ARMv8.1)
  bool crc32 = false;  // armv8.2-a
};

// The first feature that `baseline` needs and `cpu` lacks; empty when nothing is missing or the
// baseline is not a raised one.
std::string_view MissingCpuFeature(std::string_view baseline, const CpuFeatures& cpu);

// Whether a binary compiled for `baseline` is safe from a SIGILL on an older CPU: the compiler's
// default and the architectures' base ISAs need no check; a raised baseline must be one
// MissingCpuFeature() knows. cpu_guard.cc refuses to compile a guard for any other baseline.
constexpr bool GuardsCpuBaseline(std::string_view baseline) {
  return baseline.empty() || baseline == "-march=x86-64" || baseline == "-march=armv8-a" ||
         baseline == kCpuBaselineX86V3 || baseline == kCpuBaselineArmV82;
}

// The guard's answer for `cpu`: when it lacks a feature of `baseline`, writes "antb1: this CPU
// lacks <feature>, ..." to `fd` and returns 70 (the CLI's internal error); otherwise writes nothing
// and returns 0. It calls only write(2): the guard runs before any constructor.
int ReportMissingCpuFeature(std::string_view baseline, const CpuFeatures& cpu, int fd);

}  // namespace antb1::cli
