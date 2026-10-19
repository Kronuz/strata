# durability-modes' T1 and T2 probes, built apart from strata's own targets:
#   cmake -S bench/durability -B build-durability -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-durability --target count_syncs sizing syncspy
# strata comes from this checkout; pass -DFETCHCONTENT_SOURCE_DIR_<NAME>=<path> to reuse
# dependency checkouts at strata's pins.
cmake_minimum_required(VERSION 3.18)
project(durability_probes LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../.. strata EXCLUDE_FROM_ALL)
add_executable(count_syncs count_syncs.cc)
target_link_libraries(count_syncs PRIVATE strata::strata)
add_executable(sizing sizing.cc)
target_link_libraries(sizing PRIVATE strata::strata)
# macOS only: counts fsync and F_FULLFSYNC through DYLD_INSERT_LIBRARIES.
if(APPLE)
  add_library(syncspy SHARED syncspy.c)
endif()
