antb1_add_module(
  io
  SOURCES parquet_table.cc glob.cc
  PUBLIC_DEPS common plan
  PUBLIC_LIBS Arrow::arrow_shared
  PRIVATE_LIBS Parquet::parquet_shared
)

if(ANTB1_BUILD_TESTS)
  antb1_add_module_tests(
    io
    SOURCES tests/parquet_table_test.cc tests/parquet_scan_test.cc tests/glob_test.cc
    LIBS Parquet::parquet_shared
  )
endif()
