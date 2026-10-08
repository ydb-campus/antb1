antb1_add_module(
  exec
  SOURCES
    operator.cc
    memory_budget.cc
    row_mask.cc
    double_key.cc
    aggregate_state.cc
    table_scan.cc
    filter.cc
    decimal.cc
    scan_filter.cc
    compute.cc
    parallel_compute.cc
    like.cc
    project.cc
    scalar_aggregate.cc
    aggregate_set.cc
    part_operators.cc
    part_pruning.cc
    partition_lanes.cc
    profile.cc
    profiled_operator.cc
    heavy_hitters.cc
    outer_groups.cc
    group_table.cc
    grouped_aggregate_state.cc
    group_aggregate.cc
    join_table.cc
    limit.cc
    gather.cc
    hash_join.cc
    sort.cc
    row_count.cc
    physical_planner.cc
  PUBLIC_DEPS common plan
  PUBLIC_LIBS Arrow::arrow_shared
  PRIVATE_LIBS ArrowCompute::arrow_compute_shared
)

if(ANTB1_BUILD_TESTS)
  antb1_add_module_tests(
    exec
    SOURCES
      tests/aggregate_state_test.cc
      tests/operators_test.cc
      tests/compute_test.cc
      tests/decimal_test.cc
      tests/parallel_compute_test.cc
      tests/row_count_test.cc
      tests/physical_planner_test.cc
      tests/grouped_aggregate_test.cc
      tests/sort_test.cc
      tests/like_test.cc
      tests/scan_filter_test.cc
      tests/parallel_test.cc
      tests/heavy_hitters_test.cc
      tests/join_table_test.cc
      tests/partition_lanes_test.cc
      tests/row_mask_test.cc
      tests/profile_test.cc
      tests/memory_test.cc
      tests/hash_join_test.cc
    LIBS ArrowCompute::arrow_compute_shared
  )
  # Replaces the global operator new and delete (an allocation-failure hook), so it is an executable of its own.
  # ThreadSanitizer's runtime defines them too: this one links without the sanitizer's C++ part, and the
  # sanitizer still sees every allocation, through malloc and free.
  antb1_add_gtest(
    antb1_exec_bad_alloc_tests
    LABEL unit
    PREFIX exec
    SOURCES tests/bad_alloc_test.cc
    LIBS antb1::exec ArrowCompute::arrow_compute_shared
  )
  if("thread" IN_LIST ANTB1_SANITIZE)
    target_link_options(antb1_exec_bad_alloc_tests PRIVATE -fno-sanitize-link-c++-runtime)
  endif()
endif()
