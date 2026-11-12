#!/bin/sh
# Opens COUNT indexes with each binary build.sh made, in interleaved pairs (AB, then BA), one
# warm-up run each first; stops starting pairs after 8 minutes (D59's 10-minute bound).
# Usage: run.sh OUT [COUNT [PAIRS [ARGS]]], as a strict job; ARGS go to both sides' open
# (`commit` also times each index's first commit after the opens).
set -eu
out=$1; count=${2:-1000}; pairs=${3:-6}; args=${4:-}
start=$(date +%s)
one() { printf '%s ' "$1"; "$out/idle_index.$1" open "$out/root" "$count" $args | grep -E '^(opened|open |first commit|garbage)' | tr '\n' ' '; echo; }
one before >/dev/null; one after >/dev/null
i=0
while [ "$i" -lt "$pairs" ] && [ $(( $(date +%s) - start )) -lt 480 ]; do
	if [ $((i % 2)) -eq 0 ]; then one before; one after; else one after; one before; fi
	i=$((i + 1))
done
