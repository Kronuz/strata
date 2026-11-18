#!/bin/bash
# peak-rss-ab.sh BASE NEW [TRIALS]: flush_cadence interleaved, main (A), main again (A/A), branch (B),
# and both under MallocSpaceEfficient=1 (macOS: freed medium-zone pages returned at once).
base=$1; new=$2; trials=${3:-5}
for t in $(seq 1 $trials); do
  for cfg in "500 8 m" "500 8 n" "500 32 m" "100 8 m"; do
    read -r v mb mode <<< "$cfg"
    env=(env -u CADENCE_NO_MERGE); [ $mode = n ] && env=(env CADENCE_NO_MERGE=1)
    for arm in A A2 B; do
      bin=$base; [ $arm = B ] && bin=$new
      echo "$arm t$t $v $mb $mode $("${env[@]}" $bin $v $mb 400000 d-$arm)"
    done
    if [ "$(uname)" = Darwin ] && [ $mode = m ] && [ $v = 500 ]; then
      for arm in A B; do
        bin=$base; [ $arm = B ] && bin=$new
        echo "$arm-mse t$t $v $mb $mode $(env -u CADENCE_NO_MERGE MallocSpaceEfficient=1 $bin $v $mb 400000 d-$arm)"
      done
    fi
  done
done
