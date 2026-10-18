#!/bin/bash
# One-thread, keyed indexing of the corpus sample by today's strata_engine and by the
# Tantivy harness builder, interleaved, TRIALS times each (docs/research/indexing.md,
# "Where today's pipeline spends its time"). Wall time and peak RSS from /usr/bin/time.
#   research/indexing/baseline.sh CORPUS [TRIALS=3]
set -u
root="$(cd "$(dirname "$0")/../.." && pwd)"
corpus=${1:?corpus}
trials=${2:-3}
out="$root/.scratch/baseline"
mkdir -p "$out"
strata="$root/build-research/strata_engine"
tantivy="$root/bench/engines/tantivy-0.26-st/target/release/build_index"
if [ "$(uname -s)" = Linux ]; then tflag=-v; else tflag=-l; fi
for t in $(seq 1 "$trials"); do
	for e in strata tantivy; do
		dir="$out/idx-$e"
		rm -rf "$dir"; mkdir -p "$dir"
		cat "$corpus" > /dev/null  # warm the page cache alike for both
		case $e in
		strata) cmd=(env STRATA_BUDGET_MB=2056 STRATA_THREADS=1 "$strata" index "$dir") ;;
		tantivy) cmd=(env KEYED=1 TANTIVY_HEAP_MB=2000 "$tantivy" "$dir") ;;
		esac
		/usr/bin/time $tflag -o "$out/t$t-$e.time" "${cmd[@]}" < "$corpus" > "$out/t$t-$e.log" 2>&1 || echo "FAILED $e trial $t"
		find "$dir" -type f -exec stat -f%z {} + 2>/dev/null | awk '{n += $1} END {print n}' > "$out/t$t-$e.bytes"
		printf '%s trial %d: %s s, rss %s bytes, index %s bytes\n' "$e" "$t" "$(awk '/real/ {print $1; exit}' "$out/t$t-$e.time")" \
			"$(awk '/maximum resident/ {print $1; exit}' "$out/t$t-$e.time")" "$(cat "$out/t$t-$e.bytes")"
	done
done
echo BASELINE_DONE
