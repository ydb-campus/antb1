# Continuous integration

CI runs on GitHub Actions. Every gate runs through the same `pixi run <task>` commands you use locally, so any CI
failure can be reproduced on a Linux machine (or a Mac, for the macOS leg) without GitHub. The governance decisions
behind this setup are in [ADR 0009](adr/0009-ci-and-governance.md).

## Workflows

| Workflow | File | Triggers | Purpose |
| --- | --- | --- | --- |
| `CI` | `.github/workflows/ci.yml` | pull requests to `main`, pushes to `main`, merge queue, daily schedule, manual | build, test, lint; required check `CI OK` |
| `PR title` | `.github/workflows/pr-title.yml` | pull requests (opened, edited, reopened, synchronized), merge queue | Conventional Commits title; required check `PR title` |

Later PRs add CodeQL (a merge gate through the ruleset), a nightly workflow (long fuzzing, TSan, ARM64, extended
differential tests), benchmarks, security scanning (zizmor, OpenSSF Scorecard), an agent bootstrap check and the AI
review workflows. The ClickBench data job and the coverage and fuzz job join `CI` later as well.

## Required checks

A pull request can merge only when:

- `CI OK` is green. It is an aggregator job (re-actors/alls-green) that runs even when other jobs fail or are skipped
  and succeeds only if every job it depends on succeeded. New CI jobs are added to its `needs` list, so the required
  check name never changes.
- `PR title` is green: the title is a Conventional Commit with a lowercase subject, and one of the types `feat`,
  `fix`, `perf`, `refactor`, `test`, `docs`, `build`, `ci`, `chore`, `revert`.
- One human maintainer approved and every review thread is resolved. AI reviews never count.
- Once CodeQL lands: code scanning reports no new error-level alerts and no security alerts of high severity or
  above. A CodeQL failure, or a missing CodeQL analysis, blocks all merges until it is fixed or an admin bypasses
  the rule through the PR.

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
| `CI OK` | ubuntu-slim | none | aggregator, nothing to run |
| `PR title` | ubuntu-slim | none | check the title against the rules above |

- `lint` is read-only: format checks, linters, offline zizmor, the repository drift and policy checks and their own
  tests. On pull requests it also runs GitHub's dependency review. `pixi run fmt` fixes what can be fixed
  automatically. Locally it checks tracked and untracked (non-ignored) files, the same set CI sees after commit.
- `clang-asan` is a Clang Debug build with AddressSanitizer and UndefinedBehaviorSanitizer and `-Werror`, followed by
  the hermetic tests.
- `gcc-compat` builds with GCC 15 and GCC-only warnings, with `-Werror`, and runs the hermetic tests.
- `macos-release` is a RelWithDebInfo build with `-Werror` against libc++ on Apple silicon, followed by the hermetic
  tests.
- `clang-tidy` runs clang-tidy on every translation unit, with warnings as errors.
- `pixi run ci` (Clang Debug `-Werror` plus tests) is not a separate job; it is the fast local gate inside
  `pixi run check`.
- `pixi run check-full` runs every Linux gate above in one command: lint, `ci`, `asan`, `tidy` and `ci-gcc`.
- `pixi run codeql-build` reproduces the GCC build that the CodeQL workflow will trace.

CI always calls tasks with an explicit environment and a frozen lock, for example
`pixi run --frozen -e default asan` or `pixi run --frozen -e gcc ci-gcc`, after installing pixi 0.81.0 with
prefix-dev/setup-pixi.

## Test reports and logs

Test jobs write JUnit XML to `build/<preset>/junit.xml`. The results appear as annotations on the PR and in the job
summary. When a job fails, the ctest logs and the JUnit file are uploaded as the `logs-<job>` artifact and kept for 7
days. The build jobs and `clang-tidy` also write their disk usage to the job summary. Timing and cache measurements
are recorded here once enough runs exist.

## Caches

- pixi environments: cached by prefix-dev/setup-pixi under a key that starts with `pixi-<hash of pixi.lock>-`. Only
  pushes to `main` write this cache; pull requests only read it, so a PR that changes `pixi.lock` installs its
  environments from scratch.
- ccache: restored in every build job from `ccache-<job>-` keys and saved only by scheduled and manual runs on
  `main`, which keeps the cache small and prevents pull requests from writing to it. `clang-tidy` does not use
  ccache.
- A run on a pull request is cancelled when a newer push to the same PR arrives; runs on `main` are never
  cancelled.

## Reproducing a failure

1. Find the command in the job name, for example `gcc-compat (pixi run ci-gcc)`.
2. Run it locally: `pixi run ci-gcc`. Use the same platform for the macOS leg.
3. To iterate on one test, use the dev build: `pixi run test -R '^<name>$' -V`.
4. For a sanitizer finding, run `pixi run asan`, fix the code and add a regression test; do not add a suppression
   without a maintainer's agreement.
5. For a lint failure, run `pixi run fmt`, then `pixi run lint`; the drift checks print the file, line and fix.

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
- caches are written only from `main`.

The repository settings (`tools/github/apply-settings.sh`) add the rest: workflow runs from external contributors'
forks need a maintainer's approval, and GitHub Actions cannot approve pull requests.
