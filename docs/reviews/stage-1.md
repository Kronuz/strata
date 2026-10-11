# strata stage-1 review: feasibility and architecture

- Reviewer: chief architect, stage 1.
- Date: 2026-10-11.
- Documents: `FEASIBILITY.md`, `ARCHITECTURE.md`, `BENCHMARKS.md`, `PLAN.md`, and
  `REPLICATION.md` as of 2026-10-11 08:29.
- Evidence: Xapiand `master` at `877f46ab2` (own code under `src/`, the Xapian 2.0 fork
  under `src/xapian/`, `harness/`, the docs E2E suite), and the Kronuz libraries,
  including `reactor`, `cluster/consensus`, `cluster/journal`, and `cluster/completion`.

Citations use `path:line` relative to the Xapiand repository unless a repository is
named.

## 1. Verdict

**APPROVE WITH REQUIRED CHANGES.**

Phase 0 starts after R1–R3 land in the docs; R2 and R3 change what Phase 0 measures.
Phase 1 starts after R4–R20 land. The consensus work in Track C (R18) starts now,
because it does not depend on the index format.

The direction holds:

1. Immutable segments and snapshots remove the `DatabaseModifiedError` retry loops
   that wrap every `Shard` and `DatabaseHandler` method today (the `DB_RETRIES` loops
   throughout `src/database/shard.cc`).
2. Block-max pruning, columnar doc values, and a points index replace three Xapiand
   workarounds: accuracy-term tries for ranges, `MatchSpy` aggregation over whole
   documents, and `KeyMaker` sorts over serialized value slots.
3. FEASIBILITY.md frames "faster everywhere" as gated targets with reported misses,
   which is the honest framing.
4. A deterministic single-replica core under a separate replication layer is the
   right shape for testing, embedding, and durability.
5. The documentation E2E suite is the right integration contract.

What blocks Phase 1:

1. The ingestion root cause is asserted, not measured, and the cited benchmark
   cannot carry the attribution (section 2.2).
2. The inventory misses contracts that Xapiand exposes through its HTTP API: stable
   docids, per-document term lists, metadata, transactions, revisions, the prefixed
   term namespace, sparse value slots, scored posting sources, `_percent`, and match
   estimates (section 3).
3. The design contradicts itself in five places: seqno versus revision, 1-byte
   per-field norms versus Xapian BM25 parity, term vectors used but never stored,
   suspend-on-miss inside scorers that are not coroutines, and "no offloading" next
   to an I/O pool and dedicated merge threads.
4. The plan builds the async I/O contract (Phase 6) and the Xapiand seam (Phase 7)
   last, after every scorer and reader exists, which guarantees rework.
5. The consensus and journal libraries are sound foundations, but they are qualified
   for a 3–5 voter authority, not for thousands of data-plane groups. Membership
   changes, multi-group economics, and below-commit repair must close before
   Phase 3R (ruling l).

## 2. Feasibility assessment

### 2.1 What the verdict gets right

- Every component has a published algorithm, and none needs GPL or Apache source.
- Search work is mostly CPU, so asynchronous I/O alone does not remove the worker
  pool. This is the key insight, and FEASIBILITY.md states it.
- The incremental path behind a storage seam, with Xapian removed last, keeps
  Xapiand shippable.
- Misses are reported with numbers rather than hidden.

### 2.2 Over-claims

**The ingestion root cause is unproven.** "The gap follows from the write models"
rests on one pair of numbers (Xapiand ~7,800 docs/s, Elasticsearch ~13,200 docs/s).
The run cannot separate engine cost from host cost:

1. It is one `RESTORE /bench/?commit` request carrying 20,000 replicated `accounts`
   records (`harness/loadtest.py:89-100`). The index is 3.4–9.2 MB, and the request
   finishes in 1.7–2.3 s.
2. Stored results range from 7,872 to 11,511 docs/s across builds, with up to 23%
   spread between trials of one build (`harness/results/baseline.json`: 9,681,
   7,872, 8,564). The published 7,800 sits at the low end of that range.
3. Glass flushes buffered postings only past 100,000 changes
   (`src/xapian/backends/glass/glass_database.cc:1101`), and RESTORE disables
   autocommit. A 20,000-document load therefore reaches the B-tree merge path the doc
   blames only at the final commit.
4. Xapiand's own write path adds per-document B-tree work. Each write by term opens
   the id term's posting list, which first flushes that term's pending changes
   (`glass_database.cc:1590-1592`). Each update of an existing document also scans
   `allterms_begin("V" + did)` for the version check (`src/database/shard.cc:1385`,
   `:1569`, `:1603`), and glass flushes the pending postings under that prefix before
   iterating (`glass_database.cc:1655-1672`).
5. The RESTORE path disables the WAL (`src/database/handler.cc:941`), and a one-shard
   index gets one `DocIndexer` (`handler.cc:2197`: `min(num_doc_indexers,
   endpoints.size())`). Schema processing, MessagePack serialization, and data-locator
   handling run on that path too.
6. The Elasticsearch harness sends sequential 2,000-document `_bulk` requests from
   one client (`harness/es_loadtest.py:58-65`). Elasticsearch processes the items of
   a shard-level bulk request sequentially on one write thread, so Lucene's
   per-thread writers (DWPT) barely engage. They are not what wins this benchmark.
7. Durability differs between the two runs. Elasticsearch fsyncs its translog per
   bulk request and stops the clock at `_refresh`. Xapiand's RESTORE runs without the
   WAL, and Xapian commits with `DB_NO_SYNC` (`shard.cc:78`).

The gap may come from Xapiand's per-document pipeline rather than from the B-tree. A
new engine cannot fix host cost. Phase 0 must profile the RESTORE path and attribute
time to engine calls versus host code before Problem 2 can justify the write-path
design (R1, R3).

**"Xapian is faster for reads" is unproven by the same benchmark.** All three
systems serve about 11,400 qps on 20,000 documents, where HTTP parsing, JSON, and
response rendering dominate. The run says nothing about engine read speed. Block-max
pruning makes beating Xapian on top-k disjunctions likely. Beating Lucene and Tantivy
on reads is the hard part, and nothing in the docs measures it yet.

**The algorithms are published; the behavior is not.** Xapiand users see Xapian
behavior that no paper describes:

- `TermGenerator` word rules and its `Z`-prefixed stemmed terms.
- The `QueryParser` grammar and heuristics, which parse every text-field value
  (`src/query_dsl.cc:831-877`).
- BM25's smoothed term weight (`if (tw < 2) tw = tw * 0.5 + 1;`,
  `src/xapian/weight/bm25weight.cc:107`).
- `_percent`, `get_matches_estimated()`, and the termfreq estimate for
  `OP_SYNONYM`. Wildcard, partial, and fuzzy expansions all combine through
  `OP_SYNONYM` (`query_dsl.cc:732-771`).

Reproducing this behavior is the long tail of the project. It needs a clean-room
procedure: specifications written from Xapian's documentation, plus black-box test
vectors produced by running Xapian 2.0 in a separate GPL-side harness that never
ships with strata (R3, R14).

**"No offloading" is not literal.** Writes forward to the core that owns the shard,
a per-request cross-core hop. Flush and merge run on background threads. macOS reads
use a pool. The honest claim is narrower: no blocking pool sits on the read path, and
every handoff is an explicit message. Ruling (a) gives the exact guarantee.

FEASIBILITY.md also says macOS "has no asynchronous file read API". macOS has POSIX
AIO, and `cluster/completion` already submits native `aio_read` on macOS
(`cluster/journal/README.md`, "Owned payload read completion"). The real limits are
polling-based completion on Darwin 24 and a small kernel AIO quota.

**The offload cost is not quantified.** The hand-off costs microseconds, while
measured p50 latency is 1.29 ms. Phase 0 must measure the hop (inline versus
offloaded, same query) before the doc treats it as a problem worth a rewrite.

The workspace previously rejected an async Xapian rewrite
(`~/code/Kronuz/memory/kronuz-unstallable-event-loop.md`: "async IO only hides
IO-wait, and you'd still need threads for the CPU work"). FEASIBILITY.md answers that
objection with cooperative yielding but never cites the earlier decision or the
evidence that reverses it (R19). The same note measured ~8 ms fast-query latency
under offload versus ~450 ms inline during heavy load, so cooperative yielding has a
concrete fairness bar to meet.

**Effort is unstated.** "Phases with gates, not calendar dates" avoids the question
feasibility exists to answer. The docs need an order-of-magnitude size per component,
in lines of code and engineer-months, derived from Tantivy and Lucene equivalents
(R19).

**Factual errors** (R20):

- Glass is a copy-on-write B+tree, not "updated in place". A flush writes every
  touched block and its path to the root into free space.
- "Roughly fifteen independent building-block libraries" disagrees with the nine new
  libraries listed in ARCHITECTURE.md.
- Outside strata, Xapiand's `docs/src/content/docs/benchmarks.md` says Xapiand
  "memory-maps its storage". Glass reads blocks with `pread`
  (`src/xapian/backends/glass/glass_table.cc:175`), and Xapiand's storage has no
  mmap.

### 2.3 Under-claims

- **Exact aggregations.** Xapian match spies see only the documents the matcher
  considers. Xapiand sets `check_at_least = 1` when aggregations are requested
  (`handler.cc:1172-1176`), so relevance-ordered queries can aggregate over a pruned
  subset today. Exhaustive columnar aggregation fixes this. It is also a visible
  behavior change, and the docs should say so.
- **Cancellation and time budgets.** Xapiand cannot cancel a running Xapian query on
  a pool thread. Yield points make cancellation and per-query time limits natural.
- **Integrity.** Xapiand's document storage validates nothing by default
  (`ARCHITECTURE.md`, Bugs). Checksummed segments and manifests are a real gain.
- **Retry removal.** Hundreds of lines of `DB_RETRIES` and `reopen()` loops
  disappear.
- **Consistent full copies.** A pinned manifest gives replicas a consistent copy.
  Today the primary copies live glass files and retries up to five times before
  failing with "Database changing too fast"
  (`src/server/replication_protocol_views.cc:534-585`).

### 2.4 Is "faster in all accounts" framed correctly?

FEASIBILITY.md frames it correctly; BENCHMARKS.md does not. BENCHMARKS.md says a gate
passes when the engine "beats each baseline outside the measured noise band", which
turns every row into a must-win and never defines the noise band. Fix it (R2):

1. **Classify rows.** Must-win rows gate a phase. Report rows are measured and
   explained but do not gate. The owner's stated targets (ingestion, top-k latency,
   memory) are must-win. Rows where strata stores more by design (term lists, exact
   lengths) are report rows until a phase claims them.
