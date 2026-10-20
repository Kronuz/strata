# strata's search layout used as a key-value store, against RocksDB (quick tier)

Status: done, 2026-10-20. **This is not the core-versus-RocksDB comparison.** It measures what
exists on main `68e3d29`: `basic_index<search_layout>` holding keys and stored records only
(no terms), driven as a key-value store. `basic_index<document_layout>` does not compile on
main (stage 1, the buffer, and the flush reach `inverted`'s parts: `terms`, `split_text`), so
the KV-only layout of L12 could not be run. strata is being split into `strata::core` and
`strata::search`; the core gets a key-ordered, block-cached KV read path (format 14) and the
durable turn log, and the real run follows that, with this harness. The losses below are
stated as inputs to that design: what the core's read path, cache, and log must achieve to win
each row.

**Audited** (`rocksdb-fairness-audit.md`): the sync YCSB rows are 4.99× and 5.02× slower once
RocksDB's load stays out of the run (A1); strata's "to durable" fills were charged an empty
second commit (A2); the many-index settle is about 3.97× slower with RocksDB's atomic flush and
that commit removed (A3); and the 2× fill wins are macOS barrier latency on RocksDB's flushes,
reversed (1.51× and 3.69× slower) when barriers are cheap (A4).

## Setup

- **Host:** the development Mac (Intel Core i9-9980HK, 8 cores / 16 threads, 32 GB, macOS 15.8.1,
  APFS), Apple clang 17, Release (`-O3 -DNDEBUG`, no `-march`) for both engines. Not the gate
  VM: a quick-tier direction, not a gate row (BENCHMARKS rule 4, "Tiers").
- **strata:** main `68e3d29` plus the harness (`perf/core-vs-rocksdb`), `search_layout`, key
  prefix `Q`, one `byte_budget` of M/4 for buffers, auto merge on a two-thread pool executor,
  reads through mmap.
- **RocksDB:** 11.11.1 (release branch `11.11.fb` at `ec93377878cd`; upstream tags stop at
  v11.8.1), built from source with zstd and LZ4, system allocator, `PORTABLE=1`,
  `HAVE_FULLFSYNC` (it issues `F_FULLFSYNC` on macOS, as strata does: Q9 answered).
  HyperClockCache of M with the write buffer manager charging M/4 of memtables to it, index and
  filter blocks in the cache (L0's pinned), Bloom 10 bits per key, `max_background_jobs` 2.
- **Calibration** (once, one thread, one trial per variant, `calibration.jsonl.gz`): leveled
  with 16 KiB blocks and LZ4 (`lvl-16k-lz4`) was fastest by geometric mean over `fillrandom`,
  `readrandom`, and YCSB A at both 100-byte and 1 KiB values; the six variants spread 5–15%
  on reads, up to 2× on 1 KiB writes (zstd slowest). It is RocksDB's reference for every row.
- **Rows:** 100,000 records; db_bench rows at 100-byte values, YCSB at 1 KiB; 16-byte keys;
  about 100,000 operations per row (`seekrandom` 10,000 seeks of 10, `readseq` one pass);
  every run phase capped at 20 s (15 s durable), its rate over what it finished. "Fits" is
  M = 256 MiB; "2×" is M = half the logical bytes (5 MiB at 100 B, 49 MiB at 1 KiB).
- **Trials:** five per row (three for the durable, WAL, and many-index rows), engines
  interleaved and alternated per round, each trial its own process. Ratios are strata over
  RocksDB on the medians with a 95% bootstrap interval.

### Noise and the disk cleanup

A disk cleanup of about 90 GB ran on the host between about 06:30 and 07:05 without a hold. It
overlapped the first `fits1` (06:25–06:33), A/A (06:34–06:38), and `twox1` (06:38–07:06) sets.
All three were rerun under a light hold (07:41–08:26) as `fits1@2`, `aa@2`, and `twox1@2`; only
the reruns are quoted below. Both sets are in `core-vs-rocksdb/report.md`; no row changed
direction; ratios moved by 1–17% between them, `readseq` by 36% (162× to 220× slower) and the
2× YCSB F settle by one 67 s merge wait. The durable,
WAL, four-thread, and many-index sets ran after 07:05.

A/A on the rerun (each engine against itself, alternate trials as sides): RocksDB's spread is
8–14%; strata's is 4% on writes but 38–55% on reads, its point-read path being dominated by
kernel `madvise` calls (below), which vary with host load. **A ratio inside 10% is a tie at this
tier**; every read ratio below is past 7×, far outside either band.

## Results

`thr` threads; `s/r` strata/RocksDB. Throughput is the run phase; "to durable" adds the settle
that makes the run durable (flush, merges or compactions, commit). Memory is peak RSS (strata's
includes its mmap'd segment pages; RocksDB's block cache is anonymous memory). Disk is bytes
after settling; "written" is bytes the OS wrote over logical key-plus-value bytes.

### One thread, fits (M = 256 MiB), durability none (`fits1@2`)

| Row | strata ops/s | RocksDB ops/s | Throughput [95% CI] | To durable | p50 µs s/r | p99 µs s/r | p99.9 µs s/r | CPU µs/op s/r | Peak RSS | Disk | Written |
|---|---:|---:|---|---|---|---|---|---|---|---|---|
| fillseq | 925,282 | 1,007,775 | tie (0.92 [0.87, 1.01]) | 1.76× slower | 0.53/0.66 | 2.6/2.6 | 10.8/5.2 | 1.08/0.99 | 3.3× the memory | 26% less | 25% less |
| fillrandom | 1,022,736 | 528,976 | 1.93× faster [1.63, 2.03] | 1.30× slower | 0.50/1.53 | 2.4/4.5 | 11.2/9.9 | 0.98/1.89 | 3.6× the memory | 27% less | 26% less |
| overwrite | 732,956 | 592,558 | 1.24× faster [1.09, 1.40] | 1.80× slower | 1.08/1.38 | 3.0/4.0 | 9.9/20.4 | 1.36/1.69 | 3.4× the memory | tie | tie |
| readrandom | 12,517 | 403,889 | 32.3× slower [0.021, 0.039] | 32.3× slower | 45.8/1.9 | 481/7.3 | 1384/26 | 79.6/2.5 | 2.5× the memory | 27% less | 26% less |
| readmissing | 4,475,993 | 1,574,732 | 2.84× faster [2.56, 3.14] | 2.84× faster | 0.08/0.43 | 0.16/1.7 | 3.8/18.3 | 0.22/0.63 | 2.6× the memory | 27% less | 26% less |
| readseq (records/s) | 9,383 | 2,068,509 | 220× slower | 220× slower | one pass: 10.7 s / 0.048 s | | | 106/0.47 per record | 90% more | 27% less | 26% less |
| seekrandom | 1,354 | 129,602 | 95.7× slower [0.009, 0.012] | 95.7× slower | 569/5.9 | 2703/25 | 4358/50 | 726/7.7 | 2.6× the memory | 27% less | 26% less |
| YCSB A | 25,546 | 199,158 | 7.80× slower [0.089, 0.145] | 7.41× slower | 2.2/2.4 | 346/32 | 856/63 | 38.5/5.4 | 30% more | 12% less | 50% less |
| YCSB B | 11,568 | 232,086 | 20.1× slower [0.041, 0.081] | 17.5× slower | 45.3/3.4 | 469/20 | 1122/47 | 86.2/4.3 | 30% more | 26% less | 62% less |
| YCSB C | 9,646 | 249,598 | 25.9× slower [0.033, 0.042] | 25.9× slower | 44.3/2.8 | 522/18 | 954/32 | 101/4.0 | 27% more | 26% less | 63% less |
| YCSB D | 31,413 | 295,269 | 9.40× slower [0.080, 0.157] | 8.27× slower | 0.48/1.08 | 461/22 | 864/58 | 31.7/3.4 | 31% more | 26% less | 62% less |
| YCSB E | 193 | 29,676 | 154× slower [0.004, 0.014] | 152× slower | 4178/31 | 20840/90 | 25821/173 | 4747/34 | 28% more | 29% less | 62% less |
| YCSB F | 6,339 | 66,549 | 10.5× slower [0.087, 0.207] | 10.3× slower | 71/9.9 | 1630/59 | 6128/420 | 118/13.7 | 29% more | 12% less | 50% less |

### One thread, 2× (M = half the logical bytes), durability none (`twox1@2`)

macOS cannot cap the page cache, so this regime bounds each engine's in-process memory only
(RocksDB's cache and memtables, strata's buffers); both read file pages from the page cache.
A cgroup-capped 2× row needs the gate tier's sizes (the survey's 1 GiB and 4 GiB under
`memory.max`): at 100,000 records both processes' fixed RSS exceeds a limit of half the data.

| Row | strata ops/s | RocksDB ops/s | Throughput | To durable | p99 µs s/r | CPU µs/op s/r | Peak RSS | Disk | Written |
|---|---:|---:|---|---|---|---|---|---|---|
| fillseq | 578,145 | 107,574 | 5.37× faster | 14.3× slower | 1.4/2.3 | 1.73/1.58 | 3.7× the memory | 27% less | 2.0× |
| fillrandom | 534,008 | 98,842 | 5.40× faster | 12.9× slower | 2.0/3.1 | 1.87/2.93 | 3.0× the memory | 24% less | 51% less |
| overwrite | 224,963 | 94,501 | 2.38× faster | 16.4× slower | 7.5/2.0 | 4.44/3.95 | 3.8× the memory | 39% more | 52% less |
| readrandom | 10,845 | 134,643 | 12.4× slower | 12.4× slower | 375/25 | 92.2/7.4 | 47% more | 24% less | 23% less |
| readmissing | 4,984,181 | 725,542 | 6.87× faster | 6.87× faster | 0.17/5.9 | 0.20/1.38 | 2.4× the memory | 24% less | 51% less |
| readseq (records/s) | 10,289 | 1,854,256 | 180× slower | 180× slower | — | — | 71% more | 24% less | 51% less |
| seekrandom | 916 | 41,522 | 45.3× slower | 45.3× slower | 2572/65 | 1087/24 | 79% more | 24% less | 23% less |
| YCSB A | 18,780 | 149,597 | 7.97× slower | 11.0× slower | 424/30 | 53.2/7.8 | 2.4× the memory | 59% more | 68% less |
| YCSB B | 9,734 | 86,678 | 8.90× slower | 8.51× slower | 897/43 | 102/11.5 | 73% more | 26% less | 64% less |
| YCSB C | 9,403 | 135,503 | 14.4× slower | 14.4× slower | 469/23 | 106/7.4 | 92% more | 26% less | 64% less |
| YCSB D | 26,328 | 243,792 | 9.26× slower | 8.27× slower | 350/22 | 38.0/4.1 | 2.0× the memory | 26% less | 64% less |
| YCSB E | 190 | 20,981 | 111× slower | 110× slower | 17170/124 | 5272/48 | 75% more | 29% less | 63% less |
| YCSB F | 8,554 | 90,505 | 10.6× slower | 51.9× slower | 782/37 | 120/14.2 | 2.7× the memory | 46% more | 67% less |

### Durable rows, one thread (`sync1`, three trials)

Each batch durable before the next: RocksDB `sync=true` on the batch's `WriteBatch` (one WAL
`F_FULLFSYNC`); strata `commit()` after the batch (no standalone turn log exists, K2). Rows run
15 s each, so disk and written bytes compare unequal amounts of data and are omitted.

