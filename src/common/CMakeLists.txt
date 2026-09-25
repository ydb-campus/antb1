antb1_add_module(common SOURCES int128.cc source_span.cc version.cc)
target_compile_definitions(antb1_common PRIVATE ANTB1_VERSION_STRING="${PROJECT_VERSION}" ANTB1_BUILD_TYPE="$<CONFIG>")

if(ANTB1_BUILD_TESTS)
  antb1_add_module_tests(
    common
    SOURCES tests/int128_test.cc tests/narrow_test.cc tests/source_span_test.cc tests/version_test.cc
  )
endif()
