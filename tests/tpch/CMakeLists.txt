# Tests over data derived from TPC-H (ADR 0006). The data, the query texts and DuckDB's answers are generated at
# test time from the pinned duckdb-extension-tpch package; nothing derived from TPC-H is committed.
if(NOT ANTB1_HAVE_TPCH)
  return()
endif()

# The pinned extension loads into the duckdb CLI of the same prefix by absolute path (unsigned: conda-forge cannot
# sign DuckDB extensions) and provides the 22 query texts. DuckDB runs as everywhere in the tests: one thread, no
# extension auto-install or auto-load. The output must be exactly the count. No semicolons in the SQL: CMake would
# split the argument there.
antb1_add_expect_test(
  harness.tpch.extension
  COMMAND
    "${ANTB1_DUCKDB_CLI}"
    -unsigned
    -no-init
    -noheader
    -list
    -bail
    -cmd
    "SET threads = 1"
    -cmd
    "SET autoinstall_known_extensions = false"
    -cmd
    "SET autoload_known_extensions = false"
    -cmd
    "LOAD '${ANTB1_TPCH_EXTENSION}'"
    -c
    "SELECT count(*) FROM tpch_queries()"
  EXIT_CODE 0
  REQUIRE "^22[^0-9]?$"
  LABELS harness
)

# fixtures.tpch (label setup, fixture antb1_tpch) generates the data at scale factors 0.01 and 0.1, the 22 query texts
# and DuckDB's answers into ${ANTB1_FIXTURES_DIR}/tpch (generate.cmake), checks that every dump is DuckDB's text byte
# for byte and that the query texts match queries.sha256. It prints numbers and digests only.
set(_tpch_dir "${ANTB1_FIXTURES_DIR}/tpch")
set(_generate "${CMAKE_CURRENT_SOURCE_DIR}/generate.cmake")
set(_digests "${CMAKE_CURRENT_SOURCE_DIR}/queries.sha256")
add_test(
  NAME fixtures.tpch
  COMMAND
    ${CMAKE_COMMAND} "-DDUCKDB=${ANTB1_DUCKDB_CLI}" "-DEXTENSION=${ANTB1_TPCH_EXTENSION}" "-DTPCH_DIR=${_tpch_dir}"
    "-DBINARY_DIR=${CMAKE_BINARY_DIR}" "-DDIGESTS=${_digests}" -P "${_generate}"
)
set_tests_properties(
  fixtures.tpch
  PROPERTIES LABELS setup FIXTURES_SETUP antb1_tpch TIMEOUT 300
)

# The tests that read the generated data run through run_redacted.cmake: their tables files mark every table
# `redact`, so antb1-slt prints no value and no query text; sanitizer reports (they print values) go to files; and
# output that looks unredacted (or matches a FORBID regex) is withheld before it reaches the log, so even a broken
# redaction cannot publish the data.
# TIMEOUT (default 300 seconds) is the ctest timeout; run_redacted.cmake stops the command 30 seconds earlier, so a
# test that hangs still prints how far it got.
#   _tpch_test(<name> LABEL <label> [EXIT_CODE <n>] [TIMEOUT <seconds>] [REQUIRE <regex>...] [FORBID <regex>...]
#              [NO_DATA] COMMAND <command>...)
function(_tpch_test name)
  cmake_parse_arguments(ARG "NO_DATA" "LABEL;EXIT_CODE;TIMEOUT" "REQUIRE;FORBID;COMMAND" ${ARGN})
  if(NOT DEFINED ARG_TIMEOUT)
    set(ARG_TIMEOUT 300)
  endif()
  math(EXPR _inner_timeout "${ARG_TIMEOUT} - 30")
  set(_defs "-DWORK_DIR=${CMAKE_BINARY_DIR}/slt-tmp/${name}" "-DTIMEOUT=${_inner_timeout}")
  if(DEFINED ARG_EXIT_CODE)
    list(APPEND _defs "-DEXIT_CODE=${ARG_EXIT_CODE}")
  endif()
  foreach(kind IN ITEMS REQUIRE FORBID)
    set(_i 0)
    foreach(regex IN LISTS ARG_${kind})
      list(APPEND _defs "-D${kind}_${_i}=${regex}")
      math(EXPR _i "${_i} + 1")
    endforeach()
    list(APPEND _defs "-D${kind}_COUNT=${_i}")
  endforeach()
  add_test(
    NAME ${name}
    COMMAND ${CMAKE_COMMAND} ${_defs} -P "${CMAKE_CURRENT_SOURCE_DIR}/run_redacted.cmake" -- ${ARG_COMMAND}
  )
  set_tests_properties(${name} PROPERTIES LABELS ${ARG_LABEL} TIMEOUT ${ARG_TIMEOUT})
  if(NOT ARG_NO_DATA)
    set_tests_properties(${name} PROPERTIES FIXTURES_REQUIRED antb1_tpch)
  endif()
