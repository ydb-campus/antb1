# Metamorphic relations (relations.h) as a library: the gtest suite below runs them over the fixtures, and
# tests/data runs relations over the ClickBench hits data with the same checks (antb1-data-metamorphic).
add_library(antb1_metamorphic_lib STATIC relations.cc)
target_include_directories(antb1_metamorphic_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(antb1_metamorphic_lib PUBLIC antb1_slt_lib PRIVATE antb1::build_options)

# Metamorphic tests (label metamorphic): relations.cc lists the relations; relations whose features are not
# all in tests/slt/supported_features.h are pending (reported as skipped, and must still get Unsupported).
antb1_add_fixture_gtest(
  antb1_metamorphic_tests
  LABEL metamorphic
  PREFIX metamorphic
  SOURCES metamorphic_test.cc
  LIBS antb1_metamorphic_lib Parquet::parquet_shared
)
