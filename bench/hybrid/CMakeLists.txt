# C1's strata arm (bench/hybrid/c1.py): a project of its own over strata's,
# built with `cmake -G Ninja -S bench/hybrid -B build-c1 -DCMAKE_BUILD_TYPE=Release`.
cmake_minimum_required(VERSION 3.18)
project(c1_hybrid LANGUAGES C CXX)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../.. strata EXCLUDE_FROM_ALL)
add_executable(c1_strata c1_strata.cc)
target_link_libraries(c1_strata PRIVATE strata::core)
