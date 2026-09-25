# 2. Toolchain: pixi and conda-forge, Clang 23 primary, GCC 15 compatibility leg

Date: 2026-09-25

## Status

Accepted

## Context

- The engine needs C++23, Apache Arrow C++ with its compute and Parquet libraries, CMake presets and modern
  sanitizers and linters. The maintainers' hosts are old (Ubuntu 20.04 with glibc 2.31, g++ 10 and CMake 3.16),
  root access and containers are not available everywhere, and cloud sandboxes for AI agents start from a generic
  image.
- Humans, CI and AI agents must use the same commands and get the same results on Linux x86-64, Linux aarch64 and
  macOS arm64.
- Sanitizers, clang-tidy, source-based coverage and libFuzzer all come from LLVM. Using one compiler family for
  development and for these tools avoids "works with one compiler, not the other" gaps.
- conda-forge's Arrow packages on Linux are built against libstdc++, so code that links them must use libstdc++.
- CodeQL can trace GCC up to version 16 but Clang only up to 22.

## Decision

- The toolchain comes entirely from [pixi](https://pixi.prefix.dev) 0.81.0 and the conda-forge channel. `pixi.toml` and
  `pixi.lock` (lock format version 7, three platforms) are committed; the lock is regenerated with pixi 0.81.0 only
  and never edited by hand.
- Three environments:
  - `default`: Clang 23 (clang-tools, lld, compiler-rt), CMake 4.4, Ninja, ccache, Arrow, Arrow compute and Parquet
    25, CLI11, GoogleTest, Google Benchmark, DuckDB (test oracle only, never linked into `src/`) and bash 5. On Linux
    Clang builds against conda's libstdc++ 15 with sysroot 2.28 and links with lld; on macOS it uses libc++.
  - `gcc`: the same libraries with GCC 15, on linux-64 only. It runs one compatibility CI leg with GCC-only
    warnings (`pixi run ci-gcc`) and the CodeQL traced build (`pixi run codeql-build`).
  - `lint`: formatters and linters only (clang-format, gersemi, ruff, typos, actionlint, shellcheck, zizmor,
    markdownlint-cli2, tombi, pre-commit, pytest).
- Clang 23 is the primary compiler on every platform: development, tests, ASan/UBSan, clang-tidy and, later,
  coverage, fuzzing and TSan.
- The command surface is `pixi run <task>`. Every task is defined in exactly one environment, so no `-e` is needed;
  CI passes `-e` and `--frozen` explicitly. Tasks call CMake presets (`CMakePresets.json`), and build trees live in
  `build/<preset>`.
- CMake finds dependencies with plain `find_package`, so a system installation of the same libraries also works.
- Never `-stdlib=libc++` on Linux and never `_GLIBCXX_DEBUG`; the configure step rejects both in `CMAKE_CXX_FLAGS`.
  Debug builds use `_GLIBCXX_ASSERTIONS` (and libc++ hardening on macOS).
- A new machine is set up with `bash scripts/agent-setup.sh` (Linux) or a pixi installation (macOS).

## Consequences

- No system packages, root access or containers are needed; the same lock file drives local work, CI and agent
  sandboxes.
- The environments are large (about 1.9 GB for `default`, 0.6 GB for `gcc` and 0.3 GB for `lint` on linux-64) and
  the first installation downloads them. CI caches them per lock file.
- Two compilers must stay warning-free. GCC-specific warnings (for example `-Wuseless-cast`, `-Wlogical-op`,
  `-Wduplicated-cond`) are checked only in the `gcc` leg.
- Standard-library differences between libstdc++ (Linux) and libc++ (macOS) are caught by the macOS leg.
- Strict C++23 with libstdc++ does not treat `__int128` as an integral type; the project uses its own helpers for
  `Int128`.
- Fallbacks if Clang 23 and libstdc++ 15 stop working together: keep `libstdcxx-devel` and `libgcc-devel` on the same
  major version, try a newer libstdc++, or pin Clang 22.1 (which would also allow CodeQL on Clang).
