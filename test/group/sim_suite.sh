#!/bin/sh
# Builds strata::group's simulator rows and runs them (ctest simulation_group_), each row timed.
#   test/group/sim_suite.sh BUILD_DIR [CTEST_REGEX]
# A failed configure or build exits 127, so a `--fails-without` job passes only on a failed test.
root=$(cd "$(dirname "$0")/../.." && pwd)
build=$1
regex=${2:-^simulation_group_}
cmake -G Ninja -S "$root" -B "$build/strata" -DCMAKE_BUILD_TYPE=Release >/dev/null || exit 127
cmake --build "$build/strata" -j4 --target group_sim_test || exit 127
cd "$build/strata" || exit 127
exec ctest -R "$regex" --no-tests=error --output-on-failure
