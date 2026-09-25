# antb1::build_options: language level + warnings for OUR targets only (third-party headers are -isystem).
# Warnings become errors only via CMAKE_COMPILE_WARNING_AS_ERROR in the ci*/tidy presets.
# -Wno-missing-(designated-)field-initializers: designated initializers relying on member defaults are idiomatic here.
include_guard(GLOBAL)

add_library(antb1_build_options INTERFACE)
add_library(antb1::build_options ALIAS antb1_build_options)
target_compile_features(antb1_build_options INTERFACE cxx_std_23)
target_compile_options(
  antb1_build_options
  INTERFACE
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wnon-virtual-dtor
    -Wold-style-cast
    -Wcast-align
    -Woverloaded-virtual
    -Wconversion
    -Wsign-conversion
    -Wdouble-promotion
    -Wformat=2
    -Wimplicit-fallthrough
    -Wnull-dereference
    "$<$<CXX_COMPILER_ID:GNU>:-Wduplicated-cond;-Wduplicated-branches;-Wlogical-op;-Wuseless-cast;-Wno-dangling-reference;-Wno-missing-field-initializers>"
    "$<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wshadow-all;-Wextra-semi;-Wunreachable-code-aggressive;-Wno-missing-designated-field-initializers>"
)
# ABI-safe standard-library assertions in Debug. NEVER _GLIBCXX_DEBUG (breaks the ABI with prebuilt Arrow).
target_compile_definitions(
  antb1_build_options
  INTERFACE
    "$<$<CONFIG:Debug>:_GLIBCXX_ASSERTIONS>"
    "$<$<AND:$<CONFIG:Debug>,$<PLATFORM_ID:Darwin>>:_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_EXTENSIVE>"
)
