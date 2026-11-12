#!/bin/sh
# usage: booking_load.sh BUILD_DIR [RUNS] [ROUNDS]   runs resource_budget_test's racing booking cases
# (booking_falls, two_droppers) RUNS times at ROUNDS rounds each; prints each run's two-droppers line
# and exits 1 when any run failed
dir=$1 runs=${2:-5} rounds=${3:-2000}
fail=0 i=0
while [ $i -lt $runs ]; do
	if out=$(cd "$dir" && BOOKING_ONLY=1 ROUNDS=$rounds ./resource_budget_test 2>&1); then :; else fail=$((fail + 1)); fi
	echo "run $i: $(echo "$out" | tr '\n' ' ')"
	i=$((i + 1))
done
echo "booking result: $fail failures of $runs runs ($rounds rounds each)"
[ $fail -eq 0 ]
