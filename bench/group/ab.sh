#!/bin/sh
# C5 (docs/design/journal-binding.md): group_checkpoint_latency at this checkout (fix) against
# the same bench built with the merge base's strata/group headers (base), ABAB rounds so base
# against base is the A/A. Run from the checkout under the quiet level the caller chose.
#   bench/group/ab.sh BUILD_DIR [rounds] [writes] [every]
set -e
build=$1
rounds=${2:-5}
writes=${3:-2000}
every=${4:-20}
src=$(pwd)
base=$(git -C "$src" merge-base HEAD origin/main 2>/dev/null || git -C "$src" merge-base HEAD main)
overlay=$build/base-overlay
mkdir -p "$overlay/strata/group"
for h in group.hh install.hh; do git -C "$src" show "$base:include/strata/group/$h" >"$overlay/strata/group/$h"; done
echo "base $base"
cmake -G Ninja -S "$src" -B "$build" -DCMAKE_BUILD_TYPE=Release -DSTRATA_GROUP_BENCH_BASE="$overlay" >/dev/null
cmake --build "$build" -j4 --target group_checkpoint_latency group_checkpoint_latency_base >/dev/null
r=1
while [ "$r" -le "$rounds" ]; do
	for side in base fix base fix; do
		bin=$build/group_checkpoint_latency
		[ "$side" = base ] && bin=$build/group_checkpoint_latency_base
		printf '%s %s ' "$r" "$side"
		"$bin" "$writes" "$every"
	done
	r=$((r + 1))
done
