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
    `libpthread.a` is linked whole. Static glibc 2.28 does not use the vDSO, so a clock read is a system call; antb1
    reads the clock only for `explain --analyze` and `bench`, and mimalloc only while purging, which the restart
    turns off.
  - **macOS:** Apple ships no static executables. libc++ is linked statically; libSystem and libc++abi come from the
    OS; deployment target 13.0, the floor the lock was solved for; the linker's ad-hoc signature, without an
    LC_RPATH into the environment.
  - **CPU baseline:** x86-64-v3 and armv8.2-a for antb1, Arrow and every dependency (macOS: Clang's apple-m1
    default). Arrow still picks its AVX2, AVX-512 and SVE kernels at run time. A static binary has no loader that
    checks the CPU, so a guard compiled for the base ISA runs from `.preinit_array`, before every constructor, and
    exits with a message instead of a SIGILL.
  - The allocator restart of ADR 0017 is kept unchanged: mimalloc still reads its options in a constructor.
- **The shipped binary is the tested binary.** `pixi run static` packages the binary with its third-party notices,
  `BUILDINFO.json` and a README (on Linux the line tables go to a separate `.debug` asset; the binary keeps its
  symbols for perf), then checks the packaged file: linkage, link flags, build identity (`antb1 version --verbose`),
  the restart, codec, thread and error smoke tests on files DuckDB writes at run time, and every CLI golden
  (`ANTB1_CLI_UNDER_TEST`). `pixi run static-test` runs every hermetic test against the static Arrow.
- **CI:** `static.yml` builds the three targets on PRs that touch the build and on every push to main (advisory, not
  part of `CI OK`; the prefix cache is written from main only); the nightly builds them from scratch and runs
  `static-test`. Moving every performance test to this binary (after a measured comparison with today's `bench`
  build) and publishing it with GitHub releases (after the maintainers choose a project licence) follow separately.

## Consequences

- One file per target that runs without pixi, the repository or a compatible glibc, and that names its build
  (`antb1 version --verbose`, the bench JSON's `flavor` and `cpu_baseline`, and its commit even on a host without
  git). It is 46 MB on linux-x86_64, 13 MB as a tarball, and starts in a twentieth of the time: `antb1 version`
  takes 3.0 ms with the allocator restart (1.5 ms without) against 63.5 ms (32.4 ms) for the dynamic `ci-release`
  build, medians of 300 interleaved runs on a 64-core Linux host (2026-10-25).
- ADR 0002 still holds for every other environment; only `static` takes Arrow from source. An Arrow upgrade updates
  `cmake/static/arrow-sources.lock` with pixi.lock (the dependency-update recipe), and any change of the inputs makes
  a new prefix, built from scratch (about a minute on a 64-core host).
- The static Arrow differs from conda's: Clang instead of GCC, other optimization, and its own versions of re2,
  utf8proc, brotli and zlib. `static-test` and the goldens show where results differ; expectations are never edited
  to hide it.
- The binary needs a CPU from the baseline on (Haswell and newer on x86-64, Neoverse N1 or Graviton 2 and newer on
  aarch64). The guard turns an older CPU into a message; a portable build is one environment variable away
  (`ANTB1_STATIC_CPU_FLAGS`).
- Static glibc cannot load NSS modules or gconv converters from the host, and clock reads are system calls. antb1
  uses none of the former, and the cost of the latter is measured before the perf tests move.
- A renamed copy of the binary skips the restart and runs about 5% slower (ADR 0017); the package ships it as
  `antb1`.
- Alternatives considered: conda-forge's static archives (none exist for Arrow, snappy, brotli, re2, utf8proc or
  thrift); linking glibc dynamically (keeps the loader and a host glibc requirement; it stays the fallback); a newer
  sysroot for the vDSO (the environment's dynamic test executables would then need glibc 2.34 or newer on the host);
  `-static-pie` (no `rcrt1.o` in the aarch64 sysroot, and start-up relocations); full LTO (slower, more memory on
  CI runners than ThinLTO).
