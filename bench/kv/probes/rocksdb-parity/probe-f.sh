#!/usr/bin/env bash
# Probe F (rocksdb-parity D6, review F1): handing records from L loops to one log's owner
# (handoff.cc), 4 groups per loop with one ack=sync record each, L = 2, 4, 8 against one loop
# holding them all; the owner takes them under each group's mutex (pull), drains per-loop rings
# (push), or asks each loop (ask); `local` puts all 4L groups on the owner's loop. A barrier of
# 20 or 200 µs; waits that sleep at once or spin 50 µs first; a cohort bound of 1/16 of the
# barrier or 40 µs. 1 s per cell.
set -u
P=$(cd "$(dirname "$0")" && pwd)
O=$HOME/.scratch/strata/rocksdb-parity
OUT=$O/f-$(date +%H%M%S)
mkdir -p "$OUT"
c++ -std=c++20 -O2 "$P/handoff.cc" -o "$OUT/handoff" || exit 1
for d in 20 200; do
	q=$(awk "BEGIN { print $d / 16 }")
	for sc in "0 $q" "0 40" "50 $q" "50 40"; do
		set -- $sc
		for l in 2 4 8; do
			for m in local pull push ask; do "$OUT/handoff" "$m" "$l" 4 "$d" "$2" "$1" 1; done
		done
	done
done
echo done
