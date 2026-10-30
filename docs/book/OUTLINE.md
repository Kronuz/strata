> Status: DRAFT (2026-10-27) skeleton of the "Architecture and Design" book, built from strata main `eb87572`, prospect main, the Kronuz libraries' mains, Xapiand master `8170a3ef6`, and the design and phase branches named below. Verify a source against its branch before a chapter relies on it.

# Architecture and Design: the book outline

The book explains how strata (the segment store), prospect (the search layout), the Kronuz
libraries under them, and Xapiand-on-strata work. Each section below is one how-it-works note
(house style: `gated-pipeline` `references/house-style.md`, "How-it-works note"; principle 10).
This file maps sections to the notes, designs, reviews, perf records, and commits a writer
starts from, and lists the notes still missing.

## How to read this file

Source paths:

| Prefix | Means |
|---|---|
| `S:` | strata `docs/` on main, unless an `@branch` follows |
| `P:` | prospect `docs/` on main |
| `X:` | Xapiand `docs/` on master |
| `lib:` | the library's own repository under `~/code/Kronuz/<lib>` |
| `@<branch>` | the file lives only on that branch (`origin/<branch>` in strata); main does not have it yet (none left after the 2026-10-30 branch triage) |

Strata documents that lived only on a branch. The 2026-10-30 triage (`docs/status/branch-triage-2026-10-30.md`)
landed them on main under the same paths, each with a status line; the table is the map from the branch to
those paths. Only `sim/`, `research/*/` and other prototype code stay on their branches:

| Branch (`origin/...`) | Files (now on main) |
|---|---|
| `design/write-modes` | `design/write-buffer.md`, `reviews/write-buffer-design.md` |
| `design/shared-reader` | `design/shared-reader.md`, `reviews/shared-reader-design.md` |
| `design/queryparser` | `design/queryparser.md`, `reviews/queryparser-step1.md`, `status/queryparser.md` |
| `design/cold-tier` | `design/cold-tier.md`, `reviews/cold-tier-design.md`, `status/cold-tier.md` |
| `design/hybrid` | `design/hybrid-search.md`, `design/hybrid-search/`, `status/hybrid-search.md` |
| `design/landscape` | `design/product-landscape.md` |
| `design/query-gap` | `design/query-gap.md`, `reviews/query-gap-design.md` |
| `design/peer-transport` | `design/peer-transport.md`, `reviews/peer-transport-design.md`, `reviews/peer-transport-security.md` |
| `design/simulator` | `design/simulator.md`, `reviews/simulator-design.md`, `status/simulator.md`, `sim/` |
| `design/durability-modes` | `design/durability-modes.md`, `reviews/durability-modes-*.md`, `status/durability-modes.md`, `reviews/inverter-*.md`, `status/inverter*.md` and `gates/phase-3-ratchets.md` (in prospect main since the repository split; cite them as `P:`) |
| `phase/cold-tier-step1` | `mechanisms/segment-records.md`, `test/cold_segment_test.cc` |
| `research/indexing` | `research/indexing.md` |
| `phase5/automata` | `design/automata.md` |

Commits are `repo:hash`. A merge hash is the landing on main; a plain hash is the commit that
introduced the mechanism.

Note states:

| State | Meaning |
|---|---|
| EXISTS | a note in `docs/mechanisms/` in the format of the house style; all three that exist are on unmerged branches |
| COVERED | no mechanism note, but an ARCHITECTURE, README, or design doc already explains it in present tense; a note can be a short extract |
| MISSING | nothing explains how it works except the code, the design (decisions, not mechanism), and the commits |
| RECORDS | the section is evidence (gates, A/Bs, surveys); the chapter cites the records and needs no mechanism note |

Where a note is missing, the line names the file to create, in the repository that owns the
mechanism's core (house style: a mechanism that spans repositories lives with its core and is
linked from the others). `docs/mechanisms/` does not exist on strata main, prospect main, or any
library main; the only notes are on the three branches listed in "State of the notes".

## State of the notes

| Note | Repository, branch, commit | Chapter |
|---|---|---|
| `docs/mechanisms/segment-records.md` | strata `phase/cold-tier-step1`, strata:65ca8cf | 6.4 |
| `docs/mechanisms/engine-write-path.md` | Xapiand `phase0.5/seam-write`, Xapiand:fba3e3a53 | 10.2 |
| `docs/mechanisms/engine-census.md` | Xapiand `phase0.5/census`, Xapiand:cdfdba558 | 10.3 |

The count by state is at the end of the file.

---

## 1. Orientation: layers, libraries, runtime

Owners: strata, prospect, `task`, `reactor`, `stash`. Answers: what is each piece, and what may
depend on what.

### 1.1 Layers and the ownership rule
- Explains: Xapiand over `strata::group` over prospect over the strata core over the building blocks; the core never names a search type; what each layer owns.
- Sources: `S:ARCHITECTURE.md` "Layers", `S:design/core-split.md`, `S:strata.d2`, `P:ARCHITECTURE.md` "On strata's core".
- Commits: strata:3f88a15 (core split), strata:426cc59 (repo split, search moves to prospect).
- Note: COVERED (`S:ARCHITECTURE.md`, `P:ARCHITECTURE.md`).

### 1.2 One implementation per mechanism, presets
- Explains: compatibility with Xapian, Lucene, Tantivy, RocksDB is a parameter or preset, never a second code path; the default is the preset measured fastest.
- Sources: `S:ARCHITECTURE.md`, `P:ARCHITECTURE.md`, `P:perf/bm25-presets.md`, `S:design/rocksdb-survey.md`.
- Commits: none specific (a rule applied across the code).
- Note: COVERED (the two ARCHITECTURE files).

### 1.3 The library catalog and its rules
- Explains: which Kronuz libraries the engine uses and why each earns its place; check the catalog before writing; migrate callers, no shims; which libraries stay in the host.
- Sources: `S:ARCHITECTURE.md` "Libraries", `~/code/Kronuz/cpp-libraries`, `X:design/extraction.md`, `X:dev/extraction-ledger.md`, `X:design/scheduler.md`.
- Commits: Xapiand:d21a0209b (extraction ledger).
- Note: COVERED (`S:ARCHITECTURE.md` "Libraries").

### 1.4 The executor concept and the pool executor
- Explains: `post`/`now` as the whole scheduler contract; how the core runs on the host's run queue, on a pool, or on a stepping test executor; where a stopped pool hangs.
- Sources: `include/strata/core/executor.hh`, `pool_executor.hh`, `test/stepping_executor.hh`, `S:ARCHITECTURE.md` "Concurrency and async", `S:status/reclaim-bg.md`.
- Commits: strata:7110f1a (only-worker hang beside the stopped pool).
- Note: MISSING -> strata `docs/mechanisms/executor.md`.

### 1.5 `task<T>`, budgets, and cancellation
- Explains: symmetric transfer, awaiter-executor resume, per-core frame pool, yield points. The least-attained-service scheduler and the 100-250 us slice are stated as design in ARCHITECTURE; confirm each against code before writing.
- Sources: `lib:task` README and `ARCHITECTURE.md`, `lib:task` `docs/reviews/`, `S:ARCHITECTURE.md` "Concurrency and async".
- Commits: none recorded here (task: 11 commits; start at `git -C task log`).
- Note: COVERED (`lib:task` `ARCHITECTURE.md`).

### 1.6 The reactor: shared-nothing loops, offload, shutdown
- Explains: N reactors bound to one port, a connection's life, offload to a worker pool, abort-stop-join order, the UDP path, the `inside_run` marker the offload guard reads.
- Sources: `lib:reactor` `ARCHITECTURE.md`, `docs/API.md`, `docs/reviews/`.
- Commits: reactor:d381e4d (thread-local `inside_run` marker), reactor:0c04d0d (UDP handler runs inside it).
- Note: COVERED (`lib:reactor` `ARCHITECTURE.md`).

### 1.7 stash: the lock-free slot store
- Explains: `Stash<T,Size>`, `StashSlots`, `StashValues`, the key window, walk/peep/clean traversal, concurrency model; the debouncer stack built on it.
- Sources: `lib:stash` `ARCHITECTURE.md`, `docs/API.md`, `docs/perf/window-epoch.md`, `X:design/scheduler.md`.
- Commits: Xapiand:903ad2350 (repin to the fail-fast commits), Xapiand:4b185b94a (`debounce_or_run`: past horizon is an error).
- Note: COVERED (`lib:stash` `ARCHITECTURE.md`).

### 1.8 Where this sits in the field
- Explains: the product landscape, table stakes, differentiators, what Xapiand's successor drops or changes; the owner's answers.
- Sources: `S:design/product-landscape.md` (strata:65566b2), `S:FEASIBILITY.md`, `S:PLAN.md`.
- Commits: strata:665677b (feasibility, architecture, plan).
- Note: RECORDS.

---

## 2. Storage and formats

Owners: strata core (frame, keys, stored), prospect (search sections), `bitpack`, `fst`,
`termdict`, `fusefilter`, `bkd`, `docset`, `varint`, `crc32c`, `compressors`, `endian`.

### 2.1 Layouts and plugins: how the core folds over sections
- Explains: a layout names plugins; the core calls their static hooks (`builder`, `inverter`, `records`, `buffered`, `total_length`, `length`, `consistent`, `refuse`, `report`, `check`); the plugin set in the manifest; `order_source` for merge order.
- Sources: `S:design/layers.md` L2-L4, L16, `S:design/core-split.md`, `P:ARCHITECTURE.md` "On strata's core", `include/strata/core/index.hh`, `check.hh`, `cmake/check_core_includes.cmake`, `S:reviews/layers-design.md`, `S:reviews/layers-code.md`.
- Commits: strata:ff5392e (format 13 merged into layers and the inverter), strata:3f88a15, strata:d3f9a0c (landing review of layers revision 4).
- Note: MISSING -> strata `docs/mechanisms/layout-and-plugins.md`.

