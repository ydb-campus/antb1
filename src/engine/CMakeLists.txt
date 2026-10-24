antb1_add_module(
  engine
  SOURCES session.cc format.cc profile_format.cc
  PUBLIC_DEPS common plan
  PRIVATE_DEPS sql io exec
  PUBLIC_LIBS Arrow::arrow_${ANTB1_ARROW_LINKAGE}
  PRIVATE_LIBS ArrowCompute::arrow_compute_${ANTB1_ARROW_LINKAGE}
)

if(ANTB1_BUILD_TESTS)
  antb1_add_module_tests(
    engine
    SOURCES tests/session_test.cc tests/format_test.cc
    # exec: the profile nodes of the profile format tests
    LIBS antb1::exec Parquet::parquet_${ANTB1_ARROW_LINKAGE}
  )
endif()
