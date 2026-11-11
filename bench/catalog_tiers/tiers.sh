#!/usr/bin/env bash
# Cold-tier T1 to T4 and T2d (docs/design/cold-tier.md gate rows) with catalog_tiers, against main's
# idle_index as the re-measured baseline (A3), interleaved with it run by run in one job.
#   tiers.sh build OUT MAIN_SRC [COUNT] [BIG] [NAMES]  this tree's catalog_tiers and idle_index, main's
#                                                idle_index, and the fixtures: COUNT (100,000) indexes each
#                                                of NAMES (d1s1,d100s1,d1000s1,d100s10,d1000s10), and BIG
#                                                (1,000,000; 0 none) of d1s1 as `big` (a light job); COUNT 0
#                                                keeps the fixtures
#   tiers.sh smoke OUT                           every mode end to end on 2,000 indexes (under 3 minutes)
#   tiers.sh t1 OUT FIXTURE [COUNT] [REPS]       T1: memory per tier, plain and defer_io, heap per tier
#   tiers.sh t2 OUT FIXTURE [COUNT] [REPS] [drop]  T2 and T2d: first query per tier, plain and defer_io;
#                                                `drop` drops the page cache before each timed loop (root)
#   tiers.sh t3 OUT [COUNT] [REQUESTS] [OPEN HOT]   T3 on `big`: every index once, then Zipf at 0.99
#   tiers.sh t4 OUT THETA [scan] [WARM] [REQUESTS] [COUNT OPEN HOT]  T4 on `big`: hit ratio against
#                                                the S3-FIFO models
# OPEN and HOT scale the caps down with COUNT for a run that fits one 10-minute job (D59).
# The caps are the row's per platform: Linux soft_hot 10,000, soft_open 50,000 (T3) and 110,000 (T4);
# macOS soft_hot 5,000 and soft_open 50,000.
set -euo pipefail
cmd=$1 out=$2
here="$(cd "$(dirname "$0")/../.." && pwd)"
if [ "$(uname -s)" = Linux ]; then
	hot=10000 open3=50000 open4=110000
else
	hot=5000 open3=50000 open4=50000
fi
drop_now() {  # the page cache, for main's runs (root only)
	sync
	if [ "$(uname -s)" = Linux ]; then echo 3 >/proc/sys/vm/drop_caches; else purge; fi
}
header() { echo "branch $(cat "$out/branch/commit"), main $(cat "$out/main/commit"), $(uname -sm), $(date -u +%FT%TZ)"; }
case "$cmd" in
build)
	main_src=$3 count=${4:-100000} big=${5:-1000000} names=${6:-d1s1,d100s1,d1000s1,d100s10,d1000s10}
	rm -rf "${out:?}/branch-build" "${out:?}/main-build"  # a job's checkout differs from the last one's
	mkdir -p "$out/branch" "$out/main"
	cmake -S "$here/bench/catalog_tiers" -B "$out/branch-build" -DCMAKE_BUILD_TYPE=Release >/dev/null
	cmake --build "$out/branch-build" -j4 --target catalog_tiers catalog_tiers_count idle_index >/dev/null
	cp "$out/branch-build/catalog_tiers" "$out/branch-build/catalog_tiers_count" "$out/branch/"
	cp "$out/branch-build/strata/idle_index" "$out/branch/"
	git -C "$here" rev-parse HEAD >"$out/branch/commit"
	cmake -S "$main_src" -B "$out/main-build" -DCMAKE_BUILD_TYPE=Release >/dev/null
	cmake --build "$out/main-build" -j4 --target idle_index idle_index_count >/dev/null
	cp "$out/main-build/idle_index" "$out/main-build/idle_index_count" "$out/main/"
	git -C "$main_src" rev-parse HEAD >"$out/main/commit"
	file "$out/branch/catalog_tiers" "$out/main/idle_index"
	[ "$count" = 0 ] && exit 0
	# this tree's idle_index builds the fixtures (main's ignores SEGMENTS and merges); both sides open them
	for f in d1s1:1:1 d100s1:100:1 d1000s1:1000:1 d100s10:100:10 d1000s10:1000:10; do
		IFS=: read -r name docs segs <<<"$f"
		case ",$names," in *",$name,"*) ;; *) continue ;; esac
		rm -rf "${out:?}/$name"
		"$out/branch/idle_index" build "$out/$name" "$count" "$docs" "$segs" | grep -E "^template"
	done
	[ "$big" = 0 ] && exit 0
	rm -rf "${out:?}/big"
	"$out/branch/idle_index" build "$out/big" "$big" 1 1
	;;
