# CLI golden tests (label cli): the antb1 binary's stdout, stderr and exit code (tests/README.md).
# Update the golden files after an intended change: ANTB1_UPDATE_GOLDENS=1 pixi run test -L cli, then review.
set(_f "${ANTB1_FIXTURES_DIR}")
set(_sql "${CMAKE_CURRENT_SOURCE_DIR}/sql")
set(_t "t=${_f}/hits_like.parquet")
set(_count "SELECT COUNT(*) FROM t")

# ---- output formats ----
antb1_add_cli_golden(
  query_table
  ARGS query -c "${_count}" --table "${_t}"
)
antb1_add_cli_golden(
  query_csv
  ARGS query -c "${_count}" --table "${_t}" --format csv
)
antb1_add_cli_golden(
  query_json
  ARGS query -c "${_count}" --table "${_t}" --format json
)
antb1_add_cli_golden(
  query_empty_table
  ARGS query -c "${_count}" --table "t=${_f}/empty.parquet" --format csv
)

# ---- SQL sources and table forms ----
antb1_add_cli_golden(
  query_file
  ARGS query -f "${_sql}/count_star.sql" --table "${_t}" --format csv
)
antb1_add_cli_golden(
  query_stdin
  INPUT "${_sql}/count_star.sql"
  ARGS query -c - --table "${_t}" --format csv
)
antb1_add_cli_golden(
  query_table_glob
  ARGS query -c "${_count}" --table "t=${_f}/hits_like_split/part-*.parquet" --format csv
)
antb1_add_cli_golden(
  query_table_list
  ARGS
    query
    -c
    "${_count}"
    --table
    "t=${_f}/hits_like_split/part-0.parquet,${_f}/hits_like_split/part-3.parquet"
    --format
    csv
)
antb1_add_cli_golden(
  query_from_path
  ARGS query -c "SELECT COUNT(*) FROM '${_f}/hits_like_split/part-[12].parquet'" --format csv
)
antb1_add_cli_golden(
  query_timing
  ARGS query -c "${_count}" --table "${_t}" --format csv --timing
  STDERR_REGEX "^[0-9]+[.][0-9][0-9][0-9][0-9][0-9][0-9]$"
)

# ---- errors and exit codes: 1 query error, 2 usage, 3 I/O, 4 unsupported ----
antb1_add_cli_golden(
  error_parse
  EXIT_CODE 1
  STDERR
  ARGS query -c "SELECT COUNT(*) FROM" --table "${_t}"
)
antb1_add_cli_golden(
  error_bind
  EXIT_CODE 1
  STDERR
  ARGS query -c "SELECT COUNT(*) FROM nope" --table "${_t}"
)
antb1_add_cli_golden(
  error_bind_json
  EXIT_CODE 1
  STDERR
  ARGS query -c "SELECT COUNT(*)\nFROM nope" --table "${_t}" --format json
)
antb1_add_cli_golden(
  unsupported
  EXIT_CODE 4
  STDERR
  ARGS query -c "SELECT COUNT(*) FROM t GROUP BY CounterID" --table "${_t}"
)
antb1_add_cli_golden(
  unsupported_json
  EXIT_CODE 4
  STDERR
  ARGS query -c "SELECT SUM(AdvEngineID) FROM t" --table "${_t}" --format json
)
antb1_add_cli_golden(
  unsupported_explain
  EXIT_CODE 4
  STDERR
  ARGS explain -c "SELECT * FROM t ORDER BY WatchID LIMIT 10" --table "${_t}"
)
antb1_add_cli_golden(
  unsupported_where
  EXIT_CODE 4
  STDERR
  ARGS query -c "SELECT COUNT(*) FROM t\nWHERE IsMobile = 1" --table "${_t}"
)
antb1_add_cli_golden(
  error_bind_column
  EXIT_CODE 1
  STDERR
  ARGS query -c "SELECT COUNT(*) FROM t WHERE nope = 1" --table "${_t}"
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
  ARGS query -c "SELECT COUNT(*) FROM t WHERE CounterID = 'x'" --table "${_t}" --format json
)
antb1_add_cli_golden(usage_no_command EXIT_CODE 2 STDERR_REGEX "subcommand")
antb1_add_cli_golden(
  usage_bad_format
  EXIT_CODE 2
  STDERR_REGEX "--format"
  ARGS query -c "${_count}" --format xml
)
antb1_add_cli_golden(usage_no_sql EXIT_CODE 2 STDERR_REGEX "pass exactly one of" ARGS query --table "${_t}")
antb1_add_cli_golden(
  usage_two_sql_sources
  EXIT_CODE 2
  STDERR_REGEX "pass exactly one of"
  ARGS query -c "${_count}" -f "${_sql}/count_star.sql" --table "${_t}"
)
antb1_add_cli_golden(
  usage_bad_table_spec
  EXIT_CODE 2
  STDERR_REGEX "--table expects NAME=PATH"
  ARGS query -c "${_count}" --table t
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
  ARGS query -c "${_count}" --table "t=${_f}/no_such.parquet"
)
antb1_add_cli_golden(
  io_missing_table_file_json
  EXIT_CODE 3
  STDERR_REGEX "^[{]\"error\":[{]\"kind\":\"io\",\"message\":\".*no_such[.]parquet.*\"[}][}]$"
  ARGS query -c "${_count}" --table "t=${_f}/no_such.parquet" --format json
)
antb1_add_cli_golden(
  io_missing_from_path
  EXIT_CODE 3
  STDERR_REGEX "^antb1: io error: .*[$][{]FIXTURES[}]/no_such[.]parquet"
  ARGS query -c "SELECT COUNT(*) FROM '${_f}/no_such.parquet'"
)
antb1_add_cli_golden(
  io_not_parquet
  EXIT_CODE 3
  STDERR_REGEX "^antb1: io error: cannot read Parquet file '[$][{]SOURCE[}]/tests/cli/sql/count_star[.]sql'"
  ARGS query -c "${_count}" --table "t=${_sql}/count_star.sql"
)
antb1_add_cli_golden(
  io_no_glob_match
  EXIT_CODE 3
  STDERR
  ARGS query -c "${_count}" --table "t=${_f}/hits_like_split/none-*.parquet"
)
antb1_add_cli_golden(
  io_schema_mismatch
  EXIT_CODE 3
  STDERR
  ARGS query -c "${_count}" --table "t=${_f}/hits_like.parquet,${_f}/edge.parquet"
)
antb1_add_cli_golden(
  io_missing_sql_file
  EXIT_CODE 3
  STDERR
  ARGS query -f "${_f}/no_such.sql" --table "${_t}"
)

# ---- schema, explain, version ----
antb1_add_cli_golden(schema_clickbench ARGS schema --clickbench --table "${_t}")
antb1_add_cli_golden(schema_edge ARGS schema --table "edge=${_f}/edge.parquet")
antb1_add_cli_golden(
  schema_column_type
  ARGS schema --column-type u16=DATE --table "edge=${_f}/edge.parquet"
)
antb1_add_cli_golden(
  explain
  ARGS explain -c "${_count}" --table "t=${_f}/hits_like_split/part-*.parquet"
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
  explain_projection
  ARGS explain -c "SELECT URL, \"title\" AS t FROM t WHERE SearchPhrase <> '' LIMIT 10" --table "${_t}"
)
antb1_add_cli_golden(
  version
  STDOUT_REGEX "^antb1 [0-9]+[.][0-9]+[.][0-9]+[^ ]* [(]Apache Arrow [0-9]+[.][0-9]+[.][0-9]+[)]$"
  ARGS version
)

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
