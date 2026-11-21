# Google Benchmark micro benchmarks (docs/benchmarks.md). `pixi run bench` builds them in preset `static` (against
# the static Arrow) and writes micro.json into build/static/bench-results. Presets with tests and
# ANTB1_BUILD_BENCHMARKS (ci-release, static) also run the bench-smoke tests: every benchmark once, so
# they keep building and running.
# Benchmark numbers never gate a PR.
add_executable(antb1-bench-micro micro_bench.cc)
# Key-table comparisons exercise the same private factory that GroupTable uses.
target_include_directories(antb1-bench-micro PRIVATE ${PROJECT_SOURCE_DIR}/src/exec)
target_link_libraries(
  antb1-bench-micro
  PRIVATE
    antb1::sql
    antb1::plan
    antb1::io
    antb1::exec
    antb1::common
    ArrowCompute::arrow_compute_${ANTB1_ARROW_LINKAGE}
    Parquet::parquet_${ANTB1_ARROW_LINKAGE}
    benchmark::benchmark
    antb1::build_options
)

# Prototype measurements for grouping sets (ADR 0038's Evidence): not run by any test.
add_executable(antb1-bench-gsets grouping_sets_bench.cc)
target_link_libraries(
  antb1-bench-gsets
  PRIVATE antb1::plan antb1::exec antb1::common benchmark::benchmark antb1::build_options
)
# It times src/exec's parallel operators directly, which no public header declares.
target_include_directories(antb1-bench-gsets PRIVATE ${PROJECT_SOURCE_DIR}/src/exec)

# Prototype measurements for set operations (ADR 0039's X4s update): not run by any test.
add_executable(antb1-bench-setops set_operation_bench.cc)
target_link_libraries(
  antb1-bench-setops
  PRIVATE antb1::plan antb1::exec antb1::common benchmark::benchmark antb1::build_options
)
# It times src/exec's SetOperationOperator and UnionAllOperator, which no public header declares.
target_include_directories(antb1-bench-setops PRIVATE ${PROJECT_SOURCE_DIR}/src/exec)

if(ANTB1_BUILD_TESTS)
  # --benchmark_dry_run: one iteration of every benchmark. TMPDIR keeps BM_ScanColumn's file in the build tree.
  set(_tmp "${CMAKE_CURRENT_BINARY_DIR}/tmp")
  file(MAKE_DIRECTORY "${_tmp}")
  # The full grid exceeds one smoke test's timeout. Google's leading "-" selects the complement,
  # so new benchmark names also land in exactly one shard. Both run their usual full-size inputs.
  set(_local_groups "^BM_GroupProgram/[^/]+/(Single|Partitioned|Hybrid[^/]*)/")
  add_test(
    NAME bench.micro.smoke.local-groups
    COMMAND antb1-bench-micro --benchmark_dry_run=true "--benchmark_filter=${_local_groups}"
  )
  add_test(
    NAME bench.micro.smoke.other
    COMMAND antb1-bench-micro --benchmark_dry_run=true "--benchmark_filter=-${_local_groups}"
  )
  set_tests_properties(
    bench.micro.smoke.local-groups
    bench.micro.smoke.other
    PROPERTIES
      LABELS bench-smoke
      TIMEOUT 300
      RESOURCE_LOCK antb1_bench_micro
      ENVIRONMENT "TMPDIR=${_tmp}"
      FAIL_REGULAR_EXPRESSION "Failed to match any benchmarks|ERROR OCCURRED"
  )
endif()
