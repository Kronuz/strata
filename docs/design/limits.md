# Limits

Every limit strata enforces names the mechanism that needs it, and is at least the
largest of the reference engines' (Lucene, Tantivy, Xapian; Elasticsearch and Quickwit
inherit Lucene's and Tantivy's) unless a measured cost says otherwise, recorded here.
A limit copied from another engine without its reason is a bug. Tuning defaults (merge
policy, buffer sizes) are not limits: they are presets, and the fastest measured one is
the default.

Reference values were read from Lucene 10.4.0's `IndexWriter`/`ArrayUtil` constants and
Tantivy 0.26.0's source. Xapian's are its documented ones (clean room: never its source).

| Limit | strata | Load-bearing mechanism | Lucene 10.4 | Tantivy 0.26 | Xapian | Status |
|---|---|---|---|---|---|---|
| Term bytes | 2^32 − 1 (`format::max_term_bytes`); a replicated term is also under the group's command limit | `termdict::max_key_bytes` = `fst::max_key_length`: fst validation stores each node's depth in 32 bits | 32,766 (throws) | 65,530 (drops); the default tokenizer drops tokens over 40 | 245 | Fixed: was Xapian's 245, copied |
| Documents per segment | 2^32 − 2 | 32-bit ordinals and the `no_more_docs` sentinel | 2,147,483,519 per index | 2^32 per segment (u32 `DocId`) | 2^32 − 1 per database (default build) | At the largest; docids are 64-bit |
| Buffered entries (`buffered_docs_cap`) | 2^32 − 2 an index buffer, dead copies included; a transaction with more operations is invalid | The buffer's key and docid maps store a buffer index in 32 bits, and 2^32 − 1 is their empty marker; a flush writes the buffer as segments of at most 2^32 − 2 documents. The writer flushes before a transaction would pass the cap | As the documents-per-segment row (flushed by RAM, `IndexWriter.MAX_DOCS` an index in all) | As the documents-per-segment row (u32 `DocId`; flushed by memory budget) | — | Fixed: with no byte budget (`buffer_share` is unbounded by default) nothing stopped a buffer at 2^32 entries |
| Position | 2^32 − 1 | 32-bit positions in postings | 2,147,483,519 | 2^32 − 1 | 2^32 − 1 | At the largest |
| Split text (`split_text`) words a document | 2^32 − 1 words, each at most the term limit; no byte cap of its own | 32-bit positions and document length; a word at or past byte 2^32 of its text takes a side entry (64-bit address), since tuple offsets are 32 bits. Stage 1 counts words only for a text past 2^32 − 1 bytes (`text_detail::splittable`) | 2,147,483,519 positions | 2^32 − 1 positions | 2^32 − 1 positions | At the largest (CR16; was 2^32 − 1 bytes, 2^31 words) |
| Within-document frequency, document length | 2^32 − 1 | 32-bit fields | 2^31 − 1 | 2^32 − 1 | 2^32 − 1 | At the largest |
| Fields | No limit (term prefixes) | — | No fixed limit | 65,535 (u16 field ids) | No limit (term prefixes) | At the largest |
| Buffered run per batch (`tuple_buffer`) | 2^32 − 1 bytes a run (`tuple_buffer::max_run_bytes`); none a document: an entry past the cap takes a run alone | Run marks (entry offsets in a run) are 32 bits; entry and section lengths are 64 bits. A batch's run is cut between terms at the cap, so a run of more than one entry keeps its marks within 32 bits, and an entry past the cap takes a run alone (CR12). The early drain, when an entry's bound (its term, 46 bytes, 20 a tuple, 5 a given position) could pass the cap, only keeps such an entry to one document's postings. A document's tokens are bounded only by the position, wdf, length, and split-text rows | `RAMPerThreadHardLimitMB`, 1,945 MB by default, is checked after each document and only marks the thread's buffer for flush (`DocumentsWriterFlushControl.java:237-241`); within a document the per-thread pools are int-addressed (`Math.addExact` in `ByteBlockPool.java:199` and `IntBlockPool.java:157`), so a document past about 2 GiB of pool throws an aborting exception (`IndexingChain.java:1319-1321`) that aborts the thread's buffer and closes the writer as a tragic event (`DocumentsWriterPerThread.java:556-566`) | ≈ 4 GiB a thread (u32 arena addresses; a larger budget is rejected); the budget is checked after each document, and a document past the arena panics (`assert!(page_id < 4096)`, tantivy-stacker 0.7.0 `memory_arena.rs:204`) | — (its flush threshold counts documents) | **At or above the largest**: no per-document token cap of its own |
| Stored record per document | 64 MiB (`format::max_stored_record`) | Bounds what reading a frame allocates when a file's checksums were recomputed (review C9) | ≈ 2 GiB (Java arrays; 715,827,877 chars per string); in practice `RAMPerThreadHardLimitMB`, 1,945 MB, fails a larger document's inversion first | ≈ 4 GiB (u32 block offsets, unchecked) | — | **Below the largest: open** |
| Replicated command | 1 MiB default, 64 MiB ceiling (consensus `Limits::command_bytes`) | Decoders bound each entry's allocation by the node ceilings (R16) | — | — | — | **Open, with the stored record**: out of the box a replicated transaction is under 1 MiB |
| Merge policy: 5 GiB merged, 10 per merge, 30 per forced merge | Lucene `TieredMergePolicy`'s defaults | None: tuning presets | — | — | — | **To measure** (presets rule) |
| Reclaim step, quota and cap (`reclaim_step`, `reclaim_slice`, `reclaim_cap`) | A file is truncated 16 MiB (`reclaim_step`) at a time, then unlinked. A published merge adds the bytes it obsoleted to a debt (`debt_`); each publish pays at most `reclaim_cap`, 64 MiB, of it; a commit reclaims `reclaim_slice`, 64 MiB, and so pays the debt too; `reclaim()` and admission drain everything | Holds exactly: one syscall frees at most a step; one publish frees at most `reclaim_cap` plus a step (a test counts the mutations in the call); a merge obsoleting more than the cap leaves the excess as debt, paid oldest file first by later publishes and commits. The queue stays bounded while the merges one call publishes obsolete no more than the cap on average (the cap is at least the merge rate, or the queue grows again: 600 MB of 64 KiB overwrites and 193 to 210 files on 1M-record fills at 10x RAM waited, `docs/perf/explore-bigval.md`, `explore-bigdisk.md`); a single merge past the cap (up to `max_merged_bytes`, 5 GiB) is paid over the next calls, not in one. With a host executor the owner posts whole files to it (truncate in steps, sync, unlink) and returns, so the cap bounds the bytes handed to the pool per publish, plus one file, and a 5 GiB file occupies one worker for its whole truncation; a failed delete comes back and is requeued. `drain()`, `close()`, the destructor and `wait_deletes()` wait for the posted deletes with no timeout, so the host keeps the pool running until its indexes close (ending it first hangs them). Nothing caps the bytes in flight: without a storage budget nothing else on disk is bounded either, so the host sets a budget, and admission drains (`docs/status/reclaim-bg.md`). Without one the deletes run on the owner thread, in steps. `retain()` runs first, so a segment a kept manifest, an unflushed list, or a pending merge names is never queued | Deletes unreferenced files at once, when the merge commits (`IndexFileDeleter`), with no step or cap | Garbage-collects after a merge or commit, with no step or cap | — | Fixed: the queue waited for the host's commits |
| Reordering (`reordering.md` D4): min_df 4,096, max_df 1.0, min_partition 32, 20 iterations, `reorder_memory` 256 MB, reorder at force merge only | Lucene 10.4's documented `BPIndexReorderer` defaults and the reference `-bp` builder's settings | None: tuning presets; `reorder_memory`'s fallback is deterministic, not a cap on input | — | — | — | **To measure** (presets rule) |

