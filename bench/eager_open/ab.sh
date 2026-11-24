#!/usr/bin/env bash
# Eager against lazy opens on Xapiand's path (docs/perf/eager-open.md): one harness built twice
# over one prospect, PROSPECT_SRC: `base` over BASE_SRC's strata, `proto` over this tree; every
# row interleaves the sides and arms run by run in one job.
#   ab.sh build OUT BASE_SRC PROSPECT_SRC     both binaries, Release, Ninja (a light job)
#   ab.sh buildside OUT SIDE PROSPECT_SRC     this tree over PROSPECT_SRC as SIDE (a light job)
#   ab.sh fixtures OUT COUNT                  d1000s1, d1000s4, d1000s10 of COUNT clones, and
#                                             d1000s4c (100 clones, one field dropped from the table)
#   ab.sh row OUT FIXTURE COUNT ROUNDS ARMS...  per round, each side of SIDES ("base proto"; with aa,
#                                             a copy of base, the A/A), the order rotated each round,
#                                             each ARM of `eager_open open ... query`
#   ab.sh heap OUT FIXTURE COUNT ARMS...      the counting build's live heap per index, each side
#   ab.sh identity OUT COUNT                  identity hashes and refusals, every arm and side of
#                                             SIDES ("base proto")
set -euo pipefail
cmd=$1 out=$2
here="$(cd "$(dirname "$0")/../.." && pwd)"
run() {  # SIDE BIN ARGS...
	local side=$1 bin=$2
	shift 2
	echo "== $side $bin $*"
	"$out/$side/$bin" "$@"
}
case "$cmd" in
build)
	base_src=$3 prospect_src=$4
	mkdir -p "$out/base" "$out/proto" "$out/aa"
	cmake -G Ninja -S "$here/bench/eager_open" -B "$out/base-build" -DCMAKE_BUILD_TYPE=Release -DSTRATA_SOURCE="$base_src" -DFETCHCONTENT_SOURCE_DIR_PROSPECT="$prospect_src" >/dev/null
	cmake --build "$out/base-build" -j4 --target eager_open eager_open_count >/dev/null
	cmake -G Ninja -S "$here/bench/eager_open" -B "$out/proto-build" -DCMAKE_BUILD_TYPE=Release -DSTRATA_SOURCE="$here" -DFETCHCONTENT_SOURCE_DIR_PROSPECT="$prospect_src" >/dev/null
	cmake --build "$out/proto-build" -j4 --target eager_open eager_open_count >/dev/null
	for s in base proto; do cp "$out/$s-build/eager_open" "$out/$s-build/eager_open_count" "$out/$s/"; done
	cp "$out/base/eager_open" "$out/base/eager_open_count" "$out/aa/"
	{ git -C "$base_src" rev-parse HEAD; git -C "$prospect_src" rev-parse HEAD; } >"$out/base/commit"
	cp "$out/base/commit" "$out/aa/commit"
	{ git -C "$here" rev-parse HEAD; git -C "$prospect_src" rev-parse HEAD; } >"$out/proto/commit"
	file "$out/base/eager_open" "$out/proto/eager_open"
	cat "$out/base/commit" "$out/proto/commit"
	;;
buildside)
	side=$3 prospect_src=$4
	mkdir -p "$out/$side"
	cmake -G Ninja -S "$here/bench/eager_open" -B "$out/$side-build" -DCMAKE_BUILD_TYPE=Release -DSTRATA_SOURCE="$here" -DFETCHCONTENT_SOURCE_DIR_PROSPECT="$prospect_src" >/dev/null
	cmake --build "$out/$side-build" -j4 --target eager_open eager_open_count >/dev/null
	cp "$out/$side-build/eager_open" "$out/$side-build/eager_open_count" "$out/$side/"
	{ git -C "$here" rev-parse HEAD; git -C "$prospect_src" rev-parse HEAD; } >"$out/$side/commit"
	file "$out/$side/eager_open"
	cat "$out/$side/commit"
	;;
fixtures)
	count=$3
	for f in d1000s1:1000:1 d1000s4:1000:4 d1000s10:1000:10; do
		IFS=: read -r name docs segs <<<"$f"
		rm -rf "${out:?}/$name"
		"$out/base/eager_open" build "$out/$name" "$count" "$docs" "$segs"
	done
	rm -rf "${out:?}/d1000s4c"
	"$out/base/eager_open" build "$out/d1000s4c" 100 1000 4 corrupt
	;;
row)
	fixture=$3 count=$4 rounds=$5
	shift 5
	read -r -a sides <<<"${SIDES:-base proto}"
	echo "sides: $(for s in "${sides[@]}"; do printf '%s %s; ' "$s" "$(tr '\n' ' ' <"$out/$s/commit")"; done)$(uname -sm), $(date -u +%FT%TZ), $fixture x $count"
	n=${#sides[@]}
	for p in $(seq 0 $((rounds - 1))); do
		for i in $(seq 0 $((n - 1))); do
			side=${sides[$(((i + p) % n))]}
			for a in "$@"; do run "$side" eager_open open "$out/$fixture" "$count" "$a" query; done
		done
	done
	;;
heap)
	fixture=$3 count=$4
	shift 4
	for side in base proto; do
		for a in "$@"; do run "$side" eager_open_count open "$out/$fixture" "$count" "$a" query; done
	done
	;;
identity)
	count=$3
	for fixture in d1000s1 d1000s4 d1000s10 d1000s4c; do
		n=$count
		[ "$fixture" = d1000s4c ] && n=100
		for side in ${SIDES:-base proto}; do
			for a in today eager lazy warm; do run "$side" eager_open identity "$out/$fixture" "$n" "$a"; done
		done
	done
	;;
*)
	echo "usage: ab.sh build|fixtures|row|heap|identity OUT ..." >&2
	exit 2
	;;
esac
