#!/bin/sh
# Probe (rocksdb-parity D9): tenx1 fillseq, then readall over the runs it leaves, on kv_strata
# binaries A and B interleaved TRIALS times. Prints per run the get digest (identity: equal on A
# and B), write amplification, segments, and the run and readall phases' rate and CPU.
#   runs-readall.sh A_BIN B_BIN OUT [TRIALS]
set -eu
A=$1 B=$2 OUT=$3 T=${4:-3}
mkdir -p "$OUT"
i=0
while [ "$i" -lt "$T" ]; do
	for side in A B; do
		eval bin=\$$side
		rm -rf "$OUT/id"
		KV_GET_DIGEST=1 "$bin" --dir "$OUT/id" --workload fillseq --after readall --records 1000000 --ops 100000 \
			--value-size 100 --threads 1 --durability none --batch 1 --memory-mb 12 --max-seconds 20 >"$OUT/id.json" 2>"$OUT/id.err"
		python3 - "$side" "$OUT/id.json" "$OUT/id.err" <<'EOF'
import json, re, sys
side, js, err = sys.argv[1:]
r = json.loads(open(js).read().strip().splitlines()[-1])
d = re.search(r"get digest (\w+) hits (\d+)", open(err).read())
ph = {p["name"]: p for p in r["phases"]}
print(f"{side}: digest {d.group(1)} hits {d.group(2)} wamp {r['written'] / r['logical']:.3f} segments {r['stats']['segments']}"
      f" run {ph['run']['ops_per_sec']:.0f} ops/s {ph['run']['cpu_us_per_op']:.3f} us/op"
      f" readall {ph['after']['ops_per_sec']:.0f} ops/s {ph['after']['cpu_us_per_op']:.3f} us/op p99 {ph['after']['p99_us']:.2f}", flush=True)
EOF
	done
	i=$((i + 1))
done
rm -rf "$OUT/id" "$OUT/id.json" "$OUT/id.err"
