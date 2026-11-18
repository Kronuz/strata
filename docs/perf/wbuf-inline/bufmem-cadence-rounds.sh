#!/bin/bash
# bytes per record (base, u32 lengths, varint lengths) and records per flush (base, new), interleaved rounds
cd "$(dirname "$0")"
for r in 1 2 3 4 5; do
  for cfg in "document 100" "carry 100" "carry-extra 100" "document 500" "document 1500"; do
    read -r l v <<< "$cfg"
    for arm in base u32 varint; do
      b=b-base; [ $arm = u32 ] && b=b-u32; [ $arm = varint ] && b=b-rel
      [ $arm = base ] && [ $l != document ] && continue
      o=$(./$b/kv_proto_bufmem --layout $l --value $v --dir bm2-$arm 2>&1 | tr '\n' ' ')
      rss=$(echo "$o" | grep -o '"rss_per_record":[0-9.]*' | cut -d: -f2); hp=$(echo "$o" | grep -o '"heap_per_record":[0-9.]*' | cut -d: -f2); dg=$(echo "$o" | grep -o 'scan digest [0-9a-f]*' | cut -d' ' -f3); ix=$(echo "$o" | grep -o 'index digest [0-9a-f]*' | cut -d' ' -f3)
      echo "bufmem $l $v $arm round$r rss=$rss heap=$hp scan=$dg index=$ix"
    done
  done
done
for r in 1 2 3; do
  for cfg in "100 8" "100 32" "500 8" "500 32" "1024 32"; do
    read -r v mb <<< "$cfg"
    for arm in base new; do echo "cadence $arm round$r $(./cadence-$arm $v $mb 400000 cad2-$arm)"; done
  done
done
