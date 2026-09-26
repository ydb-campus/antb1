# antb1-slt: sqllogictest runner (tests/slt/README.md). The DuckDB oracle is linked only here, never into src/.
add_library(
  antb1_slt_lib
  STATIC
  runner/antb1_engine.cc
  runner/canonical.cc
  runner/clickbench.cc
  runner/difftest.cc
  runner/query_file.cc
  runner/query_gen.cc
  runner/result_diff.cc
  runner/runner.cc
  runner/sha256.cc
  runner/slt_file.cc
  runner/tables.cc
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
    tests/antb1_engine_test.cc
    tests/canonical_test.cc
    tests/clickbench_test.cc
    tests/comparator_test.cc
    tests/difftest_test.cc
    tests/query_file_test.cc
    tests/query_gen_test.cc
    tests/runner_test.cc
    tests/slt_file_test.cc
  LIBS antb1_slt_lib Parquet::parquet_shared
)

# Test cases: tests/slt/cases/<area>/<file>.slt -> slt.<area>.<file> (+ oracle.<area>.<file>).
file(GLOB _areas CONFIGURE_DEPENDS LIST_DIRECTORIES true "${CMAKE_CURRENT_SOURCE_DIR}/cases/*")
foreach(area_dir IN LISTS _areas)
  if(IS_DIRECTORY "${area_dir}")
    antb1_add_slt_suite(DIR "${area_dir}")
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
# with a fixed seed. `pixi run diff-random` runs more (ANTB1_DIFF_SEED, ANTB1_DIFF_COUNT).
if(ANTB1_HAVE_DUCKDB)
  antb1_slt_args(_args diff.random)
  add_test(
    NAME diff.random
    COMMAND antb1-slt diff ${_args} --seed 20260925 --count 300
  )
  set_tests_properties(
    diff.random
    PROPERTIES LABELS diff FIXTURES_REQUIRED antb1_fixtures TIMEOUT 300
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
endif()

antb1_add_slt_complete_target()
