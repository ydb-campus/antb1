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

  # Data derived from TPC-H (ADR 0006): a separate duckdb CLI process loads TPC's dbgen from the pinned
  # duckdb-extension-tpch package by absolute path; the oracle itself never loads extensions. Both come from the
  # prefix that holds libduckdb, so their DuckDB versions match. Required wherever the oracle is required (ON).
  set(ANTB1_HAVE_TPCH OFF)
  if(ANTB1_HAVE_DUCKDB)
    get_filename_component(_antb1_duckdb_prefix "${ANTB1_DUCKDB_LIBRARY}" DIRECTORY)
    get_filename_component(_antb1_duckdb_prefix "${_antb1_duckdb_prefix}" DIRECTORY)
    find_program(ANTB1_DUCKDB_CLI duckdb PATHS "${_antb1_duckdb_prefix}/bin" NO_DEFAULT_PATH)
    file(GLOB _antb1_tpch_extension "${_antb1_duckdb_prefix}/duckdb/extensions/v*/*/tpch.duckdb_extension")
    list(LENGTH _antb1_tpch_extension _antb1_tpch_count)
    if(ANTB1_DUCKDB_CLI AND _antb1_tpch_count EQUAL 1)
      set(ANTB1_TPCH_EXTENSION "${_antb1_tpch_extension}")
      set(ANTB1_HAVE_TPCH ON)
      message(STATUS "antb1: TPC-H dbgen extension ${ANTB1_TPCH_EXTENSION}")
    elseif(ANTB1_WITH_DUCKDB STREQUAL "ON")
      message(
        FATAL_ERROR
        "ANTB1_WITH_DUCKDB=ON, but the duckdb CLI or exactly one tpch extension was not found in ${_antb1_duckdb_prefix}"
        " (pixi: duckdb-cli, duckdb-extension-tpch)"
      )
    else()
      message(STATUS "antb1: duckdb CLI or tpch extension not found; TPC-H-derived tests are not registered")
    endif()
  endif()
endif()
if(ANTB1_BUILD_BENCHMARKS AND NOT ANTB1_FUZZ_ONLY)
  find_package(benchmark CONFIG REQUIRED) # benchmark::benchmark
endif()
