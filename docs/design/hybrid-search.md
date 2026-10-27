> Status: DRAFT (2026-10-27) Revision 0, ready for preliminary review; evidence for the owner's go/no-go on AI retrieval, not a commitment.

# Hybrid search: full text plus vectors for AI retrieval

Revision 0, 2026-10-27. State: designing (ready for preliminary review). Workstream `hybrid-search`
(ledger `docs/status/hybrid-search.md`). Decisions D1 to D12, gate rows T1 to T10, owner
questions Q1 to Q6, research items R1 to R5. Code is cited at strata `98ce743`, prospect
`9de01a1`, Xapiand `da26127a3`; Lucene 10.4.0 (`9983b7c`, 2026-02-20) and Tantivy 0.26.0 from
local source trees (Apache-2.0, MIT). Web sources were read on 2026-10-27 ([Sources](#sources)).
No GPL source was opened. The one measurement here is a throwaway single-thread microbenchmark
(`.scratch/vecbench/`, raw lines in `docs/design/hybrid-search/`); every other number is quoted
from a cited source or marked as an estimate.

The owner's direction (2026-10-27): steer toward AI retrieval only where we are a clear fit, with
full-text quality at the Elasticsearch and Quickwit bar; and the goal for vectors is the best
vector search of all, judged against the field's best systems on each axis.

<<FIT>>

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

<<BASELINES>>

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

<<CROSSOVER>>

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

<<ANN>>

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

<<GATES>>

## Questions for the owner

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

<<SOURCES>>
