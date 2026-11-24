# basic_index's file ledger (quest index-restructure A3; docs/mechanisms/index-layout.md), included
# from the top-level CMakeLists.txt with the other test/index/*.cmake files.
add_executable(index_ledger_test "${CMAKE_CURRENT_SOURCE_DIR}/test/index/ledger_test.cc")
target_link_libraries(index_ledger_test PRIVATE strata::core)
add_test(NAME index_ledger COMMAND index_ledger_test)
