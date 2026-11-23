cmake_minimum_required(VERSION 3.18)
project(simulation LANGUAGES CXX)

# The deterministic cluster simulator (docs/mechanisms/simulator.md): header-only, no
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
  # Warnings are errors on the simulator's own targets only, never on what links it (SIM-R25).
  set(simulation_warnings -Wall -Wextra -Werror)
  foreach(t random engine checker history network)
    add_executable(simulation_${t}_test test/${t}_test.cc)
    target_link_libraries(simulation_${t}_test PRIVATE simulation)
    target_compile_options(simulation_${t}_test PRIVATE ${simulation_warnings})
    add_test(NAME simulation_${t} COMMAND simulation_${t}_test)
  endforeach()

  # sim_check HISTORY (D12): 0 linearizable, 1 violation, 2 inconclusive, 3 malformed.
  add_executable(sim_check tools/sim_check.cc)
  target_link_libraries(sim_check PRIVATE simulation)
  target_compile_options(sim_check PRIVATE ${simulation_warnings})
  add_test(NAME simulation_sim_check_linearizable COMMAND sim_check "${CMAKE_CURRENT_SOURCE_DIR}/test/histories/linearizable.history")
  add_test(NAME simulation_sim_check_violation COMMAND sim_check "${CMAKE_CURRENT_SOURCE_DIR}/test/histories/violation.history")
  set_tests_properties(simulation_sim_check_violation PROPERTIES WILL_FAIL TRUE)

  # sim_run SEED...: a register server and its clients over the faulty network, the history
  # checked. Five seeds are linearizable; with a lost write planted, the checker rejects each;
  # one seed run twice in two processes writes one history.
  add_executable(sim_run tools/sim_run.cc)
  target_link_libraries(sim_run PRIVATE simulation)
  target_compile_options(sim_run PRIVATE ${simulation_warnings})
  add_test(NAME simulation_run COMMAND sim_run 1 2 3 4 5)
  add_test(NAME simulation_run_lost_write COMMAND sim_run --plant lost-write --expect violation 1 2 3 4 5)
  add_test(NAME simulation_run_same_history COMMAND ${CMAKE_COMMAND} -DRUN=$<TARGET_FILE:sim_run> -DSEED=7
    -DOUT=${CMAKE_CURRENT_BINARY_DIR}/same_history -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/same_history.cmake)

  # strata::group's rows (quest group-heal A5, docs/gates/phase-3r.md), when strata adds the
  # simulator: the voters' world (test/group/world.hh) through every fault class at a short budget,
  # its histories checked here. Named simulation_group_ so the group suite's ^group_ skips them.
  if(TARGET strata::group)
    add_executable(group_sim_test ${CMAKE_CURRENT_SOURCE_DIR}/../test/group/sim_test.cc)
    target_link_libraries(group_sim_test PRIVATE simulation strata::group faultlab::faultlab)
    # Beside strata's other group tests, where test/group/mutants.py runs its target.
    set_target_properties(group_sim_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    foreach(row faults_3 faults_5 barriers_3 barriers_5 membership_3 membership_5 growth_1 device_3)
      string(REGEX MATCH "^[a-z]+" name ${row})
      string(REGEX MATCH "[0-9]+$" voters ${row})
      add_test(NAME simulation_group_${row} COMMAND group_sim_test ${name} ${voters})
      set_tests_properties(simulation_group_${row} PROPERTIES TIMEOUT 600)
    endforeach()
    # A3's simulator half: corruption of every file class, answered by repair.
    add_test(NAME simulation_group_corruption COMMAND group_sim_test corruption)
    set_tests_properties(simulation_group_corruption PROPERTIES TIMEOUT 600)
  endif()

  # 2^32 events (SIM-R17); off by default, run in Release.
  option(SIMULATION_SLOW_TESTS "Build the simulator's slow tests" OFF)
  if(SIMULATION_SLOW_TESTS)
    add_executable(simulation_engine_wrap_test test/engine_wrap_test.cc)
    target_link_libraries(simulation_engine_wrap_test PRIVATE simulation)
    target_compile_options(simulation_engine_wrap_test PRIVATE ${simulation_warnings})
    add_test(NAME simulation_engine_wrap COMMAND simulation_engine_wrap_test)
  endif()
endif()
