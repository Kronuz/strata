#!/bin/bash
# N indexes writing at once on the gate VM, one thread each (docs/perf/small-indexes.md, M2),
# every engine, TRIALS rounds (5) with the order rotated, the page cache dropped before each.
#   [N=8] [DOCS=100000] [TRIALS=5] [BUDGET_MB=2056] [CORPUS=corpus-1000000.json] [ENGINES="strata tantivy lucene"] many_write.sh
# The indexes share BUDGET_MB (T4's calibrated node budget), each an N-th of it.
# Results in ~/bench/results/si-m2-N-DOCS, scored by many_score.py DIR N*DOCS.
set -u
cd ~/bench/search-benchmark-game || exit 1
n=${N:-8} docs=${DOCS:-100000} trials=${TRIALS:-5} budget=${BUDGET_MB:-2056} corpus=${CORPUS:-corpus-1000000.json}
STRATA_BUDGET_MB=$budget TANTIVY_HEAP_MB=$((budget / n)) LUCENE_RAM_MB=$(awk "BEGIN {print $budget / $n}")
export STRATA_BUDGET_MB TANTIVY_HEAP_MB LUCENE_RAM_MB
out=~/bench/results/si-m2-$n-$docs; mkdir -p "$out"
ulimit -n 1048576
T=~/si/engines/tantivy/target/release/many
L=(java --add-modules jdk.incubator.vector --enable-native-access=ALL-UNNAMED -cp "$HOME/si/engines/lucene/build/libs/search-index-benchmark-game-lucene-1.0-SNAPSHOT.jar:$HOME/si/engines/lucene/build/dependencies/*" Many)
S=${S:-$HOME/si/build-f5/strata_engine}
read -r -a engines <<< "${ENGINES:-strata tantivy lucene}"
count=${#engines[@]}
for t in $(seq 1 "$trials"); do
	for i in $(seq 0 $((count - 1))); do
		e=${engines[$(((i + t - 1) % count))]}
		unit=t$t-$e
		[ -f "$out/$unit.time" ] && continue
		root=~/si/m2-idx; rm -rf "$root"
		case $e in
		strata) cmd=("$S" build-small "$root" "$n" "$docs" "$n") ;;
		tantivy) cmd=("$T" build "$root" "$n" "$docs" "$n") ;;
		lucene) cmd=("${L[@]}" build "$root" "$n" "$docs" "$n") ;;
		esac
		sync; echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null
		/usr/bin/time -v -o "$out/$unit.time" "${cmd[@]}" < "$corpus" > "$out/$unit.log" 2>&1 || echo "UNIT_FAILED $unit"
		du -sb "$root" | cut -f1 > "$out/$unit.bytes"
		echo "$unit $(grep Elapsed "$out/$unit.time" | awk '{print $NF}') cpu $(grep 'Percent of CPU' "$out/$unit.time" | awk '{print $NF}') user $(grep 'User time' "$out/$unit.time" | awk '{print $NF}') sys $(grep 'System time' "$out/$unit.time" | awk '{print $NF}') rss $(grep Maximum "$out/$unit.time" | awk '{print $NF}') bytes $(cat "$out/$unit.bytes")"
	done
done
rm -rf ~/si/m2-idx
echo M2_DONE
