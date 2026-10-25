# 33. A standalone static binary for releases and performance work

Date: 2026-10-25

## Status

Proposed

## Context

- **Where antb1 runs today:** every build links conda-forge's shared Arrow ([ADR 0002](0002-toolchain-pixi-conda-forge.md)).
  The binary needs the pixi environment: its RUNPATH points into `.pixi`, and a copy that runs elsewhere needs
  127 shared libraries (165 MB) and the host's glibc 2.17 or newer. Releases and benchmark hosts without the
  repository, pixi or that environment cannot run it.
- **Start-up:** a plain `antb1 version` spends most of its time in the dynamic loader, and the allocator restart
  ([ADR 0017](0017-allocator-keeps-memory.md)) pays it twice.
- **Optimization:** the benchmark build is Release (-O3) for antb1 only. conda-forge builds Arrow with GCC at -O2,
  with LTO inside each library but never across the antb1/Arrow boundary, and antb1's own loops are compiled for
  the architecture's base ISA.
- **The maintainers want** one fast binary per target (Linux x86-64, Linux aarch64, macOS arm64), built in pixi but
  running without it, published with GitHub releases and used for every performance measurement.

## Decision

- **A fourth pixi environment, `static`,** without conda's Arrow. It shares the build tools (feature `base`) and the
  Clang 23 toolchain (feature `llvm`) with `default`, in the same solve group, so both resolve to the same package
  versions. conda's clang config file puts `-isystem $CONDA_PREFIX/include` before every include path, so an
  environment with conda's Arrow, re2, utf8proc, snappy, lz4, thrift or zlib would shadow the bundled headers.
  Boost's headers and CMake package come from conda (`libboost-devel`); no Boost library is linked.
- **Arrow from pinned sources.** `pixi run static-arrow` builds Apache Arrow 25.0.0 (the version pixi.lock pins) and
  the 11 dependencies Arrow bundles for the enabled features, from tarballs pinned by sha256 and size in
  `cmake/static/arrow-sources.lock` and downloaded once (network on the first run only, like `fetch-data`). The
  result is a static prefix under `~/.cache/antb1/arrow/<key>`, where the key hashes everything that decides it.
  - The pins come only from the ASF release: Arrow's tarball checked against its `.sha512` and GPG signature, every
    other pin from that tarball's `cpp/thirdparty/versions.txt`. Each file then lists byte-identical copies of at
    least two operators, tried in order. Arrow 25.0.0 and Thrift 0.22.0 left Apache's mirrors, and
    `archive.apache.org` is one throttled server, so it comes last. The copies stay out of the key, so a changed
    copy rebuilds nothing; `--check-urls` checks them on PRs that change the lock, and nightly.
  - Only what antb1 uses is on: compute, Parquet with the snappy, gzip, brotli, zstd and lz4 codecs, RE2, utf8proc
    and mimalloc. jemalloc, Parquet encryption, the filesystems, glog, OpenTelemetry and backtraces are off, which
    also keeps every user of NSS and `dlopen` out of a static glibc link.
  - Everything is compiled with the same Clang, `-O3` and ThinLTO: `cmake/static/project-include.cmake` reaches every
    project, the dependencies included, but never a configure check. Arrow replaces CMake's `-O3` by `-O2`, so its
    `ARROW_*_FLAGS_RELEASE` restate `-O3`; the project include restates it once more after the targets' own flags.
  - Gates check the prefix before it is used (`scripts/static_gates.py prefix`): static archives only, every C and
    C++ file at `-O3` with `-flto=thin`, no header of a bundled library from the environment, every download a local
    file, Arrow's option set, and a report of bitcode and native archive members.
- **antb1 in preset `static`:** Release, `-O3`, ThinLTO against that prefix (`ANTB1_ARROW_LINKAGE=static` switches every
  target from `Arrow::arrow_shared` to `Arrow::arrow_static`, and the same for ArrowCompute and Parquet).
  - **Linux:** fully static and non-PIE, on the locked glibc 2.28 sysroot: no loader, no dynamic section, any
    distribution with kernel 3.2 (x86-64) or 3.7 (aarch64). Below glibc 2.34 libstdc++ refers to pthread weakly, so
    `libpthread.a` is linked whole. Static glibc 2.28 does not use the vDSO, so a clock read is a system call. antb1
    reads the clock a few times per query (7 times per run in `bench`, about once more per join build), twice per
    operator and batch under `explain --analyze`, and mimalloc only while purging, which the restart turns off;
    Consequences give the measured cost.
  - **macOS:** Apple ships no static executables. libc++ is linked statically; libSystem and libc++abi come from the
    OS; deployment target 13.0, the floor the lock was solved for; the linker's ad-hoc signature, without an
    LC_RPATH into the environment.
  - **CPU baseline:** x86-64-v3 and armv8.2-a for antb1, Arrow and every dependency (macOS: Clang's apple-m1
    default). Arrow still picks its AVX2, AVX-512 and SVE kernels at run time. A static binary has no loader that
    checks the CPU, so a guard compiled for the base ISA runs from `.preinit_array`, before every constructor, and
    exits with a message instead of a SIGILL.
  - The allocator restart of ADR 0017 is kept unchanged: mimalloc still reads its options in a constructor.