endfunction()

# The answers: all 22 queries through the in-process DuckDB oracle against DuckDB's stored answers, at each scale
# factor. They prove that the oracle reads the generated files as dbgen meant them (and, under ASan, exercise DuckDB's
# Parquet reader early).
foreach(sf IN ITEMS 0_01 0_1)
  set(_name harness.tpch.answers.sf${sf})
  antb1_slt_args(
    _args
    ${_name}
    TABLES "${CMAKE_CURRENT_SOURCE_DIR}/tables_sf${sf}.txt"
    --queries
    "${_tpch_dir}/queries"
    --answers
    "${_tpch_dir}/sf${sf}/answers"
  )
  _tpch_test(
    ${_name}
    LABEL harness
    REQUIRE "ANSWERS: PASS queries=22 passed=22 "
    COMMAND $<TARGET_FILE:antb1-slt> answers ${_args}
  )
endforeach()

# Random differential test over the tables at scale factor 0.01 (tests/slt/runner/difftest.h), and the same queries
# on 4 threads with batches of 1000 rows, equal to the 1-thread results byte for byte (ADR 0013). The seed is chosen
# so that DuckDB itself stays clean under ASan: DuckDB 1.5.5 leaks the state of a string MIN or MAX over many groups
# without ORDER BY, which the generated queries of a seed can contain. The queries also depend on the generator and on
# the supported features, so a change to either is checked under ASan too. A failure in antb1 is fixed, never avoided
# by another seed.
set(_seed 20261002)
foreach(kind IN ITEMS diff parallel)
  if(kind STREQUAL "diff")
    set(_name diff.tpch)
    set(_options "")
  else()
    set(_name parallel.diff.tpch)
    set(
      _options
      --threads
      4
      --same-as-threads
      1
      --batch-size
      1000
    )
  endif()
  antb1_slt_args(
    _args
    ${_name}
    TABLES "${CMAKE_CURRENT_SOURCE_DIR}/tables_sf0_01.txt"
    --seed
    ${_seed}
    --count
    300
    ${_options}
  )
  _tpch_test(${_name} LABEL ${kind} REQUIRE "DIFF: PASS " COMMAND $<TARGET_FILE:antb1-slt> diff ${_args})
endforeach()