### 2.2 The segment file: sections, table of contents, footer
- Explains: one file of sections, a TOC, and a footer with the format version and CRC32C; one numbering space for section kinds; header and footer layout; file order.
- Sources: `S:ARCHITECTURE.md` "Segment format", `S:design/format-13.md` F3, F5, F6, `include/strata/core/format.hh`, `section_io.hh`, `S:reviews/format-13-code.md`.
- Commits: strata:ff5392e, strata:77751a0 (segment id as physical identity, F13-R41, branch commit).
- Note: MISSING -> strata `docs/mechanisms/segment-frame.md`.

### 2.3 Format versions, the freeze, compatibility
- Explains: what format 13 froze, structure flags, manifest version 2, why formats stay open until first release, how a reader refuses a version it does not know.
- Sources: `S:design/format-13.md` F1, F2, F4, F20, `S:reviews/format-13-design.md`, `format-13-merge.md`, `format-13-mutants.md`, `P:perf/format13-landing.md`, `P:perf/format13-gate-vm.md`.
- Commits: strata:ff5392e, strata:e3222eb (inverter follow-ups under format 13), strata:831459e (bit 16 reserved for a clock per version).
- Note: MISSING -> strata `docs/mechanisms/format-versions.md`.

### 2.4 The key dictionary and the primary-key filter
- Explains: sorted keys in prefix-compressed `termdict` blocks plus a binary fuse filter that answers "absent" for most lookups; how a get, a replace, and a delete resolve a key.
- Sources: `S:design/format-13.md` F8, `include/strata/core/keys.hh`, `test/key_index_test.cc`, `test/keys_fuse_test.cc`, `lib:termdict` README, `lib:fusefilter` README.
- Commits: strata:6105148 (fuse-fence merge), strata:310ec4d (hash takeover), fusefilter:b4f4ce1 (initial filter).
- Note: MISSING -> strata `docs/mechanisms/key-dictionary.md`.

### 2.5 Stored records
- Explains: zstd blocks with an offset index, the minimum legal section size, borrowing records at flush, copying blocks unchanged in a merge when none is deleted.
- Sources: `S:design/format-13.md` F19, `S:design/format-14-kv.md` D1, `S:status/stored-short-section.md`, `fuzz/fuzz_stored.cc`, `lib:compressors` `ARCHITECTURE.md`.
- Commits: strata:75716f0 (SR2 test and fuzz mode), strata:a199e6b (refuse stored sections under 21 bytes), strata:c6d7bf6 (stream the stored section in a merge).
- Note: MISSING -> strata `docs/mechanisms/stored-records.md`.

### 2.6 Format 14: key-ordered blocks, the document layout as a key-value store
- Explains: 4 KiB codec-tagged blocks in key order, the decoded-block cache, key-ordered scans over the buffer's sorted view, merges that keep key order.
- Sources: `S:design/format-14-kv.md` D1-D4, `S:design/format-14-kv/report.md`, `S:status/f14-step1.md`, `S:status/kv-scan.md`, `S:status/compact-slots.md`, `S:reviews/kv-scan.md`.
- Commits: strata:4d36aba (design), strata:75d1e7e (step 1), strata:89f9fd7 (`basic_index::scan`), strata:8fcd4e3 (8-byte map slots).
- Note: MISSING -> strata `docs/mechanisms/key-ordered-blocks.md`. Scan and compact slots landed through perf work with ledgers only, no design.

### 2.7 Docids, ordinals, and the docid map
- Explains: per-shard monotonic docids never reused; segment-local ordinals; the docid map as identity in flushed segments and a permutation in reordered merges; `lastdocid` in the manifest.
- Sources: `S:ARCHITECTURE.md` "Data model", `S:design/reordering.md` D1, `include/strata/core/docids.hh`.
- Commits: strata:7a4afca (`reorder_threads`, same order at any count).
- Note: MISSING -> strata `docs/mechanisms/docids-and-ordinals.md`.

### 2.8 Live documents: copy-on-write deleted bits
- Explains: per-generation live-docs side files, copy on write, the delete count that sizes them, how readers and the writer share one segment read path.
- Sources: `include/strata/core/deleted_bits.hh`, `S:design/phase-3.md` "Files", `S:ARCHITECTURE.md` "Write path" item 5.
- Commits: strata:302a414 (copy live bits on write; one segment read path).
- Note: MISSING -> strata `docs/mechanisms/deleted-bits.md`.

### 2.9 Block codecs: FOR, PFOR, the four-lane layout
- Explains: 128-value blocks, fixed 4x32-bit interleaved lanes, SSE4.1/NEON/AVX2 decode with scalar fallback, the no-wdf and all-ones flags, LEB128 tails through `varint`.
- Sources: `lib:bitpack` `ARCHITECTURE.md`, `lib:varint` `ARCHITECTURE.md`, `P:ARCHITECTURE.md` "Codec rules".
- Commits: none recorded here.
- Note: COVERED (`lib:bitpack`, `lib:varint` `ARCHITECTURE.md`).

### 2.10 Postings, positions, term blocks, and skip data
- Explains: 128-ordinal posting blocks, two skip levels with competitive (wdf, doclen) pairs, term blocks of 25-48 terms, the term index FST, df-1 terms in the block, variable block-max partitions.
- Sources: `P:ARCHITECTURE.md` "The search sections", `S:design/phase-2.md`, `S:design/format-13.md` F10-F14, `P:perf/p12-term-dictionary.md`, `P:perf/format13-query-items.md`.
- Commits: prospect:7b08a4c (format 13 merged), prospect:aa532de (f13 gate).
- Note: MISSING -> prospect `docs/mechanisms/postings-layout.md`.

### 2.11 The term dictionary: block tree and seek
- Explains: block-tree layout on `fst`, in-block forward seek and its gate, decode of the last key from run headers, validation at open.
- Sources: `lib:termdict` README, `lib:termdict` `docs/perf/forward-seek.md`, `docs/perf/seek-decode.md`, `docs/status/`, `docs/reviews/`, `P:perf/p12-term-dictionary.md`.
- Commits: termdict:cd5c733 (forward seek in the open block), termdict:74d518a (merge), termdict:175693a (corrupt on a malformed FST at open), termdict:8b1e964 (merge).
- Note: MISSING -> termdict `docs/mechanisms/block-tree.md`.

### 2.12 fst: minimal acyclic transducers
- Explains: sorted-input builder, `get`, `floor`, range streams, automaton intersection; the structural pass at open.
- Sources: `lib:fst` `ARCHITECTURE.md`, README.
- Commits: none recorded here (9 commits).
- Note: COVERED (`lib:fst` `ARCHITECTURE.md`).

### 2.13 Doc values and sparse slots
- Explains: slot directory, numeric columns (delta, GCD, table, bit-packed), sorted-set ordinals, binary lists, per-column bounds.
- Sources: `P:design/phase-4.md`, `P:ARCHITECTURE.md` "The search sections", `S:ARCHITECTURE.md` "Data model", `S:design/format-13.md` F15.
- Commits: none recorded here.
- Note: MISSING -> prospect `docs/mechanisms/doc-values.md`.

### 2.14 Points: the BKD tree and docsets
- Explains: 1-D numeric and 2-D interval BKD trees; dense, sparse, and chunked docsets; filter-cache bitsets.
- Sources: `lib:bkd` README, `lib:docset` `ARCHITECTURE.md`, `S:design/format-13.md` F16, `P:design/phase-4.md`.
- Commits: none recorded here (bkd: 2 commits; docset: 8).
- Note: COVERED for docset (`lib:docset` `ARCHITECTURE.md`); MISSING for bkd -> bkd `docs/mechanisms/bkd-tree.md`.

