# 32. Aggregate placement: computing a grouped aggregate below the join that reads it

Date: 2026-10-22

## Status

Proposed

## Context

A join multiplies rows and a grouped aggregate reduces them. When the aggregate sits above the join and reads only
one side of it, the two can be exchanged: group that side first, join the partial groups, and finish the
aggregate above. Yan and Larson named this *performing group-by before join* (SIGMOD 1994). It pays when the
reduction happens before the multiplication rather than after, and in the good case it removes the multiplication
entirely, because a pre-grouped relation has exactly one row per key.

- **One query of the 22 is a textbook instance, and it is the worst of them against DuckDB by peak memory.** Q13's
  shape -- customers left-joined to orders on the customer key, counting orders per customer, then histogramming
  the counts -- has antb1 build a hash table on **148,451,295** order rows, probe 15,000,000 customers to emit
  **153,451,474** rows, and group those back down to 15,000,000. Measured at SF 100 on four threads, moving the
  aggregate below the join:

  | | as written | pre-aggregated |
  | --- | ---: | ---: |
  | hash build | 148 M rows | ~10 M groups |
  | join output | 153 M rows | 15 M rows |
  | final grouping input | 153 M rows | 15 M rows |
  | time | 4.581 s | **2.263 s (0.494x)** |
  | peak memory | 14,325 MiB | **1,554 MiB (0.108x)** |

  Answers identical over all 48 rows. The ratio held across four independent measurements taken days apart
  (0.547x, 0.546x, 0.494x), so it is not a measurement artifact. **The memory figure is the stronger half of the
  case**: that is the difference between a query needing 14 GB and one fitting in a container.

- **A survey of all 22 found the shape in 14 of them and worth doing in 2.** Cardinalities are exact counters from
  `explain --analyze` at SF 1, which the generator makes scale-stable; "reduction" is the pre-aggregation's rows in
  against groups out, and "keeps" is the fraction of those groups surviving the joins above.

  | query | reduction | keeps | verdict |
  | --- | ---: | ---: | --- |
  | Q13 | 10:1 | 100% | **worth it** -- 0.494x, peak 0.108x |
  | Q10 | 3.02:1 | 100% | **worth it** -- 0.935x, peak 0.827x |
  | Q9 | 1825:1 | 100% | no effect -- the ratio is a false positive, see below |
  | Q7 | 1484:1 | 4% | refuse -- the aggregate's own input grows 25x |
  | Q21 | 9.88:1 | 4.1% | refuse -- input grows 23.9x, and it destroys a 24:1 prune on three scans |
  | Q18 | 4.0:1 | 100% | refuse -- destroys two existing prunes |
  | Q3 | 3.9:1 | 1.4% | refuse -- **1.419x slower, peak 4.17x worse** |
  | Q20 | 1.67:1 | 1.1% | refuse |
  | Q5 | 1.30:1 | 4.0% | refuse |
  | Q14 | 1.20:1 | 100% | refuse |
  | Q8 | 1.015:1 | 6% | refuse |
  | Q2, Q11, Q12 | 1.00:1 | -- | refuse without costing |
  | Q1, Q4, Q6, Q15, Q16, Q17, Q19, Q22 | -- | -- | no shape |

- **The reason so few qualify is structural, and it generalises past this benchmark.** Condition 3 below forces the
  pre-grouping key to be the join key together with every column of the outer grouping set that comes from the
  pre-grouped side -- and in a star-like schema that union is usually a **key** of that side, which makes the
  reduction 1.00:1 by construction. Q2, Q11 and Q16 are refused on that alone, with no cardinality estimate
  needed. Where the grouping columns all come from the other side the pre-grouping is the join key by itself, and
  the available join keys are either the other table's primary key (Q12, Q18) or near-unique in the already
  filtered rows (Q8 at 1.015:1, Q14 at 1.20:1). Q13 qualifies because its pre-grouping key has a genuine ~10:1
  fanout **and** the outer join above it discards nothing. Only Q10 shares both properties.

- **The obvious discriminator does not work, and it fails in both directions.** "Reduce a lot and discard little"
  is the natural rule and it is wrong. Q9 scores the best marks in the entire survey on both axes -- 1825:1
  reduction, nothing discarded -- and the transform provably does nothing to it: its aggregate consumes 319,404
  rows and emits 175 groups **in both forms**, so the ratio was being read off a quantity that the push leaves
  unchanged. Every hash build in Q9's plan is unique on its key, so no join there multiplies and there is nothing
  for a pre-aggregation to shrink. In the other direction the ratio does not even order the two cases that matter:
  Q10 wins at 3.02:1 and Q3 loses at 3.9:1.

