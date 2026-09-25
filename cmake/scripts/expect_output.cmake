# Runs a command and checks its exit code and output. Used by antb1_add_expect_test() for tests that
# must fail in one specific way (harness self-tests, redaction canaries): unlike WILL_FAIL, a crash or a
# different failure never counts as a pass.
#
# cmake -DEXIT_CODE=<n> [-DREQUIRE_COUNT=<k> -DREQUIRE_0=<regex> ...] [-DFORBID_COUNT=<k> -DFORBID_0=<regex> ...]
#       -P expect_output.cmake -- <command> [args...]
#
# The command's stdout and stderr are merged and echoed. The test fails unless the exit code equals
# EXIT_CODE, every REQUIRE_<i> regex matches the output and no FORBID_<i> regex matches it.
cmake_minimum_required(VERSION 3.28)

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
  message(FATAL_ERROR "expect_output.cmake: no command after --")
endif()
if(NOT DEFINED EXIT_CODE)
  set(EXIT_CODE 1)
endif()

execute_process(COMMAND ${_command} RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _out)
message("${_out}")

set(_problems "")
if(NOT _rc STREQUAL "${EXIT_CODE}")
  string(APPEND _problems "\n  exit code: expected ${EXIT_CODE}, got '${_rc}'")
endif()
if(DEFINED REQUIRE_COUNT AND REQUIRE_COUNT GREATER 0)
  math(EXPR _n "${REQUIRE_COUNT} - 1")
  foreach(_i RANGE 0 ${_n})
    if(NOT _out MATCHES "${REQUIRE_${_i}}")
      string(APPEND _problems "\n  output does not match required regex: ${REQUIRE_${_i}}")
    endif()
  endforeach()
endif()
if(DEFINED FORBID_COUNT AND FORBID_COUNT GREATER 0)
  math(EXPR _n "${FORBID_COUNT} - 1")
  foreach(_i RANGE 0 ${_n})
    if(_out MATCHES "${FORBID_${_i}}")
      string(APPEND _problems "\n  output matches forbidden regex: ${FORBID_${_i}}")
    endif()
  endforeach()
endif()
if(_problems)
  list(JOIN _command " " _shown)
  message(FATAL_ERROR "expect_output.cmake: ${_shown}${_problems}")
endif()
