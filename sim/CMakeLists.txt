cmake_minimum_required(VERSION 3.18)
project(simulation LANGUAGES CXX)

# The deterministic cluster simulator (docs/design/simulator.md): header-only, no
# dependencies yet. It builds alone, so it can move into its own repository (D11, Q1).
add_library(simulation INTERFACE)
target_include_directories(simulation INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(simulation INTERFACE cxx_std_20)
# D2: no contraction, so floating point rounds the same on every platform; every target that
# links the simulator compiles with it.
target_compile_options(simulation INTERFACE -ffp-contract=off)
add_library(simulation::simulation ALIAS simulation)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR OR SIMULATION_TESTS)
  enable_testing()
  foreach(t random engine checker network)
    add_executable(simulation_${t}_test test/${t}_test.cc)
    target_link_libraries(simulation_${t}_test PRIVATE simulation)
    add_test(NAME simulation_${t} COMMAND simulation_${t}_test)
  endforeach()

  # 2^32 events (SIM-R17); off by default, run in Release.
  option(SIMULATION_SLOW_TESTS "Build the simulator's slow tests" OFF)
  if(SIMULATION_SLOW_TESTS)
    add_executable(simulation_engine_wrap_test test/engine_wrap_test.cc)
    target_link_libraries(simulation_engine_wrap_test PRIVATE simulation)
    add_test(NAME simulation_engine_wrap COMMAND simulation_engine_wrap_test)
  endif()
endif()
