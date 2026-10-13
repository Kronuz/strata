# strata benchmark matrix

Every performance claim about the engine comes from this matrix.

## Rules

1. **Row classes.** A must-win row gates a phase. A report row is measured and
   explained but does not gate. Query speed, indexing speed, memory, and disk are
   must-win. Data strata keeps beyond what the baselines keep (term lists, exact
   lengths) is excluded from the disk comparison and reported in its own rows.
2. **Ratcheting thresholds.** Each phase states its targets against Xapian first,
   then against Lucene and Tantivy at ratios that tighten per phase (see
   [PLAN.md](PLAN.md)).
3. **Noise band.** At least five trials after warm-up. Report the median and the 95%
   bootstrap confidence interval of the ratio to the baseline. A row wins when the
   interval excludes 1.0 and the median effect is at least 3%.
4. **Gate machine.** Gates run in strata's own QEMU virtual machines on the
   development Mac (HVF acceleration, `-cpu host`): Linux with 8 vCPUs and 16 GB for
   io_uring and the gate runs, and FreeBSD for portability. They are separate from
   Detent's VMs. A gate compares ratios, not absolute numbers: strata and every
   baseline run in the same VM in the same session, with interleaved trials
   (A, B, A, B, …) so thermal drift and host noise affect both sides equally.
   Absolute numbers are informational. Every baseline run records the VM
   configuration and a measured in-VM fsync latency. Rows the VM cannot represent
   (cold start, faults with the index at twice RAM, fsync and acknowledgement
   latency, absolute tail targets) run on bare-metal Linux (NVMe, pinned CPU
   frequency) at the gates that claim them: Phase 3R, Phase 6, and Phase 7.
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

For each class: mean and p99 latency at concurrency 1, and throughput at saturation.

## System and lifecycle rows

| Row | Class |
|---|---|
| Serving memory (idle and under load) | must-win against Lucene, Tantivy, Xapiand, Elasticsearch, and Quickwit |
| Heap allocations and bytes per query, by class (allocation-counting build) | must-win from Phase 3, zero in steady state from Phase 6 |
| Reclamation (file deletion after merges and truncation): CPU per GB and longest slice | report |
| Cold start: open time per GB, first-query latency with a dropped page cache | report |
| Crash recovery time as a function of retained log | report |
| Reactor stall p99 and light-query p99 under heavy load | must-win from Phase 6 |
| Major reactor page faults per 1,000 queries, index at twice RAM | must-win from Phase 6 |
| System-level p50, p95, p99 under the Xapiand harness | must-win from Phase 7 |
| Quorum-ack ingest against Elasticsearch with one replica | must-win from Phase 3R |
| Ingest, search, and aggregation at 3, 30, and at least 300 nodes, against Elasticsearch and Quickwit | must-win from Phase 7 |
| Membership convergence, placement throughput, and fan-out latency at the scale contract (simulated) | must-win from Track D (DISTRIBUTION.md) |

## Reporting

Each run records the commit, compiler and flags, CPU model, governor, kernel or OS
version, corpus checksum, settings per row, and trial count. Results are stored as
JSON by `benchkit` and summarized in a table per gate.
