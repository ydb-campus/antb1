# Architecture allow-list: the single source of truth for module dependencies.
# Changing an edge is an architecture decision: write an ADR (docs/adr/) and update docs/architecture.md
# (tools/lint/check_repo.py R007 keeps them in sync).
include_guard(GLOBAL)

set(
  ANTB1_MODULES
  common
  sql
  plan
  io
  exec
  engine
  cli
)

set(ANTB1_DEPS_common "")
set(ANTB1_DEPS_sql "common")
set(ANTB1_DEPS_plan "common;sql")
set(ANTB1_DEPS_io "common;plan")
set(ANTB1_DEPS_exec "common;plan") # never io: exec scans through plan::Table
set(ANTB1_DEPS_engine "common;sql;plan;io;exec")
set(ANTB1_DEPS_cli "common;engine")

set(ANTB1_EXT_common "")
set(ANTB1_EXT_sql "")
set(ANTB1_EXT_plan "Arrow::arrow_shared")
set(ANTB1_EXT_io "Arrow::arrow_shared;Parquet::parquet_shared")
set(ANTB1_EXT_exec "Arrow::arrow_shared;ArrowCompute::arrow_compute_shared")
set(ANTB1_EXT_engine "Arrow::arrow_shared;ArrowCompute::arrow_compute_shared")
set(ANTB1_EXT_cli "Arrow::arrow_shared;CLI11::CLI11")

# antb1_add_module(<name> SOURCES ... [PUBLIC_DEPS ...] [PRIVATE_DEPS ...] [PUBLIC_LIBS ...] [PRIVATE_LIBS ...])
# Creates STATIC library antb1_<name> (alias antb1::<name>) with public headers in include/antb1/<name>/.
function(antb1_add_module name)
  cmake_parse_arguments(ARG "" "" "SOURCES;PUBLIC_DEPS;PRIVATE_DEPS;PUBLIC_LIBS;PRIVATE_LIBS" ${ARGN})
  if(NOT name IN_LIST ANTB1_MODULES)
    message(FATAL_ERROR "antb1: unknown module '${name}' (cmake/Antb1Modules.cmake: ANTB1_MODULES)")
  endif()
  foreach(dep IN LISTS ARG_PUBLIC_DEPS ARG_PRIVATE_DEPS)
    if(NOT dep IN_LIST ANTB1_DEPS_${name})
      message(
        FATAL_ERROR
        "antb1: module '${name}' must not depend on '${dep}' (allowed: '${ANTB1_DEPS_${name}}'). "
        "Changing module edges is an architecture decision: write an ADR and update cmake/Antb1Modules.cmake."
      )
    endif()
  endforeach()
  foreach(lib IN LISTS ARG_PUBLIC_LIBS ARG_PRIVATE_LIBS)
    if(NOT lib IN_LIST ANTB1_EXT_${name})
      message(FATAL_ERROR "antb1: module '${name}' must not link '${lib}' (allowed: '${ANTB1_EXT_${name}}')")
    endif()
  endforeach()
  list(TRANSFORM ARG_PUBLIC_DEPS PREPEND "antb1::" OUTPUT_VARIABLE _public)
  list(TRANSFORM ARG_PRIVATE_DEPS PREPEND "antb1::" OUTPUT_VARIABLE _private)
  add_library(antb1_${name} STATIC ${ARG_SOURCES})
  add_library(antb1::${name} ALIAS antb1_${name})
  target_include_directories(
    antb1_${name}
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
  )
  target_link_libraries(
    antb1_${name}
    PUBLIC ${_public} ${ARG_PUBLIC_LIBS}
    PRIVATE antb1::build_options ${_private} ${ARG_PRIVATE_LIBS}
  )
  set_property(GLOBAL APPEND PROPERTY ANTB1_BUILT_MODULES ${name})
endfunction()

# antb1_check_module_graph(): re-walk every built module's link libraries; FATAL on a stray antb1:: edge
# (catches target_link_libraries() calls that bypass antb1_add_module()).
function(antb1_check_module_graph)
  get_property(_built GLOBAL PROPERTY ANTB1_BUILT_MODULES)
  foreach(name IN LISTS _built)
    get_target_property(_libs antb1_${name} LINK_LIBRARIES)
    get_target_property(_ilibs antb1_${name} INTERFACE_LINK_LIBRARIES)
    foreach(lib IN LISTS _libs _ilibs)
      if(lib MATCHES "^antb1::(.+)$" AND NOT CMAKE_MATCH_1 STREQUAL "build_options")
        if(NOT CMAKE_MATCH_1 IN_LIST ANTB1_DEPS_${name})
          message(FATAL_ERROR "antb1: module '${name}' links antb1::${CMAKE_MATCH_1}, not in its allow-list")
        endif()
      endif()
    endforeach()
  endforeach()
endfunction()
