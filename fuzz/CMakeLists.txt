# Fuzzing of the Arrow-free SQL front end: the round-trip property of sql::Parse and sql::ToSql
# (sql_parser_property.h). Seeds (fuzz/corpus/sql_parser) are our own queries; minimized crash inputs are
# committed to fuzz/regressions (README.md there).
#   antb1-fuzz-replay        every build (GCC too): fuzz.replay.sql_parser (label fuzz-replay), harness self-tests
#   antb1-sql-parser-fuzzer  ANTB1_BUILD_FUZZERS=ON (Clang, preset `fuzz`): fuzz.sql_parser.smoke (label fuzz)
set(ANTB1_FUZZ_CORPUS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/corpus/sql_parser")
set(ANTB1_FUZZ_REGRESSIONS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/regressions")
set(ANTB1_FUZZ_DICT "${CMAKE_CURRENT_SOURCE_DIR}/sql.dict")
# build/fuzz/artifacts/ for the `fuzz` preset: crash, leak and timeout inputs (uploaded by CI on failure).
set(ANTB1_FUZZ_ARTIFACTS_DIR "${CMAKE_BINARY_DIR}/artifacts")

if(ANTB1_BUILD_FUZZERS)
  # libFuzzer's coverage instrumentation, per target and never globally: the code under test (common, sql) and
  # the fuzz targets. The binaries that link the instrumented libraries without the fuzzing engine (unit tests,
  # antb1-fuzz-replay) get the SanitizerCoverage callbacks from the sanitizer runtime, so fuzzers need
  # ANTB1_SANITIZE.
  if(NOT ANTB1_SANITIZE)
    message(FATAL_ERROR "ANTB1_BUILD_FUZZERS needs ANTB1_SANITIZE (e.g. 'address;undefined'): use preset `fuzz`")
  endif()
  foreach(target IN ITEMS antb1_common antb1_sql)
    target_compile_options(${target} PRIVATE -fsanitize=fuzzer-no-link)
  endforeach()
endif()

add_executable(antb1-fuzz-replay replay_main.cc)
target_link_libraries(antb1-fuzz-replay PRIVATE antb1::sql antb1::common antb1::build_options)

if(ANTB1_BUILD_TESTS)
  add_test(
    NAME fuzz.replay.sql_parser
    COMMAND antb1-fuzz-replay "${ANTB1_FUZZ_CORPUS_DIR}" "${ANTB1_FUZZ_REGRESSIONS_DIR}"
  )
  set_tests_properties(fuzz.replay.sql_parser PROPERTIES LABELS fuzz-replay TIMEOUT 120)

  # Harness self-tests: each check of the property must catch a broken unparser (replay_main.cc), and a wrong
  # corpus path must fail instead of passing with no input.
  antb1_add_expect_test(
    harness.fuzz.replay.mutate_suffix
    LABELS harness
    EXIT_CODE 1
    REQUIRE
      "VIOLATION [^ ]*/count_star_path\\.sql: Parse\\(ToSql\\(ast\\)\\) failed"
      "VIOLATION [^ ]*/count_star\\.sql: Parse\\(ToSql\\(ast\\)\\) is not EqualIgnoringSpans to ast"
      "fuzz replay: FAIL inputs=[0-9]+ violations=[1-9]"
    COMMAND $<TARGET_FILE:antb1-fuzz-replay> --mutate-unparse=suffix "${ANTB1_FUZZ_CORPUS_DIR}"
  )
  antb1_add_expect_test(
    harness.fuzz.replay.mutate_whitespace
    LABELS harness
    EXIT_CODE 1
    REQUIRE
      "VIOLATION [^ ]*/count_star_layout\\.sql: ToSql is not idempotent"
      "fuzz replay: FAIL inputs=[0-9]+ violations=[1-9]"
    FORBID "VIOLATION [^ ]*/count_star\\.sql:"
    COMMAND $<TARGET_FILE:antb1-fuzz-replay> --mutate-unparse=whitespace "${ANTB1_FUZZ_CORPUS_DIR}"
  )
  antb1_add_expect_test(
    harness.fuzz.replay.missing_path
    LABELS harness
    EXIT_CODE 2
    REQUIRE "no such file or directory"
    COMMAND $<TARGET_FILE:antb1-fuzz-replay> "${ANTB1_FUZZ_CORPUS_DIR}/does-not-exist"
  )
endif()

if(ANTB1_BUILD_FUZZERS)
  add_executable(antb1-sql-parser-fuzzer sql_parser_fuzzer.cc)
  target_compile_options(antb1-sql-parser-fuzzer PRIVATE -fsanitize=fuzzer-no-link)
  # ANTB1_FUZZ_ENGINE: -fsanitize=fuzzer (libFuzzer's main) or another engine's flags/library.
  target_link_libraries(
    antb1-sql-parser-fuzzer
    PRIVATE antb1::sql antb1::common antb1::build_options ${ANTB1_FUZZ_ENGINE}
  )

  if(ANTB1_BUILD_TESTS)
    # A short deterministic libFuzzer run (cmake/scripts/RunFuzzSmoke.cmake); `pixi run fuzz` is the long one.
    add_test(
      NAME fuzz.sql_parser.smoke
      COMMAND
        ${CMAKE_COMMAND} "-DFUZZER=$<TARGET_FILE:antb1-sql-parser-fuzzer>" "-DDICT=${ANTB1_FUZZ_DICT}"
        "-DCORPUS_DIR=${ANTB1_FUZZ_CORPUS_DIR}" "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/smoke-corpus/sql_parser"
        "-DARTIFACTS_DIR=${ANTB1_FUZZ_ARTIFACTS_DIR}" -P "${PROJECT_SOURCE_DIR}/cmake/scripts/RunFuzzSmoke.cmake"
    )
    set_tests_properties(fuzz.sql_parser.smoke PROPERTIES LABELS fuzz TIMEOUT 900)
  endif()
endif()
