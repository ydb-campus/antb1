# Recipe: fix a CI failure

Every CI gate runs a `pixi run <task>` that you can run locally, and every job name contains that command. The skill
`.agents/skills/fix-ci-failure/SKILL.md` is the short version; [ci.md](../ci.md) describes the workflows.

## Rules

- Reproduce first, then fix the cause. A rerun is justified only for an infrastructure failure (network, runner,
  cache service), and the PR should say so.
- Never weaken a gate: no disabled or skipped tests, silenced warnings, sanitizer suppressions, clang-tidy
  exclusions or bare `NOLINT` (AGENTS.md, golden rule 6).
- Never paste ClickBench data or unredacted data-test output into a PR, an issue, a review or a prompt.
- Hand-off rule: if the fix needs an "Ask a human first" path, for example a workflow under `.github/`, `pixi.toml`,
  `CMakePresets.json`, `cmake/`, `.clang-tidy`, `.clang-format` or `tools/lint/`, stop and describe the exact change
  (file, diff, reason) for a maintainer.

## 1. Find the failing job

```bash
gh pr checks <pr-number>                    # which checks failed
gh run view <run-id> --log-failed           # the failing steps' logs
gh run view <run-id> --json jobs            # job names and conclusions
```

- `CI OK` is only an aggregator (re-actors/alls-green): it fails when any job it needs failed or was cancelled.
  Look at the other jobs.
- Test failures also appear as annotations on the PR and in the job summary (from the JUnit file). On failure the
  ctest logs and JUnit XML are uploaded as the `logs-<job>` artifact for 7 days
  (`gh run download <run-id> -n logs-<job>`).

## 2. Reproduce locally

| Job | Command | Notes |
| --- | --- | --- |
| `lint (pixi run lint)` | `pixi run lint` | formatting: `pixi run fmt`; drift checks print `path:line: Rxxx message (fix: ...)` |
| `clang-asan (pixi run asan)` | `pixi run asan` | ASan and UBSan; the first report is the one to fix |
| `gcc-compat (pixi run ci-gcc)` | `pixi run ci-gcc` | GCC 15 with GCC-only warnings; installs the `gcc` environment (linux-64) |
| `macos-release (pixi run release)` | `pixi run release` | Apple silicon, libc++; without a Mac, reason from the log and the libc++ differences |
| `clang-tidy (pixi run tidy)` | `pixi run tidy` | every translation unit, warnings as errors |
| `PR title` | none | edit the title: Conventional Commit, lowercase subject, an allowed type |

`pixi run check-full` runs every Linux gate in one command. On a pull request `lint` also runs GitHub's dependency
review, which fails on a vulnerable or disallowed new dependency: that needs a different package version, which is
a [dependency-update](dependency-update.md).

## 3. Narrow it down

```bash
pixi run test -R '^<module>\.<Suite>\.<Case>$' -V            # one test, verbose, dev build
pixi run test --rerun-failed --output-on-failure              # the tests that failed last time
pixi run test -R '^<module>\.' --repeat until-fail:20 --schedule-random   # hunt a flaky test
```

- Compiler errors and warnings (`-Werror`): fix the code. Narrowing conversions go through `antb1::TryNarrow` or
  `antb1::Narrow`, never a cast that only silences `-Wconversion`.
- Sanitizer reports: find the first report, write a test that triggers it, fix the ownership or bounds bug.
- clang-tidy: fix the finding; a `NOLINT` needs the check name and a reason, and a maintainer's agreement.
- A test that passes locally but fails in CI: compare the environment (the preset pins `LC_ALL=C`, `TZ=UTC` and
  thread counts), look for order dependence (`--schedule-random`), shared temporary paths, wall-clock time or
  uninitialized memory.
- Lint drift findings (Rxxx) name the file, the line and the fix. A finding in an "Ask a human first" file (for
  example the module table against `cmake/Antb1Modules.cmake`) usually means the docs must follow the code.

## 4. Fix, verify, push

1. Fix the cause and add a regression test that fails without the fix.
2. Rerun the command of the failing job, then `pixi run check` (or `pixi run check-full` for sanitizer, GCC or
   clang-tidy failures).
3. Push to the same branch. The branch does not need to be up to date with `main` unless there are conflicts.
4. In the PR, say what failed, why, and which commands you ran to verify the fix.
