#!/usr/bin/env bash
# Probe D (rocksdb-parity design): the cost of re-homing groups onto another loop count (journal
# Host's re-partition at open), on a real directory with syncs made counted no-ops (nosync.c):
# CPU, bytes, syncs, peak RSS, for N idle groups.
set -u
P=$(cd "$(dirname "$0")" && pwd)
O=$HOME/.scratch/strata/rocksdb-parity
OUT=$O/d-$(date +%H%M%S)
mkdir -p "$OUT"
echo "OUT=$OUT"
cc -O2 -dynamiclib "$P/nosync.c" -o "$OUT/nosync.dylib" || exit 1
# The test's compile and link lines, with rehome.cc in its place.
cc_line=$(ninja -C build-rel -t commands standalone_turn_log_test | grep -- '-c .*test/standalone_turn_log_test.cc' | tail -1)
ld_line=$(ninja -C build-rel -t commands standalone_turn_log_test | grep -- '-o standalone_turn_log_test ' | tail -1)
[ -n "$cc_line" ] && [ -n "$ld_line" ] || { echo "no build lines"; exit 1; }
src=$(echo "$cc_line" | grep -o '[^ ]*test/standalone_turn_log_test.cc' | tail -1)
obj=$(echo "$cc_line" | grep -o -- '-o [^ ]*' | tail -1 | cut -c4-)
cc_line=${cc_line//$src/$P/rehome.cc}
cc_line=${cc_line//$obj/$OUT/rehome.o}
ld_line=${ld_line//$obj/$OUT/rehome.o}
ld_line=${ld_line//-o standalone_turn_log_test /-o $OUT/rehome }
(cd build-rel && eval "$cc_line") || exit 1
(cd build-rel && eval "$ld_line") || exit 1
i=0
for args in "10000 4 8" "10000 8 2" "100000 4 8" "100000 8 2" "100000 4 8 48 1024 10" "300000 4 8"; do
	i=$((i + 1))
	echo "== rehome $args"
	mkdir -m 700 "$OUT/data-$i"
	DYLD_INSERT_LIBRARIES=$OUT/nosync.dylib "$OUT/rehome" "$OUT/data-$i" $args
	rm -rf "$OUT/data-$i"
done
echo done
