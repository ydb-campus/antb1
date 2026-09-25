antb1_add_module(
  cli
  SOURCES cli.cc
  PUBLIC_DEPS common
  PRIVATE_DEPS engine
  PUBLIC_LIBS Arrow::arrow_shared
  PRIVATE_LIBS CLI11::CLI11
)

add_executable(antb1 main.cc)
target_link_libraries(antb1 PRIVATE antb1::cli antb1::build_options)

if(ANTB1_BUILD_TESTS)
  antb1_add_module_tests(cli SOURCES tests/cli_test.cc LIBS antb1::plan Parquet::parquet_shared)
endif()
