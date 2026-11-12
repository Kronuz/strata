#!/bin/sh
# Builds index_test with one half of the open's sync dropped and exits 0 only when the T4 case
# that guards that half fails: M1 (log only) at index_test.cc:182 (case 10), M2 (directory only)
# at index_test.cc:137 (case 9). Exits 2 on a build failure, 1 when the case passes.
# Usage: mutant.sh M1|M2, from the checkout's root; restores index.hh on exit.
set -u
f=include/strata/core/index.hh
sync='io_->sync_files_and_directory({\&log_file, 1});'
case ${1:-} in
M1) repl='log_->sync();  \/\/ MUTANT M1: log only'; want='index_test.cc:182:' ;;
M2) repl='io_->sync_directory();  \/\/ MUTANT M2: directory only'; want='index_test.cc:137:' ;;
*) echo "usage: mutant.sh M1|M2" >&2; exit 2 ;;
esac
cp "$f" "$f.orig" || exit 2
trap 'mv "$f.orig" "$f"' EXIT
sed "s/$sync/$repl/" "$f.orig" > "$f"
grep -n "MUTANT" "$f" || { echo "the open's sync line was not found" >&2; exit 2; }
cmake -B build -DCMAKE_BUILD_TYPE=Release >/dev/null && cmake --build build --target index_test -j4 >/dev/null || exit 2
./build/index_test > index_test.out 2>&1
rc=$?
cat index_test.out
[ "$rc" -ne 0 ] && grep -q "$want" index_test.out
