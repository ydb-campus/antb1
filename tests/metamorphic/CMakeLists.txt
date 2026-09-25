# Metamorphic tests (label metamorphic): relations.cc lists the relations; relations whose features are not
# all in tests/slt/supported_features.h are pending (reported as skipped, and must still get Unsupported).
antb1_add_fixture_gtest(
  antb1_metamorphic_tests
  LABEL metamorphic
  PREFIX metamorphic
  SOURCES relations.cc metamorphic_test.cc
  LIBS antb1_slt_lib Parquet::parquet_shared
)
