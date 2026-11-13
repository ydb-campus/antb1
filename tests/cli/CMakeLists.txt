# CLI golden tests (label cli): the antb1 binary's stdout, stderr and exit code (tests/README.md).
# Update the golden files after an intended change: ANTB1_UPDATE_GOLDENS=1 pixi run test -L cli, then review.
set(_f "${ANTB1_FIXTURES_DIR}")
set(_sql "${CMAKE_CURRENT_SOURCE_DIR}/sql")
set(_t "t=${_f}/hits_like.parquet")
set(_count "SELECT COUNT(*) FROM t")

# ---- output formats ----
antb1_add_cli_golden(
  query_table
  ARGS query --threads 1 -c "${_count}" --table "${_t}"
)
antb1_add_cli_golden(
  query_csv
  ARGS query --threads 1 -c "${_count}" --table "${_t}" --format csv
)
antb1_add_cli_golden(
  query_json
  ARGS query --threads 1 -c "${_count}" --table "${_t}" --format json
)
antb1_add_cli_golden(
  query_empty_table
  ARGS query --threads 1 -c "${_count}" --table "t=${_f}/empty.parquet" --format csv
)

# ---- results of every query shape (our own queries over the fixtures) ----
set(_aggregates "SELECT COUNT(*), COUNT(Title), SUM(UserID), AVG(ResolutionWidth), MIN(EventDate), MAX(URL) FROM t")
antb1_add_cli_golden(
  query_aggregates
  ARGS query --threads 1 --clickbench -c "${_aggregates}" --table "${_t}"
)
antb1_add_cli_golden(
  query_aggregates_json
  ARGS
    query
    --threads
    1
    --clickbench
    -c
    "${_aggregates} WHERE RegionID < 5000 AND IsMobile <> 0"
    --table
    "${_t}"
    --format
    json
)
antb1_add_cli_golden(
  query_aggregates_empty
  ARGS
    query
    --threads
    1
    -c
    "SELECT COUNT(*), COUNT(Title), SUM(RegionID) AS s, AVG(RegionID), MIN(Title), MAX(WatchID) FROM t WHERE RegionID > 70000"
    --table
    "${_t}"
    --format
    csv
)
antb1_add_cli_golden(
  query_aggregates_empty_json
  ARGS
    query
    --threads
    1
    -c
    "SELECT COUNT(*), SUM(RegionID), MIN(Title) FROM t WHERE RegionID = 1.5"
    --table
    "${_t}"
    --format
    json
)
antb1_add_cli_golden(
  query_projection
  ARGS
    query
    --threads
    1
    --clickbench
    -c
    "SELECT WatchID, EventDate, ResolutionWidth AS width, SearchPhrase FROM t WHERE SearchPhrase <> '' AND RegionID < 3000 LIMIT 4"
    --table
    "${_t}"
)
antb1_add_cli_golden(
  query_projection_csv
  ARGS query --threads 1 -c "SELECT id, i64, d, s FROM t WHERE id <= 4" --table "t=${_f}/edge.parquet" --format csv
)
antb1_add_cli_golden(
  query_projection_json
  ARGS
    query
    --threads
    1
    -c
    "SELECT id, i64, d, s, u FROM edge WHERE id >= 5 AND id <> 7 LIMIT 4"
    --table
    "edge=${_f}/edge.parquet"
    --format
    json
)
# Stars among the select items: each expands where it stands, its columns under their declared names,
# so a name repeats (a repeated key in json, as in DuckDB's CLI).
set(_stars "SELECT z.zn_id AS x, *, c.* FROM zones AS z JOIN cities AS c ON z.zn_city = c.ct_id WHERE z.zn_id <= 2")
set(_star_tables --table "zones=${_f}/star/zones.parquet" --table "cities=${_f}/star/cities.parquet")
antb1_add_cli_golden(
  query_star_items
  ARGS query --threads 1 -c "${_stars}" ${_star_tables}
)
antb1_add_cli_golden(
  query_star_items_csv
  ARGS query --threads 1 -c "${_stars}" ${_star_tables} --format csv
)
antb1_add_cli_golden(
  query_star_items_json
  ARGS query --threads 1 -c "${_stars}" ${_star_tables} --format json
)
antb1_add_cli_golden(
  query_decimals_json
  ARGS
    query
    --threads
    1
    -c
    "SELECT id, d9_2, d38_10, d38_0, MAX(p) FROM d WHERE id < 10 GROUP BY id, d9_2, d38_10, d38_0 ORDER BY id LIMIT 4"
    --table
    "d=${_f}/decimals.parquet"
    --format
    json
)
antb1_add_cli_golden(
  query_limit_zero
  ARGS query --threads 1 -c "SELECT Title FROM t LIMIT 0" --table "${_t}" --format csv
)
antb1_add_cli_golden(
  query_edge_aggregates
  ARGS
    query
    --threads
    1
    -c
    "SELECT SUM(i64), SUM(u16), AVG(i64), MAX(d), MIN(s) FROM edge"
    --table
    "edge=${_f}/edge.parquet"
)

