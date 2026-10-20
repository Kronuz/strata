# strata's core against RocksDB: the `document` profile, quick tier

Status: plan, written before the first measurement. Results replace the "Results" section.

## What it measures

PLAN's `document` profile gate at the quick tier (BENCHMARKS.md, "Tiers"; PLAN, "Ratchets",
"Targeted runs"): strata's lifecycle core as a key-value engine, format 13's
`document_layout` (`keys` and `stored` only, layers.md L12) through `basic_index`, against
RocksDB pinned and built locally on its fastest configuration per value size
(`rocksdb-survey.md`, section 4). It is the first measurement of the core alone; it gates
nothing (a quick-tier ratio is a direction, not a verdict) and names where strata loses.

## Hypothesis

From the survey's mapping (section 2), before measuring:

1. Point gets from segments lose: every get decompresses a whole zstd frame of up to 64 KiB
   (K6), where RocksDB reads a 4 KiB block from its cache.
2. Scans lose by more: records are in docid order, not key order (K5), so a key-ordered
   scan decodes one frame per record, and buffered keys have no sorted view (K3).
3. Writes are closer: stage 1 resolves every key (Q1) where RocksDB writes blind, but the
   buffer is cheaper than a skiplist memtable; under `sync`, strata pays a full commit per
   batch (no standalone turn log, K2), so durable rows lose by an order of magnitude.
4. Memory and disk: strata's mmap'd reads count file pages in RSS; RocksDB's block cache is
   bounded. Disk is close, both compressing; write amplification lower for strata's tiered
   merges than for leveled compaction.

A hypothesis stays only if the numbers show it; the "Top losses" section says which held.

## Procedure

- **Harness.** `bench/kv/kvgen.hh` (one seeded stream: 16-byte keys `Q` + 15 digits of a
  bijective scramble of the record id, db_bench's 0.5-compressible values, YCSB's scrambled
  zipfian and latest), `bench/kv/kvdriver.hh` (phases, HDR-style latency histogram,
  `getrusage` and the OS's bytes-written counter, disk bytes), one driver per engine:
  `bench/kv/kv_strata.cc` (target `kv_strata`) and `bench/engines/rocksdb-11.11/kv_rocksdb.cc`
  (built by its `build.sh`). `bench/kv/run.py` calibrates, interleaves, scores, and times
  every phase.
- **Workloads.** db_bench's `fillseq`, `fillrandom`, `overwrite`, `readrandom`,
  `readmissing` (absent keys, the filter row), `readseq`, `seekrandom` (seek plus 10 next)
  at 100-byte values; YCSB A to F at 1 KiB values (A 50/50 read/update, B 95/5, C read
  only, D read latest with 5% inserts, E scans of 1 to 100 with 5% inserts, F
  read-modify-write), zipfian except D. 100,000 records and about 100,000 operations per
  row (`seekrandom` 10,000 seeks, `readseq` one pass), each row loaded by `fillrandom` first.
- **Memory.** One budget M per row: RocksDB's HyperClockCache of M with the write buffer
  manager charging M/4 of memtables to it, index and filter blocks in the cache; strata's
  `byte_budget` of M/4 for its buffers (it has no block cache; reads go through mmap).
  "Fits": M = 256 MiB. "2×": M = half the row's logical bytes.
- **Durability.** `none` (RocksDB `disableWAL`, one flush at the end; strata one commit at
  the end) and `sync` (each batch durable before the next: RocksDB `sync=true` on the
  batch's `WriteBatch`; strata `commit()` after the batch). RocksDB's default `wal` (written,
  not synced) is a report row for RocksDB alone (owner, Q3). On macOS both issue
  `F_FULLFSYNC` (RocksDB's `HAVE_FULLFSYNC`, strata's `completion` IO).
- **Threads.** One, then four. RocksDB's four threads share one DB; strata's four threads
  each own one index (the many-indexes model, Q4), with the same keys per thread.
- **Many indexes.** 1,000 strata indexes against one RocksDB DB with 1,000 column families,
  100,000 records in all, routed by key hash; reopen time, RSS and descriptors after open,
  then `readrandom`.
- **Background work.** Two threads each: strata's pool executor for merges, RocksDB's
  `max_background_jobs`.
- **Calibration.** Once per engine, at one thread: RocksDB's variants (leveled and universal;
  4 and 16 KiB blocks; LZ4, zstd, none; the data block hash index) over `fillrandom`,
  `readrandom`, and YCSB A at each value size, the fastest by geometric mean kept for every
  row of that value size.
- **Trials.** Five per row, the engines interleaved (A B A B, the order alternated per
  round), each trial its own process. An A/A row (each engine against itself) measures the
  noise. Ratios are strata over RocksDB on the row's median, with a 95% bootstrap interval.
- **Holds.** `quiet check` first; one-thread rows under a light hold, four-thread rows under a
  strict one, each run wrapped in `quiet run`.
- **Axes.** Throughput, p50/p99/p99.9 latency; peak and end RSS; disk bytes after settling and
  bytes written over logical bytes (the OS counter: `proc_pid_rusage` on macOS,
  `/proc/self/io` on Linux); CPU per operation (user plus system, every thread).

## What would keep or remove a conclusion

A row's direction stands when the interval excludes 1.0 and the median effect is at least
3% (BENCHMARKS rule 3); inside the A/A noise band it is reported as a tie. A loss is
attributed only with a profile of strata's side naming where its time goes.

## Results

Pending.
