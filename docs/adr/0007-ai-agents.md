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
  [.github/copilot-instructions.md](../../.github/copilot-instructions.md) and [CLAUDE.md](../../CLAUDE.md); skills
  shared by every agent live in `.agents/skills/`.
- Agents use exactly the same `pixi run <task>` commands as humans and CI; there is no agent-only path.
  `scripts/agent-setup.sh` bootstraps pixi and the locked environments in any Linux sandbox.
- The toolchain, CI and governance paths listed under "Ask a human first" are changed only with a maintainer's
  explicit approval; CODEOWNERS covers the same set.
- AI reviews are advisory. They comment, never approve, and never count toward the required human approval. The
  repository settings do not let GitHub Actions approve pull requests. No AI workflow is a required check.
- AI integrations on GitHub, set up as described in [docs/agents.md](../agents.md):
  - Claude: `@claude` on issues and PRs (`.github/workflows/claude.yml`) and one automatic review per PR from a
    prompt that applies the Code Review Rules (`.github/workflows/claude-review.yml`), both through the Claude
    GitHub App and a Claude subscription token (`CLAUDE_CODE_OAUTH_TOKEN`);
  - Codex: native review on request (`@codex review`, automatic reviews off) and a harness audit per PR
    (`.github/workflows/codex-review.yml`) that runs a pinned Codex CLI read-only with a spend-capped project key
    (`OPENAI_API_KEY`) and posts one sticky comment;
  - Copilot: `.github/copilot-instructions.md` and `.github/workflows/copilot-setup-steps.yml`, which prepares
    the same pixi environments for the coding agent;
  - cloud sandboxes (Claude Code on the web, Codex cloud, Copilot) install the same locked environments
    (`default` and `lint`), after which every task works without network access.
- Defense in depth against prompt injection and secret misuse: `@claude` requires write access for both the sender
  and the issue or PR author and refuses fork PRs; the automatic reviews run only on same-repository, non-draft PRs;
  in GitHub Actions, Claude runs with hooks disabled and only named `pixi run` tasks plus read-only `git` and `gh`;
  the Codex audit takes its prompt and AGENTS.md from the base commit, does not load the PR's AGENTS.md as
  instructions and has no write token (a separate job posts its comment); every AI job's workflow token is read-only.
- Every PR discloses AI assistance in its template and names the accountable human.
- `pixi run lint` keeps the agent-facing files honest: commands in the docs must exist, links must resolve, and
  AGENTS.md must stay within its size limits and contain the Code Review Rules.

## Consequences

- One file to keep up to date; tool-specific files stay small.
- Agents that follow AGENTS.md produce changes that pass the same gates as human changes.
- The "Ask a human first" list slows down changes to CI and the toolchain, which is intended.
- Every PR gets up to two automatic AI reviews (Claude review, Codex audit) plus on-request ones; each costs money
  or subscription quota, so none of them re-runs on every push.
- Anyone with write access can still read the repository secrets from a same-repository PR run by changing a
  workflow. CODEOWNERS review on `.github/`, the spend cap and quick rotation (docs/agents.md) limit the damage.
- The Claude subscription token expires after about a year and must be renewed by hand; an `ANTHROPIC_API_KEY`
  secret is the fallback if the subscription's limits do not fit the team.
