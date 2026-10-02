# The ctest fixtures.tpch (tests/tpch/CMakeLists.txt): generates the data derived from TPC-H, the 22 query texts and
# DuckDB's answers at test time with the pinned duckdb-extension-tpch package, which contains TPC's dbgen (ADR 0006).
# Nothing it writes is ever committed or printed: the output is numbers and digests only.
#
#   cmake -DDUCKDB=<duckdb CLI> -DEXTENSION=<tpch.duckdb_extension> -DTPCH_DIR=<build dir>/fixtures/tpch
#         -DBINARY_DIR=<build dir> -DDIGESTS=<tests/tpch/queries.sha256> [-DCHECK_ONLY=ON] -P generate.cmake
#
# TPCH_DIR (inside the build tree; anything else is refused):
#   generate.sql                  our own SQL that the duckdb CLI runs (no TPC text)
#   queries/q01.sql .. q22.sql    the query texts, in DuckDB's dialect
#   sf0_01/<table>.parquet        the eight tables at scale factor 0.01 (snappy, row groups of 2048 rows)
#   sf0_01/answers/q01.csv ..     DuckDB's answers at that scale factor (`antb1-slt answers` reads them)
#   sf0_1/...                     the same at scale factor 0.1
#
# The duckdb CLI starts with -unsigned (conda-forge cannot sign DuckDB extensions) and -no-init, runs on one thread
# with no extension auto-install or auto-load and no progress bar, and loads the extension by absolute path (DuckDB
# would search the home directory first). It generates both scale factors in memory, then locks itself down before it
# writes anything: allowed_directories (TPCH_DIR and its real path), enable_external_access = false, lock_configuration.
# Files are overwritten in place, so a second run gives the same bytes.
#
# Checks: every dumped text is byte-exact (its SHA256 equals the sha256 DuckDB computed of the text), and the query
# texts equal the committed digests: another text is another workload. With CHECK_ONLY the script runs no DuckDB and
# only compares the query texts in TPCH_DIR with DIGESTS (the drift self-test, harness.tpch.drift).
#
#   ANTB1_UPDATE_TPCH_DIGESTS=1  rewrite DIGESTS from the generated texts (after an update of the package)
#   ANTB1_TPCH_SHOW=1            print DuckDB's messages after a failure (local runs only; refused on GitHub Actions)
cmake_minimum_required(VERSION 3.28)

foreach(_var IN ITEMS TPCH_DIR BINARY_DIR DIGESTS)
  if(NOT DEFINED ${_var} OR NOT IS_ABSOLUTE "${${_var}}")
    message(FATAL_ERROR "generate.cmake: -D${_var}=<absolute path> is required")
  endif()
endforeach()
cmake_path(NORMAL_PATH TPCH_DIR)
cmake_path(NORMAL_PATH BINARY_DIR)
cmake_path(IS_PREFIX BINARY_DIR "${TPCH_DIR}" NORMALIZE _inside)
if(NOT _inside OR TPCH_DIR STREQUAL BINARY_DIR)
  message(FATAL_ERROR "generate.cmake: TPCH_DIR=${TPCH_DIR} is not inside the build tree ${BINARY_DIR}")
endif()

set(_scale_factors 0.01 0.1)
set(
  _tables
  customer
  lineitem
  nation
  orders
  part
  partsupp
  region
  supplier
)
set(
  _queries
  1
  2
  3
  4
  5
  6
  7
  8
  9
  10
  11
  12
  13
  14
  15
  16
  17
  18
  19
  20
  21
  22
)

function(_tpch_name out n)
  if(n LESS 10)
    set(${out} "q0${n}" PARENT_SCOPE)
  else()
    set(${out} "q${n}" PARENT_SCOPE)
  endif()
endfunction()

function(_tpch_dir out sf)
  string(REPLACE "." "_" _dir "sf${sf}")
  set(${out} "${_dir}" PARENT_SCOPE)
endfunction()

# The committed digests: "<sha256>  qNN.sql" lines; '#' starts a comment.
function(_tpch_read_digests out_names out_digests)
  set(_lines "")
  if(EXISTS "${DIGESTS}")
    file(STRINGS "${DIGESTS}" _lines)
  endif()
  set(_names "")
  set(_digests "")
  foreach(_line IN LISTS _lines)
    if(_line MATCHES "^#" OR _line STREQUAL "")
      continue()
    endif()
    string(LENGTH "${_line}" _length)
    if(NOT _line MATCHES "^([0-9a-f]+)  (q[0-9][0-9]\\.sql)$" OR NOT _length EQUAL 73)
      message(FATAL_ERROR "generate.cmake: ${DIGESTS}: malformed line (expected '<sha256>  qNN.sql')")
    endif()
    list(APPEND _names "${CMAKE_MATCH_2}")
    list(APPEND _digests "${CMAKE_MATCH_1}")
  endforeach()
  set(${out_names} "${_names}" PARENT_SCOPE)
  set(${out_digests} "${_digests}" PARENT_SCOPE)
