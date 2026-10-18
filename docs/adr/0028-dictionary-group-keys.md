# 28. Grouping a Parquet dictionary instead of its rows

Date: 2026-10-18

## Status

Rejected

This record exists for its measurements. Two earlier ADRs left the question open on reasoning rather
than numbers — [ADR 0020](0020-filter-pushdown.md) deferred "evaluating string predicates once per
dictionary entry" for dictionary-encoded pages, and [ADR 0027](0027-string-views.md) rejected
"dictionary-encode VARCHAR columns" as a general answer — and the answer turns out to hinge on a
property of Arrow's Parquet reader that neither could have predicted.

## Context

After the two grouping changes of 2026-10-18 the engine runs ClickBench in 20.20 s against DuckDB's
18.07 s, down from 22.81 s. A profile of the queries that group by a string puts **30-34% of their
CPU inside `arrow::compute::Grouper`**, and the two ways of attacking that directly are both spent:

- a hash table of our own ([ADR 0019](0019-own-aggregation-hash-table.md)) reaches **parity**, twice
  confirmed, most recently rebased on both merged changes — 0.96-1.03x over seven fixed-width-key
  queries;
- the inline-string key layout of ADR 0027 S2 only resolves *false* candidates, about 5% of probes,
  because DuckDB's 12-byte inline window covers 0.01% of this data's URL rows and ~2% of its
  SearchPhrase rows. A true match — 82% of the probes of the worst query — reads every byte in
  either engine.

What remained was not a faster probe but **fewer probes**. Every string column of the data is
dictionary-encoded in all 226 row groups, and the engine discards that: the scan expands each column
to dense `arrow::binary()` and the grouper then hashes and compares every row. The idea was to group
each dictionary once and map rows through their indices.

It looked excellent. Reading one row group of the widest column (450,560 rows, 56,316 distinct) as
`dictionary<int32, binary>` and handing it to the grouper took **6.27 ns a row against the dense
column's 27.70 — 4.4x**, for the same group count. Arrow's grouper accepts a dictionary key type and
exploits it; `take`, `filter`, `sort_indices`, `equal`, `unique`, `is_in`, `if_else` and `cast` all
accept one too, so unlike ADR 0027's view layout the compute surface was not the obstacle. Arrow even
enforced the boundary for us: a grouper consumes dictionary batches while the dictionary is identical
and refuses with `NotImplemented: Unifying differing dictionaries` otherwise, and since a part is one
row group ([ADR 0013](0013-parallel-execution.md)) "a dictionary is valid within one part" was both
the rule we would have written and the rule the library checks.

## Decision

**We will not read Parquet dictionaries as group keys.** Three measurements, in the order they
arrived, take the 4.4x down to about 3% of one query:

1. **The reader rebuilds a dictionary per batch, not per row group.** Fed through
   `GetRecordBatchReader` at the engine's 64Ki batch size, the same row group arrives as seven
   batches with seven different dictionaries, and the grouper fails outright on the second one. The
   4.4x was an artefact of reading a whole row group as a single `ExecBatch`. "One dictionary per row
   group" is true of the file's encoding and false of what the reader hands over.
2. **Doing the pass ourselves recovers only 1.5x.** The grouper's dictionary support is not needed:
   group each batch's distinct values (dense binary, long supported) and map rows through the
   indices. That works at any batch size, and the win is entirely a function of how often the
   dictionary is rebuilt:

   | batch size | dictionary entries a row | dense | ours | |
   | --- | ---: | ---: | ---: | ---: |
   | **64Ki (the engine's)** | 0.57 | 27.18 ns | 17.97 ns | **1.51x** |
   | 128Ki | 0.37 | 27.44 | 13.90 | 1.97x |
   | 256Ki | 0.21 | 27.57 | 9.89 | 2.79x |
   | one batch a row group | 0.12 | 28.02 | 7.77 | 3.61x |

   A narrower column (SearchPhrase, 13,967 distinct a row group) reads 1.38x at 64Ki.
3. **The batch size cannot be raised to compensate.** End to end, 256Ki batches cost **+2.9%** over
   ten representative queries and are worst on the very queries this would have helped (+6%, +7% and
   +19% on the three largest grouped ones), while smaller batches are far worse (+9.5% at 32Ki,
   +33.5% at 16Ki). 64Ki is at a local optimum — a useful result in itself, since the constant had
   never been measured.

And only part-local rows could have benefited in any case: the worst query routes 43.8 M of its
100 M rows straight past its part table, and routed batches reach a lane carrying different
dictionaries. That leaves about 1.53 s of addressable CPU in a query that burns roughly 18, so 1.51x
on it is **about 3%** — against the 10% this was asked to clear.

So the cost was a public-header change in `plan` (a per-column request on `plan::Table::ScanPart`
and an amendment to its "columns typed as in `schema()`" contract), a decode path in `io`, an
eligibility rule in the physical planner, and a dictionary arm plus two `cast` points in
`exec::GroupTable` — four modules and a retention hazard, for 3% of one query.

## Consequences

- ADR 0020's deferred item and ADR 0027's rejected alternative are both now answered with numbers
  rather than reasoning, and the reason is recorded where the next person will look: **the reader's
  per-batch dictionary**, not the compute surface and not the cross-part unification rule, which
  were the two things both ADRs expected to be the obstacle.
- The engine's 64Ki batch size is measured, not assumed, and is at a local optimum in both
  directions. A change to it should quote better numbers than these.
- `arrow::compute::Grouper` accepting a dictionary key type is recorded as a fact for whoever next
  needs it. It is only useful to an operator that can keep one dictionary across every batch it
  consumes — which this engine's scan does not provide.
- Nothing in ADR 0027's S1 or S2 is affected: the view layout is a different representation and its
  own measurements stand.
- What is *not* closed: the same trick inside `io`'s filtered scan path, where a pushed predicate
  could be tested once per dictionary entry rather than once per row. That is ADR 0020's deferred
  item, it is a different mechanism (a filter, not a group key), and nothing here measures it.

## Alternatives considered

- **One batch per row group for eligible scans.** The measured fallback, and the reason the decision
  is a rejection rather than a deferral: it is the only way to reach 3.6x, and item 3 above shows it
  costs more elsewhere than it buys here.
- **Pruning each routed slice's dictionary** so the routed path could benefit too. `take` on a
  dictionary keeps every entry, so a lane would see a part's whole dictionary for a sixty-fourth of
  its rows — worse than dense. Pruning means a pass over the indices per slice, which is the work
  being avoided.
- **A dictionary as the engine's VARCHAR representation**, decoded lazily. Feasible in a way ADR
  0027's views were not, since most kernels accept one — but `binary_length`, `match_substring`,
  `min_max` and `count_distinct` all refuse, which is STRLEN, LIKE, MIN of a string and
  COUNT(DISTINCT), so the decode points would be everywhere the data actually goes. Rejected for the
  same reason ADR 0027 rejected the equivalent: a whole-engine change before one query gets faster.
