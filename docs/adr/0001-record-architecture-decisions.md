# 1. Record architecture decisions

Date: 2026-09-25

## Status

Accepted

## Context

antb1 is built by a small group of people together with AI coding agents (Claude Code, Codex, Copilot). Agents
start every session without memory of earlier discussions, and humans rotate in and out. Decisions such as "SUM
returns a 128-bit integer" or "exec never depends on io" must be written down where both can find them, with the
reasons, so that nobody reverses them by accident.

## Decision

We record architecturally significant decisions as Architecture Decision Records in `docs/adr/`, using Michael
Nygard's format: a numbered file `docs/adr/<NNNN>-<short-title>.md` with the sections Status, Context, Decision
and Consequences.

- A decision is architecturally significant when it changes a module boundary (`cmake/Antb1Modules.cmake`), a
  dependency or the toolchain, SQL semantics, the error model, the test or data policy, or CI and governance.
- [docs/adr/README.md](README.md) lists every ADR; `pixi run lint` checks that the index is complete.
- Statuses are Proposed, Accepted, Deprecated and Superseded by ADR N. An accepted ADR is not rewritten: a new ADR
  supersedes it, and the old one only gets its status updated.
- An ADR is written in the same PR as the change it justifies. Changes under `docs/adr/` request review from the
  maintainers (CODEOWNERS), and status changes need a maintainer's approval.

## Consequences

- Reviewers and agents can check a change against a written decision instead of relying on memory.
- Changing a module edge, a semantic rule or a gate takes a little more work, which is intended.
- The index and the ADRs must be kept in sync; the lint check catches a missing entry.
