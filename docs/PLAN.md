# strata plan

Work proceeds in phases. The architect reviewer approves each gate before the next
phase starts.

## Goals

1. **Beat every baseline by a considerable margin, and stay correct.** strata beats
   Lucene, Tantivy, Xapian, Elasticsearch, and Quickwit on query speed, indexing
   speed, memory, and disk space, by the margins of the ratchets below: by Phase 6,
   queries at ≤ 0.7× the faster engine's time, indexing at ≥ 1.3× its throughput,
   memory at ≤ 0.7× and disk at ≤ 0.8× the smaller engine's footprint. Each baseline
   runs in its fastest supported configuration (for Lucene, that includes document
   reordering). None of this costs correctness: pruned results equal exhaustive ones,
   scores match the oracle, and every gate re-proves both.
2. **Literature and novel design.** Every known technique that applies is evaluated:
   from published work, from the baselines, and from Xapian. strata also designs its
   own techniques where its format allows something the baselines cannot do. Each idea
   lands only with a before/after measurement, and stays only if it wins. Goal 2 is
   checked against Track P's list, which grows by review; each item ends in a
   `docs/perf/` entry with its four-axis table, kept or removed.
3. **Distributed-first.** The engine is designed for clusters of hundreds of
   thousands of nodes from the start, not retrofitted. It is fully featured like
   Elasticsearch and Quickwit: replicated, rebalanced, and queried across nodes. Every
   design decision states its behavior at that scale. [DISTRIBUTION.md](DISTRIBUTION.md)
   holds the design, and Xapiand's own design is adopted only where it holds up at that
   scale. Its UDP discovery and Raft give way to the consensus stack built for Detent.
4. **Test parity and coverage.** strata's tests cover everything the Lucene, Tantivy,
   and Xapian test suites cover, with measured coverage targets.
   [TESTING.md](TESTING.md) holds the inventory and the rules.
5. **Memory, tracked and minimized, with no garbage collector.** Memory is a
   must-win row at every gate, measured against every baseline (ARCHITECTURE.md,
   "Memory"). Nothing in strata, or in a library it uses, reclaims memory with a
   tracing collector, a collector thread, or deferred reclamation of unbounded work.
   Every object has an owner and is freed deterministically. Work that deletes
   obsolete files after merges, checkpoints, and log truncation is called
   reclamation: it runs in bounded slices, and its cost is measured like any other
   work.

## Ratchets

Each gate's targets on the four axes, measured as BENCHMARKS.md, "Ratchet rows",
defines them.

- **Baselines.** Each gate fixes its baseline set with pinned versions: Lucene,
  Lucene with document reordering (from Phase 3), and Tantivy. Every engine runs the
  same corpus, analysis, positions, stored fields, norms, and keys (BENCHMARKS.md,
  "Equal content").
- **Reference.** Per axis, the best figure in the set: the fastest engine per query
  class, the highest indexing throughput, the smallest memory and disk. strata meets
  every axis in one configuration.
- **Passing.** Ratios are strata over the reference. Timed rows take the median of at
  least five interleaved trials (from Phase 3; the Phase 2 indexing row ran three)
  with the 95% bootstrap interval, and the interval's bound on the target's side meets
  the target: the upper bound for ≤ targets, the lower bound for ≥ targets. Queries are
  floor-subtracted. Disk rows are single exact measurements. A row that misses its
  target blocks its gate.
- **Margins.** "By the same margin" at Phase 7 means Phase 6's ratios against
  Elasticsearch and Quickwit: queries ≤ 0.7×, indexing ≥ 1.3×, memory ≤ 0.7×, disk
  ≤ 0.8×.

Every technique reports its effect on all four axes. One that wins an axis may not
push another past its ratchet, and a trade between axes is stated with both sides'
numbers.

### Query speed

| Gate | Every query class, every command |
|---|---|
| Phase 2 | ≤ 1.5× (≤ 2× blocks) |
| Phase 3 | ≤ 1.0× |
| Phase 4, Phase 5 | ≤ 1.0× on the classes they add |
| Phase 6 | ≤ 0.7× on every class |
| Phase 7 | The claimed system rows beat Elasticsearch and Quickwit by the same margin |