# ---- join plans (ADR 0022): the shapes whose order and build sides the statistics decide ----
# One --table per table, never a glob: the star files have different schemas (tests/slt/tables.txt),
# so one glob table would be an I/O error. The plan reads footers only, so --threads 1 shows it all.
# A cycle of four tables: every edge of the relations already joined becomes a key, so the edge that
# closes the cycle is a second key of its join.
antb1_add_cli_golden(
  explain_join_cycle
  ARGS
    explain
    --threads
    1
    -c
    "SELECT COUNT(*), SUM(tr_fare) FROM trips, zones, cities, riders WHERE tr_pickup = zn_id AND zn_city = ct_id AND rd_city = ct_id AND tr_rider = rd_id"
    --table
    "trips=${_f}/star/trips.parquet"
    --table
    "zones=${_f}/star/zones.parquet"
    --table
    "cities=${_f}/star/cities.parquet"
    --table
    "riders=${_f}/star/riders.parquet"
)
# The shortcut zn_city = dv_city is many-to-many, next to the keys tr_pickup = zn_id and
# tr_driver = dv_id: it is no edge to join on while a key edge is available.
antb1_add_cli_golden(
  explain_join_shortcut
  ARGS
    explain
    --threads
    1
    -c
    "SELECT COUNT(*), SUM(tr_fare) FROM trips, zones, drivers WHERE tr_pickup = zn_id AND tr_driver = dv_id AND zn_city = dv_city"
    --table
    "trips=${_f}/star/trips.parquet"
    --table
    "zones=${_f}/star/zones.parquet"
    --table
    "drivers=${_f}/star/drivers.parquet"
)
# --build-side-ratio (ADR 0026): the same query, the same answer, a different build side. The
# equality on `a` leaves the relations joined so far estimated at far fewer rows than the relation
# the last step adds, so building on the smaller side (ratio 1) builds on them instead, while a
# ratio that trusts the estimates only beyond a 400-fold difference keeps the relation it adds.
# Two cases, because one would still pass if the flag were unwired and the default happened to match.
antb1_add_cli_golden(
  explain_build_side_ratio_prefers_the_smaller_side
  ARGS
    explain
    --threads
    1
    --build-side-ratio
    1
    -c
    "SELECT COUNT(*) FROM trips a, trips b, trips c WHERE a.tr_pickup = b.tr_pickup AND b.tr_pickup = c.tr_pickup AND a.tr_row = 7"
    --table
    "trips=${_f}/star/trips.parquet"
)
antb1_add_cli_golden(
  explain_build_side_ratio_keeps_the_added_relation
  ARGS
    explain
    --threads
    1
    --build-side-ratio
    400
    -c
    "SELECT COUNT(*) FROM trips a, trips b, trips c WHERE a.tr_pickup = b.tr_pickup AND b.tr_pickup = c.tr_pickup AND a.tr_row = 7"
    --table
    "trips=${_f}/star/trips.parquet"
)
# --join-order greedy (ADR 0026's C5): ADR 0022's rule instead of the search, on the same query as
# explain_join_cycle above, whose searched plan is bushy. The pair is what shows the flag is wired:
# either alone would pass if it were ignored and the default happened to match.
antb1_add_cli_golden(
  explain_join_order_greedy
  ARGS
    explain
    --threads
    1
    --join-order
    greedy
    -c
    "SELECT COUNT(*), SUM(tr_fare) FROM trips, zones, cities, riders WHERE tr_pickup = zn_id AND zn_city = ct_id AND rd_city = ct_id AND tr_rider = rd_id"
    --table
    "trips=${_f}/star/trips.parquet"
    --table
    "zones=${_f}/star/zones.parquet"
    --table
    "cities=${_f}/star/cities.parquet"
    --table
    "riders=${_f}/star/riders.parquet"
)
# A LEFT JOIN unit that is one relation of an inner join (ADR 0022's join-order rule 3): estimated
# like an inner join of its inputs, never below its left input, so the 240-row unit riders LEFT JOIN
# promos is built and the 3000 trips probe it.
antb1_add_cli_golden(
  explain_join_left_unit_built
  ARGS
    explain
    --threads
    1
    -c
    "SELECT COUNT(*), COUNT(pm_id) FROM trips, riders LEFT JOIN promos ON rd_id = pm_id WHERE tr_rider = rd_id"
    --table
    "trips=${_f}/star/trips.parquet"
    --table
    "riders=${_f}/star/riders.parquet"
    --table
    "promos=${_f}/star/promos.parquet"
)
# The same unit as a derived table: its plan's estimate (EstimateRows) is the unit's now, so the
# derived table of 240 rows is built and the trips probe it, where it used to read as the largest.
antb1_add_cli_golden(
  explain_join_left_unit_derived_built
  ARGS
    explain
    --threads
    1
    -c
    "SELECT COUNT(*) FROM trips, (SELECT rd_id FROM riders LEFT JOIN promos ON rd_id = pm_id) AS r WHERE tr_rider = r.rd_id"
    --table
    "trips=${_f}/star/trips.parquet"
    --table
    "riders=${_f}/star/riders.parquet"
    --table
    "promos=${_f}/star/promos.parquet"
)
# A pair of facts joined many-to-many on a model, closed by a cycle through two roles of the calendar (ADR 0037's
# PF1): the search first reduces bookings through riders and the year of the booking's day, joins the days of that
# week (the week equality, the only key of its join), and builds that side, so that the snapshot fleet streams as the
# probe of the join of the two facts, on two keys: the model, and fleet's day against those days. Joining the facts on
# the model alone would multiply every booking by the model's 312 snapshot rows. The greedy rule, the control, probes
# from fleet and builds the whole of bookings.
antb1_add_cli_golden(
  explain_join_fact_pair
  ARGS
    explain
    --threads
    1
    -c
    "SELECT COUNT(*), COUNT(pm_id) FROM bookings JOIN riders ON bk_rider = rd_id JOIN days AS booked ON bk_day = booked.dy_day JOIN days AS snapped ON booked.dy_week = snapped.dy_week JOIN fleet ON fl_model = bk_model AND fl_day = snapped.dy_day JOIN zones ON fl_zone = zn_id JOIN models ON md_id = bk_model JOIN days AS picked ON bk_pickup = picked.dy_day LEFT JOIN promos ON bk_promo = pm_id WHERE fl_available < bk_vehicles AND picked.dy_day > booked.dy_day AND rd_tier = 'gold' AND booked.dy_year = 2027"
    --table
    "bookings=${_f}/star/bookings.parquet"
    --table
    "riders=${_f}/star/riders.parquet"
    --table
    "days=${_f}/star/days.parquet"
    --table
    "fleet=${_f}/star/fleet.parquet"
    --table
    "zones=${_f}/star/zones.parquet"
    --table
    "models=${_f}/star/models.parquet"
    --table
    "promos=${_f}/star/promos.parquet"
)
antb1_add_cli_golden(
  explain_join_fact_pair_greedy
  ARGS
    explain
    --threads
    1
    --join-order
    greedy
    -c
    "SELECT COUNT(*), COUNT(pm_id) FROM bookings JOIN riders ON bk_rider = rd_id JOIN days AS booked ON bk_day = booked.dy_day JOIN days AS snapped ON booked.dy_week = snapped.dy_week JOIN fleet ON fl_model = bk_model AND fl_day = snapped.dy_day JOIN zones ON fl_zone = zn_id JOIN models ON md_id = bk_model JOIN days AS picked ON bk_pickup = picked.dy_day LEFT JOIN promos ON bk_promo = pm_id WHERE fl_available < bk_vehicles AND picked.dy_day > booked.dy_day AND rd_tier = 'gold' AND booked.dy_year = 2027"
    --table
    "bookings=${_f}/star/bookings.parquet"
    --table
    "riders=${_f}/star/riders.parquet"
    --table
    "days=${_f}/star/days.parquet"
    --table
    "fleet=${_f}/star/fleet.parquet"
    --table
    "zones=${_f}/star/zones.parquet"
    --table
    "models=${_f}/star/models.parquet"
    --table
    "promos=${_f}/star/promos.parquet"
)
# A FROM list that starts with two tables sharing no predicate: no cross product, since the chain
# connects them through the third.
antb1_add_cli_golden(
  explain_join_unconnected_first
  ARGS
    explain
    --threads
    1
    -c
    "SELECT COUNT(*), SUM(tr_fare) FROM zones, riders, trips WHERE tr_pickup = zn_id AND tr_rider = rd_id"
    --table
    "zones=${_f}/star/zones.parquet"
    --table
    "riders=${_f}/star/riders.parquet"
    --table
    "trips=${_f}/star/trips.parquet"
)
# OR factoring (ADR 0022's J3): both branches share the equality and tr_row > 100, so the key joins
# the relations and the shared filter reaches trips' Scan, while what is left of the branches is one
# condition computed on zones' branch. Without the factoring this query has no key at all and exits
# with code 4.
antb1_add_cli_golden(
  explain_or_factoring
  ARGS
    explain
    --threads
    1
    -c
    "SELECT COUNT(*), SUM(tr_fare) FROM trips, zones WHERE tr_pickup = zn_id AND tr_row > 100 AND zn_radius > 1 OR tr_pickup = zn_id AND tr_row > 100 AND zn_city = 2"
    --table
    "trips=${_f}/star/trips.parquet"
    --table
    "zones=${_f}/star/zones.parquet"
)
# explain --physical: the plan the planner would run, which the logical plan cannot show. Over the
# split fixture so the parts are real -- a part pipeline over trips under the join, the build of
# that join beside it, the predicate of each side pushed into its scan, and on the probe's scan the
# key filter ADR 0029 could prune its parts with. Deterministic, because nothing here is measured:
# a run's rows, times and metrics are what --analyze adds to these same lines.
antb1_add_cli_golden(
  explain_physical
  ARGS
    explain
    --physical
    --threads
    1
    -c
    "SELECT COUNT(*), SUM(tr_fare) FROM trips, zones WHERE tr_pickup = zn_id AND tr_row >= 100 AND zn_city = 2"
    --table
    "trips=${_f}/star/trips.parquet"
    --table
    "zones=${_f}/star/zones.parquet"
)
# --physical and --analyze are the same plan, so they cannot both be asked for. The message is
# pinned, not just the exit code: a usage error for another reason would also exit 2.
antb1_add_cli_golden(
  explain_physical_excludes_analyze
  EXIT_CODE 2
  STDERR_REGEX "--physical"
  ARGS explain --physical --analyze -c "SELECT COUNT(*) FROM t" --table "${_t}"
)
# The first branch is only the shared equality, so it makes the OR true wherever the equality holds
# and the equality is the whole condition: nothing of zn_radius reaches the plan, and zones reads
# the key's column alone.
antb1_add_cli_golden(
  explain_or_absorbed
  ARGS
    explain
    --threads
    1
    -c
    "SELECT COUNT(*) FROM trips, zones WHERE tr_pickup = zn_id OR tr_pickup = zn_id AND zn_radius > 1"
    --table
    "trips=${_f}/star/trips.parquet"
    --table
    "zones=${_f}/star/zones.parquet"
)

