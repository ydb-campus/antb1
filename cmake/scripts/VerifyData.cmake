# ctest data.hits0.verify (label data), the setup test of the ctest fixture antb1_hits0: every file of
# tools/data/clickbench.lock is in the data directory with its pinned size and sha256, so the other data tests
# (FIXTURES_REQUIRED antb1_hits0) only ever read verified data.
#
# cmake -DLOCK=<tools/data/clickbench.lock> -DSOURCE_DIR=<repository root> -P VerifyData.cmake -- --redact
#
# The data directory comes from the environment at test time (DataPaths.cmake). With ANTB1_HITS_FILES set (a host
# run on other hits files, e.g. the full dataset from `pixi run fetch-data --full`, which verifies them) the pinned
# hits file is skipped with a warning, and the override must match at least one file. The report holds only file
# names, sizes and hashes, never data: `--redact`, which every data test gets, changes nothing here.
cmake_minimum_required(VERSION 3.28)
include("${CMAKE_CURRENT_LIST_DIR}/DataPaths.cmake")

antb1_script_args(_args)
if(NOT _args STREQUAL "--redact")
  message(FATAL_ERROR "VerifyData.cmake: expected the arguments `-- --redact`, got '${_args}'")
endif()
if(NOT DEFINED LOCK OR NOT EXISTS "${LOCK}")
  message(FATAL_ERROR "VerifyData.cmake: -DLOCK=<tools/data/clickbench.lock> is required")
endif()
antb1_data_paths()

set(_fix "run `pixi run fetch-data` (it downloads the pinned files into ${ANTB1_DATA_DIR_PATH})")
set(_problems "")
set(_verified 0)
file(STRINGS "${LOCK}" _lines)
foreach(_line IN LISTS _lines)
  string(STRIP "${_line}" _line)
  if(_line STREQUAL "" OR _line MATCHES "^#")
    continue()
  endif()
  if(NOT _line MATCHES "^([^ /]+) +([0-9a-f]+) +([0-9]+) +(https://[^ ]+)$")
    string(APPEND _problems "\n  ${LOCK}: malformed line (expected: <name> <sha256> <bytes> <url>)")
    continue()
  endif()
  set(_name "${CMAKE_MATCH_1}")
  set(_sha "${CMAKE_MATCH_2}")
  set(_bytes "${CMAKE_MATCH_3}")
  if(ANTB1_HITS_OVERRIDE AND _name MATCHES "^hits_[0-9]+\\.parquet$")
    list(LENGTH ANTB1_HITS_LIST _n)
    message(
      WARNING
      "ANTB1_HITS_FILES is set: the data tests read ${_n} other hits file(s), which are not verified here "
      "(`pixi run fetch-data --full` verifies the full dataset); the pinned ${_name} is not checked."
    )
    continue()
  endif()
  set(_path "${ANTB1_DATA_DIR_PATH}/${_name}")
  if(NOT EXISTS "${_path}")
    string(APPEND _problems "\n  ${_name}: missing from ${ANTB1_DATA_DIR_PATH}")
    continue()
  endif()
  file(SIZE "${_path}" _size)
  if(NOT _size EQUAL _bytes)
    string(APPEND _problems "\n  ${_name}: ${_size} bytes, the pin says ${_bytes}")
    continue()
  endif()
  file(SHA256 "${_path}" _actual)
  if(NOT _actual STREQUAL _sha)
    string(APPEND _problems "\n  ${_name}: sha256 ${_actual}, the pin says ${_sha}")
    continue()
  endif()
  message("data.hits0.verify: ${_name}: ${_size} bytes, sha256 ${_actual} (pinned)")
  math(EXPR _verified "${_verified} + 1")
endforeach()

if(ANTB1_HITS_OVERRIDE AND NOT ANTB1_HITS_LIST)
  string(APPEND _problems "\n  ANTB1_HITS_FILES='$ENV{ANTB1_HITS_FILES}' matches no file")
endif()
if(_verified EQUAL 0 AND NOT _problems)
  string(APPEND _problems "\n  ${LOCK} pins no file")
endif()
if(_problems)
  message(FATAL_ERROR "data.hits0.verify: the ClickBench data is not ready:${_problems}\n  Fix: ${_fix}")
endif()
message("data.hits0.verify: OK (${ANTB1_DATA_DIR_PATH})")
