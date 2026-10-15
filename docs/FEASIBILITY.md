# Feasibility: an MIT, async, replicated search engine for Xapiand

`strata` is a working name for the engine.

## Verdict

The engine is feasible and large. Every capability Xapiand uses from Xapian has a
published algorithm, and none of them requires GPL or Apache source. Reproducing
Xapian's *behavior* (term generation, query parsing, BM25 details, estimates) is the
long tail, and it runs through the clean-room procedure below.

The engine reaches Xapiand behind an engine seam that Phase 0.5 builds with Xapian as
its first implementation, so Xapiand keeps working at every phase. Xapian is removed
only after the engine passes the documentation E2E suite, the relevance-parity gate,
and the performance gates.

"Faster than Lucene and Xapian on every axis" is a target. [BENCHMARKS.md](BENCHMARKS.md)
splits it into must-win rows, which gate phases, and report rows, which are measured
and explained. A miss on a must-win row blocks its gate (PLAN.md, "Ratchets"); a
report row's miss is reported with numbers and a cause.

## Effort

Order-of-magnitude sizes, from the equivalent parts of Tantivy and Lucene:

| Component | Lines of C++ | Engineer-months |
|---|---:|---:|
| `bitpack`, `docset`, `fusefilter` (ordered keys are a map in strata, not a library) | 5k | 1.5 |
| `fst`, `termdict`, `automata` | 8k | 2.5 |
| `task`, `residency` (on `cluster/completion`) | 4k | 2 |
| `bkd` | 3k | 1 |
| `unicode`, `snowball` packaging, `analysis` (Xapian-compatible) | 6k + generated | 2.5 |
| `queryparser` (Xapian syntax and behavior) | 4k | 2 |
| `faultfs`, `benchkit` | 2.5k | 1 |
| Engine core: writer, segments, merge, manifest, scorers, collectors, aggregations | 30k | 9 |
| Track C (consensus gaps G1–G7) and `strata::group` | 18k | 8 |
| Xapiand seam, remote protocol replacement, async refactor | 12k changed | 5 |
| Track P: performance research, each item measured and kept or removed | 15k | 8 |
| Track D: membership, placement groups, simulator, fan-out, storage tiers, upgrades | 40k | 18 |
| Test parity with Lucene, Tantivy, and Xapian (inventory and ported tests) | 20k of tests | 5 |
| **Total** | **~170k** | **~66** |

Five programs run in parallel: the engine, Track C in the `cluster` repository, the
Xapiand seam with its asynchronous refactor, Track P, and Track D.

## The problems

### Ingestion throughput

On Xapiand's harness, Xapiand indexes about 7,800 docs/s and Elasticsearch 8.15 about
13,200 docs/s. Phase 0 measures where the time goes ([bench/phase0](../bench/phase0/README.md)):

- **Xapiand's RESTORE** of 100,000 Wikipedia documents runs at 964 docs/s, and Xapian
  accounts for 28.2% of it. Per-document shard check-out and check-in (lock convoys and
  futex wake-ups across shards), a per-document WAL deletion enqueue, and document
  preparation take most of the rest. Any engine bounds this path's speedup at 1.39×;
  the gap with Elasticsearch on Xapiand's harness is mostly Xapiand's own pipeline, and
  its fixes belong in Xapiand.
- **At engine level** on the 5M-document corpus, Xapian 2.1 indexes 881 docs/s on its one
  writer thread, against about 14,000 for Lucene 10.4 and Tantivy 0.26 with their
  multi-threaded writers. Xapian's write model is the engine-level gap:

| | Xapian (glass) | Lucene |
|---|---|---|
| Unit of write | Copy-on-write B+tree: a flush rewrites every touched block and its path to the root | Immutable segment, written once sequentially |
| Writers | One `WritableDatabase` per database | One in-memory segment per indexing thread |
| Posting update | Buffered changes merge into existing posting chunks at flush | A new segment; background merges later |

strata's write path is judged by the engine-level indexing rows against Lucene and
Tantivy; Xapiand's host fixes are judged by its own harness.

### Request offloading

Xapian's API is synchronous, so Xapiand runs every Xapian call on a worker pool and
posts the result back to the reactor.

The workspace rejected an asynchronous Xapian rewrite earlier
(`~/code/Kronuz/memory/kronuz-unstallable-event-loop.md`): asynchronous I/O only hides
I/O wait, and the CPU work still needs threads. That objection holds for asynchronous
I/O alone. The engine answers it differently: queries are time-sliced. A query runs
on the reactor that received it, yields every 100–250 µs between doc-id windows, and
light queries are scheduled ahead of demoted heavy ones. The same note measured light
queries at ~8 ms under offload versus ~450 ms inline under heavy load, in the June
`Kronuz/http` prototype and without a percentile. Phase 0 measures the light-query
p99 under offload on current Xapiand, and the time-sliced model must beat that
number (Phase 6 gate). Phase 0 measures the
cost of the offload hop itself, so the benefit is stated in numbers.

