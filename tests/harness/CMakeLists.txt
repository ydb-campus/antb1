# Harness self-tests that are not part of one runner (label harness): the logical fixture digest.
add_library(antb1_fixture_digest_lib STATIC fixture_digest.cc)
target_include_directories(antb1_fixture_digest_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(
  antb1_fixture_digest_lib
  PUBLIC Arrow::arrow_shared Parquet::parquet_shared antb1_slt_lib
  PRIVATE antb1::build_options
)

add_executable(antb1-fixture-digest fixture_digest_main.cc)
target_link_libraries(antb1-fixture-digest PRIVATE antb1_fixture_digest_lib antb1::build_options)

# The fixtures this build generated must match the committed digest (the same on Linux and macOS).
add_test(
  NAME harness.fixtures.digest
  COMMAND antb1-fixture-digest --check "${PROJECT_SOURCE_DIR}/tests/fixtures/fixtures.digest" "${ANTB1_FIXTURES_DIR}"
)
set_tests_properties(harness.fixtures.digest PROPERTIES LABELS harness FIXTURES_REQUIRED antb1_fixtures)

antb1_add_gtest(
  antb1_fixture_digest_tests
  LABEL harness
  PREFIX harness
  SOURCES fixture_digest_test.cc
  LIBS antb1_fixture_digest_lib
)
