#!/bin/sh
# Builds and runs group_install_test before CMakeLists.txt registers it: a wrapper project adds
# this checkout as a subdirectory (its own tests stay off) and the test beside it.
#   test/group/install_build.sh BUILD_DIR [--build-only | TEST ARGS...]
# STRATA_CMAKE_ARGS adds configure arguments (a build type, sanitizer flags).
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
build=$1
shift
mkdir -p "$build/wrap"
cat > "$build/wrap/CMakeLists.txt" <<WRAP
cmake_minimum_required(VERSION 3.18)
project(install_wrap LANGUAGES C CXX)
add_subdirectory("$root" strata)
add_executable(group_install_test "$root/test/group/install_test.cc")
target_link_libraries(group_install_test PRIVATE strata::group)
WRAP
# shellcheck disable=SC2086
cmake -S "$build/wrap" -B "$build/out" -DCMAKE_BUILD_TYPE=Release $STRATA_CMAKE_ARGS >/dev/null
cmake --build "$build/out" --target group_install_test -j4
[ "$1" = --build-only ] && exit 0
exec "$build/out/group_install_test" "$@"
