# ctest fuzz.sql_parser.smoke (label fuzz; `pixi run fuzz-smoke`): a short, deterministic libFuzzer run.
#
# cmake -DFUZZER=<exe> -DDICT=<abs .dict> -DCORPUS_DIR=<abs dir> -DWORK_DIR=<abs dir> -DARTIFACTS_DIR=<abs dir>
#       [-DRUNS=200000] -P RunFuzzSmoke.cmake
#
# WORK_DIR is recreated empty and comes first, so libFuzzer writes the inputs it finds there and never into the
# committed corpus CORPUS_DIR, which only seeds the run. Crash, leak and timeout inputs go to ARTIFACTS_DIR.
cmake_minimum_required(VERSION 3.28)

foreach(
  var
  FUZZER
  DICT
  CORPUS_DIR
  WORK_DIR
  ARTIFACTS_DIR
)
  if(NOT DEFINED ${var} OR "${${var}}" STREQUAL "")
    message(FATAL_ERROR "RunFuzzSmoke.cmake: -D${var}=... is required")
  endif()
  if(NOT IS_ABSOLUTE "${${var}}")
    message(FATAL_ERROR "RunFuzzSmoke.cmake: ${var} must be an absolute path (got '${${var}}')")
  endif()
endforeach()
if(NOT DEFINED RUNS)
  set(RUNS 200000)
endif()
if(NOT IS_DIRECTORY "${CORPUS_DIR}")
  message(FATAL_ERROR "RunFuzzSmoke.cmake: the seed corpus ${CORPUS_DIR} does not exist")
endif()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}" "${ARTIFACTS_DIR}")

set(
  _command
  "${FUZZER}"
  -seed=1
  -runs=${RUNS}
  "-dict=${DICT}"
  -timeout=10
  -rss_limit_mb=2048
  "-artifact_prefix=${ARTIFACTS_DIR}/"
  -print_final_stats=1
  "${WORK_DIR}"
  "${CORPUS_DIR}"
)
list(JOIN _command " " _shown)
message("fuzz smoke: ${_shown}")
execute_process(COMMAND ${_command} RESULT_VARIABLE _rc)
if(NOT _rc STREQUAL "0")
  message(
    FATAL_ERROR
    "fuzz smoke: FAIL (libFuzzer exit '${_rc}'). The failing input is in ${ARTIFACTS_DIR}/ (crash-*, leak-*, "
    "timeout-*, oom-*). Reproduce: ${FUZZER} <artifact>; minimize: ${FUZZER} -minimize_crash=1 -runs=100000 "
    "<artifact>; then commit the minimized input to fuzz/regressions/ with the fix (fuzz/regressions/README.md)."
  )
endif()
message("fuzz smoke: PASS")