# ---- sub-plans (ADR 0022's J4): what a derived table and a CTE reference look like ----
# A grouped derived table joined with a table: the sub-plan is a lower part of the probe branch,
# estimated from its input (3000 rows), so it probes and zones builds.
antb1_add_cli_golden(
  explain_derived_table_join
  ARGS
    explain
    --threads
    1
    -c
    "SELECT zn_city, SUM(n) FROM (SELECT tr_pickup, COUNT(*) AS n FROM trips GROUP BY tr_pickup) AS p JOIN zones ON p.tr_pickup = zn_id GROUP BY zn_city"
    --table
    "trips=${_f}/star/trips.parquet"
    --table
    "zones=${_f}/star/zones.parquet"
)
# A LEFT JOIN unit's ON, by its own rules (ADR 0022): the equality between its inputs is its key,
# the conjunct that reads only the right input is pushed into that input (rule 11), and the one that
# reads only the preserved side is a residual of the join, since a left row that fails it is padded
# and not dropped. The build side is the right input, which the executor requires.
antb1_add_cli_golden(
  explain_left_join_fixed_unit
  ARGS
    explain
    --threads
    1
    -c
    "SELECT COUNT(*), COUNT(zn_id) FROM trips LEFT JOIN zones ON tr_pickup = zn_id AND zn_radius > 2 AND tr_fare > 20"
    --table
    "trips=${_f}/star/trips.parquet"
    --table
    "zones=${_f}/star/zones.parquet"
)
# A WHERE conjunct that reads the null-supplying side stays above the unit, where it drops the
# padded rows: it never reaches the right input, and the join stays a LEFT JOIN, since there is no
# LEFT-to-INNER conversion under a NULL-rejecting WHERE (ADR 0022).
antb1_add_cli_golden(
  explain_left_join_where_split
  ARGS
    explain
    --threads
    1
    -c
    "SELECT COUNT(*) FROM trips LEFT JOIN zones ON tr_pickup = zn_id WHERE zn_radius <= 2"
    --table
    "trips=${_f}/star/trips.parquet"
    --table
    "zones=${_f}/star/zones.parquet"
)
# A CTE referenced twice is bound twice and scanned twice (ADR 0022), so the plan holds the subtree
# twice rather than one shared node -- and says so: the two members carry the `shared` note
# docs/adr/0030-shared-sub-plans.md's E1 adds. Nothing is shared yet; the note is what makes the
# duplication visible to a reader instead of leaving it to be counted by hand.
antb1_add_cli_golden(
  explain_cte_referenced_twice
  ARGS
    explain
    --threads
    1
    -c
    "WITH per_zone AS (SELECT tr_pickup AS zone, COUNT(*) AS n FROM trips GROUP BY tr_pickup) SELECT COUNT(*) FROM per_zone AS a, per_zone AS b WHERE a.zone = b.zone"
    --table
    "trips=${_f}/star/trips.parquet"
)
# A sub-plan that ends in a sink: the inner ORDER BY with LIMIT is a top-N below the outer
# aggregate, which drains it.
antb1_add_cli_golden(
  explain_derived_table_top_n
  ARGS
    explain
    --threads
    1
    -c
    "SELECT COUNT(*), SUM(tr_fare) FROM (SELECT tr_row, tr_fare FROM trips ORDER BY tr_fare DESC NULLS LAST, tr_row LIMIT 10) AS top"
    --table
    "trips=${_f}/star/trips.parquet"
)
# An IN subquery as a semi join above the block's inner joins (ADR 0022's J5): the sub-plan is the
# build side, since the join does not keep its rows, and the probe's cast to the key type is a
# Compute of its own directly below the join -- below the select list's, so a select-list expression
# that can overflow is computed only for the rows the join keeps (divergence D16). Two conjuncts
# stack in the order they are written, the second a null-aware anti join.
antb1_add_cli_golden(
  explain_in_subquery_semi_join
  ARGS
    explain
    --threads
    1
    -c
    "SELECT COUNT(*) FROM trips JOIN zones ON tr_pickup = zn_id WHERE tr_driver IN (SELECT dv_id FROM drivers) AND tr_rider NOT IN (SELECT rd_id FROM riders)"
    --table
    "trips=${_f}/star/trips.parquet"
    --table
    "zones=${_f}/star/zones.parquet"
    --table
    "drivers=${_f}/star/drivers.parquet"
    --table
    "riders=${_f}/star/riders.parquet"
)
# The same form in HAVING: its join sits above the aggregation and the filter of HAVING's other
# conjuncts, below the select list's computed columns, over the groups rather than over the rows.
antb1_add_cli_golden(
  explain_in_subquery_having
  ARGS
    explain
    --threads
    1
    -c
    "SELECT tr_pickup, COUNT(*) FROM trips GROUP BY tr_pickup HAVING tr_pickup NOT IN (SELECT zn_id FROM zones)"
    --table
    "trips=${_f}/star/trips.parquet"
    --table
    "zones=${_f}/star/zones.parquet"
)
# An IN under OR is a null-aware mark join (ADR 0041's SQ7a): every probe row once, with a BOOLEAN
# mark that the OR's filter reads above it; its probe's cast to the key type is a Compute of its own
# directly below the join, as a semi join's is.
antb1_add_cli_golden(
  explain_in_subquery_mark
  ARGS
    explain
    --threads
    1
    -c
    "SELECT COUNT(*) FROM trips WHERE tr_row < 10 OR tr_driver NOT IN (SELECT dv_id FROM drivers WHERE dv_id > 5)"
    --table
    "trips=${_f}/star/trips.parquet"
    --table
    "drivers=${_f}/star/drivers.parquet"
)
# An IN that every branch of an OR shares is factored out first and is a semi join, with no mark.
antb1_add_cli_golden(
  explain_in_subquery_or_factored
  ARGS
    explain
    --threads
    1
    -c
    "SELECT COUNT(*) FROM trips WHERE (tr_row < 10 AND tr_pickup IN (SELECT zn_id FROM zones WHERE zn_city = 1)) OR (tr_row > 2990 AND tr_pickup IN (SELECT zn_id FROM zones WHERE zn_city = 1))"
    --table
    "trips=${_f}/star/trips.parquet"
    --table
    "zones=${_f}/star/zones.parquet"
)
# NOTs over an IN conjunct fold into its kind (ADR 0041's SQ7b): NOT (x NOT IN q) is a semi join.
antb1_add_cli_golden(
  explain_not_not_in_semi_join
  ARGS
    explain
    --threads
    1
    -c
    "SELECT COUNT(*) FROM trips WHERE NOT (tr_driver NOT IN (SELECT dv_id FROM drivers WHERE dv_id > 5))"
    --table
    "trips=${_f}/star/trips.parquet"
    --table
    "drivers=${_f}/star/drivers.parquet"
)
# An IN over a set operation: its set is the operation's node, the probe widened to its type.
antb1_add_cli_golden(
  explain_in_subquery_over_union
  ARGS
    explain
    --threads
    1
    -c
    "SELECT COUNT(*) FROM trips WHERE tr_pickup = ANY (SELECT zn_id FROM zones WHERE zn_city = 1 UNION SELECT ct_id FROM cities)"
    --table
    "trips=${_f}/star/trips.parquet"
    --table
    "zones=${_f}/star/zones.parquet"
    --table
    "cities=${_f}/star/cities.parquet"
)
# A mark in HAVING stands over the groups, above the aggregation; one in an aggregate's argument
# stands below it, over the rows, above WHERE's filter.
antb1_add_cli_golden(
  explain_in_subquery_mark_having
  ARGS
    explain
    --threads
    1
    -c
    "SELECT tr_pickup, SUM(CASE WHEN tr_rider IN (SELECT rd_id FROM riders WHERE rd_id < 50) THEN 1 ELSE 0 END) FROM trips WHERE tr_row > 100 GROUP BY tr_pickup HAVING COUNT(*) > 60 OR tr_pickup IN (SELECT zn_id FROM zones WHERE zn_city = 1)"
    --table
    "trips=${_f}/star/trips.parquet"
    --table
    "zones=${_f}/star/zones.parquet"
    --table
    "riders=${_f}/star/riders.parquet"
)
# HAVING computes what its conditions read and filters directly above the aggregation, before the
# select list and ORDER BY are computed, as DuckDB does: their expressions run only over the groups
# that pass.
antb1_add_cli_golden(
  explain_having_before_select_list
  ARGS
    explain
    --threads
    1
    -c
    "SELECT tr_pickup, SUM(tr_distance) * 2 AS d FROM trips GROUP BY tr_pickup HAVING COUNT(*) > 1 AND MAX(tr_row) - MIN(tr_row) > 10 ORDER BY d DESC, tr_pickup"
    --table
    "trips=${_f}/star/trips.parquet"
)
# A scalar subquery as a keyless one-row join (ADR 0022's J5): its build is the aggregate's single
# row, which the join appends to every probe row. The conjunct is a filter of the block's slot
# (ADR 0041's decision 9): what it computes, the outer side, stands above the join, and its
# comparison above that.
antb1_add_cli_golden(
  explain_scalar_subquery_one_row_join
  ARGS
    explain
    --threads
    1
    -c
    "SELECT COUNT(*) FROM trips WHERE tr_fare * 2 > (SELECT AVG(tr_fare) FROM trips)"
    --table
    "trips=${_f}/star/trips.parquet"
)
# In HAVING over a global aggregate whose select list is exactly its aggregates: the join's appended
# column stands above the aggregation, so a Project restores the list.
antb1_add_cli_golden(
  explain_scalar_subquery_having_global
  ARGS
    explain
    --threads
    1
    -c
    "SELECT MIN(tr_driver) FROM trips HAVING MIN(tr_driver) = (SELECT MIN(dv_id) FROM drivers)"
    --table
    "trips=${_f}/star/trips.parquet"
    --table
    "drivers=${_f}/star/drivers.parquet"
)
# A scalar subquery of any other shape (ADR 0041's SQ4a): its single value, an aggregate over the
# subquery's plan under the same one-row join -- NULL over no row, the row's over one, and over two
# or more DuckDB's error, which the query below reports at exit code 1.
antb1_add_cli_golden(
  explain_scalar_subquery_single_value
  ARGS
    explain
    --threads
    1
    -c
    "SELECT COUNT(*) FROM trips WHERE tr_pickup = (SELECT zn_id FROM zones ORDER BY zn_radius DESC, zn_id LIMIT 1)"
    --table
    "trips=${_f}/star/trips.parquet"
    --table
    "zones=${_f}/star/zones.parquet"
)
antb1_add_cli_golden(
  error_scalar_subquery_rows
  EXIT_CODE 1
  STDERR
  ARGS
    query
    --threads
    1
    -c
    "SELECT COUNT(*) FROM trips WHERE tr_pickup = (SELECT zn_id FROM zones)"
    --table
    "trips=${_f}/star/trips.parquet"
    --table
    "zones=${_f}/star/zones.parquet"
)
# An uncorrelated EXISTS: COUNT(*) over LIMIT 1 of its block, its select list dropped, under the
# one-row join, compared with 1 (NOT EXISTS: 0); in HAVING above the aggregation.
antb1_add_cli_golden(
  explain_exists_uncorrelated_having
  ARGS
    explain
    --threads
    1
    -c
    "SELECT zn_city, COUNT(*) FROM zones GROUP BY zn_city HAVING NOT EXISTS (SELECT tr_rider FROM trips WHERE tr_fare > 1000)"
    --table
    "trips=${_f}/star/trips.parquet"
    --table
    "zones=${_f}/star/zones.parquet"
)
# A BETWEEN whose bound is a subquery: its two comparisons, the subquery's above its one-row join
# and the other one pushed into the scan.
antb1_add_cli_golden(
  explain_between_subquery_bound
  ARGS
    explain
    --threads
    1
    -c
    "SELECT COUNT(*) FROM trips WHERE tr_pickup BETWEEN 3 AND (SELECT zn_id FROM zones ORDER BY zn_id DESC LIMIT 1)"
    --table
    "trips=${_f}/star/trips.parquet"
    --table
    "zones=${_f}/star/zones.parquet"
)
# Values inside conditions (ADR 0041's SQ1): each value's one-row join stands in the block's slot,
# in the order they are bound, and the conjunct that holds it is a filter after them -- what it
# computes, then its predicate (decision 9). A value under OR with arithmetic next to an EXISTS under
# NOT, whose count is a value too.
antb1_add_cli_golden(
  explain_value_conditions
  ARGS
    explain
    --threads
    1
    -c
    "SELECT COUNT(*) FROM trips WHERE tr_fare * 2 > (SELECT AVG(tr_fare) FROM trips) + 1 OR NOT EXISTS (SELECT 1 FROM promos)"
    --table
    "trips=${_f}/star/trips.parquet"
    --table
    "promos=${_f}/star/promos.parquet"
)
# A subquery written twice in one slot is one value, one join (decision 10): here both bounds of a
# BETWEEN, the second with a constant added.
antb1_add_cli_golden(
  explain_value_bound_once
  ARGS
    explain
    --threads
    1
    -c
    "SELECT COUNT(*) FROM trips WHERE tr_pickup BETWEEN (SELECT MIN(zn_id) FROM zones) AND (SELECT MIN(zn_id) FROM zones) + 10"
    --table
    "trips=${_f}/star/trips.parquet"
    --table
    "zones=${_f}/star/zones.parquet"
)
# An inner ON inside a FULL JOIN's left input has that input's own slot, below the FULL JOIN, which
# then pads what its filter keeps.
antb1_add_cli_golden(
  explain_value_full_join_left_input
  ARGS
    explain
    --threads
    1
    -c
    "SELECT COUNT(*), COUNT(t.tr_row), COUNT(d.dv_id) FROM trips t JOIN zones z ON t.tr_pickup = z.zn_id AND z.zn_city < (SELECT MIN(ct_id) + 2 FROM cities) FULL JOIN drivers d ON t.tr_driver = d.dv_id"
    --table
    "trips=${_f}/star/trips.parquet"
    --table
    "zones=${_f}/star/zones.parquet"
    --table
    "cities=${_f}/star/cities.parquet"
    --table
    "drivers=${_f}/star/drivers.parquet"
)
# A HAVING conjunct with a value stands in HAVING's slot (ADR 0041's SQ1b): above the aggregation and
# the filter of HAVING's other conjuncts, its value's join and then what it computes and its filter,
# below the select list's computed column.
antb1_add_cli_golden(
  explain_value_having_slot
  ARGS
    explain
    --threads
    1
    -c
    "SELECT tr_pickup, SUM(tr_fare) * 2 AS s FROM trips GROUP BY tr_pickup HAVING COUNT(*) > 50 AND SUM(tr_fare) > (SELECT AVG(tr_fare) FROM trips) * 70 ORDER BY tr_pickup"
    --table
    "trips=${_f}/star/trips.parquet"
)

