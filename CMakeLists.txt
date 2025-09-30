cmake_minimum_required(VERSION 3.10 FATAL_ERROR)

project(minisat)
cmake_policy(SET CMP0042 NEW)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
#--------------------------------------------------------------------------------------------------
# Configurable options:

option(STATIC_BINARIES "Link binaries statically." ON)
option(USE_SORELEASE   "Use SORELEASE in shared library filename." ON)

#--------------------------------------------------------------------------------------------------
# Library version:

set(MINISAT_SOMAJOR   2)
set(MINISAT_SOMINOR   1)
set(MINISAT_SORELEASE 0)

# Compute VERSION and SOVERSION:
if (USE_SORELEASE)
  set(MINISAT_VERSION ${MINISAT_SOMAJOR}.${MINISAT_SOMINOR}.${MINISAT_SORELEASE})
else()
  set(MINISAT_VERSION ${MINISAT_SOMAJOR}.${MINISAT_SOMINOR})
endif()
set(MINISAT_SOVERSION ${MINISAT_SOMAJOR})

#--------------------------------------------------------------------------------------------------
# Dependencies:

find_package(ZLIB)
# Find JsonCpp manually since find_package failed
find_path(JSONCPP_INCLUDE_DIR json/json.h PATHS /usr/include /usr/local/include)
find_library(JSONCPP_LIBRARY NAMES jsoncpp PATHS /usr/lib /usr/local/lib)

if (JSONCPP_INCLUDE_DIR AND JSONCPP_LIBRARY)
    message(STATUS "Found JsonCpp include: ${JSONCPP_INCLUDE_DIR}")
    message(STATUS "Found JsonCpp library: ${JSONCPP_LIBRARY}")
    include_directories(${JSONCPP_INCLUDE_DIR})
else()
    message(FATAL_ERROR "JsonCpp not found. Please install it or provide its path.")
endif()

include_directories(${ZLIB_INCLUDE_DIR})
include_directories(${minisat_SOURCE_DIR})

#--------------------------------------------------------------------------------------------------
# Compile flags:

add_definitions(-D__STDC_FORMAT_MACROS -D__STDC_LIMIT_MACROS)

#--------------------------------------------------------------------------------------------------
# Build Targets:

set(MINISAT_LIB_SOURCES
    src/utils/Options.cc
    src/utils/System.cc
    src/utils/ExternalWatcher.cc
    src/core/Solver.cc
    src/simp/SimpSolver.cc)

add_library(minisat-lib-static STATIC ${MINISAT_LIB_SOURCES})
target_link_libraries(minisat-lib-static ${ZLIB_LIBRARY} ${JSONCPP_LIBRARY}) # Link JsonCpp

add_library(minisat-lib-shared SHARED ${MINISAT_LIB_SOURCES})
target_link_libraries(minisat-lib-shared ${ZLIB_LIBRARY} ${JSONCPP_LIBRARY}) # Link JsonCpp

add_executable(minisat_core src/core/Main.cc)
add_executable(minisat_simp src/simp/Main.cc)

if(STATIC_BINARIES)
  target_link_libraries(minisat_core minisat-lib-static)
  target_link_libraries(minisat_simp minisat-lib-static)
else()
  target_link_libraries(minisat_core minisat-lib-shared)
  target_link_libraries(minisat_simp minisat-lib-shared)
endif()

set_target_properties(minisat-lib-static PROPERTIES OUTPUT_NAME "minisat")
set_target_properties(minisat-lib-shared
  PROPERTIES
    OUTPUT_NAME "minisat" 
    VERSION ${MINISAT_VERSION}
    SOVERSION ${MINISAT_SOVERSION})

set_target_properties(minisat_simp       PROPERTIES OUTPUT_NAME "minisat")

#--------------------------------------------------------------------------------------------------
# Installation targets:

install(TARGETS minisat-lib-static minisat-lib-shared minisat_core minisat_simp 
        RUNTIME DESTINATION bin
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib)

install(DIRECTORY src/mtl src/utils src/core src/simp
        DESTINATION include/minisat
        FILES_MATCHING PATTERN "*.h")