- **The mirror direction was examined and is not supported by measurement.** ADR 0023's Path 2 leaves Q20 with a
  grouped aggregate *below* its join, where the aggregate turns 91,030,658 rows into 54,543,918 groups to serve a
  probe of 868,888 rows -- 99% of them discarded, which looks exactly like a case for pulling the aggregate back
  above the join. A hand-written form that groups after the join measured 0.881x and then 0.778x. **Both were
  measured arm after arm, and interleaved and rotated over six repeats the same pair is 1.010x** -- the apparent
  win was the second arm running on a warmer page cache every time. Peak memory does improve, 7,301 to 6,716 MiB
  (0.920x). So there is no time to recover here, and this ADR decides only the forward direction.

- **A prerequisite had to be fixed before any of this could be measured honestly, and it is the sharpest lesson of
  the work.** Every rewrite that introduces a derived table exposes join keys whose domains the binder cannot
  read, and the estimator substituted each relation's row count for an unknown domain -- in a divisor, where the
  largest reading is the fewest rows. The first attempt at this transform produced a plan **the search ranked
  cheapest at 150,000 rows that produced 6,697,748,923**, running 145.2 s where the same query with that one join
  ordered differently runs 140.6 ms. That was an estimator failure and not an unavoidable join graph: two of the
  three orderings of that triangle avoid the bad edge entirely. Fixed in ADR 0022's 2026-10-22 update, and **this
  item cannot be built on an estimator that reads a derived table's joins four orders of magnitude low**, because
  the gate below would be fed the same numbers.

## Decision

- **The rewrite.** With `R` carrying the grouping columns and `S` the side the aggregates read:

  ```text
  γ_{G, F(v)}( R ⋈_{R.k = S.k} S )
      =
  γ_{G, F_final(p)}( R ⋈_{R.k = S.k} γ_{S.k ∪ (G ∩ cols S), F_partial(v) → p}(S) )
  ```

- **Legality, all five required.**
  1. every aggregate's input columns come from `S` only -- not from `R`, and not from an expression mixing them;
  2. every aggregate is decomposable (the table below);
  3. `S` is pre-grouped by its join columns **plus every `S` column appearing in `G`**;
  4. the join is an equi-join on those columns;
  5. where `S` is the null-supplying side of an outer join, the final step maps a missing partial to `F` over the
     empty set.

  **Condition 3 is the one that produces a wrong answer silently** if its second half is dropped: a partial group
  then mixes rows the outer grouping separates, and the answer comes back with too few rows and inflated measures.
  It must be enforced from the grouping set directly and **never inferred from a functional dependency**, because
  in both queries of the 22 where an `S`-side grouping column exists it happens to be determined by the join key
  -- so an implementation that forgets it still answers correctly there and fails elsewhere.

  **No multiplicity correction is needed in this direction.** `R`'s multiplicity replicates the partials exactly
  as it would have replicated the raw rows, since the same multiset of `S` rows is seen once per matching `R` row
  in both plans and merging is associative. The mirror transform does need one; it is out of scope.

- **Decomposition, which the engine already classifies.** ADR 0014's `TwoLevelAggregation` predicate already
  decides which calls are independent of merge order, and that classification is exactly what this rewrite needs:

  | kind | partial | final |
  | --- | --- | --- |
  | `kCountStar`, `kCount` | COUNT | SUM of partials |
  | `kSum` | SUM | SUM |
  | `kMin`, `kMax` | MIN / MAX | MIN / MAX |
  | `kAvg` | a (SUM, COUNT) pair | SUM/SUM, divided above the join |
  | `kCountDistinct` | -- | not decomposable unless the distinct column enters the pre-grouping |

  Integer SUM accumulates in `Int128` and COUNT returns HUGEINT on both sides of the split, and a partial SUM over
  a DECIMAL keeps its own scale (ADR 0021). Only COUNT has a non-NULL identity, so condition 5's mapping is
  injected per aggregate rather than once per node.

