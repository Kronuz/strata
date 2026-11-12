#!/bin/sh
# run.sh beside a writer that keeps the device's cache dirty (4 MiB written and fsync'd in a loop,
# no F_FULLFSYNC), as other indexes' commits do on a host serving many (docs/perf/open-sync.md).
# Usage: loaded.sh OUT [COUNT [PAIRS [ARGS]]], as a strict job (ARGS as run.sh's).
set -eu
out=$1; count=${2:-1000}; pairs=${3:-6}; args=${4:-}
python3 -c '
import os, sys
p = sys.argv[1]; b = os.urandom(4 << 20)
while True:
    fd = os.open(p, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644)
    os.write(fd, b); os.fsync(fd); os.close(fd)
' "$out/writer.dat" &
w=$!
trap 'kill $w 2>/dev/null; rm -f "$out/writer.dat"' EXIT
sh "$(dirname "$0")/run.sh" "$out" "$count" "$pairs" "$args"
