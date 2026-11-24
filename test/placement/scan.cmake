# The placement records' seekable scan against a brute-force filter, and its work bound (placement A2).
add_executable(placement_scan_test test/placement/scan_test.cc)
target_link_libraries(placement_scan_test PRIVATE strata::placement)
add_test(NAME placement_scan COMMAND placement_scan_test)
