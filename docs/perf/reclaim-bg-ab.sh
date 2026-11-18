#!/bin/zsh
# usage: ab.sh ROUNDS  -- interleaved A (main) and B (branch); run under quiet strict
R=~/code/Kronuz/strata-wt/reclaim-bg/.scratch/ab
cd $R
run() { arm=$1; bin=$2
 rm -rf data
 KV_GET_DIGEST=1 $bin --dir $R/data --threads 1 --durability none --batch 1 --max-seconds 60 --workload ycsb_a --records 2000 --ops 20000 --value-size 65536 --memory-mb 256 > o.json 2>o.err
 python3 -c "
import json;r=json.load(open('o.json'));ph=' '.join('%s=%d/%.2fs'%(p['name'],p.get('ops_per_sec',0),p['seconds']) for p in r['phases'] if p['name'] in ('run','settle'))
print('$arm','disk',r['disk'],'files',r['files'],ph,'total',round(r['total_seconds'],2),open('o.err').read().split('digest')[-1].strip())"
 rm -rf data; }
for i in $(seq $1); do
 echo "-- round $i"
 run A $R/kv_main
 run B $R/kv_branch
done
