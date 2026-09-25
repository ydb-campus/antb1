# Runs one ClickBench data test (tests/data/CMakeLists.txt) on the data resolved at test time (DataPaths.cmake):
#
# cmake -DSOURCE_DIR=<repository root> -DWORK_DIR=<dir in the build tree> -P RunDataTest.cmake -- <command> [args...]
#
# These arguments of the command are replaced first:
#   {HITS_FILES}   one argument per hits file (globs expanded)
#   {HITS_TABLES}  a tables file (tests/slt/runner/tables.h) written to WORK_DIR with the one table the queries read:
#                  `hits <files> clickbench` (EventDate as DATE on both engines, as ClickBench's DuckDB setup does)
#   {QUERIES}      <data dir>/queries.sql: ClickBench's query file, fetched by `pixi run fetch-data`, never committed
# The command's output passes through unchanged (the commands redact it themselves); the test fails unless the
# command exits with 0.
cmake_minimum_required(VERSION 3.28)
include("${CMAKE_CURRENT_LIST_DIR}/DataPaths.cmake")

if(NOT DEFINED WORK_DIR OR NOT IS_ABSOLUTE "${WORK_DIR}")
  message(FATAL_ERROR "RunDataTest.cmake: -DWORK_DIR=<absolute directory> is required")
endif()
antb1_script_args(_args)
if(NOT _args)
  message(FATAL_ERROR "RunDataTest.cmake: no command after --")
endif()
antb1_data_paths()
if(NOT ANTB1_HITS_LIST)
  message(FATAL_ERROR "RunDataTest.cmake: no hits file at ${ANTB1_HITS_PATTERNS}: run `pixi run fetch-data`")
endif()

file(MAKE_DIRECTORY "${WORK_DIR}")
set(_tables "${WORK_DIR}/tables.txt")
list(JOIN ANTB1_HITS_PATTERNS "," _files)
file(
  WRITE "${_tables}"
  "# Written by cmake/scripts/RunDataTest.cmake: the ClickBench table of the data tests.\n"
  "hits ${_files} clickbench\n"
)

set(_command "")
foreach(_arg IN LISTS _args)
  if(_arg STREQUAL "{HITS_FILES}")
    list(APPEND _command ${ANTB1_HITS_LIST})
  elseif(_arg STREQUAL "{HITS_TABLES}")
    list(APPEND _command "${_tables}")
  elseif(_arg STREQUAL "{QUERIES}")
    list(APPEND _command "${ANTB1_DATA_DIR_PATH}/queries.sql")
  else()
    list(APPEND _command "${_arg}")
  endif()
endforeach()

execute_process(COMMAND ${_command} RESULT_VARIABLE _rc)
if(NOT _rc STREQUAL "0")
  list(GET _command 0 _program)
  get_filename_component(_program "${_program}" NAME)
  message(FATAL_ERROR "RunDataTest.cmake: ${_program} failed (exit code ${_rc}); see its report above")
endif()
