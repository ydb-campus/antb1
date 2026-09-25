# Google Benchmark micro benchmarks (docs/benchmarks.md). `pixi run bench` builds them in the Release `bench`
# preset (tests OFF) and writes build/bench/micro.json. Presets with tests and ANTB1_BUILD_BENCHMARKS (ci-release)
# also run bench.micro.smoke (label bench-smoke): every benchmark once, so they keep building and running.
# Benchmark numbers never gate a PR.
add_executable(antb1-bench-micro micro_bench.cc)
target_link_libraries(
  antb1-bench-micro
  PRIVATE
    antb1::sql
    antb1::plan
    antb1::io
    antb1::exec
    antb1::common
    ArrowCompute::arrow_compute_shared
    Parquet::parquet_shared
    benchmark::benchmark
    antb1::build_options
)

if(ANTB1_BUILD_TESTS)
  # --benchmark_dry_run: one iteration of every benchmark. TMPDIR keeps BM_ScanColumn's file in the build tree.
  set(_tmp "${CMAKE_CURRENT_BINARY_DIR}/tmp")
  file(MAKE_DIRECTORY "${_tmp}")
  add_test(NAME bench.micro.smoke COMMAND antb1-bench-micro --benchmark_dry_run=true)
  set_tests_properties(
    bench.micro.smoke
    PROPERTIES LABELS bench-smoke TIMEOUT 300 ENVIRONMENT "TMPDIR=${_tmp}"
  )
endif()