# Values in a block that does not aggregate (ADR 0041's SQ2a) join in block 0's per-row slot, below
# the computed select items, the sort and the limit: one join per distinct subquery, the item's and
# the ORDER BY's.
antb1_add_cli_golden(
  explain_select_value
  ARGS
    explain
    --threads
    1
    -c
    "SELECT tr_row, tr_pickup * (SELECT MIN(zn_id) FROM zones) AS v, CASE WHEN EXISTS (SELECT * FROM zones WHERE zn_id > 39) THEN 1 ELSE 0 END AS e FROM trips WHERE tr_row < 5 ORDER BY (SELECT MAX(zn_id) FROM zones) - tr_row, tr_row LIMIT 3"
    --table
    "trips=${_f}/star/trips.parquet"
    --table
    "zones=${_f}/star/zones.parquet"
)
# An EXISTS block's select list is dropped: a value there that no clause reads plants nothing.
antb1_add_cli_golden(
  explain_exists_select_list_value
  ARGS
    explain
    --threads
    1
    -c
    "SELECT COUNT(*) FROM trips WHERE EXISTS (SELECT (SELECT zn_id FROM zones) FROM zones z WHERE z.zn_id = trips.tr_pickup)"
    --table
    "trips=${_f}/star/trips.parquet"
    --table
    "zones=${_f}/star/zones.parquet"
)
# ADR 0041's SQ0 keeps the unnesting's plans byte for byte; these pin what only its EXPLAIN shows.
# A scalar subquery's key is named by the last correlated conjunct that names its column.
antb1_add_cli_golden(
  explain_unnest_key_named_by_its_last_spelling
  ARGS
    explain
    --threads
    1
    -c
    "SELECT zn_id FROM zones z WHERE z.zn_parent > (SELECT MAX(c.ct_pop) FROM cities c WHERE c.ct_id = z.zn_city AND z.zn_city = ct_id) ORDER BY zn_id"
    --table
    "zones=${_f}/star/zones.parquet"
    --table
    "cities=${_f}/star/cities.parquet"
)
# Its keys, and the group's, follow the outer columns' order, not the conjuncts'.
antb1_add_cli_golden(
  explain_unnest_keys_in_outer_column_order
  ARGS
    explain
    --threads
    1
    -c
    "SELECT zn_id FROM zones z WHERE z.zn_id > (SELECT MAX(c.ct_pop) FROM cities c WHERE c.ct_id = z.zn_city AND c.ct_pop = z.zn_parent AND c.ct_row = z.zn_row) ORDER BY zn_id"
    --table
    "zones=${_f}/star/zones.parquet"
    --table
    "cities=${_f}/star/cities.parquet"
)
# An EXISTS's keys of two types: each side cast to the key type, the outer casts first.
antb1_add_cli_golden(
  explain_unnest_key_casts
  ARGS
    explain
    --threads
    1
    -c
    "SELECT zn_id FROM zones z WHERE EXISTS (SELECT 1 FROM riders r WHERE r.rd_city = z.zn_id AND r.rd_id = z.zn_city) ORDER BY zn_id"
    --table
    "zones=${_f}/star/zones.parquet"
    --table
    "riders=${_f}/star/riders.parquet"
)
# A nested EXISTS without a key is refused at its own span, not at the outer one's.
antb1_add_cli_golden(
  unsupported_unnest_nested_refusal
  EXIT_CODE 4
  STDERR
  ARGS
    query
    --threads
    1
    -c
    "SELECT zn_id FROM zones z WHERE EXISTS (SELECT 1 FROM cities c WHERE c.ct_id = z.zn_city AND EXISTS (SELECT 1 FROM riders r WHERE r.rd_city < c.ct_id)) ORDER BY zn_id"
    --table
    "zones=${_f}/star/zones.parquet"
    --table
    "cities=${_f}/star/cities.parquet"
    --table
    "riders=${_f}/star/riders.parquet"
)
# The outer subquery is unnested first: when both are refused, the outer one's refusal is reported.
antb1_add_cli_golden(
  unsupported_unnest_nested_outer_first
  EXIT_CODE 4
  STDERR
  ARGS
    query
    --threads
    1
    -c
    "SELECT zn_id FROM zones z WHERE EXISTS (SELECT 1 FROM cities c WHERE c.ct_id < z.zn_city AND EXISTS (SELECT 1 FROM riders r WHERE r.rd_city < c.ct_id)) ORDER BY zn_id"
    --table
    "zones=${_f}/star/zones.parquet"
    --table
    "cities=${_f}/star/cities.parquet"
    --table
    "riders=${_f}/star/riders.parquet"
)
# Of two stacked scalar subqueries that are refused, the one written last is met first.
antb1_add_cli_golden(
  unsupported_unnest_stacked_refusal
  EXIT_CODE 4
  STDERR
  ARGS
    query
    --threads
    1
    -c
    "SELECT zn_id FROM zones z WHERE z.zn_parent > (SELECT MIN(c.ct_pop) FROM cities c WHERE c.ct_id < z.zn_city) AND z.zn_row > (SELECT MAX(c.ct_pop) FROM cities c WHERE c.ct_pop < z.zn_parent) ORDER BY zn_id"
    --table
    "zones=${_f}/star/zones.parquet"
    --table
    "cities=${_f}/star/cities.parquet"
)
# A correlated COUNT whose comparison passes its 0 is joined back LEFT (ADR 0041's SQ6): the join builds the grouped
# riders, padded for the cities without one, the count is read above it through COALESCE(count, 0), and the semi-join
# reduction restricts the one key over a bare scan, which D5 does not reach under a LEFT join.
antb1_add_cli_golden(
  explain_unnest_left_join_back
  ARGS
    explain
    --threads
    1
    -c
    "SELECT c.ct_id FROM cities c WHERE (SELECT COUNT(*) FROM riders r WHERE r.rd_city = c.ct_id) = 0 ORDER BY 1"
    --table
    "cities=${_f}/star/cities.parquet"
    --table
    "riders=${_f}/star/riders.parquet"
)
# A constant select item over ORDER BY COUNT(*) is the constant over no rows too, so it moves above the LEFT join back
# and a padded city reads it (ADR 0041's SQ6).
antb1_add_cli_golden(
  explain_unnest_left_item_moved
  ARGS
    explain
    --threads
    1
    -c
    "SELECT c.ct_id FROM cities c WHERE (SELECT 42 FROM riders r WHERE r.rd_city = c.ct_id ORDER BY COUNT(*)) = 42 ORDER BY 1"
    --table
    "cities=${_f}/star/cities.parquet"
    --table
    "riders=${_f}/star/riders.parquet"
)
# A HAVING inside a correlated scalar subquery is a CASE over its value, never a filter (ADR 0041's SQ6): a city whose
# group it rejects reads NULL. COUNT(*) < 78 keeps the empty row's 0, which `= 0` passes, so the join back is LEFT
# and the condition and the CASE stand above it.
antb1_add_cli_golden(
  explain_unnest_having_as_case
  ARGS
    explain
    --threads
    1
    -c
    "SELECT c.ct_id FROM cities c WHERE (SELECT COUNT(*) FROM riders r WHERE r.rd_city = c.ct_id HAVING COUNT(*) < 78) = 0 ORDER BY 1"
    --table
    "cities=${_f}/star/cities.parquet"
    --table
    "riders=${_f}/star/riders.parquet"
)
# Two stacked LEFT join backs, both restricted (ADR 0041's SQ6): each semi-join reduction clones the base below the
# stack, the scan of cities, so the outer rewrite no longer declines for the dependent join below it, and no rewrite
# is cloned.
antb1_add_cli_golden(
  explain_unnest_stacked_left_restricted
  ARGS
    explain
    --threads
    1
    -c
    "SELECT c.ct_id FROM cities c WHERE (SELECT COUNT(*) FROM riders r WHERE r.rd_city = c.ct_id) = 0 AND (SELECT COUNT(*) FROM drivers d WHERE d.dv_city = c.ct_id) = 0 ORDER BY 1"
    --table
    "cities=${_f}/star/cities.parquet"
    --table
    "riders=${_f}/star/riders.parquet"
    --table
    "drivers=${_f}/star/drivers.parquet"
)
# An unaliased select item holding a value is named by antb1's own text of the item (divergence D8).
antb1_add_cli_golden(
  query_select_value_names
  ARGS
    query
    --threads
    1
    -c
    "SELECT ct_id, (SELECT MIN(zn_id) FROM zones), ct_id + (SELECT COUNT(*) FROM cities) FROM cities WHERE ct_id < 3 ORDER BY ct_id"
    --table
    "cities=${_f}/star/cities.parquet"
    --table
    "zones=${_f}/star/zones.parquet"
)