### Indexing speed

| Gate | Single-thread and multi-thread throughput |
|---|---|
| Phase 2 | ≥ 1.0× Lucene, ≥ 0.8× Tantivy |
| Phase 3 | ≥ 1.0× the faster of Lucene and Tantivy |
| Phase 6 | ≥ 1.3× |

### Memory

| Gate | Rows |
|---|---|
| Phase 2 | Recorded: indexing peak RSS, serving RSS with the index open, and heap bytes and allocations per query by class |
| Phase 3 | Indexing memory within the node budget plus 10%, and ≤ 1.0×; serving memory ≤ 1.0×; allocations per query do not grow with the number of matches (a property row, without a baseline) |
| Phase 6 | Indexing and serving memory ≤ 0.7×; zero heap allocations per query in steady state on the term, conjunction, disjunction, and phrase paths (per-core arenas and pools) |
| Phase 7 | Per-node memory for cluster state stated as a function of cluster size and measured at the scale contract; serving memory beats Elasticsearch and Quickwit by the same margin |

### Disk

Whole index after a full merge, and each section against its counterpart in each
engine, as BENCHMARKS.md's counterpart table places them.

| Gate | Rows |
|---|---|
| Phase 2 | Whole index recorded; postings ≤ 1.02× `.doc` |
| Phase 3 | Whole index ≤ 1.0×, and every section ≤ 1.0× its counterpart (the term dictionary's 1.5× target in the Phase 2 gate report is superseded) |
| Phase 6 | Whole index ≤ 0.8×, and every section ≤ 0.9× |
| Phase 7 | Bytes per document on disk, the journal included, beat Elasticsearch and Quickwit by the same margin |

## Every gate

- Tests passing under ASan and UBSan, and TSan where threads are involved.
- One CPU-hour of fuzzing, at the gate's commit, for each decoder and parser added or
  changed in the phase, with zero findings and a hang timeout; every other decoder's
  checked-in corpus replayed as a test.
- Readers that reject unknown format versions, and a fixture for every format change.
- Line and branch coverage of the phase's code (every file and function added or
  changed in the phase) at or above TESTING.md's targets, and that phase's rows of the
  test-parity inventory ported and passing.
- A statement, for each component the phase adds, of its behavior at the target
  cluster scale (DISTRIBUTION.md, "Scale contract").

Track items (C, P, and D) meet these for the code they add. Numbers follow the rules
in [BENCHMARKS.md](BENCHMARKS.md).

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
and Tantivy; ratios are reported both raw and with the pipe floor subtracted, as
BENCHMARKS.md defines it):

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
- **Format 12**, one version for all of Phase 3's format work, frozen before merge
  code, with its fixture and a test that format 11 is rejected: `pk_filter`,
  `versions`, and the docids section; a docids kind for reordered segments (a
  per-segment flag, a permutation, and a sorted docid-to-ordinal index; the
  equal-bound tie skip applies only to docid-ordered segments), so merges can reorder
  (P1) while flushes keep docid order; the stored section's largest frame; the term
  dictionary's redesign (P12); the postings change the disk row needs; and the
  verified-region size (P15), recorded in the table of contents.
- **Track P items committed to Phase 3**, each against the gap it closes (the gap
  table in `docs/reviews/plan-amendments.md`, A29, re-measured with Lucene with
  reordering before the first lands):
  - Intersection and negated classes: P2, P7, P9.
  - Phrases: P8, and P5 with its disk cost stated.
  - `intersection_union`: block-max WAND for nested disjunctions (review C7).
  - Frequent terms: the frontier cost (review C15) and P3.
  - Every class: P6, since per-segment term lookups and scorer setup multiply with
    segments.
  - Disk and serving memory: P1, P12, P13, P15.
  - Indexing: a profile of the Phase 2 writer and the design's D18 measurement, before
    buffer code.
- **Tools at Phase 3's start:** Lucene with reordering in the harness, an
  allocation-counting build, the cgroup memory tool, and a runner that scores all four
  axes (`benchkit`).
