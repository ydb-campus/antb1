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
| `Security` | `.github/workflows/security.yml` | pushes and pull requests that change `.github/**`, weekly, manual | zizmor with its online audits, results in the Security tab; advisory |
| `Scorecard` | `.github/workflows/scorecard.yml` | pushes to `main`, weekly, branch protection changes | OpenSSF Scorecard, results in the Security tab and on scorecard.dev; advisory |
| `Agent bootstrap` | `.github/workflows/agent-bootstrap.yml` | pull requests that change `scripts/agent-setup.sh`, `pixi.toml`, `pixi.lock` or the workflow; weekly; manual | a cold `scripts/agent-setup.sh` from both pixi sources, then the tests; advisory |
| Dependabot | `.github/dependabot.yml` | weekly (Monday) | one grouped PR that updates the pinned GitHub Actions, for releases at least 7 days old |
| `Claude` | `.github/workflows/claude.yml` | `@claude` in issues, PR comments and reviews; issue label `claude` | advisory: Claude Code works on the issue or PR (write-access gate, no forks) |
| `Claude review` | `.github/workflows/claude-review.yml` | pull requests opened, ready for review or reopened | advisory: one automatic Claude review per PR, inline comments |
| `Codex harness audit` | `.github/workflows/codex-review.yml` | pull requests opened, ready for review or reopened; label `codex-review` | advisory: read-only Codex audit, one sticky comment |
| `Copilot Setup Steps` | `.github/workflows/copilot-setup-steps.yml` | every Copilot coding-agent task; manual; changes to itself, `pixi.toml` or `pixi.lock` | prepares Copilot's environment (`default` and `lint`, dev build) |
| `pixi.lock update` | `.github/workflows/pixi-lock-update.yml` | weekly (Monday), manual | PR `build(deps): update pixi.lock` from the antb1-bot App |
| `Benchmarks` | `.github/workflows/bench.yml` | pushes to `main` that touch `src/`, `bench/`, `tools/fixturegen/` or `pixi.lock`; pull requests labeled `performance`; manual | micro benchmarks and ClickBench timings; publishes the series from `main` to `gh-pages`; advisory, never gating ([benchmarks.md](benchmarks.md)) |

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
| `bench-run (pixi run bench && pixi run bench-clickbench)` (advisory) | ubuntu-24.04 | `default` | `pixi run bench` · `pixi run bench-clickbench` |
| `bench-publish` (advisory, `main` only) | ubuntu-24.04 | none | nothing to reproduce: appends `micro.json` to the `gh-pages` series |
| `PR title` | ubuntu-slim | none | check the title against the rules above |

Advisory jobs (not required):

| Job | Workflow | Runner | What it runs |
| --- | --- | --- | --- |
| `claude (@claude)` | `Claude` | ubuntu-24.04 | Claude Code with named `pixi run` tasks and read-only `git`/`gh` only; 60 min |
| `claude-review (code-review plugin)` | `Claude review` | ubuntu-24.04 | Anthropic's code-review plugin; 30 min |
| `codex-audit (harness checklist)` | `Codex harness audit` | ubuntu-24.04 | Codex CLI 0.155.1, `:read-only` profile, no sudo; 30 min |
| `codex-audit comment` | `Codex harness audit` | ubuntu-slim | posts the audit as one sticky comment |
| `copilot-setup-steps` | `Copilot Setup Steps` | ubuntu-24.04 | `pixi install --locked -e lint`, then `pixi run --frozen -e default build` |
| `pixi update` | `pixi.lock update` | ubuntu-24.04 | `pixi update` with pixi 0.81.0 and a PR through the antb1-bot App token |

- `lint` is read-only: format checks, linters, offline zizmor, the repository drift and policy checks and their own
  tests. On pull requests it also runs GitHub's dependency review. `pixi run fmt` fixes what can be fixed
  automatically. Locally it checks tracked and untracked (non-ignored) files, the same set CI sees after commit.
