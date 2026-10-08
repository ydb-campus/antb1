# antb1-slt: sqllogictest runner (tests/slt/README.md). The DuckDB oracle is linked only here, never into src/.
add_library(
  antb1_slt_lib
  STATIC
  runner/answers.cc
  runner/antb1_engine.cc
  runner/canonical.cc
  runner/clickbench.cc
  runner/difftest.cc
  runner/ordered_compare.cc
  runner/query_file.cc
  runner/query_gen.cc
  runner/ratchet.cc
  runner/result_diff.cc
  runner/runner.cc
  runner/sha256.cc
  runner/slt_file.cc
  runner/tables.cc
  runner/unordered_limit.cc
)
# SHA-256 hashes every fixture value (harness.fixtures.digest): optimize it in Debug builds too.
set_source_files_properties(runner/sha256.cc PROPERTIES COMPILE_OPTIONS "-O2")
# runner/ for the runner headers; tests/slt for supported_features.h (also used by tests/metamorphic).
target_include_directories(antb1_slt_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/runner" "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(
  antb1_slt_lib
  PUBLIC antb1::engine antb1::io antb1::plan antb1::common Arrow::arrow_shared
  PRIVATE antb1::build_options Parquet::parquet_shared
)
if(ANTB1_HAVE_DUCKDB)
  target_sources(antb1_slt_lib PRIVATE runner/duckdb_engine.cc)
  target_link_libraries(antb1_slt_lib PUBLIC antb1_ext_duckdb)
  target_compile_definitions(antb1_slt_lib PUBLIC ANTB1_SLT_HAVE_DUCKDB)
endif()

add_executable(antb1-slt runner/main.cc)
target_link_libraries(antb1-slt PRIVATE antb1_slt_lib CLI11::CLI11 antb1::build_options)

antb1_add_gtest(
  antb1_slt_tests
  LABEL harness
  PREFIX harness
  SOURCES
    tests/answers_test.cc
    tests/antb1_engine_test.cc
    tests/canonical_test.cc
    tests/clickbench_test.cc
    tests/ratchet_test.cc
    tests/comparator_test.cc
    tests/limit_compare_test.cc
    tests/ordered_compare_test.cc
    tests/difftest_test.cc
    tests/join_tables.cc
    tests/query_file_test.cc
    tests/query_gen_test.cc
    tests/runner_test.cc
    tests/slt_file_test.cc
    tests/supported_features_test.cc
    tests/tables_test.cc
  LIBS antb1_slt_lib Parquet::parquet_shared
)
# fuzz/ for sql_parser_property.h: generated queries must keep the parser's round-trip property.
target_include_directories(antb1_slt_tests PRIVATE "${PROJECT_SOURCE_DIR}/fuzz")
if(ANTB1_HAVE_DUCKDB)
  # Against the DuckDB library: plan::DuckDbFloatOf on seeded random literals, long runs of ties, the
  # adapter's DECIMAL columns, the keywords DuckDB reads as table aliases and DuckDB's reading of
  # generated joins.
  target_sources(
    antb1_slt_tests
    PRIVATE
      tests/duckdb_engine_test.cc
      tests/float_literal_oracle_test.cc
      tests/ordered_compare_oracle_test.cc
      tests/query_gen_oracle_test.cc
      tests/table_alias_oracle_test.cc
  )
endif()

# Test cases: tests/slt/cases/<area>/<file>.slt -> slt.<area>.<file> (+ oracle.<area>.<file>).
file(GLOB _areas CONFIGURE_DEPENDS LIST_DIRECTORIES true "${CMAKE_CURRENT_SOURCE_DIR}/cases/*")
foreach(area_dir IN LISTS _areas)
  if(IS_DIRECTORY "${area_dir}")
    antb1_add_slt_suite(DIR "${area_dir}")
  endif()
endforeach()

# Parallel execution (label parallel, docs/adr/0013-parallel-execution.md): every case file on 4 engine threads,
# with batches of 1000 rows so that parts span several batches. Every result must equal the 1-thread result
# byte for byte, before any sorting (antb1-slt --same-as-threads), and the expectations as usual.
foreach(area_dir IN LISTS _areas)
  if(IS_DIRECTORY "${area_dir}")
    get_filename_component(_area "${area_dir}" NAME)
    file(GLOB _files CONFIGURE_DEPENDS "${area_dir}/*.slt")
    foreach(file IN LISTS _files)
      get_filename_component(_stem "${file}" NAME_WE)
      set(_name "parallel.${_area}.${_stem}")
      antb1_slt_args(
        _args
        ${_name}
        --engine
        antb1
        --threads
        4
        --same-as-threads
        1
        --batch-size
        1000
      )
      add_test(NAME ${_name} COMMAND antb1-slt run ${_args} "${file}")
      set_tests_properties(
        ${_name}
        PROPERTIES LABELS parallel FIXTURES_REQUIRED antb1_fixtures TIMEOUT 300
      )
    endforeach()
  endif()
endforeach()

# Harness self-tests: selftest/mutate.slt passes as is (slt.selftest.mutate), and every corruption of the
# antb1 results must make the runner fail with exit code 1 and a FAIL line.
antb1_add_slt_suite(DIR selftest)
set(_mutate_file "${CMAKE_CURRENT_SOURCE_DIR}/selftest/mutate.slt")
set(
  _mutations
  value
  null
  drop-row
  extra-row
  extra-column
  error
  unsupported
  succeed
  canary
) # runner.h: Mutation
foreach(kind IN LISTS _mutations)
  set(_name harness.slt.mutate.${kind})
  antb1_slt_args(_args ${_name} --engine antb1 --mutate ${kind})
  set(_require "FAIL .*mutate\\.slt:[0-9]+: " "SLT: FAIL ")
  if(kind STREQUAL "unsupported")
    list(APPEND _require "antb1 reports Unsupported" "unsupported=[1-9]")
  endif()
  antb1_add_expect_test(
    ${_name}
    LABELS harness
    FIXTURES_REQUIRED antb1_fixtures
    REQUIRE ${_require}
    COMMAND $<TARGET_FILE:antb1-slt> run ${_args} "${_mutate_file}"
  )
endforeach()

# Redaction canary: a mismatch reported with --redact must not print the SQL text or any value (the
# expected 10000, the mutated 100001), only ids, types, row counts and hashes.
set(_name harness.slt.redact)
set(
  _options
  --engine
  antb1
  --mutate
  value
  --redact
)
antb1_slt_args(_args ${_name} ${_options})
antb1_add_expect_test(
  ${_name}
  LABELS harness
  FIXTURES_REQUIRED antb1_fixtures
  REQUIRE "FAIL .*redact_canary\\.slt:[0-9]+: query I nosort \\[antb1\\]: result mismatch" "sha256: expected [0-9a-f]+"
  FORBID "SELECT" "COUNT" "hits_like_required" "(^|[^0-9a-f])10000" "100001"
  COMMAND $<TARGET_FILE:antb1-slt> run ${_args} "${CMAKE_CURRENT_SOURCE_DIR}/selftest/redact_canary.slt"
)

# Redaction canary with sentinels: canary/redact_sentinels.slt fails in every record, and a redacted
# report must not print any sentinel (SQL, expected and actual values, error messages, regexes).
set(_canary_regex "[aA][nN][tT][bB]1_[cC][aA][nN][aA][rR][yY]") # ANTB1_CANARY_*, antb1_canary_*
set(_name harness.slt.redact_sentinels)
antb1_slt_args(
  _args
  ${_name}
  --engine
  antb1
  --redact
  --mutate
  canary
)
antb1_add_expect_test(
  ${_name}
  LABELS harness
  FIXTURES_REQUIRED antb1_fixtures
  REQUIRE "FAIL .*redact_sentinels\\.slt:[0-9]+: query I nosort \\[antb1\\]: result mismatch" "SLT: FAIL .* failed=4 "
  FORBID "${_canary_regex}" "[sS][eE][lL][eE][cC][tT]"
  COMMAND $<TARGET_FILE:antb1-slt> run ${_args} "${CMAKE_CURRENT_SOURCE_DIR}/canary/redact_sentinels.slt"
)

# Random differential test (label diff): antb1 vs DuckDB on generated queries (runner/difftest.h),
# with a fixed seed. `pixi run diff-random` runs more (ANTB1_DIFF_SEED, ANTB1_DIFF_COUNT) over every table.
# diff.random names its tables (a table added to tables.txt would change every query of the seed);
# diff.decimal covers the DECIMAL columns of decimals.parquet with its own seed, and diff.star the star
# schema of the join tests (tools/fixturegen/star.h) with another. A new seed must pass `pixi run asan` first:
# DuckDB itself leaks on the queries of some seeds (docs/testing.md).
set(
  _diff_tables
  --table
  hits_like
  --table
  hits_like_nulls
  --table
  hits_like_split
  --table
  hits_like_required
  --table
  edge
  --table
  floats
  --table
  empty
)
set(
  _star_tables
  --table
  trips
  --table
  riders
  --table
  drivers
  --table
  zones
  --table
  cities
  --table
  shifts
  --table
  tariffs
  --table
  promos
)
if(ANTB1_HAVE_DUCKDB)
  antb1_slt_args(_args diff.random)
  add_test(
    NAME diff.random
    COMMAND antb1-slt diff ${_args} ${_diff_tables} --seed 20260925 --count 300
  )
  set_tests_properties(
    diff.random
    PROPERTIES LABELS diff FIXTURES_REQUIRED antb1_fixtures TIMEOUT 300
  )
  antb1_slt_args(_args diff.decimal)
  add_test(
    NAME diff.decimal
    COMMAND antb1-slt diff ${_args} --table decimals --seed 20261003 --count 300
  )
  set_tests_properties(
    diff.decimal
    PROPERTIES LABELS diff FIXTURES_REQUIRED antb1_fixtures TIMEOUT 300
  )
  antb1_slt_args(_args diff.star)
  add_test(
    NAME diff.star
    COMMAND antb1-slt diff ${_args} ${_star_tables} --seed 20261005 --count 300
  )
  set_tests_properties(
    diff.star
    PROPERTIES LABELS diff FIXTURES_REQUIRED antb1_fixtures TIMEOUT 300
  )
  # The same queries (seed) on 4 threads with 700-row batches: DuckDB's answers, and the 1-thread results byte for
  # byte. (Seed 20260929 makes DuckDB itself leak under ASan: MIN of VARCHAR with GROUP BY and LIMIT.)
  antb1_slt_args(
    _args
    parallel.diff.random
    --threads
    4
    --same-as-threads
    1
    --batch-size
    700
  )
  add_test(
    NAME parallel.diff.random
    COMMAND antb1-slt diff ${_args} ${_diff_tables} --seed 20260925 --count 300
  )
  set_tests_properties(
    parallel.diff.random
    PROPERTIES LABELS parallel FIXTURES_REQUIRED antb1_fixtures TIMEOUT 300
  )
  # diff.star on 4 threads with 97-row batches, fewer than the rows of every part of trips and shifts, so that a part
  # spans several batches.
  antb1_slt_args(
    _args
    parallel.diff.star
    --threads
    4
    --same-as-threads
    1
    --batch-size
    97
  )
  add_test(
    NAME parallel.diff.star
    COMMAND antb1-slt diff ${_args} ${_star_tables} --seed 20261005 --count 300
  )
  set_tests_properties(
    parallel.diff.star
    PROPERTIES LABELS parallel FIXTURES_REQUIRED antb1_fixtures TIMEOUT 300
  )

  # Self-tests: the differential test must catch every corruption of antb1's results.
  foreach(
    kind
    IN
    ITEMS value null drop-row extra-row extra-column error unsupported canary
  )
    set(_name harness.diff.mutate.${kind})
    antb1_slt_args(
      _args
      ${_name}
      --seed
      1
      --count
      12
      --mutate
      ${kind}
    )
    set(
      _require
      "FAIL diff case [0-9]+ \\(seed 1, "
      "ANTB1_DIFF_SEED=1 ANTB1_DIFF_ONLY=[0-9]+ pixi run diff-random"
      "DIFF: FAIL "
    )
    if(kind STREQUAL "unsupported")
      list(APPEND _require "antb1 reports Unsupported for a query that uses only supported features")
    endif()
    antb1_add_expect_test(
      ${_name}
      LABELS harness
      FIXTURES_REQUIRED antb1_fixtures
      REQUIRE ${_require}
      COMMAND $<TARGET_FILE:antb1-slt> diff ${_args}
    )
  endforeach()

  # Redaction canary: the only table is named after a sentinel (it appears in the SQL only) and antb1's
  # first value becomes a sentinel; a redacted report prints neither, nor any SQL.
  set(_name harness.diff.redact)
  antb1_slt_args(
    _args
    ${_name}
    TABLES "${CMAKE_CURRENT_SOURCE_DIR}/canary/tables.txt"
    --seed
    1
    --count
    12
    --redact
    --mutate
    canary
  )
  antb1_add_expect_test(
    ${_name}
    LABELS harness
    FIXTURES_REQUIRED antb1_fixtures
    REQUIRE
      "FAIL diff case [0-9]+ \\(seed 1, supported features\\): result mismatch"
      "sha256: DuckDB [0-9a-f]+"
      "DIFF: FAIL "
    FORBID "${_canary_regex}" "[sS][eE][lL][eE][cC][tT]"
    COMMAND $<TARGET_FILE:antb1-slt> diff ${_args}
  )

  # `antb1-slt queries` and `antb1-slt clickbench` (the ClickBench data tests, tests/data) on the canary query
  # file: a correct run passes, corrupted answers and ratchet differences fail, and a redacted report prints
  # neither the SQL (it names the sentinel table) nor a value.
  set(_queries "${CMAKE_CURRENT_SOURCE_DIR}/canary/canary_queries.sql")
  set(_canary_tables TABLES "${CMAKE_CURRENT_SOURCE_DIR}/canary/tables.txt")
  set(_redacted_forbid FORBID "${_canary_regex}" "[sS][eE][lL][eE][cC][tT]")
  set(_cases pass mutate.value mutate.unsupported redact)
  set(_opts_pass "")
  set(_exit_pass 0)
  set(_require_pass "QUERIES: PASS .* queries=2 compared=1 pending=1 rejected=0 failed=0")
  set(_opts_mutate.value --mutate value)
  set(_exit_mutate.value 1)
  set(_require_mutate.value "FAIL .*canary_queries\\.sql:[0-9]+: result mismatch" "QUERIES: FAIL ")
  set(_opts_mutate.unsupported --mutate unsupported)
  set(_exit_mutate.unsupported 1)
  set(_require_mutate.unsupported "antb1 reports Unsupported for a query that uses only supported features")
  set(_opts_redact --redact --mutate canary)
  set(_exit_redact 1)
  set(_require_redact "FAIL .*canary_queries\\.sql:[0-9]+: result mismatch" "sha256: DuckDB [0-9a-f]+" "QUERIES: FAIL ")
  foreach(case IN LISTS _cases)
    set(_name harness.queries.${case})
    antb1_slt_args(_args ${_name} ${_canary_tables} ${_opts_${case}})
    set(_forbid "")
    if(case STREQUAL "redact")
      set(_forbid ${_redacted_forbid})
    endif()
    antb1_add_expect_test(
      ${_name}
      LABELS harness
      FIXTURES_REQUIRED antb1_fixtures
      EXIT_CODE ${_exit_${case}}
      REQUIRE ${_require_${case}} ${_forbid}
      COMMAND $<TARGET_FILE:antb1-slt> queries ${_args} "${_queries}"
    )
  endforeach()

  # clickbench: the ratchet (canary/status_pass_*.json) must equal the passing queries exactly.
  set(_cases pass unexpected_pass unexpected_fail redact)
  set(_status_pass q0)
  set(_exit_pass 0)
  set(_require_pass "Q0: pass" "Q1: unsupported" "CLICKBENCH: PASS queries=2 pass=\\[0\\]")
  set(_status_unexpected_pass none)
  set(_exit_unexpected_pass 1)
  set(_require_unexpected_pass "FAIL Q0: unexpected pass" "CLICKBENCH: FAIL ")
  set(_status_unexpected_fail q0_q1)
  set(_exit_unexpected_fail 1)
  set(_require_unexpected_fail "FAIL Q1: unexpected fail" "CLICKBENCH: FAIL ")
  set(_status_redact q0)
  set(_opts_redact --redact --mutate canary)
  set(_exit_redact 1)
  set(_require_redact "FAIL Q0: result mismatch" "sha256: DuckDB [0-9a-f]+" "CLICKBENCH: FAIL ")
  set(_opts_pass "")
  set(_opts_unexpected_pass "")
  set(_opts_unexpected_fail "")
  foreach(case IN LISTS _cases)
    set(_name harness.clickbench.${case})
    set(_status "${CMAKE_CURRENT_SOURCE_DIR}/canary/status_pass_${_status_${case}}.json")
    antb1_slt_args(_args ${_name} ${_canary_tables} --status "${_status}" ${_opts_${case}})
    set(_forbid "")
    if(case STREQUAL "redact")
      set(_forbid ${_redacted_forbid})
    endif()
    antb1_add_expect_test(
      ${_name}
      LABELS harness
      FIXTURES_REQUIRED antb1_fixtures
      EXIT_CODE ${_exit_${case}}
      REQUIRE ${_require_${case}} ${_forbid}
      COMMAND $<TARGET_FILE:antb1-slt> clickbench ${_args} "${_queries}"
    )
  endforeach()

  # `antb1-slt answers` (runner/answers.h) on our own queries over the canary table and their answers, written
  # into the build tree (lint R009 keeps files named like q01.sql out of the repository). Each answer is spelled
  # as a check must accept it: a DECIMAL without its trailing zeros (Q2), rows in another order (Q3), NULL as an
  # empty field (Q4). The run passes; with --mutate canary every query fails, and the report, redacted without
  # --redact, prints no SQL and no value. --show-values prints them (here our own canary values, so the test unsets
  # GITHUB_ACTIONS) and is refused on GitHub Actions.
  set(_answers_dir "${CMAKE_CURRENT_BINARY_DIR}/answers-canary")
  file(WRITE "${_answers_dir}/queries/q01.sql" "SELECT COUNT(*) /* ANTB1_CANARY_SQL */ FROM antb1_canary_table;\n")
  file(WRITE "${_answers_dir}/answers/q01.csv" "count_star()\n12\n")
  file(
    WRITE "${_answers_dir}/queries/q02.sql"
    "SELECT SUM(CAST(id AS DECIMAL(15,2))) AS total FROM antb1_canary_table;\n"
  )
  file(WRITE "${_answers_dir}/answers/q02.csv" "total\n66\n")
  file(
    WRITE "${_answers_dir}/queries/q03.sql"
    "SELECT id, 'antb1_canary_text' AS t FROM antb1_canary_table WHERE id < 3;\n"
  )
  file(WRITE "${_answers_dir}/answers/q03.csv" "id|t\n2|antb1_canary_text\n1|antb1_canary_text\n0|antb1_canary_text\n")
  file(
    WRITE "${_answers_dir}/queries/q04.sql"
    "SELECT CAST(NULL AS INTEGER) AS n FROM antb1_canary_table WHERE id = 0;\n"
  )
  file(WRITE "${_answers_dir}/answers/q04.csv" "n\n\n")
  set(_answers_args --queries "${_answers_dir}/queries" --answers "${_answers_dir}/answers")
  set(_cases pass redact show_values show_values_on_ci)
  set(_opts_pass "")
  set(_exit_pass 0)
  set(
    _require_pass
    "Q1: pass\n"
    "Q2: pass\n"
    "Q3: pass \\(rows in another order\\)\n"
    "Q4: pass\n"
    "ANSWERS: PASS queries=4 passed=4 reordered=1 failed=0"
  )
  set(_opts_redact --mutate canary)
  set(_exit_redact 1)
  set(
    _require_redact
    "FAIL Q1: result mismatch: values differ"
    "sha256: answer [0-9a-f]+"
    "--show-values --only 1\n"
    "ANSWERS: FAIL queries=4 passed=0 reordered=0 failed=4"
  )
  set(
    _opts_show_values
    --mutate
    canary
    --show-values
    --only
    1
  )
  set(_exit_show_values 1)
  set(
    _require_show_values
    "FAIL Q1: result mismatch: values differ\n  SQL:\n    SELECT COUNT\\(\\*\\) /\\* ANTB1_CANARY_SQL \\*/ FROM antb1_canary_table\n"
    "row 0: answer 12\n +DuckDB ANTB1_CANARY_VALUE\n"
    "  repro:\n.* --show-values --only 1\n"
  )
  set(_opts_show_values_on_ci --show-values)
  set(_exit_show_values_on_ci 2)
  set(_require_show_values_on_ci "--show-values .*refused when GITHUB_ACTIONS=true")
  foreach(case IN LISTS _cases)
    set(_name harness.answers.${case})
    antb1_slt_args(_args ${_name} ${_canary_tables} ${_answers_args} ${_opts_${case}})
    set(_forbid ${_redacted_forbid})
    if(case STREQUAL "show_values")
      set(_forbid "")
    endif()
    antb1_add_expect_test(
      ${_name}
      LABELS harness
      FIXTURES_REQUIRED antb1_fixtures
      EXIT_CODE ${_exit_${case}}
      REQUIRE ${_require_${case}} ${_forbid}
      COMMAND $<TARGET_FILE:antb1-slt> answers ${_args}
    )
  endforeach()
  set_tests_properties(harness.answers.show_values PROPERTIES ENVIRONMENT_MODIFICATION "GITHUB_ACTIONS=unset:")
  set_tests_properties(harness.answers.show_values_on_ci PROPERTIES ENVIRONMENT "GITHUB_ACTIONS=true")

  # `antb1-slt tpch` (runner/ratchet.h; the ratchet of the queries derived from TPC-H, tests/tpch) on our own numbered
  # queries over the canary table marked `redact`, written into the build tree (lint R009). Each case is a directory
  # of queries and a ratchet: a window function is the standard unsupported query, so all-unsupported passes; and an
  # unexpected pass, an unexpected fail, a wrong answer (--mutate canary), and a parse, bind or execution error (only
  # Unsupported is a clean failure for this workload) each fail with a redacted report.
  set(_tpch_selftest "${CMAKE_CURRENT_BINARY_DIR}/tpch-selftest")
  set(_unsupported "SELECT ROW_NUMBER() OVER () FROM antb1_canary_table;\n")
  set(_answered "SELECT COUNT(*) FROM antb1_canary_table;\n")
  set(_q1_pass "${_unsupported}")
  set(_q1_unexpected_pass "${_answered}")
  set(_q1_unexpected_fail "${_unsupported}")
  set(_q1_wrong_answer "${_answered}")
  set(_q1_parse_error "SELECT FROM antb1_canary_table WHERE;\n")
  set(_q1_bind_error "SELECT antb1_no_such_column FROM antb1_canary_table;\n")
  set(_q1_execution_error "SELECT id * 9223372036854775807 FROM antb1_canary_table;\n")
  set(
    _cases
    pass
    unexpected_pass
    unexpected_fail
    wrong_answer
    parse_error
    bind_error
    execution_error
  )
  set(
    _require_pass
    "Q1: running\nQ1: unsupported\nQ2: running\nQ2: unsupported\n"
    "TPCH: PASS queries=2 pass=\\[\\] unsupported=2 rejected=0 failed=0 ratchet=\\[\\]\n"
  )
  set(_require_unexpected_pass "FAIL Q1: unexpected pass: antb1 answers Q1 and equals DuckDB")
  set(_require_unexpected_fail "FAIL Q1: unexpected fail: the ratchet .* lists Q1, but it no longer passes")
  set(_require_wrong_answer "FAIL Q1: result mismatch" "sha256: DuckDB [0-9a-f]+")
  set(_require_parse_error "FAIL Q1: antb1 fails with a parse error; a query derived from TPC-H")
  set(_require_bind_error "FAIL Q1: antb1 fails with a bind error; a query derived from TPC-H")
  set(_require_execution_error "FAIL Q1: antb1 fails with an execution error; a query derived from TPC-H")
  foreach(case IN LISTS _cases)
    set(_name harness.tpch.status.${case})
    set(_dir "${_tpch_selftest}/${case}")
    file(WRITE "${_dir}/queries/q01.sql" "${_q1_${case}}")
    file(WRITE "${_dir}/queries/q02.sql" "${_unsupported}")
    set(_pass "")
    if(case MATCHES "^(unexpected_fail|wrong_answer)$")
      set(_pass 1)
    endif()
    file(WRITE "${_dir}/status.json" "{\"pass\": [${_pass}]}\n")
    set(_exit 1)
    set(_options "")
    if(case STREQUAL "pass")
      set(_exit 0)
    elseif(case STREQUAL "wrong_answer")
      set(_options --mutate canary)
    endif()
    antb1_slt_args(
      _args
      ${_name}
      TABLES "${CMAKE_CURRENT_SOURCE_DIR}/canary/tables_redact.txt"
      --queries
      "${_dir}/queries"
      --status
      "${_dir}/status.json"
      --memory-limit
      1GiB
      ${_options}
    )
    set(_require ${_require_${case}})
    if(NOT case STREQUAL "pass")
      list(APPEND _require "TPCH: FAIL queries=2 ")
    endif()
    if(NOT case MATCHES "^(pass|unexpected_pass)$")
      list(APPEND _require "--show-values --only 1\n")
    endif()
    antb1_add_expect_test(
      ${_name}
      LABELS harness
      FIXTURES_REQUIRED antb1_fixtures
      EXIT_CODE ${_exit}
      REQUIRE ${_require} ${_redacted_forbid}
      COMMAND $<TARGET_FILE:antb1-slt> tpch ${_args}
    )
    # Their output must not depend on a developer's ANTB1_TPCH_TIMES (ctest passes the environment through).
    set_tests_properties(${_name} PROPERTIES ENVIRONMENT_MODIFICATION "ANTB1_TPCH_TIMES=unset:")
  endforeach()

  # The `redact` option of a tables file (runner/tables.h): canary/tables_redact.txt marks the canary table, so every
  # report is redacted without --redact and its repro adds --show-values; an explicit --redact wins over
  # --show-values; `diff --list` (also with --redact over any tables) and `complete` are refused.
  set(_redact_tables TABLES "${CMAKE_CURRENT_SOURCE_DIR}/canary/tables_redact.txt")
  set(_name harness.slt.redact_table)
  antb1_slt_args(
    _args
    ${_name}
    ${_redact_tables}
    --engine
    antb1
    --mutate
    canary
  )
  antb1_add_expect_test(
    ${_name}
    LABELS harness
    FIXTURES_REQUIRED antb1_fixtures
    REQUIRE
      "FAIL .*redact_table\\.slt:[0-9]+: query I nosort \\[antb1\\]: result mismatch"
      "sha256: expected [0-9a-f]+"
      "--show-values\n"
      "SLT: FAIL "
      ${_redacted_forbid}
    COMMAND $<TARGET_FILE:antb1-slt> run ${_args} "${CMAKE_CURRENT_SOURCE_DIR}/canary/redact_table.slt"
  )
  set(_name harness.clickbench.redact_table)
  antb1_slt_args(
    _args
    ${_name}
    ${_redact_tables}
    --status
    "${CMAKE_CURRENT_SOURCE_DIR}/canary/status_pass_q0.json"
    --mutate
    canary
  )
  antb1_add_expect_test(
    ${_name}
    LABELS harness
    FIXTURES_REQUIRED antb1_fixtures
    REQUIRE
      "FAIL Q0: result mismatch"
      "sha256: DuckDB [0-9a-f]+"
      "--show-values --only 0\n"
      "CLICKBENCH: FAIL "
      ${_redacted_forbid}
    COMMAND $<TARGET_FILE:antb1-slt> clickbench ${_args} "${_queries}"
  )
  set(_name harness.queries.redact_wins)
  antb1_slt_args(
    _args
    ${_name}
    ${_redact_tables}
    --redact
    --show-values
    --mutate
    canary
  )
  antb1_add_expect_test(
    ${_name}
    LABELS harness
    FIXTURES_REQUIRED antb1_fixtures
    REQUIRE
      "FAIL .*canary_queries\\.sql:[0-9]+: result mismatch"
      "sha256: DuckDB [0-9a-f]+"
      "QUERIES: FAIL "
      ${_redacted_forbid}
    COMMAND $<TARGET_FILE:antb1-slt> queries ${_args} "${_queries}"
  )
  set_tests_properties(${_name} PROPERTIES ENVIRONMENT_MODIFICATION "GITHUB_ACTIONS=unset:")
  set(_name harness.diff.redact.list)
  antb1_slt_args(
    _args
    ${_name}
    --seed
    1
    --count
    12
    --redact
    --list
  )
  antb1_add_expect_test(
    ${_name}
    LABELS harness
    FIXTURES_REQUIRED antb1_fixtures
    EXIT_CODE 2
    REQUIRE "diff --list prints the generated SQL" ${_redacted_forbid}
    COMMAND $<TARGET_FILE:antb1-slt> diff ${_args}
  )
  set(_name harness.diff.redact_table)
  antb1_slt_args(
    _args
    ${_name}
    ${_redact_tables}
    --seed
    1
    --count
    12
    --mutate
    canary
  )
  antb1_add_expect_test(
    ${_name}
    LABELS harness
    FIXTURES_REQUIRED antb1_fixtures
    REQUIRE
      "FAIL diff case [0-9]+ \\(seed 1, supported features\\): result mismatch"
      "sha256: DuckDB [0-9a-f]+"
      "--show-values --only [0-9]+\n"
      "DIFF: FAIL "
    FORBID "${_canary_regex}" "[sS][eE][lL][eE][cC][tT]" "pixi run diff-random"
    COMMAND $<TARGET_FILE:antb1-slt> diff ${_args}
  )
  set(_name harness.diff.redact_table.list)
  antb1_slt_args(
    _args
    ${_name}
    ${_redact_tables}
    --seed
    1
    --count
    12
    --list
  )
  antb1_add_expect_test(
    ${_name}
    LABELS harness
    FIXTURES_REQUIRED antb1_fixtures
    EXIT_CODE 2
    REQUIRE "diff --list prints the generated SQL" ${_redacted_forbid}
    COMMAND $<TARGET_FILE:antb1-slt> diff ${_args}
  )
  set(_name harness.queries.redact_table)
  antb1_slt_args(_args ${_name} ${_redact_tables} --mutate canary)
  antb1_add_expect_test(
    ${_name}
    LABELS harness
    FIXTURES_REQUIRED antb1_fixtures
    REQUIRE
      "FAIL .*canary_queries\\.sql:[0-9]+: result mismatch"
      "sha256: DuckDB [0-9a-f]+"
      "QUERIES: FAIL "
      ${_redacted_forbid}
    COMMAND $<TARGET_FILE:antb1-slt> queries ${_args} "${_queries}"
  )
  # On a copy in the build tree: a regression must not rewrite a source file.
  set(_complete_file "${CMAKE_CURRENT_BINARY_DIR}/redact-table-complete.slt")
  configure_file(selftest/redact_canary.slt "${_complete_file}" COPYONLY)
  set(_name harness.complete.redact_table)
  antb1_slt_args(_args ${_name} ${_redact_tables})
  antb1_add_expect_test(
    ${_name}
    LABELS harness
    FIXTURES_REQUIRED antb1_fixtures
    EXIT_CODE 2
    REQUIRE "complete writes values into .slt files; it is refused for tables marked `redact`"
    COMMAND $<TARGET_FILE:antb1-slt> complete ${_args} "${_complete_file}"
  )
endif()

# The pending guard (tests/slt/README.md): slt.* and parallel.* skip a `pending <roadmap id>` record, which DuckDB
# runs as usual. harness.slt.pending runs the pending records of every registered file on antb1, and fails with
# "remove the guard (<id>)" when antb1 answers one with anything but Unsupported. Like slt-complete, it reads the
# registered files after the last antb1_add_slt_suite(), at the end of this file. Its self-tests make antb1 answer
# the pending records of selftest/pending.slt, or fail them with an execution error, and the check must fail.
get_property(_slt_files GLOBAL PROPERTY ANTB1_SLT_FILES)
antb1_slt_args(_args harness.slt.pending)
add_test(NAME harness.slt.pending COMMAND antb1-slt pending ${_args} ${_slt_files})
set_tests_properties(
  harness.slt.pending
  PROPERTIES LABELS harness FIXTURES_REQUIRED antb1_fixtures TIMEOUT 300
)
foreach(kind IN ITEMS succeed error)
  set(_name harness.slt.pending.mutate.${kind})
  antb1_slt_args(_args ${_name} --mutate ${kind})
  antb1_add_expect_test(
    ${_name}
    LABELS harness
    FIXTURES_REQUIRED antb1_fixtures
    REQUIRE
      "FAIL .*pending\\.slt:[0-9]+: query I nosort \\[antb1\\]: remove the guard \\(Z9\\): "
      "FAIL .*pending\\.slt:[0-9]+: statement error \\[antb1\\]: remove the guard \\(Z9\\): "
      "PENDING: FAIL files=1 with_pending=1 records=2 unsupported=0 failed=2 by id: Z9=2"
    COMMAND $<TARGET_FILE:antb1-slt> pending ${_args} "${CMAKE_CURRENT_SOURCE_DIR}/selftest/pending.slt"
  )
endforeach()

antb1_add_slt_complete_target()
