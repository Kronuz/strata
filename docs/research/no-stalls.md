# Background work without stalls: the bound, the survey, and what to build

- Date: 2026-10-18. Branch `research/no-stalls` (worktree `~/code/Kronuz/strata-wt/no-stalls`),
  from main `734949e`. Ledger `docs/status/no-stalls.md`.
- Prototypes: `research/no-stalls/` (built with `-DSTRATA_RESEARCH=ON`): `ingest.cc`, the strata
  driver with the controllers; `rocks.cc`, the same loop over RocksDB v11.8.1 (the newest tag,
  2026-08-04); `core.sh` runs the matrix; `summarize.py` medians the rounds per window. Raw
  outputs are in the worktree's `.scratch/runs/` (untracked; the tables below are copied from
  them).
- Machine: the Mac (Intel i9-9980HK, 8 cores, 32 GB, macOS 15, APFS on the internal SSD,
  Apple clang 17, Release `-O3 -march=native`), light holds, other agents' single-thread jobs
  possibly live. Every run is 2 to 3 threads (a writer, a reader, one merge thread), so the
  machine is never the constraint; the merge *budget* is, by construction (section 4).

## 1. The problem, in the owner's words

Under sustained ingest, merges fall behind, debt grows, and then either writes stall or
queries slow as segments pile up. RocksDB answers with hard thresholds (a slowdown rate at
20 L0 files, a stop at 36): latency cliffs. "Slow merges slow everything at the worst possible
time", as garbage collectors do. strata serves millions of indexes per host, tens of thousands
open per process, on one shared pool (`Kronuz/threadpool` feature/scheduler: fair groups,
classes query > flush > merge), so the answer has to hold across indexes, not per index.

The brief asks for the bound (what merge work an ingest rate requires and what a policy can
avoid), the survey, at least three ideas with one that breaks a common assumption, each with
a prototype at sustained ingest against RocksDB under the same load, reporting write and
query latency over time as p50/p99/p999 per window, never averages.

## 2. The bound

### 2.1 What an ingest rate costs in merges