- `clang-asan` is a Clang Debug build with AddressSanitizer and UndefinedBehaviorSanitizer and `-Werror`, followed by
  the hermetic tests.
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
  files and all but the newest ccache of each leg, then lists what is left in the job summary. A run whose commit is
  no longer the head of `main` deletes nothing, so it never removes the caches of a newer lock. Pull requests still
  on an older `pixi.lock` install their environments without a cache until they merge `main`.
- `pixi run ci` (Clang Debug `-Werror` plus tests) is not a separate job; it is the fast local gate inside
  `pixi run check`.
- `pixi run check-full` runs every Linux gate above except the data tests in one command: `check` (lint and `ci`),
  `asan`, `tidy`, `coverage`, `fuzz-smoke` and `ci-gcc`. Run `pixi run test-data` for `clickbench-hits0`.

CI always calls tasks with an explicit environment and a frozen lock, for example
`pixi run --frozen -e default asan` or `pixi run --frozen -e gcc ci-gcc`, after installing pixi 0.81.0 with
prefix-dev/setup-pixi.

## Nightly

`nightly.yml` runs the deep checks on `main` every night (and on demand). It is advisory: nothing blocks a merge,
but every failure is tracked in an issue.

| Job | Runner | Reproduce locally | What it adds |
| --- | --- | --- | --- |
| `fuzz-long` | ubuntu-24.04 | `ANTB1_FUZZ_SECONDS=1200 pixi run fuzz` | 20 minutes of libFuzzer with a random seed; crash inputs are uploaded as `fuzz-long-artifacts` for 14 days |
| `tsan` | ubuntu-24.04 | `pixi run tsan` | the hermetic tests under ThreadSanitizer |
| `asan-data` | ubuntu-24.04 | `pixi run asan-data` | the data tests under ASan and UBSan (redacted, no uploads) |
| `arm64` | ubuntu-24.04-arm | `pixi run release` then `pixi run test-data` | Linux ARM64: the release build, the hermetic tests and the data tests |
| `diff-extended` | ubuntu-24.04 | `ANTB1_DIFF_SEED=<run id> ANTB1_DIFF_COUNT=20000 pixi run diff-random` | 20,000 random differential queries with the run id as the seed |
| `ci-shuffle` | ubuntu-24.04 | `pixi run ci-shuffle` | the hermetic tests in random order, each repeated until it fails (at most twice) |
| `report` | ubuntu-slim | nothing to run | on any failed or timed-out job: opens an issue labelled `nightly-failure` and `agent-task`, or comments on the open one, with the failing jobs, the run URL and the commands above |

The nightly jobs only read caches: pixi environments and ccache come from the runs on `main` (`asan-data` reuses
the ccache of `clang-asan`, `fuzz-long` that of `clang-coverage-fuzz`), and the data comes from the cache that
`clickbench-hits0` saved. The ARM64 environments are not cached, to stay within the cache budget. To work on a
nightly failure, take the issue, run its command locally, fix the cause with a regression test and close the issue
from the PR.

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
  and only checkout, upload-artifact, codeql-action/upload-sarif and scorecard-action as steps. The findings
  accepted for now are the missing license (decided later) and the admin bypass of the ruleset.
- `Agent bootstrap` checks the shared sandbox bootstrap from scratch, as a Claude, Codex or Copilot sandbox runs it:
  `bash scripts/agent-setup.sh --envs "default lint"` with `ANTB1_PIXI_SOURCE=github` and `ANTB1_PIXI_SOURCE=conda`,
  then `pixi run --as-is test` and `pixi run --as-is doctor --json`. It uses no caches on purpose.
- Dependabot opens one grouped PR per week (`ci(deps): ...`, labels `dependencies` and `github-actions`) that moves
  the SHA pins of the actions to releases at least 7 days old. A new action also needs an entry in
  `tools/github/allowed-actions.json`. `pixi.lock` is not updated by Dependabot.

