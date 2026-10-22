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
  | Q10 | 3.02:1 | 100% | **worth it at a placement this ADR does not describe** -- see the G2a update |
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

- **Correction (2026-10-22, from G2a): Q10's 0.945x is at a placement this ADR's Decision does not describe.**
  The rewrite below exchanges an aggregate with **one** join. Q10's measured win pre-groups two joins further
  down, and the placement the single exchange actually reaches -- pre-grouping at the join to the nation
  dimension, by the seven columns condition 3 requires there -- measures **1.245x slower** at SF 100 (3.155 s to
  3.926 s, peak 5,164 to 6,819 MiB), answers identical, interleaved and rotated. **The gate refuses it**, so
  nothing is wrong with the gate; what is wrong is counting Q10 among the queries this decision wins. An
  iterated exchange, pushing an aggregate along a chain of joins, would reach the measured placement and is not
  part of this ADR. On its own evidence, then, **this decision is worth one query of the 22** and the honest
  reading of its case is Q13's 0.494x and 12.3x memory alone.

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
  5. where `S` is the null-supplying side of an outer join, the final step supplies, for a preserved row with no
     partial, what the written query answered over the one padded row the join gave it. **That is not quite "F
     over the empty set", and COUNT(*) is where the difference bites**: it counted the padded row, so its value
     is 1, where `COUNT(col)` on that side counted a NULL and is 0, and every other kind is NULL. Corrected
     after item G1's review caught the earlier wording, which gave 0 for both counts.

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
  a DECIMAL keeps its own scale (ADR 0021). Only the two counts have a non-NULL value for a preserved row with
  no partial, and they differ from each other, so condition 5's mapping is injected per call and never once per
  node.

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

- **One query of 22 improves at the placement this decision reaches** -- Q13 -- and the rest of the fourteen
  shape-positive ones must be refused, so the gate is most of this item's value and nearly all of its risk.
  **As of G2b that one query does not improve yet**: it is a count, and merging partial counts needs the kind
  item G2c adds.
  Q10's win needs an iterated exchange this ADR does not describe (see the correction in the Context). A
  wrongly gated rule is comfortably net negative: the survey
  produced four written rewrites that regress by 1.03x to 1.89x and one that regressed by about 2,700x before the
  estimator fix.
- **The case rests on peak memory more than on throughput, and on one query.** Q13 is 0.494x on time and
  **0.108x on peak**, which is the number that carries this decision: 14,325 MiB to 1,554. Against the 22 that
  is about 1% of the total time, so **this is not a throughput item** and should not be sold as one. It is worth
  doing for what it does to the memory of the query that needs 14 GB.
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
  - Update (2026-10-22, G1): **shipped.** It reports **8 of the 22**, and every query the survey called
    shape-positive but G1 does not report is one of its own documented refusals: a Project between the aggregate
    and the join for five of them, and for one an aggregate whose `CASE` argument reads both inputs, which is
    condition 1 and which the survey had independently noted would need an extension to be legal at all. Both
    queries this ADR says are worth doing are reported.
  - **Two things G1's review corrected, each of which would have become a wrong answer in G2.** A join carrying
    a residual was reported as legal and is not: the residual decides which pairs match on something other than
    the keys, so the pre-grouping destroys the per-row values it reads and a partial aggregates rows the residual
    rejects, which no final step can take back. And condition 5's value above was wrong for `COUNT(*)`. Neither
    could reach an answer, because G1 rewrites nothing -- which is the whole argument for shipping a reporting
    pass first, and it earned its keep on the first review.
- **G2 split into two, because the gate needs a cardinality estimate this planner has never had.** Nothing
  estimates the groups a grouping would make: `EstimateRows` returns a `GroupAggregate`'s *input* rows as an
  upper bound (`join_order.cc`), which is exactly the quantity the gate's reduction test cannot use. Building
  that estimate and then verifying the gate against this ADR's own table is worth doing before any answer can
  move, on the same reasoning that made G1 a reporting pass.
