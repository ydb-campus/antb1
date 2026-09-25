# Test registration helpers. Labels (docs/testing.md, R008): unit integration slt oracle diff metamorphic cli
# harness fuzz-replay fuzz bench-smoke data setup.
include_guard(GLOBAL)

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
