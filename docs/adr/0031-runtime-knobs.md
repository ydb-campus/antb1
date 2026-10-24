# 31. Runtime knobs: a decision switchable without a rebuild, globally and per operator

Date: 2026-10-19

## Status

Proposed

## Context

- **The problem, stated by the maintainer.** Decisions are activated unconditionally or by a heuristic; a few can be
  overridden from the command line, and those are global. The ask is for knobs adjustable **without patching source
  or rebuilding**, so a feature can be switched off and its profit measured *even once later optimizations land* --
  and for the same control **per operator**: leave the defaults everywhere and change one operator, or switch a
  feature off globally and enable it for one operator.
- **Why this is not a convenience.** [ADR 0026](0026-join-performance.md)'s C12 needed four builds of four trees to
  answer "what was this worth", and could only do it for work already in `git`. Three findings of that entry exist
  because a rebuild was the only way to get them: D2 earns 0.2% because D1 had already taken its prize, C6's gain
  was credited to the wrong ADR until its commit was located, and C2c's named beneficiary turned out not to move.
  **Every one of those is a question a knob answers in one run.** It is also the only way to keep an off-path honest
  as the engine changes: a measurement taken once decays, and today nothing re-takes it.
- **The engine already has this pattern, for exactly this reason.** `plan::BindOptions`
  (`src/plan/include/antb1/plan/plan_options.h:30`) carries `build_side_ratio` and `join_order`, reaching the CLI
  as `--build-side-ratio` and `--join-order` (`src/cli/cli.cc:116`). The header's own words about `join_order`:
  it "is kept so that a plan can be compared against it and so that a query whose order the search gets wrong has
  somewhere to go". That is this ADR's motivation, already written down for one decision. **So the global layer is
  not a new mechanism; it is one existing pattern applied to the decisions that never got it.** Two details of that
  pattern are binding: `--build-side-ratio` is converted to **integer hundredths** at `src/cli/cli.cc:224` so that a
  plan depends only on metadata and is identical on every host, and `engine::SessionOptions::batch_size`
  (`src/engine/include/antb1/engine/session.h:31`) exists with no CLI flag at all -- a knob need not ship a flag.
- **What does not exist.** There is no `SET`, no `PRAGMA`, no settings statement (`sql::Parse` returns one
  `SelectStatement`), no configuration file, and **no environment variable that production code reads for engine
  behaviour**: the only `getenv` in `src/` is `src/cli/allocator.cc`, for third-party allocator setup under
  [ADR 0017](0017-allocator-keeps-memory.md). Every `ANTB1_*` variable belongs to a script or a test. There is
  also **no warning channel**: `plan` and above return `arrow::Status`, which is a failure or nothing, and a knob
  system needs to say
  "I could not apply what you asked" without failing the query.
- **No node has an identity today.** `plan::LogicalPlan`'s nodes are a `variant` behind `shared_ptr` with no id;
  `ColumnId` identifies columns, not nodes. `exec::BuildPhysicalPlan`
  (`src/exec/include/antb1/exec/physical_planner.h:29`) takes a plan and a profile and **no options at all**, and
  one logical node becomes one physical operator *per part*, through a `PartPipeline` factory that is called
  concurrently and captures everything by value. So a per-operator layer needs both a name for a node and a new
  parameter to carry the names in.
