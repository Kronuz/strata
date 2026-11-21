# strata::placement's repair (quest placement, task repair; docs/mechanisms/placement-repair.md).
add_executable(placement_repair_plan_test "${CMAKE_CURRENT_SOURCE_DIR}/test/placement/repair_plan_test.cc")
target_link_libraries(placement_repair_plan_test PRIVATE strata::placement)
add_test(NAME placement_repair_plan COMMAND placement_repair_plan_test)
add_executable(placement_repair_group_test "${CMAKE_CURRENT_SOURCE_DIR}/test/placement/repair_group_test.cc")
target_link_libraries(placement_repair_group_test PRIVATE strata::placement)
add_test(NAME placement_repair_group COMMAND placement_repair_group_test)
set_tests_properties(placement_repair_group PROPERTIES TIMEOUT 1500)
