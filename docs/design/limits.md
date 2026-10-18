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
| Position | 2^32 − 1 | 32-bit positions in postings | 2,147,483,519 | 2^32 − 1 | 2^32 − 1 | At the largest |
| Within-document frequency, document length | 2^32 − 1 | 32-bit fields | 2^31 − 1 | 2^32 − 1 | 2^32 − 1 | At the largest |
| Fields | No limit (term prefixes) | — | No fixed limit | 65,535 (u16 field ids) | No limit (term prefixes) | At the largest |
| Stored record per document | 64 MiB (`format::max_stored_record`) | Bounds what reading a frame allocates when a file's checksums were recomputed (review C9) | ≈ 2 GiB (Java arrays; 715,827,877 chars per string); in practice `RAMPerThreadHardLimitMB`, 1,945 MB, fails a larger document's inversion first | ≈ 4 GiB (u32 block offsets, unchecked) | — | **Below the largest: open** |
| Replicated command | 1 MiB default, 64 MiB ceiling (consensus `Limits::command_bytes`) | Decoders bound each entry's allocation by the node ceilings (R16) | — | — | — | **Open, with the stored record**: out of the box a replicated transaction is under 1 MiB |
| Merge policy: 5 GiB merged, 10 per merge, 30 per forced merge | Lucene `TieredMergePolicy`'s defaults | None: tuning presets | — | — | — | **To measure** (presets rule) |
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

## Phase 5 rows (docs/design/automata.md, D11)

Reference columns: Lucene 10.4 and Tantivy 0.26 from their documentation (clean room);
Xapian from its documentation.

| Limit | strata | Mechanism | Lucene 10.4 | Tantivy 0.26 | Xapian | Status |
|---|---|---|---|---|---|---|
| Edit distance | Every value | None needed: band cells are 64-bit, and k clamps at 2^62, past any key's length, which changes no answer | 2 (`LevenshteinAutomata`'s tables) | 255 by type (`FuzzyTermQuery::new` takes `distance: u8`) | Unsigned, no fixed limit | At the largest |
| Pattern length | No limit | The query's byte budget (Phase 4's D13a): construction charges the glob masks and the Levenshtein target before allocating, the walk each recorded state (at most 4,097 × (words + 2) words); a refused charge fails the query with `query_budget_error` | Determinization work limit, 10,000 states (throws) | A regex size limit | No fixed limit | At the largest |
| Expanded terms | No limit (the caller's `max_expansion`) | The query's byte budget: each distinct term and each segment's info is charged as the expansion grows | `IndexSearcher` max clause count, 1,024, for scoring rewrites; none for constant score | No fixed limit | No fixed limit (`max_expansion` 0) | At the largest |
| Walk time | No limit | Cancellation and pause checks every `check_every` entries | — | — | — | Bounded by the query's deadline |
| `resume_depth` 4,096 characters, `seek_after` 64 entries, `check_every` 1,024 entries | Tuning presets | None: they change no answer | — | — | — | **To measure** (presets rule) |
