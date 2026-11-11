#!/usr/bin/env bash
# Cold-tier K6, T1's step-1 rows (docs/design/cold-tier.md T1, N1): per-index memory of an idle
# index, hot (every segment mapped) against main's open, warm (demoted) and lazy (the default),
# on 1- and 100-document fixtures with 1 and 10 segments.
#   k6.sh build OUT MAIN_SRC [COUNT]   builds this tree's and MAIN_SRC's idle_index into OUT and the
#                                      fixtures (a light job)
#   k6.sh measure OUT [COUNT] [REPS]   runs them (a strict job); one line per run, then the heap
set -euo pipefail
cmd=$1 out=$2
here="$(cd "$(dirname "$0")/../.." && pwd)"
case "$cmd" in
build)
	main_src=$3 count=${4:-10000}
	mkdir -p "$out"
	for side in branch main; do
		src=$here
		[ "$side" = main ] && src=$main_src
		cmake -S "$src" -B "$out/$side-build" -DCMAKE_BUILD_TYPE=Release >/dev/null
		cmake --build "$out/$side-build" -j4 --target idle_index idle_index_count >/dev/null
		mkdir -p "$out/$side"
		cp "$out/$side-build/idle_index" "$out/$side-build/idle_index_count" "$out/$side/"
		(cd "$src" && git rev-parse HEAD 2>/dev/null || echo unknown) >"$out/$side/commit"
	done
	rm -rf "$out/d1s1" "$out/d100s1" "$out/d100s10"
	"$out/branch/idle_index" build "$out/d1s1" "$count" 1 1
	"$out/branch/idle_index" build "$out/d100s1" "$count" 100 1
	"$out/branch/idle_index" build "$out/d100s10" "$count" 100 10
	;;
measure)
	count=${3:-10000} reps=${4:-3}
	echo "branch $(cat "$out/branch/commit"), main $(cat "$out/main/commit"), $count indexes, $(uname -sm)"
	# Main's harness is the baseline (A3): its runs interleave with this tree's, rep by rep.
	for f in d1s1 d100s1 d100s10; do
		for r in $(seq "$reps"); do
			for run in "main -" "branch hot" "branch warm" "branch lazy"; do
				set -- $run
				args=(open "$out/$f" "$count" shared idle 5)
				[ "$2" != - ] && args+=(tier "$2")
				"$out/$1/idle_index" "${args[@]}" | grep -E "^after open|^demote|^LIMIT" | sed "s|^|$f $1 $2 rep$r: |"
			done
		done
		for run in "main -" "branch hot" "branch warm" "branch lazy"; do
			set -- $run
			args=(open "$out/$f" "$count" shared idle 5)
			[ "$2" != - ] && args+=(tier "$2")
			"$out/$1/idle_index_count" "${args[@]}" | grep -E "^heap live|^LIMIT" | sed "s|^|$f $1 $2: |"
		done
	done
	;;
*)
	echo "usage: k6.sh build OUT MAIN_SRC [COUNT] | measure OUT [COUNT] [REPS]" >&2
	exit 2
	;;
esac
