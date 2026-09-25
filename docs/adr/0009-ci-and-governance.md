# 9. CI and repository governance

Date: 2026-09-25

## Status

Accepted

## Context

- The repository is public, belongs to a GitHub Free organization and has a handful of maintainers and many AI
  agents contributing. Every PR must pass the same gates, and the gates themselves must be hard to weaken by
  accident.
- Branch-protection rules that list every CI job break whenever jobs are renamed or added. Requiring branches to be
  up to date, combined with dismissing stale approvals, would dismiss every approval each time `main` moves.
- Third-party GitHub Actions are a supply-chain risk; tags can be moved, and workflows triggered by forks can leak
  secrets.

## Decision

- Required checks:
  - `CI OK`: an aggregator job in `.github/workflows/ci.yml` (re-actors/alls-green) that depends on every CI job and
    runs even when they fail, so the required check name stays stable while jobs change;
  - `PR title`: Conventional Commits with a lowercase subject (`.github/workflows/pr-title.yml`), because the PR
    title becomes the squash commit on `main`;
  - CodeQL through the ruleset's code-scanning rule (errors and high-severity security alerts), once the CodeQL
    workflow lands. It is built with GCC 15 because CodeQL cannot trace Clang 23.
- Ruleset on `main`, managed as code in `tools/github/` and applied in two stages (first the pull-request rules,
  then the required checks once they have reported): no deletion, no force pushes, linear history, squash merges
  only, one approving review, stale approvals dismissed on push, all review threads resolved. Strict status checks
  are off; a merge queue (`merge_group` is already a trigger) can restore tested-with-main merges later. Admins may
  bypass only through a pull request.
- CODEOWNERS covers the governance paths (the same set as "Ask a human first" in AGENTS.md, plus `pixi.lock` and
  `docs/adr/`). Review is requested, not required.
- Workflow hardening, enforced by `pixi run lint` (actionlint, zizmor and the repository policy checks):
  top-level `permissions: {}` and minimal per-job permissions; `timeout-minutes` on every job;
  `persist-credentials: false`; no `pull_request_target` or `workflow_run`; no `${{ github.event.* }}` inside
  `run:`; caches written only from `main`; every action pinned to a full commit SHA with a version comment.
- Actions allowlist: the repository allows only GitHub-owned actions and the third-party actions listed in
  `tools/github/allowed-actions.json`, with SHA pinning required. Fork pull requests from external contributors
  need approval before workflows run, and GitHub Actions cannot approve pull requests.
- Repository settings (squash commit title and body from the PR, deleted head branches, auto-merge, secret scanning
  with push protection, Dependabot alerts, private vulnerability reporting) are applied by
  `tools/github/apply-settings.sh`, which is a dry run unless asked to apply.
- Automated dependency PRs (the weekly `pixi.lock` refresh, `.github/workflows/pixi-lock-update.yml`) come from a
  dedicated organization GitHub App (antb1-bot), so they trigger CI like any other PR without a personal token.

## Consequences

- Adding or renaming CI jobs never requires a ruleset change.
- With strict mode off, two PRs can each pass CI and still conflict semantically after both merge. Push-to-main CI
  and the daily run catch this; a merge queue is the planned remedy when volume grows.
- A failing or missing CodeQL analysis blocks every merge; only an admin bypass through the PR gets around it.
- Updating an action means updating its SHA pin, its version comment and, for a new action, the allowlist.