endfunction()

# Compares the query texts in TPCH_DIR with the committed digests; FATAL_ERROR with numbers and digests only.
function(_tpch_check_drift)
  _tpch_read_digests(_names _expected)
  set(_problems "")
  set(_new "")
  foreach(_n IN LISTS _queries)
    _tpch_name(_q ${_n})
    set(_file "${TPCH_DIR}/queries/${_q}.sql")
    if(NOT EXISTS "${_file}")
      string(APPEND _problems "\n  ${_q}.sql: not generated (run the fixtures.tpch test)")
      continue()
    endif()
    file(SHA256 "${_file}" _actual)
    string(APPEND _new "${_actual}  ${_q}.sql\n")
    list(FIND _names "${_q}.sql" _i)
    if(_i LESS 0)
      string(APPEND _problems "\n  ${_q}.sql: no digest in ${DIGESTS}")
      continue()
    endif()
    list(GET _expected ${_i} _want)
    if(NOT _actual STREQUAL _want)
      string(APPEND _problems "\n  ${_q}.sql: sha256 ${_actual}, committed ${_want}")
    endif()
  endforeach()
  list(LENGTH _names _count)
  list(LENGTH _queries _query_count)
  if(NOT _count EQUAL _query_count)
    string(APPEND _problems "\n  ${DIGESTS} lists ${_count} texts, expected ${_query_count}")
  endif()
  if(_problems STREQUAL "")
    message("generate.cmake: the ${_query_count} query texts match ${DIGESTS}")
    return()
  endif()
  if("$ENV{ANTB1_UPDATE_TPCH_DIGESTS}" STREQUAL "1" AND NOT CHECK_ONLY)
    file(
      WRITE "${DIGESTS}"
      "# sha256 of the 22 query texts derived from TPC-H that the pinned duckdb-extension-tpch package returns\n"
      "# (tpch_queries(), written by tests/tpch/generate.cmake). The texts themselves are never committed (ADR 0006).\n"
      "# After an update of the package, review the change and rewrite this file:\n"
      "#   ANTB1_UPDATE_TPCH_DIGESTS=1 pixi run test -R '^fixtures\\.tpch$'\n"
      "${_new}"
    )
    message("generate.cmake: rewrote ${DIGESTS}; review the diff")
    return()
  endif()
  message(
    FATAL_ERROR
    "generate.cmake: the query texts differ from the committed digests (another package version is another "
    "workload):${_problems}\nAfter an intended update of duckdb-extension-tpch, rewrite the digests and review the "
    "diff: ANTB1_UPDATE_TPCH_DIGESTS=1 pixi run test -R '^fixtures\\.tpch$'"
  )
endfunction()

if(CHECK_ONLY)
  _tpch_check_drift()
  return()
endif()

foreach(_var IN ITEMS DUCKDB EXTENSION)
  if(NOT DEFINED ${_var} OR NOT EXISTS "${${_var}}")
    message(FATAL_ERROR "generate.cmake: -D${_var}=<existing file> is required")
  endif()
endforeach()

file(MAKE_DIRECTORY "${TPCH_DIR}/queries" "${TPCH_DIR}/tmp")
file(REAL_PATH "${TPCH_DIR}" _real_out)

# DuckDB string literals: no quote may end one early (paths come from the build tree).
foreach(_path IN ITEMS "${TPCH_DIR}" "${_real_out}" "${EXTENSION}")
  if(_path MATCHES "'")
    message(FATAL_ERROR "generate.cmake: a path with a single quote is not supported: ${_path}")
  endif()
endforeach()

set(_sql "-- Written by tests/tpch/generate.cmake: our own SQL, run by the duckdb CLI of the pinned package.\n")
string(
  APPEND _sql
  "SET threads = 1;\n"
  "SET autoinstall_known_extensions = false;\n"
  "SET autoload_known_extensions = false;\n"
  "SET enable_progress_bar = false;\n"
  "SET temp_directory = '${TPCH_DIR}/tmp';\n"
  "LOAD '${EXTENSION}';\n"
)
foreach(_sf IN LISTS _scale_factors)
  _tpch_dir(_dir ${_sf})
  file(MAKE_DIRECTORY "${TPCH_DIR}/${_dir}/answers")
  string(APPEND _sql "CREATE SCHEMA ${_dir};\n" "CALL dbgen(sf = ${_sf}, schema = '${_dir}');\n")