- **Conditions from the Phase 2 gate** (`docs/reviews/phase-2-gate.md`): C1–C3, C5,
  C7, C9, and C13–C16 by their stated points; C14's `verify` (ordinals, both scoring
  schemes) lands before any query-path change.

Gate:

- Power-loss injection at every barrier of a randomized workload, across at least
  10,000 crash points, with zero acknowledged-write loss and zero inconsistent opens.
- The digest is equal across two replays.
- Write amplification reported against Lucene's tiered policy at equal segment
  budgets; multi-thread indexing must-win rows.
- Disk full: fail-closed writes, no corruption, recovery once space returns.
- The Phase 3 rows of every ratchet (query speed, indexing speed, memory, disk),
  queries on multi-segment indexes both after merging and before it; `verify` and
  `parity` on multi-segment indexes.
- Coverage targets and the test-parity inventory's Phase 2 and Phase 3 rows.

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
- Phase 3's memory budget takes `ledger`'s shape and switches to it when step 5 lands;
  strata does not extract `ledger`, since that changes code Detent owns.
- strata uses `consensus` for every authority: the root, the placement groups, and the
  shard groups. Step 10 decides only whether `cluster` keeps `cluster::Raft` for its
  other consumers, by LIBRARY_BOUNDARIES' measurements.
- From `ledger`, strata takes only the capacity and permit core. Detent's retirement
  runtime destroys owners on a cleanup thread, which Goal 5 forbids; strata asks the
  Detent work for it as a target of its own that neither `completion` nor strata links,
  before step 5's API is fixed. Goal 5's audit of `completion`, `journal`, and
  `consensus` lands before Phase 3R adopts them.

- C1: membership changes and voter roles (learners, optional witness).
- C2: multi-group design and prototype: one journal per core, group commit across
  groups, coalesced heartbeats, quiescent groups, forced checkpoints for laggards.
  Measure fsyncs per second per node at 1,000 groups.
- C3: repair at or below the commit cursor.
- C4: stream transport with flow control (TLS before production).
- C5: a deterministic simulation harness with a linearizability checker.

Gate per item: tests and measurements in the `consensus` and `journal` repositories,
reviewed by the architect. The design is `docs/design/track-c.md`.

- **Before Phase 3R:** the bare-metal machine (BENCHMARKS rule 4): a MacBook Pro
  with an Apple M1 Pro (arm64, 32 GB) under macOS, `F_FULLFSYNC` as the durable barrier for every
  engine, frequency not pinnable (more interleaved trials instead). It also makes
  arm64 a first-class target: strata and its libraries build and pass there, and the
  SIMD paths (bitpack, hashes, fusefilter) get NEON or portable fallbacks measured
  against x86. Linux-only rows stay in the gate VM.

## Track P: performance research (parallel, every phase)

Each item states the query class or footprint it targets, lands behind a measurement
on the gate VM of all four axes (query speed, indexing speed, memory, and disk; before
and after, interleaved, at least five trials), and must keep `verify`
(pruned equals exhaustive on every gate query) and oracle parity green. An item that
does not win is recorded with its numbers and removed.

From the literature and the baselines:

- P1: document reordering by recursive graph bisection at merge time (Dhulipala et
  al., 2016), measured against Lucene's own `-bp` configuration.
- P2: SIMD intersection of decoded blocks and galloping across blocks (Lemire et al.,
  2016); SIMD rank and select in bitset blocks.
- P3: variable-size blocks for block-max pruning (Mallia et al., 2017), and
  impact-ordered evaluation for top-k on long lists (anytime ranking, Mackenzie et
  al., 2022), on document-ordered postings.
- P4: partitioned Elias-Fano and Roaring containers as alternative block encodings,
  chosen per block by size and decode cost.
- P5: next-word (common-grams) positional entries for frequent word pairs (Williams
  et al., 2004), with their index-size cost stated.
- P6: prefetching and cache-line layout of skip data, huge pages for pinned
  dictionaries, and the per-query fixed cost (term open, dictionary lookup, region
  verification) measured and cut.

