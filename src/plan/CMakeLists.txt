antb1_add_module(
  plan
  SOURCES
    types.cc
    catalog.cc
    sql_status.cc
    literal.cc
    scope.cc
    join_order.cc
    or_factoring.cc
    binder.cc
    logical_plan.cc
    optimizer.cc
    explain.cc
  PUBLIC_DEPS common sql
  PUBLIC_LIBS Arrow::arrow_shared
)

if(ANTB1_BUILD_TESTS)
  antb1_add_module_tests(
    plan
    SOURCES
      tests/types_test.cc
      tests/literal_test.cc
      tests/binder_test.cc
      tests/scope_test.cc
      tests/join_order_test.cc
      tests/or_factoring_test.cc
      tests/column_ids_test.cc
      tests/folding_test.cc
      tests/optimizer_test.cc
      tests/explain_test.cc
      tests/logical_plan_test.cc
      tests/sql_status_test.cc
  )
endif()