| Row | strata ops/s | RocksDB ops/s | Throughput [95% CI] | p50 µs s/r | p99 µs s/r | CPU µs/op s/r |
|---|---:|---:|---|---|---|---|
| fillrandom, batch 100 | 897 | 4,674 | 5.21× slower [0.157, 0.206] | 0.54/0.10 | 138/24 | 24.3/3.9 |
| overwrite, batch 100 | 923 | 4,597 | 4.98× slower [0.193, 0.205] | 3.1/0.10 | 71/42 | 42.6/6.4 |
| YCSB A, batch 1 | 20 | 93 | 4.74× slower [0.204, 0.215] | 1106/144 | 111,673/36,438 | 3340/466 |
| YCSB F, batch 1 | 19 | 96 | 4.93× slower [0.186, 0.216] | 1204/120 | 121,111/25,297 | 3688/501 |
| fillrandom, batch 1 | 9 | 44 | 4.80× slower [0.185, 0.216] | 105,382/21,103 | 147,849/38,011 | 1830/219 |

RocksDB's WAL tier (written, not synced; owner Q3: a report row for RocksDB alone, strata has
none): `fillrandom` 126,339 ops/s and `overwrite` 126,165, against 528,976 and 592,558 with
`disableWAL`, so its WAL write costs it 4.2–4.7× on this host. strata's side of that row ran
its `none` tier and is not a comparison.