strata's own:

- P7: membership probes on dense terms: a candidate tests a bitset block, or a full
  bitmap for the highest-df terms, in O(1) instead of advancing and decoding.
- P8: phrase prefilters: per-block summaries of which frequent pairs occur adjacent,
  checked before any position decodes.
- P9: a cost-based planner choosing leapfrog, windows, probes, or bitmap AND per
  clause from measured unit costs (the conjunction model in `prefer_windows`
  generalized).
- P10: distributed threshold sharing: shards exchange the running k-th score during a
  query, so every shard prunes against the global threshold, not its local one.
- P11: count and aggregation answers from per-segment statistics and sketches where
  exact, extended to cross-shard plans.

Footprint (disk and memory):

- P12: the term dictionary: offsets relative to the block's first term, an inline
  form for single-posting terms, statistics elided when implied (cf equal to df),
  and shared-prefix and suffix compression measured against `.tim` plus `.tip`.
- P13: positions: per-block choice among FOR, PFOR, and Elias-Fano, and position
  deltas within a document coded against the document's length.
- P14: stored fields: dictionary-trained zstd per segment and block sizes tuned for
  fetch latency against size.
- P15: serving memory: what stays pinned (dictionary indexes, skip data) against
  what stays mapped, chosen per segment by access rate, and per-query arenas
  sized from the planner's estimate.

Reclamation (`docs/perf/reclamation.md`):

- P16: deletion affinity: merges of adjacent age and similar size, so documents
  deleted in age order leave whole segments to drop without a rewrite.
- P17: rebase copy: merges copy postings blocks with no deleted document verbatim,
  rewriting only the block base.
- P18: holes for deleted stored records (`fallocate(PUNCH_HOLE)`, `F_PUNCHHOLE`,
  `fspacectl`) instead of rewrites, with verified regions inside a hole marked.
- P19: off-core reclamation: stepped removal and `munmap` on the background I/O
  thread, rate-limited, measured by foreground p99 on every platform.

Every Track P measurement covers Linux, FreeBSD, and macOS, on x86_64 and arm64, and an
item stays only if it wins, or at least does not lose, on all of them.

Gate per item: its before/after table in `docs/perf/` and the architect's review.

- P20: frozen indexes (`docs/design/frozen-index.md`): a freeze that rebuilds an index as
  one reordered segment with quantized impacts, exact block bounds, and a static key hash,
  and an unfreeze that makes it writable again; measured against the same index unfrozen
  and against PISA. Single-node half after Phase 3's gate; cluster half (read replicas,
  object storage) after Phase 3R and Track D6.

- P21: field-update generations and counter merge operators
  (`docs/design/workload-profiles.md`, items 4–5): doc-value updates without reindexing,
  through the same per-generation side files as live docs; measured against Lucene's
  updatable doc values and against RocksDB merge operators.

## Track D: distribution (parallel, from Phase 3)

[DISTRIBUTION.md](DISTRIBUTION.md) is the design. Track D builds it in a
deterministic cluster simulator first and on real nodes second.

- D1: the scale contract and a deterministic simulator of at least 100,000 nodes and
  10^7 shards (time, network, disks, and failures simulated), shared with Track C's
  harness.
- D2: membership and failure detection by SWIM-style gossip over Detent's peer
  transport. Xapiand's UDP multicast discovery and its UDP Raft are not carried
  forward.
- D3: a partitioned placement authority: a small strict-consensus root and placement
  groups that each own a range of indexes; rebalancing, draining, and rack- and
  zone-aware replica placement; per-index placement strategies (count, fraction, all,
  constraints, follow, affinity groups) and adaptive locality copies
  (`docs/design/placement-strategies.md`).
- D4: index namespaces: Xapiand's index per path, mapped to shards and groups, with
  aliases and resharding by split.
- D5: the query path at scale: fan-out trees with aggregator tiers, mergeable partial
  results, global statistics, P10's threshold sharing, partial results under
  timeouts, and hedged requests.