The guarantee is latency fairness with bounded reactor stalls, not throughput
isolation. Writes still cross to the shard's owner core as messages, flushes and
merges run in the background, and macOS reads complete through POSIX AIO.

### Durability and replication

Xapiand's replication is asynchronous and pull-based, and its default WAL is
asynchronous, so an acknowledged write can be lost when a node fails before the WAL is
flushed or a replica pulls it. The engine replicates every shard through a Raft group
(`cluster::consensus` with `kronuz::journal`) and acknowledges a write once a quorum
has it on disk by default. See [REPLICATION.md](REPLICATION.md).

## What Xapiand uses from Xapian

Every row needs an engine equivalent. Phase 0.5's engine seam becomes the
authoritative inventory.

| Xapian surface | What Xapiand does with it | Engine requirement |
|---|---|---|
| `Document` terms, positions, wdf | Text and keyword terms; per-field wdf weights; explicit positions via `add_posting` | Postings and positions; terms are opaque byte strings |
| Opaque term namespace | Terms are `prefix + ctype + value`; nested prefixes concatenate their parents'; "exists" is a wildcard on a field prefix; boolean terms have wdf 0; terms are capped at 245 bytes (Xapian's limit; strata's is 2^32 − 1, docs/design/limits.md) | Terms as opaque bytes with ordered iteration over any byte prefix |
| Stable docids, `get_lastdocid()`, docid order | Numeric auto-ids, `_docid`, `_last_id`, `_doc_del`, dump with `skip_to` resume, WAL ops by docid | A per-shard monotonic docid, never reused, stable across replace and merge; `lastdocid` in the manifest |
| Per-document term lists | ESet expansion, the `_terms` aggregation, the info endpoint (about 40 E2E assertions) | Opt-in stored term lists per field; re-analysis from the stored record otherwise |
| Values (`valueno`) | Slots are sparse xxh32 hashes; reserved slots 0–20; multi-value `StringList`s; value statistics over the remote protocol | Doc values keyed by 32-bit slot, multi-valued, sparse presence, per-column statistics |
| `Document` data | MessagePack record | Stored record |
| `replace_document(term)`, `DocumentInfo`, versions | Upsert by id with optimistic concurrency | Primary-key index, versioned updates, live version map |
| Metadata keys | Schema in `RESERVED_SCHEMA`, user metadata over HTTP, storage-locator keys | A key-value store versioned with the manifest |
| Transactions, `cancel_transaction` | Atomic changeset apply, `MSG_CANCEL` | Atomic multi-operation batches with cancel |
| Revision | +1 per commit, `_revision`, WAL replay keyed by revision | A commit counter, separate from seqno and version |
| Database UUID | Binds WAL and volumes to a shard; replica divergence; `_uuid` | Index identity in the manifest |
| Query ops: AND, OR, AND_NOT, XOR, FILTER, AND_MAYBE, SYNONYM, ELITE_SET, MAX, SCALE_WEIGHT, PHRASE, NEAR, WILDCARD, EDIT_DISTANCE | Query DSL | Scorers with Xapian semantics, including `OP_SYNONYM` statistics and `ELITE_SET` selection |
| `ValuePostingSource` subclasses | Ranges; geo as interval sets weighted by centroid angle; serialized to remotes | Scored-filter iterators with `weight()` and `max_weight()`; geo as 2-D interval sets |
| `KeyMaker`, `MultiValueKeyMaker` | Sort keys computed from string metrics, geo distance, closest or farthest value | A serializable sort-key function API over doc values |
| `Enquire` | `check_at_least`, docid tie-break, `collapse_max`, `_rank`, `_weight`, `_percent`, `get_matches_estimated()` asserted exactly by E2E | Xapian's estimate triple, exact counting on request, Xapian's percent formula |
| `MatchSpy` | Aggregations over the documents the matcher considers | Exhaustive columnar aggregation (a documented behavior change) |
| `RSet`, `get_eset`, `ExpandDecider` | `nearest` and `fuzzy` expansion | Relevance-feedback expansion over stored term lists |
| `TermGenerator`, `Stem`, `Stopper` | Word rules, `Z` stems, stop strategies, CJK n-grams, ICU word breaks | A Xapian-compatible analyzer as the default |
| `QueryParser` | Parses text-field values with phrase, love/hate, fuzzy, boolean, wildcard, partial, CJK flags | A Xapian-syntax `queryparser` |
| Weights | Default BM25, `BoolWeight` | Xapian's exact BM25 formula; Bool |
| Statistics | doccount, average and total length, doclength bounds, collection freq, termfreq, unique terms, wdf docmax, per-document length | Exact `N`, total length, and length bounds; termfreq semantics after deletes per ARCHITECTURE |
| `Database::check` | Exposed over HTTP | A verify API over checksums and structure |
| Remote protocol (about 40 messages) | Distributed search, remote fetch, write forwarding | A shard RPC carrying queries, scored filters, sort keys, and aggregations |
| Durability coupling | `DB_NO_SYNC` behind the WAL, `DB_RETRY_LOCK`, `Database::refs()` | Replaced by [REPLICATION.md](REPLICATION.md) |