- **The candidate decisions, measured and located.** `kFilterBuildShare = 16`
  (`src/exec/physical_planner.cc:556`, ADR 0029's D1), `kMinSharedRows = 10'000'000`
  (`:1188`, ADR 0030's E2), `KeyFilter::kMaxPublishedKeys`
  (`src/exec/include/antb1/exec/key_filter.h:75`, ADR 0029's D2), `KeyFilter::kMaxBytes` (`:68`, moved once
  already on measurement, which is the profile of a knob), `MaterializedTable::kMaxBytes`
  (`src/exec/materialized_table.h:42`), `GroupTable::kPartitions = 32` (`src/exec/group_table.h:55`), the
  direct-layout gate (`src/exec/join_table.cc:641`), and the attachment sites of D1, D2 and D5 in
  `src/exec/physical_planner.cc`, several of which have no gate at all and would need a knob rather than an exposed
  constant.
- **And the discovery that decides the shape of this ADR: three of those are not answer-neutral, and one is not
  fixable.** A survey-and-critique pass over three independent designs found that the obvious knob set is wrong:
  - **`GroupTable::kPartitions` must never become a knob.** Its own comment (`src/exec/group_table.h:47`) says
    the count is "a constant, never the number of threads, so that the result is the same for any number of
    threads", citing [ADR 0013](0013-parallel-execution.md). The code therefore *documents the result as depending
    on it*. A top-N above a grouped aggregate becomes a `PartitionTopN` whose candidate set is bounded per
    partition, so changing the count changes which rows are candidates and a tie at the limit boundary resolves
    differently. Separately, `src/exec/group_table.cc:149` is
    `ANTB1_CHECK(std::has_single_bit(partitions))`, and `ANTB1_CHECK` calls `std::abort()`
    (`src/common/include/antb1/common/check.h:17`), so a value of 1000 would abort the process -- against this
    ADR's rule that a bad option warns and never aborts.
  - **A knob that admits more work can turn a query that answers into an error.** Enabling a sharing group commits
    the query to `MaterializedTable::kMaxBytes`, which fails it at `src/exec/materialized_table.cc:64` with a
    comment that sharing "cannot be abandoned here"; forcing the direct layout reaches `Affordable`
    (`src/exec/join_table.cc:131`), the memory-failure guard.
  - **`kFilterBuildShare` is a divisor**, at `src/exec/physical_planner.cc:646` and `:784`. A share of `0`, which
    reads naturally as "never attach", is integer division by zero.
  The lesson generalises and is the spine of the Decision: **a knob is not safe because it looks like a performance
  constant.** Safety has to be proved per knob and enforced by a test, and the direction a knob may move matters as
  much as its range.

## Decision

Draft. What follows is the shape to build and measure, not a decision taken.

### A knob is a named value from a closed registry, and the registry is a test input

One enumerator per knob in `antb1::common`, with its kind, range, default, the decision it gates and its **class**.
`common` is the only module below both `plan` and `exec`; a knob set is plain data, so it needs no Arrow and the
module edges of `cmake/Antb1Modules.cmake` are untouched. Parsing returns `std::expected`, as `common` does, and the
`plan` boundary converts through `sql_status.h`.

The registry being closed and enumerable is what makes the next rule enforceable rather than aspirational.

### Rule 1: a knob may never change an answer, and that is proved by two tests, not by inspection

For every knob in the registry, a test asserts over a fixture set that the off arm and the on arm produce
**byte-identical answers**. That test alone is not enough, and the reason is the sharpest finding of the design
pass: **a knob that is never read produces two identical arms and passes vacuously.** So each knob additionally
owes an **observable-difference witness**: on some fixture it must change a *profile metric* while the answer stays
identical. Wired and neutral then becomes a tested conjunction, and a knob cannot enter the registry without both.

This is what ADR 0029's D3 made possible: every mechanism already reports what it pruned, so the witness has
something to read.

### Rule 2: polarity -- a knob may only lower resource use, never raise it

A knob may say "do not bother" where the engine would have acted. It may **not** say "act where the engine's cost
gate said no", because every such gate guards a resource, and relaxing one can turn a query that answers into an
error -- which Rule 1 does not catch, since an error is not a wrong answer. So `share-sub-plan` and `direct-layout`
are **off-only** knobs, and a value knob that feeds a memory bound may only move in the safe direction.

A value substituted into arithmetic gets a validated range that excludes the degenerate value, not a range that
merely looks generous: `join-filter-share` has a floor of `1`, and "never attach" is the separate boolean, because
the share is a divisor.

### Rule 3: the phase split -- per-operator knobs are exec-phase only, which makes an id stable by construction

Knobs divide in two by *when they are consumed*:

- **Plan-phase** knobs are read during `Bind` or `Optimize` and change the plan: `build-side-ratio`, `join-order`,
  and the semi-join placement of ADR 0029's D0. These stay **global**, as they are today.
- **Exec-phase** knobs are read by the physical planner or an operator and change no logical plan: every
  attachment gate of ADR 0029 and ADR 0030, and the key-publication cap.

**Only exec-phase knobs get a per-operator form.** The consequence is the point: ids are assigned over the
*optimized* logical plan, and an exec-phase knob by definition does not change that plan, so **a per-operator knob
cannot invalidate the id it is addressed to**. The maintainer accepted "some options may change the plan, so old ids
are unresolved"; this is strictly better than the ask -- for per-operator knobs it cannot happen at all, and only
editing the SQL or moving a *global plan-phase* knob can move an id.

Without the split the scheme attacks itself: a per-operator knob that moved the semi join would change every
ancestor's shape and therefore the id of the node it names.

### Rule 4: an operator is addressed by what it reads, not by a hash of it

An address is typable and read off `EXPLAIN`: the operator's kind and the relations beneath it --
`scan:lineitem`, `join:customer+orders` -- with an occurrence suffix only where that is ambiguous. Addresses are
printed by `explain --addresses`, behind a flag so that the 28 existing `tests/cli/golden/explain_*` files stay
byte-identical.

**This rejects the salt-and-hash mechanism the maintainer proposed, while adopting the requirement behind it.** The
requirement -- never silently target the wrong operator -- is right and is the worst failure available here: a
sweep that hits the wrong operator records a number under the wrong label and nobody finds out. A truncated salted
digest buys a *probability* of avoiding that. Verifying uniqueness at resolve time and **refusing an ambiguous
address** buys
certainty, and costs nothing to compute. A hash also brings problems a typable address does not have: it is not
reproducible if it uses `std::hash` or unordered iteration order; it would embed an absolute path for
`FROM '<tmpdir>/t.parquet'`, which `src/plan/scope.h:38` accepts, making ids machine-dependent and the tests
non-hermetic; and it cannot be read off a plan by a human, which is the whole use case. The precedent is
PostgreSQL's `pg_hint_plan`, which names relations rather than nodes, and the survey found no system that addresses
an operator by a stable id across replans -- the vendors that got closest (SQL Server plan guides, Oracle SQL plan
baselines) key on a hash of the **query text**, not of a node, and deliberately exclude hints from that hash.

### Rule 5: nothing a knob can change may enter an address, and every outcome is reported

The second half of self-consistency: if a knob's effect appeared in the address, setting it would un-address its own
target. So the fields a knob can move -- `JoinNode::build`, `JoinNode::estimate` -- are excluded from an address,
as SQL Server excludes hints from `query_hash`, and a test asserts per knob that addresses are invariant under it.

Resolution has four outcomes and **each is reported through a new diagnostics channel** (a vector of warnings on the
result, printed by the CLI to stderr), because silence is the failure mode:

| outcome | meaning |
| --- | --- |
| applied | resolved to one operator, **and that operator read it** |
| unresolved | no operator matches -- the plan changed; a warning, as the maintainer asked |
| ambiguous | several match; nothing is applied, because guessing is the a wrong target |
| resolved but unread | it named a real operator that never consults this knob -- a wiring bug, and the one a measurement harness must never swallow |

`--knobs-strict` turns anything but `applied` into a non-zero exit, because for a benchmark harness a silent no-op
is worse than a failure.

## Consequences

- A new public parameter on `exec::BuildPhysicalPlan`, a new header in `common`, a new CLI surface and a new
  diagnostics channel. No new module edge.
- `explain --addresses` is a new output; the existing 28 explain goldens do not move, and the new output gets its
  own golden.
- Three of the eight decisions that look like knobs do not become knobs, and the ADR says so with the reason at
  `file:line`. `GroupTable::kPartitions` is excluded **permanently**, not deferred.
- The knob off-path becomes a tested configuration, which is the durable form of what C12 had to rebuild for. It is
  also a cost: every knob is two tests and a registry entry forever, and a knob whose feature is later deleted must
  be deleted with it.
- The differential test gains a dimension: `diff-random` can perturb knobs per seed, so answer-neutrality is
  re-checked as the engine changes rather than asserted once.

## Plan

- **K1, feat(common,plan,exec,cli): the registry, the global layer and both gates.** The enumerators, parsing, a
  `--set name=value` surface, the diagnostics channel and `--knobs-strict`; the answer-equality test and the
  observable-difference witness for each knob shipped. The knob set is the exec-phase, answer-neutral, lowering-only
  subset: D1's and D5's attachment, D2's part skip, E2's sharing (off-only), the key-publication cap, and
  `join-filter-share` with a floor of 1. **This item alone satisfies "switch a feature off and measure its profit
  without rebuilding"**, which is most of the stated value and needs no identity machinery at all.
- **K2, feat(plan,cli): an address per operator, and `explain --addresses`.** Addresses computed over the optimized
  plan, uniqueness verified, ambiguity reported; the invariance test of Rule 5. No behaviour change.
- **K3, feat(exec): per-operator overrides.** `--set name@address=value`, precedence global-then-address, the four
  outcomes, and the read-accounting that makes *resolved but unread* detectable.
- **K4, test(diff): perturb knobs per seed.** The answer-neutrality claim becomes continuous rather than a
  one-time assertion.
- **K5, docs(adr): record what K1-K4 measured, and accept or defer.** The re-measurement that K1 makes cheap: the
  per-item figures of ADRs 0029 and 0030 re-taken on one binary, which is the first time they can be compared
  without four builds. Protocol as in ADR 0026's C12, whose lesson applies -- a matched pair reproduces to about 5%
  on the ratio, and arm order fixes the sign of the residual.

## Alternatives considered

- **A salted, truncated structural digest of the operator's subtree**, reusing ADR 0030 E1's `SubtreeKey` and
  `Origins` (`src/plan/shared_sub_plans.cc`). The most appealing option, since the machinery exists, is already
  deterministic and pointer-free, and already has a determinism test. Rejected as the *primary* address for the
  reasons under Rule 4 -- an embedded table path is machine-dependent, a truncated digest trades certainty for
  probability, and it cannot be typed by a human -- and because E1 deliberately excludes a Scan's field list from
  the key, which is right for an equality test and a collision for an address. Worth revisiting only if addresses
  prove ambiguous in practice, where a digest would become the disambiguator rather than the name.
- **Minted sequential node ids**, in the spirit of `ColumnId`. Cheap, exactly reproducible and collision-free
  within a query. Rejected because an id a user holds must survive a rebuild and an unrelated edit, and a mint
  ordinal moves when any earlier node is added; the critique also found that the optimizer rebuilds nodes with
  designated initializers at a dozen sites (`src/plan/optimizer.cc`), several of which would silently drop an id
  field. Kept as the *tiebreak* ordinal inside an ambiguous address.
- **A `SET` statement in SQL.** The natural interface, and what every surveyed engine offers. Rejected for now:
  `sql::Parse` returns one `SelectStatement`, so a settings statement is a parser and binder change of its own, and
  it would put knobs in the SQL the differential tests generate. A CLI surface reaches the same measurements
  without touching the language.
- **Environment variables.** Rejected: no production code reads one today, the repo's convention is that `ANTB1_*`
  belongs to harnesses, and `std::getenv` is not thread-safe -- the codebase walks `main`'s `envp` instead
  (`tests/slt/runner/main.cc:148`). A flag is explicit and shows up in the command a PR quotes.
- **Leave it to `git` and a rebuild**, which is the status quo and what C12 did. Rejected on that entry's own
  evidence: it cost four builds, it cannot measure a feature against *future* optimizations, and it produced two
  attributions that had to be corrected after the fact.
- **A knob per decision with no registry and no answer test.** Rejected because the design pass found three
  answer-unsafe knobs in a set of eight chosen by people reading the same code. Without the registry there is
  nothing for a test to enumerate, and the one failure mode nobody notices -- a knob that resolves and is never
  read -- is invisible by construction.

This workload is derived from the TPC-H Benchmark and is not comparable to published TPC-H Benchmark results, as
this implementation does not comply with all requirements of the TPC-H Benchmark.
