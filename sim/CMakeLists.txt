cmake_minimum_required(VERSION 3.18)
project(simulation LANGUAGES CXX)

# The deterministic cluster simulator (docs/design/simulator.md): header-only, no
# dependencies yet. It builds alone, so it can move into its own repository (D11, Q1).
add_library(simulation INTERFACE)
target_include_directories(simulation INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(simulation INTERFACE cxx_std_20)
add_library(simulation::simulation ALIAS simulation)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR OR SIMULATION_TESTS)
  enable_testing()
  foreach(t random engine checker)
    add_executable(simulation_${t}_test test/${t}_test.cc)
    target_link_libraries(simulation_${t}_test PRIVATE simulation)
    # D2: no contraction, so floating point rounds the same on every platform.
    target_compile_options(simulation_${t}_test PRIVATE -ffp-contract=off)
    add_test(NAME simulation_${t} COMMAND simulation_${t}_test)
  endforeach()
endif()
