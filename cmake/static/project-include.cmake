# CMAKE_PROJECT_INCLUDE of the static build (docs/adr/0033-standalone-static-binary.md). CMake includes it after
# every project() call: in antb1's `static` preset, in the Arrow build of scripts/static-arrow.sh and in every
# dependency Arrow bundles (Arrow forwards it to each ExternalProject; FetchContent projects get it through their
# project() call). Directory options reach every real target, on the compile and on the link line (the ThinLTO code
# generation at link time then gets the same CPU baseline), but never a configure check (try_compile) or CMake's
# compiler identification, which therefore behave exactly as without LTO.
# ANTB1_STATIC_CPU_FLAGS comes from cmake/static/toolchain.cmake. Directory options follow CMAKE_<LANG>_FLAGS and a
# target's COMPILE_FLAGS, so the baseline also wins over the -march that Arrow adds for its SIMD level (its per-file
# kernel flags still come last), and -O3 over a bundled project's own -O2 (utf8proc hard-codes it). Line tables
# (-gline-tables-only, which changes no code) let perf name inlined frames; `pixi run static` moves them into the
# separate .debug asset.
get_directory_property(_antb1_static_options COMPILE_OPTIONS)
string(FIND "${_antb1_static_options}" "-flto=thin" _antb1_static_found)
if(_antb1_static_found EQUAL -1) # not inherited from an enclosing project()
  add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:-O3;-flto=thin;-gline-tables-only>" ${ANTB1_STATIC_CPU_FLAGS})
  add_link_options(-flto=thin ${ANTB1_STATIC_CPU_FLAGS})
endif()
unset(_antb1_static_options)
unset(_antb1_static_found)
# Every project writes its compilation database, which scripts/static_gates.py reads to check -O3 and -flto=thin.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