- **The shipped binary is the tested binary.** `pixi run static` packages the binary with LICENSE, NOTICE, its
  third-party notices, `BUILDINFO.json` and a README (on Linux the line tables go to a separate `.debug` asset; the
  binary keeps its symbols for perf), then checks the package and the packaged file: linkage, link flags, build
  identity (`antb1 version --verbose`), the restart, codec, thread and error smoke tests on files DuckDB writes at
  run time, and every CLI golden (`ANTB1_CLI_UNDER_TEST`). `pixi run static-test` runs every hermetic test against
  the static Arrow. Each tarball member is the staged file those checks ran, and the package is checked again as a
  fresh runner gets it (`scripts/static_gates.py verify`, standard library only, so a runner without pixi or DuckDB
  can run it): unpacked with tarfile's data filter, with the same runs on the smoke kit that `pixi run static` writes,
  and on Linux also in a container of an image that holds only the binary and the kit. `static.yml` runs it with the
  runner's own `python3` on every leg (`pixi run static-check <tarball>` locally).
- **CI:** `static.yml` builds the three targets on PRs that touch the build and on every push to main (advisory, not
  part of `CI OK`; the prefix cache is written from main only); the nightly builds them from scratch and runs
  `static-test`. Every performance test runs on this binary since 2026-10-25, after the comparisons in Consequences:
  `pixi run bench` and `pixi run bench-clickbench` build in `build/static`, `bench.yml` publishes the series
  `dev/bench-static`, and the dynamic `bench` preset is gone (docs/benchmarks.md). The project licence and the
  sources that go with every Linux package (`pixi run static-sources`) are
  [ADR 0034](0034-licence-and-static-sources.md); publishing the binary with GitHub releases follows separately.

## Consequences

- One file per target that runs without pixi, the repository or a compatible glibc, and that names its build
  (`antb1 version --verbose`, the bench JSON's `flavor` and `cpu_baseline`, and its commit even on a host without
  git). It is 46 MB on linux-x86_64, 13 MB as a tarball, and starts in a twentieth of the time: `antb1 version`
  takes 3.0 ms with the allocator restart (1.5 ms without) against 63.5 ms (32.4 ms) for the dynamic `ci-release`
  build, medians of 300 interleaved runs on a 64-core Linux host (2026-10-25).
- **What the flavour is worth on Linux (ClickBench, measured 2026-10-25).** The setup:
  - 43 ClickBench queries on the 100 files, hot in the page cache, on a two-socket Xeon Gold 6338 host (64 cores,
    128 threads, AVX-512);
  - three builds of one commit, every answer equal;
  - each query in a fresh process with three tries, its time the faster of tries 2 and 3, the builds in a balanced
    order.

  Summed over the queries, the static binary took 0.822 of preset `bench`'s time at 1 thread, 0.854 at 16, 0.940
  at 64 and 0.950 at 128 (`ci-release`, `-O2`, was within 1% of `bench`).
  - It executes 28-29% fewer user-mode instructions on every query, and its lower IPC gives part of that back.
  - At 1 and 16 threads 42 of the 43 queries are faster beyond the run's noise. At 64 and 128 threads the gain is
    real in sum but small per query.
  - Each process also spends about 0.08 s less outside the query timer.

  The run cannot separate ThinLTO, Arrow built by Clang at `-O3`, the x86-64-v3 baseline and static linking: like
  the entry below, it claims the package, not a cause.
