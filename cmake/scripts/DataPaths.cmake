# Where the ClickBench data tests find their data, resolved when the test runs (not at configure time), for
# VerifyData.cmake and RunDataTest.cmake:
#
#   ANTB1_DATA_DIR    the data directory of `pixi run fetch-data` (default ~/.cache/antb1/clickbench)
#   ANTB1_HITS_FILES  optional: the hits files the tests read instead of <data dir>/hits_0.parquet, as a file, a glob
#                     (in the file name) or a comma-separated list of them, e.g. the full dataset:
#                     ANTB1_HITS_FILES="$HOME/.cache/antb1/clickbench/full/hits_*.parquet" pixi run test-data
#
# Relative paths are relative to the repository root (SOURCE_DIR), as in scripts/fetch-data.sh. Paths must not
# contain whitespace or commas (the tables file of tests/slt/runner/tables.h separates files with them).
include_guard(GLOBAL)

# antb1_data_paths(): sets in the caller's scope
#   ANTB1_DATA_DIR_PATH  the data directory (absolute)
#   ANTB1_HITS_PATTERNS  the hits files or globs (absolute), for tables files
#   ANTB1_HITS_LIST      the hits files, globs expanded (sorted); empty if nothing matches
#   ANTB1_HITS_OVERRIDE  TRUE when ANTB1_HITS_FILES is set
function(antb1_data_paths)
  if(NOT DEFINED SOURCE_DIR OR NOT IS_ABSOLUTE "${SOURCE_DIR}")
    message(FATAL_ERROR "DataPaths.cmake: -DSOURCE_DIR=<absolute repository root> is required")
  endif()
  set(_dir "$ENV{ANTB1_DATA_DIR}")
  if(_dir STREQUAL "")
    if("$ENV{HOME}" STREQUAL "")
      message(FATAL_ERROR "DataPaths.cmake: neither ANTB1_DATA_DIR nor HOME is set")
    endif()
    set(_dir "$ENV{HOME}/.cache/antb1/clickbench")
  endif()
  cmake_path(ABSOLUTE_PATH _dir BASE_DIRECTORY "${SOURCE_DIR}" NORMALIZE)
  string(REGEX REPLACE "(.)/$" "\\1" _dir "${_dir}")

  set(_override FALSE)
  set(_raw "$ENV{ANTB1_HITS_FILES}")
  if(_raw STREQUAL "")
    set(_raw "${_dir}/hits_0.parquet")
  else()
    set(_override TRUE)
  endif()
  string(REPLACE "," ";" _entries "${_raw}")
  set(_patterns "")
  set(_files "")
  foreach(_entry IN LISTS _entries)
    string(STRIP "${_entry}" _entry)
    if(_entry STREQUAL "")
      continue()
    endif()
    if(_entry MATCHES "[ \t]")
      message(FATAL_ERROR "DataPaths.cmake: '${_entry}': paths with whitespace are not supported")
    endif()
    cmake_path(ABSOLUTE_PATH _entry BASE_DIRECTORY "${SOURCE_DIR}" NORMALIZE)
    list(APPEND _patterns "${_entry}")
    if(_entry MATCHES "[*?[]")
      file(GLOB _matches LIST_DIRECTORIES false "${_entry}")
      list(SORT _matches)
      list(APPEND _files ${_matches})
    elseif(EXISTS "${_entry}")
      list(APPEND _files "${_entry}")
    endif()
  endforeach()
  if(NOT _patterns)
    message(FATAL_ERROR "DataPaths.cmake: ANTB1_HITS_FILES='${_raw}' names no file")
  endif()

  set(ANTB1_DATA_DIR_PATH "${_dir}" PARENT_SCOPE)
  set(ANTB1_HITS_PATTERNS "${_patterns}" PARENT_SCOPE)
  set(ANTB1_HITS_LIST "${_files}" PARENT_SCOPE)
  set(ANTB1_HITS_OVERRIDE ${_override} PARENT_SCOPE)
endfunction()

# antb1_script_args(<out-var>): the arguments after `--` of this `cmake -P` run.
function(antb1_script_args out)
  set(_args "")
  set(_after FALSE)
  math(EXPR _last "${CMAKE_ARGC} - 1")
  foreach(_i RANGE 0 ${_last})
    if(_after)
      list(APPEND _args "${CMAKE_ARGV${_i}}")
    elseif(CMAKE_ARGV${_i} STREQUAL "--")
      set(_after TRUE)
    endif()
  endforeach()
  set(${out} "${_args}" PARENT_SCOPE)
endfunction()
