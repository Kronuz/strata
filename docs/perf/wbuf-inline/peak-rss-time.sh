#!/bin/bash
# peak-rss-time.sh BASE NEW FIX2 [TRIALS]: wall seconds of flush_cadence, interleaved, merges on.
base=$1; new=$2; fix=$3; trials=${4:-5}; TIMEFORMAT=%R
for t in $(seq 1 $trials); do
  for cfg in "100 8" "500 8" "500 32"; do
    read -r v mb <<< "$cfg"
    for arm in A B F; do
      bin=$base; [ $arm = B ] && bin=$new; [ $arm = F ] && bin=$fix
      s=$( { time env -u CADENCE_NO_MERGE $bin $v $mb 400000 d-$arm > /dev/null; } 2>&1 )
      echo "$arm t$t $v $mb $s"
    done
  done
done