# In a block that aggregates (ADR 0041's SQ2b) a value in an aggregate's argument joins per row,
# below the aggregation, and one over the aggregates joins after it, below the computed item.
antb1_add_cli_golden(
  explain_select_value_over_aggregate
  ARGS
    explain
    --threads
    1
    -c
    "SELECT zn_city, SUM(zn_id * (SELECT MIN(ct_id) FROM cities)) AS s, COUNT(*) + (SELECT MAX(ct_id) FROM cities) AS c FROM zones GROUP BY zn_city ORDER BY zn_city"
    --table
    "zones=${_f}/star/zones.parquet"
    --table
    "cities=${_f}/star/cities.parquet"
)
# A GROUP BY key that holds a value and the select item written alike read one join: the item is
# the key.
antb1_add_cli_golden(
  explain_group_by_value_key
  ARGS
    explain
    --threads
    1
    -c
    "SELECT ct_id % 2 + (SELECT MIN(zn_id) FROM zones) AS k, COUNT(*) FROM cities GROUP BY ct_id % 2 + (SELECT MIN(zn_id) FROM zones) ORDER BY k"
    --table
    "cities=${_f}/star/cities.parquet"
    --table
    "zones=${_f}/star/zones.parquet"
)

# ---- bench: ClickBench-format JSON (docs/benchmarks.md); timings vary, so regexes ----
# CMake regex: [.] [{] [}] [[] []] match the character itself; \n is a newline. Built with string(APPEND):
# a CMake list would split at the ; separators inside [ ].
set(_num "[0-9]+[.][0-9]+")
set(_bench_json "^[{]\n  \"system\": \"antb1\",\n  \"date\": \"[0-9]+-[0-9][0-9]-[0-9][0-9]\",\n")
string(APPEND _bench_json "  \"machine\": \"[^\"]+\",\n  \"cluster_size\": 1,\n  \"proprietary\": \"no\",\n")
string(APPEND _bench_json "  \"hardware\": \"cpu\",\n  \"tuned\": \"no\",\n")
string(APPEND _bench_json "  \"tags\": [[]\"C[+][+]\", \"column-oriented\", \"embedded\", \"stateless\"[]],\n")
string(APPEND _bench_json "  \"load_time\": ${_num},\n  \"data_size\": [1-9][0-9]*,\n")
string(APPEND _bench_json "  \"concurrent_qps\": null,\n  \"concurrent_error_ratio\": null,\n  \"result\": [[]\n")
string(APPEND _bench_json "    [[]${_num}, ${_num}[]],\n    [[]${_num}, ${_num}[]],\n")
string(APPEND _bench_json "    [[]null, null[]],\n    [[]null, null[]]\n  []],\n  \"antb1\": [{]\n")
string(APPEND _bench_json ".*    \"cache\": \"lukewarm\",\n    \"tries\": 2,\n    \"batch_size\": 65536,\n")
string(APPEND _bench_json "    \"threads\": 1,\n    \"memory_limit\": [1-9][0-9]*,\n")
string(APPEND _bench_json "    \"failed\": [[][{]\"query\": 2, \"kind\": \"unsupported\"[}], ")
string(APPEND _bench_json "[{]\"query\": 3, \"kind\": \"bind\"[}][]]\n  [}]\n[}]$")
set(_bench_log "^Q0: ${_num} ${_num}\nQ1: ${_num} ${_num}\nQ2: unsupported error\nQ3: bind error\n")
string(APPEND _bench_log "bench: 4 queries, 2 answered, 2 failed[;] JSON on stdout [(]lukewarm[)]$")
antb1_add_cli_golden(
  bench
  STDOUT_REGEX "${_bench_json}"
  STDERR_REGEX "${_bench_log}"
  ARGS bench --threads 1 --clickbench --queries "${_sql}/bench.sql" --table "${_t}" --tries 2 --out -
)
antb1_add_cli_golden(
  bench_missing_queries
  EXIT_CODE 3
  STDERR_REGEX "^antb1: io error: cannot read the query file '[$][{]FIXTURES[}]/no_such[.]sql'$"
  ARGS bench --threads 1 --queries "${_f}/no_such.sql" --table "${_t}" --out -
)
antb1_add_cli_golden(
  usage_bench_tries
  EXIT_CODE 2
  STDERR_REGEX "--tries"
  ARGS bench --threads 1 --queries "${_sql}/bench.sql" --table "${_t}" --tries 0 --out -
)

