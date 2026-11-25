#!/bin/sh
# Probe (rocksdb-parity msg-1): search latency over sorted runs. Builds prospect's prospect_engine
# against this strata checkout (task at strata's pin; STRATA_MAX_RUNS sets merge_policy::max_runs
# for its index command), indexes a corpus keyed in order at max_runs 0 (the tiers alone: A, and A2
# built apart for the A/A), 64 (B, the default) and 16 (C, a lower cap), then times term, phrase
# and AND queries (TOP_10, COUNT) on each, the side order rotating each round. Run from the strata
# checkout's root.
#   search-runs.sh build PROSPECT_SRC OUT [DOCS] [BUFFER_MB]
#   search-runs.sh time OUT [ROUNDS] [RUNS]
set -eu
here=$(cd "$(dirname "$0")" && pwd)
mode=$1
if [ "$mode" = build ]; then
	src=$2 out=$3 docs=${4:-550000} mb=${5:-4}
	mkdir -p "$out"
	rm -rf "$out/prospect" && mkdir "$out/prospect"
	if git -C "$src" rev-parse --git-dir >/dev/null 2>&1; then
		git -C "$src" archive HEAD | tar -x -C "$out/prospect"
	else
		cp -R "$src/." "$out/prospect"
	fi
	pin=$(grep -o 'task.git GIT_TAG [0-9a-f]*' CMakeLists.txt)
	sed -i.orig "s/task.git GIT_TAG [0-9a-f]*/$pin/" "$out/prospect/CMakeLists.txt"
	sed -i.orig '/o.auto_merge = !merge/a\
	if (const char* runs = std::getenv("STRATA_MAX_RUNS")) { o.policy.max_runs = std::strtoull(runs, nullptr, 10); }
' "$out/prospect/tools/prospect_engine.cc"
	grep -q STRATA_MAX_RUNS "$out/prospect/tools/prospect_engine.cc"
	cmake -G Ninja -S "$out/prospect" -B "$out/build" -DCMAKE_BUILD_TYPE=Release -DFETCHCONTENT_SOURCE_DIR_STRATA="$PWD" >"$out/configure.log"
	ninja -C "$out/build" -j4 prospect_engine >"$out/build.log"
	python3 "$here/search_runs.py" corpus "$out" "$docs"
	for side in a a2 b c; do
		case $side in b) runs=64 ;; c) runs=16 ;; *) runs=0 ;; esac
		rm -rf "$out/idx-$side"
		STRATA_MAX_RUNS=$runs PROSPECT_BUFFER_MB=$mb PROSPECT_IDENTITY_SEED=1 "$out/build/prospect_engine" index "$out/idx-$side" <"$out/corpus.json" 2>"$out/index-$side.log"
		echo "$side (max_runs $runs): $(tail -1 "$out/index-$side.log"), $(du -sk "$out/idx-$side" | cut -f1) KiB"
	done
	exit 0
fi
out=$2 rounds=${3:-8} runs=${4:-3}
bin=$out/build/prospect_engine
rm -rf "$out/t" && mkdir -p "$out/t"
r=1
while [ "$r" -le "$rounds" ]; do
	case $((r % 4)) in 1) order="a a2 b c" ;; 2) order="c a a2 b" ;; 3) order="b c a a2" ;; *) order="a2 b c a" ;; esac
	for c in TOP_10 COUNT; do
		for s in $order; do
			for q in term phrase and; do
				"$bin" bench "$out/idx-$s" "$c" "$runs" <"$out/$q.txt" >"$out/t/$r-$s-$c-$q.tsv" 2>>"$out/t/bench.err"
			done
		done
	done
	echo "$(date +%T) round $r"
	r=$((r + 1))
done
python3 "$here/search_runs.py" report "$out"
for s in a a2 b c; do echo "$s: $(tail -1 "$out/index-$s.log")"; done
