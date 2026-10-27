# Re-measure of three hunt-list rows, 2026-10-26

Strata on origin/main 5a0299b (includes 451d360, reclaim backlog and background deletes), Release, macOS x86_64, 16 CPUs. Commands are `.scratch/kv-cov/run.sh` unchanged (`bench/kv/run.py rows`, sets `big1` and `sizes1`, `lvl-16k-lz4` RocksDB 11.11), with `KV_SEGREPORT=1`. 3 interleaved trials per row and engine under `quiet strict`; medians, spread is min-max over the 3. Old numbers are the medians of 5 trials from `.scratch/kv-cov/out` (main f2d6107).

Verdict: both disk rows are closed. The memory row is not, but the RSS is mostly clean pages, not enforced-budget dirty memory; the budget does not cover what RSS counts.

## Row 1: 10x RAM, peak RSS (big1, 1M records, 12 MiB; ycsb_a 200k x 1 KiB, 24 MiB)

| Workload | Strata before | Strata now [spread] | RocksDB now [spread] | Strata vs RocksDB now |
|---|---|---|---|---|
| fillrandom v100 | 353 MiB | 146 [145-149] | 70 [67-70] | 2.1x larger |
| readrandom v100 | 390 | 192 [189-204] | 122 [116-131] | 1.6x larger |
| seekrandom v100 | 361 | 186 [183-217] | 94 [93-98] | 2.0x larger |
| ycsb_a v1024 | 645 | 246 [238-250] | 147 [145-149] | 1.7x larger |

Strata is 2.4x, 2.0x, 1.9x and 2.6x smaller than before; the loss against RocksDB shrank from 5.3x/3.1x/3.4x/4.5x to 1.6-2.1x. Throughput is still 3.3-5.1x faster on fill/read/ycsb and 1.3x faster on seek (strata/RocksDB: 400,937/121,768; 370,260/90,876; 68,210/51,204; 264,752/77,574 ops/s).

Is the budget enforced? Split from `vmmap -summary` taken at the highest `ps` RSS sample (`.scratch/vm.py`; snapshots are at 154 MiB of a 213 MiB peak, so they undercount the final jump):

| Case | Peak RSS | Physical footprint (dirty) peak | Clean freed malloc, resident | Mapped file, resident | Budget capacity / `budget_peak` |
|---|---|---|---|---|---|
| strata readrandom v100 | 213 | 40.5 MiB | 67.5 MiB (MALLOC_MEDIUM empty) + 60.6 MiB (MALLOC_MEDIUM, 0.4 MiB dirty) | 28.7 MiB | 3 / 45.3 MiB |
| rocksdb readrandom v100 | 131 | 53.3 MiB | 35.7 + 13.9 MiB | 0 | n/a |
| strata ycsb_a v1024 | 248 | 66.1 MiB | 26.0 MiB | 117.8 MiB | 6 / 13.9 MiB |

- The budget (a `byte_budget` of memory/4, floor 1 MiB: 3 MiB and 6 MiB here) covers memtable and flush buffers only. It is exceeded by forced draws: `budget_peak` is 45 MiB on the 1M-record rows, 15x its capacity. So the enforced part is not enforced under bulk load.
- Anonymous dirty memory is 14-66 MiB, below RocksDB's 23-53 MiB footprint on the same rows for read, above it for ycsb_a v1024 (66 vs not measured).
- The rest of RSS is clean: freed malloc pages the allocator keeps resident (about 128 MiB on readrandom) and, on ycsb_a, 118 MiB of mapped segment pages. Neither is counted by the budget.

Dominant cause: freed-but-resident malloc arenas (MALLOC_MEDIUM, the large transient flush/merge/read buffers) on fill/read/seek, and mapped segment pages on ycsb_a v1024. Budget overshoot (45 MiB vs 3 MiB) is the secondary one. Hunt target: return transient buffers (madvise or a bounded arena, or `malloc_zone_pressure_relief`), draw read and merge buffers from the budget, and measure `phys_footprint` rather than RSS for the budget claim. Caveat: macOS RSS includes these clean pages; the Linux anon/file split is not measured here.

