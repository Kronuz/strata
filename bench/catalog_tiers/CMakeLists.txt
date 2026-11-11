# The cold tier's gate rows through the catalog (docs/perf/catalog-tiers.md): a project of its own
# over strata's, built with `cmake -S bench/catalog_tiers`; the second build counts the heap.
cmake_minimum_required(VERSION 3.18)
project(catalog_tiers LANGUAGES C CXX)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../.. strata EXCLUDE_FROM_ALL)
add_executable(catalog_tiers catalog_tiers.cc)
add_executable(catalog_tiers_count catalog_tiers.cc)
target_compile_definitions(catalog_tiers_count PRIVATE IDLE_COUNT)
target_compile_options(catalog_tiers_count PRIVATE -fno-omit-frame-pointer)
# This tree's idle_index builds the fixtures (main's merges their segments).
add_executable(tiers_idle_index ../idle_index/idle_index.cc)
foreach(t catalog_tiers catalog_tiers_count tiers_idle_index)
  target_link_libraries(${t} PRIVATE strata::core)
endforeach()
if(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
  target_link_libraries(catalog_tiers_count PRIVATE execinfo)
endif()
