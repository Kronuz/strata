#!/usr/bin/env bash
# Probe B (rocksdb-parity design): group commit and log count, fillrandom sync/1 100 B, K writers
# (one index each) on L loops, one turn log each (L=1: one log shared by every index), against RocksDB's K threads; F_FULLFSYNC per op in the run phase. The cohort
# build is cohort.py's probe patch, with STRATA_COHORT_US as its bound. Short runs.
set -u
P=$(cd "$(dirname "$0")" && pwd)
O=$HOME/.scratch/strata/rocksdb-parity
R=$HOME/.scratch/rocksdb-11.11-build/kv_rocksdb
OUT=$O/b-$(date +%H%M%S)
mkdir -p "$OUT"
cc -O2 -dynamiclib "$P/wbytes.c" -o "$OUT/wbytes.dylib" || exit 1
python3 "$P/cohort.py" include/strata/core/turn_seal.hh || exit 1
cmake -B build-cohort -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null && ninja -C build-cohort kv_strata >/dev/null || exit 1
git checkout -- include/strata/core/turn_seal.hh
echo "OUT=$OUT"

# name binary K L cohort_us
one() {
	local name=$1 bin=$2 k=$3 l=$4 us=$5
	shift 5
	local data=$OUT/data-$name
	rm -rf "$data"
	WBYTES_LOG=$OUT/$name.wbytes.tsv DYLD_INSERT_LIBRARIES=$OUT/wbytes.dylib KV_PHASE_MARKS=1 KV_LOOPS=$l STRATA_COHORT_US=$us \
		"$bin" --dir "$data" --workload fillrandom --records 100000 --ops 100000 --value-size 100 --threads "$k" \
		--durability sync --batch 1 --memory-mb 256 --max-seconds 8 "$@" >"$OUT/$name.json" 2>"$OUT/$name.err"
	echo "exit $?" >>"$OUT/$name.err"
	python3 -I - "$OUT/$name.json" "$OUT/$name.wbytes.tsv" "$name" <<'EOF'
import json, sys
r = json.loads(open(sys.argv[1]).read().strip().splitlines()[-1])
p = [x for x in r["phases"] if x["name"] == "run"][0]
full = sum(int(l.split("\t")[5]) for l in list(open(sys.argv[2]))[1:] if l.split("\t")[0] == "run")
print("%-22s ops/s %7.1f p50 %6.1f ms p99 %6.1f ms ops %6d F_FULLFSYNC %5d syncs/s %6.1f ops/barrier %.2f cpu/op %.0f us" % (
    sys.argv[3], p["ops_per_sec"], p["p50_us"] / 1000, p["p99_us"] / 1000, p["ops"], full, full / p["seconds"], p["ops"] / max(full, 1), p["cpu_us_per_op"]))
EOF
}

B=$PWD/build-rel/kv_strata
C=$PWD/build-cohort/kv_strata
for t in 1 2; do
	for kl in 1:1 2:1 2:2 4:1 4:2 4:4 8:1 8:2 8:4 8:8; do
		k=${kl%:*} l=${kl#*:}
		one "t$t-strata-K$k-L$l" "$B" "$k" "$l" 0
		one "t$t-cohort-K$k-L$l" "$C" "$k" "$l" 1000
	done
	for k in 1 2 4 8; do one "t$t-rocksdb-K$k" "$R" "$k" 1 0 --variant lvl-16k-lz4; done
done
rm -rf "$OUT"/data-*
echo done
