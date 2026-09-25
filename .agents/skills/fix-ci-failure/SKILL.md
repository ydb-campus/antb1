---
name: fix-ci-failure
description: Diagnose and fix a failing antb1 CI check on a pull request - find the failing job, reproduce it locally with the pixi task named in the job title, fix the cause and add a regression test. Use when `CI OK`, a CI job or the `PR title` check is red, or when asked why CI fails.
---

# Fix a CI failure

Step-by-step details per job: [docs/recipes/fix-ci-failure.md](../../../docs/recipes/fix-ci-failure.md). Job map:
[docs/ci.md](../../../docs/ci.md#jobs-and-local-commands).

## Rules

- Reproduce before you fix, and fix the cause. Never rerun a job blindly, and never weaken a gate: no disabled or
  skipped tests, silenced warnings, sanitizer suppressions, clang-tidy exclusions or bare `NOLINT`.
- `CI OK` only aggregates the other jobs: find the job that failed.
- Every job name contains the command that reproduces it, for example `clang-asan (pixi run asan)`.
- Never paste ClickBench data or unredacted data-test output into a PR, an issue or a prompt.
- If the fix needs an "Ask a human first" path (AGENTS.md), for example a workflow in `.github/`, `pixi.toml`,
  `CMakePresets.json`, `cmake/`, `.clang-tidy` or `tools/lint/`: stop and hand off with the exact change (file,
  diff, reason) for a maintainer.

## Steps

1. Find the failure: `gh pr checks <pr>`, then `gh run view <run-id> --log-failed` for the failing job. Test
   failures also appear as annotations and in the job summary.
2. Reproduce locally with the command from the job name:

   | Job | Command |
   | --- | --- |
   | `lint (pixi run lint)` | `pixi run lint` (fix formatting with `pixi run fmt`) |
   | `clang-asan (pixi run asan)` | `pixi run asan` |
   | `gcc-compat (pixi run ci-gcc)` | `pixi run ci-gcc` (installs the `gcc` environment, linux-64) |
   | `macos-release (pixi run release)` | `pixi run release` (needs a Mac; otherwise reason from the log) |
   | `clang-tidy (pixi run tidy)` | `pixi run tidy` |
   | `PR title` | fix the title: Conventional Commit, lowercase subject |

3. Iterate on one test with the dev build: `pixi run test -R '^<name>$' -V`; for a flaky test use
   `pixi run test -R '^<name>$' --repeat until-fail:20 --schedule-random`.
4. Fix the code; for a bug, add a test that fails without the fix.
5. Rerun the failing command, then `pixi run check` (all Linux gates: `pixi run check-full`), and push.
6. Only an infrastructure failure (network, runner, cache service) justifies a rerun; say so in the PR.