Let a host ingest `W` bytes per second of flushed segments (strata writes 0.36 bytes of segment
per corpus byte at 5,000-document flushes: 2.1 MB/s at 5,000 documents/s of this corpus) in
flushes of `f` bytes, and keep at most `S` segments. A tiered policy with fanout `T` (Lucene's
and strata's: 8 per tier, 10 per merge) merges a byte once per tier it climbs, so over a run
that flushes `N` times every byte is rewritten about `log_T(N)` times: 2 to 3 times in a
three-minute run at one flush a second (180 flushes), 5 to 6 times over a day (86,400). The
merge *bytes* are therefore `W · log_T(N)` per second of ingest, a slowly growing multiple of
the flush rate, and the merge *CPU* is those bytes times the cost per byte, which for strata
today is the dominant term: 497 ns per posting against a copy floor of 24.6 ns
(`docs/research/indexing.md` 5.3), 20×. Section 4.1 measures today's merge at about 48 µs
of CPU per document merged, 125 µs per document ingested at the 2.6 rewrites a two-minute
run reaches, against the writer's own 90 µs per document (10 to apply, 80 to flush); with
the copy merge it would be 2 to 3 µs per document merged.

Nothing a policy does changes `W`; what it can change is the exponent and the constant:

| Lever | Changes | Pays with |
|---|---|---|
| Fanout `T` (wider merges) | `log_T(N)`: fanout 30 instead of 8 is 1.6× fewer rewrites | up to `T` segments per tier for queries to visit |
| The floor (bigger flushes) | `N`: flushing every 10 s instead of every 1 s is one tier less | refresh latency and buffer memory |
| The merge's cost per byte (copy merges) | the constant, 20× | format 13's items (a base ordinal per term, tail-coded blocks anywhere, aligned positions) |
| Which merges run first (small tiers first) | nothing in the total; the *segment count* per byte written | the big tiers' debt waits, which costs queries nothing per byte and the host a cap |
| Admission | `W` itself, by slowing writers | throughput, which is the only thing left to pay with when the budget is short |

### 2.2 What the debt costs

Debt has two prices and a controller needs both:

1. **Queries pay per segment**, whatever the segment's size: a term lookup and a scorer per
   segment per query. Section 4.1 measures it on this corpus: TOP_10 over the 300-query
   sample costs about 30 µs per extra segment at the median (5 segments: 0.1 ms; 27:
   0.7 ms) and more at the tail (p99 0.3 → 2.1 ms), so a hundred small segments are about
   3 ms on the median query and 10 ms at its p99.
2. **Writers pay nothing until someone makes them.** Today strata's writer never waits for a
   merge (merges run on the executor; storage admission is the only refusal), so under a
   merge budget below demand its segment count grows without bound and queries pay all of
   it; RocksDB's writer pays at 20 L0 files and stops at 36.

### 2.3 What "no stalls" can mean

Under sustained demand above capacity the only steady states are: writers slowed to the
merge capacity, queries slowed by unbounded segments, or data dropped. So "no stalls" cannot
mean "writers never slow"; it has to mean:

1. **No cliff.** The slowdown is proportional and starts early, so throughput and latency
   are continuous functions of debt, never a step to 16 MB/s or to zero.
2. **No synchronous wait for a merge to finish** anywhere in the write path (Lucene's
   `maybeStall` and RocksDB's stop both wait for a merge; both are cliffs).
3. **Capacity measured, not assumed.** The admitted rate follows the merge throughput the
   host actually delivers (CockroachDB's tokens from bytes compacted out of L0), so a slow
   disk or a busy pool lowers admission smoothly instead of walking it into the stop.
4. **The query side bounded by policy**, by a cap on segments per index past which the
   index's merges outrank every other index's (section 5's `hot`), so a cold index can carry
   debt and a hot one cannot.

What hardware sets: on this host one merge thread today merges about 9 MB/s of segments
when busy (section 4.1: 5.6 MB/s at 60% of a core), one writer thread flushes 2.1 MB/s at
5,000 documents/s, so one merge thread keeps up with one writer thread at fanout 8 through
about four tiers (an hour at one flush a second) and falls behind as the tiers deepen or the
writer speeds up; the copy merge moves that line by 20×, past any ingest rate one writer can
produce.

## 3. Survey

Read freely: Lucene (Apache-2.0), Tantivy (MIT), RocksDB (Apache-2.0 of its dual license),
Pebble (BSD-3), ScyllaDB's and CockroachDB's published designs, the papers. Nothing is
ported, so no notices travel. The engines' numbers below are read from the pinned sources
this session (Lucene 10.4.0, Tantivy 0.26.0, RocksDB v11.8.1 `include/rocksdb/advanced_options.h`
and `db/column_family.cc`).

### 3.1 RocksDB: thresholds, a delayed rate, a rate limiter

Write stalls (`advanced_options.h`): `level0_slowdown_writes_trigger = 20`,
`level0_stop_writes_trigger = 36`, `soft_pending_compaction_bytes_limit = 64 GiB`,
`hard_pending_compaction_bytes_limit = 256 GiB`, `max_write_buffer_number = 2` (a second
full memtable with the first still flushing is a stop). Past a slowdown trigger every write
is paced at `delayed_write_rate` (16 MB/s unless set), which `SetupDelay`
(`db/column_family.cc`) then adapts: × 0.8 whenever compaction debt did not shrink since the
last check, × 0.6 near or at a stop ("the penalty is more than the reward"), ÷ 0.8 when debt
shrank, never above the user's rate, never below 16 KB/s; `WriteOptions::no_slowdown` returns
`Incomplete` instead of waiting. The rate limiter (`NewGenericRateLimiter`: a token refill
every 100 ms, fairness 10, flushes at high priority, compactions low) caps background writes;
its auto-tuned variant (2017) moves the cap multiplicatively between the user's ceiling and a
derived floor from how often the limiter drains. Intra-L0 compaction merges L0 files among
themselves when L0 → L1 cannot keep up, which is the one place RocksDB *changes the merge
shape* under debt rather than the writer's rate.

What to keep: the adaptive delayed rate is a proportional controller hiding behind a cliff;
the cliff (nothing before 20 files, a stop at 36) is the failure mode. Section 4 measures it.

### 3.2 TiKV's flow control and CockroachDB's admission control: the stall replaced

TiKV (`storage.flow-control`, default on) disables RocksDB's write stall for its KvDB and
RaftDB and throttles its own scheduler instead, from `memtables-threshold = 5`,
`l0-files-threshold = 20`, `soft-pending-compaction-bytes-limit = 192 GiB` (some requests
rejected with `ServerIsBusy`), `hard-pending-compaction-bytes-limit = 1024 GiB` (all
rejected). Smoother, and above the engine rather than inside it, but still a reject at the
hard limit.

CockroachDB's admission control (the 2022 design; Pebble underneath) is the cleanest prior
art for this brief: store writes are admitted with byte tokens computed, when the store is
overloaded, from "bytes added to the system, and bytes compacted out of level 0", with the
goal of keeping L0 read amplification at or below a configurable threshold (20 sublevels).
The admitted rate is the measured drain rate: feed-forward from capacity, not feedback from a
threshold. Pebble keeps hard backstops (`L0StopWritesThreshold = 12` sublevels,
`MemTableStopWritesThreshold = 2` memtables), and its `L0CompactionFileThreshold = 500` is
documented against admission control's "threshold of 1000 files to start throttling".

### 3.3 Lucene: a stall of its own, and a throttle that is off

`ConcurrentMergeScheduler` (10.4.0): `maxThreadCount = max(1, cores / 2)`,
`maxMergeCount = maxThreadCount + 5`; `maybeStall` pauses an *indexing* thread in a
`wait(250)` loop while merges are pending and `mergeThreadCount() >= maxMergeCount`: a hard
stall on merge count, Lucene's version of RocksDB's stop. The auto I/O throttle is **off by
default** (`doAutoIOThrottle = false`); when on, merges start at 20 MB/s, the rate rises
20% when a new big merge (≥ 50 MB) arrives while another of similar size has run for over
3 s (a "backlog"), falls by ÷ 1.1 otherwise, between 5 and 10,240 MB/s, and
`MergeRateLimiter.maybePause` sleeps the merge thread in slices of at most 250 ms (nothing
under 2 ms); merges beyond `maxThreadCount` are paused outright (rate 0, "stopped").
`TieredMergePolicy`: `segsPerTier = 8`, `maxMergeAtOnce = 10`, floor 16 MB, max 5 GB,
`deletesPctAllowed = 20`, score = skew × bytes^0.05 × nonDelRatio², which strata's
`merge_policy` reproduces.

What to keep: pausing a merge thread at write boundaries is cheap and Lucene already does
it (section 4.3 measures strata's checkpoint spacing); what to avoid: a stall keyed to a
count of merges.

### 3.4 Tantivy: debt unbounded

`LogMergePolicy` (0.26.0): levels by log2 of the document count (`level_log_size = 0.75`,
`min_layer_size = 10,000`), a level with ≥ 8 segments merges, nothing above 10M documents
merges. `SegmentUpdater::consider_merge_options` runs after every segment add, commit, and
merge end; `start_merge` spawns on a pool of `num_merge_threads = 4` and never blocks;
nothing throttles indexing but the 10,000-document pipeline channel. Tantivy is where strata
is today: writers never wait and segments accumulate without bound.

### 3.5 SILK: priority and preemption inside the engine

SILK (Balmau et al., USENIX ATC 2019, best paper): tail latency comes from "interference
between client writes, flushes and compactions"; three mechanisms: idle I/O bandwidth goes
to internal work, flushes and L0 → L1 compactions outrank higher-level ones, and higher-level
compactions are preempted when the low levels need the bandwidth; up to two orders of
magnitude lower p99 than RocksDB and TRIAD on a Nutanix workload. The mapping to strata: the
flush and the small-tier merges are what keep the writer's path clear (the writer never
blocks on them here, but the segment count does), so they outrank big-tier merges, and a
big merge must be *preemptible* at a bounded granularity, which is section 4.3's finding.

### 3.6 ScyllaDB: a backlog controller instead of thresholds

ScyllaDB's compaction controller (2018) defines the backlog of size-tiered compaction as the
bytes every SSTable still has to pass through: `B = Σ S_i · log₄(ΣS / S_i)` (base 4 because
STCS merges four), minus what compaction has already read, so a single SSTable is zero debt
and a byte in a small SSTable is several tiers of debt; compaction's scheduling shares are
proportional to `B`, with foreground writes taking the rest of a fixed bandwidth. A doubling
of ingest (55 → 110 MB/s) settles at a higher backlog and higher compaction shares with
"latencies predictable and stable in every percentile". This is the debt unit strata should
adopt (bytes × tiers remaining, not a count of files), and the proportional *CPU* share is
what the pool's classes and quanta provide (threadpool `docs/research/scheduler.md`, R2, R3).

### 3.7 The LSM design space: Dostoevsky, PebblesDB, WiscKey, X-Engine, bLSM, LevelDB

- **Dostoevsky** (Dayan and Idreos, SIGMOD 2018) and **Monkey** (SIGMOD 2017): lazy
  leveling (tiering at every level but the largest, leveling there) keeps point-lookup cost
  while cutting update cost, and the Fluid LSM-tree makes the merge width per level a dial
  tuned to the workload. The dial is section 4's I2: widen merges when behind.
- **PebblesDB** (Raju et al., SOSP 2017): guards let SSTables overlap within a level so
  compaction never rewrites data inside a level, 2.4 to 3× less write amplification for
  slightly slower reads. Term-major segments have no key-range partition to exploit; the
  analog is not rewriting what needs no re-encoding (the copy merge).
- **WiscKey** (Lu et al., FAST 2016) and RocksDB's BlobDB: values out of the tree, so merges
  move pointers. strata's stored records already copy raw at merge when none of their
  documents is deleted; the posting blocks are the "values" the copy merge appends.
- **X-Engine** (Huang et al., SIGMOD 2019, Alibaba): compaction reuses unchanged extents
  (2 MB) and data blocks whose ranges do not overlap, instead of rewriting them, cutting
  compaction I/O and CPU: the closest published precedent for copy merges that append whole
  blocks.
- **bLSM** (Sears and Ramakrishnan, SIGMOD 2012): the "spring and gear" scheduler ties the
  memtable's fill rate to the merge's progress so that neither runs ahead, bounding write
  latency without pauses: a proportional controller on the writer keyed to merge progress,
  the first published form of section 4's I1.
- **LevelDB's seek compaction**: a file that absorbs more seeks than its size over 16 KB is
  compacted, because the reads it costs now exceed the compaction's cost; RocksDB removed
  it as unpredictable. It is the per-file form of section 5's assumption breaker: charge
  the merge to the reads it saves.
- **TRIAD** (Balmau et al., USENIX ATC 2017): hot keys kept in the memtable, L0 built from
  the commit log instead of a flush. Not applicable to segments, noted for completeness.

### 3.8 The garbage-collection analogy, taken seriously

The owner's framing is GC's: a collector that cannot keep up stops the world. The GC answers
are work-based incremental collection (Baker 1978: the mutator does a bounded slice of
collection per allocation, so debt cannot diverge) and time-based scheduling with a minimum
mutator utilization guarantee (Metronome, Bacon, Cheng, Rajan, POPL 2003: in every window of
length `w` the mutator gets at least `u·w`). For strata: the pool's reservation and the
100 µs quantum give queries the MMU (`scheduler.md` 5.2: a query waits 12 to 40 µs at the
median on a saturated pool); work-based pacing is admission from measured capacity (I1);
and both need the collector's unit of work to be small, which is the merge slice (I3).

### 3.9 What the survey says about this problem

1. Everyone bounds debt at the writer eventually (RocksDB, Lucene, TiKV, Pebble); only
   Tantivy and strata today do not, and pay in queries. The question is the *shape*: a
   threshold (RocksDB, Lucene) or a proportional controller (ScyllaDB's shares, CockroachDB's
   tokens, bLSM's gear).
2. The unit of debt should be bytes × tiers remaining (ScyllaDB), which a tiered policy can
   compute from segment sizes alone; a count of files (RocksDB's L0) is a proxy that ignores
   how much each file will cost.
3. Nobody schedules merges across thousands of indexes by what the merge is worth; LevelDB
   priced compaction by reads once, per file. With a host that serves millions of indexes,
   that price decides where the merge budget goes.
4. The merge must be preemptible in small slices for any of this to hold on a shared pool
   (SILK's preemption, Lucene's pause); strata's merge today yields every output chunk, and
   section 4.3 says how long that is.

## 4. The prototypes and what they measured

### 4.0 The harness

`research_ingest` (strata) and `research_rocks` (RocksDB) run the same loop: one writer
thread upserts the corpus's documents (the first 100,000 of the gate corpus, 1,177 bytes
each, cycled with fresh keys) in transactions of 10 at an offered rate, each transaction
due at its scheduled time; one reader thread runs a query every 10 ms (strata: TOP_10 over
300 queries drawn from the sample's vocabulary, a third terms, a third conjunctions, a third
disjunctions, over an owned snapshot the writer publishes; RocksDB: a `Get` of a written
key); and a line per window of 5 s reports:

- **sojourn** (`w_`): completion minus the transaction's due time, the client's view, so a
  writer stalled for 400 ms shows every transaction due meanwhile at 400 ms and more;
- **service** (`s_`): completion minus arrival at the engine, the engine's own blocking,
  admission delay included for the controllers;
- **query** (`q_`): the query's own time; the reader never waits for the writer. A TOP_10
  search and a point get are not the same work, so the read columns compare each engine
  with itself over time (how its debt taxes its readers), never strata with RocksDB;
- the debt: strata's segments, the policy's allowance (`allowed_segments`) and the excess
  over it, merges pending, merged MB (the rate limiter's admitted bytes), the flush time on
  the writer thread; RocksDB's L0 files, files in all, estimated pending compaction MB,
  stall ms (`STALL_MICROS`), compaction and flush MB, the delayed write rate.

Both engines run one background thread for merges or compactions (RocksDB also gets its
flush thread, since strata's flush is on the writer's thread), no log (RocksDB
`disableWAL`, strata commits once at the end), RocksDB with lz4 and a 10-bit Bloom filter,
an 8 MB memtable so that it, like strata's one-second refresh, writes a file every second
or two, and strata with a 2 GB budget that never forces a flush. The windows are 5 s so a
window holds 2,500 transactions and 500 queries: p99 and p999 are read from 25 and 2 to 3
samples at the top, so p999 per window is indicative and the run's p999 is the number.

**The budget.** Overload is set by construction: each engine may merge 1.5 bytes per byte
it flushes (strata 3 MB/s on 2.1 MB/s flushed; RocksDB, whose limiter also counts flushes,
9 MB/s on 3.5 MB/s flushed with lz4, so its compaction gets about 5.5 MB/s). Against each
engine's own unthrottled demand (4.1) that is 55% for strata, 67% for universal RocksDB, 38%
for leveled: the same rule squeezes leveled compaction harder because its own write
amplification is higher, which is reported as such, not hidden.

### 4.1 Today, unthrottled: the calibration

5,000 documents/s, 120 s, no merge limit, one run each (`cal-*.tsv`). Every 20 s:

| strata | admitted/5 s | sojourn p99 | service p50 / p99 / p999 / max | query p50 / p99 | segments | merged MB | flush ms per 5 s | CPU s per 5 s |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 to 5 s | 25,090 | 407 ms | 0.1 / 3.8 / 15 / 22 ms | 0.1 / 0.3 ms | 5 | 0 | 2,031 | 3.0 |
| 20 to 25 s | 25,950 | 468 ms | 0.1 / 1.6 / 37 / 180 ms | 0.3 / 1.8 ms | 11 | 55 | 2,273 | 4.2 |
| 40 to 45 s | 26,540 | 528 ms | 0.1 / 8.5 / 17 / 253 ms | 0.4 / 1.3 ms | 16 | 149 | 2,010 | 5.6 |
| 60 to 65 s | 25,790 | 421 ms | 0.1 / 9.0 / 30 / 163 ms | 0.4 / 1.6 ms | 11 | 281 | 2,008 | 5.4 |
| 80 to 85 s | 26,380 | 487 ms | 0.2 / 8.9 / 34 / 218 ms | 0.5 / 2.2 ms | 12 | 390 | 2,248 | 5.7 |
| 100 to 105 s | 25,090 | 406 ms | 0.1 / 5.1 / 16 / 18 ms | 0.7 / 2.1 ms | 27 | 473 | 1,950 | 5.8 |
| end | 600,010 documents, 9 segments, 248 MB on disk, 676 MB merged (19 merges), drain and commit 9.4 s | | | | | | | |

| RocksDB, no compression | admitted/5 s | sojourn p99 | service p50 / p99 / p999 / max | get p50 / p99 | L0 files | compacted MB | flushed MB | CPU s per 5 s |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| leveled, 20 to 25 s | 25,060 | 9.5 ms | 0.05 / 0.3 / 7.0 / 7.7 ms | 0.1 / 0.3 ms | 0 | 299 | 127 | 4.1 |
| leveled, 60 to 65 s | 25,020 | 4.0 ms | 0.05 / 0.4 / 2.1 / 3.0 ms | 0.1 / 0.9 ms | 0 | 1,373 | 345 | 5.0 |
| leveled, 100 to 105 s | 25,040 | 3.7 ms | 0.05 / 0.2 / 1.1 / 7.6 ms | 0.1 / 1.2 ms | 0 | 2,729 | 554 | 7.0 |
| leveled, end | 600,010 documents, 642 MB on disk, 3,452 MB compacted, 642 MB flushed, 0 s stalled | | | | | | | |
| universal, 60 to 65 s | 25,040 | 9.8 ms | 0.05 / 0.3 / 8.8 / 9.1 ms | 0.1 / 0.3 ms | 0 | 951 | 345 | 3.9 |
| universal, end | 599,980 documents, 642 MB on disk, 2,479 MB compacted, 642 MB flushed, 0 s stalled | | | | | | | |

With lz4 (60 s runs): RocksDB flushes 3.47 MB/s and compacts 14.5 MB/s leveled (4.2× its
flushes) and 8.2 MB/s universal (2.35×).

What the calibration says, before any idea:

1. **strata's stall today is the flush, not the merge.** The writer's sojourn p99 is 400 to
   530 ms in every window because the one-second refresh flushes 5,000 documents in about
   400 ms on the writer thread (80 µs per document at this flush size; `flush_ms` is 2.0 to
   2.3 s per 5 s, a 40% duty cycle). Every transaction due during a flush waits for it.
   ARCHITECTURE's write path already says "flush (background)" and phase-3 D11/D15 freeze a
   buffer for a background flush; as built, `refresh()` flushes inline. Nothing merge-side
   matters until this moves: it is ten times the next stall.
2. **Service p999 of 15 to 37 ms and maxima of 160 to 250 ms inside `apply`** at a 0.1 ms
   median: not merges (the merge thread never blocks the writer) and not flushes (the
   explicit refresh is outside `apply`). The sizes fit two things the writer does on its own
   thread: the tuple buffer's batch drain (`tuple_buffer::drain` groups, encodes, and sorts
   a batch of 2^20 tuples, about 5,700 documents here, whenever one fills between
   refreshes: tens of ms) and the version map's growth (a rehash of hundreds of thousands
   of keys: 100 to 200 ms). Both are the inverter owner's to attribute and bound (a
   reserved or incrementally grown map; a smaller batch), flagged here, since under the
   design they would be stalls of the owner core.
3. **Merge demand is 2.6 bytes per flushed byte after two minutes** (676 MB merged for
   258 MB flushed), 5.6 MB/s at 60% of one core (CPU per 5 s minus the writer's 2.3 s of
   flush and 0.25 s of apply and the reader's 0.2 s), so about 48 µs of CPU per document
   per rewrite and 125 µs per document ingested against the writer's 90. RocksDB leveled
   compacts 5.4 bytes per flushed byte (3.9 universal) at 0.6 to 1.4 cores, uncompressed.
4. **Queries pay about 30 µs per segment at the median** on this sample (5 → 27 segments:
   0.1 → 0.7 ms; p99 0.3 → 2.1 ms); RocksDB's gets stay at 0.1 ms since its L0 never
   grows here.
5. **A merge yields to the executor every output chunk of 1 MiB**, 1,078 checkpoints in 19
   merges: 428 gaps under 1 ms, 130 of 1 to 2 ms, then 25 to 30 in each bucket from 4 to
   64 ms, 58 of 64 to 128 ms, 96 of 128 to 256, 159 of 256 to 512, 32 of 512 to 1,024, 2
   over 1 s (max 1.67 s). Weighted by time, about 95% of merge time sits in gaps of 64 ms
   or more: a merge today can be paused or cancelled at a boundary within about 300 ms at
   the median and 1.7 s at worst, and a reordering merge (not run here: natural merges do
   not reorder) has no checkpoint in its bisection at all.

Section 5 states each idea the configurations below prototype (I1 to I5) with the assumption it drops.

### 4.2 Under the budget: strata today, I1, I2, and RocksDB (three rounds, medians)

5,000 documents/s offered for 180 s, the budget of 4.0 (strata 3 MB/s of merge writes,
RocksDB 9 MB/s of flush and compaction writes), six configurations per round in rotated
order, three rounds (`core.sh`; raw: `runs/core/*-r{1,2,3}.tsv`). Each cell is the median
over the rounds of that window's value; "admitted" is documents in the 5 s window; times in
ms. RocksDB's rows repeat to within 0.2 s of stall across rounds: with the limiter set, its
behavior is deterministic.

**strata today** (`none`: the policy's merges on the pool, no control):

| window | admitted | service p50 / p99 / p999 / max | query p50 / p99 | segments | merges queued | merged MB |
|---|---:|---:|---:|---:|---:|---:|
| 0 to 5 s | 25,850 | 0.1 / 0.4 / 1.0 / 1.2 | 0.1 / 0.2 | 5 | 0 | 0 |
| 40 to 45 s | 25,030 | 0.1 / 0.4 / 1.3 / 1.8 | 0.5 / 1.1 | 24 | 2 | 85 |
| 80 to 85 s | 25,030 | 0.1 / 0.3 / 2.1 / 42 | 0.5 / 1.2 | 50 | 6 | 185 |
| 120 to 125 s | 25,030 | 0.1 / 0.5 / 1.8 / 48 | 0.8 / 1.9 | 64 | 8 | 285 |
| 160 to 165 s | 25,020 | 0.2 / 0.6 / 2.0 / 60 | 1.5 / 3.6 | 90 | 12 | 384 |
| end | 900,010 admitted (all); 18 to 19 segments after a drain of 150 to 167 s; 793 to 806 MB merged in all; 27 to 28 merges | | | | | |

**I1, proportional admission** (`proportional`, low 12, high 28, floor 5%):

| window | admitted | service p50 / p99 / p999 / max | query p50 / p99 | segments | queued | merged MB | cap (docs/s) |
|---|---:|---:|---:|---:|---:|---:|---:|
| 0 to 5 s | 25,180 | 0.1 / 0.4 / 1.1 / 2.0 | 0.1 / 0.2 | 5 | 0 | 0 | 5,000 |
| 40 to 45 s | 17,070 | 2.4 / 3.7 / 5.4 / 6.6 | 0.6 / 1.2 | 24 | 2 | 86 | 3,750 |
| 80 to 85 s | 1,140 | 45 / 48 / 48 / 48 | 0.4 / 1.0 | 50 | 6 | 185 | 250 |
| 120 to 125 s | 4,370 | 7.9 / 38 / 40 / 40 | 0.5 / 0.9 | 37 | 5 | 280 | 250 |
| 160 to 165 s | 2,190 | 16 / 48 / 48 / 48 | 0.8 / 3.0 | 38 | 5 | 373 | 250 |
| end | 464,300 to 540,330 admitted (52 to 60%); 10 to 13 segments after a drain of 42 to 96 s; 450 to 656 MB merged | | | | | | |

**I2, the debt-adaptive width** (`adaptive`: 8 at 16 segments to 30 at 64):

| window | admitted | service p50 / p99 / p999 / max | query p50 / p99 | segments | queued | merged MB | width |
|---|---:|---:|---:|---:|---:|---:|---:|
| 0 to 5 s | 25,370 | 0.1 / 0.6 / 1.2 / 2.5 | 0.1 / 0.2 | 5 | 0 | 0 | 8 |
| 40 to 45 s | 25,020 | 0.1 / 0.4 / 1.7 / 2.9 | 0.4 / 1.1 | 24 | 2 | 85 | 12 |
| 80 to 85 s | 25,010 | 0.1 / 0.4 / 1.8 / 49 | 0.9 / 1.6 | 48 | 2 | 179 | 23 |
| 120 to 125 s | 25,030 | 0.1 / 0.6 / 1.5 / 42 | 0.8 / 1.5 | 57 | 2 | 274 | 27 |
| 160 to 165 s | 25,030 | 0.1 / 0.6 / 1.8 / 48 | 1.0 / 2.7 | 78 | 2 | 368 | 30 |
| end | 900,010 admitted (all); 18 to 28 segments after a drain of 49 to 75 s; 537 to 564 MB merged in all; 11 merges | | | | | | |

**RocksDB leveled** (its defaults: slowdown at 20 L0 files, stop at 36):

| window | admitted | service p50 / p99 / p999 / max | get p50 / p99 | L0 files | pending MB | stall ms in the window | compacted MB |
|---|---:|---:|---:|---:|---:|---:|---:|
| 0 to 5 s | 25,030 | < 0.1 / 0.1 / 0.2 / 0.5 | < 0.1 / 0.2 | 2 | 0 | 0 | 0 |
| 40 to 45 s | 25,020 | < 0.1 / 0.1 / 0.1 / 0.1 | 0.1 / 0.2 | 18 | 87 | 0 | 184 |
| 60 to 65 s | 13,350 | 3.1 / 13 / 27 / 32 | 0.1 / 0.2 | 23 | 112 | 4,960 | 290 |
| 80 to 85 s | 14,880 | < 0.1 / 19 / 1,274 / 1,281 | 0.1 / 0.2 | 21 | 102 | 4,585 | 406 |
| 120 to 125 s | 2,330 | 19 / 63 / 112 / 112 | 0.1 / 0.2 | 22 | 107 | 5,046 | 684 |
| 160 to 165 s | 25,460 | < 0.1 / 17 / 174 / 1,363 | 0.1 / 0.2 | 16 | 78 | 5,158 | 945 |
| end | 613,710 to 621,000 admitted (68 to 69%); stalled 120.4 to 120.6 s of 180 in every round; 1,188 to 1,192 MB compacted | | | | | | |

**RocksDB universal** (tiered, its closest shape to strata's policy):

| window | admitted | service p50 / p99 / p999 / max | get p50 / p99 | L0 files | stall ms | compacted MB |
|---|---:|---:|---:|---:|---:|---:|
| 0 to 5 s | 25,030 | < 0.1 / 0.1 / 0.1 / 0.1 | < 0.1 / 0.1 | 2 | 0 | 0 |
| 40 to 45 s | 25,020 | < 0.1 / 0.1 / 0.1 / 0.1 | 0.1 / 0.2 | 4 | 0 | 183 |
| 80 to 85 s | 25,020 | < 0.1 / 0.1 / 0.1 / 0.1 | 0.1 / 0.2 | 26 | 0 | 381 |
| 120 to 125 s | 25,030 | < 0.1 / 0.1 / 0.1 / 0.1 | 0.1 / 0.2 | 8 | 0 | 578 |
| 160 to 165 s | 5,770 | < 0.1 / 2.9 / 3.8 / 4.6 | 0.1 / 0.9 | 28 | 12,612 | 865 |
| end | 812,580 to 851,420 admitted (90 to 95%); stalled 19.2 to 38.6 s, one stop of 12.6 s at 160 s; 926 to 993 MB compacted | | | | | |

**RocksDB leveled with its triggers off** (`stalls=0`: debt unbounded, as strata today):

| window | admitted | service p50 / p99 / p999 / max | get p50 / p99 | L0 files | pending MB | compacted MB |
|---|---:|---:|---:|---:|---:|---:|
| 0 to 5 s | 25,040 | < 0.1 / 0.1 / 0.1 / 0.1 | < 0.1 / 0.1 | 2 | 0 | 0 |
| 40 to 45 s | 25,030 | < 0.1 / 0.1 / 0.1 / 0.1 | 0.1 / 0.2 | 18 | 87 | 184 |
| 80 to 85 s | 25,020 | < 0.1 / < 0.1 / 0.1 / 0.1 | 0.1 / 0.2 | 25 | 122 | 380 |
| 120 to 125 s | 25,020 | < 0.1 / 0.1 / 0.1 / 0.1 | 0.1 / 0.3 | 35 | 170 | 575 |
| 160 to 165 s | 25,030 | < 0.1 / 0.1 / 0.1 / 0.1 | 0.1 / 0.3 | 64 | 311 | 773 |
| end | 900,010 admitted (all); 53 to 55 files, 311 MB of pending compaction; 847 to 862 MB compacted | | | | | |

What the matrix says:

1. **The cliff, measured.** At a compaction budget of 38% of its demand, leveled RocksDB
   spends 120 of 180 s stalled: from 60 s on, 4.6 to 5.2 s of every 5 s window is stall
   time, single writes take 1.27 to 1.36 s, and admission alternates between about 2,300
   documents per 5 s and bursts of 25,000 to 30,000. Universal, at 67% of its demand, runs
   clean for 160 s and then stops for 12.6 s. The adaptive delayed rate (3.1) is visible as
   the 3 to 19 ms service medians between stops; it never prevents the stop, because the
   stop is a separate threshold.
2. **Unbounded debt costs the reader, and which reader decides how much.** RocksDB with
   its triggers off and strata today behave alike (nothing stalls, L0 → 64, segments → 90),
   but RocksDB's gets stay at 0.1 ms (a Bloom filter per L0 file makes a point get's probe
   nearly free) while strata's queries pay their 30 µs per segment: p50 0.1 → 1.5 ms, p99
   0.2 → 3.6 ms. And the debt is still owed at the end: 150 to 167 s to drain.
3. **I1 removes the cliff and keeps the queries** (p50 0.4 to 0.8 ms, p99 0.9 to 3.0 ms at
   24 to 50 segments instead of 90; service never above 48 ms, which is the admission
   spacing at the floor, 10 documents per 40 ms) but as prototyped it sat at its 5% floor
   for about 100 of 180 s and admitted 52 to 60%, when the budget sustains about 55% in
   steady state: the count-keyed ramp is a poor controller. Two reasons, both fixable: the
   policy's merges are run in posting order, so a big merge at the head of the queue keeps
   the count high for tens of seconds while the small retirements wait (4.3 measures
   cheapest-first), and a count of segments ignores how much each one costs to retire
   (the bytes-and-rate form of 4.3 and section 6).
4. **I2 cuts the merge bytes by 30%** (537 to 564 MB against 793 to 806 for the same end
   state) with 11 merges instead of 27 to 28 and a drain of 49 to 75 s instead of 150 to
   167; the segment count during the run is budget-bound either way (78 against 90 at
   160 s) and the queries alike. Lazy leveling's trade holds: width lowers write
   amplification under debt, and its cost to queries (more segments per tier) is invisible
   under a budget that cannot keep the count down anyway.
5. **A limiter finding.** strata's merges used 384 to 418 MB of a 566 MB budget (68 to
   74%): `rate_limiter::admit` sets `next_ = max(next_, now) + cost`, a bucket with no
   burst capacity, so the merge's compute time between two admissions is lost. RocksDB's
   limiter refills per 100 ms period and banks a period: 85 to 91% of its budget was used.
   For the performance engineer: bank one period, or credit the time since the last
   admission.

### 4.3 The pause pairs (I3) and the three extras round 1 asked for

**Pausing a merge at its checkpoints.** Three pairs of 120 s runs at 5,000 documents/s,
unthrottled, `pause_on_flush` off and on (the first pair on, then off-on-on-off):

| run | pause latency to a parked merge: n, p50, p99, max | flush ms per 5 s | sojourn p99 | merged in the run |
|---|---|---:|---:|---:|
| off (22:40) | | 1,266 | 270 ms | 718 MB |
| on (22:43) | 55, 69 ms, 225 ms, 225 ms | 946 | 194 ms | 680 MB |
| off-2 (00:29) | | 858 | 186 ms | 786 MB |
| on-2 (00:31) | 35, 35 ms, 168 ms, 168 ms | 672 | 133 ms | 858 MB |
| on-3 (00:34) | 21, 49 ms, 101 ms, 101 ms | 487 | 90 ms | 857 MB |
| off-3 (00:36) | | 461 | 84 ms | 857 MB |

A merge today parks at a checkpoint 35 to 69 ms after the request at the median and 101
to 225 ms at worst, consistent with the chunk spacing of 4.1 item 5 (a request lands
midway through a gap). The flush speed-up with merges paused is **not established**: the
host got faster through the night (the off runs' flush time fell 1,266 → 858 → 461 ms per
5 s, about 2.7×, with other agents' VM jobs ending) and in the two pairs where the on run
came second it was 22 to 25% faster, while in the one pair where the off run came second
the off run was 6% faster. No claim; the host drift is larger than any effect here.

**The extras** (three rounds in rotated order at 01:00 to 01:30, the same budget as 4.2;
the host was about 1.5× faster than during 4.2, so flush times do not compare across the
two matrices, while segment counts, which the merge budget drives, do):

I1's design form, `feedforward` (the cap from the merge bytes per second the host
delivered over the measured rewrites per flushed byte, at the measured bytes per document;
draining by at most half above the low mark):

| window | admitted | service p50 / p99 / p999 / max | query p50 / p99 | segments | merged MB | cap |
|---|---:|---:|---:|---:|---:|---:|
| 0 to 5 s | 25,010 | < 0.1 / 0.3 / 0.7 / 0.8 | < 0.1 / 0.2 | 4 | 0 | 5,000 |
| 40 to 45 s | 25,000 | 0.1 / 2.3 / 2.4 / 2.9 | 0.3 / 0.7 | 23 | 89 | 4,546 |
| 80 to 85 s | 11,670 | 4.1 / 4.5 / 5.2 / 5.3 | 0.5 / 1.0 | 49 | 189 | 2,442 |
| 120 to 125 s | 9,080 | 5.3 / 6.0 / 6.6 / 6.6 | 0.6 / 1.1 | 63 | 289 | 1,912 |
| 160 to 165 s | 7,280 | 6.5 / 7.9 / 8.3 / 8.3 | 0.7 / 1.4 | 71 | 390 | 1,545 |
| end | 498,980 to 505,490 admitted (56%); 14 to 19 segments after a drain of 60 to 67 s; 579 to 598 MB merged | | | | | |

`refresh` (the interval stretches one second per 8 segments of excess past the low mark):

| window | admitted | service p50 / p99 / p999 / max | query p50 / p99 | segments | merged MB | refresh | sojourn p99 | flush ms per 5 s |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 to 5 s | 25,010 | < 0.1 / 0.3 / 0.7 / 0.8 | < 0.1 / 0.2 | 4 | 0 | 1.0 s | 81 | 375 |
| 40 to 45 s | 25,000 | 0.1 / 0.3 / 1.3 / 1.7 | 0.3 / 0.7 | 22 | 89 | 1.25 s | 79 | 446 |
| 80 to 85 s | 25,000 | 0.1 / 0.3 / 2.5 / 50 | 0.4 / 0.9 | 31 | 189 | 2.25 s | 100 | 301 |
| 120 to 125 s | 25,000 | 0.1 / 0.2 / 37 / 44 | 0.4 / 0.8 | 32 | 291 | 2.25 s | 106 | 263 |
| 160 to 165 s | 25,000 | 0.1 / 0.2 / 41 / 50 | 0.6 / 1.2 | 42 | 392 | 3.5 s | 156 | 206 |
| end | 900,010 admitted (all); 11 segments after a drain of 159 to 163 s; 829 to 834 MB merged | | | | | | | |

`global` with one index (the host's scheduler: cheapest retirement first, four queued):

| window | admitted | service p50 / p99 / p999 / max | query p50 / p99 | segments | merged MB |
|---|---:|---:|---:|---:|---:|
| 0 to 5 s | 25,010 | < 0.1 / 0.3 / 0.7 / 0.8 | < 0.1 / 0.2 | 4 | 0 |
| 40 to 45 s | 25,000 | 0.1 / 0.2 / 1.3 / 1.7 | 0.3 / 0.7 | 23 | 88 |
| 80 to 85 s | 25,090 | 0.1 / 0.2 / 1.5 / 41 | 0.4 / 1.0 | 35 | 187 |
| 120 to 125 s | 25,000 | 0.1 / 0.2 / 1.7 / 41 | 0.6 / 1.4 | 47 | 286 |
| 160 to 165 s | 25,000 | 0.1 / 0.3 / 1.8 / 40 | 0.7 / 1.5 | 59 | 386 |
| end | 900,010 admitted (all); 54 to 61 segments left, drain 0.8 to 10.5 s; 423 to 448 MB merged in all, 17 to 18 merges | | | | | |

What the extras say:

1. **Feed-forward admission is smooth where the ramp was not**: service p99 never above
   8 ms and no floor lock-in (the ramp's 48 ms at 5%); the cap follows the measured
   throughput down to about 1,550 documents/s, which is this budget's sustainable rate
   (2.3 MB/s merged over 2.8 rewrites at 0.43 KB per document is about 1,900). It admitted
   56%, the same as the ramp, and the count still climbed to 71: a refresh a second makes
   a segment a second whatever is admitted, so admission alone cannot hold the count in a
   search engine. It is the right last resort, not the first.
2. **The refresh interval is the strongest single lever measured**: everything admitted,
   42 segments against today's 90, query p99 1.2 ms against 3.6, and 45% less flush CPU
   per document (fixed costs amortized over bigger flushes: 375 → 206 ms per 5 s at the
   same ingest). Its price is visibility latency (3.5 s instead of 1 at the end) and, with
   the flush still on the writer's thread, a longer stall per flush (sojourn p99 80 →
   156 ms); the service p999 of 37 to 41 ms is the tuple buffer's batch drain, which now
   fills between refreshes (40 ms per 2^20 tuples: 4.1 item 2 confirmed). Item 1 of
   section 6 removes the first; the inverter's owner bounds the second.
3. **Cheapest-first scheduling keeps 35% fewer segments for the same bytes** (59 against
   90 at 386 MB), query p99 1.5 ms against 3.6, and never builds a backlog: today's engine
   queues the policy's merges in posting order and drains 150 to 167 s of them at the
   end; the host chooses from the current state each tick and leaves the big merges for
   when the budget frees (54 to 61 segments left, 0.8 to 10.5 s to drain). SILK's "lower
   levels first" and the tiered policy's own window order, applied to the queue.

### 4.4 Many indexes (I4): today's round robin, the host's scheduler, and heat

32 indexes, 5,000 documents/s round robin, each index refreshed every 4 s staggered,
queries Zipf(1) over the indexes, 3 MB/s, 180 s, two rounds. The first attempt
(`runs/many-v1/`) is a harness artifact and is kept as such: the scheduler ticked every
4 s and counted a finished merge as in flight until its index's next refresh published it,
so `global` and `hot` posted 38 merges where today's round robin posted 160. With
`publish_finished()` and a 250 ms tick they post 76 to 90 and use the same budget bytes:

| configuration | admitted | service p99 | query p50 / p99 / p999 at 40 s, 100 s, 160 s | segments at 180 s (max per index) | merged in the run | end |
|---|---:|---:|---|---:|---:|---|
| today (`none`) | all | 0.2 to 0.4 ms | 0.1 / 0.8 / 1.5; 0.2 / 1.6 / 2.1; 0.3 / 1.1 / 1.9 ms | 886 (28 to 33) | 346 MB | 95 merges queued; 155 segments after a drain of 244 to 248 s |
| `global` | all | 0.2 to 0.4 ms | 0.1 / 0.3 / 1.8; 0.2 / 0.5 / 1.0; 0.2 / 3.5 / 5.3 ms | 682 (40) | 346 MB | 83 to 90 merges, none queued; 637 to 700 segments, drain 11 to 12 s |
| `hot` | all | 0.2 to 0.4 ms | 0.1 / 0.3 / 1.1; 0.1 / 0.6 / 0.9; 0.2 / 4.2 / 9.4 ms | 777 (32 to 36) | 351 MB | 76 to 77 merges; 759 to 769 segments, drain 11 s |

1. **The host's scheduler retires 23% more segments per byte than today's round robin**
   (682 against 886 for the same 346 MB) and has no backlog: today's engine ends with 95
   merges queued across the indexes and takes 244 to 248 s to drain them.
2. **`hot` is inconclusive as prototyped, and the reason is in its cap.** Through 100 s,
   while the heat ordering acted, its query p99 (0.3 to 0.6 ms) sat below today's (0.3 to
   1.6); from 120 s on it was worse (2.3 to 4.2 against 1.1 to 2.6). With uniform ingest
   every index reaches four times its allowance by about 120 s, the cap then outranks heat
   for every index at once, and the scheduler degenerates to smallest-bytes-first. The next
   experiment is a heat-weighted cap (a cold index's cap higher), a per-index breakdown of
   counts and query times, and skewed ingest (the hot indexes also write more, as on
   Xapiand's hosts). Two rounds and 500 queries per window also leave p99 noisy at this
   scale; the direction through 100 s is the result to carry, not a number.

## 5. The ideas

Each idea names the assumption it drops, the mechanism, and the prototype that measured it
(`research_ingest`'s `control=` and `pause_on_flush=` switches; the engine hooks are
`index::policy()`, `schedule_merge()`, and `take_snapshot()`). The matrix (4.2) runs every
single-index configuration under the budget of 4.0 at 5,000 documents/s for 180 s, three
rounds in rotated order; the many-index matrix (4.4) runs 32 indexes.

### I1. Proportional admission: a slope where RocksDB has steps

Dropped: "writes are either admitted at full rate or held" (RocksDB's slowdown and stop,
Lucene's `maybeStall`). The writer's admitted rate is a continuous function of the debt:
full up to `low` excess segments over the policy's allowance, falling linearly to a floor
(5% of the offered rate, never zero) at `high`, so throughput, sojourn, and service move
together and smoothly as the debt moves. The marks here are RocksDB's own, translated:
`low = 12` (20 segments with the allowance at 8) and `high = 28` (36). The controller runs
once per refresh and spaces transactions at the capped rate; the wait counts as service.
Its design form (section 6) is CockroachDB's: the cap set from the merge throughput the
host measured in the last interval, with the ramp as the backstop; this prototype measures
the ramp alone.

### I2. A debt-adaptive merge width: do less work per byte when behind

Dropped: "the merge policy's shape is a constant". Under a budget below demand the write
amplification is the lever a policy still holds (section 2.1): merging 30 segments at a
time rewrites a byte `log_30 / log_8 = 1.6×` less often than merging 8, at the price of up
to 30 segments per tier. The prototype moves `segments_per_tier` and `max_merge_at_once`
from 8 (Lucene's) to 30 (its forced-merge width) linearly as the raw segment count goes from
16 to 64, through `index::policy()`, and the engine's `maybe_merge` picks the policy up at
the next refresh. Dostoevsky's lazy leveling, as a dial driven by debt.

### I3. Merges that yield at block boundaries

Dropped: "a merge is one long task". Today a merge yields to the executor only when it
writes an output chunk (section 4.1 item 5), so the pool cannot preempt it within its
quantum, a cancel loses up to 1.7 s of work, and SILK-style preemption of a big merge for a
small one is impossible at useful granularity. The prototype (`pause_on_flush=1`) pauses
every merge at its next checkpoint while the writer flushes and resumes it after, through
a metered executor that also times the pause: the latency from the request to a merge
thread parked is what the slice is worth, and the flush's time with the merge paused
against not paused says whether the two contend on this host. The design form is a
checkpoint per posting block in the merge's plugins (one relaxed load per 128 postings,
about 60 µs of work apart at today's merge cost, 3 µs with the copy merge) and, past that,
the merge as a resumable state (section 6).

### I4. One merge budget across indexes, and the assumption breaker: charge the merge to the reader

Dropped: "every index keeps its own tier invariant, and merge debt is bytes". With tens of
thousands of indexes on one pool, today each index posts its policy's merges to its own
task group and the pool serves groups round robin: the budget is shared by count, not by
need. Two host-level schedulers replace that, with `auto_merge` off and the host running
every index's `find_merges` once per interval and submitting at most the pool's width plus
one:

- `global`: merges ranked by bytes written per segment retired, cheapest first, so the
  budget buys the most segment reduction per byte (the small tiers first, as SILK's L0
  priority and the tiered policy's own window order);
- `hot`: the same cost divided by the index's query rate (an EWMA of its queries per
  interval), so the budget goes where queries will pay for the debt, and an index past four
  times its allowance jumps the queue whatever its heat (the cap that keeps a cold index's
  debt bounded). LevelDB's seek compaction priced one file's compaction by the reads it
  saves; this prices every merge on the host that way. A cold index never merges, its
  segments cost no query anything, and a hot index gets the whole budget.

The many-index matrix: 32 indexes, 5,000 documents/s round robin (156 per index per
second), each index refreshed every 4 s staggered (so the writer's flush duty stays what it
is with one index), queries Zipf(1) over the indexes (the first gets 25%, the 32nd 0.8%),
the same 3 MB/s budget. Reported: query p50/p99 (dominated by the hot indexes), the
largest segment count of any index, the total, and the merged bytes.

### I5. Copy merges (not prototyped here)

Dropped: "a merge decodes and re-encodes every posting". `docs/research/indexing.md` 5.3
measured today's two-segment merge at 497 ns per posting and the copy merge's floor at
24.6 ns (a dictionary walk and a byte copy of whole blocks; X-Engine's extent reuse is the
published precedent), 20×, and listed the format 13 items it needs (a base ordinal per
term, tail-coded blocks anywhere, aligned positions). Section 2.1 puts it in the bound: it
moves one merge thread's capacity from about 9 MB/s to about 180 MB/s of segments on this
host, past what one writer can flush, so under it the budget question becomes I/O and the
host's fairness, not CPU. It is the largest single lever and it is format work, so it is
recorded for the format 13 designer and not re-measured.

## 5a. Kept for the record: what lost or did not resolve

| Idea | Result | Why |
|---|---|---|
| I1 as a count-keyed ramp with a floor | Locked at its 5% floor for 100 of 180 s; 52 to 60% admitted | A count ignores what each segment costs to retire; FIFO big merges keep the count high while small retirements wait |
| Pausing merges during flushes for the flush's sake | 22 to 25% faster flushes in two pairs, 6% slower in the third | The host's speed drifted 2.7× over the night; the effect is inside the drift |
| `hot` with a fixed cap at four times the allowance | Better through 100 s, worse after | The cap fires for every index at once under uniform ingest and overrides heat |
| The first many-index rounds | 38 merges against 160 | A harness artifact (tick and in-flight accounting), fixed and rerun |

## 6. Recommendation

For the designer and the performance engineer, in the order the measurements rank them:

1. **Flush off the owner thread.** The largest stall by ten times today: 80 µs per
   document on the writer's thread at one-second refreshes, sojourn p99 of 80 to 530 ms
   depending on the host's speed (4.1 item 1), nothing to do with merges. Phase-3 D15
   already says it: freeze the buffer at refresh, flush it on the pool's flush class, the
   owner continues into a fresh buffer, the budget holds two. It also removes the price of
   item 3's bigger flushes and the per-flush interference question of 4.3.
2. **The host schedules merges: cheapest retirement first, just in time, finished merges
   installed at once.** `auto_merge` off per index; a host loop on a 250 ms tick runs
   every open index's `find_merges`, ranks the candidates by bytes written per segment
   retired, submits the pool's width plus a few through `schedule_merge()`, and calls
   `publish_finished()`; the pool's merge class (scheduler.md R3) runs them. Measured:
   35% fewer segments for the same bytes with one index (4.3 item 3), 23% fewer across 32
   (4.4), and no backlog (0.8 to 12 s drains against 150 to 248). Heat weighting is the
   next experiment, with a heat-weighted cap (4.4 item 2); the mechanism is the same loop.
3. **Debt answers first with the refresh interval, last with admission, never with a
   stop.** The debt unit is ScyllaDB's backlog, bytes × tiers remaining, computed from
   the segment sizes (3.6), not a count. Past a low mark the refresh interval stretches
   (one second per 8 segments of excess in the prototype) up to a cap; past a high mark the
   admitted rate follows the merge throughput the host measured (feed-forward: merged
   bytes per second over the measured rewrites per flushed byte at the measured bytes per
   document), draining by at most half, with a floor and no stop. Measured: the refresh
   alone held 42 segments against 90 at full admission with query p99 1.2 against 3.6 ms
   and 45% less flush CPU (4.3 item 2); feed-forward admission kept service p99 under
   8 ms with no lock-in (4.3 item 1); RocksDB's thresholds cost it 120 of 180 s (4.2).
4. **A checkpoint per posting block in the merge.** A merge parks 35 to 69 ms (p50) to
   225 ms (max) after a pause request today, against the pool's 100 µs quantum and the
   10 ms merge slice the scheduler assumes (4.3). One `checkpoint()` per 128-posting block
   in the plugins' merge loops (a relaxed load about 60 µs of work apart today, 3 µs with
   the copy merge) and one per part in the bisection's gain loop; then a merge is pausable
   and cancellable within a slice, and SILK's preemption of a big merge by a small one is a
   scheduling rule (item 2) rather than a loss of work. The resumable merge, a state the
   pool can drop and re-post, is the step after, for when a paused merge's memory matters.
5. **Width under debt**, 8 to 30 as the backlog grows: 30% fewer merge bytes for the same
   end state and no measured query cost under a budget that cannot keep the count down
   (4.2 item 4). A preset of the policy; measure it again once items 2 and 3 exist.
6. **Copy merges** (I5, for the format 13 designer): 20× on the merge's constant, the one
   lever that moves the bound itself (2.1); after it the budget question is I/O and the
   pool's fairness, not CPU.
7. **The limiter banks a period.** `rate_limiter` loses the compute time between two
   admissions (68 to 74% of the budget used; RocksDB's, which refills per 100 ms, 85 to
   91%; 4.2 item 5): credit the time since the last admission, up to one period.

Not to build: a count-keyed ramp with a floor; RocksDB's thresholds in any form; a
"pause merges during flushes" rule (no gain beyond the host's drift).

Limits these make, each load-bearing: the refresh interval's cap (visibility latency; the
prototype's 16 s is below Elasticsearch's 30 s search-idle refresh, so 30 s is the number
to adopt and name); the admission floor (never zero; the measured rate's floor, not a
fraction); the per-index segment cap past which merges outrank heat (four times the
allowance in the prototype; the design weights it by heat, and it is also what bounds a
cold index's file count and pinned memory).

## 7. Findings for others

- **For the inverter's owner.** `tuple_buffer::drain` costs about 40 ms per 2^20-tuple
  batch on the writer's thread whenever a batch fills between refreshes (4.3 item 2's
  p999); `apply` maxima of 160 to 250 ms at 100,000 to 600,000 keys fit the version map's
  growth (4.1 item 2, a hypothesis to attribute). Both are owner-core stalls under the
  design.
- **For the performance engineer.** The limiter's bucket (item 7). The index-size note's
  IS-2 (a merge waiting up to 5 s on a budget it can never fit) stands: the harness set
  `merge_memory_wait = 0`.
- **For the format 13 designer.** Copy merges (I5) and the per-block checkpoint (item 4)
  touch the same merge loops; design them together.
- **For threadpool's scheduler.** The merge slice the pool can assume today is 64 to
  512 ms between yields (4.1 item 5), not 10 ms, until item 4 lands; its R3 reservation is
  what protects queries meanwhile.
- **For the lead.** The host's speed drifted 2.7× over the night with other agents' VM
  jobs; every A/B here is within a rotated round and absolute numbers are not comparable
  across matrices. My pipelines ran `quiet run` without `quiet wait` until 00:27 and one
  pause pair overlapped a strict hold; the rest ran behind `quiet wait --light`.

## 8. Reproducing

```sh
cmake -B .scratch/build-rel -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS=-march=native \
  -DSTRATA_RESEARCH=ON -DSTRATA_ROCKSDB_SRC=.scratch/rocksdb-src -DSTRATA_ROCKSDB_BUILD=.scratch/rocksdb-build
ninja -C .scratch/build-rel research_ingest research_rocks
B=.scratch/build-rel/research/no-stalls; C=.scratch/corpus-100k.json   # the gate corpus's first 100,000 lines
LIMIT_MB=3 ROCKS_LIMIT_MB=9 research/no-stalls/core.sh $B $C .scratch/runs/core 1 180   # one round; rounds 2, 3 likewise
LIMIT_MB=3 research/no-stalls/many.sh $B $C .scratch/runs/many 1 180
python3 -I research/no-stalls/summarize.py --every 4 .scratch/runs/core/*.tsv
```

RocksDB v11.8.1 builds with `-DWITH_GFLAGS=ON -DWITH_ZSTD=ON -DWITH_LZ4=ON -DWITH_TESTS=OFF
-DWITH_BENCHMARK_TOOLS=ON -DROCKSDB_BUILD_SHARED=OFF -DPORTABLE=0` (its `db_bench` is built
beside `librocksdb.a` as the survey's sanity check; the driver is `research_rocks`). Every
run goes behind `quiet wait --light` and a `quiet run` hold.
