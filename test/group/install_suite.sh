#!/bin/sh
# Builds strata::group's tests against the consensus checkout at CONSENSUS_DIR (its install
# surface, until CMakeLists.txt pins it) and runs them: ctest's group_ tests, then
# group_install_test through install_build.sh.
#   CONSENSUS_DIR=DIR test/group/install_suite.sh BUILD_DIR [CTEST_REGEX]
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
build=$1
regex=${2:-^group_}
[ -n "$CONSENSUS_DIR" ] || { echo "CONSENSUS_DIR is unset" >&2; exit 127; }
cmake -S "$root" -B "$build/strata" -DCMAKE_BUILD_TYPE=Release -DFETCHCONTENT_SOURCE_DIR_CONSENSUS="$CONSENSUS_DIR" >/dev/null
cmake --build "$build/strata" -j4 --target group_voters_test group_lone_voter_test group_lifetime_test group_footprint_test group_file_set_test
(cd "$build/strata" && ctest -R "$regex" --output-on-failure --stop-on-failure)
STRATA_CMAKE_ARGS="-DFETCHCONTENT_SOURCE_DIR_CONSENSUS=$CONSENSUS_DIR" sh "$root/test/group/install_build.sh" "$build/install"
