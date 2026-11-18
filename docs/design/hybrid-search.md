> Status: PARKED (2026-10-27) the owner's Q1 ruling is not yet; resumes after the cold tier lands (docs/design/cold-tier.md).

# Hybrid search: full text plus vectors for AI retrieval

Revision 0, 2026-10-27. State: designing (ready for preliminary review). Workstream `hybrid-search`
(ledger `docs/status/hybrid-search.md`). Decisions D1 to D12, gate rows T1 to T10, owner
questions Q1 to Q6, research items R1 to R5. Code is cited at strata `98ce743`, prospect
`9de01a1`, Xapiand `da26127a3`; Lucene 10.4.0 (`9983b7c`, 2026-02-20) and Tantivy 0.26.0 from
local source trees (Apache-2.0, MIT). Web sources were read on 2026-10-27 ([Sources](#sources)).
No GPL source was opened. The one measurement here is a throwaway single-thread microbenchmark
(source, driver and raw lines in `docs/design/hybrid-search/`); every other number is quoted
from a cited source or marked as an estimate.

The owner's direction (2026-10-27): steer toward AI retrieval only where we are a clear fit, with
full-text quality at the Elasticsearch and Quickwit bar; and the goal for vectors is the best
vector search of all, judged against the field's best systems on each axis.

## Fit verdict

**Go, with conditions.** On the evidence so far, we can be the best in the field on the axes the
many-small-indexes workload rewards, and we start at zero where the leaders are strongest. The
"best of all" claim is made per axis, only after its row is measured.

### Where our foundations clearly win, and what each claim still needs

| Claim | Why it is structural | Evidence it needs |
|---|---|---|
| Small per-tenant indexes: exact SIMD search beats graph search on total cost | Under the crossover (n × d ≈ 1.4 × 10^7 for `i8`, 5 to 9 × 10^7 for 1-bit codes at 1 ms, one thread, D4) an exact code scan answers at recall 1.0 with no build and nothing resident; a graph pays its build, its links, and random reads to approximate the same answer. Lucene itself builds no graph under 648 vectors a segment (Baselines). On latency alone it is unproven: graph latency at 10^3 to 10^5 vectors is not measured | T1, T2 (hnswlib beside it) on `main`; this design's microbenchmark is one x86 Mac, one thread |
| No graph rebuild on merge | Codes are self-contained per vector (D3b), so a merge copies rows; Lucene 10.4 requantizes every vector at each merge and inserts every node not in the largest input's graph (Baselines) | T6 against Lucene 10.4's int8 and BBQ merges |
| Quantization-first storage | The scanned column is the code (`i8`, or `bit1` at d/8 + 8 bytes), full precision is read only to rerank, and neither is resident while idle (D11) | T3; R2 for `bit1`'s fixed centroid |
| Filters on the full-text match set without a recall cliff | The vector scorer visits only the filter's ordinals, so recall is 1.0 at every selectivity and cost falls with it (D4); graph systems manage the cliff with ACORN, payload edges, or a scan threshold (Baselines) | T5 against Lucene, Qdrant, Weaviate |
| Hybrid on one snapshot, visible on acknowledgement | Both legs read one view (D5); a search sees acknowledged writes through the write buffer's coalesced freeze (`write-buffer.md` D11) for vectors as for text, where LanceDB scans unindexed rows on a slower path and Turbopuffer scans its log [ld-vi] [tp-a] | T4, and the write buffer's visibility rows |
| Idle cost at millions of indexes | No resident vector structure (D11), on top of D1's per-index work (the landscape's T1) | T3, T8; D1's measurement is still open (`product-landscape.md:170`) |

### Where we would be weak or only at parity

| Against | Their strength | Our position |
|---|---|---|
| ann-benchmarks leaders (NGT-QSG, ScaNN, glass), FAISS, hnswlib, USearch | Peak single-thread QPS on one 1M-vector RAM-resident index: 18,098 QPS at 0.95 on sift-128 [annb] | Nothing yet. T7 is research-grade (R1) |
| Qdrant, Milvus/Zilliz, Weaviate | Mature ANN, filtered graphs, quantization menus, clients, integrations, managed clouds; Zilliz 13,316 QPS on Cohere 1M (vendor) [vdbb] | Parity at best for one large collection; we lead only if T8 shows per-tenant cost |
| Turbopuffer | Object-storage namespaces, "unlimited (seen 250M+)", warm p50 14 ms at 1M (vendor) [tp-l] [tp-a] | Closed, hosted; our cold tier is the landscape's undesigned D6 |
| LanceDB | Columnar files, RaBitQ IVF (vendor 0.968 recall at 5 ms a core) [lance-rq] | Close on mechanism; we add BM25 at Lucene's bar and real-time visibility |
| Vespa | Exact and HNSW in one engine, tensors, mature hybrid ranking | Parity on hybrid ranking; we lead on per-index cost only if T8 shows it |
| pgvector, ParadeDB | SQL, transactions, the Postgres ecosystem | Not our market; pgvector's IVFFlat sits at 25 QPS on sift [annb] |
| Embedding inference (Weaviate, Qdrant Cloud, Meilisearch embedders) | One API from text to results | None by design (D10, Q4) |

### Conditions

1. **C1:** T3 and T8 show a lower RSS and p99 at 10^5 tenants than Qdrant, LanceDB, pgvector and
   Elasticsearch in their own recommended multi-tenant setups. If they do not, the AI-retrieval
   position has no structural lead and the answer becomes Q1 (c).
2. **C2:** T4 shows hybrid quality at Elasticsearch's level with the BM25 leg at its bar.
3. **C3:** T7 and T9 (best QPS at fixed recall, streaming) are claimed only after R1 and R5; until
   then the product says "exact and filtered best, ANN competitive", not "best".

### The cheapest experiment that settles the open part

C1 needs no code of ours beyond what exists: load Cohere 1M × 768 [vdbb], split into 10^4 and
10^5 tenants by a Zipf size law, into Qdrant (one collection, payload tenancy, its recommended
shape), LanceDB (table per tenant), pgvector (partitioned table) and Elasticsearch (index per
tenant up to its limit), and measure RSS, files, and p99 of a top-10 query to a random tenant
under `quiet exclusive`. Beside it, this design's crossover gives our exact-scan time per
tenant size, and D1's idle-index measurement gives our per-index floor. One lead session of
harness work, a few hours of runs; if the field already serves 10^5 tenants within our
projected cost, the lead is narrow and Q1 should be (c).

This is beyond a design draft in two places, both named for a researcher: R1 (the ANN structure
that could top T7 without losing T3 and T6) and R3 (filtered ANN at scale with a full-text match
as the filter).


## What this builds on, and what it changes

| Document | What this design uses | What it changes |
|---|---|---|
| `product-landscape.md` (branch `design/landscape`, `fc57ebe`) | D3 (exact per small index plus RRF first, ANN later, `:154`); Q2's answer (`:437`); the vector survey (Table A, `:52-76`) | Replaces D3's "ANN by bake-off later" with D7's first approximate structure and R1 |
| `format-13.md` | F3's kind table and F23's adding rule (`:295-341`, `:1301-1374`); the freeze-readiness row "Vectors: an F23 plugin under an unlisted bit, kinds above 22, its alignment fixed by its first release" (`:1391`) | Fills that row: kinds 23 to 25, bit 17 (D3) |
| `layers.md` | The plugin concept (L4, `:346-416`), merge context (L6, `:474-520`), parts and the buffer (L10, `:731-806`) | Adds `vectors_plugin` and its part; no core change |
| `write-buffer.md` (`origin/design/write-modes`, revision 4) | Records in lanes (D3), reads at r (D5), freeze as an in-memory segment (D8), search reads at a freeze (D11) | Nothing: vectors ride D11's search path (D8 here) |
| prospect `phase-4.md` | The slot table (D3, `:97-128`), presence in `docset`'s frozen form (D4, `:157-176`), scored filters (D6, `:265-276`), query planning (D7, `:278-287`) | Adds `declare_vector` beside `declare_slot`; the vector scorer joins D6's contract |
| `limits.md` | The rule: a limit names its mechanism and is at least the largest reference engine's (`:1-7`) | Adds the vector rows (D2) |
| `small-indexes.md` | Directions 1 to 6 (`:20-44`) | Nothing: vectors add no per-index resident state (D4), which T3 checks |

## Baselines: the field's best, per axis

Gathered 2026-10-27 by a research agent through web fetches and paper PDFs (the limits of that
are the landscape's, `product-landscape.md` section 7); Lucene and Tantivy rows are checked in
their source here. "Vendor" marks a vendor's own claim. ann-benchmarks runs are one query at a
time on one CPU (r6i.16xlarge, hyperthreading off) [annb]; big-ann-benchmarks NeurIPS'23 runs on
8 vCPU and 16 GB (Azure D8lds_v5) at 10-recall@10 ≥ 0.9 [bigann].

| Axis | Best known | Number and configuration | Source |
|---|---|---|---|
| QPS at recall@10 0.95 / 0.99, sift-128 (1M) | NGT-QSG (`qsgngt`) | 18,098 / 11,163; then glass 15,171 (R 48, L 200), NGT-qg | [annb] |
| Same, glove-100 (1.18M, angular) | `qsgngt` at 0.95; ScaNN at 0.99 | 3,984; 991 (2,000 leaves, AVQ 0.2) | [annb] |
| Same, gist-960 (1M) | glass at 0.95; pynndescent at 0.99 | 569; 218 | [annb] |
| The same rows for systems we compare with | hnswlib, FAISS HNSW, Vamana, Lucene, Qdrant, Weaviate | sift-128 at 0.95: 5,065; 2,957; 6,463; 1,661; 845; 1,132 | [annb] |
| Filtered ANN, 10M (YFCC, 192-d `uint8`, tags) | ParlayANN `parlayivf` | 37,671 QPS (tag-aware IVF); Filtered-DiskANN 5,752; FAISS baseline 3,253 | [bigann] |
| Streaming updates (MS Turing 30M, 1 h runbook, 8 GB) | PyANNS (DiskANN with 8-bit codes) | recall 0.8865; DiskANN baseline 0.7218 | [bigann] |
| Database QPS, Cohere 1M × 768 (vendor-run) | Zilliz Cloud | 13,316 QPS, p99 2 ms, recall 0.938; Qdrant Cloud 1,242 at 0.947; Elastic Cloud 2,354 at 0.914 | [vdbb] |
| Smallest vector in RAM | RaBitQ family: Elasticsearch BBQ | d/8 + 14 bytes; 1M × 1024-d in 208 MB against 4,162 MB; recall over 0.9 at 3× oversampling (vendor) | [es-bbq] |
| Accuracy per bit | Extended RaBitQ | 4, 5, 7 bits per dimension give over 0.90, 0.95, 0.99 recall without reranking | [erabitq] |
| Cheapest build | Elasticsearch DiskBBQ (IVF, BBQ codes, SOAR spilling) | 1M vectors: 94 s at 0.91 recall against HNSW-BBQ's 1,054 s at 0.92 (vendor; dataset not stated) | [es-dbbq] |
| Quantized IVF per core | LanceDB `IVF_RQ` (RaBitQ) | 7-bit: 0.968 recall at 5.0 ms, one core, 24 probes (vendor) | [lance-rq] |
| Graph plus quantization | SymphonyQG | 1.5 to 4.5× the best baseline's QPS at 0.95, one thread | [symqg] |
| Merge cost, graph | Lucene 10.2 join-set merge (`MergingHnswGraphBuilder`) | about 30% faster merges than seeding from the largest graph alone; int8 force merge 113.8 s → 70.9 s (quora-E5) | [es-merge] |
| Update cost, disk | FreshDiskANN; SPFresh | 1,800 inserts and 1,800 deletes a second at near-billion scale, a 10% change merged at 10% of a rebuild; SPFresh 2.41× lower tail latency than rebuilding, on 10 GB and 2 cores | [fresh] [spfresh] |
| Filtered graph search | ACORN | 2 to 1,000× QPS over prior methods at fixed recall | [acorn] |

How the systems we meet handle the four things this design turns on:

| System | Exact path | Small segments | Quantization and merge | Filtered search |
|---|---|---|---|---|
| Lucene 10.4 | Exact when the filter admits at most the per-leaf k, or when the graph search visits too much (`AbstractKnnVectorQuery.java:221-242`) | No graph under `n > 100 ln n`, 648 vectors (`Lucene99HnswVectorsFormat.java:134`, `Lucene99HnswVectorsWriter.java:621-626`, `HnswGraphSearcher.java:56-57`) | 8, 4, 2 and 1-bit scalar codes (`Lucene104ScalarQuantizedVectorsFormat.java:119-150`); merges recompute the centroid and requantize every vector (`Lucene104ScalarQuantizedVectorsWriter.java:331-370`); graphs seeded from the largest input (`IncrementalHnswGraphMerger.java:44-142`) | ACORN-1-style expansion (`FilteredHnswGraphSearcher.java:30-42`) |
| Tantivy 0.26 | None | — | — | No vector search in core: no vector, kNN or HNSW code in its source (`grep -rli 'knn\|hnsw\|nearest.neighbo\|dense.?vector'` over `tantivy-0.26.0/src` finds nothing); ParadeDB's flat and IVF backends for it were closed unmerged (PRs 2972, 2973, 3019, 2026) [tv-pr]; Meilisearch uses arroy [arroy] |
| Elasticsearch 9.4 | `flat`, `int8_flat`, `bbq_flat` | — | HNSW and DiskBBQ (IVF) over int8, int4, BBQ; default `bbq_disk` for float where licensed [es-dv] | ACORN by default from 9.1 indices [es-acorn] |
| OpenSearch | faiss, lucene; nmslib deprecated | — | `on_disk`: faiss HNSW over 32× binary codes, rescoring at 2× oversample [os-disk] | Engine's |
| Qdrant | `exact=true`; the planner scans below `full_scan_threshold` | Planner | `float32`, `float16`, `uint8`; scalar, binary, product codes [qd-v] | Payload-aware extra edges; ACORN from 1.16, off by default [qd-f] |
| Weaviate | Brute force under `flatSearchCutOff` | — | BQ, SQ, PQ, RQ 8/4/1-bit [wv-q] | ACORN-1 since 1.27 [wv-acorn] |
| LanceDB | `bypass_vector_index()`; unindexed rows scanned [ld-vi] | — | IVF_PQ, IVF_RQ, IVF_HNSW_SQ [ld-vi] | Prefilter on IVF |
| pgvector | Exact by default [pgv] | — | `halfvec`, `bit`; HNSW, IVFFlat | Iterative index scans [pgv] |
| Turbopuffer | Unindexed writes scanned in the log | — | SPFresh centroid index on object storage [tp-a] | Exact filter indexes |


## D1. One vector plugin in prospect, kernels in a new library

| Piece | Where | Why |
|---|---|---|
| `vectors_plugin` (sections, builder, reader, merge, scorer) | prospect, `include/prospect/vectors.hh` | Scoring and queries are prospect's (`ARCHITECTURE.md:1-10`); the core never names a search type (`core-split.md`). A key-value index with vectors and no text is `layout<keys_plugin, stored_plugin, vectors_plugin>` |
| Distance kernels, quantizers, top-k selection | New library `Kronuz/vecdist` (working name) | Nothing in the catalog computes distances (`cpp-libraries/README.md`, 80 libraries; `AGENTS.md:162-164` lists `compressors`, `bitpack`, `docset` only). Kernels are reusable outside search, as `bitpack` is |
| Fusion (RRF, linear) | prospect, `include/prospect/fusion.hh` | One implementation used by the searcher and Xapiand's coordinator |
| DSL and the Elasticsearch subset | Xapiand `query_dsl`, the landscape's D7 adapter | Q3 of the landscape: our DSL canonical, ES as an adapter (`product-landscape.md:434`) |

Ported code keeps its notice: FAISS (MIT), hnswlib (Apache-2.0), SimSIMD (Apache-2.0) and Lucene
(Apache-2.0) are readable and portable; none is linked. A reference kernel library is
benchmark-only, as `bitpack` treats SIMD-BP128 (`PLAN.md:330`).

## D2. Vector fields: declaration and limits

A vector field is declared once by a replicated operation, `declare_vector`, beside D3's
`declare_slot` (prospect `phase-4.md:97-128`), in the transaction of its first document:

| Declaration field | Values |
|---|---|
| Field id | u32, the slot id space (`limits.md:32`) |
| Dimensions | 1 to 65,536 |
| Input element type | `f32`, `f16`, `bf16`, `i8`, `bit` (what clients send: Elasticsearch's `float`, `bfloat16`, `byte`, `bit` [es-dv]; Qdrant's `float32`, `float16`, `uint8` [qd-v]) |
| Similarity | `cosine` (normalized at ingest, then dot), `dot`, `l2`, `hamming` (bit only) |
| Scan code | `i8` (default for float input), `bit1` (RaBitQ-style, D3), or `none` (scan the stored input) |
| Keep full precision | `f32`, `f16`, or `no` (codes only: smallest index, rerank impossible) |
| Centroid | Optional, for `bit1`; fixed for the field's life (D3c) |

Redeclaring identically is a no-op; anything else is a deterministic rejection, as slots are.

| Limit | strata | Mechanism | Lucene 10.4 | Tantivy 0.26 | Others | Status |
|---|---|---|---|---|---|---|
| Dimensions | 65,536 | An `i8` dot product accumulates in `i32` lanes: 65,536 × 127² = 1.06 × 10^9, half of 2^31, so no kernel widens; at 133,144 it would overflow. A `bit1` code's popcount fits `u32`. Equal to the largest surveyed | 1,024 by default, codec-overridable (`KnnVectorsFormat.java:39`) | — (no vectors, below) | Qdrant 65,536 [qd-v]; pgvector 16,000 stored, 2,000 indexed [pgv]; Turbopuffer 10,752 [tp-l]; Elasticsearch 4,096 [es-dv] | Design |
| Vector fields per document | No count limit | Fields are rows of the section's directory, not section kinds | No fixed count | — | — | Design |
| Bytes of vectors per document | The replicated command (1 MiB default, 64 MiB ceiling, `limits.md:22`) | The command carries the document. One 65,536-d `f32` vector is 256 KiB | 2 GiB per document | — | — | Open, with document size (`limits.md:51-67`) |

## D3. Storage: a code column to scan, a precision column to rerank

### D3a. Sections

Three kinds under one new structure bit, added within format 13 by F23 (`format-13.md:1301-1374`):

| Kind | Id | Class | Holds |
|---|---:|---|---|
| `vector_codes` | 23 | regions | Per field: the scan codes, one fixed-stride row per present ordinal, in ordinal order |
| `vector_full` | 24 | regions | Per field kept at full precision: `f32` or `f16` rows, in ordinal order; read only to rerank |
| `vector_index` | 25 | pinned | The field directory: each field's declaration copy, presence (`docset`'s frozen form, as values use, prospect `phase-4.md:161-164`), row stride, column offsets, the code's parameters, statistics |
| reserved `vector_ann` | 26 | by its first release | D7's partitions; reserved now so the ANN step needs no new rule |

- **Bit 17, `vectors`**, owned by the `vectors` plugin (F4's mask per plugin, `layers.md:385-391`). An
  index lists it in its mask at its first `declare_vector`, so an index without vectors stays
  readable by a binary without the plugin (F23 rule 4); a binary without it refuses such an index
  as `unsupported_index` (rule 5).
- **Alignment** (F23 rule 2): each column starts 64-byte aligned and each row's stride is its
  code size rounded up to 16 bytes, so a kernel never splits a row across a column start and
  needs no tail loop for strides of 16; padding is zeros. At 384 dimensions `i8` and `bit1` rows
  need none. The cost is at most 15 bytes a row and 63 a column.
- **Digest** (F21, F23 rule 7): the full-precision input is content and gets its own records;
  codes are derived and get none.
- **Presence and rank:** a document without the field has no row; a present ordinal's row is its
  rank, exactly as values index columns.

### D3b. Codes

| Code | Row | Score | Rerank | Bytes per vector at 768-d |
|---|---|---|---|---|
| `i8` | d signed bytes and an `f32` scale | Σ aᵢqᵢ × scale_a × scale_q, `i32` accumulation | From `vector_full` | 768 + 4 (16-byte stride: 784) |
| `bit1` | d bits and two `f32` correction factors (RaBitQ's estimator) | Hamming or popcount inner product, corrected | From `vector_full` | 96 + 8 (stride 112) |
| `none` | The input as stored (`f32`, `f16`, `i8`, `bit`) | The input's kernel | Not needed | 3,072 (`f32`) |
| `vector_full` `f16` | d halves | — | — | 1,536 |

**Every code is self-contained per vector:** `i8` scales per vector, and `bit1` corrects per vector
against the field's fixed centroid (D3c). A merge then copies rows byte for byte (D9) and never
requantizes, which Lucene 10.4's scalar-quantized format does at every merge: it recomputes the merged
segment's centroid and requantizes every vector
(`Lucene104ScalarQuantizedVectorsWriter.java:331-370`).

### D3c. The `bit1` centroid

RaBitQ's error bound holds for any center, and its variance grows with the data's distance from
it [rabitq]. A per-segment centroid moves at each merge and forces requantization; a field
centroid fixed at declaration does not. Normalized embeddings (cosine) are centered near the
origin's direction, so the default is the zero centroid; a client may declare a centroid computed
from a sample. Whether a fixed centroid keeps recall within 0.5 points of per-segment centroids
on the gate datasets is R2, measured before `bit1` is the default anywhere.

## D4. Exact search first

```
query vector ─► per segment: filter docset ∧ live ─► code kernel over rows ─► top (k × o)
                buffer: frozen lanes, f32 rows ───────────────────────────────┘     │
                                                       rerank on vector_full ◄──────┘ ─► top k
```

- **Per segment** the scorer walks the field's present ordinals in order, intersected with the
  live docs and the query's filter (any prospect docset iterator: a full-text match, a term, a
  range), scores each row with the code kernel into a 256-entry buffer, and offers the buffer to
  a top-(k × o) selector; `o`, the oversampling factor, is 1 for `none`, 2 for `i8` and 4 for
  `bit1` by default, tuned by T1. The candidates are rescored from `vector_full` and the best k
  kept. Rows are read sequentially, so the region checksums verify on first read as every
  regions section does (`format-13.md:331-332`).
- **Filtered search is exact at every selectivity.** The vector scorer visits only ordinals the
  filter yields, so recall against brute force over the filtered set is 1.0 whatever the
  selectivity: there is no graph to fall out of, which is the recall cliff graph indexes manage
  with ACORN-style or filter-aware construction (Baselines). Cost is proportional to the
  matches, not the index.
- **Kernels** (`vecdist`): `f32` and `f16` dot and L2 with FMA, `i8` dot in `i32`, popcount for
  `bit`, with runtime dispatch: x86_64 AVX2 with FMA and F16C as the baseline, AVX-512 (VNNI's
  `vpdpbusd` for `i8`, `vpopcntq` for bits) where present; arm64 NEON as the baseline (`sdot` where
  `dotprod` is present, as on every Apple M-series and Graviton2+ part); a scalar fallback for
  everything. Query-side kernels may use FMA; ties break by docid (`ARCHITECTURE.md`, collectors).
- **Determinism.** Normalization and quantization run at stage 1 and at flush in one scalar,
  fixed-order routine compiled without FP contraction, so every replica writes identical codes
  from identical input on x86_64 and arm64 alike. Query-time scores may differ in the last ulp
  across platforms; results only differ when two scores tie to that ulp.

### Where exact stops being cheap

The microbenchmark (`docs/design/hybrid-search/vecbench.cc`, driver `run.sh`, summary
`report.py`, raw lines `vecbench-mac.jsonl`): one thread, top-10 by dot product over unit
vectors, rows contiguous as D3a lays them out, 256-entry score buffers. **Hot** re-queries one
index; **cold** queries a different copy each time from a pool of 256 MB, past the 16 MB L3, as a
host of many indexes does; **filtered** scores a random 10% or 1% of the ordinals. Host: the
development Mac, i9-9980HK (x86_64, AVX2, FMA, F16C, no AVX-512), macOS 15, Apple clang 17,
`-O3 -mavx2 -mfma -mf16c -mpopcnt`; run under `quiet strict` (macOS: exclusive) on 2026-10-27,
the timed process about 8 minutes. arm64 was compile-checked only (`-target arm64-apple-macos13`);
no arm64 host was available. Medians of at least 15 queries:

| µs a query, hot / cold | 1k × 384 | 10k × 384 | 1k × 768 | 10k × 768 | 100k × 768 | 10k × 1536 |
|---|---|---|---|---|---|---|
| `f32` scalar (no SIMD) | 331 | 3,612 | 683 | 7,232 | 73,528 | 14,797 |
| `f32` AVX2 | 31 / 92 | 623 / 923 | 54 / 186 | 1,610 / 1,833 | 19,235 | 3,574 / 4,064 |
| `f16` | 25 / 54 | 251 / 481 | 47 / 94 | 764 / 1,031 | 9,621 / 10,323 | 1,856 / 2,104 |
| `i8` | 16 / 28 | 145 / 272 | 28 / 53 | 281 / 511 | 5,159 / 5,215 | 845 / 1,003 |
| `bit` (1 bit a dimension) | 5.9 / 7.7 | 52 / 73 | 6.7 / 10.8 | 65 / 102 | 709 / 1,003 | 101 / 172 |

What it shows:

1. **Past the caches every code is bandwidth-bound:** 14 to 17 GB/s for `f32`, `f16` and `i8`
   at one thread here, so the cost of a query is the code bytes it reads. SIMD is 10.6× faster
   than the scalar loop for `f32` at 1k × 384, 11.5× at 1k × 1536 (hot).
2. **The crossover, at one thread on this host:** exact search stays under **1 ms a query while
   n × d stays under about 1.4 × 10^7 for `i8`**, 7.5 × 10^6 for `f16`, 4 × 10^6 for `f32`, and 5 to
   9 × 10^7 for 1-bit codes; ten times that under 10 ms. At 768 dimensions that is about 19,000
   vectors for `i8` and 99,000 for 1-bit codes at 1 ms, and 96,000 and 495,000 at 5 ms.
3. **A typical tenant is far below it:** 10,000 vectors at 768-d answer in 0.51 ms cold with `i8`
   and 0.10 ms with 1-bit codes, before reranking 40 candidates (an estimate of a few µs).
4. **Filtered cost follows the matches:** per scored vector a 1% filter costs 0.3 to 1.5× a full
   scan's, a 10% filter 0.9 to 1.9× (scattered rows defeat the prefetcher), so a 1% filter on
   100k × 768 `i8` scores in 37 µs.
5. **Cold small indexes cost 1.0 to 4.2× hot ones** at 100 to 1,000 vectors, which T8 sees at host
   scale.

**D7's threshold, proposed:** a segment's field builds partitions when its code scan passes
5 ms a query on the gate host, which here is n × d above 7 × 10^7 for `i8` and 4 × 10^8 for 1-bit
codes (about 500,000 vectors at 768-d). T2 re-measures on `main`, on arm64 and Linux, and sets the
preset. **Not measured here:** graph latency at the same sizes (T2 runs hnswlib beside it), so
whether exact also wins on latency, not only on cost, between 10^3 and 10^5 vectors is open.


## D5. Fusion: reciprocal rank and score blending

A hybrid query has legs (a text query, one or more vector queries) and a fusion:

| Fusion | Score | Parameters | Precedent |
|---|---|---|---|
| `rrf` | Σ over legs of 1 / (c + rankᵢ) | c (60 by default), window w per leg | Cormack et al. 2009 [rrf]; Elasticsearch, OpenSearch, Solr, Qdrant, Milvus, LanceDB, Turbopuffer, ParadeDB (`product-landscape.md:54-71`) |
| `linear` | Σ wᵢ × normᵢ(scoreᵢ) | Weights; normalization `minmax` over the leg's window, `zscore`, or `none` | Elasticsearch's linear retriever, OpenSearch's normalization processor, Qdrant's DBSF (a z-score variant) (`product-landscape.md:54-68`) |

- **Each leg runs on the same view** (`write-buffer.md` D5's read position r), so a document
  written between legs is either in both or in neither.
- **Distributed:** each shard returns each leg's top w with scores; the coordinator merges each
  leg's lists into its global top w, then fuses. Ranks in RRF and windows in `minmax` are global,
  so the fused result equals the single-shard result on the same data (the landscape's "equal
  across one shard and many", `PLAN.md:614`).
- **A document missing from a leg** contributes nothing to `rrf` and 0 after normalization to
  `linear`.
- **Filters apply to every leg** unless a leg names its own, so "vectors restricted to the
  full-text match" is a filter on the vector leg equal to the text leg's query, run as a
  non-scoring docset (`ARCHITECTURE.md`, read path 4).
- **Single-pass blending** (one scorer over BM25 plus a vector score, pruned with block-max
  bounds) needs a per-block upper bound on the vector score; with exact search the bound is a
  block's ball (centroid and radius), qᵀc + ‖q‖r. Whether that prunes enough to beat two legs plus
  fusion is R4, not a decision.

## D6. Query API

Xapiand's DSL stays canonical (the landscape's Q3 answer, `product-landscape.md:434`). A `_knn`
leaf joins the query language, and `_hybrid` names legs and a fusion:

```json
SEARCH /tenant-42/
{
  "_query": {
    "_hybrid": {
      "_legs": {
        "text":  { "body": "rotate signing keys" },
        "dense": { "_knn": { "_field": "embedding", "_vector": [0.12, -0.03, ...], "_k": 50,
                             "_filter": { "lang": "en" } } }
      },
      "_fusion": { "_rrf": { "_c": 60, "_window": 100 } }
    }
  },
  "_limit": 10
}
```

- `_knn` alone is an ordinary query (`"_query": {"_knn": {...}}`), and inside `_and` it is a
  scored filter (prospect `phase-4.md:265-276`). `_fusion` takes `_rrf` or
  `{"_linear": {"_weights": {"text": 0.3, "dense": 0.7}, "_normalize": "minmax"}}`.
- **The schema** gains a `vector` type: `{"embedding": {"_type": "vector", "_dims": 768,
  "_similarity": "cosine", "_code": "i8", "_full": "f16"}}`, which issues `declare_vector`.
- **The Elasticsearch subset** maps `dense_vector` (`dims`, `element_type`, `similarity`,
  `index_options.type`: `flat`, `int8_flat`, `bbq_flat` map to codes `none`, `i8`, `bit1`; the
  `*_hnsw` and `bbq_disk` types map to the same codes, with D7's partitions once they exist), the
  top-level `knn` clause (`field`, `query_vector`, `k`, `num_candidates` as the window, `filter`,
  `similarity`), and the `rrf` and `linear` retrievers [es-dv] [es-rrf].

## D7. Approximate later: partitions first

Judged by the many-small-indexes constraint: what an idle index holds, what a flush and a merge
pay, and how a filter and a cold read behave. Numbers are the Baselines'; per-vector sizes at
768-d are arithmetic.

| Structure | Idle index holds | Bytes a vector (768-d) | Build and merge | Filter | Cold query |
|---|---|---|---|---|---|
| HNSW (Lucene, hnswlib, Qdrant, Weaviate, ES) | Nothing if not resident, but searches then fault random pages | Codes plus 2M links of 4 B at layer 0: 128 B at M = 16, Lucene's default (`HnswGraphBuilder.java:50`) | ef_construction searches per insert; merges seed from the largest graph and still insert the rest (Lucene's best: about 30% faster [es-merge]); gist-960 took hnswlib 6,652 s [annb] | ACORN-style expansion, a cliff to manage | Random reads per hop |
| IVF over RaBitQ codes (DiskBBQ, LanceDB `IVF_RQ`, SPFresh) | Nothing; a centroid table read per query | 96 + 8 B (`bit1`) plus a 4-byte ordinal in its list | k-means on a sample and one assignment pass: DiskBBQ built 11× faster than HNSW-BBQ (vendor) [es-dbbq]; with fixed centroids a merge concatenates lists | A list is an ordinal run: intersect with the filter docset; tag-aware lists won big-ann's filtered track [bigann] | A few sequential reads |
| Vamana / DiskANN | Codes in RAM (32 B PQ), graph on SSD | About 348 B a vector on SSD at degree 92 (SIFT1B) [diskann] | Two passes of search and prune; FreshDiskANN merges at 10% of a rebuild [fresh] | Filtered-DiskANN's label edges | SSD round trips per hop |
| Quantized graph (SymphonyQG, PAG) | As HNSW | Codes plus links | As HNSW, faster per insert | As HNSW | As HNSW |

**Decision: IVF over the `bit1` codes, with `i8` or full-precision rerank, is the first
approximate structure** (kind 26, `vector_ann`), built per segment only where exact search stops
being cheap:

- **When it is needed:** a segment's exact scan of one field passes the budget the crossover
  table sets (below), at its code. Smaller segments, which the many-indexes workload is nearly
  all of, never build one: Lucene's own rule skips graphs under 648 vectors, and the crossover
  puts ours far higher because a code scan is cheaper than a graph's random walk.
- **Built at merge, never at flush,** on the merge's thread and budget, so ingest pays nothing and
  a merge of small segments stays a copy (D9).
- **Fixed centroids per field generation:** lists of a merged segment reuse its largest input's
  centroids when its quantization error has not drifted past a bound, so a merge concatenates and
  appends instead of re-clustering, which is SPFresh's local-rebalance idea at segment grain; R5
  measures the drift bound.
- **Spilling (SOAR)** assigns a vector to a second list where that improves recall, as ScaNN and
  DiskBBQ do [soar]; R1 decides whether it pays at our sizes.

**Why not HNSW first:** its links cost more than the `bit1` code itself, its merges insert every
vector again, and a cold graph reads randomly; it leads peak QPS on one hot, large index, which is
the axis we pick second (Q2). Whether a quantized graph should later join IVF for hot large
indexes is R1's bake-off; only one ANN structure ships ("One implementation per mechanism").

This part is research-grade. The rows where the field's best lead today (T7's QPS at fixed
recall, T9's streaming recall) are won, if at all, by R1 and R5, not by this decision alone.


## D8. The write path

| Stage | What happens to a vector |
|---|---|
| Stage 1 (validate) | `vectors_plugin::validate` checks the field is declared, the length equals its dimensions, and every component is finite; cosine fields are normalized and a zero vector is rejected. Rejections are deterministic, before stage 2 (`layers.md:777`) |
| Buffer | The part holds the input as declared (`f32`, `f16`, ...); `P::bytes` charges its bytes, so flush triggers and admission see it (`layers.md:368, 401-404`) |
| Search before a flush | Reads at a freeze (`write-buffer.md` D11): a frozen lane's vectors are a contiguous `f32` array per field, scanned exactly with the same kernels; visible on acknowledgement once the coalesced freeze passes |
| Flush | The builder writes `vector_full` (input at the kept precision), quantizes into `vector_codes`, and writes `vector_index`; rows in the flush's output order |
| Merge | Rows copied by the merge's permutation (`merge_context::order`, `layers.md:492-494`): byte copies of code and precision rows, dead rows dropped, no requantization (D3b). A merged output past D7's threshold builds partitions on the merge's thread, charged to its memory (`merge_options::memory`) |
| Delete, replace | Live docs, as every plugin: a replaced document's old row stays until the merge drops it; the scorer skips dead ordinals before scoring them |

The digest covers the input vectors (D3a), so replicas that apply the same log agree on vectors
byte for byte, and on codes by D4's determinism rule.

## D9. Merge cost

A copy merge of vectors is `memcpy` per row plus the presence rebuild: no distance is computed
and no graph is rebuilt. Elasticsearch documents HNSW merges as its dominant indexing cost and
Lucene seeds merged graphs from the largest input's graph to cut it (Baselines). For the
many-small-indexes workload, where nearly every segment stays under D7's threshold, merges stay
copies; T6 measures it.

## D10. Embeddings: none in the engine

strata and prospect store and search vectors; they never run a model. Xapiand may later offer an
optional embedder hook (Q4). What the hook would be, if the owner wants it:

- **At ingest, on the leader only,** before the document enters the log, so the vector is content
  replicated in the command and every replica stores the same bytes (an embedding computed per
  replica would differ across hardware and model versions and break the digest).
- **At query time,** for a `_text_to_vector` leaf.
- **Through an external HTTP service** with the OpenAI-compatible `/v1/embeddings` shape, never
  in-process: no GPU, ONNX, or tokenizer dependency enters the server.

## D11. What an index with vectors costs while idle

Nothing beyond its segments' files and the `vector_index` section, which is pinned and verified
at open (`format-13.md:331-332`) and holds only directory rows: no graph, no centroids, no
resident codes. An idle index's resident cost is then the landscape's T1 (`product-landscape.md:170`)
plus one directory row per field, an estimate of under 100 bytes a field. Codes and precision
rows are read through the page cache on first use. D7's partitions, when an index has them,
add only their centroid table (an estimate of √n × d bytes in `i8`: 1M vectors at 768-d is
about 0.8 MB, read on a query, not at open).

## D12. Not in this design

Multi-vector late interaction (ColBERT; Qdrant has `max_sim` [qd-v]), learned sparse vectors
(SPLADE; Milvus models BM25 itself as sparse vectors, `product-landscape.md:70`), GPU search,
reranking with cross-encoders, and object-storage residency (the landscape's D6). Each is a
later design; none changes D3's sections, since each adds kinds under its own bit.

## Gate rows

Measured by the lead on `main`, under `quiet exclusive`, on x86_64 and arm64 (Linux, plus macOS
for T2 and T3). Datasets are the ones the field reports on, so numbers compare directly:
sift-128, glove-100, gist-960 (ann-benchmarks' HDF5 files with their ground truth) [annb];
Cohere 1M × 768 (VectorDBBench's standard case) [vdbb]; YFCC-10M filtered and MS Turing 30M
streaming (big-ann NeurIPS'23) [bigann]; BEIR SciFact, NFCorpus, FiQA-2018, TREC-COVID and the 12
CQADupStack forums with Q6's embeddings for hybrid quality. Each row's "best" is the Baselines'.

| Row | Target | Best today | Mechanism | Procedure |
|---|---|---|---|---|
| T1 Recall against brute force (exact path) | recall@10 = 1.000 with code `none`; ≥ 0.999 with `i8` at o = 2; ≥ 0.995 with `bit1` at o = 4 (estimates T1 confirms or retunes o) | Exact is the ceiling; ANN systems report 0.90 to 0.99 | Rerank from `vector_full` (D4) | Split each dataset into indexes of 10^3 to 10^6 vectors; 1,000 queries; ground truth by an `f32` scalar scan; report recall@10 per code and o |
| T2 Latency per index size (exact) | At or below the best exact kernel, FAISS's flat scan and SimSIMD, in ns a vector at equal code and dimensions, one thread; the crossover table reproduced on `main`; hnswlib at the same sizes beside it | FAISS / SimSIMD (benchmark-only references) | `vecdist` kernels, row stride (D3a), buffered top-k | Step 1's `bench/` port of `.scratch/vecbench` with the references beside it; hot and cold (index pool past the LLC) |
| T3 Memory per idle index with vectors | An idle index with a vector field resident within 100 B of the same index without it; no descriptor or mapping added | No published per-tenant figure (`product-landscape.md` section 7) | No resident structure (D11) | The landscape's T1 procedure with a 768-d field at 1, 10, 100, 1,000 documents; Qdrant collection per tenant, LanceDB table per tenant, pgvector partition, ES index per tenant at 10^4 tenants beside it |
| T4 Hybrid quality | BEIR nDCG@10: the BM25 leg within 0.5 points of Elasticsearch's BM25; RRF ≥ max(BM25, dense) on every set and within 0.5 points of Elasticsearch's RRF on the same embeddings | Elasticsearch's RRF and linear retrievers | D5 | Index each BEIR set in Elysian and Elasticsearch 9.x with identical embeddings; `trec_eval` nDCG@10 and recall@100 |
| T5 Filtered recall and latency | Exact path: recall@10 = 1.000 at every selectivity from 0.01% to 100%, latency proportional to matches; with partitions, recall ≥ 0.95 at every selectivity | Lucene 10.4 ACORN-1, Qdrant payload edges, Weaviate ACORN (cliffs managed, not absent) | D4's filter-first scan | Cohere 1M with a synthetic attribute at 0.01% to 100% selectivity plus a BM25 match as filter; the same sweep on Lucene, Qdrant, Weaviate |
| T6 Build and merge cost | Ingest with vectors ≥ 0.9× ingest without, per thread; a merge of segments under the threshold at memory-copy speed (GB/s of rows); with partitions, build ≤ DiskBBQ's ratio to HNSW (11× cheaper) | Lucene 10.2 join-set merges; DiskBBQ's build (vendor) | Copy merges (D9), merge-time IVF (D7) | Cohere 1M ingest in 100k batches with periodic merges; CPU seconds and bytes per merge; Lucene 10.4 int8 HNSW and BBQ beside it |
| T7 QPS at fixed recall (with partitions) | At or above the ann-benchmarks leader per dataset at recall 0.95 and 0.99, one thread; Cohere 1M at or above Zilliz's 13,316 QPS at recall ≥ 0.94 on equal hardware | `qsgngt`, ScaNN, glass; Zilliz Cloud (vendor) | R1 | ann-benchmarks' harness and config; VectorDBBench's Cohere case |
| T8 Many tenants per host | 10^5 indexes (Zipf sizes, 10 to 10^5 vectors, 384-d), hybrid queries to random tenants: RSS and p99 below every compared system's at equal hardware | Turbopuffer claims unlimited namespaces, warm p50 14 ms at 1M (vendor, closed) [tp-a] [tp-l] | D11, exact under the crossover | The cheapest experiment (Fit verdict) on Elysian, Qdrant (payload tenancy), LanceDB, pgvector, Elasticsearch |
| T9 Streaming updates | big-ann's streaming runbook: recall ≥ 0.8865 within 8 GB | PyANNS | Buffer exact search plus R5 | big-ann-benchmarks' streaming track harness |
| T10 Determinism | Identical codes and digests from identical input on x86_64 and arm64 | — | D4's scalar quantizer | Step 1's golden file; step 2's digest test across CI platforms |


## Questions for the owner

Rulings (owner and lead, 2026-10-27): Q1 not yet (owner): the C1 field run (`docs/perf/c1-field-tenants.md`) shows shared-table
shapes already serve 10^5 small tenants cheaply; decide after the cold tier lands and C1 reruns
with strata and 1,000-vector tenants. Q2 per-index cost (memory, idle cost, small-index latency) wins over peak QPS
on one large index (owner). Q3 (a) our own `vecdist`, with SimSIMD as a benchmark to beat. Q4 (a)
no embedder hook now. Q5 (a) `i8` scan codes plus `f16` for rerank. Q6 chosen when T4 runs.

**Q1. The AI-retrieval direction.** (a) Go with the fit verdict's conditions; (b) go without
conditions; (c) no-go: vectors as table stakes only (exact plus fusion), no ANN program.
Recommendation: (a). Steps 1 to 3 are worth building under (c) too, so nothing waits on this
answer; the ANN program (step 5, R1) and the "best of all" claims do.

**Q2. The axis we pick where "best everywhere" conflicts.** (a) Memory and disk per vector,
per-idle-index cost, build and merge cost, filtered recall, and cold latency first; peak QPS on
one hot, large, RAM-resident index second; (b) peak QPS first, as ann-benchmarks ranks.
Recommendation: (a). The workload is millions of small indexes per host, most idle
(`small-indexes.md:3-8`); a structure that wins QPS by holding a graph in RAM loses the
per-host count we exist for. (a) still targets the QPS row (T7) through R1.

**Q3. The kernel library.** (a) A new Kronuz library (`vecdist`, working name) with our kernels
and quantizers, SimSIMD and FAISS as benchmark-only references; (b) depend on SimSIMD
(Apache-2.0); (c) kernels inside prospect. Recommendation: (a): the quantizers are
format-coupled (D3b, D4's determinism rule), the kernels are a few hundred lines per ISA, and
the catalog's rule is one small library per idea.

**Q4. An embedder hook.** (a) None now; revisit after T4 shows hybrid quality; (b) Xapiand calls
an external OpenAI-compatible embeddings service at ingest (leader only) and query (D10);
(c) in-process models. Recommendation: (a), then (b) if users ask; never (c).

**Q5. Default codes for a float field.** (a) `i8` scan codes plus `f16` kept for rerank;
(b) `bit1` plus `f16`; (c) `none` (scan `f32`). Recommendation: (a) until R2 shows `bit1` with a
fixed centroid within 0.5 recall points of per-segment centroids at oversample 4; then `bit1` for
768 dimensions and up, where it reads a quarter of `i8`'s bytes.

**Q6. The embedding model for the hybrid-quality datasets** (T4): BEIR's text needs embeddings
computed once, offline, outside the engine. (a) `bge-small-en-v1.5` (384-d, MIT) for every BEIR
set, plus the published Cohere and OpenAI sets VectorDBBench uses for the vector rows; (b) a
larger model. Recommendation: (a): small, open, reproducible on a laptop, and the 384-d row is
the many-small-indexes case.

## Steps

| Step | What | Tier (PLAN's review tiers) | Blocked by |
|---|---|---|---|
| 1 | `vecdist`: kernels, quantizers, top-k (below) | Shared library, larger change: one code review | Nothing (Q3's working name) |
| 2 | `vectors_plugin` exact path: D2's declaration, D3's sections, flush, copy merge, reader, the filtered scorer with rerank; fixtures; a decoder fuzz target | Format: proof plus the decoder-safety review | Step 1 |
| 3 | Fusion (D5), `_knn` and `_hybrid` in Xapiand's DSL, the ES subset's `dense_vector`, `knn`, `rrf` and `linear` (D6) | Shared public API: shape settled with the owner first | Step 2; Q4 for nothing |
| 4 | The lead runs T1 to T6 and T8 on `main` | Gate | Steps 2 and 3 |
| 5 | R1 (researcher), then D7's partitions under kind 26 | Format, research | Q1 (a) or (b), Q2 |

### Step 1, sized for one developer launch

`Kronuz/vecdist`, a standalone MIT library in the catalog's shape (CMake target
`vecdist::vecdist`, C++20, the shared CI workflow), with no strata or prospect change.
Proposed done criteria for the lead:

- K1 Kernels: `f32`, `f16`, `bf16` against an `f32` query (dot, L2); `i8` dot in `i32`; `bit`
  Hamming; RaBitQ-style `bit1` estimate. Paths: AVX2 with FMA and F16C, AVX-512 (VNNI,
  VPOPCNTDQ), NEON (with and without `dotprod`), scalar; runtime dispatch (`cpuid` on x86_64,
  `getauxval` on Linux arm64, `sysctl hw.optional.arm.FEAT_DotProd` on macOS).
- K2 Differential tests against the scalar reference on every path the CI host has: integer
  kernels exact; float kernels within a stated bound of the reference; dimensions 1 to 4,096
  and 65,536; unaligned inputs; strides of D3a.
- K3 Quantizers (`i8` per-vector scale, `bit1` with corrections against a given centroid),
  scalar and contraction-free; a golden file of codes checked byte-identical on every CI
  platform (x86_64 and arm64).
- K4 A top-k selector over score buffers, ties by ordinal ascending, with a randomized test
  against `std::partial_sort`.
- K5 The microbenchmark of `.scratch/vecbench` ported to `bench/`, with FAISS's flat scan and
  SimSIMD as benchmark-only references fetched only by a benchmark option; informational, the
  lead measures.
- K6 CI green on Linux (x86_64, arm64), macOS (arm64, x86_64), FreeBSD, ASan and UBSan; a
  `cpp-libraries` catalog entry.

## Research items

| Item | Question | Why it needs research | Who |
|---|---|---|---|
| R1 | The ANN structure that wins T7 without losing T3 and T6: IVF over RaBitQ codes with SOAR spilling, or a quantized graph (SymphonyQG, PAG) for hot large segments | The leaders (`qsgngt`, ScaNN, glass, ParlayANN) are tuned graph or partition engines; matching them while staying merge-cheap is unsolved in shipped systems | Researcher (one, launched after Q1) |
| R2 | Recall of `bit1` codes against a fixed field centroid against per-segment centroids | RaBitQ's bound holds for any center; the variance cost is unmeasured on real embeddings | Explorer, after step 1 |
| R3 | Filtered ANN at large scale: tag-aware lists (Puck, `parlayivf`) against ACORN expansion, with the full-text match set as the filter | Big-ann's filtered winners used tag-aware IVF; a BM25 match is not a tag | Researcher, with R1 |
| R4 | Single-pass blended top-k with block-max bounds for vector scores | Whether ball bounds prune enough to beat two legs plus fusion | Explorer, after step 3 |
| R5 | Centroid drift and list rebalancing across merges (SPFresh at segment grain) | Decides whether merges stay concatenations | With R1 |

## Sources

All accessed 2026-10-27 unless dated. "Vendor" marks a vendor's own claim. The research agent
read every row but `es-rrf`, `es-acorn`, `os-disk`, `qd-f` and `wv-q` by fetching it; those five
named pages its summary cited, and their links here are unverified.

| ID | Source |
|---|---|
| annb | [ann-benchmarks.com](https://ann-benchmarks.com/) and [its README](https://github.com/erikbern/ann-benchmarks) (r6i.16xlarge, one query at a time; README April 2025) |
| bigann | Simhadri et al., [Results of the big-ann-benchmarks NeurIPS'23 competition](https://arxiv.org/abs/2409.17424) |
| vdbb | [VectorDBBench leaderboard](https://zilliz.com/vdbbench-leaderboard) (vendor-run, undated) and [repository](https://github.com/zilliztech/VectorDBBench) |
| es-dv | [Elasticsearch dense_vector](https://www.elastic.co/docs/reference/elasticsearch/mapping-reference/dense-vector) (9.4) |
| es-rrf | [Elasticsearch retrievers](https://www.elastic.co/docs/reference/elasticsearch/rest-apis/retrievers) |
| es-bbq | [Better Binary Quantization](https://www.elastic.co/search-labs/blog/better-binary-quantization-lucene-elasticsearch) (2024-11-11, vendor) |
| es-dbbq | [DiskBBQ](https://www.elastic.co/search-labs/blog/diskbbq-elasticsearch-introduction) (2025-10-23, vendor) |
| es-merge | [Speeding up HNSW merges](https://www.elastic.co/search-labs/blog/hnsw-graphs-speed-up-merging) (vendor) |
| es-acorn | Elasticsearch ACORN-1 filtered kNN, default for 9.1 indices (vendor) |
| os-disk | [OpenSearch disk-based vector search](https://docs.opensearch.org/latest/vector-search/optimizing-storage/disk-based-vector-search/) |
| qd-v | [Qdrant vectors](https://qdrant.tech/documentation/concepts/vectors/) |
| qd-f | [Qdrant filtering and indexing](https://qdrant.tech/documentation/concepts/indexing/) |
| wv-q, wv-acorn | [Weaviate compression](https://docs.weaviate.io/weaviate/concepts/vector-quantization); [ACORN in Weaviate](https://weaviate.io/blog/speed-up-filtered-vector-search) (2024-11-19, vendor) |
| ld-vi, lance-rq | [LanceDB vector indexes](https://docs.lancedb.com/indexing/vector-index); [RaBitQ in LanceDB](https://lancedb.com/blog/rabitq-gets-faster-higher-recall-lower-latency-query-time-control) (vendor) |
| pgv | [pgvector README](https://github.com/pgvector/pgvector) |
| tp-a, tp-l | [Turbopuffer architecture](https://turbopuffer.com/docs/architecture), [limits](https://turbopuffer.com/docs/limits) (vendor) |
| tv-pr | Tantivy PRs [2972](https://github.com/quickwit-oss/tantivy/pull/2972), [2973](https://github.com/quickwit-oss/tantivy/pull/2973), [3019](https://github.com/quickwit-oss/tantivy/pull/3019) (2026, closed unmerged) |
| arroy | [meilisearch/arroy](https://github.com/meilisearch/arroy) |
| rabitq, erabitq | Gao and Long, [RaBitQ](https://arxiv.org/abs/2405.12497) (SIGMOD 2024); [Extended RaBitQ](https://arxiv.org/abs/2409.09913) (2024) |
| symqg | [SymphonyQG](https://arxiv.org/abs/2411.12229) (SIGMOD 2025) |
| soar | [SOAR](https://arxiv.org/abs/2404.00774) (NeurIPS 2023) |
| acorn | [ACORN](https://arxiv.org/abs/2403.04871) (SIGMOD 2024) |
| diskann, fresh, spfresh | [DiskANN](https://suhasjs.github.io/files/diskann_neurips19.pdf) (NeurIPS 2019); [FreshDiskANN](https://arxiv.org/abs/2105.09613); [SPFresh](https://arxiv.org/abs/2410.14452) (SOSP 2023) |
| rrf | Cormack, Clarke, Büttcher, Reciprocal rank fusion (SIGIR 2009) |

