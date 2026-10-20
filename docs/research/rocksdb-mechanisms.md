# RocksDB's core mechanisms: a clean-room specification

Status: research note, 2026-10-20. Companion to `docs/perf/rocksdb-fairness-audit.md`, whose
counted runs supply the measured costs quoted here, and an extension of
`docs/design/rocksdb-survey.md`, which catalogs RocksDB's mechanisms and maps them to strata.
This note goes one level down: for each core mechanism, what it does, the invariant it keeps,
what it costs, and which of strata's measured losses it explains.

**Clean room.** RocksDB is dual-licensed GPLv2 and Apache 2.0; it was read under Apache 2.0
(release branch `11.11.fb` at `ec93377878cd`, the build the KV harness links). This note is
prose and diagrams written from that reading, the wiki, and our own measurements. It contains
no RocksDB code, no transcription of its code, and no identifiers beyond the names of public
options and statistics. Defaults are those of that commit.

**The losses** (`docs/perf/core-vs-rocksdb.md`, one thread unless stated):

| Tag | Loss | Size |
|---|---|---|
| **G** | Point gets: `readrandom`, YCSB B and C | 32× slower at fits (80 µs of CPU per get against 2.5 µs); 12× at 2×; 2.95× with 1,000 small indexes |
| **S** | Scans: `readseq`, `seekrandom`, YCSB E | 220×, 96×, and 154× slower |
| **D** | Durable writes: sync per batch of 100 and of 1; "to durable" fills | 4.7–5.2× slower; fills 1.3–1.8× slower to durable at fits |
| **M** | Memory per buffered record (16-byte key, 100-byte value) | 683 B against 153 B: 4.5× |

Each mechanism below ends with **Invariant**, **Cost**, and **Explains** (a loss tag, or
"none" when strata already wins or the mechanism is not on the measured path).

## 0. The shape

```
 writers ──▶ write group ──▶ WAL (one log, all column families)
                 │
                 ▼
          mutable memtable ──switch──▶ immutable memtables ──flush──▶ L0 SST files
          (skiplist, arena)                                              │ compaction
                                                                          ▼
 readers ──▶ SuperVersion { memtable, immutables, Version{L0, L1 … Ln} }   Lbase … Ln
                                      MANIFEST: the log of Version edits; CURRENT names it
```

Everything mutable is in memory; everything on disk is immutable. A record is an *internal
key* (the user key, then a 56-bit sequence number and an 8-bit type packed in 8 bytes) and a
value. Internal keys sort by user key ascending, then sequence descending, so the newest
version of a key is the first one a search for it meets. Writes are blind: a put never reads
the old value; older versions are shadowed until compaction drops them.

## 1. Write path

### 1.1 Write batches and sequence numbers

A write is a batch: one or more puts, deletes, and merges, applied atomically. The batch is
assigned a contiguous range of sequence numbers, one per record, at the moment its group is
written (1.2); a reader's view is a sequence number, so a batch is visible all at once or not
at all.

- **Invariant:** every record has a unique sequence number; a batch's records become visible
  together, in the order of their sequence numbers.
- **Cost:** the batch is serialized once (a byte string with a count and a header); sequence
  assignment is a counter under the write queue.
- **Explains:** none.

### 1.2 The write queue and group commit

Writers enqueue themselves on a lock-free list. The writer at the head becomes the **leader**;
those behind it that are compatible join its **group**, up to a byte limit
(`max_write_batch_group_size_bytes`, 1 MiB; only 128 KiB beyond the leader's own batch when that
batch is at most 128 KiB, so a small write is not held behind a large group). Compatibility is strict: a writer asking for a sync never joins a
leader that does not sync; writes with and without the WAL never mix; nor do writes with
different slowdown or rate-limit settings.

```
 W1 (leader) ─┐   one WAL record for the group  ──▶ one write() ──▶ one barrier if any sync
 W2 ──────────┼─▶ [hdr | W1 | W2 | W3]                │
 W3 ──────────┘                                       ▼
                    memtable inserts: by the leader, or each writer its own in parallel
                    (concurrent memtable writes), then the group's sequence is published
```

The leader merges the group's batches into one WAL record, writes it, and, if any member asked
for durability, issues one barrier for the whole group. Memtable inserts then run either by the
leader alone or in parallel by every member (the default, which the skiplist's lock-free
insert allows). The last sequence number is published only when every member has inserted, so
readers never see a partial group. Options split the pipeline further: *pipelined write* lets
the next group's WAL write overlap this group's memtable inserts; *two write queues* separate
WAL-only writes; *unordered write* gives up snapshot immutability for throughput. None is on by
default, and the harness uses none.

- **Invariant:** acknowledged writes are durable to the level their group asked for, and become
  visible in sequence order; a sync writer is never acknowledged on a non-sync group's write.