2. **Use ratcheting thresholds.** A phase gates against Xapian first, then against
   Lucene and Tantivy at stated ratios that tighten per phase (section 6.4).
3. **Define the noise band.** Run at least five trials after warm-up and report the
   median with a 95% bootstrap confidence interval of the ratio. A row wins when the
   interval excludes 1.0 and the median effect is at least 3%.
4. **Fix the gate machine.** Gates run on a dedicated Linux host with a fixed CPU
   frequency (performance governor, turbo pinned or disabled) and NVMe storage. The
   i9-9980HK MacBook throttles thermally and has no io_uring, so macOS numbers are
   informational.
5. **Compare equal semantics.** Use the same analyzer, the same scoring function (or
   count-only collectors), and the same stored-field and durability settings, stated
   per row. An engine-level ingest row without fsync is not comparable with a
   system-level row with translog fsync.
6. **Add the missing rows:** resident memory (idle, serving, indexing), cold start
   (open time per GB and first-query latency with a dropped page cache),
   crash-recovery time, index size with and without term lists, and a Xapiand-shaped
   corpus (prefixed terms, boolean terms, sparse value slots, frequent replaces)
   beside Wikipedia.

## 3. Inventory gaps

FEASIBILITY.md's table covers query operators and the obvious document API. These
Xapian surfaces are load-bearing in Xapiand and are missing or mis-mapped:

| # | Xapian surface | Where Xapiand relies on it | Engine requirement |
|---|---|---|---|
| 1 | Stable shard docids, `get_lastdocid()`, docid-order iteration | Numeric auto-ids are `(did-1)*n_shards + shard + 1` with `did = lastdocid + 1` (`shard.cc:1539-1540`); WAL replace and delete by docid (`wal.cc:554-561`); `_docid` in hits and write responses (`search_views.cc:919`, `:2015`); `_last_id` and `_doc_del = lastdocid - doccount` (`handler.cc:1909-1910`); dump walks `postlist_begin("")` with `skip_to` resume (`handler.cc:831`, `:890`) | A per-shard monotonic docid that is never reused and survives replace and merge, kept as a stored column with a docid-to-document index. `lastdocid` is persisted in the manifest |
| 2 | Per-document term lists with wdf and positions | ESet expansion for `nearest` and `fuzzy` (`handler.cc:1220-1294`); the `_terms` aggregation reads each match's term list (`aggregations/metrics.cc:121-139`); the info endpoint returns terms and positions (`handler.cc:2693-2710`, `:2879`); about 40 E2E assertions on `terms.*` | Per-field opt-in stored term vectors, plus re-analysis from the stored record for the info endpoint and for ESet over at most a few hundred documents. ARCHITECTURE.md uses "term vectors" for expansion but defines no file for them |
| 3 | Metadata keys, committed atomically with documents | The schema lives in `RESERVED_SCHEMA` (`schemas_lru.cc:277-381`); user metadata over HTTP (`search_views.cc:1067-1132`); key listing (`shard.cc:2000`); the storage-locator fallback keys `\x00...` (`shard.cc:1141`); the WAL `SET_METADATA` op | A key-value store versioned by the manifest |
| 4 | Transactions: `begin`, `commit`, and `cancel_transaction` | Replication applies changesets inside a transaction (`replication_protocol_views.cc:983-987`); `MSG_CANCEL`; flushed and unflushed modes (`shard.cc:786-797`) | Atomic multi-operation batches and cancel |
| 5 | Revision: +1 per commit, "no change, no commit" | `assert(current_revision == prior_revision + 1)` (`shard.cc:805`); WAL lines tagged with the open revision, and replay requires `revision == db_revision` (`wal.cc:528`); replication by revision; `_revision` in the info endpoint (`handler.cc:1907`) | A commit counter separate from per-operation order (ruling c) |
| 6 | Database UUID | Binds WAL and data volumes to a shard (`shard.cc:195-236`, `wal.cc:88-134`); detects replica divergence (`replication_protocol_views.cc:514-521`); `_uuid` | An index identity in the manifest |
| 7 | All-terms iteration by arbitrary byte prefix | Version lookup by `"V" + did` on updates; remote `MSG_ALLTERMS` | Ordered iteration over any byte prefix |
| 8 | An opaque, host-owned term namespace | Terms are `prefix + ctype + value`. Nested field prefixes concatenate their parent's prefix (`schema.cc:5821-5870`); global per-type namespaces exist (`get_prefixed_global`); "exists" is `OP_WILDCARD` on a field prefix (`query_dsl.cc:647`, `:681`); terms are capped at 245 bytes (`schema.cc:5298`); boolean terms carry wdf 0 | Terms as opaque byte strings. ARCHITECTURE.md's "terms scoped by field ordinal, so prefix and wildcard queries never cross fields" breaks exists queries and parent-prefix wildcards |
| 9 | Value slots | Slots are xxh32 hashes of (prefix, type), so they are sparse 32-bit ids (`database/utils.cc:59-68`); reserved slots 0–20 include global per-type slots (`database/utils.h:41-57`); values are serialized multi-value `StringList`s; `get_value_freq/lower_bound/upper_bound` serve remote `MSG_VALUESTATS` | A doc-values directory keyed by 32-bit slot, multi-valued, with sparse presence and per-column statistics |
| 10 | `ValuePostingSource` subclasses that score | Range checks (`multivalue/range.cc`); geo is interval-set intersection weighted by centroid angle (`multivalue/geospatialrange.cc:61-112`); all are serialized to remote shards | A scored-filter iterator API with `weight()` and `max_weight()`; geo as interval sets, not points |
| 11 | Computed sort keys | `KeyMaker` subclasses compute string metrics (Levenshtein, Jaro, Soundex), geo distance, and closest or farthest over multi-values (`multivalue/keymaker.h:261-358`); `set_sort_by_key_then_relevance`; serialized to remotes | A sort-key function API over doc values that can be serialized for remote shards |
| 12 | `Enquire` semantics | `check_at_least`; `docid_order` ASCENDING tie-break; `collapse_max`; `_rank`, `_weight`, and `_percent` per hit (`search_views.cc:2022-2024`); `total = get_matches_estimated()` (`search_views.cc:1980`), which E2E asserts exactly (333 in `reference-guide/search/query-dsl/leaf-queries/text-queries/stop-words.md:34`, 286 in `exploration.md:551`) | Xapian's lower-bound, estimate, and upper-bound triple; exact counting when asked; the Xapian percent formula |
| 13 | `MatchSpy` coverage | Aggregations see only considered documents (section 2.3) | Exhaustive aggregation, documented as a behavior change |
| 14 | `TermGenerator` | Word rules; `Z` stems under `STEM_SOME`; stop strategies; `FLAG_CJK_NGRAM` and ICU `FLAG_WORD_BREAKS` behind `cjk_words` and `USE_ICU` (`multivalue/generate_terms.cc:496-528`); per-field wdf weights and explicit positions through `add_posting(term, pos, weight)` (`schema.cc:5308`) | A Xapian-compatible analyzer as the default (ruling j) |
| 15 | `QueryParser` | Parses text-field values with phrase, love/hate, fuzzy, boolean, wildcard, partial, and CJK flags and expansion limits (`query_dsl.cc:831-877`) | A Xapian-syntax query parser. This is a different language from Xapiand's boolean string language (`src/booleanParser/`), so "extends boolean-parser" is the wrong base |
| 16 | `Database::check` | Exposed over HTTP (`search_views.cc:1783`) | A verify API covering checksums and structure |
| 17 | The remote protocol (about 40 message types) | Distributed search, remote document fetch, and write forwarding to primaries (`server/remote_protocol_views.cc`, against Xapian's `RemoteDatabase` client) | A complete client and server replacement with query, posting-source, sort-key, and aggregation serialization. Phase 7's "shard RPC" bullet understates it |
| 18 | Statistics | `get_doccount`, `get_avlength`, doclength bounds, `has_positions`, `get_total_length`, `get_collection_freq`, exact `get_termfreq`, `get_unique_terms`, `get_wdfdocmax`, and per-document `get_doclength` (`handler.cc:1902-1915`; remote protocol) | Exact `N`, total length, and doclength bounds. Termfreq semantics after deletes need a decision (ruling d) |
| 19 | Durability coupling | Xapian opens `DB_NO_SYNC` because the WAL anchors durability (`shard.cc:78`); `DB_RETRY_LOCK`; `Database::refs()` for pool ownership checks | Defined by ruling (l) |
| 20 | Over-scoped: spelling and synonyms | No HTTP path adds spellings or synonyms. `FLAG_SPELLING_CORRECTION` and `FLAG_SYNONYM` appear only in a debug printer (`query_dsl.cc:81-84`); the rest is remote passthrough and WAL op types | Drop them from engine scope, including the "fst-backed synonyms and spelling" item in Phase 5 |
| 21 | GPL-derived code outside `src/xapian/` | `sortable_serialise.{cc,h}`; `length.{cc,h}`, which defines the WAL and locator byte formats; `server/remote_protocol_views.{cc,h}`; `xapian.h`; `server/remote_protocol_service.h` includes `xapian/common/pack.h` | Phase 8 covers these too. `cluster/length.h` is an MIT, byte-compatible varint candidate once its provenance is confirmed |

## 4. Architecture rulings

### (a) Async model

**Ruling: own `task<T>`, approved; engine-owned run queue, rejected.** The host's
per-core executor is the run queue. The engine defines a small scheduler concept
(`post(handle)`, `now()`) and an I/O concept, and ships an Asio binding plus a
deterministic inline scheduler for tests. This mirrors `cluster/completion`, which is
protocol-independent and adds Asio through an optional adapter. It keeps the core free
of Asio and `reactor`, and it lets the group layer run whole clusters under
deterministic simulation.

- **Scorers are not coroutines.** A query runs as one coroutine that drives a
  resumable bulk scorer over doc-id windows (start at 4,096 docs and tune) and yields
  between windows. A plain `advance()` cannot suspend, so ARCHITECTURE.md's "a cache
  miss suspends the coroutine" holds only at defined suspension points:
  1. Query start, after term lookups, when residency for the query's ranges is
     requested.
  2. Segment boundaries.
  3. Window boundaries.
  4. Stored-record fetches.
- **Budget.** Read the cycle counter once per window against a slice of 100–250 µs.
  Bound one window's worst case by capping window size and per-window positional
  checks, and measure the bound.
- **Scheduling.** Use least-attained-service ordering: a task's first slice runs at
  high priority, and a task that exhausts its budget is demoted. Light queries then
  stop queueing behind heavy ones without classifying routes in advance. Add a
  per-core cap on demoted tasks and shed load with 503 at the cap.
- **Honest guarantee.** No reactor stall exceeds one slice plus one window.
  Light-query latency stays fair under heavy load. This is latency fairness, not
  throughput isolation: a saturated core does not borrow idle cores. Writes cross to
  the shard owner's core as messages. Flush and merge run on background threads or in
  slices, never as a 100 ms stall on a reactor.
- **Required semantics.** A task resumes its awaiter on the awaiter's executor.
  Cancellation is observed at yield points. Exceptions propagate. Symmetric transfer
  keeps deep chains in constant stack. Coroutine frames come from a per-core pool,
  with zero `malloc` on the hot path.
- **Integration hazard.** Xapiand's own code blocks: the `DatabasePool` checks out
  shards with condition-variable waits (`database/pool.h:91`, `:98-111`), and the
  path includes storage `pread`s, WAL fsyncs, schema LRU locks, and Lua scripts.
  Removing the offload pool before these become asynchronous primitives stalls
  reactors, and a coroutine waiting on a condvar held by another coroutine on the same
  thread deadlocks. Phase 7 splits accordingly (R15).

### (b) I/O

**Ruling: mmap is the default read path, with an asynchronous residency layer; a
verified `pread` block cache is a second backend.**

- mmap gives zero-copy decode and no double caching, and Lucene and Tantivy, the
  baselines to beat, both use it. Copying every block into a user cache adds work on
  every read.
- The residency layer runs at query start. It computes each term's postings ranges
  (positions only for phrase queries) and makes them resident asynchronously:
  `madvise(WILLNEED)` or io_uring reads on Linux, and the existing POSIX AIO backend
  on macOS. Evaluation then runs against resident pages.
- **Pinned set:** the term index, skip and impact data, document lengths, live docs,
  primary-key filters, doc-value columns in active use for sort, aggregation, or
  collapse, and the manifest.
- **Stall metric (a gate):** major faults on reactor threads per 1,000 queries, read
  per thread on Linux, plus p99 latency with the index at twice RAM.
- **The verified block cache** (`pread` into memory checksummed on load) serves
  indexes much larger than RAM on slow storage, and the strict corruption mode in
  ruling (l). mmap pages re-read after eviction are never re-verified, so mmap cannot
  promise that every read was verified.
- **I/O substrate.** Reuse `cluster/completion` (io_uring, POSIX AIO, Asio adapter)
  instead of building a new I/O layer in `blockcache`. Its queues hold one outstanding
  operation by design, so the read path needs configurable-depth queues; that is a gap
  to close there.
- **Single-file segments.** Keep sections in one file with a table of contents. Many
  shards times many segments times ten files exhausts file descriptors and multiplies
  fsyncs.

### (c) Write path

**Ruling: one writer per shard, owned by one core. Partitioned multi-writer is a
gated Phase 3b option. A Lucene-style DWPT pool is rejected.**

- **Order.** Operations get a total order at admission on the owner core. Analysis
  may run on the receiving core and ship a prepared document, as Xapiand already does
  with `DocPreparer` before `Shard::replace_document`.
- **Apply runs in two stages.** A cheap sequential stage on the owner assigns docids,
  checks versions, and updates the primary-key map. An expensive stage does the
  inversion. Keeping docid assignment sequential keeps it deterministic, which docids
  exposed as `_docid` and `_last_id` require.
- **Partitioned option.** Hash the primary key to a partition, give each partition a
  buffer on another core, keep merges partition-local, and cut commits with a marker
  message through FIFO channels. Build it only if single-core inversion throughput
  measures below twice the target ingest rate. Primary-key partitioning already keeps
  deletes inside one partition, so Lucene's global delete queue is unnecessary.
- **Three numbers, defined separately.** They are conflated today:
  1. **revision** is the commit counter: +1 per commit, Xapiand's `_revision`.
  2. **seqno** is the per-operation admission order. Under groups it is the log
     position.
  3. **version** is the per-document, user-visible `_version`.

  The current text ("every operation carries the host's sequence number (Xapiand's
  revision)"; "deletes apply only to documents with a lower seqno") assigns one
  revision to many operations, so a delete and an add of the same id inside one
  revision tie.
- **Deletes** resolve at flush and refresh against the owner's segments, through a
  primary-key filter and a term-block lookup. Ordering inside the buffer is positional
  (the doc count at delete time), so no per-document seqno column is needed.
- **Live version map.** Ids written since the last refresh go in a per-shard map, so
  version checks on hot ids skip segment lookups.
- **Transactions and cancel** are an atomic batch with an undo of buffered
  operations.
- **Memory.** One indexing budget covers the whole node, across all shards, and
  flushes the largest buffer first. A per-shard, per-core budget explodes with
  Xapiand's shard counts.
- **WAL invariant.** Today, replay is correct because a commit is atomic and the
  revision is exact. Under groups (ruling l) the journal replaces the WAL. Replay then
  starts at exactly the applied index recorded in the manifest the journal checkpoint
  references, and apply is deterministic.

### (d) Postings codec, norms, positions

**Ruling: blocks of 128 with a 4×32-bit interleaved on-disk layout.
Frame-of-reference (FOR) for doc deltas, patched FOR (PFOR) for frequencies, flags
for "no frequencies" and "all ones", VByte tails, two skip levels with competitive
(wdf, doclen) pairs, and exact per-document lengths.**

- Current Lucene (10.3) uses the same core: 128-integer blocks, doc deltas packed
  without exceptions, frequencies with exceptions, skip data per block and every 32
  blocks, and impacts as competitive frequency-norm pairs
  ([Lucene103PostingsFormat](https://lucene.apache.org/core/10_3_1/core/org/apache/lucene/codecs/lucene103/Lucene103PostingsFormat.html)).
- **Layout is part of the format.** Fix the 4-lane interleaving on disk. SSE4.1 and
  NEON decode it natively, and AVX2 processes two lane groups. A layout tuned for AVX2
  makes NEON pay a de-interleave.
- **Boolean terms.** Terms with wdf 0 dominate Xapiand indexes (ids, versions,
  keywords, accuracy terms), so a per-term "no frequencies" mode and a per-block
  "all ones" flag cost zero bytes for frequencies.
- **StreamVByte** compresses worse on small doc gaps (it needs at least a byte per
  integer plus control bits). Evaluate it only for tails and positions. Treat
  dense-block bitset encoding, the roaring idea, as an experiment, not the default.
- **Block maxima store competitive (wdf, doclen) pairs, never precomputed scores.**
  Two-phase distributed search rescoring with global statistics invalidates any
  precomputed bound.
- **Norms.** Xapian BM25 uses the exact length of the whole document, the sum of wdf
  over all its non-boolean terms, not per-field lengths (`bm25weight.cc:169`). Store
  it as a bit-packed fixed-width column per segment for O(1) access, with segment
  minimum and maximum. One-byte per-field norms break parity by construction. Keep
  them only as an opt-in for other weighting schemes.
- **Positions** are sorted unique per (term, doc), stored as PFOR delta blocks. They
  can be arbitrary, because schemas set explicit positions.
- **Statistics after deletes need a decision.** Xapian's termfreq is exact, while
  Lucene counts deleted documents until merge. Every Xapiand update is a delete plus
  an add, so Lucene semantics drift scores on small, update-heavy indexes.
  Recommendation: keep `N` and average length exact (cheap, from the length column
  and live docs), approximate termfreq until merge, and measure the drift in the
  parity gate. Exact termfreq is possible only where term lists are stored.

### (e) Term dictionary

**Ruling: a block tree. Prefix-compressed term blocks of 25–48 terms with inline
statistics and postings pointers, indexed by a small resident FST over block
prefixes. A full-vocabulary FST as the primary dictionary is rejected.**

- One lookup is one block read, a natural asynchronous suspension point. A
  full-vocabulary FST's resident size grows with the vocabulary, and a traversal can
  miss anywhere.
- Wildcard, prefix, and Levenshtein automata intersect the block tree directly.
- The in-memory segment uses an open-addressing hash map with term bytes in an arena
  and sorts at flush.
- Terms are opaque byte strings (inventory row 8). An optional field id groups
  statistics only.

### (f) Doc values, BKD, geo

**Ruling: doc values keyed by the host's 32-bit slot through a per-segment slot
directory. Column types: sorted-numeric (multi-valued i64, u64, f64), sorted-set
(multi-valued bytes with a per-segment ordinal dictionary), and binary (opaque lists,
the compatibility path). Presence is sparse.**

- **BKD** is 1-D for numeric ranges and 2-D (start, end) for interval sets. Geo
  shapes are sets of HTM trixel ranges with centroids, matched by interval
  intersection (`geospatialrange.cc:84-112`), so "HTM trixel ids as points" mis-maps
  them.
- **HTM stays in Xapiand.** The engine has no `htm` or `cartesian` dependency.
- **Scored filters.** Geo distance and range weights need the scored-filter iterator
  API (inventory row 10).
- **Accuracy terms.** BKD retires the accuracy-term tries for numeric ranges. The
  schema still reports `_accuracy` and `_accuracy_prefix`, and E2E asserts them, so
  keep the schema fields and verify any change in generated terms.
- **Sorting.** Sort keys come from the computed sort-key API (inventory row 11).
  Index sorting with early termination defers: Xapiand sorts by arbitrary per-query
  keys, so index sorting pays only for one key.

### (g) Scoring and pruning

**Ruling: block-max MaxScore for top-level disjunctions, block-max WAND for nested
scoring disjunctions, chosen by a measured cost model.**

- Lucene moved top-level disjunctions from block-max WAND to block-max MaxScore in
  9.9 ([Elastic Search Labs](https://www.elastic.co/search-labs/blog/more-skipping-with-bm-maxscore)).
  Choose the algorithm from measured costs (essential clause count, k, clause cost
  spread) on the Wikipedia query set, not by clause count alone.
- **Exhaustive mode.** Pruning stays off when aggregations or exact totals are
  requested. `total` follows Xapian's lower-bound, estimate, and upper-bound
  semantics.
- **BM25 parity.** Implement Xapian's formula exactly: the smoothed IDF, the k3
  query-frequency factor, `min_normlen`, the k2 extra term, and `(k1 + 1)` scaling.
  Feed it exact lengths and exact `N`. On a corpus without deletes, the target is
  identical top-10 order and a relative score error of at most 1e-9. With deletes,
  report the drift.
- **Operator parity.** Define `OP_SYNONYM` termfreq (Xapian estimates it),
  `OP_ELITE_SET` term selection, `OP_MAX`, `OP_SCALE_WEIGHT`, and `_percent` against
  oracle vectors.
- **Distributed search.** Phase 1 gathers statistics from every shard; phase 2
  scores with them. Pruning bounds come from (wdf, doclen) pairs, so global statistics
  stay usable.

### (h) Segment file set, checksums, manifest, crash safety

**Ruling:**

- **Single-file segments** (sections, TOC, footer). Live docs go in per-generation
  files.
- **One checksum, CRC32C, everywhere.** It is hardware-accelerated on x86 (SSE4.2)
  and ARMv8, the journal already uses it, and REPLICATION.md otherwise mixes xxHash64
  and CRC32C.
- **When to verify:** footers and small files fully at open, compressed and
  stored-record blocks on every read, 64 KiB region checksums when the residency
  layer brings a region in, and whole files on merge, replication, and scrub.
- **Manifest.** `manifest-N` is checksummed and carries the index UUID, format
  version, revision, `lastdocid`, `N` and total length, the metadata reference, the
  segment list with sizes and CRCs, live-docs generations, and, under groups, the
  applied index and term.
- **Commit order:** segment write and fsync, live-docs write and fsync, manifest
  write and fsync, then directory fsync. Use `F_FULLFSYNC` on macOS, as the journal
  already does, because plain `fsync` there does not flush the drive cache.
- **Deletion policy.** Delete a file only when no snapshot, manifest in use, or
  journal checkpoint references it.
- **Recovery** opens the manifest the journal checkpoint references, or the highest
  valid one when standalone, and never falls back silently. Corruption fails closed.
- **Crash tests** simulate power loss by dropping unsynced writes at every barrier,
  through the same injected filesystem the journal uses, so segment, manifest, and
  journal barriers are tested as one node. `kill -9` alone does not lose page-cache
  data, so it does not test fsync ordering; `harness/wal_recovery_check.sh` tests only
  process death today.
- **Decoders** are memory-safe on corrupt input and fuzzed.

### (i) Replication by segment shipping

**Ruling: rejected as the replication mechanism; superseded by ruling (l).** Segment
files ship only to install checkpoints (bootstrap, far-behind replicas, repair) and
possibly for a later bulk-import fast path. Merges make continuous segment shipping
cost bandwidth equal to write amplification, and the unflushed tail still needs a
log.

### (j) Analysis

**Ruling: the default analyzer is a clean-room re-implementation of Xapian
`TermGenerator` behavior. UAX #29 is an additional analyzer. ICU is an optional plugin
for `cjk_words` only.**

- The default reproduces word rules, case folding, `Z`-prefixed stems, stop
  strategies, and CJK n-grams. It is validated by oracle vectors from Xapian 2.0 on
  multilingual text.
- UAX #29 changes term output that E2E asserts, so it cannot be the default.
- Unicode tables are generated from a pinned UCD version, for categories, case
  folding, and the decompositions accent folding needs.
- **Snowball** (BSD-3): pin upstream algorithms and the compiler at the generator
  version Xapian 2.0 uses (Snowball 3.0.0, `src/xapian/languages/compiler/header.h`).
  Cover Xapian's 35 algorithms. Check in the generated C with its license and a
  regeneration script. Gate each language on a vocabulary compared against Xapian's
  stemmers.

### (k) Library decomposition

| Library | Ruling |
|---|---|
| `task` | Right-sized once the run queue goes. It adds the scheduler and I/O concepts, executor affinity, cancellation, and an Asio binding target |
| `bitpack` | Right-sized. It owns FOR, PFOR, VByte, the "no frequencies" and "all ones" flags, and the fixed 4-lane layout |
| `fst` | Keep it standalone. Its role shrinks to the block-tree index |
| `termdict` (new) | The block-tree term dictionary on top of `fst` |
| `levenshtein` | Merge into `automata`: Levenshtein DFAs, wildcard and prefix automata, and intersection over any ordered term source |
| `docset` | Right-sized. It also carries live-docs and filter-cache bitsets |
| `bkd` | Right-sized. It must cover 2-D interval fields |
| `blockcache` | Re-scope as `residency`: prefetch, pinning, and the verified-cache backend on top of `cluster/completion`, with no new I/O layer |
| `analysis` | Split into `unicode` (UCD tables, folding, UAX #29), `snowball` (stemmer packaging), and `analysis` (Xapian-compatible generator, UAX #29 tokenizer, CJK, stoppers) |
| `sortable` | Right-sized. Xapiand also needs it to replace GPL `sortable_serialise` |
| `queryparser` (new) | Xapian `QueryParser` syntax and behavior. `boolean-parser` is Xapiand's other language, not a base for this one |
| `fusefilter` (new) | Binary fuse filters for primary keys in immutable segments, about 9 bits per key at 0.4% false positives with three memory accesses |
| `faultfs` (new) | Power-loss and fault injection shared by the core, the journal, and the group layer |
| `benchkit` (new) | Trial runner, statistics, JSON output, and the noise-band rule from section 2.4 |
| `group` | In the strata project as its own target. The core never depends on it |

Kronuz libraries in the wrong role:

| Library | Problem |
|---|---|
| `bloom-filter` | Compile-time `N`, 32 bits per key, 20 probes (20 cache misses per lookup). Wrong for runtime-sized, serialized, per-segment filters |
| `queue` | A mutex and condvar blocking MPMC queue. Never on reactor paths |
| `scheduler` | Runs its own timer thread. Engine timers come from the host executor |
| `allocators` | Accounts through a global `new`/`delete` override. Indexing buffers use arenas with explicit accounting |
| `lru-cache` | Fine for low-rate caches. Not for per-block or per-query hot paths |
| `string-similarity`, `double-metaphone`, `soundex`, `htm`, `cartesian`, `msgpack` | Host concerns that belong in Xapiand. The engine exposes sort-key and scored-filter APIs instead |
| `cpp-btree` | Apache-2.0. Allowed only if the license policy (R14) admits permissive non-MIT dependencies with notices |
| `cluster` | No LICENSE file, and the `consensus/` and `journal/` sources carry no license header. License the repository before strata depends on it |

### (l) Replication, durability, and consistency

**Ruling: approve the three-layer split. Approve `cluster/consensus` with
`cluster/journal` as the foundation, not as ready. Run document-mode replication only
through Phase 7. Make `quorum` the default acknowledgement. State the guarantee
conditionally. Close the consensus gaps on a parallel track before Phase 3R.**

**Where replication lives.** It lives in the strata project, in `strata::group`, as a
separate target above a replication-free core. Xapiand owns placement and routing.

- The coupled invariants are engine internals: log truncation depends on
  checkpoints, checkpoints on manifest retention, and replay on deterministic apply.
  Splitting them across a process boundary recreates today's failure. Xapiand's async
  WAL, async pull replication, and `DB_NO_SYNC` combine into acknowledged-but-lost
  writes, a revision-coupled WAL, and live-file copies that retry.
- The core stays standalone, so engine-level comparisons against Lucene and Tantivy,
  and deterministic simulation, run without a network.
- **Placement decisions come from a durable consensus authority** (a
  strict-consensus meta group), not from gossip. Gossip supplies liveness hints only.
  The legacy `cluster::Raft` is volatile ("restarting a voter loses its term, vote,
  and log", `cluster/SAFETY.md`), so it cannot decide Raft membership.
- When groups land, retire Xapiand's per-shard `ELECT_PRIMARY` election
  (`src/server/discovery.cc`), so leadership has one source of truth. Xapiand's
  `AGENTS.md` invariants about `ELECT_PRIMARY` and the `DB_UPDATED` multicast change
  with it.

**Determinism contract.** The leader resolves all nondeterminism before proposing:
UUID ids, timestamps, Lua script effects (`DatabaseHandler::call_script`), and schema
resolution, including foreign schemas read from other indexes. A command carries the
prepared document, the expected version, and the schema version. Apply rejects a
schema-version mismatch, and the leader re-prepares. Apply is a pure function of state
and batch. Tests compare a logical-content digest across replicas after every
schedule. Divergence that every checksum accepts is the failure checksums cannot
catch.

**Is reusing `cluster/consensus` and `cluster/journal` sound?** The shape is right:

- A deterministic, I/O-free Raft core with explicit persistence barriers.
- A journal that fails closed instead of truncating an acknowledged batch. Silently
  truncating a follower's acknowledged entry can lose a quorum-acknowledged write.
- `F_FULLFSYNC` on macOS.
- Injected-filesystem tests at every barrier.
- Completion backends with an Asio adapter that match ruling (a).

Both libraries are qualified for a fixed 3–5 voter authority ("Configuration
changes, authenticated transport and production capacity remain later gates",
`cluster/consensus/README.md`). Gaps, in order:

1. **G1, membership changes and voter roles.** Add and remove voters, non-voting
   learners that catch up before promotion, and an optional witness (a log-only voter
   that never leads). Without G1, a node that loses its disk cannot rejoin, because
   the README forbids rejoining under the old identity, so REPLICATION.md's "rebuilds
   it from a healthy peer" does not work today.
2. **G2, replica-count semantics.** Raft tolerates `f` failures with `2f + 1` voters.
   Xapiand's `num_replicas=1` (two copies) becomes a 2-voter group that tolerates zero
   failures and blocks writes when either copy is down. The product needs
   `rf ∈ {1, 3, 5}`, or two data voters plus a witness. This decision belongs to the
   owner.
3. **G3, repair at or below the commit cursor.** Consensus installs only images that
   advance past the receiver's commit cursor. A healthy voter whose segment files fail
   a checksum needs an image reinstall at or below its commit index, followed by replay
   of its own log.
4. **G4, multi-group economics.** A journal per group means an fsync stream and a
   heartbeat stream per group. Xapiand runs many indexes times shards per node, so this
   does not scale. Design one journal per core (or per disk) that multiplexes the
   groups that core owns, with group commit across groups, coalesced heartbeats, and
   quiescent idle groups. A shared log truncates only below its slowest group, so it
   needs forced checkpoints for laggards. Prototype it and measure fsyncs per second
   per node at 1,000 groups before Phase 3R.
5. **G5, stream transport** with flow control, then TLS before production. CRC is
   not authentication.
6. **G6, checkpoint representation.** Do not copy segments into journal artifacts:
   that doubles every segment write and runs into the 8-dependency and 512 MiB
   artifact bounds. The journal checkpoint bundle references the strata manifest
   (applied index, term, manifest CRC). Strata owns segment files and their retention,
   and snapshot transfer streams a segment pack in 64 KiB chunks. This removes
   REPLICATION.md gap 4 instead of patching the journal.
7. **G7, cost budget.**
   - A journal append is four barriers (journal sync, temporary manifest sync,
     rename, directory sync) and measures 62 ms p50 per durable group on the
     development Mac (`cluster/journal/README.md`, "Initial measurement"). Measure it
     on Linux NVMe, and consider an A/B checksummed frontier slot to reach two
     barriers. Group commit is mandatory.
   - The checkpoint freeze measured 85–520 ms on the Mac, which can exceed the default
     100–200 tick election window. Drive checkpoints by retained-log bytes and time,
     never by refresh, and budget the freeze against election timeouts.
   - Retune the defaults for bulk ingest: 1 MiB per command, 64 MiB retained log, and
     1,024 uncommitted entries (`cluster/consensus/types.h:54-63`). Recovery time
     equals replay of the retained log, so its size is also a recovery-time budget.

G1, G3, G4, and G6 close before Phase 3R starts. G5's TLS closes before production.
G2 is a product decision recorded in REPLICATION.md.

**Modes.**

| Mode | Ruling |
|---|---|
| `ack=quorum` | Default. With `rf=1` it equals a local fsync |
| `ack=all` | Keep it. State that it times out while any voter is down |
| `ack=local`, `ack=none` | Keep them, renamed or documented as "acknowledged before commit; lost on leader change". Raft truncates a deposed leader's uncommitted suffix, so a `local` write fsynced on the old leader is lost when another voter wins without it. REPLICATION.md's "survives process and power loss on the leader" is wrong for `rf > 1`. These modes match `DB_SYNCHRONOUS_WAL` and the async WAL only when `rf = 1` |
| `read=nearest` | Default |
| `read=session` | Compare the token with the replica's visible (refreshed) seqno, not its applied seqno. An applied but unrefreshed write is invisible |
| `read=linearizable` | A quorum probe plus waiting for a refresh that covers the captured commit index. Recommend it for get-by-id (served from the live version map). Searches pay a forced refresh |
| `refresh=false` | Default, with a 1 s interval like the current commit debounce. `wait_for` and `true` stay |
| Versioned writes | Evaluated at apply. Approved |

Define seqno as the pair (log index, position in batch), ordered lexicographically,
or as a packed integer with fixed position bits. "Log index plus its position in the
batch" collides: index 5 position 1 equals index 6 position 0.

Xapiand mapping, for REPLICATION.md:

| Xapiand today | Group equivalent |
|---|---|
| Async WAL default (`DB_NO_SYNC` plus async writer) | `ack=none` at `rf=1` |
| `DB_SYNCHRONOUS_WAL` | `ack=local` at `rf=1` (equals `quorum`) |
| `?commit` | `ack=quorum` plus `refresh=true` |
| Autocommit debounce (1 s) | Periodic refresh (visibility) and a checkpoint policy (log truncation) |
| `DB_DISABLE_WAL` for RESTORE | No log bypass. RESTORE uses `ack=none`, or a later bulk-import fast path where the leader builds segments and proposes one import command |
| `DB_DISABLE_WAL` for replica seeding | Checkpoint install |
| Async pull triggered by `DB_UPDATED` | Leader push, with learners for catch-up |
| Live glass file copy with retries | Install from a pinned manifest, with no retries |
| `ELECT_PRIMARY` | Group leadership |

The new default changes one experience. With `rf = 1` on the development Mac, one
durable group costs about 62 ms, so a single client issuing sequential writes slows
down. Concurrent clients amortize it through group commit. State this in the docs.

**Document versus segment replication.** Only `document` mode ships through Phase 7.
Every voter applies every batch, which costs indexing CPU times `rf` but gives
deterministic logical content and immediate failover. It also matches Xapiand's
current logical changesets. `segment` mode is a later experiment with its own gate.
Followers save at least 50% CPU at no more than three times the network bytes.
Failover re-indexes the suffix since the last fetched segment set within a stated
bound. The fault suite finds no new failure modes. Physical segments differ per
replica in document mode, so repair goes through G3, not through fetching individual
files.

**The honest guarantee.** Replace "zero chance" with:

> Under the fault model, at most `f` of `2f + 1` voters fail at once by crash, power
> loss, or disk loss; `fsync` and `F_FULLFSYNC` are honored; and nodes are not
> Byzantine. Within it, a write acknowledged with `ack=quorum` or `ack=all` is never
> lost, and a replica never serves a log entry, manifest, or block whose checksum
> fails where it is verified: it fails closed and reports the shard unavailable.

Outside the model, loss or corruption is possible:

- More than `f` simultaneous failures.
- Disks or filesystems that acknowledge flushes they do not persist.
- Corruption that CRC32C misses: about 2^-32 per corrupted block.
- Memory or CPU corruption before checksumming, as on non-ECC hosts.
- Software bugs.
- Pages re-read under mmap after verification.
- Writes acknowledged with `ack=local` or `ack=none`, which sit outside the no-loss
  guarantee by definition.

A strict mode that verifies every read uses the verified block cache from ruling (b).

**Phase plan.** Ruling (l) moves these items (R17, R18):

1. **Track C** runs in the `cluster` repository from now on, in parallel with
   Phases 1–3: C1 (G1), C2 (the G4 design and measurement), C3 (G3), C4 (G5
   transport), and C5, a deterministic simulation harness with a linearizability
   checker over client histories.
2. **Phase 3** gains deterministic `apply`, `checkpoint`, and `install`, the
   logical-content digest test, and the binding between the manifest and journal
   checkpoints.
3. **Phase 3R** starts only after C1–C4 pass. Its gate is in section 6.4.
4. **Phase 7** splits:
   - 7a puts strata behind the seam with Xapiand's WAL at `rf = 1`.
   - 7b moves shards to groups, replacing the WAL, the replication protocol, and
     `ELECT_PRIMARY`; moves placement to the authority; and adds the HTTP modes.
   - 7c removes the offload pools once Xapiand's blocking primitives are
     asynchronous.

## 5. Required changes

Before Phase 0:

1. **R1 (FEASIBILITY).** Restate the ingestion root cause as a hypothesis. Cite the
   benchmark facts in section 2.2, and make Phase 0's profile the evidence.
2. **R2 (BENCHMARKS).** Classify rows as must-win or report and use ratcheting
   thresholds. Define the noise band (five or more trials, median, 95% bootstrap
   interval of the ratio, at least 3% effect). Name a Linux gate machine with pinned
   frequency. Require equal semantics and a stated durability setting per row. Add
   memory, cold-start, recovery-time, and index-size rows, and a Xapiand-shaped
   corpus.
3. **R3 (PLAN Phase 0).** Add:
   - A flame-graph profile of Xapiand's RESTORE and query paths, attributing time to
     Xapian versus host code.
   - The offload hop cost (inline versus offloaded) on the current build.
   - A Xapian baseline driven through Xapiand's usage pattern (`replace_document` by
     term with the version check).
   - Oracle vectors from a GPL-side harness kept outside strata: `TermGenerator`,
     `QueryParser`, stemmers, BM25 scores, `_percent`, and estimates.

Before Phase 1:

4. **R4 (FEASIBILITY inventory).** Add rows 1–19 from section 3 with their engine
   mappings. Remove spelling and synonyms (row 20).
5. **R5 (ARCHITECTURE data model).** Define:
   - Terms as opaque byte strings with arbitrary-prefix iteration.
   - Stable monotonic docids and a persisted `lastdocid`.
   - The metadata store.
   - Opt-in stored term lists with a re-analysis fallback.
   - Doc values keyed by sparse 32-bit slots, multi-valued, with sparse presence.
6. **R6 (ARCHITECTURE write path).** Define revision, seqno, and version separately.
   Specify one owner core per shard and two-stage apply. Specify partitioned writers
   as a gated option with the marker commit cut. Specify atomic batches and cancel, a
   node-wide indexing memory budget, flush off the reactor, and the live version map.
7. **R7 (ARCHITECTURE async).** Remove the engine run queue. Add the scheduler and
   I/O concepts, the list of suspension points, window-based bulk scoring with the
   slice budget, least-attained-service scheduling, executor affinity, cancellation,
   and the guarantee text from ruling (a).
8. **R8 (ARCHITECTURE I/O).** Make mmap with asynchronous residency the default, list
   the pinned set, make the verified `pread` cache a second backend, use single-file
   segments, add the reactor-fault metric, and reuse `cluster/completion`.
9. **R9 (ARCHITECTURE codec and scoring).** Specify:
   - Exact per-document lengths, not 1-byte per-field norms.
   - Competitive (wdf, doclen) pairs in skip data.
   - FOR, PFOR, and the "no frequencies" and "all ones" flags.
   - The fixed 4-lane layout.
   - Block-max MaxScore as the default.
   - Exhaustive mode.
   - Statistics semantics after deletes.
   - Xapian's percent and the estimate triple.
10. **R10 (ARCHITECTURE term dictionary).** Make the block tree with a resident
    prefix FST the dictionary, and drop the full-vocabulary FST as primary.
11. **R11 (ARCHITECTURE structured data).** Specify geo as interval sets in a 2-D
    BKD, the scored-filter API, HTM in the host, the computed sort-key API, and the
    sorted-numeric, sorted-set, and binary column types.
12. **R12 (ARCHITECTURE analysis and query).** Make the Xapian-compatible
    `TermGenerator` the default, with UAX #29 as an additional analyzer and ICU
    optional for `cjk_words`. Pin Snowball. Add a `queryparser` library separate from
    `boolean-parser`.
13. **R13 (ARCHITECTURE libraries).** Apply the tables in ruling (k).
14. **R14 (FEASIBILITY constraints).** Write the clean-room procedure for behavioral
    compatibility. Restate the license policy as: no copyleft, no code derived from
    Xapian or Lucene sources, and permissive dependencies only with attribution,
    stating whether Apache-2.0 is allowed. List the GPL-derived files outside
    `src/xapian/` (row 21).
15. **R15 (PLAN structure).**
    - Add Phase 0.5, the Xapiand engine seam: route every Xapian call through an
      engine interface with Xapian as the first implementation, with zero E2E
      regressions and performance within the noise band. That interface becomes the
      authoritative inventory.
    - Move the async contract (`task` plus the residency API with a synchronous
      backend) into Phases 1–2, so Phase 6 becomes backends and tuning.
    - Split Phase 7 into 7a, 7b, and 7c.
    - Extend Phase 8 to the GPL-derived files outside `src/xapian/`.
16. **R16 (PLAN gates).** Replace "tests, benchmarks, README" with the numeric gates
    in section 6.
17. **R17 (REPLICATION).** Make these changes:
    - Redefine `local` and `none`.
    - Base `session` on the visible seqno.
    - Define `linearizable` cost and scope.
    - Define seqno as (log index, position) without collisions.
    - Add the determinism contract and the schema version in commands.
    - Make checkpoints a journal bundle that references the strata manifest, with no
      segment artifacts.
    - Add voter roles and replica-count semantics (G2).
    - Put placement on a strict-consensus authority and retire `ELECT_PRIMARY`.
    - Add the repair path (G3).
    - Unify checksums on CRC32C.
    - Use the honest guarantee text and the extended fault-model exclusions.
    - Add the Xapiand mapping table.
    - Extend the gap list to G1–G7.
18. **R18 (PLAN replication).** Add Track C with gates C1–C5, make C1–C4
    preconditions of Phase 3R, add deterministic `apply`, `checkpoint`, and `install`
    to Phase 3, and use the Phase 3R gate in section 6.4.
19. **R19 (FEASIBILITY effort and history).** Give an order-of-magnitude size per
    component. Cite the earlier unstallable-event-loop decision and the evidence that
    reverses it, with the Phase 0 offload measurement.
20. **R20 (factual fixes).** Correct "updated in place" (glass is copy-on-write), the
    "fifteen libraries" count, and macOS "no asynchronous file read API". Outside
    strata, flag the "memory-maps its storage" sentence in Xapiand's `benchmarks.md`.

## 6. Phase plan critique

### 6.1 Ordering

1. **The seam comes first, not in Phase 7.** Building the interface against Xapian
   proves the inventory, keeps Xapiand shippable, and makes 7a a swap instead of a
   rewrite.
2. **The async and residency contract comes before the scorers.** Phase 2 scorers
   written against synchronous memory get rewritten in Phase 6. Phase 6 should only
   add backends and tune.
3. **Analysis (Phase 5) gates correctness earlier than its position suggests.** The
   match sets the E2E suite asserts depend on Xapian-compatible terms. Move the
   `TermGenerator`-compatible analyzer into Phase 2 for `text` fields, and leave
   Phase 5 for the long tail: CJK, all stemmers, the `QueryParser`, and expansion.
4. **Phase 3R depends on Track C**, which belongs to another repository and is a
   multi-month effort. Starting it now keeps it off the critical path.

### 6.2 Missing gates

1. **Correctness parity.** Boolean queries return identical match sets against the
   Xapian oracle. BM25 top-10 is identical without deletes (relative error ≤ 1e-9),
   and drift is reported with deletes. The E2E suite pins `sort=_id` (for example
   `leaf-queries/text-queries/simple.md`), so it never checks ranking and needs this
   separate gate.
2. **Memory.** Indexing RSS stays within the node budget plus 10%. Serving RSS stays
   at or below Xapiand's for the same index (470 MB on the harness).
3. **Cold start.** Report open time per GB of index, and first-query latency with a
   dropped page cache. Xapiand opens shards lazily through the pool LRU, so open cost
   lands on queries.
4. **Recovery.** Report time to reopen after a crash, as a function of retained log
   size.
5. **Disk full.** No corruption, fail-closed writes, and recovery after space
   returns.
6. **Fuzzing.** Every decoder and parser runs at least one CPU-hour per target per
   phase under ASan and UBSan with zero findings.
7. **Format versioning.** Readers reject unknown versions, and a format change needs
   a test fixture.

### 6.3 Phase 1 gate (concrete)

APIs frozen at the gate:

- **`bitpack`:** FOR encode and decode of a 128-integer block given a bit width;
  delta decode with a base; PFOR with exceptions; VByte tails; the block header
  layout; and the "no frequencies" and "all ones" flags.
- **`fst`:** sorted-input `Builder::insert(key, output)` and `finish()`; `get`,
  `floor` (the block-tree hook), range stream, and `intersect(automaton)`.
- **`docset`:** an iterator concept (`doc`, `next`, `advance`, `cost`); dense and
  sparse sets; in-place AND, OR, and ANDNOT; cardinality.
- **`task`:** `task<T>`; the scheduler and I/O concepts; yield with a budget;
  `when_all`; cancellation; an `asio::awaitable` adapter in a separate target.
- **Shared:** the scorer iterator concept and the residency interface, with a
  synchronous backend, are defined even though no scorer exists yet.

Tests:

- Property round-trips for every bit width from 0 to 32 and every block position.
- Fuzzing as in section 6.2.
- Automaton intersection against brute force on 100,000 random patterns.
- For `task`: executor affinity, cancellation at yield, exception propagation,
  10^6-deep symmetric transfer in constant stack, and 10^8 tasks with random
  cancellation, clean under ASan, UBSan, and TSan.

Benchmark targets on the gate machine. Each is relative to a strong reference
measured in the same run, because absolute numbers before Phase 0 calibration would
be guesses:

| Library | Target |
|---|---|
| `bitpack` | Decode, including the delta step, at ≥ 90% of a reference SIMD-BP128/D1 implementation (benchmark-only, never linked), and ≥ 3× the scalar path. Encoded size within 2% of Lucene's FOR and PFOR output on the same Wikipedia postings, which Phase 0 dumps |
| `fst` | Exact lookup p50 within 2× an open-addressing hash map on the Wikipedia vocabulary, at ≤ 25% of its bytes. Build rate and bytes per term reported against a front-coded sorted array |
| `docset` | Within 1.2× of CRoaring (benchmark-only) on AND, OR, ANDNOT, and iteration for densities from 10^-6 to 0.5 |
| `task` | Create, await, and destroy of a no-op task ≤ 30 ns with zero `malloc`, checked by an allocation counter. Yield-and-resume through the Asio binding ≤ 1 µs p99 on an idle loop |

### 6.4 Later gate targets

1. **Phase 0.5.** Zero E2E assertion regressions against the pre-seam build,
   measured as the set of passing assertions out of 303 requests. Harness throughput
   within the noise band.
2. **Phase 2.**
   - Must win against Xapian on every query class and on single-thread indexing.
   - Single-thread indexing ≥ Lucene with equal settings (positions on, matching
     stored fields).
   - Top-10 latency within 1.5× of Tantivy on every class, or the gap explained before
     Phase 3.
   - The parity gate (section 6.2) on text fields.
3. **Phase 3.**
   - Power-loss injection at every barrier of a randomized workload across at least
     10,000 crash points, with zero acknowledged-write loss and zero inconsistent
     opens.
   - The determinism digest is equal across two replays.
   - Write amplification is reported against Lucene's tiered policy at equal segment
     budgets.
4. **Phase 3R.**
   - At least 10^7 simulated steps over at least 1,000 seeds each for 3 and 5 voters,
     with crashes, power loss at every barrier (journal, segment, and manifest),
     partitions, clock skew, membership changes, and corruption of every file class.
   - A linearizability checker passes over write and get-by-id histories.
   - Zero `quorum`-acknowledged loss and zero served corruption.
   - Replica rebuild after disk loss.
   - Quorum-ack ingest against Elasticsearch 8.x with one replica, both with one
     sequential client and with 16 concurrent clients.
   - p99 acknowledgement latency on Linux NVMe.
   - Fsyncs per second per node at 1,000 groups, within budget.
5. **Phase 6.** Reactor p99 stall ≤ 1 ms under the mixed heavy and light load.
   Light-query p99 under heavy load ≤ 2× its unloaded p99 plus 1 ms, against today's
   ~8 ms under offload. Major reactor faults per 1,000 queries ≤ 1 with the index at
   twice RAM. Must win against Lucene on the claimed rows.
6. **Phase 7.** Zero E2E regressions. `harness/cluster_check.sh` and a power-loss
   replacement for `harness/wal_recovery_check.sh` pass. The harness beats current
   Xapiand on all rows and Elasticsearch on the claimed rows.

## 7. Top risks the docs do not name

1. **Fixing the wrong bottleneck.** If Phase 0 shows ingestion is host-bound, the new
   write path does not close the Elasticsearch gap. Cheap host fixes, such as the
   per-update `allterms_begin` version scan, belong in Xapiand regardless.
2. **The behavioral long tail.** `TermGenerator`, `QueryParser`, `_percent`,
   estimates, and `OP_SYNONYM` statistics are where schedules slip, and the
   clean-room procedure slows each of them.
3. **Hidden blocking inside Xapiand.** Pool condvars, storage `pread`s, fsyncs,
   schema locks, and Lua make "no offloading" a Xapiand refactor as much as an engine
   feature. The pool's blocking checkout on a reactor thread can deadlock.
4. **Multi-group consensus economics.** The consensus library's design center is one
   authority group. Thousands of shard groups per node change the fsync, heartbeat,
   and memory math.
5. **Replica-count semantics.** Raft turns today's two-copy configuration into one
   with no fault tolerance.
6. **Benchmark noise on the development machine.** Thermal throttling on the i9
   MacBook, and no io_uring on macOS.
7. **Statistics drift on update-heavy small indexes.** This is common Xapiand usage,
   and it diverges from Xapian ranking until merge.
8. **Merge write amplification with stored records in segments.** Merges must
   bulk-copy compressed blocks without recompressing them, or ingest at scale loses to
   Elasticsearch.
9. **File-descriptor and fsync explosion.** Many shards times segments times files.
10. **Open cost of pinned structures** on lazily opened shards, and atomic snapshot
    refcounts shared across 16 cores. Use per-core snapshot handles published by
    message.
11. **Determinism leaks** (scripts, foreign schemas, id generation) that produce
    replicas which are logically different but internally valid, which checksums
    cannot detect.
12. **Unstated effort.** Three parallel programs (engine, consensus Track C, Xapiand
    seam and asynchronous refactor), each multi-month, with no sizing.
13. **License hygiene.** The unlicensed `cluster` repository, Apache-2.0
    `cpp-btree`, and Xapian-derived files outside `src/xapian/`.

## Addendum: re-review of the revised docs

- Date: 2026-10-11.
- Documents: `FEASIBILITY.md`, `ARCHITECTURE.md`, `BENCHMARKS.md`, `PLAN.md`, and
  `REPLICATION.md` as of 2026-10-11 09:01.
- Also read: `~/code/Detent/docs/LIBRARY_BOUNDARIES.md`, `~/code/Kronuz/varint`,
  `string-similarity/levenshtein.h`, `cluster/journal/codec.h`, the journal tests,
  the `cluster` working tree, and Xapiand commit `1691e5b2e`.

### Verdict

- **Phase 0: APPROVED.** Its baseline records gain items 1 and 2 below.
- **Phase 1: APPROVED.** Items 3 and 4 land before the Phase 1 APIs freeze; neither
  delays the start.
- Items 5–12 close before the later gates named in each item. Items 13–16 are
  follow-ups outside strata. None of them blocks Phase 0 or Phase 1.

### Status of R1–R20

| Item | Status | Evidence |
|---|---|---|
| R1 | Closed | FEASIBILITY "Ingestion throughput (hypothesis, measured in Phase 0)" lists the seven benchmark facts and makes the Phase 0 profile the evidence |
| R2 | Closed; item 5 scopes the VM rows | BENCHMARKS rules 1–5 (row classes, ratchets, noise band, gate machine, equal semantics) and the new rows |
| R3 | Closed | PLAN Phase 0: profile, hop cost, Xapian driven Xapiand's way, oracle harness, Lucene postings dump |
| R4 | Closed | The FEASIBILITY inventory covers rows 1–19; spelling and synonyms are out of scope |
| R5 | Closed | ARCHITECTURE "Data model" |
| R6 | Closed | ARCHITECTURE "Three counters" and "Write path" |
| R7 | Closed | ARCHITECTURE "Concurrency and async" |
| R8 | Closed | ARCHITECTURE "I/O"; `residency` on `completion` |
| R9 | Closed | ARCHITECTURE segment format, codec rules, scoring, statistics after deletes |
| R10 | Closed | Term blocks with a resident prefix FST |
| R11 | Closed | Doc-value columns, 2-D interval points, scored filters, sort-key functions |
| R12 | Closed | ARCHITECTURE "Text analysis" |
| R13 | Closed; items 7 and 9 correct two library choices | ARCHITECTURE "Libraries" |
| R14 | Closed | License policy, clean-room procedure, corrected GPL list |
| R15 | Closed | Phase 0.5, the residency interface in Phases 1–2, Phase 7 split into 7a/7b/7c, Phase 8 list |
| R16 | Closed for Phases 0 through 3, 3R, 6, and 7; item 8 covers Phases 4 and 5 | PLAN gates |
| R17 | Closed; item 11 corrects the witness | REPLICATION |
| R18 | Closed; item 12 fixes the Track C ordering | PLAN "Track C" and "Phase 3R" |
| R19 | Closed | FEASIBILITY effort table and the cited unstallable-loop decision |
| R20 | Closed in strata; item 15 tracks the Xapiand doc | FEASIBILITY |

### Owner decisions

1. **Replica counts** (`rf ∈ {1, 3, 5}` and `rf = 2 + witness`, no two-voter
   groups). Accepted, closing G2 as a decision. The witness role needs item 11.
2. **License.** Apache-2.0 dependencies with notices in `NOTICE`, no copyleft.
   Accepted.
3. **`cluster` license.** The MIT `LICENSE` (untracked) and the header changes (85
   modified files) sit uncommitted in the working tree. Accepted. Strata takes no
   build dependency on `cluster` until they are committed (item 13); Phases 0–2 need
   none.
4. **Gate machine.** QEMU/HVF virtual machines on the development Mac, same-session
   interleaved-trial ratios, and a bare-metal confirmation before Phase 7. Accepted for
   CPU-bound rows, with the scoping in item 5.
5. **Correction to stage-1 row 21.** Accepted. `src/length.{cc,h}` carries the MIT
   notice under Olly Betts' copyright, added with his permission in Xapiand commit
   `1691e5b2e` ("Got permission by olly to include length.h and length.cc with MIT
   license"), so row 21 is wrong on those two files. `sortable_serialise`,
   `remote_protocol_views`, and `xapian.h` are GPL as listed, and
   `remote_protocol_service.h` is MIT but includes GPL `pack.h`.
6. **No duplication of the Kronuz catalog.** Accepted. The revised docs reuse
   `varint`, `string-similarity`, `metrics`, `ledger`, and `endian`, and move CRC32C
   into `hashes`. Two choices still need correction: `faultfs` (item 7) and the edit
   distance (item 9).
7. **Detent coordination.** Accepted. The ordering ruling follows.

### Track C and Phase 3R against the library split

`LIBRARY_BOUNDARIES.md` plans these steps in order:

1. Extract `metrics`.
2. Split `completion`, `journal`, and `consensus` into their own repositories,
   renaming `cluster::consensus` to `kronuz::consensus` (about 90 references in
   Detent), adding the missing journal link, and deleting the forwarding headers.
3. Move Detent's TLS peer transport into `consensus/transport`.
4. Remove the legacy API halves, such as those in `completion/image.h`.
5. Extract `ledger`.
6. Steps 6–9 cover the HTTP client, other extractions, pinned revisions, and README
   rewrites.
7. Step 10 measures the legacy `cluster::Raft` against `consensus`, then keeps or
   retires it.

Ruling: Track C code depends on the split; engine Phases 0–2 do not.

1. Track C code for C1, the C2 prototype, C3, and C5 starts after steps 1 and 2
   land. Those steps are mechanical, planned first, and low-risk. Track C code
   written before them rebases across the namespace rename and the directory moves,
   and it lands in a layout that is about to disappear. Track C design work and the
   G7 Linux NVMe measurements start now on the existing code.
2. C4 reuses the transport, so it starts after step 3.
3. Phase 3R follows C1–C4, so it depends on steps 1–3.
4. Strata Phase 3 uses `ledger` for the node-wide indexing budget, so step 5 lands
   before Phase 3. If it has not, strata performs the extraction as its second
   consumer instead of writing a private budget.
5. The `completion` queue-depth change that the `residency` async backends need in
   Phase 6, and step 4's removal of the legacy `completion/image.h` paths, both land
   in the `completion` repository after step 2.
6. Step 10 and the meta group are one decision. The meta group is how Xapiand's
   control plane leaves `cluster::Raft`, so the step-10 measurement includes the
   meta-group workload (Xapiand's membership churn and placement writes), and G1
   serves both.
7. Step 8 and step 9 also apply to strata. Every `FetchContent_Declare` pins a tested
   commit from Phase 1 on, for benchmark reproducibility. Library READMEs state what
   the library is, its API, and its guarantees, with qualification evidence in
   strata's `docs/`.

### Open items

Phase 0 records:

1. **VM configuration in every baseline.** Every baseline JSON records the VM
   configuration (QEMU version, accelerator, vCPU count, memory, disk cache and AIO
   mode) and an fsync latency measured inside the VM. Elasticsearch fsyncs its
   translog per bulk and Xapiand's RESTORE does not, so the ingest ratio depends on
   the VM's flush path.
2. **A measured offload baseline for Phase 6.** Phase 0 measures light-query latency
   (p50 and p99) under the mixed heavy and light load on current Xapiand with its
   offload pools. The Phase 6 gate compares against "~8 ms under today's offload".
   That number comes from the June Kronuz/http prototype and states no percentile, yet
   FEASIBILITY now calls it p99 without a source.

Before the Phase 1 APIs freeze:

3. **Postings carry segment ordinals, not docids.** ARCHITECTURE defines docids as
   stable per-shard numbers that a replace keeps, so a replaced document's new
   version carries an old docid at a late ordinal, and ordinal order is not docid
   order.
   - Postings, the scorer iterator's `doc()`, and `docset` work on ordinals.
   - The docid map serves the docid-ascending tie-break, docid-order walks (dump with
     `skip_to` resume), and docid lookups.
   - Merges may re-sort documents by docid.
   - Today's text says "128-docid blocks" and "stores its docid in the term block";
     both mean ordinals.
4. **A tail-codec row in the `bitpack` gate.** Most terms in a Zipfian vocabulary
   have fewer than 128 postings, so their whole list is a tail, and prefix, wildcard,
   and fuzzy expansions touch thousands of them. `varint`'s `serialise_length` format
   saves a byte on values 128–254 and spends a 0xff escape byte on values of 255 and
   above: a 100,000-document gap takes 4 bytes, against 3 in LEB128.
   - The gate measures tail decode throughput and size on rare-term postings
     (document frequency below 128) for `serialise_length`, LEB128, group-varint, and
     StreamVByte. The tail format freezes on the result.
   - A new encoding goes into `varint`, per the owner's rule, not into `bitpack`.
   - The same measurement covers the integers in term blocks (file-offset deltas).

Before later gates:

5. **VM scoping (BENCHMARKS, before the Phase 2 gate).** Interleaved ratios absorb
   thermal drift for CPU-bound rows. They do not fix three VM distortions:
   - Guest disk reads can come from the host page cache, so cold-start,
     index-at-twice-RAM, and major-fault rows measure host RAM.
   - Guest fsync timing reflects QEMU's flush path, not NVMe.
   - macOS offers no vCPU pinning, so host preemption inflates absolute tails.

   Fsync and acknowledgement latency, cold start, twice-RAM faults, crash-recovery
   time, and the absolute tail targets are therefore bare-metal rows. The bare-metal
   run happens at the gate that claims them: Phase 3R for acknowledgement latency, as
   PLAN's "on Linux NVMe" already says, and Phase 6 for stalls and faults, not only
   before Phase 7. FEASIBILITY's risk row "Linux gate machine with pinned frequency"
   changes to match.
6. **CRC32C in `hashes` (before Phase 2).** CRC32C moves into `hashes` with SSE4.2
   and ARMv8 acceleration before segment footers use it. The journal's copy
   (`cluster/journal/codec.h:40-62`) is a byte-at-a-time table loop, and the journal
   switches to `hashes` in coordination with Detent.
7. **`faultfs` is an extraction, not a new library (before Phase 3).** The journal
   tests already contain the power-loss model `faultfs` needs: `Model` with visible
   and durable `Inode` contents, `MemoryFile`, `MemoryIO` over the `IO` interface, and
   `power_loss(keep_visible_namespace)` (`cluster/test/journal_test.cc:45-116`).
   Extract it, coordinated with Detent, into a library next to `completion`. Then
   route segment, live-docs, and manifest I/O through the `completion` `IO` interface,
   so one model covers journal, segment, and manifest barriers, as ruling (h)
   requires.
8. **Ratio targets for Phases 4 and 5.** Their gates name classes without the ratio
   targets that BENCHMARKS rule 2 requires. State targets against Xapian, Lucene, and
   Tantivy, and state from which phase each must-win indexing and memory row gates
   (indexing memory from Phase 3, when the budget lands).
9. **Fuzzy parity needs a different distance (before Phase 5).** Xapian's edit
   distance counts insertion, deletion, substitution, and transposition of two
   neighboring characters, over code points (`src/xapian/api/editdistance.h:32-41`).
   `string-similarity/levenshtein.h` computes plain Levenshtein over bytes and returns
   a normalized double, so it is neither the right test reference nor the right
   ranking for `OP_EDIT_DISTANCE` parity.
   - Extend `string-similarity` with an integer, code-point, transposition-aware
     distance. The oracle vectors settle restricted versus unrestricted
     transpositions.
   - `automata` builds Levenshtein DFAs with transpositions to match.
   - ARCHITECTURE also lists `string-similarity` both as an engine dependency and
     under "stay in the host"; it is now an engine dependency.
10. **`benchkit` histograms (non-blocking).** `benchkit` takes its duration
    histograms from `metrics` rather than defining its own.
11. **A witness may lead transiently (REPLICATION, before Track C's C1 code).** With
    `rf = 2 + witness` and "never leads", one failure can stall the group:
    1. Leader D1 commits entry e with the witness while data voter D2 lags.
    2. D1 fails.
    3. D2 lacks e, so the witness denies D2's vote under Raft's up-to-date rule.
    4. The witness cannot campaign, so the group has no leader until D1 returns. If
       D1's disk is lost, it never does.

    The witness must be able to win an election, replicate its log suffix to a data
    voter, and transfer leadership to it. It never applies, serves reads, or
    acknowledges client writes as leader. Restate the witness role and add this
    schedule to the C1 and C5 tests.
12. **Track C ordering (PLAN, before Track C code starts).** Replace "lands in
    whichever repositories exist at that point" with the ordering ruling above:
    - Steps 1–2 before C1, C3, C5, and the C2 prototype.
    - Step 3 before C4.
    - Step 5 before strata Phase 3.
    - Step 10 decided jointly with the meta group.

    REPLICATION names `kronuz::consensus` once step 2 lands.

Follow-ups outside strata (non-blocking):

13. **Commit the `cluster` license.** The `LICENSE` file and the header changes are
    for the owner to commit.
14. **Correct the `varint` provenance note.** `varint/README.md`, section
    "Provenance", says Xapiand's original length codec derives from Xapian's GPL code
    and is "not safe to republish" under MIT. Commit `1691e5b2e` records Olly Betts'
    MIT permission. Correct the README; `varint` stays an independent MIT
    implementation either way.
15. **Fix Xapiand's mmap claim.** Xapiand's `docs/src/content/docs/benchmarks.md:29`
    and `:105` still say Xapiand memory-maps its storage; glass reads with `pread`.
16. **Update the catalog.** `Kronuz/cpp-libraries` gets an entry as each strata
    library lands, per ARCHITECTURE rule 2. The catalog has no local clone under
    `~/code/Kronuz`.
