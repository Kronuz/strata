# Format 14: the document layout as a key-value store

Revision 0, 2026-10-20. Design only to the first buildable step (PLAN, "Formats stay open until
the first release", `docs/PLAN.md:16-25`): no review ladder; the owner settles the shape from
"Shape for the owner" below, then a developer builds step 1. Decisions D1 to D5, gate rows T1 to
T8, owner questions Q1 and Q2. Code is cited at main `426cc59`.

strata is the core now (PLAN, "Names", `docs/PLAN.md:74-85`); search is prospect's. This design
changes the core's `document_layout` (`include/strata/core/reader.hh:525`), whose job is to beat
RocksDB as a key-value store on all four axes (PLAN, "The `document` profile gate",
`docs/PLAN.md:616-626`; the lead's priority, `docs/status/README.md:19-31`), for the many-index
workload: tens of thousands of open indexes per host, so per-open-index memory counts
(`docs/PLAN.md:222-231`).

Every number below is either measured by this design's prototypes (`bench/kv/proto/`, raw lines
in `docs/design/format-14-kv/`), quoted from a cited report, or marked as an estimate.

## What this builds on, and what it changes

| Document | What this design uses | What it changes |
|---|---|---|
| `docs/perf/core-vs-rocksdb.md` | The four losses and their profiles (`:177-245`) | Nothing; it is the input |
| `docs/perf/rocksdb-fairness-audit.md` | A5 (index and filter blocks in RocksDB's cache cost it 7% on YCSB C, `:16, :134`); A6's read sources (every miss served by the page cache) | Nothing |
| `docs/research/rocksdb-mechanisms.md` | 2.3 block table, 2.6 restarts, 2.7 HyperClockCache, 3 merging iterator, 1.4 arena memtable (`:128, :316, :343, :387`) | Nothing; no RocksDB code enters strata |
| `docs/design/rocksdb-survey.md` | K3 (bounded ordered scans, `:208`), K5 (key-ordered merges as index sorting with the key, `:210`), K6 (frame size, then a CLOCK cache charged to the budget, `:211`), M1 and M3 (`:242-244`) | Answers M3 and M1 for the document layout; K5 becomes the document layout's order at flush as well as at merge |
| `docs/design/format-13.md` | F8 (the key dictionary, `:524-726`), F9 (versions), F18 (`index_sort`, `:1043-1052`), F19 (stored frames, `:1054-1065`), F4's structure bits | F18's sort gains a key component and F4's requiring rule for it; F19's frames gain a codec byte and a writer-chosen size |
| `docs/design/core-split.md` | The core's hooks (`order_source`, `:33`); "scan: there is none yet" (`:82-85`) | Adds `order_source<document_layout>` and the core's `scan` |
| `docs/design/layers.md` L12 (`:856-878`) | `document_layout = layout<keys_plugin, stored_plugin>` | Its segments are key-ordered |
| `design/durability-modes` (branch), D1 and first step 1 | Commit becomes durability (one turn-log barrier), visibility, and a background checkpoint; step 1 is `RFTW0002` in Kronuz/journal, approved | Nothing: loss 3 is that design's (below) |

## The losses, and what removes each

| Loss (measured, `core-vs-rocksdb.md`) | Cause (profiled there) | Removed by |
|---|---|---|
| Point gets 12-32× slower | A 64 KiB zstd frame decoded per get (47%) into a fresh buffer whose free costs `madvise` (43%) (`:177-188`) | D1 (4 KiB blocks, LZ4), D2 (decoded-block cache, reused buffers) |
| Scans 45-220× slower | Records in docid order: a frame per record; no sorted view of the buffer (`:199-212`) | D1 (records in key order), D3 (core scan, the buffer's sorted view), D4 (merges keep key order) |
| Durable writes about 5× slower | Five barriers per commit against one WAL barrier per group (`:214-228`) | Not here: durability-modes D1 and its step 1, the durable turn log (one barrier per turn, checkpoint off the acknowledgement path). This design adds no barrier and needs only that apply stays deterministic and replayable, which it does |
| 683 B per buffered record against 153 B | The search layout's doubling arrays (`:235-240`); the document layout's buffer measures 494 B (below) | D3 (arena records, a compact version map) |

## The prototypes

Four programs under `bench/kv/proto/`, each a CMake target outside `all`, run by
`bench/kv/proto/run.py` in three interleaved rounds (configurations alternate direction each
round), medians quoted. Host: the development Mac (i9-9980HK, 8 cores, 32 GB, macOS 15.8.1,
APFS), Apple clang 17, `-O3 -DNDEBUG`, the pinned zstd 1.5.7 and the system LZ4 1.10.0. One-thread
runs under light holds, the cache's 1 to 16 threads under a strict hold. Records are the KV
harness's: 16-byte keys and db_bench's half-compressible values from `bench/kv/kvgen.hh`.
Durations, read from each run's `wall_s`: blocks 350 s plus 2 min for the split layout's zstd rows,
cache 97 s, buffer 56 s, the current buffer's probe 3 s.

| Program | Measures | Raw file |
|---|---|---|
| `blocks.cc` | Key-ordered blocks of 4, 8, 16 KiB; no codec, LZ4, zstd 1 and 3; two layouts (below) and today's 64 KiB zstd frames; 1,000,000 records of 100 B and 200,000 of 1 KiB | `blocks.jsonl.gz`, `frame-ab.jsonl.gz` |
| `cache.cc` | A 16-way set-associative CLOCK cache against a 64-shard mutex LRU (RocksDB's LRUCache shape), 1 to 16 threads, YCSB's scrambled zipfian | `cache.jsonl.gz` |
| `buffer.cc` | Buffered records in an arena with a skiplist, with a hash and a B-tree, or with a hash and sorted runs built on demand; 1,000,000 records of 100 B and 200,000 of 1 KiB | `buffer.jsonl.gz` |
| `bufmem.cc` | Today's document-layout buffer: resident bytes per record, 100,000 to 300,000 buffered | `bufmem.jsonl.gz` |

`docs/design/format-14-kv/report.md` holds every configuration's medians. What the prototypes do
not include: mmap and page faults (blocks are in RAM), region checksums, the filter, several
segments, the index's snapshot and live-bit checks, and any platform but macOS. Their numbers
choose among options; the gate rows measure the engine.

The targets exist only with `-DSTRATA_KV_PROTO=ON` (no other build fetches cpp-btree or looks for
LZ4); the resident-memory probes read macOS's task info or Linux's `/proc/self/statm`.

```sh
cmake -S . -B build-rel -G Ninja -DCMAKE_BUILD_TYPE=Release -DSTRATA_KV_PROTO=ON
ninja -C build-rel kv_proto_blocks kv_proto_cache kv_proto_buffer kv_proto_bufmem
QUIET_LEVEL=light quiet run f14 30 "format-14-kv prototypes" -- python3 bench/kv/proto/run.py blocks OUT 3
quiet run f14-cache 10 "format-14-kv cache" -- python3 bench/kv/proto/run.py cache OUT 3
python3 bench/kv/proto/run.py report OUT
```

## D1. Key-ordered value blocks

### D1a. A document-layout segment's ordinals follow its keys

A segment's ordinal order is its writer's choice, recorded in the segment; the document layout
chooses key order, at flush and at merge. Ordinal i is the key of rank i, so the stored section,
which holds records by ordinal, holds them in key order, and a scan reads blocks sequentially.
Documents without a key (an index with a key prefix accepts an add without one, format-13 F8)
sort after every key, by docid.

The order is recorded with format 13's `index_sort` (F18, kind 17, bit 128: `format.hh:123, 136`),
whose bit already means "the segment's ordinals follow that order". Its description gains one
component kind: slot id `0xFFFFFFFF` names the key (ascending; `missing` last puts keyless
documents at the end). F4's requiring rule changes for it: `index_sort` requires `values` when a
component names a slot (`format.hh:148`) and `keys` when it names the key. This is the survey's K5
("index sorting with the primary key as the sort key, a preset, not a mechanism",
`rocksdb-survey.md:210`), applied from the flush on. `index_sort` has no reader yet
(`reader.hh:170`); step 1 builds the one for this component, and D14's slot sorts reuse it.

**Docid order, the question asked.** The document layout does not need docid order. A docid stays
the core's identity (`ARCHITECTURE.md:50-52`: docids per shard, monotonic, kept by a replace),
and the docids section maps both ways in any order: kind 2, permuted (`docids.hh:24-35`), exists
for reordered merges. What breaks when records are key-ordered, and what each needs:

1. The flush writes in docid order (`index.hh:2055-2067` sorts the buffer by docid) and the writer
   refuses decreasing docids unless told the order is permuted (`writer.hh:300`): the document
   layout's flush sorts by key and sets `permuted`. D18's parallel flush splits into docid runs
   (`index.hh:2065`); it splits into key ranges instead.
2. The merge orders every live document by sorting on docid (`merge.hh:201-224`, which also
   checks that no docid has two live copies) and builds an `unordered_map` of every live key per
   source (`merge.hh:52, 188-198`, charging 64 B plus the key each): D4 orders by a k-way merge of
   keys and needs no map; the duplicate check stays.
3. A docid lookup in a segment (`reader.hh:200`, used by `locate_docid`, `index.hh:1618-1625`)
   becomes a search of the permuted kind's sorted ordinals instead of arithmetic. Only
   docid-addressed operations use it; the key-value operations go through keys.
4. Bytes: the permuted kind costs about 5 B per record at a million records (two 20-bit arrays and
   an 8-byte entry per 128, from `docids.hh:24-35`; computed, not measured) where a flush's
   monotone docids cost about 1 B. Rank ordinals (D1d) give about 3 B back.
5. Nothing else: live-docs bits, versions (F9), and the digest (a sum over records keyed by
   docid, `index.hh:701-707`) are order-free; replicas build the same bytes, since sorting by key
   is deterministic. The search layout keeps docid order (D5).

### D1b. Values beside the key dictionary, not inside it

Two shapes were prototyped, each reading key-ordered blocks:

- **split**, strata's sections kept: `keys` as format 13 builds it (a termdict, each value the
  key's ordinal, `keys.hh:72-112`) maps a key to its ordinal, and the stored section holds the
  values alone in blocks by ordinal. With D1a the value blocks are key-ordered.
- **inline**, RocksDB's block table (`rocksdb-mechanisms.md` 2.3, 2.6): blocks of keys and values
  together, keys prefix-coded with a restart every 16 entries, found through an array of each
  block's first key; a new plugin would replace `keys` and `stored` in the document layout.

Medians, 1,000,000 records of 100 B, 4 KiB blocks, LZ4 (`report.md`; "hit" is a get on a decoded
block, what a cache hit costs past the lookup; "miss" decodes the block into a reused buffer;
both with DRAM misses, the records' 116 MB being past the L3):

| Shape | Bytes / logical | Get, hit | Get, miss | Seek and 10 | Scan per record |
|---|---:|---:|---:|---:|---:|
| inline | 0.584 | 829 ns | 1,239 ns | 1,033 ns | 24 ns |
| split (of which the key lookup: 665 ns) | 0.567 | 922 ns | 1,481 ns | 3,090 ns | 49 ns |
| today: 64 KiB zstd frames, a fresh buffer per get (`frame-ab.jsonl.gz`) | 0.456 (with `keys`) | | 36,436 ns | | |

At 1 KiB values the two trade places: split is faster at 16 KiB (709 against 1,042 ns, inline's
restart scan walking values), inline at 4 KiB (710 against 842 ns).

**Decision: split.** Gets are within 12% of each other and bytes within 3%, and both shapes beat
RocksDB's measured 2.5 µs of CPU per cached get (`core-vs-rocksdb.md:179`) and its 0.47 µs per
record of `readseq` by wide margins in this isolation. Split keeps one key dictionary for both
layouts (PLAN's rule: one implementation per mechanism) and changes only the stored section's block
size and codec, which prospect shares (D5). Its seek costs 3× inline's because
`termdict::reader::scan` builds every key of the starting block below `from` (format-13 F8, "A
seek's cost", `:598-600`); automata's additive `termdict::reader::cursor::seek` (AUT-R3) makes it
one restart run. Inline is the alternative the owner may choose (Q1); it is a new plugin and a
second key structure.

### D1c. 4 KiB blocks

| Block | 100 B: miss (LZ4 / zstd 1) | 100 B: bytes (LZ4 / zstd 1) | 1 KiB: miss (LZ4 / zstd 1) | 1 KiB: bytes (LZ4 / zstd 1) |
|---|---|---|---|---|
| 4 KiB | 1,481 / 4,808 ns | 0.567 / 0.476 | 1,282 / 4,224 ns | 0.563 / 0.465 |
| 8 KiB | 1,894 / 7,572 ns | 0.565 / 0.460 | 1,685 / 7,034 ns | 0.557 / 0.439 |
| 16 KiB | 2,804 / 12,242 ns | 0.565 / 0.458 | 2,560 / 11,045 ns | 0.554 / 0.432 |

(split layout.) A miss's cost grows with the block; LZ4's bytes barely move (under 2%), zstd's by
4% to 7%. Hits barely move with the block (867 to 1,042 ns at 100 B). **Decision: a target of 4 KiB
of raw records per block**, the writer's setting, so a record over 4 KiB takes a block of its own
as one over 64 KiB does today (`writer.hh:236-237`); a block over 64 KiB is decoded into the
caller's buffer and never cached (D2). 4 KiB is also the region size the stored section is verified
in (`format.hh:84-87`), so a block miss verifies one or two regions.

### D1d. A codec byte per block; LZ4 by default

| Codec, 4 KiB, 100 B | Bytes / logical | Miss | Build per record | Scan per record |
|---|---:|---:|---:|---:|
| none | 0.961 | 990 ns | 302 ns | 45 ns |
| LZ4 | 0.567 | 1,481 ns | 348 ns | 49 ns |
| zstd 1 | 0.476 | 4,808 ns | 521 ns | 130 ns |
| zstd 3 | 0.476 | 4,882 ns | 669 ns | 126 ns |

At 1 KiB values the build cost per record is 2.0 µs for LZ4 and 3.9 µs for zstd 1, the flush
thread's time (`core-vs-rocksdb.md:242-244` profiled zstd at 33% of 1 KiB loads). Each frame
starts with a codec byte: 0 stored raw, 1 LZ4 block, 2 zstd frame; a codec that saves less than an
eighth stores raw, as RocksDB does. The writer chooses per block, so a merge may recompress.
**Decision: LZ4 is the default for every tier in step 1**: it is the fastest measured codec that
still compresses (no codec doubles the bytes), and the reads are the 12-220× losses. zstd 1 for
merged output is the disk preset Q2 asks about: bytes 16% below LZ4's, a miss 3.2× LZ4's, which
D2's cache hides for hot blocks.

Disk, as estimates from the measured ratios plus the docids' computed cost (D1a), at 100 B:
today's document layout stores about 0.465 of the logical bytes (0.456 measured for the frames
and `keys`, plus about 1 B of monotone docids). Format 14 with LZ4 stores about 0.61 (0.567 plus
about 5 B of permuted docids), **31% more**; strata's measured 24-29% disk lead over RocksDB
(`core-vs-rocksdb.md:245-247`) puts RocksDB near 0.64, so that is a tie. With zstd 1 on merged
output about 0.52 (12% more than today, about 19% less than RocksDB), and about 0.49 with rank
ordinals below (6% more than today).

**Rank ordinals** (after step 1). In a key-ordered segment an ordinal is the key's rank, so the
termdict's per-key LEB128 ordinal (about 3 B at a million keys) is redundant: format 13's K2
mechanism stores one ordinal per restart run and counts entries through `find_run`
(`format-13.md:644-650`). It returns about 2.9 B per record, most of what permuted docids cost.

### D1e. The key dictionary and the fuse filter stay

`keys` is unchanged in step 1 (K1, `keys.hh:23-30`): the range, the binary fuse filter (0.39%
false positives at about 9 bits per key, `readmissing` 2.8× faster than RocksDB's Bloom filter,
`core-vs-rocksdb.md:74`), the termdict blocks, read in place from the mapping. A get probes
segments newest first by range and filter with one hash (`index.hh:1628-1638`); exactly one
segment holds a live copy, so a hit costs one termdict lookup and one value block. The key path
was under 2% of today's get (`core-vs-rocksdb.md:181-183`).

**The bytes** (format 14's first pre-freeze version, the series `format.hh:72-77` names):

```
stored     frames back to back, each [codec u8][payload]; the payload, decoded, is format 13's
           [count LEB][record lengths LEB][records]; then per frame [first ordinal u32]
           [raw size u32, bit 31 reserved for stored_spans (F19)][offset u64]; then the count u32
index_sort format 13's F18 section; a component whose slot id is 0xFFFFFFFF sorts by the key
           (direction 0, missing 1: keyless documents last, by docid)
```

## D2. A decoded-block cache, one per host

Prototype medians, a universe of 4 KiB blocks under YCSB's scrambled zipfian; "hot" prefills
32,768 blocks into room for 65,536 (hits only), "zipf" gives 200,000 blocks room for 65,536; each
operation includes the generator's draw:

| Mode | Threads | CLOCK Mops/s | LRU Mops/s | CLOCK ns/op | LRU ns/op | Hit rate CLOCK / LRU |
|---|---:|---:|---:|---:|---:|---|
| hot | 1 | 7.13 | 5.27 | 140 | 190 | 0.999 / 1.000 |
| hot | 4 | 22.87 | 10.69 | 175 | 374 | |
| hot | 8 | 41.56 | 12.13 | 192 | 659 | |
| hot | 16 | 67.41 | 14.94 | 237 | 1,071 | |
| zipf | 1 | 1.38 | 0.61 | 724 | 1,643 | 0.593 / 0.593 |
| zipf | 16 | 6.74 | 3.27 | 2,374 | 4,891 | |

The CLOCK cache serves 1.35× more hits per second on one thread and 4.5× more on sixteen, at the
same hit rate. **Decision:**

- **Structure.** A 16-way set-associative CLOCK table: per slot a 64-bit meta word (state,
  a two-bit countdown, a reference count), the key, and the value. A hit compares the key, takes a
  reference and raises the countdown in one compare-and-swap, and checks the key again under the
  reference; a release is one atomic subtraction. An insert takes a free way or sweeps only its
  own set, evicting an unreferenced entry whose countdown reached zero. No locks, no global hand,
  no probe chains. Its conflict misses measured 0.1% (hot, 0.999).
- **One per host.** The host creates it and passes it to every index (an option beside the budget,
  `index.hh:320-327`), so tens of thousands of open indexes share one capacity and an open index
  pays nothing for it. Keys are a process-unique number given to each segment when it opens (never
  reused) and the block's index; a closed segment's entries are never hit again and age out.
- **Charged to the node budget.** The cache's bytes are a permit on the node's `byte_budget`
  (`budget.hh:15-91`), so buffers and cached blocks share one capacity, as RocksDB charges its
  memtables to its cache (`rocksdb-mechanisms.md` 1.6). The cache is the budget's one reclaimable
  holder: when a buffer's draw would pass the capacity, the budget asks the cache to evict first,
  so a full cache never makes buffers flush early. That is an additive `byte_budget` hook.
- **Decoding without allocating.** Blocks are decoded into buffers from per-size-class free lists
  (4 KiB, then powers of two to 64 KiB) that eviction refills, with each record's offset parsed
  once into the same buffer; codec contexts are per thread and reused. Today a get allocates its
  frame and its offsets and frees both (`reader.hh:397-405, 476-487`), and `decompress_zstd`
  creates a zstd context per call (`compressor_zstd.cc:125-137` at compressors `ed590c8`; zstd's
  `ZSTD_decompress`). In isolation the context costs little (36.4 against 37.9 µs per 64 KiB get,
  `frame-ab.jsonl.gz`); the profile attributed its 43% `madvise` to freeing each get's buffer,
  which the prototype's fresh allocations did not reproduce in isolation, and reused buffers
  remove either way. Without a cache (a host may pass none), a get decodes into a buffer the
  index keeps.
- **Pinning.** A reference is a pin: a get holds its block while it copies the value or calls
  `get(key, f)` with a view; a scan's cursor holds one block per segment. Blocks a full scan reads
  enter with countdown 0, so a `readseq` does not flush the hot set.
- **Not cached:** blocks over 64 KiB (one record's), decoded into the caller's buffer.

The cache is lock-free code, so its step takes the concurrency tier's one design review before it
is built (the pipeline's review tiers); the format does not wait for it.

## D3. Key-ordered scans and the buffer's sorted view

**The API**, on `basic_index` for layouts with `keys` and `stored`:

```cpp
// The live records with keys >= from (and < to, when given), in key order, at most n (0: no
// limit); f(key, value, version) returns false to stop. Returns the count visited.
template <class F> std::size_t scan(std::string_view from, std::size_t n, F&& f) const;
template <class F> std::size_t scan(std::string_view from, std::string_view to, std::size_t n, F&& f) const;
// The same, pulled: valid until the index's next apply, refresh, flush, or merge publication.
class cursor { public: void seek(std::string_view from); bool valid() const; void next();
               std::string_view key() const; std::string_view value() const; std::uint64_t version() const; };
```

It merges one cursor per source, smallest key first: the buffer's sorted view, and each segment's
`keys` cursor joined to its value blocks through D2. A segment whose key range misses
`[from, to)` is skipped (format-13 F8, "Bounded scans", `:605-609`). Deleted copies are skipped by
the segment's live bits, dead buffered records by their flag. Since an index holds at most one
live copy per key (format-13 F8; `index.hh:1628-1638` finds the one), the merge needs no version
shadowing and no tombstones: the newest-wins logic of RocksDB's DB iterator
(`rocksdb-mechanisms.md` 3) has nothing to do here. The cursor's validity rule matches the owner
core's model (`ARCHITECTURE.md:43`): one writer and its readers on one core, so no apply runs
between two steps unless the caller yields; a debug build checks a mutation counter. Snapshot
cursors that survive applies come later, with the snapshot handles of `ARCHITECTURE.md:156-157`.

**The buffer's view.** Today's document-layout buffer keeps each document whole (two
`std::string`s) in a doubling vector, a docid map, and a version map of 16-byte slots at most half
full (`index.hh:198-246, 983-987, 2614-2621`): **494 B per buffered record at 100 B values, 1,924 B
at 1 KiB** (`bufmem.cc`, resident growth from 100,000 to 300,000 records; the search layout's was
683 B). Prototype medians at 1,000,000 records of 100 B (200,000 of 1 KiB alike, `report.md`),
every variant holding each record inline in one arena whose chunks grow from 4 KiB to 1 MiB, with
a 24-byte header (lengths, docid, version):

| Buffer | Resident B / record | Insert (stage 1's lookup included) | Get | Seek and 10 | YCSB E per op | Sort at flush per record |
|---|---:|---:|---:|---:|---:|---:|
| skiplist (RocksDB's memtable) | 162.8 | 1,745 ns | 2,042 ns | 3,060 ns | 7,261 ns | none |
| hash + B-tree of offsets | 182.0 | 2,691 ns | 248 ns | 2,841 ns | 4,265 ns | none |
| hash + sorted runs, binary counter | 174.6 | 436 ns | 253 ns | 1,502 ns | 8,047 ns | 571 ns |
| **hash + main run and 4,096-record delta** | **176.7** | **433 ns** | **244 ns** | **1,413 ns** | **5,541 ns** | **553 ns** |

**Decision: the arena log with the version map and a sort-on-demand view of two runs.** strata
resolves every keyed write in stage 1 (`rocksdb-survey.md:31-36`), so a write is a lookup plus an
append: the hash makes that 4.0× faster than a skiplist insert and a get 8.4× faster, and seeks
2.2× faster. A view is built when a scan or a flush asks: records appended since the last view are
sorted and merged into a delta of at most 4,096 records, which merges into the main run when full,
so a scan merges two runs. YCSB E, the scan-and-insert mix, is 1.3× faster than the skiplist and
1.3× slower than the B-tree; the flush pays the sort (553 ns per record, on the flush thread),
which a skiplist pays across its inserts instead (1,745 against 433 ns each).

- **Records.** `[key length][value length][docid][version][dead]` then the key and the value,
  inline in the arena; no `std::string` per record. Chunks start at 4 KiB and double to 1 MiB, so
  a small index's buffer costs little (PLAN's many-index rule), and are charged to the budget as
  they are taken (`budget.hh:93-130`, the flush charge's quantum).
- **The version map** keeps 8-byte slots (a 32-bit arena offset in 8-byte units, 32 GiB per
  buffer, and 32 bits of the hash) at most 7/8 full, in place of 16-byte slots at most half full.
  The docid map gets the same slots.
- **Memory against the 153 B target.** The prototype's 177 B carries a 24-byte header; varint
  lengths and version and a 4-byte docid offset from the buffer's base are about 16 B less, so the
  estimate is about 161 B, against RocksDB's 153 B, which carries an 8-byte sequence number where
  strata carries a docid and a version. T6 states the target; closing the last 8 B (a denser map
  or view) is that step's measurement, not settled here.
- **Overwrites** append a new record and mark the old one dead, as today's buffer does; the view
  skips dead records; the flush writes live ones.

## D4. Merges keep key order cheaply

`order_source<document_layout>` (the core's hook, `merge.hh:146-153`, called at
`index.hh:1324-1328`) returns the output order as a k-way merge of its sources' `keys` cursors:
each source is already key-ordered, so the merge is one heap step per record, no sort.

- **Inputs are read in order.** Each source's ordinals arrive ascending, so the stored merge reads
  every input sequentially through one decoded block each, the docid-order path of today's stored
  merge (`merge.hh:131-138`), not the spill it uses for a reordered output (`merge.hh:98-128`,
  `index.hh:1330-1333`). A `merge_context` flag says the inputs ascend.
- **No key map.** The keys merge takes each output key from the order's cursors instead of a map
  of every live key per source (`merge.hh:52, 81-91, 188-198`), which charges 64 B plus the key
  per live record to the merge's memory.
- **The output** is key-ordered with `index_sort`'s key component and permuted docids, cut into
  4 KiB blocks under Q2's codec.
- **Raw block copies** (after step 1): a source block whose records are all live and whose key
  range no other source's next key falls inside is appended compressed, as ARCHITECTURE item 11
  intends (`ARCHITECTURE.md:112-114`); its cut points need the writer to close the block in
  progress first.

What D4 does not fix: a merge under a tight budget still builds each output's filter, about 64 B
per key (`keys.hh:65-68`), so the 2× rows' merge wait (`core-vs-rocksdb.md:223-226`) needs the
survey's K4 policy, not this design.

## D5. prospect's search layout

Nothing in prospect's own sections changes: `inverted`, its terms, postings, positions, norms,
and lengths, and the search layout's docid (or reordered) ordinal order are untouched. What is
shared, because it is the core's:

1. **The stored frame's codec byte** is format 14's for every layout: a search-layout segment
   written by format 14 carries it. The block size is the writer's setting per layout: the search
   layout keeps 64 KiB zstd frames until prospect measures its stored fetches (top-k documents)
   with smaller blocks.
2. **The block cache** (D2) is available to prospect's stored reads; prospect decides whether to
   pass it.
3. **`index_sort`'s reader** (D1a) is the one prospect's slot sorts use (phase-4 D14).
4. The format version: one numbering for every layout (format.hh), so prospect pins the strata
   commit that bumps it and rebuilds its fixtures.

## Gate rows

Measured by the lead, on `main` commits, with the KV harness (`bench/kv/run.py`) against RocksDB
11.11.1 at its calibrated configuration (16 KiB blocks, LZ4, HyperClockCache), quick tier first,
then the `document` gate's Linux and FreeBSD rows (PLAN, `docs/PLAN.md:616-626`). Ratios are
strata's time over RocksDB's (or bytes over bytes); targets follow the gate's ≤ 1.0×.

| Row | Target | Procedure | Lands with |
|---|---|---|---|
| T1 Fixtures and identity | Document-layout fixtures re-pinned at the new pre-freeze version; a format-13 segment refused as `unsupported_segment`; the digest of `core_kv_test`'s seeded workload equal at format 13 and format 14 | `ctest -R 'core_kv|fixtures|docids'`; the digest printed by `core_kv_test` at both commits | Step 1 |
| T2 Decoder safety | The stored frame decoder (codec byte, LZ4 and zstd bounds, record offsets) and `index_sort`'s reader fuzzed one CPU-hour each at the gate commit, zero findings; corpora replayed in CI | `fuzz/` targets under `STRATA_FUZZ`, `-max_total_time=3600 -timeout=10` | Step 1 |
| T3 Key order checked | `check()` refuses a document-layout segment whose ordinals are not its keys' ranks; a writer mutant that keeps docid order is caught | `core_kv_test` with the mutant built in | Step 1 |
| T4 Gets | `readrandom`, YCSB B and C, 100 B and 1 KiB, fits and 2×, one thread: ≤ 1.0× RocksDB's time; CPU per get recorded | `python3 bench/kv/run.py rows OUT fits1 5`, then `twox1` | Step 1 (no cache) records it; step 3 (cache) must meet it |
| T5 Scans | `readseq`, `seekrandom`, YCSB E: ≤ 1.0× RocksDB's time | The same sets | Step 2 |
| T6 Buffer memory | Resident growth per buffered record ≤ 153 B at 100 B values (RocksDB's measured memtable cost) | `kv_proto_bufmem --value 100`, three trials, light hold | Step 4 |
| T7 Disk | Document-layout bytes after the settle ≤ 1.0× RocksDB's, and within 10% of format 13's under Q2's answer | The fits sets' disk column | Step 1 (LZ4) records it; Q2's preset must meet it |
| T8 Many indexes | The 1,000-index row: RSS after reopen ≤ format 13's, `readrandom` ≤ 1.0× RocksDB's time | `python3 bench/kv/run.py rows OUT many 3` | Steps 1 and 3 |

Each row's raw lines go to `docs/perf/` with the run's phase times, as `core-vs-rocksdb.md` does.

## Questions for the owner

**Q1. The shape of the value layout (D1b).**

1. Split (recommended): `keys` unchanged, the stored section key-ordered in 4 KiB blocks. One key
   dictionary for both layouts; gets within 12% and bytes within 3% of inline's; seeks 3×
   inline's until termdict's `cursor::seek` lands.
2. Inline: RocksDB's block table as a new core plugin for the document layout (keys and values in
   one block). One block per get and the fastest seeks and scans measured, at the cost of a second
   key structure beside `keys`, its own filter and index, and its own fuzz target.

**Q2. The codec per tier (D1d).**

1. LZ4 for every tier in step 1, and zstd 1 for merged output with the cache's step, together
   with rank ordinals (recommended): reads first; disk is about 31% above format 13's (a tie with
   RocksDB's) until then, and about 6% above after (estimates, D1d).
2. zstd 1 for merged output from step 1: disk about 12% above format 13's, but a get on merged
   data decodes zstd (4.8 µs at 4 KiB) until the cache lands, so `readrandom` after a settle would
   likely stay slower than RocksDB (an estimate from D1c).
3. LZ4 everywhere, for good: the fastest reads, and the disk lead over RocksDB given up.

## First buildable step

**Step 1: key-ordered document segments in 4 KiB codec-tagged blocks.** It needs only Q1's answer
(split assumed here; inline is a different step 1) and Q2's (option 1 assumed). Under PLAN's
format rule it needs no review before it is built, and lands on proof (fixtures, digest identity,
decoder fuzzing, a revert check, the lead's run-level benchmark) with one review of the decoder's
safety.

1. **compressors** (additive): one-shot block codecs into a caller's buffer with reused per-thread
   contexts (`decompress_zstd` today returns a new string and `ZSTD_decompress` creates a
   context per call: `compressor_zstd.cc:125-137`, zstd's `zstd_decompress.c:1203-1211`); an LZ4
   block codec; `COMPRESSORS_LZ4_PINNED`, fetching LZ4 v1.10.0, as zstd is pinned, so fixtures
   hold on every platform. strata turns `COMPRESSORS_LZ4` on (`CMakeLists.txt:17-19`).
2. **format.hh**: the pre-freeze version bump; the frame's codec byte; the stored block target as
   a writer setting (`builder_settings`), 4 KiB for the document layout, 64 KiB for the search
   layout; `index_sort`'s key component and F4's requiring rule for it.
3. **Writer and reader**: `stored_builder` cuts at the target and writes the codec byte
   (`writer.hh:227-255`); the reader decodes by codec into a buffer it is given, its offsets parsed
   into the same buffer (`reader.hh:397-487`); `index_sort`'s reader validates the key component.
4. **Flush**: the document layout's buffer sorts by key, keyless last by docid, permuted docids
   (`index.hh:2055-2080`); D18's runs by key range.
5. **Merge**: `order_source<document_layout>` (D4), the ascending-inputs flag, the keys merge
   without its key map.
6. **Get**: `basic_index::get` decodes into a buffer the index keeps (`index.hh:874-884`); an
   overload `get(key, f)` lends a view instead of returning a string.
7. **check()** verifies the key order (T3); a fuzz target for the frame decoder with a checked-in
   corpus (T2).

**Done criteria** (for the lead to set in the ledger; contributors tick, never edit):

- K1 T1: fixtures re-pinned, format-13 segments refused, digest identity at both commits.
- K2 T3: `check()` refuses a segment out of key order; the docid-order mutant fails the test.
- K3 Merges: a merged document-layout segment is key-ordered with every live record once, its
  digest equal to its inputs'; the merge takes the ascending path (no records scratch file) and
  builds no key map (a test on its charged memory).
- K4 Gets allocate nothing per call besides the returned string, and nothing with `get(key, f)`
  (the allocation-counting build).
- K5 T2's fuzz target and corpus in CI; ASan and UBSan clean; each fix's test fails with its fix
  reverted.
- K6 CI green on Linux, macOS, and FreeBSD (`cpp-libraries/tools/preflight.sh` first); compressors'
  change additive with its own CI green.
- K7 The lead's A/B on the branch (format 13 main against step 1, fits1 and many sets, under a
  hold): no row lost except the disk row Q2 accepts, as functional evidence; the gate rows are
  measured on `main` after it lands.

**Then**, each its own branch: step 2, D3's `scan` and cursor with the sorted view over today's
buffer (T5); step 3, D2's cache, after its concurrency design review, with Q2's merge codec and rank
ordinals (T4, T7, T8); step 4, D3's arena buffer (T6); raw block copies when the gate rows show
merge time matters.

## Shape for the owner

1. Document-layout segments become key-ordered: ordinal = key rank, recorded with F18's
   `index_sort`; docids stay as identity in the existing permuted kind; search keeps docid order.
2. Split shape (Q1): `keys` unchanged, values in 4 KiB blocks with a codec byte; inline measured
   11% faster on gets and 3× on seeks but needs a second key structure.
3. LZ4 by default (Q2): a miss decodes in 1.5 µs against 36 µs today; disk ties RocksDB until
   zstd 1 on merged output and rank ordinals return most of the lead.
4. One CLOCK block cache per host, lock-free, charged to the node budget as its reclaimable holder:
   4.5× an LRU's hits at 16 threads.
5. A core `scan(from, to, n)` and cursor merging the buffer and segments; no shadowing needed, one
   live copy per key.
6. The buffer becomes an arena log with a compact version map and a two-run sorted view: 177 B per
   record measured against 494 B today (RocksDB 153 B); inserts 4× and gets 8× a skiplist's.
7. Durability is the turn log's (durability-modes D1, step 1), not this design's.
8. Step 1 is the format: key order, 4 KiB LZ4 blocks, reused decode buffers, key-ordered merges.

## Risks

- **The prototypes are one platform and in RAM.** mmap faults, region checksums, and Linux's
  cheaper allocator and barriers change absolute numbers; the gate rows run Linux and FreeBSD
  (PLAN `:621-624`) and decide.
- **Permuted docids cost about 4 B per record more than monotone** (computed, D1a), about 7% of a
  zstd record at 100 B; rank ordinals return most of it.
- **A cache full of blocks and buffers on one budget** needs the reclaim hook to run on the
  drawing thread without stalling it; if eviction cannot keep up, buffers flush earlier, which the
  2× rows will show.
- **The two-run view degrades with a large buffer under scan-and-insert mixes** (YCSB E 5.5 µs per
  op at a million buffered records, B-tree 4.3 µs); the many-index workload's small buffers are
  the common case, and the view's delta size is a measured knob.
