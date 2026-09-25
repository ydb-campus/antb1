# Test registration helpers. Labels (docs/testing.md, R008): unit integration slt oracle diff metamorphic cli
# harness fuzz-replay fuzz bench-smoke data setup.
include_guard(GLOBAL)

# Parquet fixtures written by the `fixtures.generate` test (tools/fixturegen); tests that read them declare
# FIXTURES_REQUIRED antb1_fixtures. Table names of the sqllogictest files map to fixtures in ANTB1_SLT_TABLES.
set(ANTB1_FIXTURES_DIR "${CMAKE_BINARY_DIR}/fixtures")
set(ANTB1_SLT_TABLES "${PROJECT_SOURCE_DIR}/tests/slt/tables.txt")

# antb1_add_gtest(<target> LABEL <label> PREFIX <prefix> SOURCES ... LIBS ... [PROPERTIES ...])
function(antb1_add_gtest target)
  cmake_parse_arguments(ARG "" "LABEL;PREFIX" "SOURCES;LIBS;PROPERTIES" ${ARGN})
  add_executable(${target} ${ARG_SOURCES})
  target_link_libraries(${target} PRIVATE ${ARG_LIBS} antb1::build_options GTest::gmock GTest::gtest_main)
  if(ANTB1_CLANG_TIDY AND EXISTS "${PROJECT_SOURCE_DIR}/tests/.clang-tidy")
    set_target_properties(
      ${target}
      PROPERTIES CXX_CLANG_TIDY "${ANTB1_CLANG_TIDY_EXE};--config-file=${PROJECT_SOURCE_DIR}/tests/.clang-tidy"
    )
  endif()
  gtest_discover_tests(
    ${target}
    DISCOVERY_MODE PRE_TEST
    TEST_PREFIX "${ARG_PREFIX}."
    PROPERTIES LABELS "${ARG_LABEL}" TIMEOUT 120 ${ARG_PROPERTIES}
  )
endfunction()

# antb1_add_fixture_gtest(<target> LABEL <label> PREFIX <prefix> SOURCES ... [LIBS ...])
# A gtest suite that reads the Parquet fixtures: its tests require the ctest fixture antb1_fixtures and
# see the fixtures and source directories through tests/support/fixtures.h.
function(antb1_add_fixture_gtest target)
  cmake_parse_arguments(ARG "" "LABEL;PREFIX" "SOURCES;LIBS" ${ARGN})
  if(NOT TARGET antb1_test_support)
    add_library(antb1_test_support INTERFACE)
    target_include_directories(antb1_test_support INTERFACE "${PROJECT_SOURCE_DIR}/tests/support")
    target_compile_definitions(
      antb1_test_support
      INTERFACE ANTB1_TEST_FIXTURES_DIR="${ANTB1_FIXTURES_DIR}" ANTB1_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    )
  endif()
  antb1_add_gtest(
    ${target}
    LABEL ${ARG_LABEL}
    PREFIX ${ARG_PREFIX}
    SOURCES ${ARG_SOURCES}
    LIBS antb1_test_support ${ARG_LIBS}
    PROPERTIES FIXTURES_REQUIRED antb1_fixtures
  )
endfunction()

# antb1_add_module_tests(<module> SOURCES ... [LIBS ...]): src/<m>/tests/*_test.cc -> "<m>.<Suite>.<Case>", label unit.
function(antb1_add_module_tests module)
  cmake_parse_arguments(ARG "" "" "SOURCES;LIBS" ${ARGN})
  antb1_add_gtest(
    antb1_${module}_tests
    LABEL unit
    PREFIX ${module}
    SOURCES ${ARG_SOURCES}
    LIBS antb1::${module} ${ARG_LIBS}
  )
endfunction()

