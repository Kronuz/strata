> Status: SUPERSEDED (2026-10-21) merge working memory is now streamed and bounded (docs/perf/flushmem.md, docs/perf/merge-stream.md).

# 10x RAM: peak RSS against the memory budget (explore/bigmem)

Question: big1 rows, strata peak RSS 353-645 MiB against M = 12-24 MiB (RocksDB 67-144 MiB).
Harness: `.scratch/explore-bigmem/{run1,sweep,sweep2}.sh` (kv_strata, Release, macOS arm64, strict holds).

## Finding

The budget is not enforced for merges. The kv bench gives strata a byte_budget of M/4 (3 MiB at
M=12). `budget_peak` (the budget's own accounting) reaches 96-115 MiB on big1 fillrandom/readrandom
and 210-419 MiB on ycsb_a/1 KiB: 30-100x the capacity. It is honest accounting, not a leak: a
merge builds each output section (stored, keys, ...) in memory, charged on its flush_charge "never
waiting for room", so working memory scales with the merged segment. The default tiered policy
(floor 16 MiB, max_merged_bytes 5 GiB) merges up to 10 segments into ~100 MiB at a 175 MB data set.

RSS split on a paused big1 fillrandom (footprint at peak, 352 MB RSS): 65 MB dirty (phys_footprint
peak 88 MB), 53 MB mapped segment files, and ~248 MB "reclaimable" malloc pages (freed merge buffers
macOS keeps resident; ru_maxrss counts them). So RSS = real merge working set (~85 MB) x ~4.
malloc_zone_pressure_relief after each merge returns the LARGE part: peak 337 -> ~232 MiB, but
MALLOC_MEDIUM stays resident; a partial fix, dropped.

## Fix prototyped (switch KV_MAXMERGE_KB in kv_strata.cc): cap merged segment size

`policy.max_merged_bytes = N KiB`, `floor_bytes = min(floor, N/2)`. Single runs, digest identical
at every setting (KV_GET_DIGEST: readrandom 9743db1c4f882e56 hits 100000; ycsb_a
c33a1d6dd42e3995 hits 50259).

| row | max merge | peak RSS MiB | budget_peak MiB | segments | load s |
|---|---|---|---|---|---|
| big1 fillrandom | default | 337 / 301 | 96 / 96 | 5 | 2.51 / 2.52 |
| | 8 MiB | 207 | 29 | 14 | 2.06 |
| | 2 MiB | 120 | 9 | 50 | 2.14 |
| big1 readrandom | default | 361 / 342 | 115 / 96 | 4-5 | 2.5 / 2.54 |
| | 8 MiB | 234 | 30 | 18 | 2.11 |
| | 2 MiB | 163 | 9 | 49 | 2.11 (run 0.30 vs 0.26 s) |
| ycsb_a 1 KiB | default | 875 / 668 | 419 / 210 | 3-4 | load 0.88, run 0.5 |
| | 4 MiB | 266 / 270 | 11 | 49 | 0.80, 0.46 |
| | 2 MiB | 258 / 261 | 11 | 49 | 0.77, 0.45 |

Direction: peak RSS 2.5-3.3x smaller on ycsb_a, 2.8x (fillrandom) and 2.1x (readrandom) smaller at 2 MiB;
fill is 15-20% faster (less merge rewriting), reads at 49 segments 0.30 vs 0.26 s (+15%, one pair;
not A/B'd). Spread: the two default runs differ by 12-30% in RSS, so ratios are indicative; the
A/A under the strict hold was the repeated default run. Disk on the run is smaller too (68 vs 181 MB:
less transient merge garbage).

## What's left

The ~120-260 MiB remaining is mapped segment files (reclaimable file pages) plus ~9-11 MiB budget
peak (3x the 3 MiB cap: flush working sets). Not done: derive the cap from the budget in the library
(e.g. max_merged_bytes ~ k x capacity, with a many-segment read/filter cost check), bound merge
section memory by streaming (the real fix; performance-engineer item), and a mapped-pages
residency measure for RSS reporting. Reads at 50 segments need a proper A/B (readrandom, seekrandom).
Not RocksDB-comparable until the cap is a policy default scaled to the budget.
