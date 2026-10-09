# 15. Query profiles and `explain --analyze`

Date: 2026-09-30

## Status

Proposed

## Context

- **Performance work needs per-operator numbers.** The comparison with DuckDB and fixes 1-5 (ADR 0013, 0014) were
  measured with timers patched into operators by hand, rebuilt and thrown away.
  - That is slow, and it misled us once: the ORDER BY LIMIT above Q32's GROUP BY was taken for most of its remaining
    time; measured, it was a fifth of it.
- **`antb1 explain` shows only the optimized logical plan.** The physical planner's choices do not show at all: part
  pipelines, statistics pruning, the #49 COUNT(DISTINCT) rewrite, two-level aggregation, top-N.
- **Operators have no names, no stats and no accessors to their inputs.** A part pipeline is built again for every
  part and runs on worker threads, so its operators are not reachable from the root.
- **The hermetic tests forbid wall-clock assertions** (AGENTS.md, ADR 0013), so anything tested must be counts.

## Decision

- **Surface: `antb1 explain --analyze`** (with `--threads`, `--memory-limit`, `--format text|json`).
  - It runs the query, drops its rows and prints every physical operator with its numbers.
  - Without `--analyze`, `explain` is unchanged.
  - The SQL `EXPLAIN [ANALYZE]` statement is deferred (maintainer's choice, 2026-09-30). It would change the parser,
    the AST, the unparser round-trip and the fuzzer.
- **The profile is a tree of `exec::ProfileNode`, one per physical operator** (`src/exec/include/antb1/exec/profile.h`).
  - Each node holds a name, the logical node's EXPLAIN text (`plan::ExplainNode`), whether it runs per part, and its
    inputs.
  - Its counters are atomic: rows, batches, time and runs.
  - It also holds named metrics (counts, durations, bytes) under a small mutex.
- **Zero cost when off.** Profiling happens only when the plan is built with a profile root
  (`BuildPhysicalPlan(plan, root)`).
  - The Builder then wraps each operator in `ProfiledOperator`, which times Open, Next and Close and counts rows and
    batches.
  - It hands the node to the operator (`Operator::set_profile`), so the operator can add its own metrics.
  - Without a root nothing is wrapped. Operators check one pointer at phase boundaries, never per row.
- **Per-part aggregation.** A part pipeline's nodes are created by its first build (the schema sample, at planning
  time). Every part's operators point at the same nodes and add into them from any thread.
  - Counts (rows, batches, runs, parts, groups) are exact and do not depend on the number of threads when every
    part is read once. Two cases break that: a LIMIT stops parts, some already started (how many depends on the
    threads), and a part that ran out of memory next to others runs again alone.
  - Times are wall time on each thread, summed over the parts, and are never compared in tests.
- **Time semantics.**
  - An operator's time includes the inputs that run inside its calls on the same thread. Its self time subtracts
    them.
  - A part operator's per-part inputs run on other threads. Their summed time is shown separately, and not subtracted.
- **Operator metrics (named, in a fixed display order):**
  - part operators:
    - `parts` read and `skipped` by statistics;
    - `part_time` (summed time of their part tasks);
    - `wait` (the consumer waiting for parts).
  - `PartGroupAggregate` and `PartTwoLevelAggregate`: `lanes_tail`, the merges after the last part, and `groups`.
    - `PartGroupAggregate` also reports `build`, the row building.
    - `PartTwoLevelAggregate` also reports `sample_parts`, `heavy_keys`, `outer` and `heavy_groups`.
  - `PartAggregate` and `PartTopN`: `merge`.
  - `TopN` and `Sort`: `sort`.
  - The query: its time, result rows, threads and peak memory (`MemoryBudget::ResetPeak` before the run).
  - Update (2026-10-09, J1b): a hash join shows its probe, `HashJoin` (per part in a part pipeline), with `find`,
    `gather`, `residual` and, on the 1:1 path, `window_rows` (the probe rows it gathered the build's columns for,
    against the rows it returns); and its build, `HashBuild`, under the operator that prepares it, with the rows its
    table holds, `parts`, `skipped` (a build from a part pipeline), `part_time`, `wait`, `lanes_tail`, `finish`,
    `null_keys`, `unique` and `direct`. The display order lists them where they come as a query runs.
  - Update (2026-10-09, E2a): semi, anti, null-aware anti and one-row joins show the same two lines and metrics. A
    probe has `find` only when it looks keys up: not a one-row join's, nor one that keeps every row or none without
    looking (an anti join over a build without rows, a null-aware anti join over an empty build input or one with a
    NULL key); `gather` only for an inner join's columns (the build columns it gathers, and on its 1:N path the probe
    rows it takes) and a one-row join's values, which its build makes once, within its `finish`; `window_rows` only on
    an inner join's 1:1 path. No metric is new.
- **Output.**
  - Text: the `Output:` line, a `Total:` line, then one line per operator indented as EXPLAIN, with the numbers in
    brackets.
  - JSON: one object, times in nanoseconds and memory in bytes, for scripts and bench tooling.

## Consequences

- **Measurements come from the product,** reproducibly, without patching code. They matched the hand-made timers of
  the Q32 investigation: the lane tail at 1.1 s, row building at 0.4 s and the top-N at 1.0 s.
- **Adding an operator means naming it in the physical planner.** Where it has phases worth seeing, it adds metrics
  through `profile()` with `ProfileTimer` (architecture.md, "Where to add things").
- **Overhead when on:** one clock read pair per batch and per operator, plus a mutex per metric update at phase
  boundaries. It is small next to the batches of 64Ki rows. Off, there is no wrapper and no clock.
- **Tests:** exact counts in unit tests (on 1 and 4 threads), and regexes for times and bytes in CLI goldens.
- **Not in this ADR:**
  - per-operator memory peaks;
  - profiles in `antb1 bench` JSON;
  - the SQL `EXPLAIN` statement.

## Alternatives considered

- **Virtual `name()`/`children()` on every operator, with the profile built from the operator tree:** part pipelines
  are rebuilt per part and never reachable from the root, so their stats would need merging anyway. The planner
  already knows the tree.
- **Counters inside every operator instead of a wrapper:** every operator would repeat the same timing code, and the
  cost would be there when profiling is off.
- **Sampling profilers (perf):** they show functions, not operators or phases. They need symbols and root-like
  access, and cannot tell part work from merges. They remain useful for hot loops.
