# Third-party packages. Plain find_package: pixi provides them via CMAKE_PREFIX_PATH=$CONDA_PREFIX,
# but any installation (e.g. Arrow APT packages) works.
include_guard(GLOBAL)

# ANTB1_FUZZ_ONLY builds only the Arrow-free modules (common, sql) and fuzz/: none of these libraries is needed.
if(NOT ANTB1_FUZZ_ONLY)
  # ArrowConfigVersion uses SameMajorVersion: never pass a version here, check a minimum instead.
  find_package(Arrow CONFIG REQUIRED)
  if(Arrow_VERSION VERSION_LESS 21)
    message(FATAL_ERROR "antb1 needs Apache Arrow >= 21 (libarrow_compute split); found ${Arrow_VERSION}. CI uses 25.x")
  endif()
  find_package(ArrowCompute CONFIG REQUIRED) # ArrowCompute::arrow_compute_shared (kernels; call compute::Initialize())
  find_package(Parquet CONFIG REQUIRED) # Parquet::parquet_shared
  find_package(CLI11 CONFIG REQUIRED) # CLI11::CLI11
  message(STATUS "antb1: Arrow ${Arrow_VERSION} (${Arrow_DIR})")
endif()

if(ANTB1_BUILD_TESTS)
  find_package(GTest CONFIG REQUIRED) # GTest::gtest_main, GTest::gmock
endif()

# DuckDB C API: the TEST ORACLE ONLY (tests/slt). Never linked into src/: the module allow-list in
# cmake/Antb1Modules.cmake has no DuckDB entry. AUTO uses it when found; ON requires it (every preset: pixi
# always provides it, so the oracle, diff and harness.diff tests can never silently disappear); OFF skips
# the oracle tests (slt tests still run on antb1). Defined for every build, so presets can always set it.
set(ANTB1_WITH_DUCKDB "AUTO" CACHE STRING "DuckDB test oracle: AUTO, ON or OFF")
set_property(CACHE ANTB1_WITH_DUCKDB PROPERTY STRINGS AUTO ON OFF)
if(NOT ANTB1_WITH_DUCKDB MATCHES "^(AUTO|ON|OFF)$")
  message(FATAL_ERROR "ANTB1_WITH_DUCKDB must be AUTO, ON or OFF (got '${ANTB1_WITH_DUCKDB}')")
endif()
if(ANTB1_BUILD_TESTS AND NOT ANTB1_FUZZ_ONLY)
  set(ANTB1_HAVE_DUCKDB OFF)
  if(NOT ANTB1_WITH_DUCKDB STREQUAL "OFF")
    find_path(ANTB1_DUCKDB_INCLUDE_DIR duckdb.h)
    find_library(ANTB1_DUCKDB_LIBRARY duckdb)
    if(ANTB1_DUCKDB_INCLUDE_DIR AND ANTB1_DUCKDB_LIBRARY)
      add_library(antb1_ext_duckdb INTERFACE IMPORTED)
      target_include_directories(antb1_ext_duckdb SYSTEM INTERFACE "${ANTB1_DUCKDB_INCLUDE_DIR}")
      target_link_libraries(antb1_ext_duckdb INTERFACE "${ANTB1_DUCKDB_LIBRARY}")
      target_compile_definitions(antb1_ext_duckdb INTERFACE DUCKDB_API_NO_DEPRECATED)
      set(ANTB1_HAVE_DUCKDB ON)
      message(STATUS "antb1: DuckDB test oracle ${ANTB1_DUCKDB_LIBRARY}")
    elseif(ANTB1_WITH_DUCKDB STREQUAL "ON")
      message(FATAL_ERROR "ANTB1_WITH_DUCKDB=ON, but duckdb.h or libduckdb was not found (pixi: libduckdb-devel)")
    else()
      message(STATUS "antb1: DuckDB not found; oracle tests are not registered (ANTB1_WITH_DUCKDB=AUTO)")
    endif()
  else()
    message(STATUS "antb1: DuckDB oracle disabled; oracle tests are not registered (ANTB1_WITH_DUCKDB=OFF)")
  endif()
endif()
if(ANTB1_BUILD_BENCHMARKS AND NOT ANTB1_FUZZ_ONLY)
  find_package(benchmark CONFIG REQUIRED) # benchmark::benchmark
endif()
