#!/usr/bin/env bash
# Probe C (rocksdb-parity design): where the CPU goes on the rows that spend more of it, by
# `sample` over the run phase: tenx1 fillseq and overwrite (tier none, 12 MiB), 1 KiB fillrandom
# (tier none, 256 MiB), and fillrandom under wal (the VM rows' CPU loss, measured on the host).
set -u
P=$(cd "$(dirname "$0")" && pwd)
O=$HOME/.scratch/strata/rocksdb-parity
S=$PWD/build-rel/kv_strata
R=$HOME/.scratch/rocksdb-11.11-build/kv_rocksdb
OUT=$O/c-$(date +%H%M%S)
mkdir -p "$OUT"
echo "OUT=$OUT"

# name engine sample_seconds args...
one() {
	local name=$1 eng=$2 secs=$3
	shift 3
	local bin=$S
	if [ "$eng" = rocksdb ]; then bin=$R; fi
	local data=$OUT/data-$name
	rm -rf "$data"
	KV_PHASE_MARKS=1 "$bin" --dir "$data" "$@" >"$OUT/$name.json" 2>"$OUT/$name.err" &
	local pid=$!
	for _ in $(seq 1 600); do
		grep -q '^phase run' "$OUT/$name.err" 2>/dev/null && break
		sleep 0.05
	done
	sample "$pid" "$secs" 1 -mayDie -file "$OUT/$name.sample.txt" >/dev/null 2>&1
	wait "$pid"
	python3 -I - "$OUT/$name.json" "$name" <<'EOF'
import json, sys
r = json.loads(open(sys.argv[1]).read().strip().splitlines()[-1])
p = [x for x in r["phases"] if x["name"] == "run"][0]
print("%-24s run %.2f s ops/s %.0f cpu/op %.2f us written %.1f MiB" % (sys.argv[2], p["seconds"], p["ops_per_sec"], p["cpu_us_per_op"], p.get("written", 0) / 2**20))
EOF
	# The heaviest frames by self time, and each thread's total.
	awk '/Sort by top of stack/{on=1;next} on&&NF{print} /Binary Images/{exit}' "$OUT/$name.sample.txt" | head -30 | sed 's/^/   top /'
	grep -E '^ +[0-9]+ Thread_' "$OUT/$name.sample.txt" | head -8 | sed 's/^/   thr /'
}

T=(--records 1000000 --ops 1000000 --value-size 100 --threads 1 --durability none --batch 1 --memory-mb 12 --max-seconds 20)
one tenx-fillseq-strata strata 2 --workload fillseq "${T[@]}"
one tenx-fillseq-rocksdb rocksdb 2 --workload fillseq "${T[@]}" --variant lvl-16k-lz4
one tenx-overwrite-strata strata 2 --workload overwrite "${T[@]}"
one tenx-overwrite-rocksdb rocksdb 2 --workload overwrite "${T[@]}" --variant lvl-16k-lz4
K=(--workload fillrandom --records 200000 --ops 200000 --value-size 1024 --threads 1 --durability none --batch 1 --memory-mb 256 --max-seconds 20)
one fill1k-strata strata 1 "${K[@]}"
one fill1k-rocksdb rocksdb 1 "${K[@]}" --variant lvl-4k-none
W=(--workload fillrandom --records 1000000 --ops 1000000 --value-size 100 --threads 1 --durability wal --batch 1 --memory-mb 256 --max-seconds 20)
one fillwal-strata strata 2 "${W[@]}"
one fillwal-rocksdb rocksdb 2 "${W[@]}" --variant lvl-16k-lz4
rm -rf "$OUT"/data-*
echo done
