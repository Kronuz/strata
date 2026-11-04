#!/bin/bash
# peak-rss-fix.sh BASE NEW FIX [TRIALS]: flush_cadence interleaved, main (A), branch (B), branch with
# charge-lane.patch (F); merges on.
base=$1; new=$2; fix=$3; trials=${4:-5}
for t in $(seq 1 $trials); do
  for cfg in "100 8" "500 8" "500 32" "1024 32"; do
    read -r v mb <<< "$cfg"
    for arm in A B F; do
      bin=$base; [ $arm = B ] && bin=$new; [ $arm = F ] && bin=$fix
      echo "$arm t$t $v $mb m $(env -u CADENCE_NO_MERGE $bin $v $mb 400000 d-$arm)"
    done
  done
done
