# Cross-module test suites; module unit tests live in src/<module>/tests/. Labels (cmake/Antb1Testing.cmake):
#   slt, oracle, diff, harness  tests/slt: sqllogictest runner, DuckDB oracle, random differential test
#   metamorphic                 tests/metamorphic: relations between antb1 answers
#   integration                 tests/integration: engine::Session end to end over the fixtures
#   cli                         tests/cli: golden outputs of the antb1 binary
#   harness                     tests/harness: fixture digest (and the self-tests of every layer)
#   data                        tests/data: ClickBench data tests (network data, redacted; not hermetic)
#   setup, harness, diff,       tests/tpch: data derived from TPC-H, generated at test time by the pinned dbgen
#   parallel                    (fixtures.tpch), its answers and differential tests, redacted
add_subdirectory(slt)
add_subdirectory(metamorphic)
add_subdirectory(integration)
add_subdirectory(cli)
add_subdirectory(harness)
add_subdirectory(data)
add_subdirectory(tpch)
