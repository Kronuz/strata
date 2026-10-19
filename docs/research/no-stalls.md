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
   sample costs about 50 to 70 µs per extra segment at the median (10 segments: 0.19 ms; 20:
   0.92 ms; 40: 2.5 ms), so a hundred small segments are 5 to 7 ms on every query.
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
