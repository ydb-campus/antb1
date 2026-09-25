# Cross-module test suites; module unit tests live in src/<module>/tests/. Labels (cmake/Antb1Testing.cmake):
#   slt, oracle, diff, harness  tests/slt: sqllogictest runner, DuckDB oracle, random differential test
#   metamorphic                 tests/metamorphic: relations between antb1 answers
#   integration                 tests/integration: engine::Session end to end over the fixtures
#   cli                         tests/cli: golden outputs of the antb1 binary
#   harness                     tests/harness: fixture digest (and the self-tests of every layer)
add_subdirectory(slt)
add_subdirectory(metamorphic)
add_subdirectory(integration)
add_subdirectory(cli)
add_subdirectory(harness)
