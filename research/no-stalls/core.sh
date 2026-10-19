#!/usr/bin/env bash
# The core matrix of docs/research/no-stalls.md: strata today, I1, and I2 against RocksDB
# (leveled, universal, and leveled with its stalls off), the same offered rate and merge
# budget, one round = every configuration once in a rotated order, under a light hold.
#   research/no-stalls/core.sh BUILD_DIR CORPUS OUT_DIR ROUND [SECONDS]
set -euo pipefail
build="$1" corpus="$2" out="$3" round="$4" seconds="${5:-180}"
mkdir -p "$out"
rate="${RATE:-5000}" limit="${LIMIT_MB:-3}" rlimit="${ROCKS_LIMIT_MB:-6}" q="${QUERIES:-100}"
common="rate=$rate batch=10 seconds=$seconds docs=100000 queries=$q window=5"
declare -a names=(strata-none strata-prop strata-adapt rocks-level rocks-universal rocks-nostall)
run_one() {
	local name="$1" dir="$out/$name-r$round"
	case "$name" in
	strata-none) "$build/research_ingest" "$dir" "$corpus" $common limit_mb="$limit" control=none ;;
	strata-prop) "$build/research_ingest" "$dir" "$corpus" $common limit_mb="$limit" control=proportional low=12 high=28 floor_rate=0.05 ;;
	strata-adapt) "$build/research_ingest" "$dir" "$corpus" $common limit_mb="$limit" control=adaptive low=16 high=64 ;;
	rocks-level) "$build/research_rocks" "$dir" "$corpus" $common limit_mb="$rlimit" compaction=level compression=lz4 ;;
	rocks-universal) "$build/research_rocks" "$dir" "$corpus" $common limit_mb="$rlimit" compaction=universal compression=lz4 ;;
	rocks-nostall) "$build/research_rocks" "$dir" "$corpus" $common limit_mb="$rlimit" compaction=level compression=lz4 stalls=0 ;;
	esac > "$out/$name-r$round.tsv" 2> "$out/$name-r$round.err"
	rm -rf "$dir"
}
n=${#names[@]}
for ((i = 0; i < n; i++)); do
	name="${names[$(((i + round) % n))]}"
	echo "$(date +%H:%M:%S) round $round: $name"
	run_one "$name"
done
echo "$(date +%H:%M:%S) round $round done"
