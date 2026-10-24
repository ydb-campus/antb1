# Arrow's options in the static build (docs/adr/0033-standalone-static-binary.md), loaded with `cmake -C` by
# scripts/static-arrow.sh. Only what antb1 uses: Arrow core, compute and Parquet with five codecs, RE2 (regexp_replace),
# utf8proc and mimalloc. Every dependency is BUNDLED, built from the local copies of cmake/static/arrow-sources.lock,
# except Boost's headers (Thrift needs no Boost library), which come from the environment. Leaving the rest out also
# keeps every user of NSS and dlopen (glog, the filesystems, HDFS, OpenTelemetry, backtrace) out of the static link.
set(CMAKE_BUILD_TYPE "Release" CACHE STRING "")
set(CMAKE_INSTALL_LIBDIR "lib" CACHE STRING "") # never lib64: antb1 finds the prefix by this layout

set(ARROW_BUILD_STATIC ON CACHE BOOL "")
set(ARROW_BUILD_SHARED OFF CACHE BOOL "")
set(ARROW_DEPENDENCY_SOURCE "BUNDLED" CACHE STRING "") # under pixi the default would be CONDA
set(ARROW_DEPENDENCY_USE_SHARED OFF CACHE BOOL "")
set(Boost_SOURCE "SYSTEM" CACHE STRING "")

set(ARROW_COMPUTE ON CACHE BOOL "")
set(ARROW_IPC ON CACHE BOOL "") # Parquet needs it
set(ARROW_PARQUET ON CACHE BOOL "")
set(PARQUET_REQUIRE_ENCRYPTION OFF CACHE BOOL "")
set(ARROW_WITH_SNAPPY ON CACHE BOOL "")
set(ARROW_WITH_ZSTD ON CACHE BOOL "")
set(ARROW_WITH_LZ4 ON CACHE BOOL "")
set(ARROW_WITH_ZLIB ON CACHE BOOL "")
set(ARROW_WITH_BROTLI ON CACHE BOOL "")
set(ARROW_WITH_BZ2 OFF CACHE BOOL "")
set(ARROW_WITH_RE2 ON CACHE BOOL "")
set(ARROW_WITH_UTF8PROC ON CACHE BOOL "")
set(ARROW_MIMALLOC ON CACHE BOOL "") # the default memory pool (ADR 0017 keeps its memory)
set(ARROW_JEMALLOC OFF CACHE BOOL "")

foreach(
  _antb1_off
  IN
  ITEMS
    ARROW_ACERO
    ARROW_AZURE
    ARROW_CSV
    ARROW_CUDA
    ARROW_DATASET
    ARROW_FILESYSTEM
    ARROW_FLIGHT
    ARROW_FLIGHT_SQL
    ARROW_GANDIVA
    ARROW_GCS
    ARROW_HDFS
    ARROW_JSON
    ARROW_ORC
    ARROW_PYTHON
    ARROW_S3
    ARROW_SUBSTRAIT
    ARROW_TENSORFLOW
    ARROW_TESTING
    ARROW_USE_GLOG
    ARROW_WITH_BACKTRACE
    ARROW_WITH_OPENTELEMETRY
    ARROW_BUILD_BENCHMARKS
    ARROW_BUILD_EXAMPLES
    ARROW_BUILD_INTEGRATION
    ARROW_BUILD_TESTS
    ARROW_BUILD_UTILITIES
    ARROW_BUILD_FUZZING_UTILITIES
    ARROW_FUZZING
    PARQUET_BUILD_EXAMPLES
    PARQUET_BUILD_EXECUTABLES
    ARROW_USE_CCACHE
    ARROW_USE_SCCACHE
)
  set(${_antb1_off} OFF CACHE BOOL "")
endforeach()
unset(_antb1_off)

# ARROW_SIMD_LEVEL (the kernels compiled without dispatch) is set per target by scripts/static-arrow.sh; the kernels
# chosen at run time go up to what the CPU has.
set(ARROW_RUNTIME_SIMD_LEVEL "MAX" CACHE STRING "")

# Arrow replaces CMake's -O3 by -O2 (cpp/cmake_modules/SetupCxxFlags.cmake) and appends only these flags after
# that, also for the dependencies it bundles: the static build is -O3 throughout.
set(ARROW_C_FLAGS_RELEASE "-O3" CACHE STRING "")
set(ARROW_CXX_FLAGS_RELEASE "-O3" CACHE STRING "")

# Build identity, read back through arrow::GetBuildInfo() (`antb1 version --verbose`). Without ARROW_GIT_ID, Arrow
# runs `git log` in whatever repository encloses its source tree.
set(ARROW_GIT_ID "59bea6ec485e7fe351d1aa6753f964f6a6bc353a" CACHE STRING "") # tag apache-arrow-25.0.0
set(ARROW_GIT_DESCRIPTION "apache-arrow-25.0.0" CACHE STRING "")
set(ARROW_PACKAGE_KIND "antb1-static" CACHE STRING "")
