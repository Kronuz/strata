#!/usr/bin/env bash
# Probe (rocksdb-parity D2 item 2, N9): one checkpoint_floor run under wbytes.c, from a strata
# checkout with the probe built in BUILD (default build). Prints the probe's JSON line and the run
# phase's bytes written by file class: the turn log (host/), zero-fill apart, and the indexes.
#   checkpoint-floor.sh [BUILD] -- probe arguments (--trigger B --indexes N --writes W ...)
set -u
P=$(cd "$(dirname "$0")" && pwd)
B=build
if [ $# -gt 0 ] && [ "$1" != "--" ]; then B=$1; shift; fi
[ "${1:-}" = "--" ] && shift
O=$HOME/.scratch/strata/checkpoint-floor
OUT=$O/run-$(date +%H%M%S)-$$
mkdir -p "$OUT"
echo "OUT=$OUT"
cc -O2 -dynamiclib "$P/wbytes.c" -o "$OUT/wbytes.dylib" || exit 1
ulimit -n 65536 2>/dev/null || ulimit -n 10240
WBYTES_LOG=$OUT/wbytes.tsv DYLD_INSERT_LIBRARIES=$OUT/wbytes.dylib "$B/checkpoint_floor" "$OUT/data" "$@" >"$OUT/run.json" 2>"$OUT/run.err"
rc=$?
cat "$OUT/run.json"
grep -v '^phase ' "$OUT/run.err" | tail -5
python3 -I - "$OUT/wbytes.tsv" <<'EOF'
import sys
rows = [l.rstrip("\n").split("\t") for l in open(sys.argv[1])][1:]
tot = {}
for ph, cls, z, b, c, f, s in rows:
    if ph != "run":
        continue
    kind = "log" if cls.startswith("host/") else "index"
    if z == "1":
        kind += " zero"
    t = tot.setdefault(kind, [0, 0, 0])
    t[0] += int(b); t[1] += int(f); t[2] += int(s)
for k in sorted(tot):
    print("run %-12s %10.1f MiB  F_FULLFSYNC=%d fsync=%d" % (k, tot[k][0] / 2**20, tot[k][1], tot[k][2]))
print("run total        %10.1f MiB" % (sum(t[0] for t in tot.values()) / 2**20))
EOF
du -sk "$OUT/data/host" "$OUT/data" 2>/dev/null | sed 's/^/disk KiB /'
rm -rf "$OUT/data"
exit $rc