# The ratchet (antb1-slt tpch, tests/slt/runner/ratchet.h): the 22 queries on antb1, and DuckDB for those antb1
# answers, against tests/data/tpch_status.json. A query passes only with DuckDB's answer (type names, DOUBLE within
# 1e-9, ORDER BY order with ties in any order); every other query must be Unsupported (exit code 4): a parse, bind,
# I/O or execution error fails the test, as do an unexpected pass and an unexpected fail. Both scale factors read
# the same ratchet, so a query passes at both or the test fails at one, and the parallel run (4 threads, batches of
# 1000 rows) must equal the 1-thread run byte for byte. Each query logs "Q<n>: running" first, so a timeout names
# it. ANTB1_TPCH_TIMES=1 adds each query's seconds (a local run; ctest and CI never set it).
set(_status "${PROJECT_SOURCE_DIR}/tests/data/tpch_status.json")
foreach(kind IN ITEMS sf0_01 sf0_1 parallel)
  if(kind STREQUAL "parallel")
    set(_name parallel.tpch.status.sf0_1)
    set(_sf 0_1)
    set(_label parallel)
    set(
      _options
      --threads
      4
      --same-as-threads
      1
      --batch-size
      1000
    )
  else()
    set(_name tpch.status.${kind})
    string(REPLACE "sf" "" _sf "${kind}")
    set(_label oracle)
    set(_options "")
  endif()
  antb1_slt_args(
    _args
    ${_name}
    TABLES "${CMAKE_CURRENT_SOURCE_DIR}/tables_sf${_sf}.txt"
    --queries
    "${_tpch_dir}/queries"
    --status
    "${_status}"
    --memory-limit
    2GiB
    ${_options}
  )
  _tpch_test(
    ${_name}
    LABEL ${_label}
    TIMEOUT 900
    REQUIRE "TPCH: PASS queries=22 "
    COMMAND $<TARGET_FILE:antb1-slt> tpch ${_args}
  )
endforeach()

# Redaction canary: with --mutate canary every query of the answers check fails, and the report prints neither the
# sentinel, nor query text, nor a sanitizer report.
set(_canary_regex "[aA][nN][tT][bB]1_[cC][aA][nN][aA][rR][yY]") # ANTB1_CANARY_*, antb1_canary_*
set(_name harness.tpch.answers.redact)
antb1_slt_args(
  _args
  ${_name}
  TABLES "${CMAKE_CURRENT_SOURCE_DIR}/tables_sf0_01.txt"
  --queries
  "${_tpch_dir}/queries"
  --answers
  "${_tpch_dir}/sf0_01/answers"
  --mutate
  canary
)
_tpch_test(
  ${_name}
  LABEL harness
  EXIT_CODE 1
  REQUIRE "FAIL Q1: " "sha256: answer [0-9a-f]+" "ANSWERS: FAIL queries=22 passed=0 "
  FORBID "${_canary_regex}" "SUMMARY:"
  COMMAND $<TARGET_FILE:antb1-slt> answers ${_args}
)

# Drift self-test: query texts that differ from the committed digests fail fixtures.tpch, and the report names the
# queries and their digests only.
set(_wrong "${CMAKE_CURRENT_BINARY_DIR}/drift-selftest.sha256")
set(_lines "")
string(REPEAT "0" 64 _zeros)
foreach(n RANGE 1 22)
  if(n LESS 10)
    set(n "0${n}")
  endif()
  string(APPEND _lines "${_zeros}  q${n}.sql\n")
endforeach()
file(WRITE "${_wrong}" "# A wrong digest for every query (harness.tpch.drift).\n${_lines}")
_tpch_test(
  harness.tpch.drift
  LABEL harness
  EXIT_CODE 1
  REQUIRE "q01\\.sql: sha256 [0-9a-f]+, committed 0+\n" "q22\\.sql: sha256 [0-9a-f]+, committed 0+\n"
  COMMAND
    ${CMAKE_COMMAND} "-DTPCH_DIR=${_tpch_dir}" "-DBINARY_DIR=${CMAKE_BINARY_DIR}" "-DDIGESTS=${_wrong}" -DCHECK_ONLY=ON
    -P "${_generate}"
)