- **G2a, feat(plan): estimate the groups a pre-aggregation would build, and report the gate's verdict**, still
  rewriting nothing.
  - Update (2026-10-22, G2a): **shipped, and it admits exactly one placement of the 22 -- Q13's**, reported as
    `worth 1500000->149999`, which is the 10:1 the survey measured by hand. Every other legal placement is
    refused, and the one refusal worth examining is Q10's (below).
  - **Three of the six gate conditions are not decidable in a logical plan**, and saying which is more useful
    than pretending otherwise. "Not a superkey" has no metadata behind it, since Parquet carries no keys -- its
    effect falls out of the reduction test instead. Condition 5's second half, that a join between the two
    positions multiplies rows, needs to know whether a build is unique on its key, which is a runtime fact; the
    obvious plan-time proxy is **wrong**, and measurably so, because Q13's join takes 150,000 rows to 1,500,000
    and emits 1,500,000, multiplying nothing at all, while the rewrite is still worth 0.494x there -- what
    shrinks is the build and the final grouping's input, not the join's output. And condition 6 is a property of
    the physical plan, since ADR 0029's filters are attached long after this runs.
  - **An unknown estimate is a refusal, and the direction matters.** This quantity sits where a *small* value
    argues for a rewrite, so an unknown group count must argue against one -- the mirror of the lesson ADR 0022's
    2026-10-22 update records, where an unknown domain divides and so had to stop reading as the largest.
  - **A LEFT JOIN carries no row estimate**: the binder keeps one only for the joins its order search ranked, so
    the gate computes its own by ADR 0022's rule 3, locally. Changing what `EstimateRows` answers for a join
    would move `ChooseBuildSide`, which is ADR 0026's decision and not this item's.
- **G2b, perf(plan): perform the rewrite behind the gate.**
  - Update (2026-10-22, G2b): **shipped for SUM, MIN and MAX, and it fires on no query of the 22** -- because
    the one placement the gate admits is a count, and a count is the one kind the plan cannot yet express.
    Measured where it does fire: a SUM over the same shape as Q13's at SF 100 goes **3.417 s to 3.003 s
    (0.879x)**, answers identical, and the exchange is answer-identical on an inner join, on a LEFT JOIN whose
    preserved side is the grouped one, and with a grouping column drawn from the grouped side.
  - **Why a count cannot be built, which is the finding of this item.** A count decomposes into a count below
    and a SUM of counts above. An integer SUM returns HUGEINT, by the rule that stops it wrapping at int64
    (AGENTS.md, ADR 0021), while COUNT's own output is BIGINT -- so the honest final has a type the query did
    not ask for, and a plan cannot narrow it back, `CastExpr` being an exact widening only. The executor says so
    outright: *no grouped aggregate SUM(BIGINT) -> BIGINT*. Built anyway, it fails Q13 and the TPC-H ratchet
    catches it, which is how this was found rather than reasoned.
  - **Two alternatives were rejected.** A narrowing cast in the plan would be a general capability introduced
    for one caller, and narrowing is exactly where an overflow hides. Registering a `SUM(BIGINT) -> BIGINT`
    accumulator would undo the rule that keeps an integer SUM from wrapping. A sum *of counts* is safe where a
    general one is not, because it is bounded by the rows counted -- so the capability wanted is narrow, and
    belongs in a kind of its own.
  - **The padding of condition 5 is not built either, and needs nothing until a count is exchangeable.** For
    every kind this performs, the value a preserved row with no partial should read is NULL, which is already
    what an aggregate of a NULL partial answers. Only a count wants something else there, so the mapping ships
    with the kind that makes a count exchangeable -- rather than sitting unreachable, which is what it was when
    first written here.
  - **Condition 3's failure mode is harder to reach than feared, and the plan validator is why.** A pre-grouping
    missing a grouping column of the aggregated side leaves the final aggregate's key reading a column the
    partial no longer outputs, and the validator refuses it -- *column is not in its input* -- rather than
    answering. So the condition that changes aggregate values silently is, in this rewrite, caught structurally.
    That is worth knowing and not worth relying on: the `.slt` cases still compare answers against DuckDB,
    including a pre-grouping of two columns and a self-join whose grouped side is the null-supplying one.
  - **One error path changes**, which is new behaviour this ADR should own: the partial range-checks each group
    against its DECIMAL width, so a group whose total fits while its partial sums cancel across the top of that
    width would now raise an overflow where the written plan answered. It needs values near 10^38 and no query
    of the 22 is close, but a rewrite that can turn an answer into an error is worth stating plainly.
- **G2c, feat(plan,exec): a kind that merges partial counts, whose result is BIGINT.** The one capability
  between this decision and its measured 0.494x. It crosses into `exec` and touches ADR 0014's merge-order
  classification and every switch over `AggKind`, which is why it is its own item rather than part of G2b.
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