- **What the flavour is worth on a workload, which is the comparison the Decision above makes a precondition of
  moving the performance tests (measured 2026-10-25).** The baseline is the build being replaced: preset `bench`
  (Release, `-O3`), not `ci-release` (RelWithDebInfo, `-O2 -g`), which an earlier draft of this entry used and
  which flatters the static arm by about a point. Both arms from the same tree (`e9ee280`, which the static
  binary records and the dynamic one does not: only `scripts/static.sh` sets `ANTB1_GIT_SHA`), TPC-H-derived
  queries at scale factor 100, 5 repeats interleaved and rotated, on a 14-thread macOS arm64 host. Every figure
  below is a **minimum over the 5 repeats**, and each peak memory the maximum over every query and repeat.

  | | 4 threads | 14 threads |
  | --- | ---: | ---: |
  | total | 0.977x (94.914 s -> 92.762 s) | 0.965x (42.594 s -> 41.098 s) |
  | geomean of the per-query ratios | 0.973x | 0.955x |
  | **the same, net of start-up** | **0.984x** | **0.979x** |
  | peak memory | 14,177 -> 14,145 MiB | 15,289 -> 15,165 MiB |

  **The third row is the one to quote**, and the two above it are why. The harness times whole processes, and on
  this host the static binary starts in **2.99 ms against 31.79 ms** (medians of 200 interleaved `antb1 version`
  runs; no allocator restart happens here at all, `RestartForAllocator` being `#ifdef __linux__`). That fixed
  28.8 ms is 1.4% of the average query at 14 threads and 0.7% at four, so it inflates the apparent win on the
  short queries -- Q11 reads 0.897x at 14 threads and 0.967x once start-up is removed -- and it accounts for
  **72% of the difference between the two configurations** (a 1.76-point raw gap in the geomeans becomes 0.50).
  What is left is real and small: **1.6% of query time at four threads and 2.1% at 14.** Subtracting a median
  start-up from a minimum query time is sound here only because the distribution is tight (dynamic min 31.22 ms,
  p90 32.61); using the minima instead moves the net geomeans by 0.0001, and either way the bias is against the
  static arm, never for it.

  Uniform rather than concentrated: **no query of the 22 is slower at either thread count**, and that is exact
  rather than a rounding -- the narrowest of the 44 readings is Q09 at 0.996x and 0.992x. Net of start-up the
  range is Q01 0.953x to Q09 0.998x at four threads and Q19 0.960x to Q09 0.997x at 14.

  So the move is supported, on a figure well under half of what the start-up numbers above suggest. What this
  host cannot settle: it is macOS arm64 only, where there is no allocator restart and the loader is not glibc's;
  the three differences the run cannot separate are ThinLTO, Arrow rebuilt with the same Clang 23 at `-O3`
  (conda's macOS Arrow is Clang 21.1.8, so the "Clang instead of GCC" difference below is a **Linux** one, not a
  cause here) and the CPU baseline -- and a difference this size can come from code layout alone, so the entry
  claims the package rather than any cause. The 14-thread leg is also 14 threads on a 14-thread host, and the
  run began while a build's load was still decaying; the interleaved minimum of 5 mitigates both, and the
  thread-count difference it feeds into is 0.5 of a point.
- ADR 0002 still holds for every other environment; only `static` takes Arrow from source. An Arrow upgrade updates
  `cmake/static/arrow-sources.lock` with pixi.lock (the dependency-update recipe), and any change of the inputs makes
  a new prefix, built from scratch (about a minute on a 64-core host).
- The static Arrow differs from conda's: Clang instead of GCC, other optimization, and its own versions of re2,
  utf8proc, brotli and zlib. `static-test` and the goldens show where results differ; expectations are never edited
  to hide it.
- The binary needs a CPU from the baseline on (Haswell and newer on x86-64, Neoverse N1 or Graviton 2 and newer on
  aarch64). The guard turns an older CPU into a message; a portable build is one environment variable away
  (`ANTB1_STATIC_CPU_FLAGS`).
- Static glibc is LGPL-2.1, so every Linux package comes with the sources of glibc, of antb1 and of every other
  library linked into it except the GCC runtime, which comes with the compiler
  ([ADR 0034](0034-licence-and-static-sources.md)).
- Static glibc cannot load NSS modules or gconv converters from the host, and clock reads are system calls. antb1
  uses none of the former. The latter was measured before the perf tests moved (2026-10-25; Linux 5.4, clocksource
  tsc, on the Xeon host of the Linux entry above):
  - `antb1 bench` makes 912 `clock_gettime` calls for the 43 ClickBench queries × 3 tries, on one file or on the
    100, at 1 to 128 threads: 9 at start and 7 per run of a query.
  - Each call costs 0.34 µs more than a vDSO read, about 2.4 µs per run.
  - `explain --analyze` makes up to 55,000 calls for one query. That is at most about 4% of its time on the worst
    cases measured: the 100 files at 16 threads, and the queries derived from TPC-H on 2048-row row groups at 4
    threads. The bound charges every call to the wall time. Profiles are diagnostics.
- Started through a symlink of another name, the binary skips the restart and runs about 5% slower (ADR 0017); a
  renamed copy restarts, since the check compares the names of `/proc/self/exe` and `argv[0]`.
  `pixi run bench-clickbench` runs a given binary by its resolved path and refuses one whose restart did not happen.
- Alternatives considered: conda-forge's static archives (none exist for Arrow, snappy, brotli, re2, utf8proc or
  thrift); linking glibc dynamically (keeps the loader and a host glibc requirement; it stays the fallback, also if
  the LGPL route of ADR 0034 becomes impractical); a newer sysroot for the vDSO (the environment's dynamic test
  executables would then need glibc 2.34 or newer on the host); `-static-pie` (no `rcrt1.o` in the aarch64 sysroot,
  and start-up relocations); full LTO (slower, more memory on CI runners than ThinLTO).

This workload is derived from the TPC-H Benchmark and is not comparable to published TPC-H Benchmark results, as
this implementation does not comply with all requirements of the TPC-H Benchmark.
