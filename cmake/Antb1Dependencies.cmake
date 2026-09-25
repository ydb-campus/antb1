# Third-party packages. Plain find_package: pixi provides them via CMAKE_PREFIX_PATH=$CONDA_PREFIX,
# but any installation (e.g. Arrow APT packages) works.
include_guard(GLOBAL)

# ArrowConfigVersion uses SameMajorVersion: never pass a version here, check a minimum instead.
find_package(Arrow CONFIG REQUIRED)
if(Arrow_VERSION VERSION_LESS 21)
  message(FATAL_ERROR "antb1 needs Apache Arrow >= 21 (libarrow_compute split); found ${Arrow_VERSION}. CI uses 25.x")
endif()
find_package(ArrowCompute CONFIG REQUIRED) # ArrowCompute::arrow_compute_shared (kernels; call compute::Initialize())
find_package(Parquet CONFIG REQUIRED) # Parquet::parquet_shared
find_package(CLI11 CONFIG REQUIRED) # CLI11::CLI11
message(STATUS "antb1: Arrow ${Arrow_VERSION} (${Arrow_DIR})")

if(ANTB1_BUILD_TESTS)
  find_package(GTest CONFIG REQUIRED) # GTest::gtest_main, GTest::gmock
endif()
if(ANTB1_BUILD_BENCHMARKS)
  find_package(benchmark CONFIG REQUIRED) # benchmark::benchmark
endif()
