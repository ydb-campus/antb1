# 34. The project licence and the sources of the static binary

Date: 2026-10-25

## Status

Proposed

## Context

- **No licence:** the repository was public without one. README.md and CONTRIBUTING.md said so, and GitHub reported
  no licence. Yet `static.yml` already published the standalone binary
  ([ADR 0033](0033-standalone-static-binary.md)): main's `antb1-<target>` artifacts, kept 30 days, and the macOS
  binary of a failed run.
- **What the binary contains:** Apache Arrow and the libraries it bundles (Apache-2.0, MIT, BSD, zlib), CLI11
  (BSD-3-Clause), Boost's headers through Thrift (BSL-1.0), and the GCC runtime (GPL-3.0 with the runtime library
  exception). On Linux it also contains the GNU C Library 2.28, linked statically. On macOS it contains libc++
  (Apache-2.0 with the LLVM exception).
- **Who wrote it:** one human author wrote every commit except the bots' lock and action updates. The tree vendors
  no third-party code.
- **What glibc's licence asks:** glibc is LGPL-2.1-or-later. Its section 6 allows a statically linked program under
  any terms that permit modification for the user's own use and reverse engineering to debug it. Each copy must
  carry a prominent notice and the licence text. The program must also come with glibc's complete source and the
  rest of the program, as source or object code, so the user can relink it with a modified glibc (6a), offered from
  the same place as the binary (6d).
- **Where glibc comes from:** conda-forge's `sysroot_linux-64` and `sysroot_linux-aarch64` builds repackage
  AlmaLinux 8.9's binary RPMs unchanged. One AlmaLinux source RPM, `glibc-2.28-236.el8_9.13.src.rpm`, is the source
  for both targets.

## Decision

- **Apache-2.0:**
  - `LICENSE` is the Apache License 2.0, byte for byte, with its appendix unfilled.
  - `NOTICE` holds two lines, `antb1` and `Copyright 2026 Aleksandr Khoroshilov`, the copyright holder. It names
    nothing else: the tree bundles no third-party code.
  - Source files carry no licence headers.
  - Contributions come under the same terms (Apache-2.0 section 5), with no CLA and no sign-off (CONTRIBUTING.md).
  - Code from elsewhere needs a licence compatible with Apache-2.0, with its notices kept and its origin named in
    the PR.
  - `/LICENSE` and `/NOTICE` are governance paths: a maintainer changes them.
- **The package carries its licences.** `scripts/static_package.py notices` writes three texts, which go into both
  the package and its `.debug` tarball:
  - LICENSE and NOTICE, the repository's own;
  - THIRD_PARTY_NOTICES.txt: every linked library's licence, one section each, labelled with the library and its
    version (Arrow's LICENSE and NOTICE cover the code Arrow bundles).
  - `static_gates.py package` checks the packed tarballs.
- **The LGPL route on Linux: every package comes with its sources.**
  - **The notice:** THIRD_PARTY_NOTICES.txt starts with the LGPL notice, and README.txt repeats it. The notice says
    where the sources are and how to relink. The licences in the package permit modification and reverse
    engineering to debug it.
  - **The pinned source:** `cmake/static/glibc-source.lock` maps each sysroot build to its source RPM, by sha256 and
    size, checked against the AlmaLinux vault's repodata. `pixi run static` refuses to package with an unmapped
    sysroot build, and a test fails a lock refresh that brings one, until a human pins the new source.
  - **The tarball:** `pixi run static-sources` writes `antb1-<target>.sources.tar` next to the package. It holds:
    - the source RPM;
    - antb1's source at the packaged commit, which must be clean and checked out;
    - `third-party/`: the pins of `cmake/static/arrow-sources.lock`, and the conda-forge archives of CLI11 and
      Boost's headers, by the environment's own conda-meta;
    - a README with the rebuild and relink steps. It says that they install env `static`, and with it TPC's dbgen
      under the TPC EULA v2.2 ([ADR 0006](0006-test-strategy-and-data-policy.md)).
  - **How it is offered:** from the same place as the binary. `static.yml` uploads a Linux package only after
    `antb1-<target>-sources`, in the same run, and every GitHub release ([ADR 0035](0035-github-releases.md)) carries
    both sources tarballs, `antb1-linux-x86_64.sources.tar` and `antb1-linux-aarch64.sources.tar`, next to the packages.
  - **The copies are checked:** every copy of the source RPM and of the conda archives
    (`pixi run static-sources --check-urls`) is checked in the `static urls` job of every PR that changes the lock
    or the script.
  - **The GCC runtime is left out.** The binary also links GCC 15.3's libstdc++.a, libgcc.a, libgcc_eh.a,
    crtbeginT.o and crtend.o (conda-forge's libstdcxx-devel and libgcc-devel, pinned in pixi.lock). The notices say
    so, and say that `pixi run static` links that build again. LGPL-2.1 exempts from the materials "anything that is
    normally distributed (in either source or binary form) with the major components (compiler, kernel, and so on)
    of the operating system on which the executable runs, unless that component itself accompanies the
    executable". Whether statically linked runtime archives count as the compiler accompanying the executable is
    an open question for the legal review below.
    - If it finds that they do, the sources tarball adds the two conda archives (object code, about 24 MB on
      linux-64; `COMPILED_PACKAGES`).
    - It also needs directions to GCC 15.3.0's corresponding source, which GPL-3 section 6 then asks for, and the
      notice would say "source or object code".
- **macOS links no LGPL library.** Its package adds libc++'s licence; the LLVM exception would not require it.

## Consequences

- antb1 can be used, changed and redistributed under Apache-2.0. GitHub and Scorecard detect the licence.
- Each Linux package has a companion of about 66 MB, and each Linux leg of `static.yml` downloads it on every run:
  64 MB on a CI runner, nothing locally once the cache `$ANTB1_ARROW_DIR/src` holds it.
- The SRPM has two copies, the AlmaLinux vault and one mirror. If neither serves it, the Linux legs of
  `static.yml` fail (they are advisory), and earlier artifacts stay.
- The Linux packages that `main` uploaded before this change carry neither a licence nor their sources. The
  maintainers delete them once this change is on `main` and the first run has uploaded packages with sources.
- **A lock refresh can block on the sysroot.** pixi.toml allows any 2.28 build. One that changes the sysroot build
  blocks until `glibc-source.lock` names its source (docs/recipes/dependency-update.md). A newly linked library
  needs a compatible licence, its notice in the package and its source in the sources tarball.
- This decision is not legal advice. An open-source office or a lawyer should review the LGPL route before the first
  public release.
- **Alternatives considered:**
  - A written offer valid for three years (6c): someone must keep the promise and the files for that long.
  - An object-code relink kit: about 260 MB of ThinLTO archives per target, relinkable only with the same LLVM.
  - Linking glibc dynamically: ADR 0033's fallback, which stays the exit if this route ever becomes impractical.
    It gives up the binary that runs on any distribution.
  - Per-file SPDX headers: not needed for the grant.