- **Cost:** one `write()` and at most one barrier per group, shared by its members; a
  handoff per follower (a short spin, then a blocking wait). With one writer there is no one to share
  with: the audit's one-thread sync rows show exactly one WAL barrier per batch (709 batches,
  709 barriers, 20.8 ms each on this Mac, 98% of the run), and no write done by another thread.
- **Explains:** **D.** strata's durable write is a full commit: five barriers per batch against
  RocksDB's one, the 4.7–5.2× of the sync rows. Group commit itself is not what wins at one
  thread; the single barrier is.

### 1.3 The write-ahead log

One WAL serves every column family. It is a sequence of 32 KiB blocks; each logical record
(a group's merged batch) is split into fragments that never cross a block boundary, each with a
7-byte header: a CRC32C over type and payload, a 2-byte length, and a type (whole, first,
middle, last). A *recyclable* variant adds the log number to the header (11 bytes) so a reused
preallocated file's stale tail is recognized and rejected. A block's tail shorter than a header
is zero-filled.

```
 | 32 KiB block                               | 32 KiB block                    |
 | hdr FULL rec | hdr FIRST rec…  | hdr MIDDLE …                | hdr LAST …| pad |
   CRC32C(4) len(2) type(1) [log#(4) recyclable]
```

The WAL file is written through a user-space buffer; after each group the buffer is flushed
to the OS unless `manual_wal_flush` is set. Durability is a separate step: `sync` on the write
(1.2), an explicit sync call, or the sync of closed WALs that precedes a flush. A new WAL file's
directory entry is synced once, the first time any write on it asks for sync. When the memtable
switches (1.5), a new WAL starts; an old WAL is deleted when every column family has flushed
past it, which `max_total_wal_size` forces by flushing the column families that pin the oldest
log.

- **Invariant:** a record is either recovered whole with a valid checksum or not at all; under
  the default recovery mode (point in time) replay stops at the first damaged record, so the
  recovered state is a prefix of the acknowledged history.
- **Cost:** with `sync=false` a `write()` per group: the harness's WAL tier is 126,000 puts/s
  against 529,000 with the WAL disabled, 4.2–4.7× slower, nearly all syscall. With `sync=true`
  one barrier per group (above).
- **Explains:** **D** (with 1.2).

### 1.4 The memtable: an arena skiplist

The default memtable is a skiplist whose nodes are allocated from an **arena**: blocks of
`write_buffer_size / 8` (4 MiB at the harness's fits setting), carved by bumping a pointer,
with a small per-core cache of partial blocks for concurrent inserts. Nothing is freed until
the whole memtable is dropped after its flush.

Each entry is one contiguous allocation: the node's tower of next pointers, then the encoded
entry: a varint length of the internal key, the internal key (user key plus 8 bytes), a varint
value length, and the value bytes. Node heights are random with a branching factor of 4 (a
quarter of nodes reach height 2, a sixteenth height 3, at most 12), so the tower averages 1.33
pointers. Inserts are lock-free (compare-and-swap per level), with an optional per-thread
insertion hint for sequential keys.

```
 arena block (4 MiB) ──▶ [tower: next[h-1] … next[0] | klen | user key | seq·type | vlen | value ] …
                           8 B × h (avg 1.33)          1     16          8          1      100
```

For the harness's 16-byte keys and 100-byte values the entry is 126 bytes, plus 10.7 bytes of
tower on average, plus alignment: about 137 bytes before arena slack. The measured RSS growth of
153 bytes per buffered record is that plus the partly filled arena block and allocator rounding.

- **Invariant:** the memtable is sorted by internal key at all times, so a get is a search and
  a scan is a walk; entries are immutable once inserted (an overwrite is a new entry with a
  higher sequence number).
- **Cost:** an insert is O(log n) key comparisons, most of them cache misses on a large
  memtable: the audit's fits fill rows spend about 1.9 µs per put (strata's buffer append is
  0.98 µs, so strata's blind fills win 1.2–1.9×). Memory is the entry plus a pointer and a third.
  Overwrites keep every version until flush.
- **Explains:** **M.** RocksDB's buffered record costs its bytes plus a fixed ~11–20 bytes in
  one arena; strata's costs 683 bytes because its buffer grows by doubling arrays and keeps
  per-document arrays (`core-vs-rocksdb.md`, "Also measured"). The fix that the arena points to
  is chunked growth and one inline record per entry, not a different structure.

### 1.5 Memtable switch, the immutable list, and flush triggers

When the mutable memtable is full it becomes **immutable** and a new one (with a new WAL)
takes writes. Full means any of: its arena passes `write_buffer_size`; the write buffer manager
asks for a flush (1.6); the WAL total passes `max_total_wal_size`; or a manual flush. Up to
`max_write_buffer_number` memtables (2 by default) may exist; a flush takes the oldest
immutables (at least `min_write_buffer_number_to_merge`, 1 by default) together.

```
 mutable ──full──▶ immutable #1 ──▶ flush job (background) ──▶ L0 file
    ▲                  │ (reads still see it)
    └── new memtable + new WAL
 max_write_buffer_number reached ──▶ writers STOP until a flush finishes (1.9)
```

- **Invariant:** a reader's view includes every immutable memtable until the version that
  replaces it with an L0 file is installed, so no record is ever invisible during a flush.
- **Cost:** a switch is cheap (a pointer swap under the DB mutex and a new WAL file); the risk is
  the stop when every memtable slot is full and the flush is slow. The audit's 2× fill rows hit
  exactly this: 1 MiB memtables, two slots, and a flush whose three barriers take about 65 ms,
  so writers stood stopped for 0.66–0.73 s of a 1.05 s run (15 stops).
- **Explains:** none of strata's losses; it is where strata's 2× fill rows *win* (5.4×), and
  that win is mostly macOS barrier latency (the audit attributes it).

### 1.6 The write buffer manager

One object caps memtable memory across column families and DB instances. It triggers a flush
of the largest eligible memtable when mutable memory passes 7/8 of its limit, or when total
memory passes the limit while mutable memory is at least half. Given a cache, it charges its
memory to the cache by inserting placeholder entries of 256 KiB each, so one capacity bounds
memtables and cached blocks together. With `allow_stall` it stops every writer while total
memory is over the limit.

- **Invariant:** memtable memory stays near one configured number for the whole process,
  across column families; with a cache, memtables and blocks compete for one capacity.
- **Cost:** accounting on every arena block allocation (an atomic add) and placeholder churn in
  the cache; flushes earlier and smaller than `write_buffer_size` would make them.
- **Explains:** none directly; it is what makes the harness's equal-memory rule meaningful on
  RocksDB's side. strata's `byte_budget` is the same mechanism.

### 1.7 Flush

A flush job merges the chosen immutable memtables with a merging iterator (3.2), drops versions
no snapshot needs and resolves merges where it can, and writes one L0 table (2.3): data blocks
compressed one at a time, index, filter, properties, footer. Then, in order: the table file is
synced; its directory is synced; a version edit adding the file and advancing the column
family's oldest needed WAL number is appended to the MANIFEST and the MANIFEST is synced; the
new version is installed; WALs no column family needs are deleted.

- **Invariant:** a table is visible only after it and its MANIFEST record are durable, so a
  crash at any point leaves either the memtable (recoverable from the WAL) or the durable table,
  never a table the MANIFEST names but the disk lacks.
- **Cost:** three barriers per flush (counted: table, directory, MANIFEST, 20–30 ms each on this
  Mac), plus compression on a background thread. The writer pays nothing unless memtables run
  out (1.5). With `disableWAL` the flush is the *only* durability, so the harness's settle is
  three barriers for RocksDB against four for strata's commit (five when it flushes a batch).
- **Explains:** **D** ("to durable" fills: a flush is 3 barriers, strata's commit 4–5 plus merges).

## 2. Read path

### 2.1 SuperVersion: a reader's consistent view

A column family's current memtable, immutable list, and Version (the set of table files per
level) are bundled in a reference-counted **SuperVersion**. Each thread caches the current one
in a thread-local slot; a read swaps it out, uses it, and swaps it back, so a get takes no
mutex and no shared atomic in the common case. A new SuperVersion (after a switch, a flush, or
a compaction) invalidates the thread-local slots, and each thread picks up the new one on its
next read.

- **Invariant:** a read sees one consistent snapshot of memtables and files for its whole
  duration, and the files it uses are not deleted while it holds them.
- **Cost:** two thread-local exchanges per read; a reference count update only when the view
  changed.
- **Explains:** none (strata's segment set snapshot plays this role).

### 2.2 The order of a get

```
 get(k) ─▶ memtable ─▶ immutables (newest first) ─▶ L0 files (newest first, each overlapping)
                                                 ─▶ L1 … Ln: one file per level by key range
          stop at the first record for k: a value returns it, a tombstone returns "not found",
          merge operands accumulate until a base value or the bottom
```

Within L1 and below, files of a level are disjoint and sorted, so one binary search over their
boundaries finds the only candidate; a file indexer carries the position found in one level
into narrower bounds for the next. Each candidate table is probed through its filter first
(2.4).

- **Invariant:** the first record met is the newest visible one, because sources are searched
  newest first and each source sorts newer versions first.
- **Cost:** per source, a memtable search or a filter probe; per table actually searched, an
  index search and a data block search. The audit's fits `readrandom` touched one source per
  get (one L0 file; the memtable missed 100,000 of 100,000), the YCSB rows one or two (memtable
  hits 20–66% for A, D, F; the rest from L6).
- **Explains:** **G** in combination with 2.3–2.7: the order is not what costs strata, the
  per-source work is.

### 2.3 The block-based table

```
 [data block 0][data block 1] … [data block n][filter][index][properties][compression dict]
 [meta-index: name → handle][footer: meta-index handle, index handle, format version, magic]
```

Every block carries a trailer: a compression-type byte and a 4-byte checksum (XXH3 by
default; since format version 6 the checksum is mixed with the block's offset in its file, so a
block read from the wrong place fails). A block is stored compressed only if that saves at
least an eighth of its size. Data blocks are cut at `block_size` (16 KiB in the harness's
calibrated variant, 4 KiB by default). With `max_open_files = -1` (the default) every table
stays open with its footer, index, and filter loaded or cached.

- **Invariant:** a table is self-describing and immutable; any block can be read and verified
  alone, by offset.
- **Cost:** one read and one decompression per block; the index and filter are read once per
  open table, then served from the cache or the table reader.
- **Explains:** **G** and **S** together with the cache: the unit of work is a block of a few
  KiB, sized independently of how values were written.

### 2.4 Filters: Bloom and Ribbon

Each table has one full filter (or partitions of one, 2.5) over its keys. The default Bloom
filter is cache-local: a key's hash picks one 64-byte cache line, and all of its probes (6 at
10 bits per key) fall in that line, so a negative costs one cache miss; at 10 bits per key the
false-positive rate is about 1%. The Ribbon filter solves a linear system over the keys' hashes
to reach the same rate in about 30% less space, for more CPU per probe and more memory to
build; a hybrid uses Bloom on upper levels and Ribbon below. Filters are built per table at
flush and compaction, never updated.

- **Invariant:** no false negatives; a negative proves the key is absent from that table.
- **Cost:** 10 bits per key in the cache; one cache line per probe. The audit's fits
  `readmissing` saw 991 false positives in 100,000 probes (0.99%), each costing a data block
  lookup.
- **Explains:** none: strata's binary fuse filter is already faster (`readmissing` 2.8× faster
  at fits, 6.9× at 2×) at a lower false-positive rate (0.39%).

### 2.5 Index blocks and partitioned indexes

The index holds one entry per data block: a separator key (the shortest key at or above the
block's last key and below the next block's first) and the block's handle. Handles are
delta-encoded (an offset is implied by the previous block's end) and the index's restart
interval is 1, so binary search reaches any entry directly. Unless a user key's versions straddle a block
boundary, separators drop the sequence number and are plain user keys. A **partitioned index** cuts the index into
blocks of about 4 KiB behind a small top-level index that stays pinned; filters can be
partitioned alongside, so a large table's metadata pages in on demand.

- **Invariant:** the separators are ordered, and the block a key would live in is the first
  whose separator is not below it.
- **Cost:** one binary search over a cached index block per table searched (about 750 entries
  in the harness's 7.5 MB L0 table at 16 KiB blocks); for partitioned indexes one more block.
- **Explains:** **G** only in the sense that strata already has the equivalent (the key
  dictionary's FST and key blocks), and its key lookup is under 2% of a get's time.

### 2.6 The data block: restart points and the hash index

Entries in a data block are prefix-compressed against the previous key: each stores the shared
prefix length, the unshared suffix length, the value length, the suffix, and the value. Every
16th entry (`block_restart_interval`) is a **restart point** stored with its whole key, and the
block ends with the array of restart offsets and their count.

```
 | e0 (full key) | e1 (shared, suffix) | … | e16 (full key) | … | restart[0..r) | r | trailer |
 seek: binary search the restart keys ──▶ linear scan of ≤ 16 entries from that restart
 hash index (optional): bucket[hash(user key)] = restart index, or empty, or collision
```

A seek binary-searches the restart keys, then scans forward at most one interval. The optional
data block hash index appends a byte array of buckets mapping a user key's hash to its restart
interval; a point get with a unique bucket skips the binary search, and a collision falls back
to it. It is used for gets only.

- **Invariant:** keys within a block are sorted; any restart point decodes alone, so a block
  supports random access at interval granularity.
- **Cost:** about log2(r) full-key comparisons plus up to 16 delta decodes per get; the block's
  values sit next to their keys, so the value costs no further read.
- **Explains:** **G** and **S.** The get's cost inside a cached block is a few hundred
  nanoseconds and needs no decompression; strata's record lives in a 64 KiB zstd frame
  decompressed whole per get (47% of a get's time) into a new heap buffer whose free costs a
  `madvise` (43%).

### 2.7 The block cache: HyperClockCache

Blocks are cached **uncompressed and parsed**, keyed by 16 bytes: a per-file base derived from
the DB session and the file number, plus the block's offset. Filter and index blocks share the
cache when `cache_index_and_filter_blocks` is set (the harness sets it, so one budget bounds
them). Since 10.7 the default cache is **HyperClockCache**:

```
 shard = bits of the key's hash (shards = capacity / 32 MiB, at most 64)
 slot array (open addressing; the automatic variant grows it by splitting, a linear-hashing scheme)
 slot: [meta word: acquire count | release count | state] [key 16 B] [value*] [charge]
       references = acquires − releases; countdown = min(3, acquires) while unreferenced
 lookup  = probe slots; one atomic add on the acquire count takes a reference
 release = one atomic add on the release count
 evict   = a clock hand sweeps; an unreferenced entry's countdown drops each pass, at 0 it is freed
```

An entry's countdown starts from its priority (index and filter high, data low) and each hit
raises it, up to 3, so frequently hit blocks survive several sweeps (generalized CLOCK with
aging); folding the countdown into the reference counters is what makes a hit one atomic add. Eviction runs in parallel on inserting threads; a referenced entry is never evicted. The
cache exceeds its capacity rather than fail an insert unless `strict_capacity_limit` is set.

- **Invariant:** a block handed to a reader stays valid until released; the total charge stays
  near capacity; lookups never block.
- **Cost:** one hash, a probe or two, and two atomic adds per block used. At fits the audit
  counted 99.3% data block hits on `readrandom` (747 misses, one per block, all first touches)
  and 93% on YCSB C; at 2× 39–61% (the rest read from the OS page cache, no device reads).
- **Explains:** **G** (the main term): a cached get costs a lookup, not a decode. strata has no
  decoded-frame cache, so its cost per get is the frame's decode at every regime.

### 2.8 Pinning and zero-copy reads

A read pins the blocks it uses through cache references. A get into a pinnable value can hand
back a slice into the cached block with no copy (the harness's get copies into a string, as
strata's does). L0 index and filter blocks can be pinned for the table's life
(`pin_l0_filter_and_index_blocks_in_cache`, set in the harness), and the top level of
partitioned metadata is pinned by default. Iterators can pin every block they pass so keys stay
valid across steps.

- **Invariant:** a slice handed out stays valid while its pin is held.
- **Cost:** a reference held per pinned block; pinned blocks cannot be evicted, so pinning
  trades capacity for lookups.
- **Explains:** **G** (no allocation per get: strata allocates and frees a frame buffer per get).

## 3. Iterators and the merging iterator

Each source has an internal iterator over internal keys: the memtable's skiplist walk; a table's
**two-level iterator** (the index iterator yields block handles, the data iterator walks the
block, fetched through the cache); and, for each level from L1, a **level iterator** that opens
the level's files one after another as the walk crosses them. A **merging iterator** combines
them with a min-heap keyed by internal key (a max-heap when moving backward), and a **DB
iterator** on top hides what a user must not see: older versions of a key, tombstones, records
newer than the read's sequence number, and merge operands (folded into a value).

```
 DB iterator (visibility, versions, tombstones, bounds)
      └── merging iterator (heap)
            ├── memtable ├── immutable … ├── L0 file 1 … L0 file k
            └── L1 level iterator ─▶ file ─▶ two-level (index ─▶ data block) … └── Ln
```

`Next` advances the heap's top child and restores the heap; changing direction re-seeks every
child. Range tombstones are kept as fragmented, sorted, non-overlapping pieces per source and
skip covered keys in bulk. `iterate_upper_bound` and prefix bounds stop the walk and let tables
and blocks beyond the bound be skipped. Sequential block reads trigger automatic readahead after
two reads from the same file, starting at 8 KiB and doubling up to 256 KiB.

- **Invariant:** keys come out in order, each visible user key exactly once with its newest
  visible value, regardless of how many sources hold versions of it.
- **Cost:** per `Next`, one heap operation over the children (one child per memtable, per L0
  file, and per non-empty level: one to three in the audit's runs) plus the sequential decode of
  the current block. A scan of n records reads about n × record size / block size blocks, in
  order: the audit's fits `readseq` read 747 blocks for 100,000 records, `seekrandom` 10,738
  block lookups for 10,000 seeks of 10.
- **Explains:** **S.** Key-ordered blocks make a scan sequential; strata stores records in docid
  order, so ten adjacent keys are ten frames, each decoded whole (`seekrandom` 96× slower). The
  merging iterator itself is what strata's scan path also needs (segments plus the buffer, with
  bounds), but the loss is the layout, not the heap.

## 4. Compaction

### 4.1 Leveled compaction with dynamic level sizes

L0 holds flushed tables whose key ranges overlap; each level from Lbase down is one sorted run
cut into tables of about `target_file_size_base` (64 MiB), disjoint in key range. With dynamic
level sizes (the default), the last level's size sets the targets upward, each level a tenth of
the next, and Lbase is the highest level whose target stays above a floor; levels above it stay
empty. A small DB therefore has only L0 and the last level: the audit's YCSB loads compacted L0
straight into L6.

```
 L0: [a…z] [b…y] [a…x] [c…z]     overlapping, newest first      score = files / 4
 L5: [a…f][g…m][n…z]             one sorted run, target T/10     score = size / target
 L6: [a…c][d…h][i…p][q…z]        one sorted run, the last level  target set by its own size
```

Each level gets a score: L0's is its file count over `level0_file_num_compaction_trigger` (4),
other levels' their size over their target. The highest score of at least 1 is compacted. In a
level, the file picked by default is the one whose overlap with the next level is smallest
relative to its own size (least write amplification per byte moved); its inputs grow to cover
the overlapping range below; a file with no overlap below moves down without rewriting. When
L0 cannot compact into Lbase because Lbase is busy, an intra-L0 compaction merges L0 files
together. At the bottom, compaction drops tombstones and versions no snapshot needs.

- **Invariant:** below L0, each level is one sorted run, so a get reads at most one table per
  level and a scan merges one iterator per level; space amplification stays near 1.11 with a
  multiplier of 10.
- **Cost:** write amplification of roughly the multiplier per level a byte passes through; each
  output table is synced, then the directory, then the MANIFEST (counted: three barriers for one
  output table).
- **Explains:** **S** indirectly: few sorted runs mean few children per scan. strata's tiered
  merges keep more segments, but the measured scan loss is the per-record frame decode, not the
  segment count.

### 4.2 Universal compaction

All data is a sequence of sorted runs ordered by age (each L0 file is a run; each non-empty
level is one). When the run count reaches the trigger, in order: if the bytes of all runs but
the oldest exceed `max_size_amplification_percent` (200%) of the oldest, compact everything;
otherwise merge the newest runs while each next run is no larger than the accumulated size
times one plus `size_ratio` (1%); otherwise merge enough of the newest runs to bring the count
below the trigger.

```
 newest ─▶ [4 MB] [4 MB] [8 MB] [60 MB] [500 MB] ◀─ oldest
            └── size-ratio merge ──┘               full compaction when the others pass 2× of 500 MB
```

- **Invariant:** run count and space amplification stay below their triggers; runs remain in
  age order, so a newer run always shadows an older one.
- **Cost:** lower write amplification than leveled, at up to twice the space during a full
  compaction, and bursty I/O. The harness's calibration measured it (`uni-4k-lz4`) slower than
  leveled at 16 KiB on these rows.
- **Explains:** none; it is the mechanism closest to strata's tiered merges.

### 4.3 Subcompactions

A large compaction, typically L0 into Lbase, is split into key ranges at boundaries sampled
from its input files, and each range is compacted by its own thread into its own output tables;
the outputs concatenate into one sorted run. `max_subcompactions` is 1 by default.

- **Invariant:** the subranges are disjoint and cover the input, so the concatenated output is
  sorted and complete.
- **Cost:** more threads and memory for one compaction; no extra I/O.
- **Explains:** none of the measured losses (strata's merge parallelism is M8 in the survey).

### 4.4 Rate limiting and background scheduling

`max_background_jobs` (2 in the harness) is split into flush slots (a quarter, at least one) in
a high-priority pool and compaction slots in a low-priority pool, so flushes never queue behind
compactions. A rate limiter, if configured, is a token bucket refilled every 100 ms, with
priorities (flush above compaction above user); it can tune its own rate from how often it
drains. WAL writes are never limited. Range syncs (`bytes_per_sync`) spread a file's writeback
on Linux; macOS has no range sync, so the harness never issues any.

- **Invariant:** background I/O stays under a configured rate; flushes are never starved by
  compactions.
- **Cost:** latency for background jobs when the bucket is empty.
- **Explains:** none (strata's merges are budget-bound instead, K4).

### 4.5 Write stalls and slowdowns

A write controller turns compaction debt into backpressure on writers, per column family:

| Condition | Delay (paced at `delayed_write_rate`, 16 MiB/s by default, adjusted) | Stop |
|---|---|---|
| Memtables | count ≥ `max_write_buffer_number` − 1, when that option is above 3 | count ≥ `max_write_buffer_number` |
| L0 files | ≥ `level0_slowdown_writes_trigger` (20) | ≥ `level0_stop_writes_trigger` (36) |
| Pending compaction bytes | ≥ soft limit (64 GiB) | ≥ hard limit (256 GiB) |
| Write buffer manager | — | total over limit, with `allow_stall` |

A delayed writer sleeps in proportion to its bytes; a stopped writer waits for a flush or
compaction to finish. The delay rate falls while debt grows and recovers when it shrinks.

- **Invariant:** read amplification (L0 file count) and memory (memtable count) stay bounded
  however fast writes arrive.
- **Cost:** write latency and throughput when background work lags. In the audit: no L0
  slowdown or stop fired in any row (L0 never passed 4 files); memtable stops fired in every
  1 KiB load at fits (2–3 stops, 40–64 ms, outside the timed rows) and dominated the 2× fill
  rows (0.66–0.73 s of 1.05 s).
- **Explains:** none of strata's losses: strata has no merge-debt backpressure yet, and its 2×
  settle instead sleeps for budget (`core-vs-rocksdb.md`, loss 3).

## 5. The MANIFEST, versions, and version edits

The on-disk state of the files is a log of **version edits** in the MANIFEST, written in the
WAL's record format. An edit adds or deletes table files (with their level, size, key range,
sequence range, and checksum), records each column family's oldest needed WAL number, the next
file number, the last sequence number, and column family creation or drop. Applying the edits
in order yields the **Version**: the files per level of every column family. `CURRENT` names
the live MANIFEST.

```
 CURRENT ──▶ MANIFEST-000005: [snapshot of all files] [edit +f8 L0] [edit −f8 +f12 L6] …
 LogAndApply: queue edits ─▶ leader appends the group ─▶ one MANIFEST sync ─▶ install Versions
 new MANIFEST (> max_manifest_file_size, 1 GiB): write snapshot, sync, write CURRENT via temp file,
                                                  sync it, rename, sync the directory
```

Concurrent edits from flushes and compactions queue and are written by one leader with one sync,
the same group-commit shape as the WAL. Versions are reference-counted: a file is deleted only
when no live Version (held by a reader, an iterator, or a compaction) references it.

- **Invariant:** the set of live files is exactly what the MANIFEST's edits produce; a file is
  durable before an edit names it, and deleted only after no edit or reader needs it.
- **Cost:** one MANIFEST sync per edit group (one per flush or compaction in the audit); a new
  MANIFEST and `CURRENT` at open (counted: ten barriers to open a new DB, 0.21 s on this Mac);
  creating a column family is one edit and one sync each (the many-index row's 1,000 creates).
- **Explains:** **D** in part: RocksDB's segment publication (a table and its edit) is off the
  acknowledgement path; strata's commit publishes segments and a manifest on it.

## 6. Sync policy per file type

On macOS with the build's `HAVE_FULLFSYNC`, every sync RocksDB issues, data or metadata, file or
directory, is `fcntl(F_FULLFSYNC)`; `use_fsync` makes no difference there. On Linux a file sync
is `fdatasync` unless `use_fsync` is set, and directories get `fsync`. Traced in the source and
counted with the barrier counter:

| File | Written | Synced | Barriers counted (macOS) |
|---|---|---|---|
| WAL | each write group (one `write()`) | on `sync=true`, once per group; explicit sync; closed WALs before a flush | one per synced group (709 per 709 batches) |
| WAL directory | at WAL creation | once, at the first sync on a new WAL | one per new WAL that is ever synced |
| Table from flush | while the flush builds it | once, after the last block | flush: table, directory, MANIFEST = 3 |
| Tables from compaction | per output table | each output table; the directory once per compaction | per compaction: outputs + 1 + MANIFEST |
| MANIFEST | per edit group | once per group | one per flush, compaction, or CF create |
| CURRENT, IDENTITY, OPTIONS | at open and on change | temp file synced, renamed, directory synced | open of a new DB: 10 in all |
| Info log | buffered | never | 0 |

- **Invariant:** nothing the MANIFEST names is volatile; nothing acknowledged under `sync=true`
  is volatile.
- **Cost:** one barrier per acknowledged durable group; three per flush; one per table plus two
  per compaction; on this Mac 20–30 ms each, on Linux NVMe one or two orders of magnitude less.
- **Explains:** **D**: one barrier per durable acknowledgement against strata's five.

## 7. Recovery

```
 open: read CURRENT ─▶ replay the MANIFEST into Versions ─▶ list WALs ≥ the oldest needed number
       ─▶ replay each WAL's records into fresh memtables (skipping column families already flushed
          past that WAL) ─▶ flush the recovered memtables to L0 (unless avoid_flush_during_recovery)
       ─▶ write a new MANIFEST and CURRENT ─▶ delete obsolete files ─▶ accept writes
```

Replay checks each WAL record's checksum; under point-in-time recovery it stops at the first
bad or missing record, so the result is a prefix of acknowledged history. Sequence numbers
resume after the last replayed one. With `disableWAL`, whatever was in memtables at a crash is
lost; with WAL writes unsynced, whatever the OS had not written back is lost on power failure
but survives a process crash.

- **Invariant:** after recovery the DB holds every write acknowledged as durable and a prefix of
  the rest, never a gap.
- **Cost:** recovery time proportional to the unflushed WAL; a flush per recovered memtable.
- **Explains:** none measured (the many-index reopen, 1.04 s for RocksDB against 0.66 s for
  strata, is table and column family opening, not WAL replay: the row ran without a WAL).

## 8. What `disableWAL` plus the final flush makes durable

With the WAL disabled, a put lives only in a memtable. The harness's settle flushes every
memtable (1.7) and waits for compactions (4.1); each flush makes its table, the directory entry,
and the MANIFEST edit durable before it installs the table; each compaction does the same for
its outputs. After the settle returns, the whole run is on disk and named by a durable MANIFEST:
a power failure after the settle loses nothing, and one before it loses everything since the
previous flush. That is the same contract as strata's `none` tier with one commit at the end,
so the "to durable" columns compare like with like; RocksDB's costs three barriers per flush,
strata's four or five per commit plus merges.

## 9. The mechanisms against strata's losses

| # | Mechanism | Invariant | Cost (measured where counted) | G | S | D | M |
|---|---|---|---|---|---|---|---|
| 1.1 | Batches and sequence numbers | Atomic, ordered visibility | A counter and one serialization | | | | |
| 1.2 | Write queue and group commit | Acknowledged ⇒ durable to the requested level, in order | One `write()` and ≤ 1 barrier per group | | | **✓** | |
| 1.3 | WAL | Recovered state is a prefix of acknowledged history | `write()` per group (4.2–4.7× vs no WAL); 1 barrier per synced group | | | **✓** | |
| 1.4 | Arena skiplist memtable | Always sorted; entries immutable | O(log n) inserts (~1.9 µs); ~137 B + slack per 116 B record | | | | **✓** |
| 1.5 | Memtable switch and immutables | No record invisible during flush | Writer stop when slots are full (2× fills: 0.7 s of 1.05 s) | | | | |
| 1.6 | Write buffer manager | One memory bound across column families | Accounting; earlier, smaller flushes | | | | |
| 1.7 | Flush | A table is durable before it is named | 3 barriers per flush, off the writer's path | | | **✓** | |
| 2.1 | SuperVersion | One consistent view per read, mutex-free | Two thread-local swaps per read | | | | |
| 2.2 | Get order | First record met is the newest visible | One probe per source | ✓ | | | |
| 2.3 | Block-based table | Immutable, self-describing, block-addressable | One read + decompress per block miss | **✓** | **✓** | | |
| 2.4 | Bloom and Ribbon filters | No false negatives | 10 bits/key; 1 cache line per probe; 1% FP | | | | |
| 2.5 | Index and partitioned index | Separators ordered | One binary search per table searched | | | | |
| 2.6 | Data block, restarts, hash index | Sorted keys; values beside keys | log2(r) compares + ≤ 16 decodes | **✓** | **✓** | | |
| 2.7 | HyperClockCache | Lock-free lookups; pinned while used | Two atomic adds per block; 93–99% hits at fits | **✓** | ✓ | | |
| 2.8 | Pinning, zero-copy | Slices valid while pinned | Capacity held by pins | ✓ | | | |
| 3 | Merging and DB iterators | Ordered, newest visible version once | One heap step per `Next`; sequential blocks | | **✓** | | |
| 4.1 | Leveled compaction | One sorted run per level below L0 | ~10× write amplification per level; 3+ barriers | | ✓ | | |
| 4.2 | Universal compaction | Bounded runs and space amplification | Up to 2× space in a full compaction | | | | |
| 4.3 | Subcompactions | Disjoint covering subranges | Threads and memory | | | | |
| 4.4 | Rate limiter, pools | Bounded background I/O; flush first | Background latency | | | | |
| 4.5 | Write stalls | Bounded L0 files and memtables | Writer latency under debt | | | | |
| 5 | MANIFEST and version edits | Live files = applied edits; durable before named | 1 sync per edit group; 10 barriers per new DB | | | ✓ | |
| 6 | Sync policy per file | Nothing named or acknowledged is volatile | 1 barrier per durable ack; 3 per flush | | | **✓** | |
| 7 | Recovery | Durable writes kept; no gaps | Proportional to unflushed WAL | | | | |

**✓** marks a mechanism that accounts for the bulk of a loss; ✓ one that contributes.

Read by loss:

1. **G, point gets:** blocks of 16 KiB decoded once and cached uncompressed (2.3, 2.7), a get
   inside a cached block costing a restart search and no allocation (2.6, 2.8). strata's
   64 KiB frame decode and buffer free per get is the whole gap.
2. **S, scans:** key-ordered blocks with values beside keys (2.3, 2.6) walked sequentially by a
   merging iterator over few sorted runs (3, 4.1). strata's docid-ordered records turn a scan
   into a frame decode per record.
3. **D, durable writes:** one WAL barrier per acknowledged group (1.2, 1.3, 6), with table and
   MANIFEST publication on background threads (1.7, 5). strata's commit puts five barriers on
   the acknowledgement path.
4. **M, memory per buffered record:** one inline arena entry per record (1.4). strata's buffer
   pays for doubling arrays and per-document arrays.

Where RocksDB's mechanisms cost it and strata already wins: blind fills (skiplist inserts,
1.4), absent keys (cache-local Bloom against binary fuse, 2.4), and the 2× fills, where every
flush is durable and so stops writers on barriers (1.5, 1.7, 4.5).