smoke)
	header
	t="$out/branch/catalog_tiers"
	for tier in hot warm cold; do
		for d in "" defer; do
			"$t" tier "$out/d100s10" 2000 "$tier" $d | grep -E "^tier|^forced"
			"$t" first "$out/d100s10" 2000 "$tier" $d | grep -E "^first|^found|^errors|^promote"
		done
	done
	"$out/branch/catalog_tiers_count" tier "$out/d1s1" 2000 warm defer | grep -E "^heap"
	"$out/main/idle_index" open "$out/d1s1" 2000 shared query | grep -E "^open|^first query"
	"$t" count "$out/big" 20000 requests 20000 soft_open 2000 soft_hot 500 defer | grep -E "^T3|^errors|^zipf|^touch|^peak"
	"$t" zipf "$out/big" 20000 warm 20000 requests 40000 soft_open 2000 soft_hot 500 defer | grep -E "^T4|^models|^measured|^hottest"
	;;
t1)
	fx=$3 count=${4:-100000} reps=${5:-3}
	header
	echo "T1 on $fx, $count indexes, $reps reps"
	for r in $(seq "$reps"); do
		"$out/main/idle_index" open "$out/$fx" "$count" shared idle 5 | grep -E "^after open|^LIMIT" | sed "s|^|$fx main rep$r: |"
		for d in "" defer; do
			for tier in hot warm cold; do
				"$out/branch/catalog_tiers" tier "$out/$fx" "$count" "$tier" $d idle 5 | grep -E "^tier|^forced|^error" | sed "s|^|$fx $tier${d:+ $d} rep$r: |"
			done
		done
	done
	"$out/main/idle_index_count" open "$out/$fx" "$count" shared idle 5 | grep -E "^heap live|^LIMIT" | sed "s|^|$fx main: |"
	for d in "" defer; do
		for tier in hot warm cold; do
			"$out/branch/catalog_tiers_count" tier "$out/$fx" "$count" "$tier" $d | grep -E "^heap live" | sed "s|^|$fx $tier${d:+ $d}: |"
		done
	done
	;;
t2)
	fx=$3 count=${4:-100000} reps=${5:-1} drop=${6:-}
	header
	echo "T2/T2d on $fx, $count indexes, $reps reps, cache ${drop:-warm}"
	for r in $(seq "$reps"); do
		if [ -n "$drop" ]; then drop_now; fi
		"$out/main/idle_index" open "$out/$fx" "$count" shared query | grep -E "^open |^first query|^LIMIT" | sed "s|^|$fx main rep$r: |"
		for d in "" defer; do
			for tier in hot warm cold; do
				"$out/branch/catalog_tiers" first "$out/$fx" "$count" "$tier" $d seed "$r" $drop | grep -E "^first|^found|^errors|^promote|^page" | sed "s|^|$fx $tier${d:+ $d} rep$r: |"
			done
		done
	done
	;;
t3)
	count=${3:-1000000} requests=${4:-1000000} open3=${5:-$open3} hot=${6:-$hot}
	header
	echo "T3 on big, $count indexes, $requests requests, soft_open $open3 soft_hot $hot"
	if [ "$(uname -s)" = Linux ]; then sysctl vm.max_map_count fs.nr_open fs.file-max; else sysctl kern.maxvnodes kern.maxfiles kern.maxfilesperproc kern.num_vnodes; fi
	ulimit -n
	"$out/branch/catalog_tiers" count "$out/big" "$count" threads 8 requests "$requests" theta 0.99 soft_open "$open3" soft_hot "$hot" defer
	if [ "$(uname -s)" = Linux ]; then grep -E "^(dentry|inode_cache|ext4_inode_cache|xfs_inode) " /proc/slabinfo 2>/dev/null || echo "slabinfo: not readable"; fi
	;;
t4)
	theta=$3 scan=${4:-} warm=${5:-5000000} requests=${6:-20000000} count=${7:-1000000} open4=${8:-$open4} hot=${9:-$hot}
	[ "$scan" = noscan ] && scan=
	header
	echo "T4 on big, $count indexes, theta $theta${scan:+ with a scan}, warm-up $warm, $requests measured, soft_open $open4 soft_hot $hot"
	ulimit -n
	"$out/branch/catalog_tiers" zipf "$out/big" "$count" theta "$theta" $scan warm "$warm" requests "$requests" threads 8 soft_open "$open4" soft_hot "$hot" defer
	;;
*)
	echo "usage: tiers.sh build OUT MAIN_SRC [COUNT] [BIG] | smoke OUT | t1 OUT FIXTURE [COUNT] [REPS] | t2 OUT FIXTURE [COUNT] [REPS] [drop] | t3 OUT [COUNT] [REQUESTS] [OPEN HOT] | t4 OUT THETA [scan] [WARM] [REQUESTS] [COUNT OPEN HOT]" >&2
	exit 2
	;;
esac
