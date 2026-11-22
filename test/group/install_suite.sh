#!/bin/sh
# Builds strata::group's tests and runs ctest's group_ tests, group_install among them.
# CONSENSUS_DIR, when set, builds against that consensus checkout instead of CMakeLists.txt's pin.
#   [CONSENSUS_DIR=DIR] test/group/install_suite.sh BUILD_DIR [CTEST_REGEX]
# A failed configure or build exits 127, so a `--fails-without` job passes only on a failed test.
root=$(cd "$(dirname "$0")/../.." && pwd)
build=$1
regex=${2:-^group_}
override=
[ -z "$CONSENSUS_DIR" ] || override="-DFETCHCONTENT_SOURCE_DIR_CONSENSUS=$CONSENSUS_DIR"
# shellcheck disable=SC2086
cmake -G Ninja -S "$root" -B "$build/strata" -DCMAKE_BUILD_TYPE=Release $override >/dev/null || exit 127
cmake --build "$build/strata" -j4 --target group_voters_test group_lone_voter_test group_lifetime_test group_footprint_test group_file_set_test group_install_test || exit 127
cd "$build/strata" || exit 127
exec ctest -R "$regex" --no-tests=error --output-on-failure --stop-on-failure
