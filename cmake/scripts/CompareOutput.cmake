# Runs a command and compares its exit code, stdout and (when requested) stderr with expectations.
# Used by antb1_add_cli_golden() (cmake/Antb1Testing.cmake) for the CLI golden tests in tests/cli.
#
# cmake -DNAME=<case> -DGOLDEN_DIR=<dir> -DEXIT_CODE=<n> [-DCOMPARE_STDERR=ON]
#       [-DSTDOUT_REGEX=<regex>] [-DSTDERR_REGEX=<regex>] [-DINPUT_FILE=<file>]
#       [-DFIXTURES_DIR=<dir>] [-DSOURCE_DIR=<dir>] -P CompareOutput.cmake -- <command> [args...]
#
#   stdout  must equal <GOLDEN_DIR>/<NAME>.stdout (be empty if there is no such file), or match
#           STDOUT_REGEX when given
#   stderr  must equal <GOLDEN_DIR>/<NAME>.stderr (the same rule) with COMPARE_STDERR, or match
#           STDERR_REGEX when given; otherwise it is only echoed
#   exit    must equal EXIT_CODE (the CLI contract: 0 ok, 1 query error, 2 usage, 3 I/O,
#           4 unsupported, 70 internal)
#
# Both outputs are normalized first: FIXTURES_DIR becomes ${FIXTURES} and SOURCE_DIR becomes ${SOURCE},
# so golden files hold no local paths. Regexes (CMake syntax) see the output without its final newline.
# With ANTB1_UPDATE_GOLDENS=1 in the environment the golden files are rewritten instead of compared
# (removed for empty output; review the diff); the exit code and the regexes are still checked.
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
if(NOT _command OR NOT DEFINED NAME OR NOT DEFINED GOLDEN_DIR OR NOT DEFINED EXIT_CODE)
  message(FATAL_ERROR "CompareOutput.cmake: needs -DNAME, -DGOLDEN_DIR, -DEXIT_CODE and a command after --")
endif()

set(_input "")
if(INPUT_FILE)
  set(_input INPUT_FILE "${INPUT_FILE}")
endif()
execute_process(COMMAND ${_command} ${_input} RESULT_VARIABLE _rc OUTPUT_VARIABLE _stdout ERROR_VARIABLE _stderr)

function(_normalize var)
  set(_text "${${var}}")
  if(FIXTURES_DIR)
    string(REPLACE "${FIXTURES_DIR}" "\${FIXTURES}" _text "${_text}")
  endif()
  if(SOURCE_DIR)
    string(REPLACE "${SOURCE_DIR}" "\${SOURCE}" _text "${_text}")
  endif()
  set(${var} "${_text}" PARENT_SCOPE)
endfunction()
_normalize(_stdout)
_normalize(_stderr)
message("---- exit code: ${_rc}\n---- stdout:\n${_stdout}---- stderr:\n${_stderr}----")

set(_update FALSE)
if("$ENV{ANTB1_UPDATE_GOLDENS}" STREQUAL "1")
  set(_update TRUE)
endif()
set(_problems "")

# _check(<stream> <text> <compare-golden> <regex>)
function(_check stream text golden regex)
  set(_file "${GOLDEN_DIR}/${NAME}.${stream}")
  set(_found "")
  if(regex)
    string(REGEX REPLACE "\n$" "" _trimmed "${text}")
    if(NOT _trimmed MATCHES "${regex}")
      set(_found "\n  ${stream} does not match the regex: ${regex}")
    endif()
  elseif(golden)
    if(_update)
      if(text STREQUAL "")
        file(REMOVE "${_file}")
      else()
        file(WRITE "${_file}" "${text}")
        message("CompareOutput.cmake: updated ${_file}")
      endif()
    else()
      set(_expected "") # no golden file: the stream must be empty
      if(EXISTS "${_file}")
        file(READ "${_file}" _expected)
      endif()
      if(NOT _expected STREQUAL text)
        set(_found "\n  ${stream} differs from ${_file} (no file: empty); expected:\n${_expected}")
      endif()
    endif()
  endif()
  set(_problems "${_problems}${_found}" PARENT_SCOPE)
endfunction()

if(NOT _rc STREQUAL "${EXIT_CODE}")
  string(APPEND _problems "\n  exit code: expected ${EXIT_CODE}, got '${_rc}'")
endif()
_check(stdout "${_stdout}" TRUE "${STDOUT_REGEX}")
_check(stderr "${_stderr}" "${COMPARE_STDERR}" "${STDERR_REGEX}")

if(_problems)
  list(JOIN _command " " _shown)
  message(
    FATAL_ERROR
    "CompareOutput.cmake: ${NAME}: ${_shown}${_problems}\n"
    "  If the change is intended: ANTB1_UPDATE_GOLDENS=1 pixi run test -R '^cli\\.${NAME}$', then review the diff."
  )
endif()
