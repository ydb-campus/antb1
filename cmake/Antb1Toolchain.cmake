# Global, compiler-level configuration (no warnings here: see Antb1BuildOptions.cmake).
include_guard(GLOBAL)

option(ANTB1_USE_CCACHE "Use ccache when available" ON)
set(ANTB1_SANITIZE "" CACHE STRING "Sanitizers: 'address;undefined' or 'thread'")
option(ANTB1_COVERAGE "Clang source-based coverage instrumentation" OFF)
option(ANTB1_BUILD_FUZZERS "Build the libFuzzer targets in fuzz/ (Clang only)" OFF)
set(ANTB1_FUZZ_ENGINE "-fsanitize=fuzzer" CACHE STRING "Link flags or library of the fuzzing engine (fuzz targets)")
set(ANTB1_REQUIRE_COMPILER "" CACHE STRING "Fail unless CMAKE_CXX_COMPILER_ID matches (e.g. GNU)")

if(ANTB1_REQUIRE_COMPILER AND NOT CMAKE_CXX_COMPILER_ID MATCHES "${ANTB1_REQUIRE_COMPILER}")
  message(
    FATAL_ERROR
    "This preset needs a ${ANTB1_REQUIRE_COMPILER} compiler, got ${CMAKE_CXX_COMPILER_ID}. "
    "Run it through its pixi task (e.g. `pixi run ci-gcc`), never with a host compiler."
  )
endif()

if(CMAKE_CXX_FLAGS MATCHES "_GLIBCXX_DEBUG")
  message(FATAL_ERROR "_GLIBCXX_DEBUG changes the std:: ABI and breaks the prebuilt Arrow. Use _GLIBCXX_ASSERTIONS.")
endif()
if(CMAKE_CXX_FLAGS MATCHES "-stdlib=libc\\+\\+" AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "conda Arrow is built against libstdc++; never use -stdlib=libc++ on Linux.")
endif()

if(ANTB1_USE_CCACHE AND NOT CMAKE_CXX_COMPILER_LAUNCHER)
  find_program(ANTB1_CCACHE_EXE ccache)
  if(ANTB1_CCACHE_EXE)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${ANTB1_CCACHE_EXE}")
  endif()
endif()

# macOS: conda-forge's libc++ headers mark newer library functions (e.g. floating-point std::from_chars) as
# unavailable below the macOS release whose *system* libc++ ships them. antb1 links the pixi environment's own
# libc++ (rpath from the conda clang config), which provides them, so the markup does not apply; see
# https://conda-forge.org/docs/maintainer/knowledge_base/#newer-c-features-with-old-sdk. Builds against the system
# libc++ (no conda libc++ next to the compiler) keep Apple's availability checks.
if(APPLE AND DEFINED ENV{CONDA_PREFIX} AND EXISTS "$ENV{CONDA_PREFIX}/lib/libc++.1.dylib")
  add_compile_definitions(_LIBCPP_DISABLE_AVAILABILITY)
endif()

# Clang on Linux links with lld (faster than GNU ld; ships in the pixi env).
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(CMAKE_LINKER_TYPE LLD)
endif()

if(ANTB1_SANITIZE)
  if("thread" IN_LIST ANTB1_SANITIZE AND "address" IN_LIST ANTB1_SANITIZE)
    message(FATAL_ERROR "ANTB1_SANITIZE: 'thread' cannot be combined with 'address'")
  endif()
  list(JOIN ANTB1_SANITIZE "," _antb1_san)
  add_compile_options(-fsanitize=${_antb1_san} -fno-omit-frame-pointer -g)
  if("undefined" IN_LIST ANTB1_SANITIZE)
    # UBSan stays recoverable so suppressions work; UBSAN_OPTIONS=halt_on_error=1 still fails the test.
    add_compile_options(-fsanitize-recover=undefined)
    set(_antb1_ignorelist "${PROJECT_SOURCE_DIR}/tools/sanitizers/ubsan-ignorelist.txt")
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND EXISTS "${_antb1_ignorelist}")
      add_compile_options(-fsanitize-ignorelist=${_antb1_ignorelist})
    endif()
  endif()
  if("address" IN_LIST ANTB1_SANITIZE)
    add_compile_options(-fno-sanitize-recover=address)
  endif()
  add_link_options(-fsanitize=${_antb1_san})
endif()

if(ANTB1_COVERAGE)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "ANTB1_COVERAGE requires Clang (source-based coverage). Run `pixi run coverage`.")
  endif()
  add_compile_options(-fprofile-instr-generate -fcoverage-mapping)
  add_link_options(-fprofile-instr-generate)
endif()

# The libFuzzer instrumentation (-fsanitize=fuzzer-no-link) is added per target in fuzz/CMakeLists.txt (common,
# sql and the fuzz targets), never globally.
if(ANTB1_BUILD_FUZZERS AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "ANTB1_BUILD_FUZZERS requires Clang (libFuzzer). Run `pixi run fuzz-smoke` or `pixi run fuzz`.")
endif()
