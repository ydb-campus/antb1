antb1_add_module(
  exec
  SOURCES
    operator.cc
    row_mask.cc
    aggregate_state.cc
    table_scan.cc
    filter.cc
    project.cc
    scalar_aggregate.cc
    limit.cc
    row_count.cc
    physical_planner.cc
  PUBLIC_DEPS common plan
  PUBLIC_LIBS Arrow::arrow_shared
  PRIVATE_LIBS ArrowCompute::arrow_compute_shared
)

if(ANTB1_BUILD_TESTS)
  antb1_add_module_tests(
    exec
    SOURCES tests/aggregate_state_test.cc tests/operators_test.cc tests/row_count_test.cc tests/physical_planner_test.cc
    LIBS ArrowCompute::arrow_compute_shared
  )
endif()
