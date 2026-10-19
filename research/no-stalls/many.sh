#!/usr/bin/env bash
# The many-index matrix of docs/research/no-stalls.md (I4): N indexes on one pool and one
# merge budget, queries Zipf-distributed over them; today's per-index policy on the pool's
# round robin against the host's scheduler by cheapest retirement (`global`) and by query
# heat (`hot`). One round = every configuration once in a rotated order.
#   research/no-stalls/many.sh BUILD_DIR CORPUS OUT_DIR ROUND [SECONDS]
set -euo pipefail
build="$1" corpus="$2" out="$3" round="$4" seconds="${5:-180}"
mkdir -p "$out"
rate="${RATE:-5000}" limit="${LIMIT_MB:-3}" q="${QUERIES:-100}" n="${INDEXES:-32}" refresh="${REFRESH_MS:-4000}"
common="rate=$rate batch=10 seconds=$seconds docs=100000 queries=$q window=5 indexes=$n refresh_ms=$refresh zipf=1.0 limit_mb=$limit"
declare -a names=(many-none many-global many-hot)
run_one() {
	local name="$1" dir="$out/$name-r$round"
	case "$name" in
	many-none) "$build/research_ingest" "$dir" "$corpus" $common control=none ;;
	many-global) "$build/research_ingest" "$dir" "$corpus" $common control=global ;;
	many-hot) "$build/research_ingest" "$dir" "$corpus" $common control=hot ;;
	esac > "$out/$name-r$round.tsv" 2> "$out/$name-r$round.err"
	rm -rf "$dir"
}
cnt=${#names[@]}
for ((i = 0; i < cnt; i++)); do
	name="${names[$(((i + round) % cnt))]}"
	echo "$(date +%H:%M:%S) round $round: $name"
	run_one "$name"
done
echo "$(date +%H:%M:%S) round $round done"
