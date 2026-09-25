# Contributing to antb1

Thanks for helping. This guide is for humans; [AGENTS.md](AGENTS.md) holds the same rules in compact form for AI
coding agents, and both describe one workflow.

## Setup

The whole toolchain (Clang 23, GCC 15, CMake, Ninja, ccache, Arrow 25, GoogleTest, DuckDB for tests, and every
formatter and linter) comes from [pixi](https://pixi.prefix.dev) and conda-forge. You do not need system packages, root
access or a container, and you should not use host compilers or host CMake.

1. Install pixi 0.81.0 or newer.
   - Linux: `bash scripts/agent-setup.sh` downloads the pinned, sha256-verified pixi 0.81.0 into
     `~/.cache/antb1/pixi-0.81.0` when no suitable pixi is on `PATH`, then installs the locked `default` and `lint`
     environments. Add that directory to `PATH` if the script installed pixi.
   - macOS (Apple silicon): install pixi from <https://pixi.prefix.dev>.
2. Check the environment with `pixi run doctor`.
3. Build and test with `pixi run test`.

The environments are `default` (Clang 23; every build and test task), `gcc` (GCC 15, linux-64 only; the
compatibility leg and the CodeQL build) and `lint` (formatters and linters). Every task exists in exactly one
environment, so `pixi run <task>` never needs `-e`; pixi installs a missing environment on first use.

`pixi.toml` and `pixi.lock` define the toolchain. Never edit `pixi.lock` by hand; a maintainer re-locks with pixi
0.81.0 when dependencies change.

## Everyday commands

| Goal | Command |
| --- | --- |
| Incremental Debug build (`build/<preset>`, preset `dev`) | `pixi run build` |
| Build and run the hermetic tests | `pixi run test` |
| Run selected tests (allowlisted ctest arguments) | `pixi run test -R '^sql\.'` · `pixi run test --rerun-failed` |
| Run the CLI from the dev build | `pixi run antb1 query -f query.sql --table t=/data/t.parquet` |
| Format C++, CMake, Python, TOML and Markdown in place | `pixi run fmt` |
| Lint and repository drift checks (read-only) | `pixi run lint` |
| Required before every PR | `pixi run check` |
| Every Linux PR gate (ASan/UBSan, clang-tidy, coverage floors, fuzz smoke, GCC leg) | `pixi run check-full` |
| Toolchain, build and data status | `pixi run doctor` |

[docs/ci.md](docs/ci.md) maps every CI job to the local command that reproduces it, and
[docs/testing.md](docs/testing.md) explains how tests are organized.

For editors: every build tree has a compilation database, `build/<preset>/compile_commands.json` (the `dev`
preset after `pixi run build`), which clangd and other tools can use.

## Making a change

1. Branch from `main` as `<type>/<short-slug>`, where `<type>` is one of the commit types below.
2. Keep one logical change per pull request. Every behavior change ships with tests in the same PR.
3. Update docs in the same PR: [docs/sql-subset.md](docs/sql-subset.md) for SQL behavior, the command table in
   [AGENTS.md](AGENTS.md) for tasks, and a new ADR in [docs/adr/](docs/adr/README.md) for architectural decisions
   (including a new module edge).
4. Run `pixi run check`. Also run `pixi run check-full` when you touch memory or ownership code in `io` or `exec`,
   CMake files or presets.
5. Open a pull request and fill in the template: what changed, the exact verification commands with their results,
   and whether AI tools helped.

### Commit and PR titles

PRs are squash-merged, so the PR title becomes the commit message on `main`. The title must follow
[Conventional Commits](https://www.conventionalcommits.org/) with a lowercase subject; the `PR title` check
enforces it. Allowed types: `feat`, `fix`, `perf`, `refactor`, `test`, `docs`, `build`, `ci`, `chore`, `revert`.
A scope is optional and is usually a module or area:

```text
feat(sql): add IN lists
fix(io): report truncated Parquet footers as I/O errors
docs: explain the ClickBench ratchet
```

Commits inside a branch can be messy; only the PR title and description end up on `main`.

## Review and merging

- A PR needs green `CI OK` and `PR title` checks, one approving review from a human maintainer, and every review
  thread resolved. A new push dismisses earlier approvals.
- The branch does not have to be up to date with `main`; update it only when there are conflicts.
- Paths that define the toolchain, CI and governance (listed under "Ask a human first" in [AGENTS.md](AGENTS.md) and
  in `.github/CODEOWNERS`) automatically request review from the maintainers. Discuss such changes first.
- Never weaken a gate to get a PR green: no disabled tests, silenced warnings or sanitizer and clang-tidy exclusions
  without a documented reason and a maintainer's agreement.

## AI assistance

AI coding agents are welcome and use the same `pixi run` tasks and rules as humans ([AGENTS.md](AGENTS.md) is their
guide).

- Disclose AI assistance in the PR template. The human who opens or merges the PR is accountable for it.
- AI reviews are advisory. They never approve a PR and never replace the required human approval.
- Never paste secrets, private data or anything derived from ClickBench into prompts, issues or PRs.

## Data policy

Nothing derived from ClickBench (Parquet files, samples, query text, result values) is ever committed, and no file
larger than 1 MiB may be committed. Tests generate their own data. See
[docs/adr/0006-test-strategy-and-data-policy.md](docs/adr/0006-test-strategy-and-data-policy.md).

## Optional git hooks

`pixi run install-git-hooks` points git at `.githooks/`. The pre-commit hook runs the same read-only lint checks
as `pixi run lint`, on staged files only, with tools from the pixi `lint` environment. Fix findings with
`pixi run fmt`. The hook is optional; CI runs the same checks.

## Reporting issues

Use the issue forms: bug reports should include the exact command and the output of `pixi run doctor --json`.
Report security vulnerabilities privately as described in [SECURITY.md](SECURITY.md), never in a public issue.

## License

The project has no license yet (see [README.md](README.md)).