Spelling and synonym tables are out of scope: no HTTP path adds spellings or
synonyms, and the flags appear only in a debug printer.

The engine adds capabilities Xapiand's features can use:

- Block-max MaxScore and WAND for top-k disjunctions.
- Columnar doc values, so sorting and aggregations do not load documents.
- BKD points for numeric ranges and interval sets.
- Near-real-time refresh without a commit.
- Snapshots that never change under a reader, which removes the `DB_RETRIES` and
  `reopen()` loops.
- Exhaustive aggregations; today relevance-ordered queries aggregate over a pruned
  subset.
- Query cancellation and per-query time limits at yield points.
- Checksummed segments and manifests, which give real corruption detection.

## Constraints

### License policy

- The engine is MIT.
- No copyleft dependencies.
- No code derived from Xapian or Lucene sources.
- Permissive dependencies only, each with its notice shipped in `NOTICE`: Snowball
  (BSD-3-Clause), Unicode data (Unicode License), optionally ICU (Unicode License),
  and Apache-2.0 libraries such as `cpp-btree`.
- Every Kronuz library the engine depends on carries a license file and license
  headers.

### Clean-room procedure

Compatibility with Xapian behavior follows one procedure:

1. A behavior specification is written from Xapian's public documentation and from
   observed output. It never quotes Xapian source.
2. A separate GPL-side oracle harness, kept outside the strata repository and never
   shipped, runs Xapian 2.x on fixed inputs and records outputs as test vectors:
   `TermGenerator` terms and positions, `QueryParser` query descriptions, stemmer
   vocabularies, BM25 weights, `_percent`, and match estimates.
3. Strata implements the specification and passes the vectors.

### GPL-derived code in Xapiand outside `src/xapian/`

Phase 8 replaces these too:

- `src/sortable_serialise.{cc,h}` (GPL, Olly Betts)
- `src/server/remote_protocol_views.{cc,h}` (GPL)
- `src/xapian.h` (GPL)
- `src/server/remote_protocol_service.h` (MIT, but includes the GPL
  `xapian/common/pack.h`)

`src/length.{cc,h}` is MIT: Xapiand commit `1691e5b2e` records Olly Betts'
permission, and the files carry the MIT notice with his copyright.

### Toolchain

C++20 on Apple clang, Homebrew LLVM, and Linux GCC and clang. SIMD kernels target
SSE4.1, AVX2, and NEON with a scalar fallback, over one fixed on-disk lane layout. Gates run as
same-session ratio comparisons in Linux and FreeBSD QEMU virtual machines on the
development Mac, with a bare-metal confirmation before Phase 7 (see
[BENCHMARKS.md](BENCHMARKS.md)).

### Compatibility

Xapiand's HTTP API and its documentation E2E suite are the contract, plus a
relevance-parity gate the E2E suite does not cover. On-disk compatibility with Xapian
databases is not a goal; migration runs through DUMP and RESTORE.

## Risks

| Risk | Mitigation |
|---|---|
| Ingestion turns out host-bound | Phase 0 profile first; host fixes land in Xapiand regardless |
| Behavioral long tail (TermGenerator, QueryParser, estimates, `OP_SYNONYM`) | Oracle vectors; analyzer moves into Phase 2 |
| Blocking primitives inside Xapiand (pool condvars, storage `pread`, fsyncs, schema locks, Lua) | Phase 7c removes pools only after these are asynchronous |
| Multi-group consensus cost at thousands of shards per node | Track C prototype measures fsyncs per second at 1,000 groups |
| Replica-count semantics under Raft | Owner decision, recorded in REPLICATION.md |
| Benchmark noise on the development laptop | Linux gate machine with pinned frequency |
| Statistics drift on update-heavy small indexes | Exact `N` and lengths; drift measured in the parity gate |
| Merge write amplification with stored records | Merges copy compressed blocks without recompressing |
| File-descriptor and fsync growth | Single-file segments; one journal per core |
| Determinism leaks (scripts, foreign schemas, ids) | Leader resolves nondeterminism; replica digest tests |
| Cooperative yielding overhead | Budget checked once per window; overhead measured |
