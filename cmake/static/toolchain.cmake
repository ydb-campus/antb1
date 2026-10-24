# Toolchain file of the static build (docs/adr/0033-standalone-static-binary.md). scripts/static-arrow.sh passes it
# to the Arrow build, Arrow forwards it to every dependency it bundles (ExternalProject), CMake reads it again for
# every try_compile, and antb1's `static` preset uses it. The compilers are the pixi environment's (CC, CXX).

# The CPU baseline of the release binary, compiled into antb1, Arrow and every bundled dependency through
# cmake/static/project-include.cmake. Arrow still picks its AVX2, AVX-512 and SVE kernels at run time.
# The environment variable ANTB1_STATIC_CPU_FLAGS overrides it (scripts/static-arrow.sh keys the prefix on it).
if(DEFINED ENV{ANTB1_STATIC_CPU_FLAGS})
  separate_arguments(ANTB1_STATIC_CPU_FLAGS UNIX_COMMAND "$ENV{ANTB1_STATIC_CPU_FLAGS}")
elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
  set(ANTB1_STATIC_CPU_FLAGS -march=x86-64-v3) # AVX2, BMI2, FMA: Haswell and newer
elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
  set(ANTB1_STATIC_CPU_FLAGS -march=armv8.2-a) # LSE atomics: Neoverse N1 (Graviton 2) and newer
else()
  set(ANTB1_STATIC_CPU_FLAGS "") # macOS arm64: Clang's default, apple-m1
endif()

if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
  # lld reads LLVM bitcode (the ThinLTO code generation runs at link time); try_compile uses it too. binutils' ar
  # cannot index bitcode members, and Arrow merges its bundled dependencies with `${CMAKE_AR} -M`.
  set(CMAKE_LINKER_TYPE LLD)
  set(CMAKE_AR "$ENV{CONDA_PREFIX}/bin/llvm-ar" CACHE FILEPATH "Archiver that indexes LLVM bitcode" FORCE)
  set(CMAKE_RANLIB "$ENV{CONDA_PREFIX}/bin/llvm-ranlib" CACHE FILEPATH "ranlib for LLVM bitcode" FORCE)
elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
  # Apple ships no static executables; the binary links only system libraries, from this macOS release on. Arrow
  # does not forward the deployment target to its bundled dependencies, so scripts/static-arrow.sh also exports
  # MACOSX_DEPLOYMENT_TARGET.
  set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Oldest macOS release the binary runs on" FORCE)
  # The environment's cctools are built on the same LLVM, so they read its bitcode. Arrow merges its bundled
  # dependencies with Apple's libtool and checks that `libtool -V` names cctools; conda installs it prefixed.
  foreach(_antb1_tool IN ITEMS ar ranlib libtool)
    string(TOUPPER "${_antb1_tool}" _antb1_var)
    find_program(
      ANTB1_STATIC_${_antb1_var}
      NAMES arm64-apple-darwin20.0.0-${_antb1_tool} ${_antb1_tool}
      HINTS "$ENV{CONDA_PREFIX}/bin"
      NO_DEFAULT_PATH
      REQUIRED
    )
    set(CMAKE_${_antb1_var} "${ANTB1_STATIC_${_antb1_var}}" CACHE FILEPATH "cctools ${_antb1_tool}" FORCE)
  endforeach()
  unset(_antb1_tool)
  unset(_antb1_var)
endif()