# ---- SQL sources and table forms ----
antb1_add_cli_golden(
  query_file
  ARGS query --threads 1 -f "${_sql}/count_star.sql" --table "${_t}" --format csv
)
antb1_add_cli_golden(
  query_stdin
  INPUT "${_sql}/count_star.sql"
  ARGS query --threads 1 -c - --table "${_t}" --format csv
)
antb1_add_cli_golden(
  query_table_glob
  ARGS query --threads 1 -c "${_count}" --table "t=${_f}/hits_like_split/part-*.parquet" --format csv
)
antb1_add_cli_golden(
  query_table_list
  ARGS
    query
    --threads
    1
    -c
    "${_count}"
    --table
    "t=${_f}/hits_like_split/part-0.parquet,${_f}/hits_like_split/part-3.parquet"
    --format
    csv
)
antb1_add_cli_golden(
  query_from_path
  ARGS query --threads 1 -c "SELECT COUNT(*) FROM '${_f}/hits_like_split/part-[12].parquet'" --format csv
)
antb1_add_cli_golden(
  query_timing
  ARGS query --threads 1 -c "${_count}" --table "${_t}" --format csv --timing
  STDERR_REGEX "^[0-9]+[.][0-9][0-9][0-9][0-9][0-9][0-9]$"
)

# ---- errors and exit codes: 1 query error, 2 usage, 3 I/O, 4 unsupported ----
antb1_add_cli_golden(
  error_parse
  EXIT_CODE 1
  STDERR
  ARGS query --threads 1 -c "SELECT COUNT(*) FROM" --table "${_t}"
)
antb1_add_cli_golden(
  error_bind
  EXIT_CODE 1
  STDERR
  ARGS query --threads 1 -c "SELECT COUNT(*) FROM nope" --table "${_t}"
)
antb1_add_cli_golden(
  error_bind_json
  EXIT_CODE 1
  STDERR
  ARGS query --threads 1 -c "SELECT COUNT(*)\nFROM nope" --table "${_t}" --format json
)
# Table u is not registered: unsupported SQL exits 4 before any name is resolved.
antb1_add_cli_golden(
  unsupported
  EXIT_CODE 4
  STDERR
  ARGS query --threads 1 -c "SELECT CounterID, row_number() OVER (ORDER BY CounterID) FROM u" --table "${_t}"
)
antb1_add_cli_golden(
  unsupported_json
  EXIT_CODE 4
  STDERR
  ARGS query --threads 1 -c "SELECT SUM(AdvEngineID) OVER () FROM u" --table "${_t}" --format json
)
antb1_add_cli_golden(
  unsupported_explain
  EXIT_CODE 4
  STDERR
  ARGS explain -c "SELECT DISTINCT ON (WatchID) WatchID FROM t LIMIT 10" --table "${_t}"
)
# Nested parentheses, each closed by a chain of 120 operators: a tree thousands of levels deep that the depth
# limit once let through (it overflowed the stack); now the depth error at the operator that is too deep.
antb1_add_cli_golden(
  unsupported_deep_operator_chains
  EXIT_CODE 4
  STDERR
  ARGS explain -f "${_sql}/deep_operator_chains.sql" --table "${_t}"
)
antb1_add_cli_golden(
  unsupported_where
  EXIT_CODE 4
  STDERR
  ARGS query --threads 1 -c "SELECT COUNT(*) FROM t\nWHERE IsMobile = 1 OR IsRefresh IS TRUE" --table "${_t}"
)
# A UNION ALL's own ORDER BY, LIMIT and OFFSET exit 4 at their first clause until ADR 0036's X5;
# values with no exact common type exit 4 at the first item that does not widen; a column count
# that differs is DuckDB's bind error, at the UNION ALL before the block.
antb1_add_cli_golden(
  unsupported_union_all_order_by
  EXIT_CODE 4
  STDERR
  ARGS query --threads 1 -c "SELECT WatchID FROM t\nUNION ALL SELECT WatchID FROM t\nORDER BY 1" --table "${_t}"
)
antb1_add_cli_golden(
  unsupported_union_all_types
  EXIT_CODE 4
  STDERR
  ARGS query --threads 1 -c "SELECT WatchID FROM t\nUNION ALL SELECT Title FROM t" --table "${_t}"
)
antb1_add_cli_golden(
  error_bind_union_all_columns
  EXIT_CODE 1
  STDERR
  ARGS query --threads 1 -c "SELECT WatchID FROM t\nUNION ALL SELECT WatchID, Title FROM t" --table "${_t}"
)
# An alias after a star exits 4 (DuckDB gives it to every column the star expands to); a qualified
# star names one FROM item of its own block, else DuckDB's bind error.
antb1_add_cli_golden(
  unsupported_star_alias
  EXIT_CODE 4
  STDERR
  ARGS query --threads 1 -c "SELECT *, WatchID AS w, * AS x FROM t" --table "${_t}"
)
antb1_add_cli_golden(
  error_bind_qualified_star
  EXIT_CODE 1
  STDERR
  ARGS query --threads 1 -c "SELECT WatchID, x.* FROM t" --table "${_t}"
)
antb1_add_cli_golden(
  error_bind_column
  EXIT_CODE 1
  STDERR
  ARGS query --threads 1 -c "SELECT COUNT(*) FROM t WHERE nope = 1" --table "${_t}"
)
antb1_add_cli_golden(
  error_bind_aggregate
  EXIT_CODE 1
  STDERR
  ARGS explain -c "SELECT SUM(Title) FROM t" --table "${_t}"
)
antb1_add_cli_golden(
  error_bind_literal_json
  EXIT_CODE 1
  STDERR
  ARGS query --threads 1 -c "SELECT COUNT(*) FROM t WHERE CounterID = 'x'" --table "${_t}" --format json
)
antb1_add_cli_golden(usage_no_command EXIT_CODE 2 STDERR_REGEX "subcommand")
antb1_add_cli_golden(
  usage_memory_limit
  EXIT_CODE 2
  STDERR_REGEX "--memory-limit"
  ARGS query --threads 1 --memory-limit 12XB -c "${_count}" --table "${_t}"
)
antb1_add_cli_golden(
  error_memory_limit
  EXIT_CODE 1
  STDERR_REGEX "^antb1: memory error: the query needs more than the memory limit of 1[.]00 KB [(]--memory-limit[)]$"
  ARGS query --threads 1 --memory-limit 1KB -c "SELECT URL, COUNT(*) FROM t GROUP BY URL" --table "${_t}"
)
antb1_add_cli_golden(
  usage_threads
  EXIT_CODE 2
  STDERR_REGEX "--threads"
  ARGS query --threads 0 -c "${_count}" --table "${_t}"
)
antb1_add_cli_golden(
  usage_bad_format
  EXIT_CODE 2
  STDERR_REGEX "--format"
  ARGS query --threads 1 -c "${_count}" --format xml
)
antb1_add_cli_golden(
  usage_no_sql
  EXIT_CODE 2
  STDERR_REGEX "pass exactly one of"
  ARGS query --threads 1 --table "${_t}"
)
antb1_add_cli_golden(
  usage_two_sql_sources
  EXIT_CODE 2
  STDERR_REGEX "pass exactly one of"
  ARGS query --threads 1 -c "${_count}" -f "${_sql}/count_star.sql" --table "${_t}"
)
antb1_add_cli_golden(
  usage_bad_table_spec
  EXIT_CODE 2
  STDERR_REGEX "--table expects NAME=PATH"
  ARGS query --threads 1 -c "${_count}" --table t
)
antb1_add_cli_golden(
  usage_bad_column_type
  EXIT_CODE 2
  STDERR_REGEX "--column-type expects COLUMN=DATE"
  ARGS schema --column-type EventDate=INT --table "${_t}"
)
antb1_add_cli_golden(
  io_missing_table_file
  EXIT_CODE 3
  STDERR_REGEX "^antb1: io error: .*[$][{]FIXTURES[}]/no_such[.]parquet"
  ARGS query --threads 1 -c "${_count}" --table "t=${_f}/no_such.parquet"
)
antb1_add_cli_golden(
  io_missing_table_file_json
  EXIT_CODE 3
  STDERR_REGEX "^[{]\"error\":[{]\"kind\":\"io\",\"message\":\".*no_such[.]parquet.*\"[}][}]$"
  ARGS query --threads 1 -c "${_count}" --table "t=${_f}/no_such.parquet" --format json
)
antb1_add_cli_golden(
  io_missing_from_path
  EXIT_CODE 3
  STDERR_REGEX "^antb1: io error: .*[$][{]FIXTURES[}]/no_such[.]parquet"
  ARGS query --threads 1 -c "SELECT COUNT(*) FROM '${_f}/no_such.parquet'"
)
antb1_add_cli_golden(
  io_not_parquet
  EXIT_CODE 3
  STDERR_REGEX "^antb1: io error: cannot read Parquet file '[$][{]SOURCE[}]/tests/cli/sql/count_star[.]sql'"
  ARGS query --threads 1 -c "${_count}" --table "t=${_sql}/count_star.sql"
)
antb1_add_cli_golden(
  io_no_glob_match
  EXIT_CODE 3
  STDERR
  ARGS query --threads 1 -c "${_count}" --table "t=${_f}/hits_like_split/none-*.parquet"
)
antb1_add_cli_golden(
  io_schema_mismatch
  EXIT_CODE 3
  STDERR
  ARGS query --threads 1 -c "${_count}" --table "t=${_f}/hits_like.parquet,${_f}/edge.parquet"
)
antb1_add_cli_golden(
  io_missing_sql_file
  EXIT_CODE 3
  STDERR
  ARGS query --threads 1 -f "${_f}/no_such.sql" --table "${_t}"
)