- **The gate, all six required.** Let `N_before` be the rows the aggregate consumes today, `N_pre` the rows it
  would consume at its new position, `G` the groups it would emit, `keep` the fraction of those surviving the
  joins above, and `G' = G × keep`.

  1. **Legality** as above.
  2. **Not a superkey**: the pre-grouping key is not a superkey of `S`. Refuses Q2, Q11 and Q16 with no costing,
     and is the highest-value guard because it is the dominant shape in this schema.
  3. **Do not push below a filter**: `N_pre ≤ N_before`. Refuses Q7, whose aggregate input would grow 25x, and
     Q21, 23.9x. This is the term a reduction ratio hides.
  4. **The join above must not discard**: `keep` at least about 0.9. Refuses Q3, Q5, Q7, Q20 and Q21. This is the
     half of the natural discriminator that survives, and the half separating Q13 from Q3.
  5. **Real work must move**: `N_before / G'` at least about 3, **and** at least one join between the two
     positions has a build that is not unique on its key. The second clause is what refuses Q9.
  6. **Do not destroy an existing prune**: the push must not remove a key filter the current plan relies on, nor
     flip a build-side choice. Refuses Q18 and Q21, and nothing else in the survey classifies those two
     correctly.

- **Weighted by what moves, not by how many rows move.** Condition 5's benefit is the rows removed times the
  per-row cost of the operators between the two positions, and that second factor spans two orders of magnitude
  across the survey: one case removes 319,229 rows from a 25-row broadcast join for 4.7 ms, another removes 76,738
  rows from two joins -- one gathering wide strings -- plus a seven-column hash aggregate, for 6.5 ms and 13 MB. A
  gate that counts rows without weighting by gather width and grouping-key width will rank them wrongly.

- **The mirror direction is refused**, on the measurement above, not deferred.

## Consequences

- **Two queries of 22 improve and twelve of the fourteen shape-positive ones must be refused**, so the gate is
  most of this item's value and nearly all of its risk. A wrongly gated rule is comfortably net negative: the survey
  produced four written rewrites that regress by 1.03x to 1.89x and one that regressed by about 2,700x before the
  estimator fix.
- **The case rests on peak memory more than on throughput.** Q13 is 0.494x on time and 0.108x on peak; Q10 is
  0.935x and 0.827x. Projecting the 22-query geomean from two queries gives roughly 3%, and that projection is
  dominated by one query -- it should not be quoted as a measurement.
- **No answer may change**, and unlike most items here that is not self-evident from the mechanism: a pre-grouping
  that merges rows the outer grouping would separate changes aggregate *values*, not row counts, so the oracle and
  the differential tester matter more than any counter assertion.
- `EXPLAIN` gains a node whose input is grouped where the written query grouped above the join, so plan goldens
  move for the queries the gate fires on.

## Plan

- **G1, feat(plan): find the legal placements and report them in `EXPLAIN`**, rewriting nothing. Following ADR
  0030's E1 precedent: a pass that only reports cannot give a wrong answer, and every later item depends on the
  legality test -- particularly condition 3's second half -- being right. It also replaces the survey's by-hand
  cardinalities with ones the engine computes.
- **G2, perf(plan,exec): perform the rewrite behind the gate.** Measured at SF 100 with peak memory beside time,
  interleaved and rotated, on Q13 and Q10 with the other 20 as the control. The gate's refusals are tested by
  construction, each with the query shape that must be refused and why.
- **G3, docs(adr): record what G1 and G2 measured**, and either accept this ADR or defer it with the numbers.

## Alternatives considered

- **Do nothing.** Defensible on throughput alone: two queries of 22, one of them worth 7%. What argues against it
  is the memory figure, which is the dimension antb1 looks worst on against DuckDB.
- **The mirror transform (lazy aggregation, and Yan and Larson's eager count).** Measured on the one query of the
  22 whose plan already places an aggregate below a join, and worth 1.010x on time. Refused.
- **A guard against the catastrophic join shape instead of a gate on the rewrite.** Penalising a join whose only
  edge has a tiny key domain fires on 9 of the 22, every one a dimension lookup whose small side's key is its
  primary key -- including the join that ADR 0026's C5 rule exists to protect. The discriminating condition is
  non-uniqueness on both sides, which the metadata gives for a base table and cannot give for a derived one, so
  the fix belonged at the estimator's input and landed there instead (ADR 0022).
- **Raising `KeyFilter::kMaxBytes` so a published key filter could rescue the Q3-like cases**, whose builds exceed
  the 16 MiB cap. Measured 16 to 32 MiB at SF 100: **1.001x over the 22**, and Q3 still receives no filter,
  because what blocks it is ADR 0029's uncarried offer through a pushing `Filter` and not the cap. The cap is not
  the binding constraint for any query of the 22.