## Row 2: 10x RAM, disk (same rows, `disk` of the final directory)

| Workload | Strata before | Strata now [spread] | RocksDB | Strata vs RocksDB now |
|---|---|---|---|---|
| fillrandom v100 | 203.6 MB | 72.3 [72.3-72.4] | 70.5 | 1.03x larger (was 2.9x) |
| readrandom v100 | 208.5 | 72.3 [72.2-72.3] | 70.5 | 1.03x larger |
| seekrandom v100 | 206.6 | 72.4 [72.4-72.4] | 70.5 | 1.03x larger |
| ycsb_a v1024 | 316.9 | 135.9 [135.7-139.0] | 121.3 | 1.12x larger (was 2.6x) |

Closed for v100: strata is 2.8x smaller than before and within 3% of RocksDB. The only residue is ycsb_a v1024: 8 live segments, `reclaim_pending 0 merges_pending 0`, dead documents 6,506 + 16,105 + 4,010 + 348 + 277 + 146 + 163 of 200,000 live (about 27,500 dead of 227,500 stored documents, 12%), so roughly 16 MB, about the whole 14.6 MB gap to RocksDB, is dead bytes in live segments that no merge has chosen to rewrite. Not a reclaim-queue or merge-debt backlog; it is the merge policy's dead-ratio threshold. Hunt target (small, 12% gap): lower the dead-ratio trigger for large segments in the merge policy, if the gap is worth the write amplification.

## Row 3: 64 KiB values, YCSB A (sizes1, 2000 records, 12 MiB)

| Metric | Strata before | Strata now [spread] | RocksDB now [spread] | Strata vs RocksDB now |
|---|---|---|---|---|
| disk | 651.7 MB | 90.2 [87.8-90.3] | 100.0 [86.3-100.0] | 10% smaller (was 6.5x larger) |
| peak RSS | 811 MiB | 416 [401-440] | 530 [502-543] | 22% smaller (was 1.5x larger) |
| ops/s | n/a | 17,966 | 8,321 | 2.2x faster |

Closed on both: disk is 7.2x smaller than before, memory 1.9x smaller. Strata ends with 6 segments, `reclaim_pending 0 merges_pending 0`, dead documents 496 of 2,896 stored, about 15 MB. RSS at the sampled peak (snapshot 134 MiB of 201 in the single vmmap run, so it undercounts): footprint peak 13.9 MiB, clean freed malloc 85 MiB, mapped file 37.6 MiB; `budget_peak` 65.8 MiB against 3 MiB capacity. RocksDB's footprint peak is 23.0 MiB. Remaining RSS is the same clean-malloc cause as row 1, not dirty memory.

## Identity

Get digests (`KV_GET_DIGEST=1`, 2 runs per engine) match between strata and RocksDB and across runs: readrandom v100 1M records `9743db1c4f882e56`, hits 100000; readrandom v65536 `16c0f0bb67fe5911`, hits 100000. The ycsb_a digests differ between engines and between runs of RocksDB because the 20 s cap stops each run at a different operation count (50,259 vs 2,684-3,010 hits); not a comparable digest, so there is none for the YCSB A rows. No A/A was run; the three trials per cell are the noise estimate.

## Hunt-list update

1. Row 2 and row 3 disk: closed. Remaining: ycsb_a v1024 at 1.12x RocksDB, caused by dead bytes in live segments below the merge trigger.
2. Row 1 and row 3 memory: open at 1.6-2.1x RocksDB on the 10x-RAM rows; dominant cause clean freed malloc pages plus mapped segments, with the budget not covering reads or merges and overshooting its capacity 15x.

Raw rows: `.scratch/out-big1`, `.scratch/out-sizes1` (not committed; summaries above).
