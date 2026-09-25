cmake_minimum_required(VERSION 3.28...4.4)
project(
  antb1
  VERSION 0.1.0
  DESCRIPTION "Experimental C++23 analytics engine over Parquet (Apache Arrow)"
  HOMEPAGE_URL "https://github.com/ydb-campus/antb1"
  LANGUAGES CXX
)

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
  message(FATAL_ERROR "In-source builds are not supported. Use: pixi run build")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF) # no C++20 modules -> no clang-scan-deps
set(CMAKE_EXPORT_COMPILE_COMMANDS ON) # clangd, clang-tidy, agents
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

option(ANTB1_BUILD_TESTS "Build tests" ${PROJECT_IS_TOP_LEVEL})
option(ANTB1_BUILD_BENCHMARKS "Build Google Benchmark micro benchmarks" OFF)
option(ANTB1_CLANG_TIDY "Run clang-tidy on every translation unit" OFF)

include(Antb1Toolchain) # global flags: ccache, linker, sanitizers, coverage, compiler guard
include(Antb1BuildOptions) # antb1::build_options (warnings) for our targets only
include(Antb1Dependencies) # find_package(Arrow, ArrowCompute, Parquet, CLI11, GTest, ...)
include(Antb1Modules) # architecture allow-list + antb1_add_module()

if(ANTB1_BUILD_TESTS)
  enable_testing()
  include(GoogleTest)
  set(CMAKE_GTEST_DISCOVER_TESTS_DISCOVERY_MODE PRE_TEST)
  include(Antb1Testing)
endif()

if(ANTB1_CLANG_TIDY)
  find_program(ANTB1_CLANG_TIDY_EXE NAMES clang-tidy REQUIRED)
  set(CMAKE_CXX_CLANG_TIDY "${ANTB1_CLANG_TIDY_EXE}")
endif()

foreach(module IN LISTS ANTB1_MODULES)
  add_subdirectory(src/${module})
endforeach()

if(ANTB1_BUILD_TESTS)
  add_subdirectory(tests)
endif()

antb1_check_module_graph()
