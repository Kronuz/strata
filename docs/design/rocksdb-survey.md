# RocksDB survey: mechanisms, strata's equivalents, and the road to parity

Status: survey for the architect's review. No code. It catalogs RocksDB's mechanisms,
maps each to strata as it stands on `main` (format 12, Phase 3 lifecycle, the layers
design approved), recommends what the core takes and in what order, lists what it
rejects, and plans the benchmark that the `document` profile's comparison needs
(`workload-profiles.md`, "How it is measured"). Recommendations are K1–K8, rejections
X1–X12, measure-first items M1–M9, and open questions Q1–Q10.

RocksDB is dual-licensed GPLv2 and Apache-2.0. This document describes its mechanisms
in our own words from its public wiki, blog, release notes, and papers, cited by tag
(the references are at the end). No RocksDB code enters strata. RocksDB enters the
harness only as a separately built baseline binary, like Lucene and Tantivy.

## Why RocksDB is the reference here

`layers.md` (L12) makes a segment with only `keys` and `stored` a sorted key-value
store with versions, snapshots, and atomic batches: `document_layout`. The `document`
and `graph` profiles are measured against RocksDB (`workload-profiles.md`). PLAN's
Goal 1 asks strata to beat every baseline on speed, indexing, memory, and disk, so
RocksDB joins the baseline set for those profiles with the same ratchet rules as
Lucene and Tantivy have for search.

The two engines share a shape and differ in one decision:

```
RocksDB:  put ─▶ WAL ─▶ memtable (sorted) ─▶ flush ─▶ L0 SST ─▶ compaction ─▶ L1..Ln (key-range partitioned)
strata:   apply ─▶ stage 1 (key → docid, version) ─▶ buffer ─▶ flush ─▶ segment (docid order) ─▶ tiered merge
```

RocksDB writes are blind: a put or delete appends a record with a sequence number and
never reads the old value; the newest record shadows older ones until compaction drops
them [fast21]. strata resolves every keyed write in stage 1: one live copy per key and
per docid is an invariant, versions increment, and conditional writes check the
current version (`phase-3.md`, D8, D16). Most of the differences below follow from
that, and Q1 asks whether the read it costs is to be kept.

## 1. RocksDB's mechanisms

Each entry: what it is, what it solves, its published effect where one exists, and the
citation. "No published number" means the cited page gives none.

### Write path

| Mechanism | What it is | What it solves | Published effect | Cite |
|---|---|---|---|---|
| Skiplist memtable | Default memtable: a concurrent skiplist ordered by internal key, about 1.33 pointers of overhead per entry; the only memtable that supports concurrent inserts | Sorted in-memory buffer serving gets and scans before flush | No published number | [memtable] |
| Hash skiplist, hash linklist, vector memtables | Buckets by key prefix (each a skiplist or a sorted list), or an unsorted vector sorted at flush | Prefix-local workloads and random-write-heavy loads with fewer pointers per entry; total-order scans become expensive | No published number | [memtable] |
| Memtable flush triggers | `write_buffer_size` (64 MB default) per memtable, `max_write_buffer_number` (2 default), `db_write_buffer_size` across column families, `max_total_wal_size` | Bounds memory and WAL length; flushes the largest or the oldest memtable | No published number | [memtable], [wal] |
| Write buffer manager | One object caps memtable memory across column families and DB instances; flushes at about 90% of the limit for the mutable memtable; can charge memtable memory to the block cache by inserting placeholder entries, so one capacity bounds memtables, data blocks, and (with `cache_index_and_filter_blocks`) index and filter blocks; `allow_stall` stalls writers over the limit | Node-wide memory bound | No published number | [wbm] |
| WAL | One log for all column families; records written after every write by default (process-crash consistency); `WriteOptions::sync` adds an fsync; `manual_wal_flush`, `disableWAL`, `wal_compression`; old logs deleted once every column family flushed past them | Durability before flush | No published number | [wal] |
| WAL format and recycling | 32 KiB blocks, records fragmented (first, middle, last) with a CRC each; the recyclable format binds each record to its log number so stale records in a reused file are rejected | Reusing preallocated log files without rewriting them | No published number | [walfmt] |
| WAL recovery modes | Tolerate a corrupted tail, absolute consistency, point-in-time recovery, or skip corrupted records | Choice of what recovery does with damage | No published number | [walrec] |
| Group commit | Concurrent writers join one queue; the head becomes leader and writes the group's WAL records and memtable inserts | Amortizes the WAL write and sync over many writers | No published number by itself | [pipe] |
| Pipelined write | The next group's WAL write overlaps the previous group's memtable insert (`enable_pipelined_write`) | Serialization of WAL and memtable stages | +20% write throughput (db_bench, ramfs, compaction not the bottleneck); about +30% with 8 writers on tmpfs, 64 MB skiplist memtable, LZ4 | [pipe] |
| Concurrent memtable writes | Writers of a group insert into the skiplist in parallel (`allow_concurrent_memtable_write`, on by default) | Single-threaded memtable insert | No published number | [memtable] |
| Unordered write | Writers insert into the memtable without waiting for earlier sequence numbers (`unordered_write`) | Write-queue serialization; gives up atomic reads and immutable snapshots unless combined with WritePrepared transactions | +63% (WAL) and +131% (no WAL) alone; +42% and +34% with WritePrepared | [unordered] |
| Write stalls | Slowdown and stop triggers on immutable memtable count, L0 file count, and estimated pending compaction bytes; during a slowdown writes are paced at `delayed_write_rate`; `no_slowdown` returns `Incomplete` instead of blocking | Keeps compaction debt and read amplification bounded when writes outrun compaction | Stalls observed 40–70% of the time on large-value overwrite benchmarks without key-value separation | [stall], [blob-blog] |
| Rate limiter | Token refill every 100 ms by default over `rate_bytes_per_sec`, flush at high priority, compaction at low; WAL writes are not limited | Background I/O bursts that hurt foreground latency | No published latency number | [rl] |
| Auto-tuned rate limiter | Multiplicative increase and decrease of the limit from how often the limiter drains, between a user ceiling and a derived floor | Choosing a rate without tuning | At 10 MB/s ingest under a 1000 MB/s ceiling the chosen limit hovered near 125 MB/s; p90–p100 background write rates lower | [autorl] |

