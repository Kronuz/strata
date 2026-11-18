# ycsb_a v1024 at 10x RAM: dead documents in live segments

Base: origin/main 5b238e1, Release, macOS x86_64. Row: `ycsb_a`, 200k records x 1 KiB, 24 MiB, 1 thread, 100k ops, `KV_SEGREPORT=1`. RocksDB (lvl-16k-lz4) disk from `remeasure-2026-10-26.md`: 121.3 MB.

## Where the policy declines (question 1)

`merge_policy::find_merges` (`include/strata/core/merge_policy.hh`) runs reclaiming merges only while the whole index's deleted fraction is over `deletes_pct_allowed` (20%, Lucene's default). It is an index-wide fraction, not per segment; there is no per-segment trigger and the tier pass ignores deletes except through live bytes. The final dead fraction here is 11-14%, so nothing fires. Dead documents at settle (arm 20): 23-33k of 223-233k.

## Experiment 1: lower `deletes_pct_allowed` (bench-only switch `KV_DELPCT` in `bench/kv/kv_strata.cc`)

3 interleaved trials per arm under `quiet strict`, arms 20 (base), 20 (A/A), 10, 5. Medians [min-max].

| Arm | disk MB | written/logical (WA) | run ops/s | run p99 us | dead at settle |
|---|---|---|---|---|---|
| 20 base | 138.8 [137.9-141.6] | 1.41 [1.38-1.54] | 249.2k [250.2-261.9k] | 8.9 | ~12% |
| 20 A/A | 139.2 [136.5-142.3] | 1.49 [1.40-1.54] | 248.8k [241.7-257.0k] | 9.2 | ~12% |
| 10 | 125.7 [124.6-126.3] | 1.85 [1.84-1.89] | 244.2k [238.8-250.8k] | 9.0 | 2-3% |
| 5 | 125.4 [125.2-128.1] | 2.01 [1.99-2.14] | 247.0k [242.4-247.7k] | 9.0 | 2.5% |

- Disk at 10: 9.4% smaller than base (125.7 vs 138.8), 1.04x RocksDB's 121.3 (was 1.14x on this run set). The A/A spread is about +-3 MB, so the 13 MB drop is well outside noise.
- Write amplification at 10: 1.3x higher than base (1.85 vs 1.41-1.49). Run-phase ops/s and p99 are within the A/A spread (the merges run in the background and the 100k ops finish before they matter).
- 5 buys no more disk than 10 and costs 8% more writes than 10 (2.01 vs 1.85): the remaining 4 MB over RocksDB is not dead documents.
- Identity: `--after readall` reports 0 misses for arms 20 and 10 (every key present). The ycsb_a driver emits no get digest, so this is a weaker check than a digest; a digest-bearing row (readrandom) is unaffected by the switch because it has no overwrites.

## Verdict

Cheapest change: `deletes_pct_allowed` 20 -> 10 (one constant; still index-wide). It closes 13 of the 17.5 MB gap (to 1.04x RocksDB) for about 30% more bytes written, with no measured run-phase throughput or p99 cost on this 100k-op row. Whether it is worth it under a sustained overwrite load is not measured here (the run finishes in under a second of merge-free time); that needs a long ycsb_a or rww row. Not tried: dead-aware selection or a settle-time pass (5 would not beat 10 on disk, so the residue is not dead documents).
