// The CPU guard of the standalone binary (docs/adr/0033-standalone-static-binary.md). The rest of
// the binary may use the raised baseline's instructions anywhere, even in constructors that run
// before main(), and a static binary has no loader that checks the CPU first. So this file is
// compiled for the architecture's base ISA and outside the ThinLTO partition
// (src/cli/CMakeLists.txt), and with ANTB1_CPU_GUARD (the baseline's flags) it checks the CPU from
// .preinit_array, before every constructor, and exits with a message instead of a SIGILL.
#include "cpu_guard.h"

#include <array>
#include <string_view>

#include <unistd.h>

#if defined(ANTB1_CPU_GUARD) && defined(__linux__)
#include <sys/auxv.h>
#if defined(__aarch64__)
#include <asm/hwcap.h>
#endif
#endif

namespace antb1::cli {

std::string_view MissingCpuFeature(std::string_view baseline, const CpuFeatures& cpu) {
  if (baseline == kCpuBaselineX86V3) {
    if (!cpu.avx2) {
      return "AVX2";
    }
    if (!cpu.bmi2) {
      return "BMI2";
    }
    if (!cpu.fma) {
      return "FMA";
    }
  } else if (baseline == kCpuBaselineArmV82) {
    if (!cpu.lse) {
      return "LSE atomics";
    }
    if (!cpu.rdm) {
      return "RDM";
    }
    if (!cpu.crc32) {
      return "CRC32";
    }
  }
  return {};
}

int ReportMissingCpuFeature(std::string_view baseline, const CpuFeatures& cpu, int fd) {
  const std::string_view missing = MissingCpuFeature(baseline, cpu);
  if (missing.empty()) {
    return 0;
  }
  const std::array<std::string_view, 5> parts{"antb1: this CPU lacks ", missing,
                                              ", which this build (", baseline,
                                              ") needs; use a build for an older CPU\n"};
  for (const std::string_view part : parts) {
    if (write(fd, part.data(), part.size()) < 0) {
      break;
    }
  }
  return 70;
}

}  // namespace antb1::cli

#if defined(ANTB1_CPU_GUARD) && defined(__linux__)
static_assert(antb1::cli::GuardsCpuBaseline(ANTB1_CPU_GUARD),
              "the CPU guard has no check for this baseline: add one to MissingCpuFeature");

namespace {

void GuardCpu(int /*argc*/, char** /*argv*/, char** /*envp*/) {
  antb1::cli::CpuFeatures cpu;
#if defined(__x86_64__)
  __builtin_cpu_init();  // its constructor has not run yet
  cpu.avx2 = __builtin_cpu_supports("avx2") != 0;
  cpu.bmi2 = __builtin_cpu_supports("bmi2") != 0;
  cpu.fma = __builtin_cpu_supports("fma") != 0;
#elif defined(__aarch64__)
  const unsigned long hwcap = getauxval(AT_HWCAP);
  cpu.lse = (hwcap & HWCAP_ATOMICS) != 0;
  cpu.rdm = (hwcap & HWCAP_ASIMDRDM) != 0;
  cpu.crc32 = (hwcap & HWCAP_CRC32) != 0;
#endif
  if (const int code = antb1::cli::ReportMissingCpuFeature(ANTB1_CPU_GUARD, cpu, STDERR_FILENO);
      code != 0) {
    _exit(code);
  }
}

// The loader-less start of a static executable runs .preinit_array before .init_array (the
// constructors). scripts/static_gates.py checks that GuardCpu is one of its entries.
[[gnu::used, gnu::section(".preinit_array")]] void (*const kGuardCpu)(int, char**,
                                                                      char**) = &GuardCpu;

}  // namespace
#endif
