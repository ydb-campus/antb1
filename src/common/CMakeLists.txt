antb1_add_module(common SOURCES int128.cc source_span.cc utf8.cc version.cc)
# Build identity (antb1 version --verbose). ANTB1_GIT_SHA is empty in dev and CI presets, so a commit rebuilds nothing.
set(_antb1_flavor "dynamic")
if(ANTB1_ARROW_LINKAGE STREQUAL "static")
  set(_antb1_flavor "static-thinlto")
endif()
list(JOIN ANTB1_STATIC_CPU_FLAGS " " _antb1_cpu) # cmake/static/toolchain.cmake (preset static only)
set(
  _antb1_identity
  "ANTB1_VERSION_STRING=\"${PROJECT_VERSION}\""
  "ANTB1_BUILD_TYPE=\"$<CONFIG>\""
  "ANTB1_GIT_SHA=\"${ANTB1_GIT_SHA}\""
  "ANTB1_BUILD_FLAVOR=\"${_antb1_flavor}\""
  "ANTB1_CPU_BASELINE=\"${_antb1_cpu}\""
)
set_source_files_properties(version.cc PROPERTIES COMPILE_DEFINITIONS "${_antb1_identity}")

if(ANTB1_BUILD_TESTS)
  antb1_add_module_tests(
    common
    SOURCES tests/int128_test.cc tests/narrow_test.cc tests/source_span_test.cc tests/utf8_test.cc tests/version_test.cc
  )
endif()