### Storage: tables, indexes, filters, compression

| Mechanism | What it is | What it solves | Published effect | Cite |
|---|---|---|---|---|
| Block-based table | The default SST: data blocks of sorted key-values with prefix-compressed keys and restart points, an index block, filter and other meta blocks, a meta-index, and a footer; block size is a tuning knob (`benchmark.sh` uses 8 KiB) | General-purpose on-disk sorted run | No published number | [bbt], [sstut], [benchsh] |
| PlainTable | An mmap-oriented SST with no compression and no block cache; an in-memory hash on key prefixes, a sampled binary-search buffer every 16 rows; files under 2 GiB (31-bit offsets); no `Prev`, prefix seeks only | Lowest latency on memory-resident data | No published number | [plain] |
| Index block types | Binary search over per-block separator keys, a hash index on prefixes, or a two-level partitioned index | Locating the data block for a key | See partitioned index | [idx] |
| Data block hash index | A hash table inside each data block mapping keys to restart intervals, for point lookups (`kDataBlockBinaryAndHash`) | Binary search cache misses inside a block | 4.6% more space; `DataBlockIter::Seek` CPU −21.8%; overall throughput +10% on cached db_bench | [dbhash] |
| Partitioned index and filters | Each SST's index and filter split into partitions loaded on demand, behind a top-level index of 0.1–1% of their size | Large index and filter blocks (0.5 to 5 MB for a 256 MB SST) crowding the cache | 11× throughput on an emulated 100 TB HDD DB with a 60 MB cache (5 to 55 op/s); Linkbench at a 2 GB instead of 6 GB cache keeps 30k tps where unpartitioned drops to 23k | [part] |
| Bloom filter (full, cache-local) | One filter per SST, a key's probes within one cache line; format 5 uses a 64-bit hash and reaches below 0.1% false positives at 16 bits per key; 10 bits per key gives about 1% | Avoiding block reads for absent keys | At 10 bits per key, cache-local FP averages 0.95% against 0.84% for a standard Bloom filter | [bloom] |
| Ribbon filter | A linear-algebra static filter (Dillinger and Walzer), a drop-in Bloom replacement since 6.15 | Filter memory | About 30% less space than Bloom at equal FP (1% at about 7 bits per key), for about 3–4× the CPU on filters; construction takes about 231 bits per key of temporary memory against 74 for Bloom | [bloom], [ribbon] |
| `optimize_filters_for_memory` | Sizes each filter to the allocator's size class and spends the slack on a lower FP rate | Allocator internal fragmentation (about 10% for jemalloc) | No published number | [bloom] |
| Prefix extractor and prefix filters | A function from key to prefix; prefixes enter the filter and hash memtables; `Seek` with `prefix_same_as_start` or bounds checks the prefix filter | Filtering range seeks within a prefix, not only point gets | No published number | [prefix], [bloom] |
| Compression per level | A compression type per level, typically none or LZ4 on upper levels and zstd at the bottom (`bottommost_compression`) | Spend CPU where bytes stay longest | No published number | [comp] |
| Dictionary compression | A zstd dictionary trained per SST from sampled data blocks, stored in its own block and cached (digested `ZSTD_DDict`) | Poor ratio of small blocks compressed alone | Deployed for blocks of 8 KiB and smaller; benefit measured up to 16 KiB; one case saved CPU and space by shrinking blocks and enabling dictionaries; no percentage published | [dict] |

### Compaction and deletion

| Mechanism | What it is | What it solves | Published effect | Cite |
|---|---|---|---|---|
| Leveled compaction | L0 holds flushed files; each level from L1 is one sorted run partitioned by key range into SSTs; a level over its target merges a file with the overlapping range of the next level; dynamic level sizing (default since 8.4) sizes levels from the last level down | Bounded space amplification and read amplification | Dynamic levels keep space amplification near 1.111 at multiplier 10 (against 1.555 in the fixed example); overwrite write amplification 9.5 on the 900M-key benchmark; 6.1–6.8 on BlobDB's overwrite runs | [level], [dyn], [cidr17], [perf], [blob-blog] |
| Intra-L0 compaction | Merges L0 files together when L1 is busy | L0 read amplification | Adds about one unit of write amplification | [level] |
| Universal compaction | Tiered: sorted runs merged with adjacent runs by size ratio; a full compaction when estimated space amplification passes `max_size_amplification_percent`; periodic and age triggers | Write amplification | Lower write amplification (example estimate 9); a full compaction needs up to 2× disk temporarily; run counts vary, and compaction is bursty | [univ] |
| FIFO compaction | Drops the oldest files when the total passes a size, or by TTL | Time-series and caches with no updates | No published number | [fifo] |
| Subcompactions | A compaction split into key ranges run in parallel (anchors sampled per file since 7.6); `max_subcompactions` 1 by default | L0→L1 and manual compactions that one thread cannot drain | No published number | [subc] |
| Compaction filter | A user callback at compaction that keeps, drops, or changes a record | Expiry and garbage collection by rule, without explicit deletes | No published number | [cfilter] |
| TTL and periodic compaction | `ttl` pushes old files down; `periodic_compaction_seconds` (30 days effective default) rewrites old files so filters run | Old data never reaching the bottom or a filter | No published number | [level], [ttl] |
| Range deletion | `DeleteRange` writes one range tombstone; v2 fragments tombstones per file into non-overlapping, sorted pieces, binary searched on reads; compaction drops covered keys | Deleting a key range without reading it | Point gets with 10,000 tombstones over 5M keys: 1.63M ops/s against 1.67M for scan-and-delete (v1 was 0.76M); short scans 361k against 377k ops/s | [range-blog], [range] |
| Single delete | A tombstone that cancels exactly one put and disappears with it | Tombstone accumulation for keys written once | No published number | [single] |
| Merge operator | A record holding an operand; reads fold operands onto the base value; compaction folds them early (full and partial merge) | Read-modify-write without a read (counters, appends) | Benchmarks on the read-modify-write page | [merge], [rmw] |

### Read path

