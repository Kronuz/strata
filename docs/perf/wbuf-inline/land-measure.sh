#!/bin/bash
# land-measure.sh OUT [cadence]: the lane-charge landing's numbers. flush_cadence (merges on) for main (A),
# the branch before the charge (B) and after it (F), 2 trials (its heap, budget and record columns
# are deterministic); then bench/kv/ab.py's wbufw rows, B against F and B against itself, 11 trials.
set -e
W=/Users/kronuz/code/Kronuz/strata-wt/wbuf-inline
S=$W/.scratch
out=$1; mkdir -p $out
for t in 1 2; do
  for cfg in "100 1" "500 1" "100 8" "500 8" "500 32" "1024 32"; do
    read -r v mb <<< "$cfg"
    for arm in A B F; do
      bin=$S/rss/cad-base; [ $arm = B ] && bin=$S/rss/cad-new; [ $arm = F ] && bin=$S/cadence-fix
      rm -rf $out/d-$arm
      echo "$arm t$t $v $mb m $(env -u CADENCE_NO_MERGE $bin $v $mb 400000 $out/d-$arm)"
    done
  done
done > $out/cadence.txt
rm -rf $out/d-A $out/d-B $out/d-F
[ "${2:-}" = cadence ] && exit 0
cd $W
KV_A=$S/kv-prefix KV_B=$S/kv-fix KV_DIR=$out/data-ab python3 bench/kv/ab.py rows $out/ab wbufw 11
KV_A=$S/kv-prefix KV_B=$S/kv-prefix KV_DIR=$out/data-aa python3 bench/kv/ab.py rows $out/aa wbufw 11
python3 bench/kv/ab.py report $out/ab > $out/ab-report.txt
python3 bench/kv/ab.py report $out/aa > $out/aa-report.txt
rm -rf $out/data-ab $out/data-aa
