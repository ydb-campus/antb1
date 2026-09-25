# Architecture decision records

This directory records the architecturally significant decisions of antb1 in Michael Nygard's format
([ADR 0001](0001-record-architecture-decisions.md)). Add a new record as `<NNNN>-<short-title>.md` with the next number,
in the same PR as the change it justifies, and list it below; `pixi run lint` checks that this index is complete.

| ADR | Title | Status |
| --- | --- | --- |
| [0001](0001-record-architecture-decisions.md) | Record architecture decisions | Accepted |
| [0002](0002-toolchain-pixi-conda-forge.md) | Toolchain: pixi and conda-forge, Clang 23 primary, GCC 15 compatibility leg | Accepted |
| [0003](0003-engine-architecture.md) | Engine architecture | Proposed |
| [0004](0004-types-null-overflow-semantics.md) | Types, NULL and overflow semantics | Accepted |
| [0005](0005-error-boundary.md) | Error boundary and exit codes | Accepted |
| [0006](0006-test-strategy-and-data-policy.md) | Test strategy and data policy | Accepted |
| [0007](0007-ai-agents.md) | AI coding agents | Accepted |
| [0008](0008-parser-and-unparser.md) | Hand-written parser with an unparser and a round-trip property | Accepted |
| [0009](0009-ci-and-governance.md) | CI and repository governance | Accepted |
