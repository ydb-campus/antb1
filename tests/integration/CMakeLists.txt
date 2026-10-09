# Integration tests (label integration): engine::Session end to end over the Parquet fixtures, corrupt,
# missing and mismatched inputs (IOError), tables over globs, Parquet scans, and hand-built hash-join
# plans over the star fixtures (they link exec as a test: no SQL reaches a join yet, and no module
# edge changes).
antb1_add_fixture_gtest(
  antb1_integration_tests
  LABEL integration
  PREFIX integration
  SOURCES session_test.cc parquet_errors_test.cc glob_test.cc scan_test.cc join_test.cc
  LIBS antb1::engine antb1::exec antb1::io antb1::plan Arrow::arrow_shared
)
