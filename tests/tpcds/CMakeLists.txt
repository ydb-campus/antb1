# Tests over data derived from TPC-DS (ADR 0006). The data and the query texts are generated at test time from the
# pinned duckdb-extension-tpcds package; nothing derived from TPC-DS is committed.
if(NOT ANTB1_HAVE_TPCDS)
  return()
endif()

# The pinned extension loads into the duckdb CLI of the same prefix by absolute path (unsigned: conda-forge cannot
# sign DuckDB extensions) and provides the 99 query texts. DuckDB runs as everywhere in the tests: one thread, no
# extension auto-install or auto-load. The output must be exactly the count. No semicolons in the SQL: CMake would
# split the argument there.
antb1_add_expect_test(
  harness.tpcds.extension
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
    "LOAD '${ANTB1_TPCDS_EXTENSION}'"
    -c
    "SELECT count(*) FROM tpcds_queries()"
  EXIT_CODE 0
  REQUIRE "^99[^0-9]?$"
  LABELS harness
)

# fixtures.tpcds (label setup, fixture antb1_tpcds) generates the data at scale factors 0.1 and 1 and the 99 query
# texts into ${ANTB1_FIXTURES_DIR}/tpcds with tests/tpch/generate.cmake, checks that every dump is DuckDB's text byte
# for byte and that the query texts match queries.sha256. It prints numbers and digests only. There are no stored
# answers to dump: tpcds_answers() fails on every scan in the pinned version (ADR 0006). Scale factor 1 because at 0.1
# 18 of the 99 queries return no rows in DuckDB (1 at scale factor 1), so a pass there would show little.
set(_tpcds_dir "${ANTB1_FIXTURES_DIR}/tpcds")
set(_generate "${PROJECT_SOURCE_DIR}/tests/tpch/generate.cmake")
set(_digests "${CMAKE_CURRENT_SOURCE_DIR}/queries.sha256")
add_test(
  NAME fixtures.tpcds
  COMMAND
    ${CMAKE_COMMAND} "-DDUCKDB=${ANTB1_DUCKDB_CLI}" "-DEXTENSION=${ANTB1_TPCDS_EXTENSION}" "-DWORKLOAD=tpcds"
    "-DDIR=${_tpcds_dir}" "-DBINARY_DIR=${CMAKE_BINARY_DIR}" "-DDIGESTS=${_digests}" -P "${_generate}"
)
# One TPC generator at a time (fixtures.tpch too): dsdgen at scale factor 1 takes about 2.4 GB in memory, next to up to
# 7 other tests.
set_tests_properties(
  fixtures.tpcds
  PROPERTIES LABELS setup FIXTURES_SETUP antb1_tpcds TIMEOUT 600 RESOURCE_LOCK antb1_tpc_generate
)

# Drift self-test: query texts that differ from the committed digests fail fixtures.tpcds, and the report names the
# queries and their digests only.
set(_wrong "${CMAKE_CURRENT_BINARY_DIR}/drift-selftest.sha256")
set(_lines "")
string(REPEAT "0" 64 _zeros)
foreach(n RANGE 1 99)
  if(n LESS 10)
    set(n "0${n}")
  endif()
  string(APPEND _lines "${_zeros}  q${n}.sql\n")
endforeach()
file(WRITE "${_wrong}" "# A wrong digest for every query (harness.tpcds.drift).\n${_lines}")
antb1_add_expect_test(
  harness.tpcds.drift
  COMMAND
    ${CMAKE_COMMAND}
    "-DWORKLOAD=tpcds"
    "-DDIR=${_tpcds_dir}"
    "-DBINARY_DIR=${CMAKE_BINARY_DIR}"
    "-DDIGESTS=${_wrong}"
    -DCHECK_ONLY=ON
    -P
    "${_generate}"
  EXIT_CODE 1
  REQUIRE "q01\\.sql: sha256 [0-9a-f]+, committed 0+\n" "q99\\.sql: sha256 [0-9a-f]+, committed 0+\n"
  LABELS harness
)
set_tests_properties(harness.tpcds.drift PROPERTIES FIXTURES_REQUIRED antb1_tpcds)

