# Runs one test over the data derived from TPC-H (tests/tpch/CMakeLists.txt) so that its log never shows that data,
# even when the test fails because redaction broke:
#
#   cmake -DWORK_DIR=<dir in the build tree> [-DEXIT_CODE=<n>] [-DREQUIRE_COUNT=<k> -DREQUIRE_0=<regex> ...]
#         [-DFORBID_COUNT=<k> -DFORBID_0=<regex> ...] -P run_redacted.cmake -- <command> [args...]
#
# - Sanitizer reports go to files in WORK_DIR/sanitizers, because an ASan or UBSan report prints values. Only their
#   SUMMARY lines (the kind of error and its source location) join the output, as in cmake/scripts/RunDataTest.cmake.
# - The output (stdout and stderr) is checked before it is printed. Output that matches a FORBID regex, or a pattern
#   of an unredacted antb1-slt report (an SQL keyword, the `SQL:` block, differing rows), is withheld: the log names
#   only the pattern, so a broken redaction fails the test without publishing what it should have hidden. Unlike
#   cmake/scripts/expect_output.cmake, which prints first and checks after.
# - The test fails unless the command exits with EXIT_CODE (default 0) and every REQUIRE regex matches. A command
#   killed by a signal fails the test with exit code 125, whatever EXIT_CODE says.
cmake_minimum_required(VERSION 3.29) # cmake_language(EXIT)

if(NOT DEFINED WORK_DIR OR NOT IS_ABSOLUTE "${WORK_DIR}")
  message(FATAL_ERROR "run_redacted.cmake: -DWORK_DIR=<absolute directory> is required")
endif()
if(NOT DEFINED EXIT_CODE)
  set(EXIT_CODE 0)
endif()
set(_command "")
set(_in_command FALSE)
math(EXPR _last "${CMAKE_ARGC} - 1")
foreach(_i RANGE 0 ${_last})
  if(_in_command)
    list(APPEND _command "${CMAKE_ARGV${_i}}")
  elseif(CMAKE_ARGV${_i} STREQUAL "--")
    set(_in_command TRUE)
  endif()
endforeach()
if(NOT _command)
  message(FATAL_ERROR "run_redacted.cmake: no command after --")
endif()

set(_sanitizer_dir "${WORK_DIR}/sanitizers")
file(REMOVE_RECURSE "${_sanitizer_dir}")
file(MAKE_DIRECTORY "${_sanitizer_dir}")
foreach(_var ASAN_OPTIONS UBSAN_OPTIONS LSAN_OPTIONS TSAN_OPTIONS)
  string(TOLOWER "${_var}" _log)
  string(REPLACE "_options" "" _log "${_log}")
  set(_options "$ENV{${_var}}")
  if(NOT _options STREQUAL "")
    string(APPEND _options ":")
  endif()
  set(ENV{${_var}} "${_options}log_path=${_sanitizer_dir}/${_log}")
endforeach()

execute_process(COMMAND ${_command} RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _out)
file(GLOB _reports LIST_DIRECTORIES false "${_sanitizer_dir}/*")
if(_reports)
  string(
    APPEND _out
    "run_redacted.cmake: sanitizer output in the files below, not printed because it can contain values of the "
    "data; read it locally:"
  )
  foreach(_report IN LISTS _reports)
    file(STRINGS "${_report}" _lines REGEX "^SUMMARY: ")
    string(APPEND _out "\n  ${_report}")
    foreach(_line IN LISTS _lines)
      string(APPEND _out "\n    ${_line}")
    endforeach()
  endforeach()
  string(APPEND _out "\n")
endif()

# What an unredacted antb1-slt report prints (tests/slt/runner/result_diff.cc): the SQL block, differing rows, and
# the SQL itself.
set(_unredacted "\n  SQL:\n" "\n    row [0-9]+: " "[sS][eE][lL][eE][cC][tT][ \t\n]")
set(_forbid "${_unredacted}")
if(DEFINED FORBID_COUNT AND FORBID_COUNT GREATER 0)
  math(EXPR _n "${FORBID_COUNT} - 1")
  foreach(_i RANGE 0 ${_n})
    list(APPEND _forbid "${FORBID_${_i}}")
  endforeach()
endif()
set(_withheld "")
foreach(_regex IN LISTS _forbid)
  if(_out MATCHES "${_regex}")
    string(APPEND _withheld "\n  ${_regex}")
  endif()
endforeach()
if(NOT _withheld STREQUAL "")
  message(
    "run_redacted.cmake: the output is withheld: it matches these forbidden patterns, so it may show data derived "
    "from TPC-H. Run the test locally to see it:${_withheld}"
  )
else()
  message("${_out}")
endif()

if(NOT _rc MATCHES "^[0-9]+$")
  message("run_redacted.cmake: the command did not exit normally: ${_rc}")
  cmake_language(EXIT 125)
endif()
set(_problems "")
if(NOT _withheld STREQUAL "")
  string(APPEND _problems "\n  the output matches a forbidden pattern")
endif()
if(NOT _rc STREQUAL "${EXIT_CODE}")
  string(APPEND _problems "\n  exit code: expected ${EXIT_CODE}, got ${_rc}")
endif()
if(DEFINED REQUIRE_COUNT AND REQUIRE_COUNT GREATER 0)
  math(EXPR _n "${REQUIRE_COUNT} - 1")
  foreach(_i RANGE 0 ${_n})
    if(NOT _out MATCHES "${REQUIRE_${_i}}")
      string(APPEND _problems "\n  the output does not match the required pattern ${REQUIRE_${_i}}")
    endif()
  endforeach()
endif()
if(NOT _problems STREQUAL "")
  list(GET _command 0 _program)
  get_filename_component(_program "${_program}" NAME)
  message(FATAL_ERROR "run_redacted.cmake: ${_program}:${_problems}")
endif()