# Self-tests of the failure paths, without data. When the duckdb CLI fails (here CMake stands in for it, which
# rejects the CLI's arguments), generate.cmake prints the exit code and the error kind only; ANTB1_TPCH_SHOW=1 shows
# the messages in a local run, never on GitHub Actions.
foreach(case IN ITEMS on_ci local)
  set(_name harness.tpch.generate.duckdb_fails.${case})
  set(_require "the duckdb CLI failed \\(exit code 1, CMake Error\\)")
  set(_forbid "")
  if(case STREQUAL "on_ci")
    set(_environment "ANTB1_TPCH_SHOW=set:1;GITHUB_ACTIONS=set:true")
    set(_forbid FORBID "Unknown argument")
  else()
    set(_environment "ANTB1_TPCH_SHOW=set:1;GITHUB_ACTIONS=unset:")
    list(APPEND _require "DuckDB's messages:\n.*Unknown argument")
  endif()
  antb1_add_expect_test(
    ${_name}
    LABELS harness
    EXIT_CODE 1
    REQUIRE ${_require} ${_forbid}
    COMMAND
      ${CMAKE_COMMAND}
      "-DDUCKDB=${CMAKE_COMMAND}"
      "-DEXTENSION=${ANTB1_TPCH_EXTENSION}"
      "-DTPCH_DIR=${CMAKE_CURRENT_BINARY_DIR}/${_name}"
      "-DBINARY_DIR=${CMAKE_BINARY_DIR}"
      "-DDIGESTS=${_wrong}"
      -P
      "${_generate}"
  )
  set_tests_properties(${_name} PROPERTIES ENVIRONMENT_MODIFICATION "${_environment}")
endforeach()

# run_redacted.cmake withholds output that matches a FORBID regex or looks like an unredacted report, prints only the
# SUMMARY lines of a sanitizer report (fake_sanitizer_report.cmake writes one with a sentinel), and fails a command
# that does not exit normally with exit code 125 after printing its output so far (fake_slow_query.cmake logs a
# query as running, then outlasts TIMEOUT). The outer expect test checks what reached the log; these outputs are our
# own sentinels, never data.
foreach(case IN ITEMS forbid unredacted sanitizer timeout)
  set(_name harness.tpch.run_redacted.${case})
  set(_inner "-DWORK_DIR=${CMAKE_BINARY_DIR}/slt-tmp/${_name}")
  set(_exit 1)
  if(case STREQUAL "forbid")
    list(APPEND _inner -DFORBID_COUNT=1 "-DFORBID_0=${_canary_regex}")
    set(_command ${CMAKE_COMMAND} -E echo ANTB1_CANARY_LEAK)
    set(_require "the output is withheld")
    set(_forbid "ANTB1_CANARY_LEAK")
  elseif(case STREQUAL "unredacted")
    set(
      _command
      ${CMAKE_COMMAND}
      -E
      echo
      SELECT
      42
    )
    set(_require "the output is withheld")
    set(_forbid "SELECT 42")
  elseif(case STREQUAL "timeout")
    list(APPEND _inner -DTIMEOUT=2)
    set(_command ${CMAKE_COMMAND} -P "${CMAKE_CURRENT_SOURCE_DIR}/fake_slow_query.cmake")
    set(_require "Q7: running\n" "the command did not exit normally")
    set(_forbid "Q7: unsupported")
    set(_exit 125)
  else()
    list(APPEND _inner -DEXIT_CODE=1)
    set(_command ${CMAKE_COMMAND} -P "${CMAKE_CURRENT_SOURCE_DIR}/fake_sanitizer_report.cmake")
    set(_require "SUMMARY: AddressSanitizer: antb1 fake report")
    set(_forbid "${_canary_regex}")
    set(_exit 0)
  endif()
  antb1_add_expect_test(
    ${_name}
    LABELS harness
    EXIT_CODE ${_exit}
    REQUIRE ${_require}
    FORBID ${_forbid}
    COMMAND ${CMAKE_COMMAND} ${_inner} -P "${CMAKE_CURRENT_SOURCE_DIR}/run_redacted.cmake" -- ${_command}
  )
endforeach()
