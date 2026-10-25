antb1_add_module(
  cli
  SOURCES cli.cc bench.cc memory_size.cc allocator.cc
  PUBLIC_DEPS common
  PRIVATE_DEPS engine
  PUBLIC_LIBS Arrow::arrow_${ANTB1_ARROW_LINKAGE}
  PRIVATE_LIBS CLI11::CLI11
)

# cpu_guard.cc is linked into the executable itself, not through the module's archive: nothing refers to its
# .preinit_array entry, which an archive member would need to be linked.
add_executable(antb1 main.cc cpu_guard.cc)
target_link_libraries(antb1 PRIVATE antb1::cli antb1::build_options)
if(ANTB1_ARROW_LINKAGE STREQUAL "static")
  antb1_link_standalone(antb1) # cmake/Antb1Toolchain.cmake
endif()

# A raised CPU baseline (cmake/static/toolchain.cmake) gets the CPU guard, which runs before any code compiled for
# it and is therefore compiled for the architecture's base ISA and outside the ThinLTO partition.
if(ANTB1_STATIC_CPU_FLAGS)
  list(JOIN ANTB1_STATIC_CPU_FLAGS " " _antb1_cpu)
  set(_antb1_base_isa -march=armv8-a)
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
    set(_antb1_base_isa -march=x86-64)
  endif()
  set_source_files_properties(
    cpu_guard.cc
    PROPERTIES COMPILE_OPTIONS "-fno-lto;${_antb1_base_isa}" COMPILE_DEFINITIONS "ANTB1_CPU_GUARD=\"${_antb1_cpu}\""
  )
endif()

if(ANTB1_BUILD_TESTS)
  antb1_add_module_tests(
    cli
    SOURCES tests/cli_test.cc tests/cpu_guard_test.cc cpu_guard.cc
    LIBS antb1::plan Parquet::parquet_${ANTB1_ARROW_LINKAGE}
  )
endif()
