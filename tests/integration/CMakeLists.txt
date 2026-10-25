# Integration tests (label integration): engine::Session end to end over the Parquet fixtures, corrupt,
# missing and mismatched inputs (IOError), tables over globs, Parquet scans, and hand-built hash-join
# plans over the star fixtures (they link exec as a test, so no module edge changes).
antb1_add_fixture_gtest(
  antb1_integration_tests
  LABEL integration
  PREFIX integration
  SOURCES session_test.cc parquet_errors_test.cc glob_test.cc scan_test.cc join_test.cc
  LIBS
    antb1::engine
    antb1::exec
    antb1::io
    antb1::plan
    Arrow::arrow_${ANTB1_ARROW_LINKAGE}
    Parquet::parquet_${ANTB1_ARROW_LINKAGE}
)
# One test needs a Parquet file whose column chunks carry no `encoding_stats`, which Arrow's writer
# always emits and DuckDB's never does (ADR 0020): it writes one with the oracle's C API at test
# time, as scripts/static_gates.py and tests/tpch/generate.cmake already do.
if(ANTB1_HAVE_DUCKDB)
  target_link_libraries(antb1_integration_tests PRIVATE antb1_ext_duckdb)
  target_compile_definitions(antb1_integration_tests PRIVATE ANTB1_INTEGRATION_HAVE_DUCKDB)
endif()