### Four threads, fits (`fits4`, strict hold)

RocksDB's four threads share one DB; strata's each own one index (Q4's many-index answer),
with the same keys per thread. `readseq` is left out (a shared DB's thread scans every key).

| Row | strata ops/s | RocksDB ops/s | Throughput [95% CI] | To durable | p99 µs s/r | CPU µs/op s/r | Peak RSS |
|---|---:|---:|---|---|---|---|---|
| fillseq | 3,159,783 | 979,555 | 3.23× faster [2.41, 13.3] | 3.43× slower | 2.9/25 | 1.22/3.92 | 2.3× the memory |
| fillrandom | 3,411,926 | 828,503 | 4.12× faster [2.89, 23.8] | 3.3× slower (wide) | 2.9/26 | 1.12/4.79 | 2.3× the memory |
| overwrite | 2,784,351 | 933,345 | 2.98× faster [2.42, 18.3] | 4.50× slower | 3.0/15 | 1.37/4.22 | 2.1× the memory |
| readrandom | 68,081 | 1,720,337 | 25.3× slower [0.034, 0.048] | 25.3× slower | 239/3.9 | 50.7/2.3 | 72% more |
| readmissing | 8,419,531 | 4,465,277 | 1.89× faster [1.80, 2.29] | 1.89× faster | 0.23/1.9 | 0.46/0.87 | 87% more |
| seekrandom | 5,061 | 398,346 | 78.7× slower | 78.7× slower | 1876/35 | 683/9.7 | 94% more |
| YCSB A | 110,553 | 765,730 | 6.93× slower | 8.18× slower | 379/25 | 35.8/5.6 | 73% more |
| YCSB B | 61,266 | 791,392 | 12.9× slower | 12.1× slower | 457/25 | 56.8/5.0 | 69% more |
| YCSB C | 37,989 | 881,380 | 23.2× slower | 23.2× slower | 473/23 | 86.2/4.5 | 58% more |
| YCSB D | 116,832 | 1,083,415 | 9.27× slower | 11.3× slower | 363/27 | 28.4/3.7 | 62% more |
| YCSB E | 752 | 137,242 | 182× slower | 171× slower | 21627/84 | 5237/29 | 74% more |
| YCSB F | 59,040 | 592,265 | 10.0× slower | 8.58× slower | 408/27 | 60.3/7.1 | 76% more |

strata's write rows scale with four independent indexes (3.3–3.5× its one-thread rate);
RocksDB's shared DB gains 1.0–1.6×. The durable side loses because the harness settles the four
indexes one after another, each commit's barriers in series.

### Many indexes: 1,000 strata indexes against 1,000 RocksDB column families (`many`, three trials)

100,000 records of 100 bytes in all, routed by an FNV-1a hash of the key; one process, one
thread, the same budget M = 256 MiB shared (strata's `byte_budget`, RocksDB's cache and write
buffer manager). Medians of three.

| Phase or axis | strata | RocksDB | Direction |
|---|---:|---:|---|
| Create 1,000 (durable) | 85.96 s | 22.32 s | 3.85× slower |
| Load 100,000 | 0.15 s | 0.15 s | tie |
| Settle the load (flush, commit) | 148.89 s | 86.50 s | 1.72× slower |
| Reopen 1,000 | 0.66 s | 1.04 s | 1.58× faster |
| RSS after reopen | 58 MiB | 137 MiB | 58% less memory |
| Peak RSS | 73 MiB | 143 MiB | 49% less memory |
| `readrandom` | 96,014 ops/s | 282,941 ops/s | 2.95× slower |
| Disk | 6.32 MiB | 25.11 MiB | 75% less disk |
| Files / descriptors | 5,000 / 1,004 | 1,009 / 1,010 | 5× the files, equal descriptors |

The point-read loss shrinks from 32× (one index) to 2.95× here because each index's stored
section is one frame of about 10 KB instead of 64 KiB: the per-get cost is the frame's size,
which is the first loss below.

## Top losses and their causes (profiled), as inputs to the core's KV design

Profiles are macOS `sample` call trees of strata's driver (`.scratch/prof/` in the worktree):
main-thread samples at 1 ms.

### 1. Point gets: a whole 64 KiB zstd frame decoded, and its buffer freed, per get

`readrandom` 32× slower, YCSB C 26×, B 20×; 80 µs of CPU per get against 2.5 µs.

- Profile (`readrandom`, 100,000 records): zstd decompression (Huffman and sequences) **47%**,
  `madvise` **43%**, everything else under 10%; the key lookup itself (filter, FST, key
  block) is under 2%.
- The record lives in a stored frame of up to 64 KiB (about 650 records at 100 B); every get
  decodes the whole frame into a new heap buffer (`decompress_zstd` returns a fresh
  `std::string`) and frees it. On macOS a 64 KiB free goes back through the medium allocator's
  `madvise`, nearly as expensive as the decompression.
- The key path is already faster than RocksDB's: `readmissing` (filter negatives) is 2.8×
  faster at one thread and 6.9× at 2×, and the many-index row, whose frames are about 10 KB,
  narrows the loss to 2.95×.
- **What the core's read path must achieve to win `readrandom` and YCSB B/C:** at most about
  2.5 µs of CPU per cached get at 100 B (RocksDB's), so the work per get must be a few KiB, not
  a frame: blocks of about 4 KiB addressed by key (format 14's key-ordered layout), a cache of
  decoded blocks with lock-free lookups charged to the budget (K6, HyperClockCache's role), and
  decoding into a reused buffer so no get allocates. RocksDB's chosen 16 KiB LZ4 blocks set
  the bar; zstd at 64 KiB frames buys 27% less disk, which a per-block codec choice
  (LZ4 hot, zstd at merge) can keep.

### 2. Scans: records in docid order, so a key-ordered scan decodes a frame per record

`readseq` 220× slower (9,383 records/s against 2.07 M), `seekrandom` 96×, YCSB E 154×.

- Profile (`seekrandom`): the same frame decode and `madvise` as gets, now once per record
  returned: the keys of a range are adjacent in the key section, but their records sit in
  docid order, so ten consecutive keys hit ten different frames. Buffered keys have no sorted
  view either; the driver keeps one (a sorted set of keys written since the last flush,
  standing in for K3), and it is not where the time goes.
- **What the core must achieve:** records stored in key order (K5 as the layout itself, which
  format 14's key-ordered read path does), so a scan of n records decodes about
  n × record size / block size blocks, sequentially; plus a merged iterator across segments and
  the buffer with bounds. To win `readseq` the core needs about 2 M records/s at 100 B on this
  host, which is block-sequential decoding at memory bandwidth, not per-record work.

### 3. Durability: five barriers per commit, and merges that wait for budget

Durable rows 4.7–5.2× slower; "to durable" fill rows 1.3–1.8× slower at fits and 13–16×
slower at 2×.

- Profile (`fillrandom`, sync per 100): `fcntl(F_FULLFSYNC)` is **96%** of the thread's time,
  in five barriers per commit of about equal cost: the data files (`sync_files`), the new
  manifest, and three directory syncs. RocksDB's sync write is one WAL barrier, which matches
  the uniform 4.7–5.2× (and on Linux, where barriers are cheaper, the ratio will differ).
- At 2×, the settle that makes a fill durable takes 15.3 s against 0.19 s. Profile: no CPU;
  the background merge sleeps in `pool_executor::sleep_until`, waiting `merge_memory_wait`
  (5 s) for room on a budget of M/4 = 1.25 MiB it never gets, three rounds in a row, while
  the writer waits in `publish_merges`. RocksDB's compactions draw no budget.
- **What the core's log must achieve:** one barrier per acknowledged batch (the turn log, K2:
  group commit, apply overlapping the barrier), so a durable write costs a log append and
  segment publication moves off the acknowledgement path. Merges under a tight budget must
  either fit a bounded working set or not wait on the writer's budget at all; a 5 s wait per
  round is a stall, not backpressure.

### Also measured

- **Memory per buffered record:** RSS grows 683 B per buffered 116-byte record (100,000 to
  300,000 records, no flush; three trials each), against 153 B for RocksDB's memtable: 4.5×.
  The heap at 300,000 records is 117 MB in a few doubling arrays (one of 53 MB, two of 16 MB,
  five of 4 MB): growth by doubling and the `search_layout` buffer's per-document arrays. It
  is why strata's fill rows peak at 3–3.7× RocksDB's RSS. The core's buffer should cost the
  record plus a small fixed overhead, grown in chunks (the arena chunks the tuple buffer
  already uses for terms).
- **1 KiB loads** are 2.7× slower than RocksDB's (0.96 s against 0.35 s for 100,000 records):
  profile `madvise` 40% (buffer growth), zstd compression 33% (the flush compresses on the
  writing thread, where RocksDB flushes in the background with LZ4), copies 12%.
- **Where strata already wins:** blind writes into the buffer (`fillrandom` 1.9×, `overwrite`
  1.2× at fits, 5.4× and 2.4× at 2×, 3–4× at four threads), absent-key reads (2.8–6.9×), disk
  (24–29% less at equal content, from zstd over LZ4; more disk where overwrites are not yet
  merged at 2×), bytes written (25–68% less), and the many-index row's memory (58% less after
  reopen), reopen time (1.58× faster), and disk (75% less).
- **The `search_layout` stand-in costs** one `norms` byte per document (100,000 bytes, 1.8% of
  the 100-byte index) and a few empty sections; a `document_layout` segment carries neither.

### Cost at the stated scale

At 80 µs of CPU per point get against 2.5 µs, a billion cached gets cost 22.1 CPU-hours on
strata's current path against 0.68 on RocksDB's: about $1.00 against $0.03 at $0.045 per
vCPU-hour (an on-demand c7i rate). Small per billion, but for a host of many small indexes
serving reads it is 32× the cores, which is what the core's read path has to remove.

## Phase times

Per run, medians in seconds, strata/RocksDB (`report.md` has every row):

| Row | Open | Load | Load settle | Run | Settle | Total |
|---|---|---|---|---|---|---|
| fits1@2 readrandom | 0.090/0.224 | 0.097/0.179 | 0.299/0.125 | 7.989/0.248 | 0/0 | 8.53/0.78 |
| fits1@2 YCSB A | 0.085/0.226 | 0.966/0.352 | 0.168/0.518 | 3.915/0.502 | 0.593/0.106 | 5.70/1.65 |
| fits1@2 YCSB E | 0.083/0.213 | 1.230/0.332 | 0.168/0.503 | 20.134/3.370 | 0.196/0.076 | 21.89/4.49 |
| twox1@2 fillrandom | 0.084/0.213 | — | — | 0.187/1.012 | 15.306/0.190 | 15.59/1.42 |
| twox1@2 YCSB F | 0.085/0.215 | 0.942/1.409 | 5.673/0.613 | 11.690/1.105 | 67.011/0.407 | 85.41/3.76 |
| many readrandom | 85.96/22.32 (create) | 0.15/0.15 | 148.9/86.5 | 1.01/0.35 (+ reopen 0.66/1.04) | 0/0 | 237/110 |

Session (`phases.jsonl`): strata configure 44 s and build about 1 min; RocksDB build about
11 min (4 jobs, nice); calibration 70 s; `fits1` 501 s; A/A 254 s; `twox1` 1,660 s; `sync1`
504 s; `wal1` 16 s; `fits4` 330 s; `many` 1,038 s; reruns `fits1@2` 622 s, `aa@2` 285 s,
`twox1@2` 1,817 s. The 2× rows' length is strata's merge waits (15 s per settle).

## What did not run, and why

- `document_layout`: does not compile as an index on main (above).
- A cgroup-capped 2× and any Linux or FreeBSD row: macOS has no page-cache cap, and at
  100,000 records a cap of half the data is below either process's fixed RSS. The gate tier's
  sizes on the Linux VM do it properly.
- `deleterandom`, `multireadrandom`, `readwhilewriting`, `mergerandom`: not in this first pass.
- jemalloc for either engine (Q5): both ran the system allocator.

## Reproduce

```sh
cmake -S . -B build-rel -G Ninja -DCMAKE_BUILD_TYPE=Release && ninja -C build-rel kv_strata
bench/engines/rocksdb-11.11/build.sh            # ~/.scratch/rocksdb-11.11-build/kv_rocksdb
export KV_STRATA=$PWD/build-rel/kv_strata KV_ROCKSDB=~/.scratch/rocksdb-11.11-build/kv_rocksdb
python3 bench/kv/run.py calibrate OUT           # then KV_VARIANT_100 / KV_VARIANT_1024
QUIET_LEVEL=light quiet run kv 30 "kv rows" -- python3 bench/kv/run.py rows OUT fits1 5
python3 bench/kv/run.py report OUT
```

Raw results: `core-vs-rocksdb/raw.jsonl.gz` (every run's JSON line, both sets),
`calibration.jsonl.gz`, `phases.jsonl`, and the generated `report.md`.