- D6: storage tiers: local segments, and immutable segments on object storage with
  stateless searchers (Quickwit's model) for cold indexes.
- D7: the blob store: whether Xapiand's Haystack-style volumes stay inside the node
  or become a separate service (DISTRIBUTION.md, "Open decisions").
- D8: rolling upgrades with mixed versions, and wire and format compatibility rules.

Order and dependencies: D1 is designed with Track C's C5 under one owner; D2 follows
the membership-transport decision (DISTRIBUTION.md, "Open decisions"); D3 follows C1
and C2; D5 and D6 follow Phase 3's manifests and snapshots; D7 lands before Phase 7. A
test cluster of at least 30 nodes exists before Track D's first gate.

D1's fidelity tiers: membership and placement at full scale; shard groups as abstract
state machines at full scale; full Raft groups and journals at a stated smaller scale;
the hardware each tier runs on is stated with its results.

Gate per item: the simulator's runs at the scale contract (no acknowledged loss, no
split placement, bounded convergence and fan-out latency), measurements on a real
cluster of at least 30 nodes, and the architect's review.

## Phase 3R: replication groups

Starts after C1–C4 pass, which places it after split steps 1–3, and once Phase 3's
lifecycle rows pass (power loss, digest, disk full, fuzzing, sanitizers, coverage).
Phase 3's ratchet rows run in parallel and gate Phase 4.

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
  client and with 16 concurrent clients; p99 acknowledgement latency on the bare-metal
  machine's NVMe (`F_FULLFSYNC`);
  fsyncs per second per node at 1,000 groups within budget.

## Phase 4: structured data

- `sortable`, doc values by slot, `bkd` (1-D and 2-D interval), scored filters,
  computed sort keys, collapse, exhaustive mode, estimate triples, `_percent`.
- Aggregation collectors (terms, histogram, date histogram, range, stats, term-list
  based `_terms`).
- Workload profiles (`docs/design/workload-profiles.md`), items 1–3 and 6: the key
  dictionary as its own section, optional sections per segment, real-time get, and
  index sorting by a doc value, all in format 13 with doc values and points; the
  oracle's key-value model.

Gate: Phase 3's ratchet rows (they gate Phase 4, since Phase 3R does not wait for
them); filtered, sort, range, geo, and aggregation classes must win against Xapian
and reach ≤ 1.0× of the faster of Lucene and Tantivy; operator parity against oracle
vectors; aggregations equal across one shard and many (DISTRIBUTION.md, "Query").

## Phase 5: analysis and query language long tail

- All Snowball languages, CJK n-grams, ICU plugin, UAX #29 analyzer.
- `automata`: wildcard, prefix, fuzzy.
- `queryparser`.
- Relevance-feedback expansion.

Gate: prefix, wildcard, fuzzy, and expansion classes must win against Xapian and
reach ≤ 1.0× of the faster of Lucene and Tantivy; per-language stemmer vocabularies
equal to Xapian's; `queryparser` vectors pass.

## Phase 6: async backends and tuning

- io_uring and POSIX AIO residency backends; the verified block cache; scheduling
  tuning.

Gate: reactor p99 stall ≤ 1 ms under the mixed heavy and light load; light-query p99
under heavy load ≤ 2× its unloaded p99 plus 1 ms (against the light-query
p99 under offload that Phase 0 measures); major reactor faults per 1,000 queries ≤ 1
with the index at twice RAM; the Phase 6 rows of every ratchet: queries ≤ 0.7×,
indexing ≥ 1.3×, memory ≤ 0.7×, and disk ≤ 0.8×.

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
row, and Elasticsearch and Quickwit on the claimed rows (ingest, search, and
aggregation at 3, 30, and at least 300 nodes); Track D's scale gates pass.

## Phase 8: removal

- Remove `src/xapian/` and every Xapian reference.
- Replace GPL files outside it: `sortable_serialise` (with `sortable`),
  `remote_protocol_views` (with the shard RPC), `xapian.h`, and the `pack.h`
  include in `remote_protocol_service.h`.
- Rename the project.

Gate: clean build without Xapian; all suites green; a license audit of every
dependency.