# ---- schema, explain, version ----
antb1_add_cli_golden(schema_clickbench ARGS schema --clickbench --table "${_t}")
antb1_add_cli_golden(schema_edge ARGS schema --table "edge=${_f}/edge.parquet")
antb1_add_cli_golden(
  schema_column_type
  ARGS schema --column-type u16=DATE --table "edge=${_f}/edge.parquet"
)
antb1_add_cli_golden(schema_decimals ARGS schema --table "d=${_f}/decimals.parquet")
antb1_add_cli_golden(
  explain
  ARGS explain -c "${_count}" --table "t=${_f}/hits_like_split/part-*.parquet"
)
# A UNION ALL (ADR 0036): one node over its blocks in written order, named as the first block is;
# the block of another type is cast below it, and only that one. Physically each block ends its own
# part pipelines, and the union's consumer is serial.
antb1_add_cli_golden(
  explain_union_all
  ARGS
    explain
    -c
    "SELECT x, COUNT(*) AS n FROM (SELECT i16 AS x FROM e WHERE id < 10 UNION ALL SELECT i32 FROM e) AS u GROUP BY x"
    --table
    "e=${_f}/edge.parquet"
)
antb1_add_cli_golden(
  explain_physical_union_all
  ARGS
    explain
    --physical
    --threads
    4
    -c
    "SELECT x, COUNT(*) AS n FROM (SELECT i16 AS x FROM e WHERE id < 10 UNION ALL SELECT i32 FROM e) AS u GROUP BY x"
    --table
    "e=${_f}/edge.parquet"
)
# A condition on a UNION ALL (ADR 0036's X7a) goes into its blocks: the block whose constant makes it
# never true is gone, a part every block's constant makes true is dropped, and the rest goes on top of
# each block, above its cast. The union is then named as its first remaining block.
antb1_add_cli_golden(
  explain_union_all_where
  ARGS
    explain
    -c
    "SELECT x, tag FROM (SELECT i16 AS x, 'small' AS tag FROM e UNION ALL SELECT i32, 'large' FROM e UNION ALL SELECT i64, 'wide' FROM e) AS u WHERE tag <> 'small' AND x > 5"
    --table
    "e=${_f}/edge.parquet"
)
antb1_add_cli_golden(
  explain_aggregate
  ARGS
    explain
    --clickbench
    -c
    "SELECT COUNT(*), SUM(ResolutionWidth) AS width, MAX(EventDate) FROM t WHERE IsMobile = 1 AND UserAgent > 1.5 AND RegionID < 3000000000 AND EventDate >= '2013-07-10'"
    --table
    "${_t}"
)
antb1_add_cli_golden(
  explain_decimal
  ARGS
    explain
    -c
    "SELECT id, p FROM d WHERE p <= 12.345 AND d38_10 > -0.5 AND d9_2 IN (1.25, 5) ORDER BY p LIMIT 3"
    --table
    "d=${_f}/decimals.parquet"
)
# Against numbers DuckDB types as DOUBLE, a DECIMAL compares in DOUBLE (ADR 0021 rule 11).
antb1_add_cli_golden(
  explain_decimal_double
  ARGS
    explain
    -c
    "SELECT id FROM d WHERE p > 1e1 AND d9_2 IN (1.25, 2e0) AND d38_10 NOT IN (1e0, 2) AND p NOT IN (1.555) AND d18_4 < b"
    --table
    "d=${_f}/decimals.parquet"
)
antb1_add_cli_golden(
  explain_group_by
  ARGS
    explain
    -c
    "SELECT COUNT(*) AS n, RegionID FROM t WHERE IsMobile = 1 GROUP BY RegionID, os LIMIT 10"
    --table
    "${_t}"
)
antb1_add_cli_golden(
  explain_dependent_keys
  ARGS
    explain
    -c
    "SELECT RegionID, RegionID + 10, COUNT(*) AS n FROM t GROUP BY RegionID, RegionID + 10 ORDER BY n DESC LIMIT 5"
    --table
    "${_t}"
)
antb1_add_cli_golden(
  explain_order_by
  ARGS
    explain
    -c
    "SELECT RegionID, COUNT(*) AS c FROM t GROUP BY RegionID ORDER BY c DESC, MAX(EventTime) NULLS FIRST LIMIT 10 OFFSET 5"
    --table
    "${_t}"
)
antb1_add_cli_golden(
  query_order_by
  ARGS
    query
    --threads
    1
    --clickbench
    -c
    "SELECT WatchID, EventDate FROM t WHERE RegionID < 3000 ORDER BY EventDate DESC, WatchID LIMIT 4 OFFSET 2"
    --table
    "${_t}"
)
antb1_add_cli_golden(
  explain_projection
  ARGS explain -c "SELECT URL, \"title\" AS t FROM t WHERE SearchPhrase <> '' LIMIT 10" --table "${_t}"
)
# explain --analyze: counts are exact on any number of threads, times and memory vary (regexes).
set(_d "[0-9]+[.][0-9][0-9][0-9]m?s")
set(_mem "[0-9.]+ [KMGT]?B")
set(_split "t=${_f}/hits_like_split/part-*.parquet")
string(
  CONCAT _explain_analyze
  "^Output: RegionID:INTEGER c:BIGINT\n"
  "Total: time=${_d} rows=3 peak_memory=${_mem} threads=4\n"
  "Project RegionID, c  [[]rows=3 batches=1 time=${_d} self=${_d}[]]\n"
  "  TopN Sort c DESC NULLS LAST, RegionID ASC NULLS LAST Limit 3  "
  "[[]rows=3 batches=1 time=${_d} self=${_d} sort=${_d}[]]\n"
  "    PartGroupAggregate GroupAggregate keys=[[]RegionID[]] COUNT[(][*][)] top-N per partition keep=3  "
  # rows is keep=3 per partition, so it tracks GroupTable::kPartitions: 96 at 32, 192 at 64.
  "[[]rows=96 batches=[0-9]+ time=${_d} parts=11 skipped=0 part_time=${_d} wait=${_d} "
  # local_groups is each part's own group count summed, so it exceeds `groups`, the distinct
  # count over the whole table: the parts hold the same key in more than one of their tables.
  # routed_path is absent here because no part of this fixture routes (no raw_parts either).
  "lanes_tail=${_d} build=${_d} groups=4357 local_groups=[0-9]+ local_path=${_d} "
  "local_rows=[0-9]+[]]\n"
  "      Filter IsMobile = 1  [[]rows=5064 batches=11 parts=11 time=${_d} [(]summed over parts[)] "
  "self=${_d}[]]\n"
  # cols_filter_only: IsMobile is read so that the pushed predicate can be applied and by nothing
  # above the scan, so no batch carries it (ADR 0020). Asserted here rather than read by hand,
  # because a scan that stops doing it answers exactly the same.
  "        Scan table=t source=parquet[(]files=4, rows=10000[)] columns=[[]RegionID, IsMobile[]], "
  "1 pushed predicate  [[]rows=5064 batches=11 parts=11 time=${_d} [(]summed over parts[)] "
  "cols_filter_only=1[]]$"
)
antb1_add_cli_golden(
  explain_analyze
  STDOUT_REGEX "${_explain_analyze}"
  ARGS
    explain
    --analyze
    --threads
    4
    -c
    "SELECT RegionID, COUNT(*) AS c FROM t WHERE IsMobile = 1 GROUP BY RegionID ORDER BY c DESC, RegionID LIMIT 3"
    --table
    "${_split}"
)
# A plain BETWEEN is two comparisons, both pushed into the scan (and used for part pruning).
antb1_add_cli_golden(
  explain_analyze_between
  STDOUT_REGEX
    "Filter RegionID >= 100 AND RegionID <= 200  [[][^]]*[]]\n +Scan table=t source=parquet[(]files=4, rows=10000[)] columns=[[]RegionID[]], 2 pushed predicates  [[]rows=[0-9]+ "
  ARGS explain --analyze --threads 4 -c "SELECT COUNT(*) FROM t WHERE RegionID BETWEEN 100 AND 200" --table "${_split}"
)
set(_n "[0-9]+")
string(
  CONCAT _explain_analyze_json
  "^[{]\"output\":\"Output: count_star[(][)]:BIGINT\",\"time_ns\":${_n},\"rows\":1,"
  "\"peak_memory_bytes\":${_n},\"threads\":1,\"plan\":"
  "[{]\"name\":\"PartAggregate\",\"detail\":\"Aggregate COUNT[(][*][)]\",\"per_part\":false,"
  "\"runs\":1,\"rows\":1,\"batches\":1,\"time_ns\":${_n},\"self_time_ns\":${_n},"
  "\"metrics\":[{]\"parts\":11,\"skipped\":0,\"part_time_ns\":${_n},\"wait_ns\":${_n},"
  "\"merge_ns\":${_n}[}],\"inputs\":[[]"
  "[{]\"name\":\"Filter\",\"detail\":\"Filter RegionID > 100\",\"per_part\":true,\"runs\":11,"
  "\"rows\":9640,\"batches\":11,\"time_ns\":${_n},\"self_time_ns\":${_n},\"metrics\":[{][}],"
  "\"inputs\":[[]"
  "[{]\"name\":\"Scan\",\"detail\":\"Scan table=t source=parquet[(]files=4, rows=10000[)] "
  "columns=[[]RegionID[]], 1 pushed predicate\",\"per_part\":true,\"runs\":11,\"rows\":9640,\"batches\":11,"
  # COUNT(*) reads nothing above the Filter, so every column the scan reads is read to be compared
  # and for nothing else -- the case an empty `needed_above` names, and the most valuable one.
  "\"time_ns\":${_n},\"self_time_ns\":${_n},\"metrics\":[{]\"cols_filter_only\":1[}],\"inputs\":[[][]]"
  "[}][]][}][]][}][}]$"
)
antb1_add_cli_golden(
  explain_analyze_json
  STDOUT_REGEX "${_explain_analyze_json}"
  ARGS explain --analyze --threads 1 --format json -c "SELECT COUNT(*) FROM t WHERE RegionID > 100" --table "${_split}"
)
# A mark join outputs every probe row once (rows=40 of zones' 40); its build counts the set's NULL
# keys, which make the mark NULL where it does not match (ADR 0041's SQ7a).
string(
  CONCAT _explain_analyze_mark
  # Balanced brackets only: an unbalanced [ would keep CMake from splitting the arguments after it.
  "HashJoin Join NULL-AWARE MARK build=right keys=.\"CAST.zn_city AS INTEGER.\" = zn_parent. "
  "carries 2 of 4 unread above  .rows=40 [^\n]*\n"
  ".*HashBuild Join NULL-AWARE MARK build=right keys=.\"CAST.zn_city AS INTEGER.\" = zn_parent. "
  "carries 2 of 4 unread above  .rows=3 [^\n]* null_keys=5 "
)
antb1_add_cli_golden(
  explain_analyze_in_subquery_mark
  STDOUT_REGEX "${_explain_analyze_mark}"
  ARGS
    explain
    --analyze
    --threads
    1
    -c
    "SELECT COUNT(*) FROM zones WHERE zn_id < 3 OR zn_city NOT IN (SELECT zn_parent FROM zones WHERE zn_id < 9)"
    --table
    "zones=${_f}/star/zones.parquet"
)
antb1_add_cli_golden(
  explain_analyze_bind_error
  EXIT_CODE 1
  STDERR
  ARGS explain --analyze --threads 1 -c "SELECT nope FROM t" --table "${_t}"
)
antb1_add_cli_golden(
  explain_analyze_json_error
  EXIT_CODE 1
  STDERR
  ARGS explain --analyze --format json --threads 1 -c "SELECT nope FROM t" --table "${_t}"
)
antb1_add_cli_golden(
  usage_explain_format_needs_analyze
  EXIT_CODE 2
  STDERR_REGEX "--format requires --analyze"
  ARGS explain --format json -c "${_count}" --table "${_t}"
)
antb1_add_cli_golden(
  version
  STDOUT_REGEX "^antb1 [0-9]+[.][0-9]+[.][0-9]+[^ ]* [(]Apache Arrow [0-9]+[.][0-9]+[.][0-9]+[)]$"
  ARGS version
)
# The build behind a binary (docs/adr/0033): the same first line, then one "<key>: <value>" line per fact. The
# values differ between builds (dynamic, static-thinlto), except that every codec must be there.
set(_line "[^\n]+\n")
set(_verbose "^antb1 [0-9]+[.][0-9]+[.][0-9]+[^ ]* [(]Apache Arrow [0-9]+[.][0-9]+[.][0-9]+[)]\n")
string(APPEND _verbose "commit: ${_line}build type: ${_line}compiler: ${_line}flavor: (dynamic|static-thinlto)\n")
string(APPEND _verbose "cpu baseline: ${_line}target: ${_line}arrow: ${_line}arrow simd: ${_line}")
string(APPEND _verbose "codecs: snappy yes, gzip yes, brotli yes, zstd yes, lz4 yes\nmemory pool: ${_line}")
string(APPEND _verbose "mimalloc purge delay: [^\n]+$")
antb1_add_cli_golden(version_verbose STDOUT_REGEX "${_verbose}" ARGS version --verbose)

