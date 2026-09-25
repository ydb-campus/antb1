# 7. AI coding agents

Date: 2026-09-25

## Status

Accepted

## Context

- Several AI coding agents (Claude Code, OpenAI Codex, GitHub Copilot) work on this repository next to humans, in
  local terminals, cloud sandboxes and GitHub Actions. Each tool reads its own instruction file.
- Separate instruction files drift apart, and agents that improvise commands (host compilers, package managers,
  `-e` flags) produce results nobody else can reproduce.
- Agents can be wrong with confidence and can be targeted by prompt injection through issues and pull requests.

## Decision

- [AGENTS.md](../../AGENTS.md) is the single canonical guide for every agent: golden rules, the command table,
  the repository map, C++ conventions, testing and PR rules, the "Ask a human first" paths and the Code Review
  Rules. It stays within 150 lines and under 32 KiB. Tool-specific files only point to it and add tool-specific notes:
  [.github/copilot-instructions.md](../../.github/copilot-instructions.md) now, a Claude Code file and shared
  skills in a later PR.
- Agents use exactly the same `pixi run <task>` commands as humans and CI; there is no agent-only path.
  `scripts/agent-setup.sh` bootstraps pixi and the locked environments in any Linux sandbox.
- The toolchain, CI and governance paths listed under "Ask a human first" are changed only with a maintainer's
  explicit approval; CODEOWNERS covers the same set.
- AI reviews are advisory. They comment, never approve, and never count toward the required human approval. The
  repository settings do not let GitHub Actions approve pull requests.
- Every PR discloses AI assistance in its template and names the accountable human.
- `pixi run lint` keeps the agent-facing files honest: commands in the docs must exist, links must resolve, and
  AGENTS.md must stay within its size limits and contain the Code Review Rules.

## Consequences

- One file to keep up to date; tool-specific files stay small.
- Agents that follow AGENTS.md produce changes that pass the same gates as human changes.
- The "Ask a human first" list slows down changes to CI and the toolchain, which is intended.
- AI workflows that run in GitHub Actions (review, `@claude`, Codex audits) are added in a later PR with their own
  permission and fork restrictions.
