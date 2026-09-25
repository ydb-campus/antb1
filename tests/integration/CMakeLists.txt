# Integration tests (label integration): engine::Session end to end over the Parquet fixtures, corrupt,
# missing and mismatched inputs (IOError), and tables over globs.
antb1_add_fixture_gtest(
  antb1_integration_tests
  LABEL integration
  PREFIX integration
  SOURCES session_test.cc parquet_errors_test.cc glob_test.cc
  LIBS antb1::engine antb1::io antb1::plan Arrow::arrow_shared
)