# antb1_add_expect_test(<name> COMMAND <cmd> [args...] [EXIT_CODE <n>] [REQUIRE <regex>...] [FORBID <regex>...]
#                       [LABELS <label>...] [FIXTURES_REQUIRED <fixture>...])
# A test that passes only if COMMAND exits with EXIT_CODE (default 1) and its merged stdout/stderr matches every
# REQUIRE regex (CMake syntax) and no FORBID regex. Use it instead of WILL_FAIL, where a crash would also "pass".
function(antb1_add_expect_test name)
  cmake_parse_arguments(ARG "" "EXIT_CODE" "COMMAND;REQUIRE;FORBID;LABELS;FIXTURES_REQUIRED" ${ARGN})
  if(NOT DEFINED ARG_EXIT_CODE)
    set(ARG_EXIT_CODE 1)
  endif()
  set(_defs "-DEXIT_CODE=${ARG_EXIT_CODE}")
  foreach(kind IN ITEMS REQUIRE FORBID)
    set(_i 0)
    foreach(regex IN LISTS ARG_${kind})
      list(APPEND _defs "-D${kind}_${_i}=${regex}")
      math(EXPR _i "${_i} + 1")
    endforeach()
    list(APPEND _defs "-D${kind}_COUNT=${_i}")
  endforeach()
  add_test(
    NAME ${name}
    COMMAND ${CMAKE_COMMAND} ${_defs} -P "${PROJECT_SOURCE_DIR}/cmake/scripts/expect_output.cmake" -- ${ARG_COMMAND}
  )
  set_tests_properties(${name} PROPERTIES LABELS "${ARG_LABELS}" TIMEOUT 120)
  if(ARG_FIXTURES_REQUIRED)
    set_tests_properties(${name} PROPERTIES FIXTURES_REQUIRED "${ARG_FIXTURES_REQUIRED}")
  endif()
endfunction()

# antb1_add_cli_golden(<case> ARGS <arg>... [EXIT_CODE <n>] [STDERR] [STDOUT_REGEX <regex>]
#                      [STDERR_REGEX <regex>] [INPUT <file>])
# Test cli.<case> (label cli): runs the antb1 binary with ARGS (stdin from INPUT) through
# cmake/scripts/CompareOutput.cmake. The exit code must be EXIT_CODE (default 0); stdout must equal
# tests/cli/golden/<case>.stdout (or match STDOUT_REGEX); with STDERR stderr must equal <case>.stderr,
# with STDERR_REGEX it must match. Outputs are normalized (fixtures dir -> ${FIXTURES}, source dir ->
# ${SOURCE}). ANTB1_UPDATE_GOLDENS=1 rewrites the golden files.
function(antb1_add_cli_golden case)
  cmake_parse_arguments(ARG "STDERR" "EXIT_CODE;STDOUT_REGEX;STDERR_REGEX;INPUT" "ARGS" ${ARGN})
  if(NOT DEFINED ARG_EXIT_CODE)
    set(ARG_EXIT_CODE 0)
  endif()
  set(
    _defs
    "-DNAME=${case}"
    "-DGOLDEN_DIR=${PROJECT_SOURCE_DIR}/tests/cli/golden"
    "-DEXIT_CODE=${ARG_EXIT_CODE}"
    "-DFIXTURES_DIR=${ANTB1_FIXTURES_DIR}"
    "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
  )
  if(ARG_STDERR)
    list(APPEND _defs "-DCOMPARE_STDERR=ON")
  endif()
  foreach(opt IN ITEMS STDOUT_REGEX STDERR_REGEX)
    if(DEFINED ARG_${opt})
      list(APPEND _defs "-D${opt}=${ARG_${opt}}")
    endif()
  endforeach()
  if(ARG_INPUT)
    list(APPEND _defs "-DINPUT_FILE=${ARG_INPUT}")
  endif()
  add_test(
    NAME cli.${case}
    COMMAND
      ${CMAKE_COMMAND} ${_defs} -P "${PROJECT_SOURCE_DIR}/cmake/scripts/CompareOutput.cmake" -- $<TARGET_FILE:antb1>
      ${ARG_ARGS}
  )
  set_tests_properties(
    cli.${case}
    PROPERTIES LABELS cli FIXTURES_REQUIRED antb1_fixtures TIMEOUT 60
  )
endfunction()

