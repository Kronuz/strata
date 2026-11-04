#!/bin/bash
# chunk-measure.sh OUT [cadence|rr]: the chunk-growth round's numbers. flush_cadence (merges on, 400,000
# writes) at shares 64 KiB to 32 MiB for main (A), the lane charge with doubling chunks (F,
# strata:8b1de41) and with next_chunk (G); its counts are deterministic, so 1 trial. Then readrandom
# (bench/kv/ab.py's wbufrr set), 41 interleaved trials each: A against G, F against G, G against itself.
set -e
W=/Users/kronuz/code/Kronuz/strata-wt/wbuf-inline
S=$W/.scratch
out=$1; mkdir -p $out
if [ "${2:-}" != rr ]; then
  for v in 100 200 500; do
    for share in 64K 96K 256K 1 8 12 32; do
      for arm in A F G; do
        rm -rf $out/d-$arm
        echo "$arm $v $share $($S/chunk/cad-$arm $v $share 400000 $out/d-$arm)"
      done
    done
  done > $out/cadence.txt
  rm -rf $out/d-A $out/d-F $out/d-G
fi
[ "${2:-}" = cadence ] && exit 0
cd $W
KV_A=$S/kv-main KV_B=$S/chunk/kv-G KV_DIR=$out/data python3 bench/kv/ab.py rows $out/ag wbufrr 41
KV_A=$S/kv-fix KV_B=$S/chunk/kv-G KV_DIR=$out/data python3 bench/kv/ab.py rows $out/fg wbufrr 41
KV_A=$S/chunk/kv-G KV_B=$S/chunk/kv-G KV_DIR=$out/data python3 bench/kv/ab.py rows $out/gg wbufrr 41
for x in ag fg gg; do python3 bench/kv/ab.py report $out/$x > $out/$x-report.txt; done
rm -rf $out/data
