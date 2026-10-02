# Tests over data derived from TPC-H (ADR 0006). The data, the query texts and DuckDB's answers are generated at
# test time from the pinned duckdb-extension-tpch package; nothing derived from TPC-H is committed.
if(NOT ANTB1_HAVE_TPCH)
  return()
endif()

# The pinned extension loads into the duckdb CLI of the same prefix by absolute path (unsigned: conda-forge cannot
# sign DuckDB extensions) and provides the 22 query texts. The output must be exactly the count. No semicolons in the
# SQL: CMake would split the argument there.
antb1_add_expect_test(
  harness.tpch.extension
  COMMAND
    "${ANTB1_DUCKDB_CLI}"
    -unsigned
    -no-init
    -noheader
    -list
    -cmd
    "LOAD '${ANTB1_TPCH_EXTENSION}'"
    -c
    "SELECT count(*) FROM tpch_queries()"
  EXIT_CODE 0
  REQUIRE "^22[^0-9]?$"
  LABELS harness
)