# antb1_slt_args(<out-var> <test-name> [TABLES <file>] [more antb1-slt options...])
# The common antb1-slt arguments of a registered test: fixtures dir, tables file (default
# ANTB1_SLT_TABLES), a private DuckDB temp dir.
function(antb1_slt_args out name)
  cmake_parse_arguments(ARG "" "TABLES" "" ${ARGN})
  if(NOT ARG_TABLES)
    set(ARG_TABLES "${ANTB1_SLT_TABLES}")
  endif()
  set(
    ${out}
    --fixtures
    "${ANTB1_FIXTURES_DIR}"
    --tables
    "${ARG_TABLES}"
    --temp-dir
    "${CMAKE_BINARY_DIR}/slt-tmp/${name}"
    --test-name
    "${name}"
    ${ARG_UNPARSED_ARGUMENTS}
    PARENT_SCOPE
  )
endfunction()

# antb1_add_slt_suite(DIR <dir> [AREA <area>])
# Registers every <dir>/*.slt file (sqllogictest, see tests/slt/README.md) as
#   slt.<area>.<file>     antb1 engine vs the expectations (label slt)
#   oracle.<area>.<file>  DuckDB vs the same expectations (label oracle; only when ANTB1_HAVE_DUCKDB)
# <area> defaults to the directory name. The files are also rewritten by the `slt-complete` target.
function(antb1_add_slt_suite)
  cmake_parse_arguments(ARG "" "DIR;AREA" "" ${ARGN})
  get_filename_component(_dir "${ARG_DIR}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
  if(NOT ARG_AREA)
    get_filename_component(ARG_AREA "${_dir}" NAME)
  endif()
  file(GLOB _files CONFIGURE_DEPENDS "${_dir}/*.slt")
  if(NOT _files)
    message(FATAL_ERROR "antb1_add_slt_suite: no .slt files in ${_dir}")
  endif()
  foreach(file IN LISTS _files)
    get_filename_component(_stem "${file}" NAME_WE)
    set(_engines antb1)
    if(ANTB1_HAVE_DUCKDB)
      list(APPEND _engines duckdb)
    endif()
    foreach(engine IN LISTS _engines)
      set(_prefix slt)
      set(_label slt)
      if(engine STREQUAL "duckdb")
        set(_prefix oracle)
        set(_label oracle)
      endif()
      set(_name "${_prefix}.${ARG_AREA}.${_stem}")
      antb1_slt_args(_args ${_name} --engine ${engine})
      add_test(NAME ${_name} COMMAND antb1-slt run ${_args} "${file}")
      set_tests_properties(
        ${_name}
        PROPERTIES LABELS ${_label} FIXTURES_REQUIRED antb1_fixtures TIMEOUT 300
      )
    endforeach()
    set_property(GLOBAL APPEND PROPERTY ANTB1_SLT_FILES "${file}")
  endforeach()
endfunction()

# antb1_add_slt_complete_target(): custom target `slt-complete` (`pixi run slt-complete`) that regenerates the
# fixtures and rewrites the expected results of every registered .slt file from DuckDB (onlyif antb1 records
# from antb1, flagged for review). Call it after the last antb1_add_slt_suite().
function(antb1_add_slt_complete_target)
  get_property(_files GLOBAL PROPERTY ANTB1_SLT_FILES)
  if(NOT ANTB1_HAVE_DUCKDB)
    add_custom_target(
      slt-complete
      COMMAND ${CMAKE_COMMAND} -E echo "slt-complete needs the DuckDB oracle: configure with ANTB1_WITH_DUCKDB=ON"
      COMMAND ${CMAKE_COMMAND} -E false
    )
    return()
  endif()
  antb1_slt_args(_args slt-complete)
  add_custom_target(
    slt-complete
    COMMAND antb1-fixturegen "${ANTB1_FIXTURES_DIR}"
    COMMAND antb1-slt complete ${_args} ${_files}
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    COMMENT "Rewriting the expected results of ${PROJECT_SOURCE_DIR}/tests/slt from DuckDB (review the diff)"
    USES_TERMINAL
    VERBATIM
  )
endfunction()
