# Fairness audit of the key-value harness against RocksDB

Status: done, 2026-10-20 (macOS host only). Audits `core-vs-rocksdb.md` (main `b8a2cb6`): for
each row, what RocksDB 11.11.1 actually did, from its own statistics, its listener events, a
barrier count, and its source (read under Apache 2.0). Companion:
`docs/research/rocksdb-mechanisms.md`, the clean-room spec of the mechanisms observed here.

## Findings

| # | Finding | Rows | Effect on the reported ratio | Fix |
|---|---|---|---|---|
| A1 | RocksDB's load of a sync row went into the run's first sync batch | `sync1` YCSB A, F | 4.74× and 4.93× slower become **4.99× and 5.02× slower** (strata's loss grows 5%) | `on_phase` hook (531c113) |
| A2 | strata's settle commits twice when no merge ran; the second commit writes a manifest (three barriers) | every "to durable" column; many-index settle | fits fills to durable 1.17–1.23× faster for strata (A/B); many-index settle 150 s → 87 s | settle commits again only for pending merges (this commit) |
| A3 | The many-index row flushed 1,000 column families as 1,000 jobs (4,000 barriers); RocksDB's atomic flush does it in one (1,003) | `many` | settle 1.72× slower becomes **about 3.97× slower** with A2 and A3 both applied | `-atomic` for any multi-family row (this commit) |
| A4 | At 2×, RocksDB's fill rows are bound by its flush barriers: writers stopped 0.66–0.73 s of a 1.05 s run | `twox1` fills | strata's 5.4× and 2.4× "faster" are macOS barrier latency: with plain `fsync` barriers RocksDB is **1.51× and 3.69× faster than strata** | none at equal memory; quote with this attribution, decide on Linux |
| A5 | Index and filter blocks in the cache cost RocksDB two cache lookups per get below L0 | YCSB B–F reads | YCSB C 7.2% [4.4, 12.9] slower than with them in the table reader; `readrandom` tie | kept: it is the equal-memory rule; stated |
| A6 | No other inequality: background work, sync paths, `none`, read sources check out | all | none | none |

Every other ratio in `core-vs-rocksdb.md` stands as reported.

## Setup

- Host and builds as `core-vs-rocksdb.md`. RocksDB's audit driver: this branch's
  `kv_rocksdb.cc` against the same `librocksdb.a` (`ec93377878cd`), built without RTTI.
  strata's timed reruns use the reported binary (main `68e3d29` plus the harness); the settle
  A/B (A2) builds both sides from this branch (main `76e16d0`), differing only in the settle.
- **Counted runs** (one trial per row, `KV_ROCKSDB_STATS=1`): RocksDB's statistics, flush and
  compaction events, and stall transitions per phase, split at the phase boundaries; every
  `F_FULLFSYNC` and `fsync` counted per file by `bench/kv/barriers.c` and assigned to its phase
  by `KV_PHASE_MARKS`. Statistics cost RocksDB about 20% on fills, so counted runs give counts,
  not rates.
- **Timed reruns** (statistics off, interleaved, under light holds): A1 three trials per
  engine; A4 three per side; A5 five per side; A3 three per variant; A2 five per side.
- Ratios are strata over RocksDB unless stated, medians with a 95% bootstrap interval.

## A1: a sync row's load landed inside the timed run

`kv_rocksdb`'s `put` chose between a direct put and the per-thread `WriteBatch` by the
**row's** durability; the driver runs every load at durability none but the engine could not
see that. In the two sync rows with a load (YCSB A and F at batch 1) the 100,000 load records
accumulated in the batch, the load settle flushed an empty DB, and the run's first update wrote
all of them in one 105 MB WAL record with one barrier.

Evidence (the reported binary under the counters, against the fixed one, YCSB A):

| Phase | Reported driver: seconds, bytes written | Fixed driver |
|---|---|---|
| load | 0.143 s, 0 B (batch appends only) | 1.299 s, 156 MB (3 flushes) |
| load settle | 0.041 s, 0 B | 0.411 s, 72 MB (L0 → L6 compaction) |
| run | 15.42 s, **169 MB** | 15.18 s, 3.7 MB |

The reported raw data shows the same signature in every RocksDB sync YCSB trial: load 0.15–0.17 s
against strata's 0.82–1.13 s, and 61 MB of tables with 0.37 MB of estimated live data.

Rerun (three trials each, interleaved):

| Row | strata ops/s | RocksDB ops/s | Reported | Rerun [95% CI] |
|---|---:|---:|---|---|
| YCSB A, sync per op | 19.5 | 97.4 | 4.74× slower | **4.99× slower** [0.200, 0.200] |
| YCSB F, sync per op | 19.4 | 97.3 | 4.93× slower | **5.02× slower** [0.197, 0.200] |

Fix: `kvdriver.hh` tells the engine each phase's durability through an optional `on_phase`,
and `kv_rocksdb` batches only in a sync phase (531c113). strata's driver was not affected.

## A2: strata's settle paid for a commit with nothing to commit

The harness's strata settle ran `commit(); wait_merges(); commit();`. A commit writes a new
manifest even when nothing changed, so when no merge was pending the second commit cost a
manifest and two directory barriers. Counted: the fits `fillrandom` settle issued 7
`F_FULLFSYNC` (4 for the commit, 3 for the empty one); the many-index settle 7,000 for 1,000
indexes (2,000 manifests).

The fix commits a second time only when the first left merges pending. A/B on this branch, five
trials each, interleaved:

| Row (fits) | Settle, two commits → one | To durable, one over two [95% CI] |
|---|---|---|
| `fillrandom` | 0.283 → 0.229 s | **1.17× faster** [1.10, 1.28] |
| `overwrite` | 0.359 → 0.268 s | **1.23× faster** [1.21, 1.33] |
| YCSB A | 0.497 → 0.395 s (merges ran, so both commits stay) | tie [0.80, 1.13] |
| many-index (counted, one trial) | 150.2 → 87.3 s; barriers 7,000 → 4,000 | 1.72× faster |

Applied to the reported medians (an estimate across sessions, not a rerun of those rows), the
fits "to durable" losses move from 1.30× to about 1.11× slower (`fillrandom`) and from 1.80× to
about 1.47× slower (`overwrite`). This finding flattered RocksDB.

## A3: the many-index settle under-configured RocksDB

`Flush` over 1,000 column families without `atomic_flush` runs 1,000 flush jobs, each syncing
its table, its directory twice, and the MANIFEST. With `atomic_flush` one job writes all 1,000
tables, syncs each, then syncs the directory and the MANIFEST once. Counted: 4,000 barriers
(85.0 s) against 1,003 (22.3 s). Timed, three trials each:

| RocksDB, 1,000 column families | Default | Atomic flush |
|---|---:|---:|
| Settle the load | 84.7 s | **22.0 s** (3.86× faster) |
| Create (1,001 MANIFEST syncs, one per family, either way) | 21.9 s | 22.4 s |
| Reopen | 0.96 s | 1.01 s |
| `readrandom` | 304,373 ops/s | 264,182 ops/s (tie: trials spread 240k–386k) |
| Peak RSS | 140 MiB | 165 MiB (18% more) |

With A2 and A3 applied, strata's settle of 1,000 indexes (87.3 s) is **about 3.97× slower**
than RocksDB's (22.0 s), not 1.72×; strata's peak-memory lead grows from 49% to about 56% less.
strata's create (85–88 s, four barriers per index) against RocksDB's (22 s, one MANIFEST sync per
family) is unchanged at 3.85× slower; RocksDB offers no batched family creation, so that row is
equal. Fix: `run.py` adds `-atomic` to RocksDB's variant for any row with more than one index.
This finding flattered strata.

## A4: the 2× fill rows measure macOS barriers on RocksDB's flushes

At 2× (M = 5 MiB) RocksDB's memtables are 1 MiB, two slots, under a 1.25 MiB write buffer
manager. Every RocksDB flush is durable (table, directory, MANIFEST: three `F_FULLFSYNC`, about
65 ms here), even with the WAL disabled. Counted on `fillrandom`: 13 flushes and 2 L0 → L6
compactions inside the 1.05 s run, 45 barriers, and **0.69 s of write stops** (15
memtable-limit stops). strata's flushes are not durable until its commit, so its run issues no
barriers and its cost moves to the settle (15.3 s of merge waits, loss 3 of the report).

Attribution: the same runs with `BARRIERS_WEAK=1`, which turns each `F_FULLFSYNC` into a plain
`fsync` (no drive-cache flush; not durable, never a row). Three trials each, interleaved:

| Row (2×) | strata | RocksDB | RocksDB, weak barriers | strata/RocksDB | strata/RocksDB weak |
|---|---:|---:|---:|---|---|
| `fillrandom` run | 531,798 | 97,151 | 802,025 | 5.47× faster [5.39, 5.59] | **1.51× slower** [0.645, 0.693] |
| `overwrite` run | 235,689 | 92,250 | 870,895 | 2.55× faster [2.52, 2.60] | **3.69× slower** [0.268, 0.273] |
| `fillrandom` to durable | 6,440 | 81,393 | 477,546 | 12.6× slower | 74× slower |

With weak barriers RocksDB records no stops at all. So the reported 2× fill wins are RocksDB
waiting on 20 ms barriers, an effect of this host's `F_FULLFSYNC`, which costs one to two orders
of magnitude less on Linux NVMe. The "to durable" column already shows the honest direction
(13–16× slower). No equal-memory setting removes it: more memtable slots would exceed M/4, and
the steady state stays bound by one flush job's barriers. The fix is in the reading: quote the
2× fill throughput only with this attribution, and let the Linux gate tier decide those rows.

## A5: index and filter blocks in the cache

The harness puts index and filter blocks in the block cache so the one budget M bounds them (the
survey's equal-memory rule), pinning only L0's. Below L0 each get then pays two extra cache
lookups. Five trials each, interleaved:

| Row (fits) | Table reader over cache [95% CI] | Peak RSS, cache / table reader |
|---|---|---|
| `readrandom` (data in one L0 table, pinned) | tie, 0.995 [0.94, 1.10] | 38.5 / 38.5 MiB |
| YCSB C (data in L6) | **1.072× faster** [1.044, 1.129] | 205 / 210 MiB |

Kept, because it is what makes the budgets equal; at this tier 7% is inside the 10% band, and
applying it to YCSB C moves 25.9× slower to about 27.8×. Stated so the gate tier can choose.

## A6: what RocksDB did, row by row (no other inequality)

### Background work during the timed phase

| Rows | Flushes in run | Compactions in run | Stops | Barriers run / settle |
|---|---|---|---|---|
| fits fills (100 B) | 0 (the 32 MiB memtable holds the 12.6 MB) | 0 | 0 | 0 / 3 |
| fits reads (`readrandom`, `readmissing`, `readseq`, `seekrandom`) | 0 | 0 | 0 | 0 / 0 |
| fits YCSB A, F | 1 (12 MB, background thread) | 0 | 0 | 3 / 3 |
| fits YCSB B–E | 0 | 0 | 0 | 0 / 0–3 |
| fits loads at 1 KiB (not timed) | 2 | L0 → L6 once, in the load settle | 2–3 memtable stops, 40–64 ms | — |
| 2× fills | 13 | 2–3 L0 → L6 | 15 memtable stops, 0.66–0.73 s | 43–48 / 6–9 |
| 2× reads | 0 | 0 | 0 | 0 |
| 2× YCSB A, F | 8 | 0–1 L0 → L6 | ≤ 139 µs | 24–27 / 6–9 |
| `fits4` (shared DB) | 0 | 0 | 0 | — |

No L0 slowdown or stop fired in any row (L0 never passed four files); compactions ran from L0
straight into L6, as dynamic level sizing does for a small DB. At fits the fill rows are pure
memtable inserts and the flush lands in the settle, as strata's does: fair. In `fits4`, four
writers form groups of about two (50,028 writes done by their own thread, 49,972 by a leader).

### Durability paths

- **`sync=true` does issue `F_FULLFSYNC` per write group.** Traced: the group leader writes the
  merged batch to the WAL, then syncs the WAL file; the POSIX file's sync is
  `fcntl(F_FULLFSYNC)` when `HAVE_FULLFSYNC` is defined, which this build defines; a new WAL's
  directory is synced once at its first sync. Counted on `sync1` `fillrandom` per 100: 709
  batches, 709 WAL barriers (20.8 ms each, 98% of the run), one WAL directory sync.
- **Group commit merges concurrent syncs.** One thread never groups (709 groups for 709 writes).
  Four threads, sync per op: 2,048 writes in 741 groups, 741 barriers (2.76 writes per barrier),
  130 ops/s against 47 at one thread. The harness has no multi-writer durable row; until strata's
  turn log (K2) groups commits, such a row would widen its loss about 2.8×.
- **strata's commit is five barriers.** Counted at sync per op: 958 `F_FULLFSYNC` for 192
  commits (4.99 each: the segment, the directory, the manifest, the directory again, and a third
  directory sync on most), each preceded by a plain `fsync`. Five against one is the 4.7–5.2×.
- **`none` makes everything durable at the settle.** With the WAL disabled a put lives only in a
  memtable; the settle's flush syncs the table, the directory, and the MANIFEST before installing
  it, and each compaction does the same for its outputs. After the settle the whole run is
  durable and named by a synced MANIFEST, the same contract as strata's single commit: the "to
  durable" columns compare like with like (3 barriers against strata's 4 after A2).
- **WAL tier (`wal1`).** 100,000 `write()` calls, no barrier until the settle; 128k ops/s against
  484k with the WAL disabled under statistics.

### Where reads were served from

Device bytes read were zero in every row: every block cache miss was a `pread` served by the OS
page cache, decompressed (LZ4) and checksummed.

| Row | Block cache data hits | Memtable | Tables |
|---|---|---|---|
| fits `readrandom` | 99.3% (747 misses: each block's first touch) | 0% | 100% one L0 table |
| fits `readmissing` | 991 false positives (0.99%) reached a block | — | 99,009 stopped at the filter |
| fits `readseq` | 0% (747 blocks read once) | — | one L0 table |
| fits `seekrandom` | 93.0% | — | one L0 table |
| fits YCSB A / B / C / D / E / F | 76 / 91 / 93 / 80 / 98 / 87% | 40 / 23 / 0 / 66 / 0 / 38% of gets | rest from L6 (YCSB A, F: about 2–3% from L0) |
| 2× `readrandom` | 39.1% | 0% | 16% L0, 84% L6 |
| 2× YCSB A–F | 35–61% | same shares as fits | — |

The YCSB runs start with a cold cache (compaction outputs are not cached), as strata starts with
no frame cache; both read file pages from the page cache. The 2× rows therefore bound in-process
memory only, as the report says; they are not I/O rows.

### Other settings checked and found equal or stated

- Background threads: two for each (RocksDB: one flush slot, one compaction slot).
- Memory: the same M/4 for buffers; RocksDB's cache M holds decoded blocks, strata has none and
  reads through mmap; peak RSS counts strata's mapped pages and RocksDB's cache, not either's
  page cache.
- Compression: LZ4 at 16 KiB blocks (RocksDB's calibrated fastest) against strata's zstd at
  64 KiB frames, its only setting; the disk axis compares codecs as much as layouts.
- Bloom at 10 bits per key (0.99% false positives measured) against strata's fuse filter
  (0.39%): each engine's default.
- strata-side harness costs, both small: the test-hook get decodes without the scan's per-segment
  frame memo, and scan rows keep a sorted set of buffered keys (charged to loads and to YCSB E's
  5% inserts).
- The reported rows ran without RocksDB statistics (the reported driver created none).

## The four axes

- **Time:** as the findings table. Net direction changes: the 2× fill rows (A4) flip from strata
  faster to RocksDB faster once barriers are cheap; nothing else changes direction.
- **Memory:** A3's atomic flush raises RocksDB's many-index peak 18% (140 → 165 MiB); A5's table
  reader variant is within 2.5%; A1, A2 change no memory.
- **Disk:** unchanged; the same files result (A1's fixed load writes its 104 MB in the load
  instead of the run, so the run's written bytes fall from 169 MB to 3.7 MB).
- **Indexing:** strata's durable fill rate at fits improves 1.17–1.23× with A2; RocksDB's
  many-index load-to-durable 3.86× with A3.

Cost at scale: these are measurement corrections, not engine wins, and the time they move is
barrier wait, not CPU; no CPU-hours or dollars follow from them.

## What did not run

Linux and FreeBSD: every finding is from the macOS host. A4 predicts the 2× fill rows differ on
Linux, where barriers are cheap; the gate tier must check it, and A1 to A3 are platform-neutral
harness fixes.

## Reproduce

```sh
bench/engines/rocksdb-11.11/build.sh
cc -O2 -dynamiclib bench/kv/barriers.c -o barriers.dylib
KV_ROCKSDB_STATS=1 KV_PHASE_MARKS=1 BARRIERS_LOG=row.tsv DYLD_INSERT_LIBRARIES=$PWD/barriers.dylib \
  ~/.scratch/rocksdb-11.11-build/kv_rocksdb --dir D --workload fillrandom --memory-mb 5 --variant lvl-16k-lz4
QUIET_LEVEL=light quiet run kv 30 "sync rows" -- python3 bench/kv/run.py rows OUT sync1@fix 3
```

Each counted run prints one `{"rocksdb_phase":…}` line per phase on stderr; `row.tsv` lists
every barrier with its time, duration, and file, aligned to the `phase NAME NS` marks.

Raw data in `rocksdb-fairness-audit/`: `counted.txt.gz` (every counted run's phases, barriers,
and counters), `counted-raw.tar.gz` (their JSON lines and stderr), `timed.jsonl.gz` (A1, A3, A4,
A5), and `settle-ab.jsonl.gz` (A2).
