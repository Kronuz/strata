# strata::placement's repair (quest placement, task repair; docs/mechanisms/placement-repair.md).
add_executable(placement_repair_plan_test "${CMAKE_CURRENT_SOURCE_DIR}/test/placement/repair_plan_test.cc")
target_link_libraries(placement_repair_plan_test PRIVATE strata::placement)
add_test(NAME placement_repair_plan COMMAND placement_repair_plan_test)