### 2.15 Small codec libraries: crc32c, varint, endian, hashes
- Explains: the one CRC-32C implementation (hardware), the length-prefixed integer format shared with Xapiand's `serialise_length`, on-disk byte order, wordwise hashes and the 32-bit fold fix.
- Sources: `lib:crc32c`, `lib:varint`, `lib:endian`, `lib:hashes` (`ARCHITECTURE.md`, `docs/BENCHMARKS.md`), `S:ARCHITECTURE.md` "Libraries" footnote on `length`.
- Commits: hashes:975492a (`hash32`: the high half, not the xor-fold), strata:28dbdea (CRC-32C from its own leaf).
- Note: COVERED (each library's `ARCHITECTURE.md`).

---

## 3. Write path

Owners: strata core (`index.hh`, `write_buffer.hh`, `writer.hh`, `owner.hh`, `budget.hh`),
prospect (inverter), `analysis`, `fusefilter`, `termdict`.

### 3.1 The owner core and the write lease
- Explains: one writer per shard on one core; operations as messages with total order at admission; the lease token as the primary owner check; handoff.
- Sources: `S:ARCHITECTURE.md` "Write path" 1, `include/strata/core/owner.hh`, `S:design/shared-reader.md` SR-D19, SR-D20, `S:status/write-buffer.md`.
- Commits: strata:0341c4b (write lease), strata:631903a (owner.hh builds without lease checks), strata:1999bdc (no owed chore is lost at a writer's handoff).
- Note: MISSING -> strata `docs/mechanisms/owner-and-lease.md`.

### 3.2 Prepare and stage 1: docids, versions, the primary-key map
- Explains: analysis on the receiving core, the prepared document, deterministic sequential stage 1, expected-version checks against the live map then the segments, the three counters (revision, seqno, version).
- Sources: `S:ARCHITECTURE.md` "Write path" 2-3 and "Three counters", `S:design/layers.md` L10, `S:design/write-buffer.md` D4, `S:REPLICATION.md` "Determinism contract".
- Commits: strata:2ec8c36 (the tuple inverter on layers' plugins; stage boundary).
- Note: MISSING -> strata `docs/mechanisms/stage-one.md`.

### 3.3 Stage 2: the tuple inverter
- Explains: inversion into the buffer, the tuple stream, term map with arena bytes, posting slices; why one-thread throughput is the metric.
- Sources: `S:research/indexing.md` (strata:e5c3ddc), `P:perf/inverter.md`, `P:ARCHITECTURE.md` (`prospect/inverter.hh`, `tuple_buffer.hh`), `S:reviews/inverter-code.md`, `inverter-followups.md`, `inverter-main.md`.
- Commits: strata:2ec8c36, strata:e3222eb, strata:69a3f8c (inverter's last fixes on main).
- Note: MISSING -> prospect `docs/mechanisms/inverter.md`.

### 3.4 The write buffer: lanes, records, version map, freeze
- Explains: lanes of records in chunks addressed by 32-bit references, the version map readable while the lease holder writes, check-in, tombstones, freeze, the byte layout of a record (flags at 24, fields at 28), and the fallback (one writer, live readers).
- Sources: `S:design/write-buffer.md` D1-D14 (revision 4, strata:8cdbe61), `include/strata/core/write_buffer.hh`, `S:status/write-buffer.md`, `S:status/write-buffer-reads.md`, `S:reviews/write-buffer-step1-code.md`, `write-buffer-reads-code.md`, `S:perf/exploration-log.md` (bake-off, T3, T11 rows).
- Commits: strata:356022d (step 1 landed), strata:74f122c (lanes, records, freeze, check-in), strata:a2d940a (D3 bytes), strata:39c9cc1 (follow-ups CR7-CR9), strata:3f9ed0d (refuse a transaction the lane cannot hold).
- Note: MISSING -> strata `docs/mechanisms/write-buffer.md`. Top priority: the design is 1,071 lines of decisions and four revisions; no page says how a record gets in and out.

### 3.5 What readers see: V, views, and reads at r
- Explains: the visible position V, views that hold buffers, reads at max(V, view), docid checks, lease-only docid maps, generation-retired tables.
- Sources: `S:design/write-buffer.md` D5, D8, D10, `S:status/write-buffer-reads.md`, `S:reviews/write-buffer-reads-code.md`.
- Commits: strata:2939b16 (V, views holding buffers, reads at r), strata:2f56d64 (landed), strata:3becd09 (keep a rejected transaction's records while readers exist).
- Note: MISSING -> strata `docs/mechanisms/buffer-visibility.md`.

### 3.6 Scans over the buffer: the binary counter of runs
- Explains: 12-byte position entries, ratio-8 binary counter, lease-applied splices retired by D10's stamp, prefix-first merge, select-32 tail; one mechanism for owner and views.
- Sources: `S:design/write-buffer.md` D9, `S:perf/exploration-log.md` (T3 and D9 rows), `docs/perf/scan-lazy-merge.md` (landed as strata:89f9fd7) and `docs/perf/write-buffer-bakeoff.md`, `test/write_buffer_test.cc`.
- Commits: strata:3801922 (item 4, C's scan), strata:f73d008 (trivial position-index entries), strata:c27307e (16-entry first block).
- Note: MISSING -> strata `docs/mechanisms/buffer-scan.md`.

### 3.7 Deletes and replaces across segment copies
- Explains: positional order inside the buffer; pre-resolution against older segments through the key filter and a term-block lookup; delete-by-docid of segment copies; docid-prev on replace records.
- Sources: `S:design/write-buffer.md` D6 and "Changes in revision 3, 4", `S:ARCHITECTURE.md` "Write path" 5, `S:reviews/write-buffer-design.md` (WB-20 to WB-24).
- Commits: strata:356022d, strata:2f56d64.
- Note: MISSING -> strata `docs/mechanisms/buffer-deletes.md`.

### 3.8 Transactions, batches, and rollback
- Explains: a transaction as an atomic batch; cancel undoes buffered operations; stage 1 failure rolls back; each undo step is logged before the change it undoes can fail.
- Sources: `test/apply_atomic_test.cc`, `S:perf/exploration-log.md` (CT-R5 rows), `S:status/write-buffer.md` (CR9).
- Commits: strata:9ee1277 (roll back a transaction whose stage 1 throws, CT-R5), strata:d3a59ee (log each undo step first, CT-R5a), strata:eb87572 (landed).
- Note: MISSING -> strata `docs/mechanisms/transactions.md`.

### 3.9 The indexing budget and pressure flush
- Explains: one node-wide budget, largest buffer flushes first, why the pressure flush gates at a quarter of the share when merges hold memory, the flush-memory bound that tests tie to filter scratch.
- Sources: `include/strata/core/budget.hh`, `S:design/phase-3.md` "Memory budget (D17, D20)", `S:status/pressure-flush.md`, `S:status/flush-memory-bound.md`, `S:perf/peak-memory.md`.
- Commits: strata:5737867 (pressure flush), strata:fb583d8 (flush-memory bound), strata:5685428 (flush-memory merge).
- Note: MISSING -> strata `docs/mechanisms/indexing-budget.md`. Landed through perf work; ledger only, no design.

### 3.10 Flush: sized up front, borrowed, streamed
- Explains: the writer is sized before it fills, stored records are borrowed instead of copied, every run is streamed; the second copy of each value and the doubling output are gone.
- Sources: `include/strata/core/borrow.hh`, `S:status/flush-memory.md`, `S:perf/flushmem.md`, `S:perf/rss-transient.md`, `S:perf/peak-memory.md`.
- Commits: strata:2331446 (size up front, borrow, stream), strata:5685428, strata:965339c (flushmem merge).
- Note: MISSING -> strata `docs/mechanisms/flush.md`.

### 3.11 The fuse filter: build, hash takeover, grow on failure
- Explains: binary fuse construction over sorted 64-bit key hashes; `build(vector&&)` takes the hashes over instead of copying; the table grows after 100 failed seeds (near some key counts no seed peels); `filter8_view` answers from serialized bytes in place.
- Sources: `lib:fusefilter` README and `fusefilter.d2`, `S:perf/fuse-fence.md`, `S:perf/flushmem.md`, `S:status/flushmem.md`.
- Commits: fusefilter:755b392 (`build(vector&&)`), fusefilter:a42bd15 (merge), fusefilter:e9eadcc (grow when no seed peels it), fusefilter:28f4241 (`filter8_view`), strata:310ec4d (strata side), strata:d4a8d75 (free versions before the build), strata:6105148 (fuse-fence merge).
- Note: MISSING -> fusefilter `docs/mechanisms/build.md` (known gap, "fuse filter hash takeover").

### 3.12 termdict block streaming
- Explains: a builder writes each block to a sink as it closes, so the dictionary's bytes never sit whole in memory; how the merge consumes the sink.
- Sources: `lib:termdict` README, `lib:termdict` `termdict.hh` (`builder(alphabet, sink)`), `S:perf/flushmem.md`, `S:perf/merge-stream.md`.
- Commits: termdict:8367263 (write blocks to a sink as each closes), termdict:cdb8042 (merge), strata:310ec4d, strata:1e81219 (pin fusefilter and termdict).
- Note: MISSING -> termdict `docs/mechanisms/block-streaming.md` (known gap).

---

## 4. Segments, merges, reclaim

Owners: strata core (`merge.hh`, `merge_policy.hh`, `files.hh`, `rate_limiter.hh`, `hazard.hh`,
`borrow.hh`), prospect (inverted merge, reorder).

### 4.1 The merge policy: tiers, rate limit, dead ratio
- Explains: tiered selection, the rate limiter, presets, the 20% dead-ratio default and why it stays, what the open "merge policy presets" row asks.
- Sources: `include/strata/core/merge_policy.hh`, `rate_limiter.hh`, `S:design/phase-3.md` "Merges (D11)", `S:design/limits.md`, `S:perf/deadratio.md`, `S:reviews/background-merges.md`, `test/merge_policy_test.cc`.
- Commits: strata:2451555 (record the dead-ratio experiment; keep the 20% default).
- Note: MISSING -> strata `docs/mechanisms/merge-policy.md`.

### 4.2 Background merges and the merge that never sleeps
- Explains: merges run off the owner on the host's pool; a merge does not wait for the writer's budget; failure and fencing of a damaged source.
- Sources: `S:reviews/background-merges.md`, `S:status/merge-wait.md`, `test/background_merge_test.cc`, `S:perf/exploration-log.md` (SR4 row).
- Commits: strata:6e13416 (merge-wait), strata:816158a (SR4: a merge over a damaged source fences the index).
- Note: MISSING -> strata `docs/mechanisms/background-merge.md`.

### 4.3 Streaming merges
- Explains: stored section copied block by block, merge plan without the docid sort for an ordered merge, postings re-encoded block by block, the working-memory ceiling.
- Sources: `S:perf/merge-stream.md`, `S:status/merge-stream.md`, `S:reviews/merge-stream.md`, `S:perf/flushmem.md`, `include/strata/core/merge.hh`, `P:ARCHITECTURE.md` (`prospect/merge.hh`).
- Commits: strata:c6d7bf6 (stream the stored section), strata:118c9b4 (merge plan without the docid sort), strata:87be659 (sanitizer runs), strata:6589f32 (merge-stream merge), strata:310ec4d.
- Note: MISSING -> strata `docs/mechanisms/streaming-merge.md`.

### 4.4 Reordering at merge: recursive graph bisection
- Explains: choosing a docid order that shrinks postings and speeds queries; the permuted docid map; the shared thread pool and the "same order at any thread count" rule; deletes in a reordering merge.
- Sources: `S:design/reordering.md` D1-D6, `S:reviews/reordering-design.md`, `prospect/bisection.hh`, `prospect/reorder.hh`, `S:gates/phase-3-ratchets.md` (reordering rows; strata:8123b71, strata:b40549e).
- Commits: strata:7a4afca (`reorder_threads`), strata:adb329e (join worker threads by hand), strata:8123b71 (about 20% off query times), strata:950d24d (crash points with reordering merges).
- Note: MISSING -> prospect `docs/mechanisms/reordering.md`.

### 4.5 Reclamation: deletes, truncation in steps, the backlog
- Explains: obsolete files are deleted only when no snapshot, manifest, or checkpoint references them; large files are truncated in steps (7-130 ms per GiB to free, by filesystem); `commit()` reclaims at most a bounded amount, so a backlog waits for the host's commits.
- Sources: `S:perf/reclamation.md`, `S:status/reclaim-backlog.md`, `S:reviews/reclaim-backlog.md`, `include/strata/core/files.hh`, `index.hh` (`reclaim`), `S:perf/exploration-log.md` (64 KiB overwrite and 10x RAM disk rows), `S:perf/remeasure-2026-10-26.md`.
- Commits: strata:b3dcc48 (reclaim-backlog merge), strata:da758af (re-measure after both reclaim fixes).
- Note: MISSING -> strata `docs/mechanisms/reclamation.md`.

### 4.6 Background deletes
- Explains: reclaim's file deletes are posted to the host's executor with owner-side accounting, a failure channel, a waiting drain, and close; a delete that cannot be posted is requeued; what a host that stops its pool first must do.
- Sources: `S:perf/reclaim-bg.md`, `S:perf/reclaim-bg-ab.sh`, `S:status/reclaim-bg.md`, `S:reviews/reclaim-bg-code.md`, `test/reclaim_bg_test.cc`.
- Commits: strata:2123803 (post reclaim's deletes to the host's executor), strata:4ce9118 (requeue a delete that cannot be posted), strata:45e6b32 (landed), strata:83ebe40 (Linux x86_64 numbers).
- Note: MISSING -> strata `docs/mechanisms/background-deletes.md` (known gap).

### 4.7 Hazard-pointer reclamation of retired snapshots
- Explains: a registry of one-word records on separate cache lines; a reader claims a record by compare-exchange, then validates; the owner stores the new pointer, retires the old, scans; why the store-load pair is sequentially consistent and no fence is used; one registry per pool partition.
- Sources: `include/strata/core/hazard.hh` (header comment), `test/hazard_test.cc`, `test/hazard_litmus_test.cc`, `S:design/shared-reader.md` D4-D6, SR-D8, SR-D17, SR-D18, `S:reviews/shared-reader-core.md`, `S:perf/hazard-flake.md`.
- Commits: strata:6373595 (the hazard registry), strata:c576997 (shared-reader-core merge), strata:cdd6f02 (fix the test releaser exiting with guards queued), strata:1c1f23d (hazard-flake merge).
- Note: MISSING -> strata `docs/mechanisms/hazard-pointers.md` (known gap).

### 4.8 Snapshots, views, retention, and close
- Explains: a per-core snapshot as a set of segments and live docs that never change under a query; views and readers on other threads; manifest retention (default 0); closing with readers outstanding.
- Sources: `S:design/shared-reader.md`, `S:status/shared-reader-views.md`, `S:reviews/shared-reader-views-code.md`, `S:reviews/retention.md`, `S:perf/retention.md`, `test/shared_view_test.cc`, `shared_read_test.cc`, `retention_test.cc`.
- Commits: strata:0341c4b (views, retention, close, write lease), strata:23e78bb (landed), strata:0156b57 (retention default 0), strata:c485ef5 (CR1-CR4).
- Note: MISSING -> strata `docs/mechanisms/snapshots-and-views.md`.

### 4.9 Checkpoints, install, digests, `check()`
- Explains: a checkpoint installed into another index; install reopens segments from the target's own files so a source's reclaim cannot truncate a mapped file; error handling that keeps EMFILE and ENOMEM unfenced; digests and the check report.
- Sources: `S:status/install-own-files.md`, `S:reviews/install-own-files-code.md`, `S:reviews/install-architect.md`, `S:REPLICATION.md` "Checkpoints and repair", `include/strata/core/check.hh`, `test/install_errors_test.cc`, `install_own_test.cc`.
- Commits: strata:17e8b3c (install from the target's own files), strata:53c088d (landed), strata:ef48ed0 (install error handling, C1-C5).
- Note: MISSING -> strata `docs/mechanisms/install-and-checkpoint.md`.

---

## 5. Durability and recovery

Owners: strata core (`files.hh`, `faultfs.hh`), `completion` (the I/O contract), `journal`.

### 5.1 The commit protocol and its barriers
- Explains: segment fsync, live-docs fsync, manifest write and fsync, directory fsync (`F_FULLFSYNC` on macOS); revision increments only when something changed; one device flush for a commit's data files; why a fresh index create costs four full syncs.
- Sources: `S:ARCHITECTURE.md` "Manifest and commit", `S:design/phase-3.md` "Commit and recovery", `S:perf/commit-barriers.md`, `S:perf/exploration-log.md` (open-cost row), `S:gates/phase-3-lifecycle.md`.
- Commits: strata:b093ded (explore/open-cost, branch commit).
- Note: MISSING -> strata `docs/mechanisms/commit-protocol.md`.

### 5.2 Manifests, recovery, and fail-closed rules
- Explains: `manifest-N` contents; standalone recovery opens the highest valid manifest, under groups the one the journal checkpoint names; no silent fallback; what retention of predecessors buys and costs.
- Sources: `S:ARCHITECTURE.md` "Manifest and commit", `S:perf/retention.md`, `S:reviews/retention.md`, `S:design/phase-3.md`.
- Commits: strata:0156b57.
- Note: MISSING -> strata `docs/mechanisms/manifest-and-recovery.md`.

### 5.3 The directory owner lock
- Explains: one process owns an index directory; classification through the I/O virtual; the `fstatfs` ruling; behavior on network and FUSE filesystems (warn, proceed).
- Sources: `S:reviews/dir-lock-code.md`, `test/directory_test.cc`, `completion` `acquire_directory_owner`.
- Commits: strata:cbcb429 (dir-lock, DL1-DL6), strata:582d6dd (pin completion for `acquire_directory_owner`).
- Note: MISSING -> strata `docs/mechanisms/directory-lock.md`.

### 5.4 faultfs: power loss and fault injection
- Explains: an `IO` implementation that loses unsynced writes at any barrier, fails reads and writes, and damages bytes; how one model covers segments, manifests, and the journal.
- Sources: `include/strata/core/faultfs.hh`, `test/faultfs_test.cc`, `S:design/phase-3.md` "faultfs (D19)", `S:gates/phase-3-lifecycle.md`, `S:reviews/phase-3-lifecycle.md`.
- Commits: none recorded here (start from `git log -- include/strata/core/faultfs.hh`).
- Note: MISSING -> strata `docs/mechanisms/faultfs.md`.

### 5.5 Media faults: transient versus damage, fencing, heal
- Explains: the fault model, heal on transient errors, fail closed or report on damage, the needs-heal path, an index fenced by a failed fuse-filter build, install's retry.
- Sources: `S:design/media-faults.md`, `test/media_test.cc`, `S:perf/fuse-fence.md`, `S:reviews/format-13-mutants.md`, `S:reviews/install-architect.md`.
- Commits: strata:816158a, strata:6105148, strata:ef48ed0.
- Note: MISSING -> strata `docs/mechanisms/media-faults.md`.

### 5.6 Integrity levels and decoder safety
- Explains: footers and small sections checked at open; stored and compressed blocks on every read; region checksums on first touch (4 KiB default); whole files on merge, replication, scrub; memory-safe decoders under fuzz; validate-at-open.
- Sources: `S:ARCHITECTURE.md` "Manifest and commit" (checksums), `S:TESTING.md` "Layers", `lib:termdict` `docs/reviews/`, `lib:residency` README, `fuzz/`.
- Commits: termdict:8b1e964 (validate-at-open merge), termdict:7649aa2 (security review, approve), residency:adc23e8 (verified regions), strata:4c3471a (fuzz campaign merge).
- Note: MISSING -> strata `docs/mechanisms/integrity-levels.md`.

### 5.7 Durability modes and the consistency spectrum
- Explains: Part A of the design (consistency modes and replication), Part B outlined (residency and retention); the per-version clock bit. Design only; no code on main.
- Sources: `S:design/durability-modes.md` (strata:95357d9, revision 10), `S:reviews/durability-modes-design.md`, `durability-modes-security.md`, `S:status/durability-modes.md`.
- Commits: strata:831459e (bit 16 reserved).
- Note: MISSING (when built) -> strata `docs/mechanisms/durability-modes.md`.

---

## 6. Residency and the cold tier

Owners: `residency`, strata core (cold tier), `task`, `completion`.

### 6.1 Mmap with asynchronous residency
- Explains: zero-copy decode; at query start the plan requests ranges and suspends until resident (`madvise(WILLNEED)`, io_uring on Linux, POSIX AIO on macOS); what stays pinned per segment; the verified block cache as the second backend (design in ARCHITECTURE).
- Sources: `lib:residency` README and `residency.d2`, `S:ARCHITECTURE.md` "I/O", `S:DISTRIBUTION.md` "Storage tiers", `lib:completion` `ARCHITECTURE.md`, `docs/PLATFORM_FINDINGS.md`.
- Commits: residency:524a8ae (initial library), residency:cdc5236 (CRC-32C from its own library).
- Note: MISSING -> residency `docs/mechanisms/residency.md`.

### 6.2 Verified regions
- Explains: checksummed regions (4 KiB default) verified on first touch; spans of equal regions; the region-size choice; mmap pages re-read after eviction are not re-verified.
- Sources: `lib:residency` `residency.hh`, `S:perf/p15-region-size.md`, `S:ARCHITECTURE.md` "I/O".
- Commits: residency:adc23e8 (verified_regions), residency:7b9e4f8 (spans of equal regions), residency:f65cf08 (model test).
- Note: MISSING -> residency `docs/mechanisms/verified-regions.md`.

### 6.3 What an idle open index costs
- Explains: heap and mapping cost per open index, and the limits to millions of indexes per host.
- Sources: `S:perf/idle-index.md`, `S:status/idle-index.md`, `S:design/small-indexes.md`, `S:perf/exploration-log.md` (idle-index row), `bench/idle_index/idle_index.cc`.
- Commits: strata:2562e8f (measure what an idle open index costs), strata:1a04bc3 (merge).
- Note: RECORDS.

### 6.4 Segment records and bodies
- Explains: a segment is a record (the owner's manifest entry, copy-on-write deleted bits, three facts the manifest needs) plus a body (mapping and decoded segment); a record can exist with no body.
- Sources: `S:mechanisms/segment-records.md` `@phase/cold-tier-step1`, `S:design/cold-tier.md` D2, D9, D11 (strata:f8c4080), `test/cold_segment_test.cc` `@phase/cold-tier-step1`.
- Commits: strata:65ca8cf (the note), strata:61dbffe (race readers' maps against the owner's prepare), strata:40fadb9 (merge main, CT-R5 rollback).
- Note: EXISTS (`docs/mechanisms/segment-records.md` on `phase/cold-tier-step1`; merge with step 1).

### 6.5 The cold tier: opening without reading
- Explains: how an index with millions of siblings opens lazily and pays only for segments queries touch; correctness under concurrency; steps 2 and 3. Design only past step 1.
- Sources: `S:design/cold-tier.md` (revision 2), `S:reviews/cold-tier-design.md`, `S:status/cold-tier.md`.
- Commits: strata:eb87572 (apply-atomic landed under step 1's base), strata:40fadb9.
- Note: MISSING (steps 2-3 unbuilt) -> strata `docs/mechanisms/cold-tier.md`.

### 6.6 Many tenants on one node
- Explains: the field at 10^4 and 10^5 small tenants, what bounds density, which designs the measurements chose between.
- Sources: `S:perf/c1-field-tenants.md`, `S:design/small-indexes.md`, `S:design/workload-profiles.md`, `S:perf/exploration-log.md` (C1 row).
- Commits: strata:21cef28 (C1 field note), strata:358b762 (merge).
- Note: RECORDS.

---

## 7. Query parsing and analysis

Owners: `analysis`, `unicode`, `snowball`, `queryparser`, `automata` (branch), Xapiand (host-side parse).

### 7.1 Analysis: Xapian-compatible term generation
- Explains: word rules, case folding, `Z`-prefixed stems, stop strategies, CJK n-grams; the `word_scanner` class; the UAX #29 analyzer as a non-default option.
- Sources: `lib:analysis` README, `lib:analysis` `docs/RULES.md`, `docs/analysis.d2`, `P:ARCHITECTURE.md` "Text analysis".
- Commits: analysis:d3f94cb (initial library), analysis:bb718c4 (`word_scanner` as a public class).
- Note: COVERED (`lib:analysis` `docs/RULES.md`, rules rather than mechanism).

### 7.2 The linear acronym scan
- Explains: why an acronym chain ("U.S.A.") was rescanned quadratically; the scan now remembers a failed chain; where a caller resets it. The same scan fixed quadratic indexing time.
- Sources: `lib:analysis` `analysis.hh` (`acronym`, line 197 onward; the forget-chain call near line 254), `S:perf/exploration-log.md` (queryparser step 2 and re-check rows: 119,785 ns/B before, 152.6 ns/B at 256 KiB after).
- Commits: analysis:9b15abf (SR1), analysis:991ff33 (merge).
- Note: MISSING -> analysis `docs/mechanisms/acronym-scan.md` (known gap).

### 7.3 Unicode tables and Snowball stemmers
- Explains: categories, simple lowercase maps, lenient UTF-8 decode generated from a pinned UCD; the 35 Snowball algorithms generated with Snowball 3.0.0, checked in, each gated on a vocabulary compared against Xapian.
- Sources: `lib:unicode` README and `tools/`, `lib:snowball` README and `tools/`, `P:ARCHITECTURE.md` "Text analysis".
- Commits: Xapiand:0f4424eb6 (stem queries with the snowball library), Xapiand:58bdfdd83 (Basque code).
- Note: MISSING -> unicode `docs/mechanisms/ucd-generation.md`, snowball `docs/mechanisms/generated-stemmers.md` (low).

### 7.4 Analysis inside Xapiand
- Explains: indexing text with `analysis` and `snowball` in place of Xapian's term generator; term differentials in the E2E harness; `cjk_words`.
- Sources: `X:dev/extraction-ledger.md` (rows for the analysis and snowball swap, `cjk_words`), `X:dev/testing.md`.
- Commits: Xapiand:fe9307227 (index with analysis and snowball), Xapiand:8f7a58011 (term differentials), Xapiand:990e0dff2 (cjk_words ledger row).
- Note: MISSING -> Xapiand `docs/mechanisms/analysis-swap.md`.

### 7.5 The query parser: iterative, tree, offsets instead of throws
- Explains: one non-recursive parser (depth bounded by memory, never the call stack), the tree and `describe`, errors as offsets, OOM mapped to an error; contrast with the recursion in Xapiand's `query_dsl::process`.
- Sources: `lib:queryparser` README, `queryparser.hh` (header comment), `S:design/queryparser.md` (strata:bc842b6), `S:reviews/queryparser-step1.md`, `S:status/queryparser.md`.
- Commits: queryparser:11a6365 (core, tree, describe, iterative parser), queryparser:8c2190c (wildcard, partial, fuzzy, CJK n-gram, pure NOT), queryparser:c0a1584 (SR1-SR4, CR2-CR5), queryparser:b732d09 (OOM test skipped under TSan).
- Note: MISSING -> queryparser `docs/mechanisms/iterative-parser.md` (known gap).

### 7.6 Compatibility preset, extension flags, and oracle parity
- Explains: Xapian `QueryParser` behavior as the compat preset, extensions behind flags; the T1/T2/T3 runs over recorded oracle vectors (528 T1 rows); how a mismatch is judged "on merit".
- Sources: `S:design/queryparser.md` sections 2-4, `lib:queryparser` `test/`, `strata-oracle/vectors`, `S:perf/exploration-log.md` (queryparser rows), `S:TESTING.md`.
- Commits: queryparser:c18feee (V2 grammar), queryparser:56d7e08 (T2 runner), queryparser:2226c88 (T3 over V3 vectors), queryparser:fccaab8 (close V2 and V3 rows), queryparser:9dcb741 (follow Xapian 2.0), queryparser:09a4ea6 (judge each A/B class on merit).
- Note: MISSING -> queryparser `docs/mechanisms/compat-and-oracle.md`.

### 7.7 Wildcard, fuzzy, and automaton expansion
- Explains: multi-term queries resolve per segment by intersecting term blocks with automata (Levenshtein DFAs, wildcard, prefix); expansions combine as `SYNONYM`.
- Sources: `S:design/automata.md` (strata:219ded0; not on main), `P:ARCHITECTURE.md` "Read path" 2, `lib:string-similarity` `ARCHITECTURE.md`.
- Commits: queryparser:8c2190c (parser side).
- Note: MISSING (library unlanded) -> `docs/mechanisms/automata.md` in the automata repository once it lands.

### 7.8 Where queries are parsed: host side on both engines
- Explains: Xapiand parses on the host and hands the engine a tree (D6, once queryparser passes T4); the A/B worktree that compares parsers; `boolean-parser` stays Xapiand's other query language.
- Sources: `S:design/xapiand-on-strata.md` D6, `X:design/extraction.md`, `S:perf/exploration-log.md` (L1 row), Xapiand worktree `queryparser-ab` (branch `phase0.5/queryparser-ab`).
- Commits: strata:7e11dec (D6: parse host-side on both engines).
- Note: MISSING -> Xapiand `docs/mechanisms/query-parse-host-side.md`.

---

## 8. Scoring and the read path

Owner: prospect (`search.hh`, `searcher.hh`, `scorer.hh`, `aggregate.hh`, `sortkey.hh`).

### 8.1 The read path: snapshot, rewrite, plan, windows
- Explains: a query takes a per-core snapshot, rewrites to term sets per segment, requests residency, then evaluates in docid windows (4,096 to start) with the coroutine yielding between windows; search over many segments.
- Sources: `P:ARCHITECTURE.md` "Read path", `P:perf/p6-multisegment.md`, `S:ARCHITECTURE.md` "Read path", `S:design/phase-2.md` "Scorers".
- Commits: prospect:fc5dce7 (phrases merge; reader recycling).
- Note: MISSING -> prospect `docs/mechanisms/read-path.md`.

### 8.2 Scorers, conjunctions, and block-max MaxScore
- Explains: iterators with `doc/next/advance/cost/score/max_score`; leapfrog from the cheapest; block-max MaxScore in 4,096-ordinal windows for top-level disjunctions; why nested disjunctions do not lead (14% of time in heap updates on the 40 `+a b c` queries); bulk top-k for conjunctions.
- Sources: `P:ARCHITECTURE.md` "Read path" 4-5, `P:perf/p6a-conjunction-bulk-topk.md`, `P:perf/count-union-bits.md`, `S:design/phase-2.md`.
- Commits: prospect:54e3cc0 (count-union-bits merge).
- Note: MISSING -> prospect `docs/mechanisms/scorers.md`.

### 8.3 Pruning floors and threshold seeds
- Explains: the optional side of required-plus-optional gets its own minimum; the seed floor is kept for counted disjunctions; threshold seeds stored in the segment (format 13 F13) at a lower writer cost; why block maxima store (wdf, doclen) pairs, never scores.
- Sources: `P:perf/pruning-floors.md`, `P:perf/format13-seeds-cost.md`, `S:design/format-13.md` F13, `P:perf/query-wins.md`.
- Commits: prospect:337f1ce (prune the optional side, keep the seed floor), prospect:59f36fe (merge), strata:907789f (query-wins merge).
- Note: MISSING -> prospect `docs/mechanisms/pruning-floors.md`. Landed through perf work, no design.

### 8.4 Counting windows and membership probes
- Explains: counting unions by FOR blocks into a window's bits; membership probes in counting windows.
- Sources: `P:perf/count-union-bits.md`, `P:perf/p7-membership-probes.md`, `P:perf/query-gap-merged.md`.
- Commits: prospect:54e3cc0.
- Note: MISSING -> prospect `docs/mechanisms/counting-windows.md`.

### 8.5 Phrases and positions
- Explains: position blocks, the phrase check against candidates (most are rejected), recycled per-thread query objects, the two-slot pair match, `can_compete` before position reads; the remaining gap to Tantivy.
- Sources: `P:perf/phrases-explore.md`, `P:perf/format13-query-items.md` (positions), `S:design/format-13.md` F14.
- Commits: prospect:0555d10 (explore), prospect:d2e22c9 (E1 recycle), prospect:95dfa26 (E2 two-slot match), prospect:3f7760c (CR1, CR2), prospect:9de01a1 (iterator 23% smaller), prospect:fc5dce7 (landed).
- Note: MISSING -> prospect `docs/mechanisms/phrase-matching.md`.

### 8.6 BM25 presets and statistics
- Explains: `bm25` (Lucene/Tantivy parameters over one-byte lengths) versus `bm25_xapian` (exact lengths, smoothed IDF, k1/k2/k3/b/min_normlen); exact N and average length; termfreq counts deleted documents until merge.
- Sources: `P:perf/bm25-presets.md`, `P:ARCHITECTURE.md` "One implementation per mechanism" and "Read path" 6-7, `S:design/phase-2.md` "Weighting".
- Commits: none recorded here.
- Note: MISSING -> prospect `docs/mechanisms/weighting.md`.

### 8.7 Collectors, sort keys, collapse, aggregations
- Explains: top-k with a rising threshold, ties by docid; host-supplied sort-key functions over doc values; collapse by slot; columnar aggregations; Xapian-style totals; exhaustive mode.
- Sources: `P:design/phase-4.md`, `P:ARCHITECTURE.md` "Read path" 8, `S:design/phase-2.md` "Collectors", `X:status/engine-query.md` (X3 step 2).
- Commits: Xapiand:1ab0ce077 (X3 step 2: sort keys, spies, aggregations).
- Note: MISSING -> prospect `docs/mechanisms/collectors.md`.

### 8.8 Scored filters: ranges and geo
- Explains: ranges and geo as scored filters exposing `weight()` and `max_weight()`; PostingSource as a value-slot filter (D4); the HTM geo id staying in the host.
- Sources: `P:ARCHITECTURE.md` "Read path" 2, 4, `S:design/xapiand-on-strata.md` D4, `lib:bkd` README.
- Commits: strata:246e2e7 (D4: PostingSource is a value-slot filter).
- Note: MISSING -> prospect `docs/mechanisms/scored-filters.md`.

### 8.9 Mergeable shard results and exact global scoring
- Explains: a shard returns statistics, top-k with sort keys, aggregation partials, and estimate triples; exact scoring gathers statistics first.
- Sources: `S:DISTRIBUTION.md` "Query", `S:ARCHITECTURE.md` "Distribution", `P:ARCHITECTURE.md` (competitive pairs).
- Commits: none recorded here (not built).
- Note: MISSING (design only) -> strata `docs/mechanisms/shard-search.md`.

### 8.10 How the query gap was closed
- Explains: attribution of each loss row to named parts, the mechanisms tried, which ones the format froze.
- Sources: `S:design/query-gap.md` (strata:e272ec2), `P:perf/format13-loss-attribution.md`, `P:perf/query-wins.md`, `P:perf/format13-query-items.md`, `S:reviews/query-gap-design.md`.
- Commits: prospect:f416724 (format 13's losses attributed), prospect:f21ce08 (1M measurements and the joint selection).
- Note: RECORDS.

### 8.11 Hybrid search: vectors beside text
- Explains: fit verdict for AI retrieval; one vector plugin in prospect with kernels in a new library; exact search first; reciprocal-rank fusion; idle cost of an index with vectors. Evidence for a go or no-go, not a commitment.
- Sources: `S:design/hybrid-search.md` (strata:8c539a5), `S:design/hybrid-search/` (vecbench), `S:status/hybrid-search.md`.
- Commits: none on main.
- Note: MISSING (design only) -> prospect `docs/mechanisms/vector-plugin.md` if built.

---

## 9. Replication and cluster

Owners: `strata::group`, `consensus`, `journal`, `cluster`, `completion`, `reactor`, Xapiand
(today's cluster).

### 9.1 Guarantee, ownership, and the determinism contract
- Explains: what a commit means under groups; one owner per shard; the deterministic core.
- Sources: `S:REPLICATION.md` "Guarantee", "Ownership", "Determinism contract", `S:design/track-c.md`.
- Commits: none recorded here.
- Note: COVERED (`S:REPLICATION.md`).

### 9.2 The replicated write path: consensus, journal, core
- Explains: log append, apply, checkpoints; the applied log position in the manifest; the (log index, position in batch) seqno.
- Sources: `S:REPLICATION.md` "Write path", `lib:consensus` `ARCHITECTURE.md`, `lib:journal` `ARCHITECTURE.md`, `docs/MEASUREMENTS.md` in each.
- Commits: none recorded here (consensus: 99 commits; journal: 57).
- Note: COVERED (the two libraries' `ARCHITECTURE.md`, `S:REPLICATION.md`).

### 9.3 Checkpoints and repair
- Explains: installing a checkpoint into a lagging replica; repair at or below the commit cursor (C3); corruption defense.
- Sources: `S:REPLICATION.md` "Checkpoints and repair", "Corruption defense", `S:design/track-c.md` C3, `S:gates/track-c-c1.md`, `S:gates/track-c-c2.md`.
- Commits: strata:17e8b3c (install side).
- Note: MISSING -> strata `docs/mechanisms/repair.md`.

### 9.4 Membership, voter roles, and multi-group operation
- Explains: C1 membership changes and voter roles; C2 many groups per node.
- Sources: `S:design/track-c.md` C1, C2, `S:gates/track-c-c1.md`, `S:gates/track-c-c2.md`, `S:REPLICATION.md` "Replica count".
- Commits: none recorded here.
- Note: MISSING -> consensus `docs/mechanisms/membership.md`.

### 9.5 Pipelined replication, flow control, and the peer transport
- Explains: C4 pipelining and flow control; identity, versions, and TLS between peers. Design only for the transport.
- Sources: `S:design/track-c.md` C4, `S:design/peer-transport.md` (strata:d8384c8), `S:reviews/peer-transport-design.md`, `peer-transport-security.md`.
- Commits: none on main.
- Note: MISSING (transport unbuilt) -> consensus `docs/mechanisms/peer-transport.md`.

### 9.6 The cluster substrate: bus, discovery, leader election
- Explains: UDP-multicast typed message bus, liveness, Raft election, as Xapiand runs them today.
- Sources: `lib:cluster` `ARCHITECTURE.md`, README, `Xapiand ARCHITECTURE.md` "Runtime architecture", `Xapiand AGENTS.md` ("One primary shard per logical shard").
- Commits: none recorded here (94 commits).
- Note: COVERED (`lib:cluster` `ARCHITECTURE.md`).

### 9.7 Asynchronous completion I/O
- Explains: owned, completion-driven file I/O; POSIX hardening, io_uring, AIO, Asio adapter; queue-depth findings per platform.
- Sources: `lib:completion` `ARCHITECTURE.md`, `docs/MEASUREMENTS.md`, `docs/PLATFORM_FINDINGS.md`, `docs/reviews/`, `docs/status/`.
- Commits: none recorded here (51 commits).
- Note: COVERED (`lib:completion` `ARCHITECTURE.md`).

### 9.8 Placement: per-index replication and locality
- Explains: Xapiand's per-index placement today, SPAR, problems in the paper, the strata placement strategy for Track D3.
- Sources: `S:design/placement-strategies.md`, `S:DISTRIBUTION.md` "Layers".
- Commits: none recorded here.
- Note: MISSING (design only) -> strata `docs/mechanisms/placement.md`.

### 9.9 The scale contract and the coordinator
- Explains: at least 100,000 nodes; the cluster layers; storage tiers; open decisions; what comes from Xapiand.
- Sources: `S:DISTRIBUTION.md`.
- Commits: none recorded here.
- Note: COVERED (`S:DISTRIBUTION.md`).

### 9.10 The deterministic simulator and the linearizability checker
- Explains: determinism, time and clocks, network, disks, faults and schedules, production code inside the simulator, fidelity tiers, shrinking a failing seed.
- Sources: `S:design/simulator.md` (strata:27cfb30), `S:reviews/simulator-design.md`, `S:status/simulator.md`, `sim/` on that branch.
- Commits: none on main (S1 engine built on the branch).
- Note: MISSING (partly built) -> strata `docs/mechanisms/simulator.md`.

### 9.11 Xapiand's cluster today: primaries, pull replication, failover
- Explains: one primary per logical shard elected via Raft; asynchronous pull-based replication triggered by the `DB_UPDATED` multicast; promoting the best live replica.
- Sources: `Xapiand ARCHITECTURE.md`, `Xapiand AGENTS.md` "Load-bearing invariants", `X:status/offload-followups.md` (DB_UPDATED liveness), branch `fix/failover`.
- Commits: Xapiand:201414347 (DB_UPDATED refreshes liveness at message time), Xapiand:8a5bd916a (promote the best live replica, branch `fix/failover`).
- Note: COVERED (`Xapiand ARCHITECTURE.md`, partial); MISSING for failover -> Xapiand `docs/mechanisms/failover.md`.

---

## 10. Xapiand on strata

Owners: Xapiand `src/engine/`, `src/manager.*`, strata design `xapiand-on-strata.md`.

### 10.1 The engine seam
- Explains: the `engine::` types (`Document`, `DocView`, `Shard`, `Query`, `search`, `Error` kinds), the Xapian implementation behind them, the seam inventory, the allowlist `seam_check.sh` enforces, the path to Phase 7a.
- Sources: `S:design/xapiand-on-strata.md` sections 1-4, `S:status/xapiand-on-strata.md`, `X:status/engine-core.md`, `X:gates/phase0.5.md` (T2), `src/engine/`.
- Commits: Xapiand:566943e9e (engine-core, X2), Xapiand:fe9307227 (X1 first commit), strata:2888951 (design merge), strata:ac094f0 (design drafted).
- Note: MISSING -> Xapiand `docs/mechanisms/engine-seam.md` (known gap).

### 10.2 The engine write path: documents in the engine's own form
- Explains: `engine::Document` as a handle over the engine's document; `Schema::index`, `GenerateTerms`, and the handler write through it; the shard and the WAL consume the same object.
- Sources: `docs/mechanisms/engine-write-path.md` `@phase0.5/seam-write`, `X:gates/phase0.5.md`, `X:perf/gate05-index.md`.
- Commits: Xapiand:fba3e3a53 (build documents in the engine's own form), Xapiand:8170a3ef6 (record the bisect: estimate fix and seam write cost).
- Note: EXISTS (on Xapiand `phase0.5/seam-write`; not on master).

### 10.3 The engine census
- Explains: `-DENGINE_CENSUS=ON` counts the facilities each HTTP request uses at the seam; off, every hook is `((void)0)`.
- Sources: `docs/mechanisms/engine-census.md` `@phase0.5/census`, `X:gates/phase0.5.md` (T1: the option does not exist on master).
- Commits: Xapiand:cdfdba558 (add the ENGINE_CENSUS counters).
- Note: EXISTS (on Xapiand `phase0.5/census`; not on master).

### 10.4 The query seam: `engine::Query` and the callback bases
- Explains: every query producer builds `engine::Query`; `query_dsl`, `range`, `geospatialrange`, `keymaker`, `generate_terms` (query side), and aggregations migrate; sort keys, spies, and aggregations after step 1; the query is translated once.
- Sources: `X:status/engine-query.md`, `X:reviews/engine-query-code.md`, `engine-query-2-code.md`, `src/engine/query.h`, `query.cc`.
- Commits: Xapiand:60efd75ef (X3 step 1), Xapiand:1ab0ce077 (X3 step 2), Xapiand:f969aea56 (step 2 review conditions).
- Note: MISSING -> Xapiand `docs/mechanisms/query-seam.md`.

### 10.5 The search and read seam: `engine::search`, `DocView`, error kinds
- Explains: the read slice (dump, info, check), `engine::search` and its Xapian implementation, DocView lifetime and the leak test, Xapian exceptions mapped to `engine::Error` kinds.
- Sources: `X:status/engine-search.md`, `X:reviews/engine-search-x4a-code.md`, `x4b-code.md`, `x4c-code.md`, `src/engine/search.h`, `error.h`, `doc_view.h`.
- Commits: Xapiand:593b78293 (X4a), Xapiand:da26127a3 (X4b), Xapiand:cca0011bb (X4c), Xapiand:84e7e00f9 (syntax errors answer 400).
- Note: MISSING -> Xapiand `docs/mechanisms/search-seam.md`.

### 10.6 The offload bridge
- Explains: one `engine::sync_wait` that refuses to block a reactor or scheduler thread (`check_may_block`); FLUSH offloaded; no DocIndexer dies on a reactor; replication and remote views tear down on the pool; the reactor's `inside_run` marker; a reactor waiting on work that needs it deadlocks.
- Sources: `X:reviews/engine-offload-architect.md` (C1-C7), `X:reviews/offload-bridge-code.md`, `X:status/offload-bridge.md`, `X:status/offload-followups.md`, `src/engine/sync.h`, `sync.cc`, `src/engine/drain.h`, `release.h`.
- Commits: Xapiand:44f628903 (offload bridge merge), Xapiand:201414347 (follow-ups CR2, CR5), reactor:d381e4d, reactor:0c04d0d.
- Note: MISSING -> Xapiand `docs/mechanisms/offload-bridge.md` (known gap).

### 10.7 Pool concurrency and shard endpoints
- Explains: the cleanup use-after-free, lost wake-ups, reopen races, and the endpoint eviction bug fixed in `checkin`.
- Sources: `X:status/pool-concurrency.md`, `X:reviews/pool-concurrency-code.md`.
- Commits: Xapiand:2466dac5c (pool concurrency merge), Xapiand:55316114d (sanitizer flags set before the fetches).
- Note: MISSING -> Xapiand `docs/mechanisms/shard-pool.md`.

### 10.8 Gate 0.5: Xapiand on the seam
- Explains: the rows (seam closed, assertions, bodies, bulk-index slowdown and the estimate change) and what each result means.
- Sources: `X:gates/phase0.5.md`, `X:perf/gate05-index.md`, `S:design/xapiand-on-strata.md` section 5.
- Commits: Xapiand:79554ef8c (gate merge), Xapiand:dada66d7a (lead verdict), Xapiand:8170a3ef6 (the bisect: estimate fix and seam write cost).
- Note: RECORDS.

### 10.9 Schema, reserved words, perfect-hash dispatch
- Explains: schema to terms, slots, and analyzers; `$`/`_` reserved keys; compile-time perfect-hash tables for the query DSL and aggregations.
- Sources: `Xapiand ARCHITECTURE.md` "Subsystems", `Xapiand AGENTS.md` "Conventions you'll see everywhere", `src/reserved/`.
- Commits: none recorded here.
- Note: COVERED (`Xapiand ARCHITECTURE.md`).

### 10.10 What storage and the WAL are today, and what replaces them
- Explains: the `storage` blob store (volumes, double-buffered aligned writes), the WAL's idempotent replay, async-by-default durability; the replacement path in strata.
- Sources: `lib:storage` `ARCHITECTURE.md`, `docs/INTEGRATION.md`, `Xapiand AGENTS.md` "Load-bearing invariants" and "Traps", `S:design/xapiand-on-strata.md`.
- Commits: none recorded here (storage: 18 commits).
- Note: COVERED (`lib:storage` `ARCHITECTURE.md`).

### 10.11 Transport security: HTTPS, cluster, discovery
- Explains: the threat model, TLS for the API, the dependency move; trusted-cluster scope.
- Sources: `X:design/transport-security.md` (revision 3), `X:reviews/transport-security-design.md`, `X:gates/transport-security-step0.md`.
- Commits: Xapiand:bb4981558 (HTTPS for the REST API), Xapiand:7e8cccb7d (E2E over HTTPS, T1, T2).
- Note: MISSING -> Xapiand `docs/mechanisms/https.md`.

### 10.12 The Xapian fork and what leaves
- Explains: the fork model, the patch stack, the upgrade procedure; which parts the seam retires. Do not read `src/xapian/` to write this; use the doc.
- Sources: `X:dev/xapian-fork.md`, `X:dev/extraction-ledger.md`.
- Commits: Xapiand:b76512580 (note that strata replaces Xapian).
- Note: COVERED (`X:dev/xapian-fork.md`).

---

## 11. Testing and measurement method

Owners: strata `docs/TESTING.md`, `docs/BENCHMARKS.md`, `docs/perf/README.md`, the libraries' tests, the explorer logs.

### 11.1 Test layers and the coverage rule
- Explains: unit and model tests, pruned-against-exhaustive, oracle parity, differential counts, media faults, fuzzing, sanitizers, crash and power loss, churn, steady state, simulation, E2E, mutation.
- Sources: `S:TESTING.md`, `P:testing/parity.md`, `S:status/core-lifecycle-tests.md`.
- Commits: strata:b3c0ea9 (port prospect's lifecycle suites to the document layout).
- Note: COVERED (`S:TESTING.md`).

### 11.2 Oracle vectors and parity
- Explains: recorded Xapian vectors for match sets, BM25 weights, analyzer output, parser descriptions; the oracle repository's layout; why parity runs against recorded vectors only.
- Sources: `strata-oracle` (`engines/`, `vectors/`), `P:testing/parity.md`, `S:TESTING.md` "Test parity".
- Commits: none recorded here.
- Note: MISSING -> strata `docs/mechanisms/oracle-parity.md`.

### 11.3 Mutation testing as a gate
- Explains: mutants of decoders, scorers, and new mechanisms; killed over total as the report row; equivalent mutants documented.
- Sources: `S:reviews/format-13-mutants.md`, `S:TESTING.md` ("Mutation testing"), `S:perf/exploration-log.md` (mutant rows).
- Commits: none recorded here.
- Note: MISSING -> strata `docs/mechanisms/mutation-gate.md`.

### 11.4 Fuzzing and sanitizer policy
- Explains: fuzz targets and corpora (minimized), what a campaign counts, ASan/UBSan/TSan in CI, the test allocator paired with ASan and skipped under TSan, tests that must not run under a sanitizer that owns `operator new`.
- Sources: `fuzz/`, `.github/workflows/ci.yml`, `S:TESTING.md`, `S:perf/exploration-log.md` (fuzz campaign row).
- Commits: strata:4c3471a (fuzz campaign merge), strata:05aa851 (pair the test allocator with ASan), queryparser:b732d09.
- Note: MISSING -> strata `docs/mechanisms/fuzz-and-sanitizers.md`.

### 11.5 Model checks and litmus tests
- Explains: a randomized Python model against a sequential reference for the write buffer (6.26M reads, 0 wrong); the hazard litmus test that proves a weaker order fails.
- Sources: `test/hazard_litmus_test.cc`, `S:reviews/write-buffer-design.md`, `S:perf/exploration-log.md` (re-check rows).
- Commits: strata:ed57019 (hazard_test names the records left claimed).
- Note: MISSING -> strata `docs/mechanisms/model-checks.md`.

### 11.6 Benchmark method: interleaved A/B, noise, holds, tiers
- Explains: interleaved trials, the noise-band rule, JSON output, the quick/overnight/gate tiers, the quiet holds that isolate runs, the gate VM.
- Sources: `S:BENCHMARKS.md` "Rules", "Tiers", "Harness notes", "Reporting", `S:perf/README.md`, `S:perf/benchmark-hosting.md`, `bench/kv/ab.py`, `tools/vm/quiet_trials.sh`.
- Commits: strata:f199fce (Linux x86_64 numbers; explore/linux-1026 merge).
- Note: COVERED (`S:BENCHMARKS.md`).

### 11.7 Baselines: RocksDB, Lucene, Tantivy, Xapian
- Explains: what each baseline measures, fairness audits of the key-value harness, the clean-room RocksDB specification, benchmark coverage per upstream engine.
- Sources: `S:design/rocksdb-survey.md`, `S:research/rocksdb-mechanisms.md`, `S:perf/rocksdb-fairness-audit.md`, `S:perf/benchmark-coverage.md`, `S:perf/core-vs-rocksdb-f14.md`, `core-vs-rocksdb-0021.md`, `core-vs-rocksdb-1022.md`.
- Commits: strata:b8a2cb6 (core vs RocksDB merge), strata:d4fec9a (rocksdb-spec merge), strata:f1ecf75 (benchmark coverage).
- Note: RECORDS.

### 11.8 The exploration log and the hunt list
- Explains: the trial mode (lead-run benchmarks, targeted explorers), how a hunt-list row becomes a landed change, stale attribution as a recurring cost.
- Sources: `S:perf/exploration-log.md`, `S:perf/hunt-list.md`, `S:status/README.md`, `S:PLAN.md`.
- Commits: strata:eb87572 (latest main).
- Note: RECORDS.

### 11.9 Docs as an end-to-end suite
- Explains: each documentation example is a request plus assertions; `docs_to_postman.py` compiles them for newman against a live server; the E2E counts (576/576) used as the seam's safety net.
- Sources: `Xapiand AGENTS.md` "Verifying changes end-to-end", `X:dev/testing.md`, `X:gates/phase0.5.md`.
- Commits: Xapiand:8f7a58011 (term differentials and scratch dir for the E2E capture).
- Note: COVERED (`X:dev/testing.md`).

---

## Missing notes, ordered by value

Value orders core mechanisms (concurrency, durability, the data plane) before libraries,
libraries before records. "Origin" says whether a design exists: `design` means the decisions
are written down but not the mechanism; `perf` means the mechanism landed through perf or fix
work with a ledger and a perf record only.

| # | Note to write | Section | Origin | Start from |
|---|---|---|---|---|
| 1 | Write buffer (lanes, records, version map, freeze) | 3.4 | design | `include/strata/core/write_buffer.hh`; `S:design/write-buffer.md` D3, D8 |
| 2 | Hazard-pointer reclamation | 4.7 | design | `include/strata/core/hazard.hh` header comment; `test/hazard_litmus_test.cc` |
| 3 | Buffer visibility: V, views, reads at r | 3.5 | design | `S:design/write-buffer.md` D5, D10; `S:status/write-buffer-reads.md` |
| 4 | Commit protocol and barriers | 5.1 | design | `S:design/phase-3.md` "Commit and recovery"; `S:perf/commit-barriers.md` |
| 5 | Layouts and plugins (the core fold) | 2.1 | design | `S:design/core-split.md`; `S:design/layers.md` L2-L4 |
| 6 | Snapshots, views, retention, close | 4.8 | design | `S:status/shared-reader-views.md`; `include/strata/core/index.hh` |
| 7 | Merge policy, background merge, streaming merge | 4.1-4.3 | design/perf | `S:reviews/background-merges.md`; `S:perf/merge-stream.md` |
| 8 | Background deletes | 4.6 | perf | `S:perf/reclaim-bg.md`; strata:2123803 |
| 9 | Reclamation backlog | 4.5 | perf | `S:perf/reclamation.md`; `S:status/reclaim-backlog.md` |
| 10 | Segment frame and format versions | 2.2, 2.3 | design | `S:design/format-13.md` F1-F6; `include/strata/core/format.hh` |
| 11 | Engine seam | 10.1 | design | `S:design/xapiand-on-strata.md` sections 1-3; `X:status/engine-core.md` |
| 12 | Offload bridge | 10.6 | review | `X:reviews/engine-offload-architect.md`; `src/engine/sync.h` |
| 13 | Flush, indexing budget, pressure flush | 3.9, 3.10 | perf | `S:status/pressure-flush.md`; `S:perf/flushmem.md`; strata:2331446 |
| 14 | Key dictionary and primary-key filter | 2.4 | design | `include/strata/core/keys.hh`; `S:design/format-13.md` F8 |
| 15 | Fuse filter build: hash takeover, grow on failure | 3.11 | perf | `lib:fusefilter` README; fusefilter:755b392, fusefilter:e9eadcc |
| 16 | termdict block streaming | 3.12 | perf | `lib:termdict` `termdict.hh` builder; termdict:8367263 |
| 17 | Transactions and rollback | 3.8 | review | `test/apply_atomic_test.cc`; strata:9ee1277 |
| 18 | Install and checkpoint | 4.9 | perf | `S:status/install-own-files.md`; strata:17e8b3c |
| 19 | Manifest, recovery, fail-closed | 5.2 | design | `S:ARCHITECTURE.md` "Manifest and commit"; `S:perf/retention.md` |
| 20 | Owner core and write lease | 3.1 | design | `include/strata/core/owner.hh`; `S:design/shared-reader.md` SR-D19, SR-D20 |
| 21 | Read path, scorers, pruning floors, counting windows | 8.1-8.4 | perf | `P:ARCHITECTURE.md` "Read path"; `P:perf/pruning-floors.md` |
| 22 | The non-recursive query parser | 7.5 | design | `lib:queryparser` `queryparser.hh` header; `S:design/queryparser.md` |
| 23 | The linear acronym scan | 7.2 | review | `lib:analysis` `analysis.hh`; analysis:9b15abf |
| 24 | Tuple inverter and stage 1 | 3.2, 3.3 | design | `P:perf/inverter.md`; `S:research/indexing.md` |
| 25 | Reordering at merge | 4.4 | design | `S:design/reordering.md`; prospect `bisection.hh` |
| 26 | faultfs, media faults, directory lock, integrity levels | 5.3-5.6 | design | `include/strata/core/faultfs.hh`; `S:design/media-faults.md` |
| 27 | Key-ordered blocks (format 14) and the scan cursor | 2.6 | design/perf | `S:design/format-14-kv.md` D1, D3; strata:89f9fd7 |
| 28 | Residency and verified regions | 6.1, 6.2 | design | `lib:residency` `residency.hh`; `S:perf/p15-region-size.md` |
| 29 | Query seam and search seam | 10.4, 10.5 | design | `X:status/engine-query.md`; `X:status/engine-search.md` |
| 30 | Deleted bits, docids and ordinals, stored records | 2.7, 2.8, 2.5 | design | `include/strata/core/deleted_bits.hh`, `docids.hh`; `S:status/stored-short-section.md` |
| 31 | Pool concurrency / shard endpoints | 10.7 | review | `X:status/pool-concurrency.md` |
| 32 | Executor concept and pool executor | 1.4 | none | `include/strata/core/executor.hh`, `pool_executor.hh` |
| 33 | Compat preset and oracle parity (queryparser), oracle vectors | 7.6, 11.2 | design | `S:design/queryparser.md` sections 2-4; `strata-oracle/vectors` |
| 34 | Postings layout, doc values, phrase matching, weighting, collectors, scored filters | 2.10, 2.13, 8.5-8.8 | design | `P:ARCHITECTURE.md`; `P:design/phase-4.md`; `P:perf/phrases-explore.md` |
| 35 | Analysis swap in Xapiand; query parsing host side; HTTPS; failover | 7.4, 7.8, 10.11, 9.11 | design/perf | `X:dev/extraction-ledger.md`; `X:design/transport-security.md` |
| 36 | Model checks, mutation gate, fuzz and sanitizers | 11.3-11.5 | none | `S:reviews/format-13-mutants.md`; `fuzz/`; `test/hazard_litmus_test.cc` |
| 37 | Cold tier steps 2-3, durability modes, peer transport, simulator, placement, shard search, hybrid, automata | 5.7, 6.5, 9.5, 9.8, 9.10, 8.9, 8.11, 7.7 | design only | write when built; the design is the source |
| 38 | bkd, unicode and snowball generation | 2.14, 7.3 | none | library READMEs |

### The nine gaps from the brief, confirmed

Searched strata main, every strata branch, prospect, every library's branches, and Xapiand
branches and worktrees for `docs/mechanisms/`. None of the nine has a note.

| Gap | Confirmed | Section | Start from |
|---|---|---|---|
| Write buffer | no note | 3.4-3.7 | above, rank 1 |
| Hazard-pointer reclamation | no note | 4.7 | rank 2 |
| Background deletes | no note | 4.6 | rank 8 |
| Fuse filter hash takeover | no note | 3.11 | rank 15 |
| termdict block streaming | no note | 3.12 | rank 16 |
| The offload bridge | no note | 10.6 | rank 12 |
| The engine seam | no note (two seam notes exist on branches: the write path and the census) | 10.1 | rank 11 |
| The non-recursive query parser | no note | 7.5 | rank 22 |
| The linear acronym scan | no note | 7.2 | rank 23 |

### Mechanisms found through git log that landed without a design

Each has a status ledger and a perf record but no design and no note: compact slots (strata:8fcd4e3),
pressure flush (strata:5737867), merge-wait (strata:6e13416), fuse-fence and grow-on-failure
(strata:6105148, fusefilter:e9eadcc), the termdict forward seek (termdict:74d518a), the
`hash32` fix (hashes:975492a), the pruning floors (prospect:59f36fe), the phrase recycling
(prospect:fc5dce7), the scan cursor (strata:89f9fd7), install from the target's own files
(strata:53c088d), and the termdict validate-at-open pass (termdict:8b1e964).

## Count by state

Counts are of the sections above; a section whose line names two states (2.14, 9.11) counts under
its first.

| State | Sections |
|---|---|
| EXISTS | 3 (6.4, 10.2, 10.3), all on unmerged branches |
| COVERED | 23 |
| MISSING | 75 |
| RECORDS | 7 |
| Total | 108 sections in 11 chapters |

## Open points for the lead

1. Merge the three branch notes with their steps (`phase/cold-tier-step1`, `phase0.5/seam-write`, `phase0.5/census`) so `docs/mechanisms/` exists on main.
2. The documents of the design branches (`design/cold-tier`, `design/queryparser`, `design/write-modes`, `design/shared-reader`, `design/hybrid`, `design/landscape`, `design/query-gap`, `design/peer-transport`, `design/simulator`, `design/durability-modes`) are on strata main since the 2026-10-30 triage, each with a status line (PARKED, HISTORY or CURRENT); a section that cites one states that status.
3. ARCHITECTURE.md states a few mechanisms as design (least-attained-service scheduling, partitioned writers behind a gate, the verified block cache backend); each note must check the code before saying it is built.
