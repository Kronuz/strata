# strata benchmark matrix

Every performance claim about the engine comes from this matrix.

## Rules

1. **Row classes.** A must-win row gates a phase. A report row is measured and
   explained but does not gate. Query speed, indexing speed, memory, and disk are
   must-win. Data strata keeps beyond what the baselines keep is excluded from the
   disk comparison and reported in its own rows ("Ratchet rows", the counterpart
   table).
2. **Ratcheting thresholds.** Each phase states its targets against Xapian first,
   then against Lucene and Tantivy at ratios that tighten per phase (see
   [PLAN.md](PLAN.md)).
3. **Noise band.** At least five trials after warm-up. Report the median and the 95%
   bootstrap confidence interval of the ratio to the baseline. A before/after claim
   (Track P) or a row whose target is to beat a baseline wins when the interval
   excludes 1.0 and the median effect is at least 3%. Ratchet rows pass by PLAN.md's
   rule instead: the interval's bound on the target's side meets the target.
4. **Gate machine.** Gates run in strata's own QEMU virtual machines on the
   development Mac (HVF acceleration, `-cpu host`): Linux with 8 vCPUs and 16 GB for
   io_uring and the gate runs, and FreeBSD for portability. They are separate from
   Detent's VMs. A gate compares ratios, not absolute numbers: strata and every
   baseline run in the same VM in the same session, with interleaved trials
   (A, B, A, B, …) so thermal drift and host noise affect both sides equally.
   Absolute numbers are informational. Every baseline run records the VM
   configuration (the Linux VM's disk passes discards to the host, `discard=unmap,
   detect-zeroes=unmap`, since 2026-10-14; I/O figures from before need a re-baseline
   to compare) and a measured in-VM fsync latency. Rows the VM cannot represent
   (cold start, faults with the index at twice RAM, fsync and acknowledgement
   latency, absolute tail targets) run on a bare-metal machine at the gates that
   claim them: Phase 3R, Phase 6, and Phase 7. The M1 Pro laptop that held this role
   was retired on 2026-10-14 (its device management keeps disabling remote login); the
   replacement is open, a rented bare-metal Mac (AWS `mac2.metal`, an M1 Mac mini) for
   macOS arm64 and a Graviton metal instance for Linux arm64, booked for one batched
   session per gate. On a Mac, durability means `F_FULLFSYNC` (macOS `fsync` leaves data in the drive's
   cache), every engine uses the same barrier, and each row states it. CPU frequency
   cannot be pinned on Apple Silicon: runs are on AC power with Low Power Mode off,
   interleaved as above, with at least seven trials and the interval reported.
   Linux-specific rows (io_uring, Linux page-cache behavior, cgroup memory) stay in
   the Linux VM.
5. **Equal semantics.** Same analyzer, same scoring function or count-only
   collectors, same stored fields, same durability setting. Each row states its
   settings. Engine-level rows without fsync are never compared with system-level
   rows with translog fsync.

## Baselines

| System | How it runs |
|---|---|
| Xapian 2.x | Direct C++ API, glass backend; also driven Xapiand's way (`replace_document` by id term with the version check) |
| Lucene (current release) | JVM harness in the style of the tantivy search-benchmark-game, warmed |
| Tantivy (current release) | The search-benchmark-game harness |
| Elasticsearch 8.x | Xapiand's `harness/es_loadtest.py`, system level, extended with concurrent clients |
| Quickwit (current release) | System level, local and object-storage splits, from Phase 7 and Track D6 |
| Xapiand (current master) | Xapiand's `harness/loadtest.py`, system level |
| IResearch 26.03.1 | The search-benchmark-game harness (C++, the engine behind ArangoDB's and SereneDB's search) |
| PISA 0.8.2 (reference) | The search-benchmark-game harness; a static-index query processor without updates, deletes, or durability. Reported beside the query rows as the state of the art for query processing, not a ratchet target |

Each baseline runs in its fastest supported configuration (for Lucene, with and
without document reordering, `-bp`), and the faster one sets the reference. The
targets per phase are PLAN.md's ratchets (query speed, indexing speed, memory, and
disk).

## Corpora

- English Wikipedia, about 5 million articles (search-benchmark-game corpus).
- A Xapiand-shaped corpus: prefixed terms, boolean terms, sparse value slots, nested
  fields, and frequent replaces.
- Xapiand's harness datasets for system-level runs.
- GeoNames points and shapes for range and geo queries.

## Indexing

| Row | Class | Measured as |
|---|---|---|
| Throughput, 1 thread | must-win | Documents per second to a committed index |
| Throughput, N threads | must-win | Same, all cores |
| Throughput, replace-heavy | must-win | Xapiand-shaped corpus, 50% updates |
| Index size | must-win | Bytes after a full merge, whole and per section against Lucene's and Tantivy's (PLAN.md, Ratchets); before merging and with and without term lists as report rows |
| Refresh latency | report | Write to visibility |
| Write amplification | report | Bytes written over final index bytes |
| Indexing memory | must-win | Peak RSS within the node budget plus 10%, and against Lucene and Tantivy (PLAN.md, Ratchets) |

## Search

Query classes, each measured for top-10 and count, and some for top-1000:

| Class | Example |
|---|---|
| Term | `title:obama` |
| Conjunction | `+barack +obama` |
| Disjunction | `barack obama` |
| Phrase | `"barack obama"` |
| Filtered | term + numeric range filter |
| Sort by field | term query sorted by a date slot |
| Range | numeric and date ranges |
| Geo | distance and polygon |
| Prefix, wildcard, fuzzy | `obam*`, `ob?ma`, `obama~2` |
| Aggregations | terms, histogram, stats over a match set |
| Expansion | ESet over the top 10 documents |

Ratchet rows score each class as "Ratchet rows" defines. p99 latency at concurrency 1
and throughput at saturation are report rows until Phase 6, which gates them with the
reactor rows.

## System and lifecycle rows

| Row | Class |
|---|---|
| Serving memory (idle and under load) | must-win against Lucene and Tantivy from Phase 3; against Xapiand, Elasticsearch, and Quickwit from Phase 7 |
| Heap allocations and bytes per query, by class (allocation-counting build) | property row: recorded at Phase 2; no growth with the number of matches from Phase 3; zero in steady state on the core paths from Phase 6 |
| Reclamation (file deletion after merges and truncation): CPU per GB and longest slice | report |
| Cold start: open time per GB, first-query latency with a dropped page cache | report |
| Crash recovery time as a function of retained log | report |
| Reactor stall p99 and light-query p99 under heavy load | must-win from Phase 6 |
| Major reactor page faults per 1,000 queries, index at twice RAM | must-win from Phase 6 |
| System-level p50, p95, p99 under the Xapiand harness | must-win from Phase 7 |
| Quorum-ack ingest against Elasticsearch with one replica | must-win from Phase 3R |
| Ingest, search, and aggregation at 3, 30, and at least 300 nodes, against Elasticsearch and Quickwit | must-win from Phase 7 |
| Membership convergence, placement throughput, and fan-out latency at the scale contract (simulated) | must-win from Track D (DISTRIBUTION.md) |

## Ratchet rows

How PLAN.md's ratchets are measured.

- **Equal content.** Every engine indexes the same corpus with the same analysis
  (each engine keeps the terms its own limits allow: Xapian skips terms over 245 bytes,
  2 in the 5M corpus; strata keeps them, docs/design/limits.md),
  positions, norms, and stored fields, and the same keys: from Phase 3, a unique id
  term per document (Lucene `StringField`, Tantivy `STRING`) with versions as doc
  values or fast fields; the Phase 2 rows ran without keys in every engine.
- **Query speed.** Per class, the geometric mean over its queries of strata's time
  over the reference engine's, each query's time its fastest of 10 runs, with the pipe
  floor subtracted from both (at least 1 µs each). The pipe floor is the smallest time
  of any engine and query over every trial of the session (`bench/phase2/gate.py`),
  and the report names the trial it came from. Counts must equal every baseline's.
- **Indexing speed.** The full 5M corpus to a committed index, one thread and N
  threads (the VM's 8 vCPUs), each engine with its own merging; 1M documents as a
  report row. **Equal memory, not equal numbers:** engines count their buffers
  differently (Lucene's `RAMBufferSizeMB` counts inverted postings in byte pools,
  strata's buffer share counts un-inverted documents by its own estimate), so the same
  setting holds different numbers of documents and different memory. strata runs under
  a node budget (D17) calibrated so its peak RSS equals the reference engine's at the
  reference's setting; the report states every engine's setting, peak RSS, and segment
  count. strata's engine indexes through `strata::index` (keys, versions, flushes,
  merges, one commit), upserting by key, with `STRATA_THREADS` inversion threads (D18). "Before merging" is the index each engine
  leaves after that run, with its segment count recorded; "after merging" is each
  force-merged to one segment.
- **Memory.** Each engine in its own cgroup v2, with `memory.current` and
  `memory.stat` (anon, file, file_mapped) sampled at a fixed interval, so the page
  cache is charged alike to every engine. Serving: idle with the index open, and under
  the gate query workload, peak and steady state. Indexing: the same buffer budget for
  every engine, peak. The JVM's heap is the smallest `-Xmx` at which Lucene's query run
  stays within the noise band of its fastest, stated in the report.
- **Disk.** Bytes after a full merge, whole and per section, single exact
  measurements.
- **Write amplification (T3).** Bytes the block device wrote during the run
  (`/proc/diskstats`), over the final index bytes, at equal memory as above,
  append-only and with half the documents replacing earlier ones by id
  (`bench/write_amp.sh`).
- **Correctness beside speed.** `strata_engine verify` (every query, both scoring
  schemes, against an exhaustive scan) and `parity` (against the oracle's Xapian rank
  vectors) run on the multi-segment index before merging, after it, and with deletes.

| strata section | Lucene | Tantivy |
|---|---|---|
| postings | `.doc` (impacts included) | `.idx` |
| positions | `.pos` | `.pos` |
| terms, term_index | `.tim`, `.tip`, `.tmd` | `.term` |
| stored | `.fdt`, `.fdx`, `.fdm` | `.store` |
| norms | `.nvd`, `.nvm` | `.fieldnorm` |
| docids, versions, `pk_filter` | the id field's terms and doc values | the id field's terms and fast field |
| lengths (exact, Xapian's semantics) | excluded, reported | excluded, reported |
| term lists (Phase 4) | excluded, reported | excluded, reported |

Sections compare as placed: strata keeps term-level competitive pairs in the term
dictionary, Lucene keeps impacts in `.doc`. Segment metadata files count in the whole
index only.

## Tiers

Three tiers, cheapest first. Exploration uses the first two; a gate row uses the third.

| Tier | What | Time | Host |
|---|---|---|---|
| A/B | `bench/quick/ab.sh`: two strata builds in-process over the gate queries, sides alternating, best of RUNS per query and round; `ab.py` scores it with a bootstrap interval | Minutes | Any; noise hits both sides alike |
| Quick | The harness against every engine (PISA included) over a fixed sample: `bench/sample_queries.py` keeps up to 40 queries per class with seed 7 (181 gate queries and 40 stratified terms, `queries-sample.txt` and `terms-sample.txt` in the harness directory), `SETS="queries-sample terms-sample" TRIALS=2 bench/query_trials.sh NAME`, scored by `bench/phase2/gate.py` | About a twelfth of a full run | The gate VM under the quiet guard |
| Gate | Every query, five trials, the engine order rotated (`bench/overnight.sh`) | One night | The gate VM under the quiet guard |

In-process A/B numbers compare strata builds only: the harness adds the client pipe, so
they are not ratios against the baselines. A quick-tier ratio is a direction, not a
verdict; a class it moves past the target is confirmed at the gate tier.

## Harness notes

- Engines read the corpus from stdin in large blocks: macOS's libc++ reads `std::cin`
  a character at a time (`completion/docs/PLATFORM_FINDINGS.md`, §6), which once made
  strata's macOS indexing row three times slower than its indexing.
- IResearch's and PISA's builds fetch git submodules inside their build steps, which
  failed in the gate VM on 2026-10-15; until fixed, their rows are owed and each report
  says so.

## Reporting

Results go in the gate reports (`docs/gates/`) and Track P's notes (`docs/perf/`),
never here; this file is the method. Each run records the commit, compiler and flags, CPU model, governor, kernel or OS
version, corpus checksum, settings per row, and trial count. Results are stored as
JSON by `benchkit` and summarized in a table per gate.
