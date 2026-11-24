#!/bin/sh
# Builds strata::group's whole suite (the group_tests target: every group_*_test and
# group_sim_test) and runs ctest's group_ tests and simulator rows, or those CTEST_REGEX names.
# CONSENSUS_DIR, when set, builds against that consensus checkout instead of CMakeLists.txt's pin.
#   [CONSENSUS_DIR=DIR] test/group/suite.sh BUILD_DIR [CTEST_REGEX]
# A failed configure or build exits 127, so a `--fails-without` job passes only on a failed test.
root=$(cd "$(dirname "$0")/../.." && pwd)
build=$1
regex=${2:-^(group_|simulation_group_)}
override=
[ -z "$CONSENSUS_DIR" ] || override="-DFETCHCONTENT_SOURCE_DIR_CONSENSUS=$CONSENSUS_DIR"
# shellcheck disable=SC2086
cmake -G Ninja -S "$root" -B "$build/strata" -DCMAKE_BUILD_TYPE=Release $override >/dev/null || exit 127
cmake --build "$build/strata" -j4 --target group_tests || exit 127
cd "$build/strata" || exit 127
exec ctest -R "$regex" --no-tests=error --output-on-failure --stop-on-failure
