#!/usr/bin/env bash
# Probe E (rocksdb-parity design): one log per loop, one shared log, and per-loop logs with one
# barrier per device, as a C floor (devbarrier.c), T = 1, 2, 4, 8 threads, 5 s each, without and
# with the cohort wait (1 ms).
set -u
P=$(cd "$(dirname "$0")" && pwd)
O=$HOME/.scratch/strata/rocksdb-parity
OUT=$O/e-$(date +%H%M%S)
mkdir -p "$OUT/data"
cc -O2 "$P/devbarrier.c" -o "$OUT/devbarrier" || exit 1
"$OUT/devbarrier" "$OUT/data" 5 0
"$OUT/devbarrier" "$OUT/data" 5 1000
rm -rf "$OUT/data"
echo done