| Mechanism | What it is | What it solves | Published effect | Cite |
|---|---|---|---|---|
| LRU block cache | Uncompressed blocks in a sharded cache (at most 64 shards, 512 KiB minimum each), a mutex per shard, a high-priority pool for index, filter, and dictionary blocks; 32 MB by default (8 MB before 8.2); no longer the default cache from 10.7 | Decompression and I/O per read | No published number | [cache] |
| HyperClockCache | A lock-free cache with CLOCK eviction on a fixed-size table; the automatic variant (8.7) needs no entry-size estimate; the default block cache since 10.7 | LRU's per-shard mutex under parallel load | Up to 4.5× ops/s against LRUCache under high parallel load in db_bench | [history] |
| Secondary cache | A second tier behind the block cache for evicted blocks, on local flash or as a compressed in-memory cache | Cache capacity beyond RAM | +15% with a local flash cache; 25–30% fewer network reads | [seccache] |
| Pinning | Pin L0 index and filter blocks, the top level of partitioned ones, or all of them in cache | Index and filter misses on the read path | No published number | [cache] |
| Iterators and bounds | Merged iterators over memtables and every level; `iterate_upper_bound`, `prefix_same_as_start`, pinned data for zero-copy keys; tailing iterators | Bounding the work of scans | No published number | [iter] |
| MultiGet and async I/O | Batched gets that probe filters first and issue block reads in parallel within and across levels (coroutines); async readahead for scans | Serial I/O latency per key | MultiGet 1292 → 775 µs/op single-level, 508 µs/op multi-level; a seek scan 849k → 414k µs; 6–15% more CPU for MultiGet | [async] |
| Direct I/O and readahead | `use_direct_reads` and direct flush and compaction I/O; automatic readahead from the third sequential read; compaction readahead | Double caching with the page cache, and cache pollution by compaction | readrandom 136,915 → 189,236 ops/s and readwhilewriting 98,240 → 143,283 ops/s with direct I/O on the 900M-key, about 3× RAM benchmark | [dio], [perf] |

### Features

| Mechanism | What it is | What it solves | Published effect | Cite |
|---|---|---|---|---|
| Column families | Key spaces in one DB with their own memtables, SSTs, and options, sharing one WAL; writes across them in one batch are atomic | Per-data-type tuning with atomic cross-family writes | No published number | [cfam] |
| Snapshots | A sequence number held by the reader; compaction keeps versions a snapshot needs | Consistent reads across keys | No published number | [snap] |
| Optimistic transactions | Writes buffered; commit checks that no read or written key changed since the transaction's snapshot | Conflict detection without locks | No published number | [txn] |
| Pessimistic transactions | Point locks taken at write, deadlock detection, two-phase commit; WriteCommitted, WritePrepared (data at prepare, a commit cache of 8M entries), WriteUnprepared | Strict isolation under contention; large transactions | WritePrepared on sysbench: improvements from 68% (insert) down to −1.2% to −1.8% (read-only); the page's baseline is not stated | [txn], [wp] |
| Checkpoints | A consistent openable copy by hard-linking SSTs and copying the manifest and live WAL | Cheap local snapshots of the whole DB | No published number | [ckpt] |
| Backups | `BackupEngine`: incremental backups that share immutable SSTs between backups | Off-node copies | No published number | [backup] |
| SST ingestion | `SstFileWriter` builds sorted files outside the DB; ingestion assigns a sequence number and places each file at the lowest level it does not overlap; `atomic_replace_range` replaces a range | Bulk loads without the write path | No published number | [ingest], [history] |
| BlobDB | Values at or above `min_blob_size` go to blob files at flush; the LSM keeps references; garbage collection rides on compaction (`blob_garbage_collection_age_cutoff` 0.25) | Rewriting large values at every compaction | Overwrite write amplification 1.4–1.7 against 6.1–6.8 (leveled) and 5.7–6.2 (universal); overwrite throughput 2.1–3.5× leveled; initial load 2.3–4.7× faster; values from 1 KB to 1 MB | [blob], [blob-blog] |
| User-defined timestamps | An application timestamp between the user key and the sequence number; reads at a timestamp; `full_history_ts_low` lets compaction drop older versions | Multi-version reads keyed by the application's clock (replication, MVCC above RocksDB) | No published number | [udt] |
| Statistics and perf context | DB-wide tickers and histograms (`Options::statistics`); per-thread, per-operation counters of time and work (`PerfContext`, `IOStatsContext`) | Attributing latency and work | No published number | [stats], [perfctx] |

## 2. Mapping to strata

"Exists", "partial", "absent", and "n/a" describe `main` today. "Where" places the work
per `layers.md`: the core (lifecycle) or a plugin (`keys`, `stored`); n/a when it has
no home.

### Write path

| Mechanism | strata today | Where | strata-native equivalent | Gap |
|---|---|---|---|---|
| Memtable (sorted) | Partial | Core | The write buffer plus the version map (`index.hh:1593`, an unordered hash map from key to docid and version) | Gets by key are served (real-time get, D13); ordered scans over buffered keys have no sorted view; the map's per-entry bytes are unmeasured against the skiplist's (M1) |
| Alternative memtables | n/a | — | One buffer | None: one implementation (X10) |
| Flush triggers, write buffer manager | Exists | Core | `byte_budget` (D17): one node budget, a buffer share per index, flush at the share or under pressure, the version map flushes past 25% of the share | The frame cache (K6) must charge the same budget, as RocksDB charges memtables to the block cache |
| WAL | Partial | Core | The turn log (`RFTW0001`, D11) built in `journal`; standalone use is `workload-profiles.md` item 8, not yet wired; today durability is `commit()` only | No per-batch durability short of a commit (two directory barriers plus segment syncs) (K2) |
| WAL format and recycling | Exists (journal) | Core | D11: 32 KiB blocks, fragments with segment number and epoch, recycled preallocated segments, `sync_data` | None |
| WAL recovery modes | Exists, one mode | Core | D11's torn-tail rule: the last turn kept only if whole; damage elsewhere costs only the groups in the block | None: one recovery rule, not a preset |
| Group commit | Exists (journal) | Core | One barrier per core per turn for every group on the core (D11) | Not yet on the standalone path (K2) |
| Pipelined write | Partial | Core | Stage 1 on the owner core feeds stage 2 inversion threads (D18); the log write and apply ordering for standalone use is undecided | K2 decides it: apply overlaps the barrier, acknowledgement waits for it |
| Concurrent memtable writes | Absent by design | Core | Stage 1 is sequential per shard; parallelism is K inversion buffers and shards per core | For the `document` profile stage 2 is nearly free, so stage 1's rate per core is the bound (M2, Q4) |
| Unordered write | n/a | — | — | Rejected (X5) |
| Write stalls | Partial | Core | Stage 1 suspends at the budget's limit; storage admission rejects with `no_space` | No backpressure from merge debt: merges run synchronously inside `refresh()` (`index.hh:294–297`), so the writer pays for them inline (K4) |
| Rate limiter | Absent | Core | Phase 3 says merges are "rate-limited"; nothing in `include/` limits them | K4 |

