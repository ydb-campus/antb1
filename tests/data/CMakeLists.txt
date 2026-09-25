# ClickBench data tests (label data): `pixi run test-data` (test preset `data` on the ci-release build), CI job
# clickbench-hits0, nightly asan-data and arm64. They read what `pixi run fetch-data` downloads
# (tools/data/clickbench.lock), resolved when the tests run: $ANTB1_DATA_DIR (default ~/.cache/antb1/clickbench)
# and, for other hits files such as the full dataset, ANTB1_HITS_FILES (cmake/scripts/DataPaths.cmake). The
# hermetic test presets exclude the label.
#
#   data.hits0.verify       the pinned files are there with the pinned size and sha256 (setup of antb1_hits0)
#   data.hits0.schema       the hits files have the physical schema that tools/fixturegen models
#   data.hits0.slice        hits0_slice.sql, our own queries, on antb1 vs DuckDB
#   data.hits0.metamorphic  hits_relations.cc: relations between antb1 answers, and COUNT(*) vs a Parquet scan
#   data.clickbench.status  ClickBench's queries on antb1 (DuckDB for those antb1 answers) vs the ratchet
#                           clickbench_status.json
#
# Data policy (docs/adr/0006-test-strategy-and-data-policy.md): every data test runs with --redact, so no value and
# no ClickBench query text reaches a log (tools/lint/check_repo.py R010 checks every add_test here); a failure
# prints the unredacted command to run locally.

add_executable(antb1-data-metamorphic data_metamorphic.cc hits_relations.cc)
target_link_libraries(
  antb1-data-metamorphic
  PRIVATE antb1_metamorphic_lib antb1_slt_lib Parquet::parquet_shared CLI11::CLI11 antb1::build_options
)

set(_verify "${PROJECT_SOURCE_DIR}/cmake/scripts/VerifyData.cmake")
set(_run "${PROJECT_SOURCE_DIR}/cmake/scripts/RunDataTest.cmake")
set(_work "${CMAKE_BINARY_DIR}/data-tmp")
set(_src "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}")

# The setup test of the ctest fixture antb1_hits0: the other data tests only run on verified data.
add_test(
  NAME data.hits0.verify
  COMMAND
    ${CMAKE_COMMAND} "-DLOCK=${PROJECT_SOURCE_DIR}/tools/data/clickbench.lock" "${_src}" -P "${_verify}" -- --redact
)
set_tests_properties(
  data.hits0.verify
  PROPERTIES LABELS data FIXTURES_SETUP antb1_hits0 TIMEOUT 600
)

add_test(
  NAME data.hits0.schema
  COMMAND
    ${CMAKE_COMMAND} "${_src}" "-DWORK_DIR=${_work}/data.hits0.schema" -P "${_run}" -- $<TARGET_FILE:antb1-fixturegen>
    --check-schema --redact {HITS_FILES}
)
add_test(
  NAME data.hits0.metamorphic
  COMMAND
    ${CMAKE_COMMAND} "${_src}" "-DWORK_DIR=${_work}/data.hits0.metamorphic" -P "${_run}" --
    $<TARGET_FILE:antb1-data-metamorphic> --tables {HITS_TABLES} --redact
)
set(_data_tests data.hits0.schema data.hits0.metamorphic)

if(ANTB1_HAVE_DUCKDB)
  add_test(
    NAME data.hits0.slice
    COMMAND
      ${CMAKE_COMMAND} "${_src}" "-DWORK_DIR=${_work}/data.hits0.slice" -P "${_run}" -- $<TARGET_FILE:antb1-slt> queries
      --fixtures "${ANTB1_FIXTURES_DIR}" --tables {HITS_TABLES} --temp-dir "${_work}/data.hits0.slice/duckdb" --redact
      "${CMAKE_CURRENT_SOURCE_DIR}/hits0_slice.sql"
  )
  add_test(
    NAME data.clickbench.status
    COMMAND
      ${CMAKE_COMMAND} "${_src}" "-DWORK_DIR=${_work}/data.clickbench.status" -P "${_run}" -- $<TARGET_FILE:antb1-slt>
      clickbench --fixtures "${ANTB1_FIXTURES_DIR}" --tables {HITS_TABLES} --temp-dir
      "${_work}/data.clickbench.status/duckdb" --status "${CMAKE_CURRENT_SOURCE_DIR}/clickbench_status.json" --redact
      {QUERIES}
  )
  list(APPEND _data_tests data.hits0.slice data.clickbench.status)
endif()

# Generous timeouts: host runs on the full dataset (ANTB1_HITS_FILES) and ASan builds are slow.
set_tests_properties(
  ${_data_tests}
  PROPERTIES LABELS data FIXTURES_REQUIRED "antb1_hits0;antb1_fixtures" TIMEOUT 1800
)