## Phase 4 rows (docs/design/phase-4.md)

Reference columns: Lucene 10.4 and Tantivy 0.26 from their sources; Xapian from its
documentation. A dash means the engine has no such structure.

| Limit | strata | Mechanism | Lucene 10.4 | Tantivy 0.26 | Xapian | Status |
|---|---|---|---|---|---|---|
| Slot ids | 0 to 2^32 − 2 | 32-bit ids; the largest is reserved (Xapian's `BAD_VALUENO`) | Fields by name, no fixed count | 65,535 fields (u16) | 0 to 2^32 − 2 (`valueno`) | Design |
| Values per document per slot | Bounded by the command and the indexing budget | No count field limits it | No fixed count (sorted-numeric, sorted-set) | No fixed count (multi-valued fast fields) | One value per slot | Design |
| A `bytes` or `binary` value | Bounded by the command limit | The command carries the document | 32,766 bytes (sorted-set doc values) | No fixed limit | No fixed limit | Design |
| Point index and data dimensions | Up to 8 and 16, recorded per tree | One byte each in the tree header | 8 and 16 | — | — | Design |
| Bytes per point dimension | Recorded per tree | A format field, chosen by D15's measurement | Up to 16 | — | — | Design |
| Date range | `f64` seconds since the epoch | The `f64` map | ±292 million years (milliseconds in a long) | ±292 years (nanoseconds in i64) | Any (Xapiand's `f64` seconds) | Design |
| Points leaf size, doc-values block size | 512, 4,096 | Presets recorded per tree or section | 512 points per leaf | — | — | To measure |
| Buckets, collapse keys, sort heap, `collapse_max` | No count limit | The query budget, the coordinator budget, and the 256 MiB frame ceiling | — (Elasticsearch: 65,536 buckets) | 65,000 buckets (default) | No fixed limit | Design |
| Sort components per key | No count limit | The query's wire form under the frame ceiling | No fixed count | No fixed count | No fixed count | Design |
| Aggregation nesting depth | No count limit | Iterative decoding and evaluation, charged to the query budget | — (Elasticsearch: no fixed depth) | No fixed depth | No fixed depth (Xapiand) | Design |
| Cells or ranges per geo covering | No count limit | The filter's wire form under the frame ceiling; the host's covering budget | — | — | — | Design |
| Sections per segment | 64 | Bounds the table-of-contents decoder's allocation (`format.hh`) | — | — | — | Load-bearing |

## Open: document size

A replicated document travels in a consensus command, so the largest document is the
smaller of the group's command limit (1 MiB by default, raisable to the 64 MiB node
ceiling) and the stored-record limit (64 MiB). Both bound
allocation when decoding untrusted or corrupt bytes, which is a real mechanism, but the
value is a choice, and it sits below Lucene's ≈ 2 GiB and Tantivy's ≈ 4 GiB.

Raising either ceiling to gigabytes lets every decoder allocate that much per entry.
The proposal: keep the per-entry and per-frame allocation bounds, and carry a document
larger than one entry as a chunked transaction (several entries, applied at the last)
and a stored record larger than one frame as a run of frames. Allocation stays
bounded, and the document limit becomes the 32-bit record length, past both engines.
The architect's ruling (Phase 4 review, R19): not Phase 4. Chunked transactions are a
Track C design coordinated with Detent; format 13 reserves a frame continuation flag.
The stored-record sizes of Xapiand's harness datasets are measured first, and Phase 7's
gate decides the default against Elasticsearch's 100 MB request bodies. Until then this
row stays open and scheduled.

## Open: merge policy presets

The merge policy's numbers are Lucene's defaults, not measurements on strata. They are
measured with the Phase 3 churn and write-amplification rows (T3, T10), and the fastest
preset at equal memory becomes the default.