# Self-tests of the golden comparison (label harness): a changed stdout and a changed exit code must fail.
# selftest/changed_stdout.stdout is text the CLI never prints (not a plausible wrong answer such as an
# off-by-one count, which would make this self-test fail exactly when the engine is broken).
foreach(case IN ITEMS changed_stdout changed_exit_code)
  set(_exit 0)
  set(_require "stdout differs from .*changed_stdout[.]stdout")
  if(case STREQUAL "changed_exit_code")
    set(_exit 4)
    set(_require "exit code: expected 4, got '0'")
  endif()
  antb1_add_expect_test(
    harness.cli.${case}
    LABELS harness
    FIXTURES_REQUIRED antb1_fixtures
    REQUIRE "${_require}"
    COMMAND
      ${CMAKE_COMMAND}
      -DNAME=changed_stdout
      "-DGOLDEN_DIR=${CMAKE_CURRENT_SOURCE_DIR}/selftest"
      -DEXIT_CODE=${_exit}
      -P
      "${PROJECT_SOURCE_DIR}/cmake/scripts/CompareOutput.cmake"
      --
      $<TARGET_FILE:antb1>
      query
      -c
      "${_count}"
      --table
      "${_t}"
      --format
      csv
  )
endforeach()

# Self-tests of ANTB1_CLI_UNDER_TEST, which runs the CLI goldens against another binary (the packaged standalone
# antb1, scripts/static.sh): the file it names runs instead of the build tree's antb1 (CMake stands in, so the
# test fails unless the override is honoured), and it is refused together with ANTB1_UPDATE_GOLDENS.
foreach(case IN ITEMS under_test under_test_update)
  set(_exit 0)
  set(_env "ANTB1_CLI_UNDER_TEST=${CMAKE_COMMAND}")
  set(_require "ANTB1_CLI_UNDER_TEST=.* instead of ")
  if(case STREQUAL "under_test_update")
    set(_exit 1)
    set(_env ${_env} ANTB1_UPDATE_GOLDENS=1)
    set(_require "never used together with[ \n]+ANTB1_UPDATE_GOLDENS") # CMake wraps the message
  endif()
  antb1_add_expect_test(
    harness.cli.${case}
    LABELS harness
    EXIT_CODE ${_exit}
    REQUIRE "${_require}"
    COMMAND
      ${CMAKE_COMMAND}
      -E
      env
      ${_env}
      ${CMAKE_COMMAND}
      -DANTB1_CLI=ON
      -DNAME=under_test
      "-DGOLDEN_DIR=${CMAKE_CURRENT_SOURCE_DIR}/selftest"
      -DEXIT_CODE=0
      "-DSTDOUT_REGEX=^cmake version "
      -P
      "${PROJECT_SOURCE_DIR}/cmake/scripts/CompareOutput.cmake"
      --
      $<TARGET_FILE:antb1>
      --version
  )
endforeach()