endforeach()
string(
  APPEND _sql
  "SET allowed_directories = ['${TPCH_DIR}/', '${_real_out}/'];\n"
  "SET enable_external_access = false;\n"
  "SET lock_configuration = true;\n"
)
# A text column dumped as CSV without quoting, minus its final line break, which COPY writes back: the file is the
# text byte for byte (checked below).
set(_text_options "FORMAT csv, HEADER false, QUOTE ''")
foreach(_sf IN LISTS _scale_factors)
  _tpch_dir(_dir ${_sf})
  foreach(_table IN LISTS _tables)
    string(
      APPEND _sql
      "COPY ${_dir}.${_table} TO '${TPCH_DIR}/${_dir}/${_table}.parquet' "
      "(FORMAT parquet, COMPRESSION snappy, ROW_GROUP_SIZE 2048);\n"
    )
  endforeach()
  foreach(_n IN LISTS _queries)
    _tpch_name(_q ${_n})
    string(
      APPEND _sql
      "COPY (SELECT answer[1:-2] FROM tpch_answers() WHERE query_nr = ${_n} AND scale_factor = ${_sf}) "
      "TO '${TPCH_DIR}/${_dir}/answers/${_q}.csv' (${_text_options});\n"
    )
  endforeach()
endforeach()
foreach(_n IN LISTS _queries)
  _tpch_name(_q ${_n})
  string(
    APPEND _sql
    "COPY (SELECT query[1:-2] FROM tpch_queries() WHERE query_nr = ${_n}) TO '${TPCH_DIR}/queries/${_q}.sql' "
    "(${_text_options});\n"
  )
endforeach()
# The digests of the texts as DuckDB has them, one "<file>|<sha256>" line each.
string(
  APPEND _sql
  "SELECT 'queries/q' || lpad(query_nr::VARCHAR, 2, '0') || '.sql', sha256(query) FROM tpch_queries() "
  "ORDER BY query_nr;\n"
)
foreach(_sf IN LISTS _scale_factors)
  _tpch_dir(_dir ${_sf})
  string(
    APPEND _sql
    "SELECT '${_dir}/answers/q' || lpad(query_nr::VARCHAR, 2, '0') || '.csv', sha256(answer) FROM tpch_answers() "
    "WHERE scale_factor = ${_sf} ORDER BY query_nr;\n"
  )
endforeach()
file(WRITE "${TPCH_DIR}/generate.sql" "${_sql}")

execute_process(
  COMMAND "${DUCKDB}" -unsigned -no-init -batch -noheader -list -bail -f "${TPCH_DIR}/generate.sql"
  RESULT_VARIABLE _rc
  OUTPUT_VARIABLE _out
  ERROR_VARIABLE _err
)
if(NOT _rc STREQUAL "0")
  set(_kind "unknown")
  if(_err MATCHES "([A-Za-z ]+ Error):")
    set(_kind "${CMAKE_MATCH_1}")
  endif()
  set(_show "")
  if("$ENV{ANTB1_TPCH_SHOW}" STREQUAL "1" AND NOT "$ENV{GITHUB_ACTIONS}" STREQUAL "true")
    set(_show "\nDuckDB's messages:\n${_err}")
  endif()
  message(
    FATAL_ERROR
    "generate.cmake: the duckdb CLI failed (exit code ${_rc}, ${_kind}); its messages are not printed because they "
    "can quote the workload. Rerun locally with ANTB1_TPCH_SHOW=1 pixi run test -R '^fixtures\\.tpch$'${_show}"
  )
endif()

# DuckDB's digests against the files: every dump is byte-exact.
string(REPLACE "\n" ";" _lines "${_out}")
set(_problems "")
set(_checked 0)
foreach(_line IN LISTS _lines)
  if(_line STREQUAL "")
    continue()
  endif()
  if(NOT _line MATCHES "^((queries|sf0_01/answers|sf0_1/answers)/q[0-9][0-9]\\.(sql|csv))\\|([0-9a-f]+)$")
    string(APPEND _problems "\n  an unexpected line in DuckDB's output")
    continue()
  endif()
  set(_file "${CMAKE_MATCH_1}")
  set(_want "${CMAKE_MATCH_4}")
  if(NOT EXISTS "${TPCH_DIR}/${_file}")
    string(APPEND _problems "\n  ${_file}: not written")
    continue()
  endif()
  file(SHA256 "${TPCH_DIR}/${_file}" _actual)
  if(NOT _actual STREQUAL _want)
    string(APPEND _problems "\n  ${_file}: sha256 ${_actual}, DuckDB's text ${_want}")
  endif()
  math(EXPR _checked "${_checked} + 1")
endforeach()
list(LENGTH _queries _query_count)
list(LENGTH _scale_factors _sf_count)
math(EXPR _expected "${_query_count} * (${_sf_count} + 1)")
if(NOT _checked EQUAL _expected)
  string(APPEND _problems "\n  ${_checked} texts checked, expected ${_expected}")
endif()
if(NOT _problems STREQUAL "")
  message(FATAL_ERROR "generate.cmake: the dumps are not DuckDB's texts byte for byte:${_problems}")
endif()
message("generate.cmake: ${_checked} texts written byte for byte, ${_sf_count} scale factors of 8 tables")
_tpch_check_drift()
