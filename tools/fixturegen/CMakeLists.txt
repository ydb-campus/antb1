# antb1-fixturegen: deterministic Parquet fixtures for the hermetic tests (built only with tests).
add_library(antb1_fixturegen_lib STATIC hits_schema.cc fixtures.cc)
target_include_directories(antb1_fixturegen_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(
  antb1_fixturegen_lib
  PUBLIC Arrow::arrow_shared Parquet::parquet_shared
  PRIVATE antb1::build_options
)

add_executable(antb1-fixturegen main.cc)
target_link_libraries(antb1-fixturegen PRIVATE antb1_fixturegen_lib antb1::build_options)

# Every test that reads fixtures declares FIXTURES_REQUIRED antb1_fixtures; ctest then runs this first
# (also when the run is filtered with -R/-L).
add_test(NAME fixtures.generate COMMAND antb1-fixturegen "${ANTB1_FIXTURES_DIR}")
set_tests_properties(
  fixtures.generate
  PROPERTIES LABELS setup FIXTURES_SETUP antb1_fixtures TIMEOUT 120
)

# --check-schema must accept the fixtures that have the partitioned layout and reject the others.
foreach(file IN ITEMS hits_like hits_like_nulls hits_like_split/part-0 empty)
  string(REPLACE "/" "." _name "${file}")
  add_test(
    NAME harness.fixtures.schema.${_name}
    COMMAND antb1-fixturegen --check-schema "${ANTB1_FIXTURES_DIR}/${file}.parquet"
  )
  set_tests_properties(harness.fixtures.schema.${_name} PROPERTIES LABELS harness FIXTURES_REQUIRED antb1_fixtures)
endforeach()
antb1_add_expect_test(
  harness.fixtures.schema.rejects_required
  LABELS harness
  FIXTURES_REQUIRED antb1_fixtures
  EXIT_CODE 1
  REQUIRE "column 2 \\(Title\\): logical type: expected None, got String" "difference\\(s\\) from the hits-like schema"
  COMMAND $<TARGET_FILE:antb1-fixturegen> --check-schema "${ANTB1_FIXTURES_DIR}/hits_like_required.parquet"
)

antb1_add_gtest(
  antb1_fixturegen_tests
  LABEL harness
  PREFIX harness
  SOURCES tests/fixturegen_test.cc
  LIBS antb1_fixturegen_lib
)