# Self-tests of the failure paths, without data, as for TPC-H (tests/tpch/CMakeLists.txt): a failing duckdb CLI
# (CMake stands in for it) prints its exit code and error kind only; ANTB1_TPCDS_SHOW=1 shows its messages in a local
# run, never on GitHub Actions.
foreach(case IN ITEMS on_ci local)
  set(_name harness.tpcds.generate.duckdb_fails.${case})
  set(_require "the duckdb CLI failed \\(exit code 1, CMake Error\\)")
  set(_forbid "")
  if(case STREQUAL "on_ci")
    set(_environment "ANTB1_TPCDS_SHOW=set:1;GITHUB_ACTIONS=set:true")
    set(_forbid FORBID "Unknown argument")
  else()
    set(_environment "ANTB1_TPCDS_SHOW=set:1;GITHUB_ACTIONS=unset:")
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
      "-DEXTENSION=${ANTB1_TPCDS_EXTENSION}"
      "-DWORKLOAD=tpcds"
      "-DDIR=${CMAKE_CURRENT_BINARY_DIR}/${_name}"
      "-DBINARY_DIR=${CMAKE_BINARY_DIR}"
      "-DDIGESTS=${_wrong}"
      -P
      "${_generate}"
  )
  set_tests_properties(${_name} PROPERTIES ENVIRONMENT_MODIFICATION "${_environment}")
endforeach()

# The generator refuses to run without a known workload.
antb1_add_expect_test(
  harness.tpcds.generate.unknown_workload
  LABELS harness
  EXIT_CODE 1
  REQUIRE "-DWORKLOAD=tpch or -DWORKLOAD=tpcds is required"
  COMMAND ${CMAKE_COMMAND} "-DWORKLOAD=tpcx" -P "${_generate}"
)

# The tests that read the generated data run through tests/tpch/run_redacted.cmake, as the TPC-H ones do: their tables
# files mark every table `redact`, so antb1-slt prints no value and no query text; sanitizer reports go to files; and
# output that looks unredacted (or matches a FORBID regex) is withheld before it reaches the log.
#   _tpcds_test(<name> LABEL <label> [TIMEOUT <seconds>] [REQUIRE <regex>...] COMMAND <command>...)
function(_tpcds_test name)
  cmake_parse_arguments(ARG "" "LABEL;TIMEOUT" "REQUIRE;COMMAND" ${ARGN})
  if(NOT DEFINED ARG_TIMEOUT)
    set(ARG_TIMEOUT 300)
  endif()
  math(EXPR _inner_timeout "${ARG_TIMEOUT} - 30")
  set(_defs "-DWORK_DIR=${CMAKE_BINARY_DIR}/slt-tmp/${name}" "-DTIMEOUT=${_inner_timeout}")
  set(_i 0)
  foreach(regex IN LISTS ARG_REQUIRE)
    list(APPEND _defs "-DREQUIRE_${_i}=${regex}")
    math(EXPR _i "${_i} + 1")
  endforeach()
  list(APPEND _defs "-DREQUIRE_COUNT=${_i}" "-DFORBID_COUNT=0")
  add_test(
    NAME ${name}
    COMMAND ${CMAKE_COMMAND} ${_defs} -P "${PROJECT_SOURCE_DIR}/tests/tpch/run_redacted.cmake" -- ${ARG_COMMAND}
  )
  set_tests_properties(
    ${name}
    PROPERTIES LABELS ${ARG_LABEL} TIMEOUT ${ARG_TIMEOUT} FIXTURES_REQUIRED antb1_tpcds
  )
endfunction()

# The ratchet (antb1-slt tpcds, tests/slt/runner/ratchet.h): the 99 queries on antb1, and DuckDB for those antb1
# answers, against tests/data/tpcds_status.json, by the rules of the TPC-H ratchet (tests/tpch/CMakeLists.txt): a
# query passes only with DuckDB's answer; every other query must be Unsupported (exit code 4). Both scale factors read
# the same ratchet, and the parallel run (4 threads, batches of 1000 rows) must equal the 1-thread run byte for byte.
# ANTB1_TPCDS_TIMES=1 in the environment of a local run adds each query's seconds.
set(_status "${PROJECT_SOURCE_DIR}/tests/data/tpcds_status.json")
foreach(kind IN ITEMS sf0_1 sf1 parallel)
  if(kind STREQUAL "parallel")
    set(_name parallel.tpcds.status.sf1)
    set(_sf 1)
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
    set(_name tpcds.status.${kind})
    string(REPLACE "sf" "" _sf "${kind}")
    set(_label oracle)
    set(_options "")
  endif()
  antb1_slt_args(
    _args
    ${_name}
    TABLES "${CMAKE_CURRENT_SOURCE_DIR}/tables_sf${_sf}.txt"
    --queries
    "${_tpcds_dir}/queries"
    --status
    "${_status}"
    --memory-limit
    2GiB
    ${_options}
  )
  _tpcds_test(
    ${_name}
    LABEL ${_label}
    TIMEOUT 900
    REQUIRE "TPCDS: PASS queries=99 "
    COMMAND $<TARGET_FILE:antb1-slt> tpcds ${_args}
  )
endforeach()
