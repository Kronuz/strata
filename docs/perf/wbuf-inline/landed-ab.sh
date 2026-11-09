#!/bin/bash
# landed-ab.sh: the landing A/B of the inline record against strata main, from this checkout's root.
#   landed-ab.sh build MAIN   kv_strata and flush_cadence at MAIN (A) and this checkout (B), in .scratch/ab
#   landed-ab.sh cadence OUT  flush_cadence (merges on, 400,000 writes) at shares 64 KiB to 32 MiB, A and B;
#                             its counts are deterministic, so 1 trial; then the table and the digest check
#   landed-ab.sh rr OUT       readrandom (bench/kv/ab.py's wbufrr set), 41 interleaved trials: B against A, B against B
set -eu
AB=$PWD/.scratch/ab
mode=$1
case $mode in
build)
	M=$(cd "$2" && pwd); mkdir -p "$AB"
	for arm in A B; do
		src=$PWD; [ $arm = A ] && src=$M
		bd=$AB/build-$arm
		cmake -S "$src" -B "$bd" -G Ninja -DCMAKE_BUILD_TYPE=Release > "$AB/cmake-$arm.log"
		ninja -C "$bd" -j4 kv_strata kv_proto_bufmem > "$AB/build-$arm.log"
		cp "$(find "$bd" -name kv_strata -type f -perm -u+x | head -1)" "$AB/kv-$arm"
		ld=$(ninja -C "$bd" -t commands kv_proto_bufmem | tail -1)
		exe=$(echo "$ld" | grep -o -- "-o [^ ]*" | tail -1 | cut -d' ' -f2)
		cc=$(ninja -C "$bd" -t commands kv_proto_bufmem | grep -- "-c [^ ]*bench/kv/proto/bufmem.cc" | tail -1)
		obj=$(echo "$cc" | grep -o -- "-o [^ ]*" | cut -d' ' -f2)
		cc=${cc/-o $obj/-o $AB/cad-$arm.o}
		cc=${cc/ -c / -I$src -c }
		cc=${cc/$src\/bench\/kv\/proto\/bufmem.cc/$PWD/docs/perf/wbuf-inline/flush_cadence.cc}
		ld=${ld/$obj/$AB/cad-$arm.o}
		ld=${ld/-o $exe/-o $AB/cad-$arm}
		(cd "$bd" && eval "$cc" && eval "$ld")
		rm -f "$AB/cad-$arm.o"
		echo "arm $arm: $(git -C "$src" rev-parse --short=12 HEAD)"; file "$AB/kv-$arm" "$AB/cad-$arm"
	done
	rm -rf "$AB/build-A" "$AB/build-B"
	;;
cadence)
	out=$2; mkdir -p "$out"
	for v in 100 200 500; do
		for share in 64K 96K 256K 1 8 12 32; do
			for arm in A B; do
				rm -rf "$out/d-$arm"
				echo "$arm $v $share $("$AB/cad-$arm" $v $share 400000 "$out/d-$arm")"
			done
		done
	done > "$out/cadence.txt"
	rm -rf "$out/d-A" "$out/d-B"
	python3 - "$out/cadence.txt" <<'P'
import json, sys
rows = {}
for line in open(sys.argv[1]):
    arm, v, share, js = line.split(" ", 3)
    rows.setdefault((int(v), share), {})[arm] = json.loads(js)
print("| value, share | flushes A/B | records/flush A | B | B/A | max RSS MiB A/B | digest |")
print("|---|---|---:|---:|---:|---|---|")
same, lo, hi = True, 9e9, 0.0
for (v, share), r in rows.items():
    a, b = r["A"], r["B"]
    x = b["records_per_flush"] / a["records_per_flush"]
    lo, hi = min(lo, x), max(hi, x)
    same &= a["digest"] == b["digest"]
    print(f"| {v} B, {share} | {a['flushes']}/{b['flushes']} | {a['records_per_flush']:,.0f} | {b['records_per_flush']:,.0f} | {x:.2f}x | {a['max_rss'] >> 20}/{b['max_rss'] >> 20} | {'same' if a['digest'] == b['digest'] else 'DIFFER'} |")
print(f"B/A records per flush: {lo:.2f}-{hi:.2f}x; digests identical across arms: {same}")
sys.exit(0 if same and lo > 1.0 else 1)
P
	;;
rr)
	out=$2; mkdir -p "$out"
	KV_A=$AB/kv-A KV_B=$AB/kv-B KV_DIR=$out/data python3 bench/kv/ab.py rows "$out/ab" wbufrr 41
	KV_A=$AB/kv-B KV_B=$AB/kv-B KV_DIR=$out/data python3 bench/kv/ab.py rows "$out/bb" wbufrr 41
	for x in ab bb; do python3 bench/kv/ab.py report "$out/$x" > "$out/$x-report.txt"; echo "== $x"; head -3 "$out/$x-report.txt"; done
	rm -rf "$out/data"
	;;
*) echo "usage: landed-ab.sh build MAIN | cadence OUT | rr OUT" >&2; exit 2 ;;
esac
