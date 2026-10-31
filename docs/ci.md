# Continuous integration

CI runs on GitHub Actions. Every gate runs through the same `pixi run <task>` commands you use locally, so any CI
failure can be reproduced on a Linux machine (or a Mac, for the macOS leg) without GitHub. The governance decisions
behind this setup are in [ADR 0009](adr/0009-ci-and-governance.md).

## Workflows

| Workflow | File | Triggers | Purpose |
| --- | --- | --- | --- |
| `CI` | `.github/workflows/ci.yml` | pull requests to `main`, pushes to `main`, merge queue, daily schedule, manual | build, test, lint, ClickBench data tests; required check `CI OK`; cache clean-up on `main` |
| `PR title` | `.github/workflows/pr-title.yml` | pull requests (opened, edited, reopened, synchronized), merge queue | Conventional Commits title; required check `PR title` |
| `CodeQL` | `.github/workflows/codeql.yml` | every pull request to `main` (no paths filter), pushes to `main`, weekly, manual | code scanning of the C++ code and the workflows; a merge gate through the ruleset ([CodeQL](#codeql)) |
| `Nightly` | `.github/workflows/nightly.yml` | daily schedule, manual | deep checks, advisory; a failure opens a `nightly-failure` issue ([Nightly](#nightly)) |
| `static` | `.github/workflows/static.yml` | pull requests that change the build (`pixi.toml`, `pixi.lock`, presets, `cmake/**`, any `CMakeLists.txt`, `src/cli/**`, `src/common/**`, `LICENSE`, `NOTICE`, the static scripts, the workflow), pushes to `main`, manual | the standalone static binary on its three targets and the sources of the Linux packages, advisory ([Standalone static build](#standalone-static-build)) |
| `Release` | `.github/workflows/release.yml` | pushes of `v*` tags (only admins create tags); manual runs, which are dry runs | a GitHub release of the standalone binary: preflight, a cold build, checks on fresh runners, provenance, a draft, a maintainer's approval, publication and a user's install on each target; never on pull requests ([Releases](#releases)) |
| `Security` | `.github/workflows/security.yml` | pushes and pull requests that change `.github/**`, weekly, manual | zizmor with its online audits, results in the Security tab; advisory |
| `Scorecard` | `.github/workflows/scorecard.yml` | pushes to `main`, weekly, branch protection changes | OpenSSF Scorecard, results in the Security tab and on scorecard.dev; advisory |
| `Agent bootstrap` | `.github/workflows/agent-bootstrap.yml` | pull requests that change `scripts/agent-setup.sh`, `pixi.toml`, `pixi.lock` or the workflow; weekly; manual | a cold `scripts/agent-setup.sh` from both pixi sources, then the tests; advisory |
| Dependabot | `.github/dependabot.yml` | weekly (Monday) | one grouped PR that updates the pinned GitHub Actions, for releases at least 7 days old |
| `Claude` | `.github/workflows/claude.yml` | `@claude` in issues, PR comments and reviews; issue label `claude` | advisory: Claude Code works on the issue or PR (write-access gate, no forks) |
| `Claude review` | `.github/workflows/claude-review.yml` | pull requests opened, ready for review or reopened | advisory: one automatic Claude review per PR, inline comments |
| `Codex harness audit` | `.github/workflows/codex-review.yml` | pull requests opened, ready for review or reopened; label `codex-review` | advisory: read-only Codex audit, one sticky comment |
| `Copilot Setup Steps` | `.github/workflows/copilot-setup-steps.yml` | every Copilot coding-agent task; manual; changes to itself, `pixi.toml` or `pixi.lock` | prepares Copilot's environment (`default` and `lint`, dev build) |
| `pixi.lock update` | `.github/workflows/pixi-lock-update.yml` | weekly (Monday), manual | PR `build(deps): update pixi.lock` from the antb1-bot App |
| `Benchmarks` | `.github/workflows/bench.yml` | pushes to `main` that touch `src/`, `bench/`, `tools/fixturegen/`, `cmake/`, `CMakeLists.txt`, `CMakePresets.json`, `pixi.toml`, `pixi.lock`, the static and bench scripts or `bench.yml`; pull requests labeled `performance`; manual | micro benchmarks and ClickBench timings on the standalone static build; publishes the micro series from `main` to `gh-pages` (`dev/bench-static`); advisory, never gating ([benchmarks.md](benchmarks.md)) |

The AI and bot workflows (Claude, Claude review, Codex harness audit, Copilot Setup Steps, pixi.lock update) and
`Benchmarks` are advisory: they are not required checks and not part of `CI OK`, and their failures never block a
merge. The AI workflows' setup, secrets and security model are described in [docs/agents.md](agents.md).

## Required checks

A pull request can merge only when:

- `CI OK` is green. It is an aggregator job (re-actors/alls-green) that runs even when other jobs fail or are skipped
  and succeeds only if every job it depends on succeeded. New CI jobs are added to its `needs` list, so the required
  check name never changes.
- `PR title` is green: the title is a Conventional Commit with a lowercase subject, and one of the types `feat`,
  `fix`, `perf`, `refactor`, `test`, `docs`, `build`, `ci`, `chore`, `revert`.
- One human maintainer approved and every review thread is resolved. AI reviews never count.
- Once ruleset stage B is applied (`tools/github/ruleset-main.json`), code scanning reports no new error-level
  alerts and no security alerts of high severity or above. **A CodeQL failure, or a missing CodeQL analysis, blocks
  all merges** until it is fixed or an admin bypasses the rule through the PR.

The branch does not need to be up to date with `main` (the ruleset's strict mode is off), so there is no need to
press "Update branch" unless there are conflicts. Push-to-main CI and the daily run catch the rare semantic conflict;
a merge queue (`merge_group` is already wired) can take over when PR volume needs it.

## Jobs and local commands

| CI job | Runner | Environment | Reproduce locally |
| --- | --- | --- | --- |
| `lint (pixi run lint)` | ubuntu-24.04 | `lint` | `pixi run lint` |
| `clang-asan (pixi run asan)` | ubuntu-24.04 | `default` | `pixi run asan` |
| `gcc-compat (pixi run ci-gcc)` | ubuntu-24.04 | `gcc` | `pixi run ci-gcc` |
| `macos-release (pixi run release)` | macos-15 (arm64) | `default` | `pixi run release` (on a Mac) |
| `clang-tidy (pixi run tidy)` | ubuntu-24.04 | `default` | `pixi run tidy` |
| `clang-coverage-fuzz (pixi run coverage && pixi run fuzz-smoke)` | ubuntu-24.04 | `default` | `pixi run coverage` · `pixi run fuzz-smoke` |
| `clickbench-hits0 (pixi run test-data)` | ubuntu-24.04 | `default` | `pixi run test-data` (downloads 122 MB once) |
| `coverage-comment` (advisory) | ubuntu-slim | none | read `build/coverage/summary.md` after `pixi run coverage` |
| `cache-gc` (`main` only) | ubuntu-slim | none | nothing to run |
| `CI OK` | ubuntu-slim | none | aggregator, nothing to run |
| `bench-run (pixi run bench && pixi run bench-clickbench)` (advisory) | ubuntu-24.04 | `static` | `pixi run bench` · `pixi run bench-clickbench` |
| `bench-publish` (advisory, `main` only) | ubuntu-24.04 | none | nothing to reproduce: appends `micro.json` to the `gh-pages` series `dev/bench-static` |
| `static <target> (pixi run static && pixi run static-check build/static/package/antb1-<target>.tar.gz && pixi run static-sources)` (advisory) | ubuntu-24.04, ubuntu-24.04-arm, macos-15 | `static` | `pixi run static`, `pixi run static-check build/static/package/antb1-<target>.tar.gz` (the verify step; on Linux with docker when it answers), then `pixi run static-sources` (on that platform) |
| `static urls (pixi run static-arrow --check-urls && pixi run static-sources --check-urls)` (advisory, PRs that change a lock of the pinned sources) | ubuntu-24.04 | `static` | `pixi run static-arrow --check-urls` · `pixi run static-sources --check-urls` |
| `preflight (python3 -I .github/scripts/release.py preflight)` (`Release`) | ubuntu-slim | none | in a checkout of the commit, with `gh` logged in: `GITHUB_REPOSITORY=ydb-campus/antb1 GITHUB_EVENT_NAME=push GITHUB_REF=refs/tags/<tag> GITHUB_SHA=<commit> pixi run -e lint python -I .github/scripts/release.py preflight` (it only reads; `GITHUB_EVENT_NAME=workflow_dispatch` with `GITHUB_REF=refs/heads/main` gives a dry run's warnings) |
| `build <target> (pixi run static && pixi run static-sources)` (`Release`) | ubuntu-24.04, ubuntu-24.04-arm, macos-15 | `static` | in a clean checkout of the commit (network): `CCACHE_DISABLE=1 ANTB1_ARROW_DIR=<an empty directory> pixi run static`, then on Linux `pixi run static-sources` with the same `ANTB1_ARROW_DIR` |
| `assemble (python3 -I .github/scripts/release.py assemble)` (`Release`) | ubuntu-24.04 | none | `gh run download <run> -R ydb-campus/antb1 -p 'antb1-*' -D <dir>`, then, in a checkout of the commit, `pixi run -e lint python -I .github/scripts/release.py assemble --in <dir> --out <a new directory>` with `GITHUB_REPOSITORY=ydb-campus/antb1` and preflight's outputs as `RELEASE_TAG`, `RELEASE_SHA`, `RELEASE_VERSION`, `RELEASE_PRERELEASE` and `RELEASE_PUBLISH`, and `gh` logged in |
| `verify <target> on <os> (pixi run static-check antb1-<target>.tar.gz)` (`Release`) | ubuntu-24.04 and ubuntu-24.04-arm (both with docker), macos-15, macos-26 | none (the runner's `python3`) | `pixi run static-check build/static/package/antb1-<target>.tar.gz` after `pixi run static` on that platform; for the run's own files `gh run download <run> -R ydb-campus/antb1 -n antb1-<target> -D <dir>`, then `pixi run static-check <dir>/antb1-<target>.tar.gz` |
| `attest (actions/attest)` (`Release`, tag runs only) | ubuntu-24.04 | none | nothing to reproduce: tag runs only |
| `draft (python3 -I .github/scripts/release.py draft)` (`Release`, tag runs only) | ubuntu-24.04 | none | nothing to reproduce: tag runs only |
| `publish (python3 -I .github/scripts/release.py publish)` (`Release`, tag runs only) | ubuntu-24.04 | none | nothing to reproduce: tag runs only; it waits for a maintainer's approval of the `release` environment |
| `check-published <target> (python3 -I .github/scripts/release.py check-published)` (`Release`, tag runs only) | ubuntu-24.04, ubuntu-24.04-arm, macos-15 | none | on that platform, in a checkout of the tag, with `gh` logged in: `GITHUB_REPOSITORY=ydb-campus/antb1 RELEASE_TAG=<tag> RELEASE_SHA=<commit> RELEASE_VERSION=<version> RELEASE_PRERELEASE=<true for an rc, else false> RELEASE_PUBLISH=true RELEASE_LATEST=<true or false> pixi run -e lint python -I .github/scripts/release.py check-published --target <target> --work <a new directory>`; it only reads, so it checks any published release |
| `PR title` | ubuntu-slim | none | check the title against the rules above |

Advisory jobs (not required):

| Job | Workflow | Runner | What it runs |
| --- | --- | --- | --- |
| `claude (@claude)` | `Claude` | ubuntu-24.04 | Claude Code with named `pixi run` tasks and read-only `git`/`gh` only; 60 min |
| `claude-review (AGENTS.md review rules)` | `Claude review` | ubuntu-24.04 | Claude Code with one review prompt, read-only `git`/`gh` and no file edits; fails when Claude did not post its summary (advisory, not in `CI OK`); 30 min |
| `codex-audit (harness checklist)` | `Codex harness audit` | ubuntu-24.04 | Codex CLI 0.155.1, `:read-only` profile, no sudo; 30 min |
| `codex-audit comment` | `Codex harness audit` | ubuntu-slim | posts the audit as one sticky comment |
| `copilot-setup-steps` | `Copilot Setup Steps` | ubuntu-24.04 | `pixi install --locked -e lint`, then `pixi run --frozen -e default build` |
| `pixi update` | `pixi.lock update` | ubuntu-24.04 | `pixi update` with pixi 0.81.0 and a PR through the antb1-bot App token |

- `lint` is read-only: format checks, linters, offline zizmor, the repository drift and policy checks and their own
  tests. On pull requests it also runs GitHub's dependency review. `pixi run fmt` fixes what can be fixed
  automatically. Locally it checks tracked and untracked (non-ignored) files, the same set CI sees after commit.
- `clang-asan` is a Clang Debug build with AddressSanitizer and UndefinedBehaviorSanitizer and `-Werror`, followed by
  the hermetic tests except the label `tpc-sf1`: the ratchet of the queries derived from TPC-DS at scale factor 1,
  whose shards come close to their timeout under ASan ([ADR 0036](adr/0036-union-all.md)). `gcc-compat` and
  `clang-coverage-fuzz` run it on every PR, and the nightly `asan-tpc-sf1` runs it under ASan
  ([testing.md](testing.md#labels)).
- `gcc-compat` builds with GCC 15 and GCC-only warnings, with `-Werror`, and runs the hermetic tests.
- `macos-release` is a RelWithDebInfo build with `-Werror` against libc++ on Apple silicon, followed by the hermetic
  tests; it also builds the micro benchmarks and runs each once (`bench.micro.smoke`).
- `clang-tidy` runs clang-tidy on every translation unit, with warnings as errors.
- `clang-coverage-fuzz` runs two Clang-only gates. `pixi run coverage` builds with source-based coverage, runs the
  hermetic tests and fails when a module drops below its floor in `tools/ci/coverage_thresholds.json`; its per-module
  table goes to the job summary and to the `coverage-summary` artifact. `pixi run fuzz-smoke` then runs even when
  coverage failed: a deterministic libFuzzer run of the SQL parser (seed 1, 200,000 runs, ASan and UBSan) and the
  corpus replay. On a fuzz failure the crash inputs are uploaded as the `fuzz-artifacts` artifact for 14 days. See
  [testing.md](testing.md#coverage) for the floors and [testing.md](testing.md#fuzzing) for fuzzing.
- `clickbench-hits0` runs the ClickBench data tests ([testing.md](testing.md#clickbench-data-tests)) on the
  `ci-release` build: antb1 against DuckDB on the pinned `hits_0` partition, metamorphic relations on it and the
  ClickBench ratchet. `ANTB1_DATA_DIR` points into the workspace (`.cache/clickbench`), which is restored from the
  Actions cache under a key made of the hash of `tools/data/clickbench.lock`; on a miss `pixi run fetch-data`
  downloads the files, and a run on `main` saves them. The output is redacted, the JUnit file is
  `build/ci-release/junit-data.xml`, and the job uploads nothing: no logs and no data.
- `coverage-comment` is advisory and not part of `CI OK`. On pull requests from branches of this repository it posts
  the coverage table as one sticky PR comment and updates it on every push. It only downloads the
  `coverage-summary` artifact, never checks out or runs PR code, and is the only job with `pull-requests: write`.
  Fork PRs get no comment (their token cannot write); the table is still in the job summary.
- `cache-gc` runs after every run on `main` (never on pull requests or in the merge queue) with `actions: write`. It
  deletes the pixi caches of other `pixi.lock` generations, the data caches of other `tools/data/clickbench.lock`
  files, all but the newest ccache of each leg and all but the newest static Arrow prefix of each target, then lists
what is left in the job summary. A run whose commit is
  no longer the head of `main` deletes nothing, so it never removes the caches of a newer lock. Pull requests still
  on an older `pixi.lock` install their environments without a cache until they merge `main`.
- `pixi run ci` (Clang Debug `-Werror` plus tests) is not a separate job; it is the fast local gate inside
  `pixi run check`.
- `pixi run check-full` runs every Linux gate above except the data tests in one command: `check` (lint and `ci`),
  `asan`, `tidy`, `coverage`, `fuzz-smoke` and `ci-gcc`. Run `pixi run test-data` for `clickbench-hits0`.

CI always calls tasks with an explicit environment and a frozen lock, for example
`pixi run --frozen -e default asan` or `pixi run --frozen -e gcc ci-gcc`, after installing pixi 0.81.0 with
prefix-dev/setup-pixi.

### Standalone static build

The release and performance binary ([ADR 0033](adr/0033-standalone-static-binary.md)) has its own environment,
`static`, and seven tasks: the five below, and the benchmarks `pixi run bench` and `pixi run bench-clickbench`, which
build in `build/static` too ([benchmarks.md](benchmarks.md)). The licence texts of the package and the sources that
go with it are [ADR 0034](adr/0034-licence-and-static-sources.md).

- `pixi run static-arrow` (network on the first run) downloads the sources pinned in
  `cmake/static/arrow-sources.lock` and builds Arrow 25 and the dependencies it bundles as static `-O3` ThinLTO
  libraries into `$ANTB1_ARROW_DIR/<key>/prefix` (default `~/.cache/antb1/arrow`). A prefix whose key matches is
  reused; `--print-key` prints the key (the cache key below).
  - Every pinned file lists byte-identical copies of at least two operators: an uploaded GitHub release asset first
    where one exists, a single-server origin such as `archive.apache.org` last. `scripts/static-sources.sh` tries
    them in order until one serves the pinned size and sha256. Each attempt is bounded: 10 s to connect, the pinned
    size, 30 s plus 1 s per 64 KiB. Only an error that may pass gets a second attempt; a dead URL, a timeout or wrong
    bytes move to the next copy.
  - `pixi run static-arrow --check-urls` checks every URL against its pin. Single-server origins get a HEAD request
    only. A dead URL or other bytes, a larger file included, fail the check at once. Any other failure gets one more
    try after the pass: a timeout or an error that may pass is then a warning, anything else (a host that does not
    resolve, a bad certificate, another HTTP error) fails. So does a file with fewer than two good URLs. The
    `static urls` job runs it on every PR that changes the lock, and the nightly `static-urls` job every night.
- `pixi run static` builds antb1 in `build/static` against that prefix, packages it into `build/static/package`
  (`antb1-<target>.tar.gz`, on Linux `antb1-<target>.debug.tar.gz`, `SHA256SUMS`) and checks the package and the
  packaged file. The package checks cover the members (each the staged file the file checks ran), LICENSE and
  NOTICE, the notices, BUILDINFO.json and the checksums; the file checks cover linkage, link flags, build identity,
  the allocator restart, smoke tests and every CLI golden. Both tarballs carry LICENSE, NOTICE and
  `THIRD_PARTY_NOTICES.txt`, which on Linux starts with the LGPL notice of the static glibc. Packaging refuses a
  sysroot build that has no line in `cmake/static/glibc-source.lock`. The smoke tests query the smoke kit in
  `build/static/smoke-kit`: Parquet files that DuckDB writes from `range()` with each codec, a truncated copy, and
  `kit.json` with their digests and DuckDB's own answer (`scripts/static_gates.py kit`). It holds no other data.
- `pixi run static-sources` (network once, Linux; after `pixi run static`) writes
  `build/static/sources/antb1-<target>.sources.tar` and its `.sha256`: the sources that go with the Linux package
  (LGPL-2.1 section 6). It holds glibc's source RPM by `cmake/static/glibc-source.lock` (the AlmaLinux vault, then a
  mirror), antb1's source at the packaged commit, the pins of `cmake/static/arrow-sources.lock`, and the conda-forge
  archives of CLI11 and Boost's headers (conda-forge, then prefix.dev). The package must come from the clean checkout
  it runs in. Downloads go through `scripts/static-sources.sh` into `$ANTB1_ARROW_DIR/src`; a whole pass is retried
  once. On macOS there is nothing to do. The GCC runtime (libstdc++, libgcc) is left out: it comes with the compiler
  that pixi.lock pins (ADR 0034). `pixi run static-sources --check-urls` checks every copy of the source RPMs and of
  the conda archives instead, as `--check-urls` does for Arrow's pins; the `static urls` job runs it on every PR that
  changes `cmake/static/glibc-source.lock` or `scripts/static_package.py`.
- `pixi run static-check [BIN]` runs those checks, except the goldens, on any standalone binary, such as a copy on
  a benchmark host. `pixi run static-check antb1-<target>.tar.gz`, with `SHA256SUMS` next to it (and on Linux
  `antb1-<target>.debug.tar.gz`), checks a package as a fresh runner does (`scripts/static_gates.py verify`):
  - the digests, then each tarball before anything is unpacked: one directory, exactly the package's members,
    regular files only, no extended attributes;
  - then the unpacked files (BUILDINFO.json, LICENSE and NOTICE against the checkout, on macOS the signature) and the
    smoke tests, with an empty environment in an empty directory;
  - on Linux, when `docker info` answers, the smoke tests again in a container of an image that holds only the binary
    and the kit: no network, no capabilities, a read-only root, user nobody.

  verify uses only Python's standard library (3.11 or newer with tarfile's data filter). It exits 2 when it cannot
  check the package where it runs.
- `pixi run static-test` builds every target of `build/static` and runs all hermetic tests against the static Arrow.

The `static` workflow runs `pixi run static` and `pixi run static-sources` on linux-x86_64 (ubuntu-24.04),
linux-aarch64 (ubuntu-24.04-arm) and darwin-arm64 (macos-15). Between the two, it verifies the package with the
runner's own `python3`, not pixi's, as a runner without pixi would, with the Linux packages also in a Docker
container. It is advisory and not part of `CI OK`. Its pixi environment is not cached. The Arrow prefix is restored
from `static-arrow-<target>-<key>` and saved from pushes to `main` only. Runs on `main` and manual runs upload the
package as the `antb1-<target>` artifact; on Linux they first check and upload its sources as
`antb1-<target>-sources`, so a package is never published without them. A failed macOS run keeps its binary, with
the licence texts, as `failed-antb1-<target>`. These runs are cold until `main` has saved a prefix (a cold build
takes about a minute on a 64-core host; the CI runners are slower). The artifacts of `main` serve the perf hosts
between releases; a release never reuses them: `release.yml` builds its own from scratch ([Releases](#releases)).

## Releases

A GitHub release publishes the standalone binary of a commit of `main` for its three targets, with the sources of
the Linux packages ([ADR 0035](adr/0035-github-releases.md)). Releases come only from `.github/workflows/release.yml`,
whose logic is `.github/scripts/release.py` and `scripts/static_gates.py`, run by the runner's own `python3`;
`pixi run release`, the RelWithDebInfo test leg, has nothing to do with them. The maintainers cut releases: an admin
pushes the tag and a maintainer approves the publication. That may be the same person, so one maintainer can release
alone. Agents never push a tag or start a GitHub release (AGENTS.md), and the Claude Code settings deny tag pushes,
`gh release` writes, `gh workflow run release.yml` and deployment approvals (CLAUDE.md).

### What a release holds

Nine assets, whose names carry no version:

| Asset | What it holds |
| --- | --- |
| `antb1-linux-x86_64.tar.gz`, `antb1-linux-aarch64.tar.gz`, `antb1-darwin-arm64.tar.gz` | the package of `pixi run static`, in one directory `antb1-<version>-<target>/`: `antb1`, `LICENSE`, `NOTICE`, `THIRD_PARTY_NOTICES.txt`, `BUILDINFO.json` and `README.txt` |
| `antb1-linux-x86_64.debug.tar.gz`, `antb1-linux-aarch64.debug.tar.gz` | the line tables of the Linux binary, `antb1.debug` with the same licence texts, which unpack next to the binary |
| `antb1-linux-x86_64.sources.tar`, `antb1-linux-aarch64.sources.tar` | the sources that go with a Linux package (`pixi run static-sources`; LGPL-2.1 section 6, [ADR 0034](adr/0034-licence-and-static-sources.md)), in `antb1-<target>-sources/`: glibc's source RPM, antb1's source at the commit, the third-party sources and a `README.txt` on how to rebuild and relink |
| `SHA256SUMS` | the sha256 of the seven tarballs |
| `antb1.sigstore.json` | the Sigstore bundle of the build provenance of the seven tarballs and `SHA256SUMS`, for checks without network |

The bundle itself is neither a subject of the provenance nor in `SHA256SUMS`, and glibc's source RPM is no asset of
its own: both sources tarballs hold it. As the names carry no version, the link to a file of the latest stable
release never changes, for example `https://github.com/ydb-campus/antb1/releases/latest/download/SHA256SUMS`;
README.md's install block downloads from there. A prerelease is never the latest release.

Users check a download as [SECURITY.md](../SECURITY.md#verifying-a-release) describes: `SHA256SUMS` shows that the
file is intact, the provenance that `release.yml` built it from the tag on a GitHub-hosted runner, and GitHub's
release attestation that it is an asset of the published release.

### Versions

- The version is `project(VERSION)` in `CMakeLists.txt`, which `antb1 version` prints. `version` in `pixi.toml`
  must be the same; preflight checks both. The first version is 0.1.0, which both files already name.
- SemVer 0.x: the minor number grows with features and breaking changes, the patch number with fixes only. Nothing
  is backported (SECURITY.md).
- A release is the annotated tag `v<VERSION>`. A tag `v<VERSION>-rc.<N>`, with N counting from 1, is a release
  candidate of that version: a prerelease, never the latest release. Its binary reports the final version, and its
  `BUILDINFO.json` records the tag (`tag`), so an rc's package names its release.
- Tags never move: nobody, admins included, updates or deletes a `v*` tag, and the tag of a deleted immutable release
  can never be used again. So a mistake costs a version: a wrong tagged commit is followed by the next version (or
  the next rc), never by a moved tag.
- Between releases, builds of `main` report the last version; `antb1 version --verbose` and `BUILDINFO.json` name
  their commit.

### Release settings

They are files in `tools/github/`, applied by `tools/github/apply-settings.sh --release`:

- `tools/github/ruleset-tags.json`, the ruleset `tags`: only admins create, update or delete a tag, any tag, so
  nobody else can start a release or publish one on a new tag that would take over the latest release.
- `tools/github/ruleset-release-tags.json`, the ruleset `release-tags`: nobody, admins included, updates or deletes a
  `v*` tag, and a new `v*` tag needs a green `CI OK` on its commit.
- `tools/github/environment-release.json`, the environment `release`: the publish job waits until one of its
  reviewers, the two maintainers, approves. Self-approval is allowed (`prevent_self_review` is false), so the admin
  who pushed the tag may approve the run. Admins cannot bypass the approval, and only `v*` tags deploy to it.
  Two-person approval can come back with a settings PR that sets `prevent_self_review` to true.
- Immutable releases: once a release is published, its assets and its tag never change; its title and notes stay
  editable.

An admin applies them right before a PR that changes them merges, the first time before the PR that adds
`release.yml`: `bash tools/github/apply-settings.sh --release` is a dry run that prints every write and reads back
what GitHub has, then `--apply --release` applies them, and its read-back must show no `DIFF`. In this order no tag
meets `release.yml` without its settings; a run that names a missing environment would create it without any
protection. preflight compares the environment and the two rulesets with their files and fails a tag push on any
difference. It cannot see the bypass lists or the immutability setting: the admin's read-back shows those, and
publish requires the published release to be immutable.

### Cutting a release

1. **The version.** `main` names it ([Versions](#versions)). For a new version, a PR `chore(release): <version>`
   changes `CMakeLists.txt` and `pixi.toml`.
2. **A dry run.** `gh workflow run release.yml --ref main`, then wait until it passes. preflight reports as warnings
   what a tag push would refuse; build, assemble and verify run in full; nothing is attested, drafted or published.
3. **The tag.** Right after `CI OK` is green on the head of `main`, an admin tags that commit with an annotated tag,
   whose message opens the release notes, and pushes the tag alone:

   ```bash
   git fetch origin
   git tag -a v0.2.0 -m "antb1 0.2.0" -m "<what changed for users, with every breaking change>" <commit>
   git push origin v0.2.0
   ```

   Merge nothing that changes `.github/workflows/` until the release is published: GitHub's Releases API may refuse
   the workflow token a release whose commit differs from `main` in `.github/workflows/` (preflight warns).
4. **The run.** preflight checks the tag, the version, `main`, `CI OK` and the settings. It takes `CI OK` from the
   newest `ci.yml` run on the commit and that run's `CI OK` job, through the Actions API, never from check runs:
   any workflow can create a check run named `CI OK`. The three targets are built
   from scratch. assemble checks every file against its checksums, the pins and the commit, and writes the one
   `SHA256SUMS` and the notes. verify checks each package on fresh runners without pixi: ubuntu-24.04 and
   ubuntu-24.04-arm, there also in a FROM-scratch container without network, macos-15 and macos-26. Only then does
   attest sign the provenance and draft upload the nine assets to a draft release. The digests reach attest, draft
   and publish as assemble's job output, which no later job can replace (any job of the run can overwrite an
   artifact).
5. **The approval.** publish waits for the `release` environment. Whoever approves, the admin who pushed the tag
   included, checks first:
   - the release path since the last release, because `release.yml` runs as the tagged commit has it:
     `git diff <last tag> <tag> -- .github/ scripts/ cmake/ tools/github/ CMakePresets.json pixi.toml pixi.lock`;
   - the draft on the releases page (only collaborators with write access see drafts): its nine assets and its notes;
   - for the first release, that the LGPL review of [ADR 0034](adr/0034-licence-and-static-sources.md) is done.

   Then approve the deployment on the run's page ("Review deployments"), or reject it
   ([When a release fails](#when-a-release-fails)). Approve or reject within 30 days: a deployment that waits
   longer fails, and the run can then no longer be re-run.
6. **Publication.** publish checks every asset of the draft once more against assemble's digests and the
   provenance and publishes the draft. Then it checks two things: that the API reports the release immutable, and
   GitHub's release attestation (`gh release verify`). Only then does the release become the latest one, and only
   when it is the highest stable version: README.md's install block downloads the latest release. Then
   check-published runs that block on each target's runner, as a user does, and the checks that SECURITY.md
   documents.
7. **The rulesets' verdict.** Look up the push's rule suite, then read its rule evaluations: the list covers the
   last day unless asked for a month, and only a suite's own page shows each ruleset's result.
   `gh api -X GET repos/ydb-campus/antb1/rulesets/rule-suites -f ref=refs/tags/<tag> -f time_period=month` gives
   its id, and `gh api repos/ydb-campus/antb1/rulesets/rule-suites/<id>` must show `release-tags`'
   `required_status_checks` with result `pass`.

### The first release

The first release, v0.1.0, comes after a release candidate, since only a tag runs attest, draft, the approval and
publish:

1. Right before the PR that adds `release.yml` merges, an admin applies the settings (`--release`, then
   `--apply --release`) and pastes the clean read-back into the PR. That PR may merge with the admin bypass.
2. `CI OK` is green on its merge commit, and a dry run (`gh workflow run release.yml --ref main`) passes.
3. The LGPL review of ADR 0034 (an open-source office or a lawyer) is done: a prerelease is public too.
4. An admin pushes the annotated tag `v0.1.0-rc.1` alone, checks the draft, approves the `release` environment,
   waits for check-published and reads the tag's rule suite (step 7 above).
5. Then `v0.1.0`, the same way. It needs no version PR: `main` already names 0.1.0.

### Release notes

assemble writes the notes (`notes.md` in `release-meta`), and the draft carries them:

- the tag's message, without a signature, and for an rc a note that it is a prerelease;
- fixed sections: the assets with their digests, the platforms and their floors (x86-64-v3, Armv8.2-A, macOS 13.0),
  the install commands (curl and the checksum), how to verify (the commands of SECURITY.md) and the licences
  (Apache-2.0, `THIRD_PARTY_NOTICES.txt`, glibc under LGPL-2.1 and where its sources are).

The tag message is the only free text, so its author keeps the rules: what changed for a user, with every breaking
change; no measured figures, no benchmark wording and no list of pull requests. The notes hold at most 125,000
characters. They stay editable after publishing; the assets and the tag do not.

### When a release fails

- Before publishing, re-run only the failed jobs ("Re-run failed jobs"). Never "Re-run all jobs": it builds tarballs
  with other bytes and signs a second attestation for the same tag. A re-run of draft first deletes the tag's
  earlier drafts.
- A dry run that fails blocks the tag: fix `main` first.
- If the tagged commit itself is wrong, reject the deployment, delete the draft, fix `main` and release the next
  version (or the next rc). The tag stays. Do not leave the run waiting: release runs go one at a time, so it would
  hold up the run of the next tag.
- publish fails after publishing: first ask the API whether the release is immutable
  (`gh api repos/ydb-campus/antb1/releases/tags/<tag> --jq .immutable`).
  - `false`: immutability was off. Delete that mutable release, turn immutability on
    (`bash tools/github/apply-settings.sh --apply --release`), then re-run all jobs: the release is built and signed
    again. It never became the latest release.
  - `true`: the release is published and stays. "Re-run failed jobs": publish then only verifies it (a late release
    attestation passes then), makes it the latest release if it should be, and check-published follows. Never
    delete it: an immutable release's tag can never be used again.
- publish says its draft is gone (deleted by hand): re-run the draft job ("Re-run this job"), which uploads a new
  draft and runs publish again.
- After publishing, never delete the release: fix forward with the next version.
- Push one tag at a time: a push of more than three tags at once starts no workflow, and release runs go one at a
  time, so a third tag would replace the second one, which waits for the first.
- A publish job that nobody approves fails after 30 days, and GitHub re-runs jobs only within 30 days of a run's
  start, while a `v*` tag can never be pushed again: that tag is spent. Delete its draft and release the next rc or
  version.

## Nightly

`nightly.yml` runs the deep checks on `main` every night (and on demand). It is advisory: nothing blocks a merge,
but every failure is tracked in an issue.

| Job | Runner | Reproduce locally | What it adds |
| --- | --- | --- | --- |
| `fuzz-long` | ubuntu-24.04 | `ANTB1_FUZZ_SECONDS=1200 pixi run fuzz` | 20 minutes of libFuzzer with a random seed; crash inputs are uploaded as `fuzz-long-artifacts` for 14 days |
| `tsan` | ubuntu-24.04 | `pixi run tsan` | the hermetic tests except the label `tpc-sf1` under ThreadSanitizer |
| `asan-data` | ubuntu-24.04 | `pixi run asan-data` | the data tests under ASan and UBSan (redacted, no uploads) |
| `asan-tpc-sf1` | ubuntu-24.04 | `pixi run asan-tpc-sf1` | the ratchet of the queries derived from TPC-DS at scale factor 1 (label `tpc-sf1`) under ASan and UBSan, four shards at a time, which `clang-asan` and `tsan` leave out; redacted output |
| `arm64` | ubuntu-24.04-arm | `pixi run release` then `pixi run test-data` | Linux ARM64: the release build, the hermetic tests and the data tests |
| `diff-extended` | ubuntu-24.04 | `ANTB1_DIFF_SEED=<run id> ANTB1_DIFF_COUNT=20000 pixi run diff-random` | 20,000 random differential queries with the run id as the seed |
| `ci-shuffle` | ubuntu-24.04 | `pixi run ci-shuffle` | the hermetic tests in random order, each repeated until it fails (at most twice) |
| `static-cold <target>` | ubuntu-24.04, ubuntu-24.04-arm, macos-15 | `ANTB1_ARROW_DIR=<an empty directory> pixi run static`, then `pixi run bench --benchmark_dry_run=true` with the same `ANTB1_ARROW_DIR` | the standalone static build from scratch, as a release builds it: every pinned source downloaded again; then the micro benchmarks against the static Arrow, one iteration each (nothing else builds them on macOS and aarch64) |
| `static-urls` | ubuntu-24.04 | `pixi run static-arrow --check-urls` | every URL of the static build's pinned sources against its pin (HEAD only for single-server origins): a dead or wrong copy shows before a cold build needs it |
| `static-test` | ubuntu-24.04 | `pixi run static-test` | every hermetic test in `build/static`, against the static Arrow (the prefix `main` saved); each of its 20 executables is a ThinLTO link over that Arrow, about 4 CPU-hours in all |
| `report` | ubuntu-slim | nothing to run | on any failed or timed-out job: opens an issue labelled `nightly-failure` and `agent-task`, or comments on the open one, with the failing jobs, the run URL and the commands above |

The nightly jobs only read caches: pixi environments and ccache come from the runs on `main` (`asan-data` and
`asan-tpc-sf1` reuse the ccache of `clang-asan`, `fuzz-long` that of `clang-coverage-fuzz`), and the data comes from
the cache that `clickbench-hits0` saved. The ARM64 environments are not cached, to stay within the cache budget. To
work on a nightly failure, take the issue, run its command locally, fix the cause with a regression test and close
the issue from the PR.

## CodeQL

`codeql.yml` analyses the C++ code and the GitHub Actions workflows with the `security-extended` queries and uploads
the results to the Security tab. It runs on every pull request, so the analysis that the ruleset's `code_scanning`
rule requires always exists; it has no `merge_group` trigger because code scanning merge protection does not apply
to merge queue groups.

- `c-cpp` (build mode `manual`): prefix-dev/setup-pixi installs and activates the `gcc` environment, CodeQL is
  initialized, and then a plain `cmake -E rm -rf build/codeql && cmake --workflow --preset codeql` builds `src/` with
  GCC 15 and without ccache, the same build as `pixi run codeql-build`. The build does not go through `pixi run`,
  because the CodeQL tracer does not follow processes through the static pixi binary. CodeQL traces GCC up to 16 but
  Clang only up to 22, which is why this build uses the `gcc` environment
  ([ADR 0002](adr/0002-toolchain-pixi-conda-forge.md)).
- `actions` (build mode `none`): the workflow files.

When CodeQL fails on a pull request, open the job log: a build failure reproduces with `pixi run codeql-build`; a
new alert is listed in the Security tab and on the PR. Fix the code, or, for a false positive, ask a maintainer to
dismiss the alert with a reason. Until then the ruleset blocks the merge (after stage B).

## Security, Scorecard, agent bootstrap and Dependabot

- `Security` runs zizmor 1.30.1 (the version of the `lint` environment, pinned in zizmorcore/zizmor-action) with its
  online audits and uploads SARIF to the Security tab. The blocking check is the offline zizmor in `pixi run lint`;
  this workflow adds the audits that need the GitHub API, such as known-vulnerable action versions. Fork pull
  requests are skipped, because their token cannot upload results.
- `Scorecard` runs OpenSSF Scorecard and publishes the results. Scorecard accepts published results only from a
  restricted workflow: no workflow-level `env` or write permissions, `id-token: write` only in its job, no job `env`,
  and only checkout, upload-artifact, codeql-action/upload-sarif and scorecard-action as steps. The finding
  accepted for now is the admin bypass of the ruleset.
- `Agent bootstrap` checks the shared sandbox bootstrap from scratch, as a Claude, Codex or Copilot sandbox runs it:
  `bash scripts/agent-setup.sh --envs "default lint"` with `ANTB1_PIXI_SOURCE=github` and `ANTB1_PIXI_SOURCE=conda`,
  then `pixi run --as-is test` and `pixi run --as-is doctor --json`. It uses no caches on purpose.
- Dependabot opens one grouped PR per week (`ci(deps): ...`, labels `dependencies` and `github-actions`) that moves
  the SHA pins of the actions to releases at least 7 days old. A new action also needs an entry in
  `tools/github/allowed-actions.json`. `pixi.lock` is not updated by Dependabot.

## Test reports and logs

Test jobs write JUnit XML to `build/<preset>/junit.xml` (`clang-coverage-fuzz`: `build/coverage/junit.xml` and
`build/fuzz/junit.xml`; the data tests: `build/ci-release/junit-data.xml` and, nightly, `build/ci-asan/junit-data.xml`;
`ci-shuffle`: `build/ci/junit-shuffle.xml`; `asan-tpc-sf1`: `build/ci-asan/junit-tpc-sf1.xml`). The results appear as
annotations on the PR and in the job summary. When a hermetic test job fails, the ctest logs and the JUnit files are
uploaded as the `logs-<job>` artifact and kept for 7 days. The build jobs and `clang-tidy` also write their disk
usage to the job summary. Timing and cache measurements are recorded here once enough runs exist.

| Artifact | Job | When | Kept |
| --- | --- | --- | --- |
| `logs-<job>` | every hermetic test job (CI legs, nightly `tsan`, `ci-shuffle` and `asan-tpc-sf1`) | on failure | 7 days |
| `coverage-summary` | `clang-coverage-fuzz` | always (when the report exists) | 7 days |
| `fuzz-artifacts` | `clang-coverage-fuzz` | when the fuzz smoke run fails | 14 days |
| `fuzz-long-artifacts` | nightly `fuzz-long` | when long fuzzing fails | 14 days |
| `scorecard-sarif` | `Scorecard` | always | 5 days |
| `bench-results` | `bench-run` | always | 30 days |
| `antb1-<target>` | `static <target>` | pushes to `main` and manual runs | 30 days |
| `antb1-<target>-sources` (Linux) | `static <target>` | pushes to `main` and manual runs, before the package | 30 days |
| `failed-antb1-<target>` (macOS) | `static <target>` | on failure, when the binary was built | 7 days |
| `antb1-<target>` | `build <target>` (`Release`) | tag runs and dry runs, after the sources on Linux | 30 days on tags, 7 on dry runs |
| `antb1-<target>-sources` (Linux) | `build <target>` (`Release`) | tag runs and dry runs, before the package | 30 days on tags, 7 on dry runs |
| `smoke-kit-<target>` | `build <target>` (`Release`) | tag runs and dry runs; never a release asset | 30 days on tags, 7 on dry runs |
| `release-meta` | `assemble` (`Release`) | tag runs and dry runs | 30 days on tags, 7 on dry runs |
| `release-provenance` | `attest` (`Release`) | tag runs | 30 days |

Artifacts never contain data: the coverage summary holds only percentages, fuzz inputs grow from our own seed
corpus, the benchmark results hold only timings and build facts (`version.txt`; never query text or results), the
sources of a package only source code (never an environment directory), a smoke kit only the Parquet files that
DuckDB writes from `range()` and `kit.json` with their digests and DuckDB's answers (never a release asset),
`release-meta` only `SHA256SUMS` and the release notes, `release-provenance` only the Sigstore bundle, and the
data jobs (`clickbench-hits0`, nightly `asan-data` and `arm64`) upload nothing. The `logs-<job>` files hold the
output of the tests over the data derived from TPC-H and TPC-DS, which is redacted like the data tests' (query
numbers, counts and hashes; [testing.md](testing.md#data-derived-from-tpc-h)); the generated data stays in the build
tree.

## Caches

- pixi environments: cached by prefix-dev/setup-pixi under a key that starts with `pixi-<hash of pixi.lock>-`. Only
  pushes to `main` write this cache; pull requests only read it, so a PR that changes `pixi.lock` installs its
  environments from scratch.
- ccache: restored in every build job from `ccache-<job>-` keys and saved only by scheduled and manual runs on
  `main`, which keeps the cache small and prevents pull requests from writing to it. `clang-tidy` does not use
  ccache.
- ClickBench data: `.cache/clickbench` under the key `clickbench-<hash of tools/data/clickbench.lock>`, saved by
  `clickbench-hits0` on `main` after a miss and read by every data job.
- The static Arrow prefix: `.cache/arrow/<key>` under the key `static-arrow-<target>-<key>`, where the key is
  `pixi run static-arrow --print-key`. The `static` workflow saves it from pushes to `main`. The nightly
  `static-test` and the benchmarks' `bench-run` only read it (on a miss they build the prefix cold and save
  nothing); `static-cold` never reads it.
- `release.yml` reads and writes no cache: setup-pixi runs with `cache: false`, `CCACHE_DISABLE=1` turns ccache
  off, and the build checks that `ANTB1_ARROW_DIR` does not exist yet, so every pinned source is downloaded and
  the Arrow prefix built again. What a release ships depends only on the tagged commit and its pins.
- `cache-gc` removes stale entries after each run on `main` (see above).
- A run on a pull request is cancelled when a newer push to the same PR arrives; runs on `main` and release runs
  are never cancelled.

## Reproducing a failure

1. Find the command in the job name, for example `gcc-compat (pixi run ci-gcc)`.
2. Run it locally: `pixi run ci-gcc`. Use the same platform for the macOS leg.
3. To iterate on one test, use the dev build: `pixi run test -R '^<name>$' -V`.
4. For a sanitizer finding, run the job's command (`pixi run asan` for `clang-asan`; nightly, `pixi run asan-data`,
   `pixi run asan-tpc-sf1` or `pixi run tsan`), fix the code and add a regression test; do not add a suppression
   without a maintainer's agreement.
5. For a lint failure, run `pixi run fmt`, then `pixi run lint`; the drift checks print the file, line and fix.
6. For a coverage failure, run `pixi run coverage` and read `build/coverage/summary.md` (module and metric) and
   `build/coverage/report.txt` (files); add tests rather than lowering a floor.
7. For a fuzz failure, download the `fuzz-artifacts` artifact (or run `pixi run fuzz-smoke`) and follow
   [fuzz/regressions/README.md](../fuzz/regressions/README.md): reproduce, minimize, commit the input with the fix.
8. For a failing random differential case, run the repro line that the failure prints:
   `ANTB1_DIFF_SEED=<seed> ANTB1_DIFF_ONLY=<case> pixi run diff-random`.
9. For a data test failure (`clickbench-hits0`), run `pixi run test-data`, then the unredacted command that the
   failure prints; it shows the SQL and the values on your machine only. Never paste them into the PR or an issue.
   An unexpected ClickBench pass or fail means the ratchet and the docs/sql-subset.md table need updating
   ([testing.md](testing.md#the-clickbench-ratchet)).
10. A failed `Release` run is the maintainers': its jobs reproduce with the commands in the table above, and only
    a maintainer re-runs one, as [When a release fails](#when-a-release-fails) describes.

## Workflow security rules

Every workflow follows these rules, and `pixi run lint` (actionlint, zizmor and the repository policy checks)
enforces them:

- top-level `permissions: {}` and the minimal permissions per job; `timeout-minutes` on every job;
- every action pinned to a full commit SHA with a `# vX.Y.Z` comment, and allowed by
  `tools/github/allowed-actions.json` (GitHub-owned actions plus the listed third-party patterns), from which the
  repository's Actions allowlist is built;
- the checkout action with `persist-credentials: false`;
- no `pull_request_target` or `workflow_run` triggers, and no `${{ github.event.* }}` expressions inside `run:`
  (values are passed through `env:`);
- caches are written only from `main`, and `release.yml` reads none;
- a job with a write permission never runs code from a pull request: `coverage-comment` (`pull-requests: write`),
  `cache-gc` (`actions: write`, `main` only), the nightly `report` (`issues: write`) and `bench-publish`
  (`contents: write` for the `gh-pages` series, pushes to `main` only) check out nothing from a PR. The release
  jobs that write run only for a `v*` tag pushed to this repository, after its preflight passed, on the tag's
  commit of `main`, and take the digests they sign or compare from assemble's job output: `attest` (`id-token: write`
  for the Sigstore signing certificate, `attestations: write` to store the attestation) checks out nothing and runs
  no code of the repository; `draft` (`contents: write` to delete earlier drafts of the tag, create the draft and
  upload its assets) and `publish` (`contents: write` to publish the draft; it waits for the `release`
  environment) check out only `.github/scripts/release.py`. `pixi run lint` runs
  `tools/ci/tests/test_release_workflow.py`, which pins these permissions, the guards and the absence of caches.
  The analyses that upload results (`CodeQL`, `Security`, `Scorecard`) hold only `security-events: write` (Scorecard
  also `id-token: write`, on `main`); on fork pull requests GitHub makes their token read-only.

The advisory AI and bot workflows add their own limits: their jobs get a read-only workflow token and write only
through their own GitHub App tokens (Claude, antb1-bot) or, for the Codex audit comment, through a separate job
that has nothing but `pull-requests: write`. The AI jobs skip pull requests from forks. See
[docs/agents.md](agents.md).

The repository settings (`tools/github/apply-settings.sh`) add the rest: workflow runs from external contributors'
forks need a maintainer's approval, and GitHub Actions cannot approve pull requests. Its `--release` mode adds the
tag rulesets, the `release` environment and immutable releases ([Release settings](#release-settings)).