### Storage

| Mechanism | strata today | Where | strata-native equivalent | Gap |
|---|---|---|---|---|
| Block-based table | Exists | Core and plugins | The segment file: header, sections, table of contents, footer (format 12; format 13's pinned tail, S1) | Records are in docid order, not key order (K5) |
| PlainTable | n/a | — | mmap is strata's default read path already | Rejected (X10) |
| Index block, data block hash index | Exists (format 13 design) | `keys` plugin | The key dictionary: FST-indexed key blocks over `termdict`'s block code (phase-4 D12) | Format 12 resolves keys through the term dictionary; format 13 lands `keys` (K3) |
| Partitioned index and filters | n/a today | `keys` plugin | Per-segment filters and FST term index, pinned per open segment | Pinned filter memory grows with keys (about 9 bits per key); measured at 10× RAM before any partitioning (M5) |
| Bloom and Ribbon filters | Exists | `keys` plugin | `pk_filter`: a binary fuse filter, `filter8`, about 9 bits per key at 0.39% FP over xxHash64 [fuse] | None by mechanism; fixed FP per bits (8-bit fingerprints only); compared, not adopted (M6) |
| Prefix extractor and prefix filters | Absent | `keys` plugin | The segment's key range check before the filter; FST range streams | No filter for range seeks; measured with `seekrandom` before adding (M7) |
| Compression | Exists | `stored` plugin | zstd frames of up to 64 KiB of records (`format.hh:66`), copied raw at merge when no record is deleted (I7) | One frame size and one zstd level everywhere; flush and merge could take different levels (M3) |
| Dictionary compression | Absent | `stored` plugin | — | Small frames lose ratio without one; tied to the frame size (M3) |

### Compaction and deletion

| Mechanism | strata today | Where | strata-native equivalent | Gap |
|---|---|---|---|---|
| Leveled compaction | n/a | Core | Tiered merges (`merge_policy.hh`, Lucene 10.4's defaults) | Space amplification under overwrite is unmeasured against leveled's 1.11 (M4); a second policy is rejected (X1) |
| Universal compaction | Exists (equivalent) | Core | Tiered merges; `deletes_pct_allowed` (20%) plays `max_size_amplification_percent`'s role; merged segments up to 5 GiB | Defaults are Lucene's, not measured on key-value data (`limits.md`, "merge policy presets") |
| FIFO compaction | Exists (equivalent) | Core | A segment whose documents are all deleted leaves at the next commit without a merge; the `log` profile is time-sorted | Expiry needs an applied delete (X3) |
| Subcompactions | Partial | Core | D18 splits a flush into docid runs on threads; merges run on one thread | Parallel merge by output docid range (M8) |
| Compaction filter, compaction-time TTL | n/a | — | Delete by query (phase-4 D16) at an applied position | Rejected (X2, X3) |
| Range deletion | Partial (format 13) | Core and `keys` | Delete by query over a key range: the `keys` scan sets live-docs bits | Cost linear in the range's live keys, against one tombstone (Q6) |
| Single delete | n/a | — | Deleting a flushed document sets one live-docs bit; a buffered one is dropped | None: strata has no tombstones to accumulate |
| Merge operator | Absent (designed) | Core | Field-update generations with delta records, folded at read and at merge (`workload-profiles.md` items 4–5) | K7 |

### Read path

| Mechanism | strata today | Where | strata-native equivalent | Gap |
|---|---|---|---|---|
| Block cache (LRU, HyperClockCache) | Absent | Core (`residency`) | mmap and the page cache; a get decompresses a whole zstd frame | Every cached point get pays a frame decompression (K6, M3) |
| Secondary cache | n/a | — | The verified block cache, `residency`'s second backend (ARCHITECTURE, I/O) | Rejected as a tier (X8) |
| Pinning | Exists | Core | Pinned sections verified whole at open (`docids`, `versions`, the key filter, the term index) | None |
| Iterators and bounds | Partial (format 13) | Core and `keys` | `key_reader::scan(key, …)` merged across segments, deleted copies skipped (phase-4 D12) | No upper bound or prefix pushed down to skip segments by key range; no buffered keys in order (K3) |
| MultiGet and async I/O | Partial | Core (`residency`) | Query start requests the plan's ranges (`madvise(WILLNEED)`, io_uring, POSIX AIO) | No batched get API; measured with `multireadrandom` (M9) |
| Direct I/O and readahead | Partial | Core (`residency`) | The verified block cache backend reads with `pread`; merges read through mmap | Merges pollute the page cache; no direct I/O path measured (M9) |

### Features

| Mechanism | strata today | Where | strata-native equivalent | Gap |
|---|---|---|---|---|
| Column families | n/a | — | One index per shard; many indexes per node share the budget and, under D11, one turn log per core | Cross-index atomic batches are out of scope (`workload-profiles.md` item 7) |
| Snapshots | Exists | Core | Per-core snapshot handles over segment sets and live-docs generations (Phase 3, "Retention") | None |
| Optimistic transactions | Exists (equivalent) | Core | Atomic transactions with expected versions checked in stage 1 (D9, D10) | Read sets are the host's: a transaction states the versions it read |
| Pessimistic transactions | n/a | — | One deterministic writer per shard in log order | Rejected (X6) |
| Checkpoints | Exists | Core | `checkpoint()` pins a manifest; `install()` copies it (D4, D5) | None |
| Backups | Exists (equivalent) | Core | Pinned checkpoints of immutable files, transferred by `install` | Incremental transfer of shared files is Track D's |
| SST ingestion | Absent | Core | — | Q7 |
| BlobDB | Partial | `stored` plugin | Stored records are already outside the key structures; merges copy unchanged frames raw (I7) | Merges still rewrite the bytes of every surviving value (M4, Q8) |
| User-defined timestamps | n/a | — | Versions per key and pinned checkpoints | Rejected (X7) |
| Statistics and perf context | Partial | Core | `benchkit`, the allocation-counting build, cgroup sampling | No per-operation counters (K1) |

## 3. Recommendation

The rules applied: one implementation per mechanism, so an adopted mechanism replaces
strata's, never sits beside it; compatibility is a preset; the fastest measured preset
is the default; every limit names its mechanism (`limits.md`). Each item states the
workload it serves and the axis it moves.

### Into the core, in order

| # | Item | Serves | Axis | Basis |
|---|---|---|---|---|
| K1 | **The key-value harness and per-operation counters.** The RocksDB engine and workloads of section 4; per-operation counters in the index (segments probed, filter negatives and positives, key blocks read, frames decompressed, bytes decompressed, stage-1 time, merge bytes in and out), compiled in and read by the engine | Every row below | None directly: it makes every later decision measurable | RocksDB's `PerfContext` [perfctx]; BENCHMARKS' rule that each claim is measured |
| K2 | **Standalone durability through the turn log** (`workload-profiles.md` item 8), with a pipelined acknowledgement: apply proceeds while the turn's barrier is in flight, and a transaction is acknowledged when its turn's barrier returns | Every durable write row: `fillrandom`, `overwrite`, YCSB A, F with sync per batch | Indexing (write throughput at equal durability) | Group commit and pipelined write [pipe]; D11 already gives one barrier per core per turn. Without it, strata's only power-loss-safe acknowledgement is a full commit |
| K3 | **Format 13's `keys` section with bounded, ordered scans,** as phase-4 D12 designs it, plus: scans take a lower and upper bound and skip segments whose key range misses them; buffered keys enter a scan through a sorted view built once per snapshot | `readseq`, `seekrandom`, YCSB E; the `graph` profile's prefix scans | Query speed (scans) | RocksDB iterator bounds [iter]; workload-profiles item 1 |
| K4 | **Background merges with a rate limit and merge-debt backpressure.** Merges leave `refresh()` for a background executor; their writes draw on a rate limit; the budget's pressure signal gains merge debt (bytes the policy wants merged), so stage 1 slows when debt passes a bound instead of paying merges inline | `fillrandom`, `overwrite` past RAM; `readwhilewriting` p99 | Indexing (sustained), query p99 | Write stalls and the rate limiter [stall], [rl], [autorl]; Phase 3 already says merges run in the background, rate-limited |
| K5 | **Key-ordered merges for keyed layouts.** A merge in a layout with `keys` writes its output permuted by key (the permuted docids kind, P1), so stored frames follow key order and a key scan reads frames sequentially; it is index sorting (phase-4 D14) with the primary key as the sort key, a preset, not a mechanism | `readseq`, `seekrandom`, YCSB E after merges; `fillrandom`'s scans | Query speed (scans), disk (adjacent keys compress together) | RocksDB's SSTs are key-sorted [bbt]; strata's records are in docid order, so with random keys a scan reads one frame per key |
| K6 | **The point-get path for stored records,** in order: the frame size as a measured preset per profile (M3); then zstd dictionaries per segment if small frames win on time but lose on disk; then the decompressed-frame cache of workload-profiles item 8, built as a CLOCK cache with lock-free lookups and charged to `byte_budget` | `readrandom`, YCSB B, C, D | Query speed (gets), memory | A get today decompresses up to 64 KiB; RocksDB caches uncompressed 4–8 KiB blocks, HyperClockCache's 4.5× over LRU under contention [history], dictionaries at ≤ 8 KiB blocks [dict] |
| K7 | **Counters and merge operands** as field-update generations (`workload-profiles.md` items 4–5), already scheduled for Track P | `mergerandom`, YCSB F's read-modify-write | Indexing, disk | Merge operator [merge]; folding at read and merge is the same idea in strata's generations |
| K8 | **Key-range delete through delete by query** over the `keys` section, with its cost per deleted key reported | `deleterandom`, range deletes in the `graph` and `log` profiles | Indexing | Range deletion [range-blog]; strata's deletes are bits, not tombstones, so no read path pays for them later |

K1 comes first because each later item is decided by a row it produces. K2 comes before
any durable write row is recorded. K3 and K5 are format 13 work and a merge preset; K4
is Phase 3 design debt that the key-value rows expose first, since search indexing
amortizes merges over larger documents. K6's three steps land one at a time, each kept
only if it wins (PLAN Goal 2).

### Rejected

| # | Mechanism | Reason |
|---|---|---|
| X1 | Leveled compaction as a second policy | One implementation per mechanism. Leveled needs key-range-partitioned files; strata's segments are docid-ordered and carry postings and columns. If tiered merging loses the disk axis on overwrite (M4), the answer is a tiered preset (a lower `deletes_pct_allowed`, a smaller maximum merged size), measured |
| X2 | Compaction filters | A merge's schedule differs between replicas, so a filter that drops records at merge makes content depend on merge timing; the digest (D12) and T2 would diverge. Rule-based deletion is an applied operation (delete by query, D16) |
| X3 | Compaction-time TTL and periodic compaction for expiry | Same reason as X2: expiry is a delete at a log position; whole-segment drops then reclaim time-sorted segments for free |
| X4 | Range tombstones | strata deletes by live-docs bits; a tombstone would be a second deletion mechanism that every read checks. K8 covers key ranges; Q6 asks whether a range too large to enumerate is a requirement |
| X5 | Unordered write | Gives up atomic reads and immutable snapshots [unordered]; strata's apply is deterministic and in order |
| X6 | Pessimistic transactions and locks | One writer per shard applies in log order; conditional versions already detect conflicts |
| X7 | User-defined timestamps | History beyond the newest version is not retained; time travel is a pinned checkpoint. Versions per key already carry the host's ordering |
| X8 | Secondary cache | strata's second tier is the verified block cache backend; a flash or NVM cache tier is outside the scale contract's node model |
| X9 | Column families | An index is the unit; atomic writes across indexes are out of scope (`workload-profiles.md` item 7) |
| X10 | PlainTable and the hash, linklist, and vector memtables | Alternatives to one mechanism kept as options; strata reads through mmap already and keeps one buffer |
| X11 | Ribbon or Bloom as a replacement for the binary fuse filter, without M6 | One filter; it changes only if M6 shows a win at equal false-positive rate |
| X12 | A RocksDB, LevelDB, or LMDB backend under strata | Already rejected (`workload-profiles.md`, verdict 2); this survey does not reopen it |

### Measure first

| # | Question | Rows | Decides |
|---|---|---|---|
| M1 | Bytes per buffered key-value: the version map (`unordered_map<std::string, key_entry>` plus `docid_keys_`) and the buffered document, against RocksDB's skiplist entry | `fillrandom` at 16-byte keys and 100-byte values: segments per GB written, flushes per second | Whether the version map needs a denser layout (an arena-backed open-addressing table) |
| M2 | Stage 1's rate per core on key-value operations, and how it scales with shards per core | `fillrandom`, `overwrite` at 1 thread; N threads against N shards | Q4 |
| M3 | Frame size (4, 8, 16, 32, 64 KiB) and zstd level per flush and per merge, with and without a dictionary | `readrandom` at "fits" and "2×"; disk bytes; merge time | K6's preset, and whether dictionaries land |
| M4 | Space and write amplification of tiered merging against leveled and universal on overwrite | `overwrite`, YCSB A at 10× RAM; values 100 B to 64 KiB | X1's preset; whether value separation (Q8) is needed |
| M5 | Pinned bytes per key (filter, versions, docids, term or key index) at 10× RAM | Serving memory with the index open | Whether filters or indexes need partitioning or paging |
| M6 | Binary fuse `filter8` against Ribbon and cache-local Bloom at equal false-positive rate: bits per key, probe time, build time and temporary memory | A microbenchmark on the `fusefilter` library with reference filters, benchmark-only, as `bitpack` does with SIMD-BP128 | X11 |
| M7 | Seek cost with many segments and no prefix filter | `seekrandom` before and after merges | Whether a prefix filter earns its bytes |
| M8 | Merge throughput on one thread against RocksDB's subcompactions | A forced merge of the 10× RAM dataset | Parallel merge by output docid range |
| M9 | mmap against the verified block cache with `pread`, and direct I/O for merges | `readrandom` and `readwhilewriting` at 2× and 10× RAM; `multireadrandom` | The read-path default for the `document` profile, and a batched get API |

## 4. Benchmark plan to parity

### How RocksDB enters the harness

- **Engine.** `bench/engines/rocksdb-<version>/`: a driver of ours (MIT) linking a
  pinned RocksDB release built from its tag, as `lucene-10.4.0-st` and
  `tantivy-0.26-st` are. The version is the newest release at least two weeks old at the
  gate session (11.12.0 was released on 2026-10-07 [history]); the report records it.
  RocksDB is never linked into strata.
- **Build.** Release, the same compiler, `-O2`/`-O3`, and `-march` as strata's engine;
  zstd and LZ4 only; liburing on Linux, since its fastest supported configuration uses
  it for MultiGet and async reads [async]. The allocator is Q5.
- **Configuration.** Its fastest supported configuration per row, chosen by a sweep
  recorded in the report: leveled with dynamic level sizes, universal, and BlobDB for the
  large-value rows; HyperClockCache; Bloom at 10 bits per key and Ribbon; block size 4,
  8, and 16 KiB; buffered and direct I/O. The fastest becomes RocksDB's reference for
  that row, as Lucene's with and without reordering does.
- **Driver and generator.** Both engines run the same operation stream from one seeded
  generator header in `bench/kv/` (keys, values, distributions, YCSB's zipfian and
  latest), compiled into both drivers, so the stream is identical and neither pays for
  the other's RNG. Each driver records per-operation latency in an HDR histogram.
- **Sanity check.** RocksDB's own `db_bench` runs the db_bench-shaped rows once per
  session; the driver must be within the noise band of it, so the harness does not slow
  the baseline.
- **strata's side.** A `document_layout` engine (`L12`) once format 13 lands; before it,
  `search_layout` with no text, keys as the key term, and the value as the stored
  record, reported as such.

### Workloads

db_bench's defaults are 16-byte keys and 100-byte values at compression ratio 0.5;
`benchmark.sh` uses 20-byte keys, 400-byte values, and zstd [dbbench], [benchsh].

| Row | Operations | Notes |
|---|---|---|
| `fillseq` | Puts in key order | Single and N threads |
| `fillrandom` | Puts in random key order | The load for read rows |
| `overwrite` | Random puts over existing keys | Write and space amplification |
| `readrandom` | Random gets of existing keys | Also 10% absent keys (filter row) |
| `multireadrandom` | Batched gets | Report row until a batched get exists (M9) |
| `readseq` | A full scan in key order | Before and after merges |
| `seekrandom` | A seek then 10 `Next` | Bounded scans (K3) |
| `readwhilewriting` | N readers, one writer rate-limited | p99 of readers |
| `mergerandom` | Merge operands on random keys | Once K7 lands; RocksDB with the uint64 add operator |
| `deleterandom` | Random deletes of existing keys | Then `readrandom` over the result |
| YCSB A–F | A 50/50 read/update; B 95/5; C read-only; D read latest with 5% inserts; E short scans with 5% inserts; F read-modify-write | Zipfian (D: latest), 1 KB records as YCSB's default and 100-byte values [ycsb] |

Value sizes: 100 B (db_bench), 400 B (`benchmark.sh`), 4 KiB, and 64 KiB for the
large-value rows that BlobDB targets.

### Dataset size relative to memory

"RAM" is the cgroup's `memory.max`, set equal for both engines, so the three regimes
fit the 16 GB gate VM: with `memory.max` at 2 GiB, "fits" is 1 GiB of logical data
(key plus value bytes), "2×" is 4 GiB, and "10×" is 20 GiB. The 10× rows on bare metal
use the machine's RAM. Each row states the key count and logical bytes.

### Equal durability

| Setting | RocksDB | strata |
|---|---|---|
| None | `disableWAL`, flush at the end | No log, one `commit()` at the end |
| Group commit | `sync=1`, N concurrent writers | Turn log, one barrier per turn (K2) |
| Sync per batch | `sync=1`, one writer, batch of 1 and of 100 | Turn log with one transaction per turn |

RocksDB's default (WAL written, not synced) survives a process crash but not power
loss; strata has no such setting, so it is a report row for RocksDB only (Q3). Durable
rows wait for K2. On macOS every engine uses `F_FULLFSYNC` (BENCHMARKS rule 4), which
the RocksDB driver must be checked to issue (Q9).

### Equal memory

BENCHMARKS' equal-memory rule, adapted: RocksDB's whole budget is one block cache with
`cache_index_and_filter_blocks` and the write buffer manager charging memtables to it
[wbm], so one number bounds its memtables, data blocks, and index and filter blocks.
strata's `byte_budget` is calibrated (`bench/calibrate_rss.sh`) so its peak RSS equals
RocksDB's at RocksDB's fastest cache setting, and both run under the same cgroup
`memory.max`, which charges the page cache alike. The report states each engine's
settings, peak RSS, page cache share, and file count.

### The four axes, and CPU

| Axis | Measured as |
|---|---|
| Speed | Operations per second; p50, p99, p99.9 per operation from the driver's histogram; five interleaved trials, median and 95% bootstrap interval of the ratio (BENCHMARKS rule 3) |
| Indexing | Load throughput to a durable state at each durability setting; `fillseq` and `fillrandom` rows |
| Memory | cgroup `memory.current` peak and steady state, `memory.stat` anon and file |
| Disk | Final bytes (space amplification over logical bytes); bytes written from `/proc/diskstats` over logical bytes (write amplification, as `bench/write_amp.sh`), cross-checked with each engine's own compaction or merge counters |
| CPU | `cpu.stat` user and system microseconds per operation, a report row |

### Platforms

Linux x86_64 and FreeBSD in the gate VMs every session; macOS on the host as a report
row; Linux and macOS arm64 on the rented bare-metal machines at the gates that book
them (BENCHMARKS rule 4). Rows the VM cannot represent (fsync latency, 10× RAM on real
hardware) run there.

### Ratchet

The `document` profile's rows take PLAN's ratchet against RocksDB's reference per row:
≤ 1.0× time and ≥ 1.0× throughput at the gate that adopts them, then Phase 6's margins.
Which gate adopts them is Q10.

## 5. Open questions for the architect

| # | Question |
|---|---|
| Q1 | **Blind writes.** Every keyed write resolves its key in stage 1, because versions increment and one live copy per key is an invariant. RocksDB writes blind. If M2 shows the resolution dominating `fillrandom` and `overwrite`, is a version derived from the log position acceptable for unconditional upserts, so that old copies are found in one sorted pass at flush instead of one probe per write? It changes the digest's `D` record and Xapiand's version semantics. |
| Q2 | **Key-ordered merges (K5).** Is permuting a keyed layout's merge output by key acceptable as the `document` profile's default, given that the docid order then serves only get-by-docid through the permuted kind's index? |
| Q3 | **Durability tiers.** Is a process-crash-only setting (log written, no barrier) wanted for strata, or are RocksDB's rows at that setting report-only? |
| Q4 | **Thread scaling.** RocksDB's N-thread rows use one DB; strata's natural unit is a shard per core. Is the comparison N threads into one strata index, N shards on one node, or both? |
| Q5 | **Allocator.** RocksDB's fastest configuration usually links jemalloc. Does each engine run with its best allocator, or both with the system allocator? |
| Q6 | **Range deletes too large to enumerate.** Is deleting a key range in time independent of its size a requirement for any profile? If not, X4 stands. |
| Q7 | **Bulk ingestion.** Should an operation that adopts an externally built segment exist, carried in the log like `install` so replicas agree, for `fillseq`-style loads? |
| Q8 | **Value separation.** If M4 shows merges rewriting large values losing the disk-write axis, does value separation land as the `stored` plugin's layout above a size threshold, replacing in-segment frames for those records? |
| Q9 | **macOS barriers.** If RocksDB does not issue `F_FULLFSYNC` on macOS, are its durable macOS rows dropped, or is the driver patched to sync with it? |
| Q10 | **Which gate.** Do the key-value rows join Phase 4's gate with format 13, or a separate gate for the `document` profile once K1–K3 land? |

## Owner decisions (2026-10-15)

After the architect's review (`docs/reviews/rocksdb-survey.md`):

- **K4: background merges are implemented in Phase 3**, with a per-node rate limit and
  the lifecycle review's condition 7 tests, before Phase 3's ratchet rows are recorded.
- **Q1: blind upserts are an option.** An index preset in which an unconditional upsert
  skips the key lookup and takes its version from its log position, for key-value
  workloads. It is measured against key resolution made cheaper (M2, the create-only
  flag, sorted batch probes); in that mode `_version` follows the log, not a per-key
  count, and the documentation says so. The architect recommended against it; the owner
  chose it as an option.
- **Q10: the key-value rows gate in their own `document`-profile gate after Phase 4's**,
  with a key-value ratchet family in PLAN.
- **Q5: strata may ship jemalloc** if the allocator rows show it fastest; both
  allocators are measured and stated.
- Q3, Q6, Q7 follow the review's recommendations: no new durability tier (`ack=none`
  on the standalone turn log), no size-independent range deletes now, bulk ingestion
  designed after format 13.

## References

| Tag | Source |
|---|---|
| [async] | Asynchronous I/O in RocksDB, https://rocksdb.org/blog/2022/10/07/asynchronous-io-in-rocksdb.html |
| [autorl] | Auto-tuned rate limiter, https://rocksdb.org/blog/2017/12/18/17-auto-tuned-rate-limiter.html |
| [backup] | https://github.com/facebook/rocksdb/wiki/How-to-backup-RocksDB |
| [bbt] | https://github.com/facebook/rocksdb/wiki/Rocksdb-BlockBasedTable-Format |
| [benchsh] | `tools/benchmark.sh` defaults, https://github.com/facebook/rocksdb/blob/main/tools/benchmark.sh |
| [blob] | https://github.com/facebook/rocksdb/wiki/BlobDB |
| [blob-blog] | Integrated BlobDB, https://rocksdb.org/blog/2021/05/26/integrated-blob-db.html |
| [bloom] | https://github.com/facebook/rocksdb/wiki/RocksDB-Bloom-Filter |
| [cache] | https://github.com/facebook/rocksdb/wiki/Block-Cache |
| [cfam] | https://github.com/facebook/rocksdb/wiki/Column-Families |
| [cfilter] | https://github.com/facebook/rocksdb/wiki/Compaction-Filter |
| [cidr17] | Dong et al., Optimizing Space Amplification in RocksDB, CIDR 2017, https://cidrdb.org/cidr2017/papers/p82-dong-cidr17.pdf |
| [ckpt] | https://github.com/facebook/rocksdb/wiki/Checkpoints |
| [comp] | https://github.com/facebook/rocksdb/wiki/Compression |
| [dbbench] | `db_bench` flag defaults, https://github.com/facebook/rocksdb/blob/main/tools/db_bench_tool.cc |
| [dbhash] | https://github.com/facebook/rocksdb/wiki/Data-Block-Hash-Index |
| [dict] | Preset dictionary compression, https://rocksdb.org/blog/2021/05/31/dictionary-compression.html |
| [dio] | https://github.com/facebook/rocksdb/wiki/Direct-IO |
| [dyn] | Dynamic level size, https://rocksdb.org/blog/2015/07/23/dynamic-level.html |
| [fast21] | Dong et al., RocksDB: Evolution of Development Priorities in a Key-value Store Serving Large-scale Applications, FAST 2021, https://www.usenix.org/conference/fast21/presentation/dong |
| [fifo] | https://github.com/facebook/rocksdb/wiki/FIFO-compaction-style |
| [fuse] | Graf and Lemire, Binary Fuse Filters: Fast and Smaller Than Xor Filters, https://arxiv.org/abs/2201.01174 |
| [history] | RocksDB release notes: HyperClockCache in 7.7.0 and 7.9.0, the automatic variant in 8.7.0, the default block cache in 10.7.0, 11.12.0's date; https://github.com/facebook/rocksdb/blob/main/HISTORY.md |
| [idx] | https://github.com/facebook/rocksdb/wiki/Index-Block-Format |
| [ingest] | https://github.com/facebook/rocksdb/wiki/Creating-and-Ingesting-SST-files |
| [iter] | https://github.com/facebook/rocksdb/wiki/Iterator |
| [level] | https://github.com/facebook/rocksdb/wiki/Leveled-Compaction |
| [memtable] | https://github.com/facebook/rocksdb/wiki/MemTable |
| [merge] | https://github.com/facebook/rocksdb/wiki/Merge-Operator |
| [part] | https://github.com/facebook/rocksdb/wiki/Partitioned-Index-Filters |
| [perf] | https://github.com/facebook/rocksdb/wiki/Performance-Benchmarks (7.2.2 rows: m5d.2xlarge, 32 GB, 900M keys, about 97 GiB after load) |
| [perfctx] | https://github.com/facebook/rocksdb/wiki/Perf-Context-and-IO-Stats-Context |
| [pipe] | https://github.com/facebook/rocksdb/wiki/Pipelined-Write |
| [plain] | https://github.com/facebook/rocksdb/wiki/PlainTable-Format |
| [prefix] | https://github.com/facebook/rocksdb/wiki/Prefix-Seek |
| [range] | https://github.com/facebook/rocksdb/wiki/DeleteRange |
| [range-blog] | DeleteRange: a new native RocksDB operation, https://rocksdb.org/blog/2018/11/21/delete-range.html |
| [ribbon] | Dillinger and Walzer, Ribbon filter: practically smaller than Bloom and Xor, https://arxiv.org/abs/2103.02515 |
| [rl] | https://github.com/facebook/rocksdb/wiki/Rate-Limiter |
| [rmw] | https://github.com/facebook/rocksdb/wiki/Read-Modify-Write-Benchmarks |
| [seccache] | RocksDB secondary cache, https://rocksdb.org/blog/2021/05/27/rocksdb-secondary-cache.html |
| [single] | https://github.com/facebook/rocksdb/wiki/Single-Delete |
| [snap] | https://github.com/facebook/rocksdb/wiki/Snapshot |
| [sstut] | https://github.com/facebook/rocksdb/wiki/A-Tutorial-of-RocksDB-SST-formats |
| [stall] | https://github.com/facebook/rocksdb/wiki/Write-Stalls |
| [stats] | https://github.com/facebook/rocksdb/wiki/Statistics |
| [subc] | https://github.com/facebook/rocksdb/wiki/Subcompaction |
| [ttl] | https://github.com/facebook/rocksdb/wiki/Time-to-Live |
| [txn] | https://github.com/facebook/rocksdb/wiki/Transactions |
| [udt] | https://github.com/facebook/rocksdb/wiki/User-defined-Timestamp |
| [univ] | https://github.com/facebook/rocksdb/wiki/Universal-Compaction |
| [unordered] | Higher write throughput with `unordered_write`, https://rocksdb.org/blog/2019/08/15/unordered-write.html |
| [wal] | https://github.com/facebook/rocksdb/wiki/Write-Ahead-Log-%28WAL%29 |
| [walfmt] | https://github.com/facebook/rocksdb/wiki/Write-Ahead-Log-File-Format |
| [walrec] | https://github.com/facebook/rocksdb/wiki/WAL-Recovery-Modes |
| [wbm] | https://github.com/facebook/rocksdb/wiki/Write-Buffer-Manager |
| [wp] | https://github.com/facebook/rocksdb/wiki/WritePrepared-Transactions |
| [ycsb] | YCSB core workloads, https://github.com/brianfrankcooper/YCSB/wiki/Core-Workloads |