## Test reports and logs

Test jobs write JUnit XML to `build/<preset>/junit.xml` (`clang-coverage-fuzz`: `build/coverage/junit.xml` and
`build/fuzz/junit.xml`; the data tests: `build/ci-release/junit-data.xml` and, nightly, `build/ci-asan/junit-data.xml`;
`ci-shuffle`: `build/ci/junit-shuffle.xml`). The results appear as annotations on the PR and in the job summary. When
a hermetic test job fails, the ctest logs and the JUnit files are uploaded as the `logs-<job>` artifact and kept for
7 days. The build jobs and `clang-tidy` also write their disk usage to the job summary. Timing and cache
measurements are recorded here once enough runs exist.

| Artifact | Job | When | Kept |
| --- | --- | --- | --- |
| `logs-<job>` | every hermetic test job (CI legs, nightly `tsan` and `ci-shuffle`) | on failure | 7 days |
| `coverage-summary` | `clang-coverage-fuzz` | always (when the report exists) | 7 days |
| `fuzz-artifacts` | `clang-coverage-fuzz` | when the fuzz smoke run fails | 14 days |
| `fuzz-long-artifacts` | nightly `fuzz-long` | when long fuzzing fails | 14 days |
| `scorecard-sarif` | `Scorecard` | always | 5 days |
| `bench-results` | `bench-run` | always | 30 days |

Artifacts never contain data: the coverage summary holds only percentages, fuzz inputs grow from our own seed
corpus, the benchmark results hold only timings (never query text or results), and the data jobs
(`clickbench-hits0`, nightly `asan-data` and `arm64`) upload nothing.

## Caches

- pixi environments: cached by prefix-dev/setup-pixi under a key that starts with `pixi-<hash of pixi.lock>-`. Only
  pushes to `main` write this cache; pull requests only read it, so a PR that changes `pixi.lock` installs its
  environments from scratch.
- ccache: restored in every build job from `ccache-<job>-` keys and saved only by scheduled and manual runs on
  `main`, which keeps the cache small and prevents pull requests from writing to it. `clang-tidy` does not use
  ccache.
- ClickBench data: `.cache/clickbench` under the key `clickbench-<hash of tools/data/clickbench.lock>`, saved by
  `clickbench-hits0` on `main` after a miss and read by every data job.
- `cache-gc` removes stale entries after each run on `main` (see above).
- A run on a pull request is cancelled when a newer push to the same PR arrives; runs on `main` are never
  cancelled.

## Reproducing a failure

1. Find the command in the job name, for example `gcc-compat (pixi run ci-gcc)`.
2. Run it locally: `pixi run ci-gcc`. Use the same platform for the macOS leg.
3. To iterate on one test, use the dev build: `pixi run test -R '^<name>$' -V`.
4. For a sanitizer finding, run `pixi run asan`, fix the code and add a regression test; do not add a suppression
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
- caches are written only from `main`;
- a job with a write permission never runs code from a pull request: `coverage-comment` (`pull-requests: write`),
  `cache-gc` (`actions: write`, `main` only), the nightly `report` (`issues: write`) and `bench-publish`
  (`contents: write` for the `gh-pages` series, pushes to `main` only) check out nothing from a PR.
  The analyses that upload results (`CodeQL`, `Security`, `Scorecard`) hold only `security-events: write` (Scorecard
  also `id-token: write`, on `main`); on fork pull requests GitHub makes their token read-only.

The advisory AI and bot workflows add their own limits: their jobs get a read-only workflow token and write only
through their own GitHub App tokens (Claude, antb1-bot) or, for the Codex audit comment, through a separate job
that has nothing but `pull-requests: write`. The AI jobs skip pull requests from forks. See
[docs/agents.md](agents.md).

The repository settings (`tools/github/apply-settings.sh`) add the rest: workflow runs from external contributors'
forks need a maintainer's approval, and GitHub Actions cannot approve pull requests.
