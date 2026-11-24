#!/usr/bin/env bash
# Probe A (rocksdb-parity design): memory map, bytes written by file class, and disk by file, on
# the sync1/wal1 rows that lose, for strata (logged and unlogged) and RocksDB. Short runs.
set -u
P=$(cd "$(dirname "$0")" && pwd)
O=$HOME/.scratch/strata/rocksdb-parity
S=$PWD/build-rel/kv_strata
R=$HOME/.scratch/rocksdb-11.11-build/kv_rocksdb
OUT=$O/a-$(date +%H%M%S)
mkdir -p "$OUT"
cc -O2 -dynamiclib "$P/wbytes.c" -o "$OUT/wbytes.dylib" || exit 1
echo "OUT=$OUT"

# name engine pause args...
one() {
	local name=$1 eng=$2 pause=$3
	shift 3
	local bin=$S var=()
	if [ "$eng" = rocksdb ]; then bin=$R; fi
	local data=$OUT/data-$name
	rm -rf "$data"
	(
		export WBYTES_LOG=$OUT/$name.wbytes.tsv DYLD_INSERT_LIBRARIES=$OUT/wbytes.dylib KV_PHASE_MARKS=1
		if [ "$pause" -gt 0 ]; then export KV_PAUSE_AFTER_RUN=$pause; fi
		exec "$bin" --dir "$data" "$@"
	) >"$OUT/$name.json" 2>"$OUT/$name.err" &
	local pid=$!
	if [ "$pause" -gt 0 ]; then
		# The pause starts after the run phase: wait for its mark, then for the run to end.
		for _ in $(seq 1 600); do
			grep -q '^phase run' "$OUT/$name.err" 2>/dev/null && break
			sleep 0.1
		done
		sleep "$((MAXS + 1))"
		vmmap -summary "$pid" >"$OUT/$name.vmmap" 2>&1
		heap -s "$pid" >"$OUT/$name.heap" 2>&1 || true
	fi
	wait "$pid"
	echo "== $name exit $?"
	find "$data" -type f -exec stat -f '%z %b %N' {} + | sort -k3 >"$OUT/$name.files"
	python3 -I - "$OUT/$name.json" <<'EOF'
import json, sys
r = json.loads(open(sys.argv[1]).read().strip().splitlines()[-1])
ph = {p["name"]: p for p in r["phases"]}
print("  peak %.1f MiB end %.1f MiB disk %.2f MiB written %.1f MiB logical %.1f MiB" % (r["peak_rss"] / 2**20, r["end_rss"] / 2**20, r["disk"] / 2**20, r["written"] / 2**20, r["logical"] / 2**20))
for n, p in ph.items():
    print("   %-12s s=%.2f rss=%.0f MiB written=%.1f MiB ops=%s" % (n, p["seconds"], p.get("rss", 0) / 2**20, p.get("written", 0) / 2**20, p.get("ops", "")))
print("  stats", json.dumps(r["stats"])[:300])
EOF
	python3 -I - "$OUT/$name.wbytes.tsv" <<'EOF'
import sys
rows = [l.rstrip("\n").split("\t") for l in open(sys.argv[1])][1:]
for ph, cls, z, b, c, f, s in rows:
    if int(b) > 0 or int(f) or int(s):
        print("   w %-12s %-28s zero=%s %9.2f MiB calls=%s F_FULLFSYNC=%s fsync=%s" % (ph, cls, z, int(b) / 2**20, c, f, s))
EOF
	echo "  files (size, 512-byte blocks):"
	sed 's|'"$data"'/||; s/^/    /' "$OUT/$name.files" | head -40
	grep -E 'MALLOC|mapped file|TOTAL|VM_ALLOCATE|Physical footprint' "$OUT/$name.vmmap" 2>/dev/null | head -30 | sed 's/^/   vm /'
	grep -iE 'All zones|DefaultMallocZone|Process 0|Physical' "$OUT/$name.heap" 2>/dev/null | head -8 | sed 's/^/   heap /'
}

MAXS=3
Y=(--workload ycsb_a --records 100000 --ops 300 --value-size 1024 --threads 1 --batch 1 --memory-mb 256 --max-seconds $MAXS)
one ycsbA-sync-strata strata 6 "${Y[@]}" --durability sync
one ycsbA-none-strata strata 6 "${Y[@]}" --durability none
one ycsbA-sync-rocksdb rocksdb 6 "${Y[@]}" --durability sync --variant lvl-4k-none
MAXS=5
F=(--workload fillrandom --records 100000 --ops 100000 --value-size 100 --threads 1 --memory-mb 256 --max-seconds $MAXS)
one fill100-sync-strata strata 0 "${F[@]}" --durability sync --batch 100
one fill100-sync-rocksdb rocksdb 0 "${F[@]}" --durability sync --batch 100 --variant lvl-16k-lz4
one fill1-sync-strata strata 0 "${F[@]}" --durability sync --batch 1
one fill1-sync-rocksdb rocksdb 0 "${F[@]}" --durability sync --batch 1 --variant lvl-16k-lz4
one fillwal-strata strata 0 "${F[@]}" --durability wal --batch 1
one fillwal-rocksdb rocksdb 0 "${F[@]}" --durability wal --batch 1 --variant lvl-16k-lz4
W=(--workload overwrite --records 100000 --ops 100000 --value-size 100 --threads 1 --memory-mb 256 --max-seconds $MAXS)
one overwal-strata strata 10 "${W[@]}" --durability wal --batch 1
one overwal-rocksdb rocksdb 10 "${W[@]}" --durability wal --batch 1 --variant lvl-16k-lz4
rm -rf "$OUT"/data-*
echo done
