# Runs the command after `--`, prints its output and then the file FILE, and exits with the command's exit code, so
# that the REQUIRE and FORBID regexes of an expect test (antb1_add_expect_test) also check the file the command wrote
# (antb1-slt tpch --costs). FILE is removed first, so a file left by an earlier run cannot pass for this one.
#
#   cmake -DFILE=<path> -P print_file_after.cmake -- <command> [args...]
cmake_minimum_required(VERSION 3.29) # cmake_language(EXIT)

if(NOT DEFINED FILE)
  message(FATAL_ERROR "print_file_after.cmake: -DFILE=<path> is required")
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
  message(FATAL_ERROR "print_file_after.cmake: no command after --")
endif()

file(REMOVE "${FILE}")
execute_process(COMMAND ${_command} RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _out)
set(_file "(none)\n")
if(EXISTS "${FILE}")
  file(READ "${FILE}" _file)
endif()
message("${_out}--- the file:\n${_file}")
if(NOT _rc MATCHES "^[0-9]+$")
  set(_rc 125)
endif()
cmake_language(EXIT ${_rc})
