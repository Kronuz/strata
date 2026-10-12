# strata plan

Work proceeds in phases. The architect reviewer approves each gate before the next
phase starts. Every gate also requires:

- Tests passing under ASan and UBSan, and TSan where threads are involved.
- At least one CPU-hour of fuzzing per decoder and parser added in the phase, with
  zero findings.
- Readers that reject unknown format versions, and a fixture for every format change.

Numbers follow the rules in [BENCHMARKS.md](BENCHMARKS.md).

## Phase 0: baselines and evidence

- strata's Linux (8 vCPUs, 16 GB) and FreeBSD QEMU virtual machines, and `benchkit`'s
  interleaved-trial ratio runner.
- Benchmark harnesses for Xapian 2.x (direct, and driven Xapiand's way), Lucene, and
  Tantivy on Wikipedia and the Xapiand-shaped corpus; the Xapiand and Elasticsearch
  system-level harness with concurrent clients.
- A flame-graph profile of Xapiand's RESTORE and query paths, attributing time to
  Xapian calls versus host code.
- The offload hop cost on the current build: the same query inline versus offloaded.
- Light-query p99 under heavy load on current Xapiand (the Phase 6 baseline).
- The VM configuration and a measured in-VM fsync latency, recorded with every
  baseline.
- A dump of Lucene's FOR and PFOR postings on Wikipedia for the `bitpack` size target.
- A GPL-side oracle harness, outside the strata repository, recording Xapian vectors:
  `TermGenerator` output, `QueryParser` descriptions, stemmer vocabularies, BM25
  weights, `_percent`, and estimates.

Gate: reproducible baseline JSON for every system; the profile's attribution of the
ingestion gap; the measured hop cost.

## Phase 0.5: the Xapiand engine seam

- Every Xapian call in Xapiand goes through an engine interface, with Xapian as its
  first implementation. The interface is the authoritative inventory.

Gate: zero E2E assertion regressions against the pre-seam build (passing assertions
out of 303 requests); harness throughput within the noise band.

## Phase 1: foundation libraries

APIs frozen at the gate:

- `bitpack`: FOR encode and decode of a 128-integer block at a given bit width, delta
  decode with a base, PFOR with exceptions, VByte tails, the block header, and the
  no-wdf and all-ones flags.
- `fst`: sorted-input `Builder::insert(key, output)` and `finish()`; `get`, `floor`,
  range streams, `intersect(automaton)`.
- `docset`: the iterator concept (`doc`, `next`, `advance`, `cost`); dense and sparse
  sets; in-place AND, OR, ANDNOT; cardinality.
- `task`: `task<T>`, the scheduler and I/O concepts, yield with a budget, `when_all`,
  cancellation, and an Asio adapter in a separate target.
- The scorer iterator concept and the residency interface with a synchronous backend.
- CRC32C in `hashes`, hardware-accelerated (SSE4.2, ARMv8), moved from
  `journal/codec.h`.

Tests:

- Round-trips for every bit width from 0 to 32 at every block position.
- Automaton intersection against brute force on 100,000 random patterns.
- `task`: executor affinity, cancellation at yield, exception propagation,
  10^6-deep symmetric transfer in constant stack, and 10^8 tasks with random
  cancellation under ASan, UBSan, and TSan.

Benchmark targets, each relative to a reference measured in the same run:

| Library | Target |
|---|---|
| Tails | `varint` tail size and decode speed against LEB128 and StreamVByte on the Wikipedia postings tails; a smaller encoding lands in `varint` |
| `bitpack` | Decode with delta at ≥ 90% of a reference SIMD-BP128/D1 (benchmark-only, never linked) and ≥ 3× scalar; encoded size within 2% of Lucene's on the same postings |
| `fst` | Lookup p50 within 2× an open-addressing hash map on the Wikipedia vocabulary at ≤ 25% of its bytes; build rate and bytes per term reported against a front-coded sorted array |
| `docset` | Within 1.2× of CRoaring (benchmark-only) on AND, OR, ANDNOT, and iteration for densities 10^-6 to 0.5 |
| `task` | Create, await, and destroy of a no-op task ≤ 30 ns with zero `malloc`; yield-and-resume through the Asio binding ≤ 1 µs p99 on an idle loop |

## Phase 2: single-segment engine

- In-memory segment writer and flush; the segment file with term blocks, postings,
  positions, and lengths.
- Segment reader on the residency interface; term, boolean, phrase, and near queries;
  BM25 with Xapian's formula and a Lucene-compatible BM25 for equal-scoring
  comparisons; block-max MaxScore and WAND; top-k collection; exact counting.
- `termdict`: the block-tree term dictionary over `fst`.
- The Xapian-compatible analyzer for text fields, checked against the oracle vectors.
- Scorer contract additions: the minimum competitive score passed into nested scorers;
  64 KiB region checksums reaching `residency`.

Gate (the harness's query classes; the per-class reference is the faster of Lucene
and Tantivy; ratios are reported both raw and with the ~155 µs pipe floor subtracted):

- Xapian is a floor: must win on every class and on single-thread indexing.
- `TOP_10`, `TOP_100`, `TOP_1000` (Lucene-compatible BM25): within 1.5× of the
  reference per class, or the gap explained before Phase 3; a class beyond 2× blocks the
  gate.
- `COUNT` and `TOP_100_COUNT`: within 1.5× of the reference per class.
- The term class gains 200 single-term queries drawn from the vocabulary, stratified
  by document frequency (rare, medium, frequent).
- Single-thread indexing: ≥ 1.0× Lucene and ≥ 0.8× Tantivy with equal settings
  (positions on, matching stored fields), measured from Phase 0 single-thread runs of
  both; Phase 3 ratchets to ≥ 1.0× Tantivy.
- Postings size: the whole postings section ≤ 1.02× Lucene's `.doc` on Wikipedia.
- `termdict` exact lookup (present and absent keys): ≤ 1.0× Lucene's `seekExact`.
- Relevance parity on text fields: identical boolean match sets against the oracle;
  identical BM25 top-10 without deletes with relative score error ≤ 1e-9.

## Phase 3: index lifecycle

- Multiple segments, snapshots, refresh, commit manifests, tiered merges.
- Primary keys, docids, updates, deletes, versions, the live version map, metadata,
  atomic batches with cancel, the node-wide memory budget.
- Deterministic `apply`, `checkpoint`, and `install`; a logical-content digest.
- `faultfs`.

Gate:

- Power-loss injection at every barrier of a randomized workload, across at least
  10,000 crash points, with zero acknowledged-write loss and zero inconsistent opens.
- The digest is equal across two replays.
- Write amplification reported against Lucene's tiered policy at equal segment
  budgets; multi-thread indexing must-win rows.
- Disk full: fail-closed writes, no corruption, recovery once space returns.

## Track C: consensus gaps (parallel with Phases 1–3)

Track C changes `cluster/consensus`, `cluster/journal`, and `cluster/completion`,
which Detent depends on and which `~/code/Detent/docs/LIBRARY_BOUNDARIES.md` plans to
split into `completion`, `journal`, and `consensus` repositories, with Detent's TLS
peer transport moving into `consensus`. Track C starts after coordination with the
Detent work, and lands in whichever repositories exist at that point. C4 reuses the
Detent peer transport instead of writing a new one.

Ordering against the Detent split plan's steps:

- Design work and Linux NVMe measurements start now.
- Track C code starts after split steps 1–2 (`metrics`; the `completion`,
  `journal`, and `consensus` repositories and the `kronuz::consensus` rename).
- C4 starts after step 3 (the peer transport in `consensus`).
- Phase 3 needs `ledger` (step 5); if it is not extracted by then, strata extracts
  it.
- Step 10 (keep or retire `cluster::Raft`) and strata's placement group are one
  decision.

- C1: membership changes and voter roles (learners, optional witness).
- C2: multi-group design and prototype: one journal per core, group commit across
  groups, coalesced heartbeats, quiescent groups, forced checkpoints for laggards.
  Measure fsyncs per second per node at 1,000 groups.
- C3: repair at or below the commit cursor.
- C4: stream transport with flow control (TLS before production).
- C5: a deterministic simulation harness with a linearizability checker.

Gate per item: tests and measurements in the `cluster` repository, reviewed by the
architect.

## Phase 3R: replication groups

Starts after C1–C4 pass, which places it after split steps 1–3.

- `strata::group`: journal checkpoints reference strata manifests; consistency modes;
  document-mode replication; repair; scrubber.

Gate:

- At least 10^7 simulated steps over at least 1,000 seeds each for 3 and 5 voters,
  with crashes, power loss at every barrier (journal, segment, manifest), partitions,
  clock skew, membership changes, and corruption of every file class.
- The linearizability checker passes over write and get-by-id histories.
- Zero `quorum`-acknowledged loss; zero served corruption; replica rebuild after disk
  loss.
- Quorum-ack ingest against Elasticsearch 8.x with one replica, with one sequential
  client and with 16 concurrent clients; p99 acknowledgement latency on Linux NVMe;
  fsyncs per second per node at 1,000 groups within budget.

## Phase 4: structured data

- `sortable`, doc values by slot, `bkd` (1-D and 2-D interval), scored filters,
  computed sort keys, collapse, exhaustive mode, estimate triples, `_percent`.
- Aggregation collectors (terms, histogram, date histogram, range, stats, term-list
  based `_terms`).

Gate: filtered, sort, range, geo, and aggregation classes must win against Xapian
and stay within 1.25× of Lucene; operator parity against oracle vectors.

## Phase 5: analysis and query language long tail

- All Snowball languages, CJK n-grams, ICU plugin, UAX #29 analyzer.
- `automata`: wildcard, prefix, fuzzy.
- `queryparser`.
- Relevance-feedback expansion.

Gate: prefix, wildcard, fuzzy, and expansion classes must win against Xapian and
stay within 1.25× of Lucene; per-language stemmer
vocabularies equal to Xapian's; `queryparser` vectors pass.

## Phase 6: async backends and tuning

- io_uring and POSIX AIO residency backends; the verified block cache; scheduling
  tuning.

Gate: reactor p99 stall ≤ 1 ms under the mixed heavy and light load; light-query p99
under heavy load ≤ 2× its unloaded p99 plus 1 ms (against the light-query
p99 under offload that Phase 0 measures); major reactor faults per 1,000 queries ≤ 1 with the index at twice RAM.

## Phase 7: Xapiand integration

- **7a.** Strata behind the seam at `rf = 1`, with Xapiand's WAL.
- **7b.** Shards become groups: the journal replaces the WAL, the group replaces the
  replication protocol and `ELECT_PRIMARY`, placement moves to the consensus
  authority, the shard RPC replaces the remote protocol, and the HTTP API exposes
  `ack`, `read`, and `refresh`.
- **7c.** Xapiand's blocking primitives become asynchronous (pool checkout, storage
  reads, fsyncs, schema locks, Lua), and the offload pools are removed.

Gate: zero E2E regressions; `harness/cluster_check.sh` and a power-loss replacement
for `harness/wal_recovery_check.sh` pass; the harness beats current Xapiand on every
row and Elasticsearch on the claimed rows.

## Phase 8: removal

- Remove `src/xapian/` and every Xapian reference.
- Replace GPL files outside it: `sortable_serialise` (with `sortable`),
  `remote_protocol_views` (with the shard RPC), `xapian.h`, and the `pack.h`
  include in `remote_protocol_service.h`.
- Rename the project.

Gate: clean build without Xapian; all suites green; a license audit of every
dependency.
