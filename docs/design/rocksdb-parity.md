# Parity with RocksDB on every row

Every loss in `docs/gates/document-profile.md` (strata `d956c0f4`) gets a measured cause, the
mechanism that beats RocksDB rather than matching it, and its class: tuning, a mechanism change, or
an architecture change. The owner's PLAN D146 sets the bar (turn-log-parity A6): at par or better on
every row and axis.

Code is cited at strata `d956c0f4` (the `document-gate` head; its `bench/kv` and `include/` are the
code `dd02c63d` timed) and journal `cc95ba6` (strata's pin), unless a line names journal `main`
(`1222e3b`). RocksDB is 11.11 as `bench/engines/rocksdb-11.11/kv_rocksdb.cc` configures it. Every
number below is either from the gate's raw rows (`~/.scratch/document-gate/rows-*-dd02c63d-*`) or
from this note's light probes, named by job:

| Probe | What | Job |
|---|---|---|
| A | memory map (`vmmap`, `heap`), bytes written per file class and zero-filled, disk per file; sync1, wal1 and YCSB A rows at small scale | 261120-071503-0c741b |
| B | K writers on 1 to 8 logs, syncs per second, and a cohort-wait prototype | 261120-072003-b88d0c |
| C | `sample` profiles of the rows that spend more CPU | 261120-072004-d21c38 |
| D | re-homing groups at another loop count, 10k to 300k groups | 261120-074846-356a48 |
| E | a file per thread, one shared file, and files per thread with one shared flush, with and without the cohort wait, as a C floor (`devbarrier.c`) | 261120-074947-257624 |
| F | handing records from L loops to one log's owner: under each group's mutex, through per-loop rings, or asked of each loop, against one loop holding every group (`handoff.cc`) | 261120-083804-ec1239 |
| G | journal's `seal_turn` with D7's repeated calls: the design review's per-group bound against one bound per log, and the resume rule (`d7_model.py`) | 261120-083208-36162a |
| B2 | probe B again with phase marks that reach `wbytes.c`: syncs per operation in the run phase alone | 261120-083211-0cbbce |

The probes ran under light holds on a shared host, so their latencies are not row numbers; their
counts (bytes, syncs, records, pages) are host-independent. The scripts are in
`bench/kv/probes/rocksdb-parity/` (`wbytes.c`, `nosync.c`, `probe-*.sh`, `cohort.py`, `rehome.cc`,
`devbarrier.c`, `handoff.cc`, `d7_model.py`).

## Summary

| Loss (gate report) | Measured cause | Mechanism | Class |
|---|---|---|---|
| Disk up to 39.5× (sync/1), 3.3-7.9× (sync/10, /100), 2.6-3.7× (`wal1`) | the turn log's preallocated 4 MiB segments, current and spare: the index alone is at par or smaller | D1: a log sized by its live bytes | mechanism (journal) |
| Bytes written 36% to 3.2× | zero-fill of every new segment, on the turn; forward copies of live records past `retain_segments`; merges rewriting disjoint runs (`tenx1` fillseq) | D2, D9 | mechanism (journal), tuning, mechanism (merge policy) |
| Peak RSS 2.1-3.4× | freed megabyte buffers that macOS's medium malloc zone keeps resident; then a write buffer that flushes at 79% of the budget, inline | D3, D4 | mechanism (turn path), mechanism (background flush) |
| Group commit 1.39× at K=4, one log | a turn starts on the first writer back; the rest of the cohort waits a whole extra barrier | D5: cohort wait | mechanism |
| Group commit 1.9-2.1× on K logs | one flush per log per turn; the device coalesces them only partly | D6: one log, one flush per device | architecture: owner (Q1) |
| `wal` p99.99 130-193× | one record per group per turn caps `ack=none` at one command per barrier (D107, D108); segment preallocation inline on the turn | D7 | mechanism (journal packing), D1 |
| p99 tier none, 1 KiB fillrandom 0.78× | flushes inline on the writer | D4 | mechanism |
| CPU per op: `wal` and the VM rows 1.4-1.6×, `tenx1` fillseq 1.97×, overwrite 1.42× | copies and `madvise` on the turn path (75% of `wal`'s busy CPU); merges rewriting key-disjoint runs; the read before every upsert | D3, D9, D11 | mechanism; D11 adds a public operation (Q4) |
| `wal1` slower in the VMs | the same `ack=none` cap, at the VMs' slower barriers; CPU in copies and zero-fill (not profiled in the VMs) | D7, D2, D3 | as above |

The lead's five suspicions, against the measurements:

1. **The write buffer grows to the whole budget.** Partly: it flushes at 79% of the index's share
   (50.8 of 64 MiB) against RocksDB's 32 MiB memtable, and builds the segment inline. But the larger
   part of the RSS loss is freed turn-path buffers that macOS keeps resident (Peak memory).
2. **The log and the segment duplicate data, with no compression.** The log holds each command
   until a checkpoint, as RocksDB's WAL does (neither compresses: the bench leaves RocksDB's
   `wal_compression` off). The excess is inside the log: forward copies (2.1× the commands) and
   zero-fill. Index segments are LZ4-compressed and smaller than RocksDB's SSTs (Disk).
3. **Group commit loses at K=4 on one log and on four logs.** Confirmed, with two causes: on one log
   the next turn starts on the first writer back (D5); on four logs each log pays its own barrier
   (D6).
4. **The `wal` p99.99 and the 1 KiB 0.78× come from inline flush.** Refuted for `wal` (no flush
   runs; it is the one-record cap, D7) and confirmed for 1 KiB (D4).
5. **VM CPU 1.4-1.6×, unexplained.** On the host 75% of `wal`'s busy CPU is copies and `madvise`
   (D3); the VM profile is task 12.

Two owner sections follow the losses: how many logs (D6, measured), and what a changed loop count
does today and must do (D10).

## Disk

**Measured (probe A).** On every logged row, strata's index alone is at par with RocksDB's files or
smaller; the difference is the turn log's preallocated segments:

| Row (probe scale) | strata index | strata log segments | RocksDB files |
|---|---|---|---|
| fillrandom sync/1, 192 ops | 0.01 MiB | 4.00 MiB (one segment) | 0.07 MiB |
| fillrandom sync/100, 19k ops | 1.32 MiB | 4.00 MiB | 1.63 MiB |
| fillrandom `wal`, 100k ops | 6.87 MiB | 8.00 MiB (current and a recycled spare) | 7.23 MiB |
| overwrite `wal`, 100k ops | 6.89 MiB | 8.00 MiB | 11.79 MiB |
| YCSB A sync/1, 1 KiB, 100k records | 58.22 MiB | 8.00 MiB | 101.00 MiB (`lvl-4k-none`) |

The open writes the first 4 MiB of zeros (`open` phase, 4.0 MiB written): `TurnLog`'s constructor
calls `begin_segment()`, which preallocates inline when no spare exists (`turn_log.h:382-387`,
`:372-381`). A spare exists only after `reclaim()` recycles a segment (`turn_log.h:352-366`); nothing
outside tests and benches calls `prepare()` (`turn_log.h:226`; journal `main` greps the same). The
default segment is 64 MiB (`turn_log.h:47-53`), so a default host carries 64-128 MiB of log per loop
whatever it holds; `kv_strata` lowers it to 4 MiB because its commands are 1 MiB
(`bench/kv/kv_strata.hh:273-278`; `turn_seal.hh` requires two framed commands per segment).

**D1. The log's disk follows its live bytes.**

1. Segments come in a ladder: the first is the smallest that holds two framed records of the
   largest command chunk (`large-tx` makes that chunk the unit, so it no longer follows the
   transaction limit); each new segment doubles while the log has more than `retain_segments`
   segments of live records, up to `segment_bytes`, and the ladder steps back down when
   reclamation empties the log.
2. A segment is preallocated off the turn: the loop prepares the next spare when it is idle, and
   only the first segment of an empty log is prepared at open.
3. A reclaimed segment is recycled when it is the size the ladder wants, else removed.

Beats RocksDB because the log costs one small segment at rest, as RocksDB's WAL costs one empty
file, and the index is smaller than RocksDB's files at 1 KiB (58.2 against 101.0 MiB) and at 100 B
(1.32 against 1.63 MiB). Class: mechanism in journal (`turn_log.h`), coordinated with Detent's lead
(journal is a shared library).

Alternatives that lose:

- Truncating the tail after a checkpoint: the next growth zero-fills again, which is D2's cost.
- `fallocate` or `F_PREALLOCATE` instead of zeros: on ext4 and XFS a write into an unwritten extent
  converts it, which puts metadata in the barrier that `fdatasync` must journal (the reason
  standalone-turn-log D11 item 4 zero-fills).
- A smaller fixed segment: the large rows then copy forward more (D2).

## Bytes written

**Measured (probe A, `wbytes.c` counts each `pwrite` by file and whether it is all zeros).** YCSB A
sync/1 at 1 KiB, whose 100k-record load runs `ack=none` through the log:

| | strata | RocksDB |
|---|---|---|
| log, records | 210 MiB for 99 MiB of commands | 101.5 MiB WAL |
| log, zero-fill | 148 MiB (37 segments) | 0 |
| index segments or SSTs | 58.2 MiB | 201.8 MiB (flush 100, compaction 102) |
| total | 416 MiB | 304 MiB |

The log's records are 2.1× its commands: past `retain_segments` (4) each turn copies up to 1 MiB of
the oldest segment's live records forward (`turn_log.h:331-349`), and a logged index checkpoints only
at a buffer flush or every `checkpoint_log_bytes` (64 MiB by default, `index.hh:788`), so with 4 MiB
segments the live records span 12 or more segments and get copied, some more than once. Every
segment after the first is new and zero-filled, because one spare at most is recycled
(`spare_segments = 1`). `wal` fillrandom: 24 MiB of zeros and 16.5 MiB of records for 11.1 MiB of
commands, against RocksDB's 13.2 MiB WAL.

**D2. Write each log byte once.**

1. Recycle into a spare pool as deep as the retention window (`spare_segments = retain_segments`),
   so zero-fill happens only while the log grows past its high-water mark.
2. Checkpoint inside the window: a logged index's `checkpoint_log_bytes` is at most
   `(retain_segments - 1) × segment_bytes` divided among the groups that wrote since the last
   checkpoint, so a busy group checkpoints before its records age out and nothing is copied.
3. Forward copies stay until no live record is out of a checkpoint's reach. Item 2 covers busy
   groups only: an idle group's checkpoint stays live until the group closes (`turn_log.h:321`),
   `reclaim()` frees only segments with no live record (`turn_log.h:352-354`), and without the
   copy that checkpoint pins its segment, so the log grows until ENOSPC fails it and every index
   on it fences. Task 13 removes `forward_slice` once parking (D10 item 3) takes idle groups out of
   the log and a quiet group with batches is asked for a checkpoint when its oldest record reaches
   the window's last segment. Until then "nothing is copied" holds for busy groups, and the copies
   are idle groups' checkpoints (48 bytes each in probe D).

Test (tasks 3 and 4): one idle group's checkpoint in the oldest segment, then 10 ×
`retain_segments` segments of another group's turns; the segment count stays at or below
`retain_segments + spare_segments + 1`, and a mutant without the forward copy fails it.

Beats RocksDB: the log writes its commands once plus framing (RocksDB writes its WAL once too), and
strata's merges already write less than RocksDB's compactions on every YCSB row (gate report, 52-78%
less). Class: tuning (items 1-2: options, `index.hh`'s trigger); item 3 keeps journal's mechanism
until task 13.

`tenx1` fillseq's 3.2× is merges, not the log: D9.

## Peak memory

**Measured (probe A, `vmmap -summary` during `KV_PAUSE_AFTER_RUN`).** The resident set the gate
reports and the process's physical footprint diverge on the logged rows:

| Row | strata RSS peak | strata footprint peak | RocksDB RSS | RocksDB footprint peak |
|---|---|---|---|---|
| YCSB A sync/1 1 KiB (load `ack=none`) | 307 MiB | 85.8 MiB | 65.7 MiB | 43.2 MiB |
| YCSB A unlogged, same load | 105 MiB | 80.5 MiB | | |
| overwrite `wal` 100 B | 153 MiB | 34.9 MiB | 37.5 MiB | 28.4 MiB |

In the logged YCSB A process, 198 MiB is `MALLOC_MEDIUM (empty)`: freed memory the medium zone keeps
resident (6.5 MiB of it dirty). The turn path allocates and frees several megabyte-sized strings per
turn: the record's payload copy (`index.hh:532`, `batch_payload` of the open body), the logical
record (`turn_log.h:266`, `logical()` copies header and payload), the turn's bytes (the emitter's
string), and each forward copy (`turn_log.h:341-345`). The unlogged row allocates none of these.

What remains is the write buffer. Both strata processes peak near 81-86 MiB of footprint against
RocksDB's 43 MiB: `kv_strata` gives the index the whole budget (`memory / 4`, 64 MiB;
`kv_strata.hh:80`, `:314`), and the buffer flushes at `capacity / 1.26` (50.8 MiB,
`index.hh:2723-2726`), then builds the segment on the writer with a working set of 26% more
(`index.hh:2720`). RocksDB's memtable is `memory / 8` (32 MiB, `kv_rocksdb.cc:138`) and flushes on a
background thread.

**D3. The turn path owns its buffers.** One encode buffer per log, reused every turn; a record's
body is encoded once into its group's buffer with room for the batch header, so sealing hands it
over without a copy; the emitter frames header and payload straight into the turn buffer instead of
building `logical()` first; a forward copy is read into the turn buffer, not its own string (copies
stay until task 13, D2 item 3). Beats RocksDB, whose WAL writer builds each
`WriteBatch` and copies it into its log buffer: strata copies each logged byte once. It also cuts
the turn's CPU (see "CPU per operation"). Class: mechanism (`index.hh` `log_state`,
`turn_seal.hh`, journal `framing.h` and `turn_log.h`).

**D4. Background flush with a smaller active buffer.** A flush freezes the active buffer (O(1),
`freeze()`, `index.hh:5056`) and hands the frozen buffer to the host's executor, which builds the
segment while the writer continues into a new active buffer; the owner installs it at its next
publish point, as merges publish today. The active buffer's limit becomes half the index's share,
and the writer waits only when a second freeze would pass the share. The frozen buffer stays
readable (reads already walk `frozen_`, `index.hh:4571`). Beats RocksDB: same two-buffer bound,
and strata's buffer holds the inverted, key-ordered form a flush writes directly. Class: mechanism
(the core's flush path, `index.hh:5087-5110`); phase-3 D11 and D15 already specify the frozen buffer
for it. It closes the 1 KiB fillrandom 0.78× and the tier-none p99 losses too (Tails).

The checkpoint under the background flush. `stage_checkpoint` names `cur_.applied` when it stages
(`index.hh:3503`), which is right only while the flush is inline: with D4 the writer is past the
freeze by then, and a checkpoint naming its position would let the log drop acknowledged records
held only in the active buffer. So:

1. `freeze()` captures the frozen buffer's applied position X_f, its last transaction's.
2. The checkpoint names X_f and is staged after that segment's `commit_files` is durable, one per
   frozen buffer in freeze order, so successive checkpoints' X increase.
3. It takes the group's next sequence N, closing the open record first, and supersedes only the
   records through X_f.index (its `covers`): batches X_f.index + 1 to N − 1 hold transactions the
   segment lacks. Journal today drops all of a group's earlier records at a checkpoint
   (`turn_log.h:321`) and refuses one whose `covers` is below the group's last sequence
   (`turn_log.h:257`), so task 6 changes journal: the checkpoint record carries `covers` in its
   bytes (a log format change, pre-release), supersedes the group's records through `covers` and its
   earlier checkpoints, and recovery keeps the records after `covers`, the checkpoint among them.
   strata's attach reads the newest checkpoint wherever it sits in the list, and replay skips it
   (`index.hh:3375`, `:3461`).

Test (task 6): an `ack=sync` write lands in the active buffer during a background build; power is
cut after the install and after the checkpoint's barrier; the write recovers. The mutant stages
`cur_.applied` and fails it.

## Group commit

**Measured (probe B, job 261120-072003-b88d0c).** fillrandom sync/1 at 100 B, K writers each on
its own index, one log, two 8 s trials each; syncs are `F_FULLFSYNC` calls on the log files in the
run phase alone (probe B2, job 261120-083211-0cbbce: probe B's counts were the whole process's,
open included, because its phase marks never reached `wbytes.c`):

| K | strata ops/s | p50 | syncs per op | cohort prototype ops/s | p50 | syncs per op | RocksDB ops/s | p50 |
|---|---|---|---|---|---|---|---|---|
| 1 | 43, 43 | 21-23 ms | 1.00 | 44, 46 | 21-22 ms | 1.00 | 44, 42 | 21-22 ms |
| 2 | 50, 64 | 22-44 ms | 0.61-0.80 | 88, 72 | 21-22 ms | 0.50 | 77, 85 | 21-22 ms |
| 4 | 97, 90 | 42-45 ms | 0.42-0.43 | 177, 183 | 21-22 ms | 0.25-0.26 | 139, 153 | 21-22 ms |
| 8 | 228, 241 | 22-42 ms | 0.20 | 360, 294 | 21 ms | 0.13-0.14 | 175, 176 | 42-43 ms |

The device takes about 45 flushes a second in every configuration with one log (36-48 syncs/s). What
differs is how many writers each flush carries. strata's loop writes the next turn as soon as one
record is open: after a turn it calls `turn()` again at once (`turn_seal.hh:281`), finds nothing,
sleeps, and the first writer back wakes it (`turn_seal.hh:156-162`). That writer's record leaves
alone, microseconds ahead of the others, which then wait out its barrier and their own: the p50 of
two barriers at K=2 and K=4. RocksDB's leader forms its group the same way but its followers spin
and yield before sleeping (`write_thread_max_yield_usec`), so more of them make the group.

**D5. The cohort wait.** After a turn that released n waiting writers, the loop waits until n
records have opened, or a bound passes, before it seals the next turn. The prototype (`cohort.py`:
`turn()` counts the writers it releases, `wake()` counts arrivals, `serve()` waits up to 1 ms)
carries every writer in every barrier: 0.26 syncs per op at K=4 and 0.13 at K=8, p50 one barrier.
Against RocksDB that is 1.23× faster at K=4 (180 against 146 ops/s, trial means), 1.87× at K=8 (327
against 175), a tie at K=1 and K=2, with RocksDB's p50 or better. The bound in the mechanism is a
fraction of the last barrier (1/16: 1.3 ms at 21 ms, a few µs on a fast NVMe flush), so a writer
that does not come back costs the others at most 6% of a barrier. The last barrier is the last
turn's write and sync, from submission to completion, without the cohort wait, so the bound does not
feed back into itself. Arrivals count joins, writers that joined a record, not records opened:
`wake()` fires only when a record opens (`index.hh:3541`), so K writers sharing one index would
never reach n and every turn would wait the whole bound. Writers that wait in `await_sealed` count
as released only when their record sealed. Under D6 n and the arrivals are counted across every loop
on the log, and the bound is at least the fan-out's wake (D6's handoff). Class: mechanism
(`turn_seal.hh` `turn()`, `serve()`, `wake()`), tested by a per-writer barrier count at K writers on
K indexes and K writers on one index, with a mutant that drops the wait. Alternatives that lose: a
fixed group-commit timer (RocksDB's `WriteOptions::sync` has none; a timer delays K=1 by its length)
and several turns in flight (`RFTW0002`, parked; it does not help a device that takes one flush at a
time).

## How many logs (owner, 2026-11-20)

The owner asked whether one turn log per event loop is what makes the logs fast. Measured on the
macOS host (probe B, same rows), it is not: the device flushes about 45 times a second, and more
logs on one device only split those flushes.

| K writers | 1 log, today | 1 log, cohort | 2 logs, cohort | 4 logs, cohort | K logs (one per loop), today | RocksDB, one WAL |
|---|---|---|---|---|---|---|
| 2 | 57 / 44 | 80 / 41 | 46 / 47 | | 46 / 47 | 81 / 46 |
| 4 | 94 / 40 | 180 / 46 | 90 / 46 | 74 / 76 | 73 / 74 | 146 / 45 |
| 8 | 235 / 47 | 327 / 43 | 180 / 47 | 142 / 74 | 124 / 127 | 175 / 35 |

Each cell is ops/s / syncs/s, the mean of two trials; syncs/s here count the whole process, open
included. The run phase alone (probe B2) keeps the shape: one log 32-46 syncs/s at every K, K logs
44, 66 and 113 at K = 2, 4 and 8.

With one log per loop each log syncs alone: syncs per op stay at 1.02, and concurrent
`F_FULLFSYNC` calls coalesce at a longer latency (p50 42-62 ms against 21 ms), so K logs reach 1.6-2.4×
the syncs of one log and fewer operations. One log shared by every index, with D5, is 2.5× faster
than one log per loop at K=4 and 2.6× at K=8. On this host "one log per device" and "one shared log"
are the same configuration: there is one device. D104 chose a log per core on the coalescing
numbers (46 calls/s on one log against 185 on eight); those calls carried one record each, which
the table shows is the wrong currency: commits per second, not syncs per second, is the axis.

The same three shapes without strata, as a C floor (probe E, job 261120-074947-257624, `devbarrier.c`):
T threads each commit 4 KiB records in a closed loop, 5 s per cell, with D5's cohort wait (1 ms)
on the shared flushes. Commits/s, with commits per flush:

| T | one file per thread, own `F_FULLFSYNC` | one shared file, one flush per cohort | file per thread, `F_BARRIERFSYNC`, one shared `F_FULLFSYNC` |
|---|---|---|---|
| 1 | 45 (1.00) | 46 (1.00) | 87 (1.00) |
| 2 | 49 (1.00) | 92 (2.00) | 100 (2.00) |
| 4 | 78 (1.00) | 183 (4.00) | 184 (3.72) |
| 8 | 111 (1.00) | 369 (8.00) | 253 (5.31) |

Without the cohort wait, the shared file carries half the cohort per flush (T=8: 187 commits/s,
4.05 per flush), the same loss as strata's loop today. The device coalesces concurrent flushes on
separate files only partly (T=8: 111 against 45 at T=1), so each extra log is a fraction of a
flush, never free. The barrier-then-flush column's T=1 cell flushes in 5 ms against 21 ms, which
suggests `F_FULLFSYNC` returns early when `F_BARRIERFSYNC` left nothing dirty; that is a durability
question, not a speedup, and it is not taken as one.

**D6. One flush per device, carried by one log per Host directory, one Host per device.** All of a
Host's loops append to one turn log, owned by one loop; the owner's turn takes every loop's records
(the handoff below), and D5's cohort wait makes every writer on the device ride one flush. Two Host
directories on one device still split its flushes, so a deployment runs one Host per device.
Measured on this host it is the fastest
shape in both probes (strata K=8: 327 ops/s against 124 for a log per loop, 2.6×; the C floor:
369 against 111, 3.3×), and it beats RocksDB's one WAL by 1.23-1.87× at K=4-8 (Group commit). It
also removes the loop count from the files: groups are placed on loops in memory, and a changed
loop count re-homes them with no copy (D10). Class: architecture (Track C D11's per-core logs and
D104 change), so it is the owner's: Q1.

Alternatives:

- *Per-loop appends with one device flush* (the third column): at par with one log at T≤4, 31%
  slower at T=8, because every loop still pays its own `F_BARRIERFSYNC`; on Linux the equivalent,
  `sync_file_range` before one `fdatasync`, gives no ordering guarantee across files without the
  flush. It keeps appends on their cores, which matters only if one log's append becomes the
  bottleneck; at 100 B `ack=none` one log already runs 97.7k ops/s, RocksDB's rate.
- *One log per loop, as today*: 2.5-3.3× slower at K=4-8 on macOS. On Linux with NVMe, concurrent
  `fdatasync` calls on separate files may overlap better; unmeasured here (no bare-metal Linux, D105;
  a VM's virtio flush is not a device's). Task 10 measures the Linux floor before D6 lands.

### D6's handoff

Every `TurnDriver` mutation is confined to its loop today: `seal` runs "on the loop", `stage` pushes
`pending_` with no lock, and so do `link`, `unlink` and `forget` (`turn_driver.h:139-153`,
`:164-180`, `:222-234`, `:403-422`). strata's standalone loop serializes every driver call under one
`turn_mu_` per loop, held across the barrier (`turn_seal.hh:164-176`, `:325`). With one log per
device a record sealed for an index on loop i must reach the owner's turn, and its completion must
come back. Probe F (job 261120-083804-ec1239) measured three ways: L loops of 4 groups, each with
one `ack=sync` record in flight, a spin of 20 or 200 µs standing in for a fast device's barrier;
commits/s as a fraction of one loop holding all 4L groups, at L = 2, 4 and 8:

| Barrier, cohort bound, waiting loops | pull: group mutex | push: per-loop rings | ask: each loop seals |
|---|---|---|---|
| 20 µs, 1/16 (1.25 µs), sleep | 0.45, 0.40, 0.41 | 0.45, 0.42, 0.43 | 0.26, 0.29, 0.31 |
| 20 µs, 40 µs, sleep | 0.62, 0.54, 0.51 | 0.59, 0.54, 0.51 | 0.39, 0.38, 0.34 |
| 20 µs, 1/16, spin 50 µs first | 0.93, 0.95, 0.91 | 0.92, 0.95, 0.95 | 0.81, 0.92, 0.84 |
| 200 µs, 1/16 (12.5 µs), sleep | 0.71, 0.53, 0.89 | 0.67, 0.50, 0.86 | 0.54, 0.77, 0.84 |
| 200 µs, 40 µs, sleep | 0.92, 0.86, 0.85 | 0.76, 0.70, 0.85 | 0.86, 0.82, 0.76 |
| 200 µs, 1/16, spin 50 µs first | 0.50, 0.51, 0.57 | 0.50, 0.50, 0.57 | 0.44, 0.44, 0.52 |

1. The lock and the queue cost the same: pull and push agree within noise in every row but one
   (200 µs with a 40 µs bound, where pull leads), and the owner collects 32 records in 1.2 µs under
   the mutexes, 0.2 µs from the rings. Pull keeps packing, since a record stays open until its turn
   takes it, and push loses it, since it seals at the join; so the lock wins.
2. Asking each loop loses in every row: its collect is a round trip to every loop, 21-28 µs a turn
   at L=8, and a busy loop would stall the whole device's turn.
3. The cost is the wake, not the handoff: a loop resumes 9-17 µs after the owner's notify (mean, on
   this host under light holds), the order of a 20 µs barrier. A cohort bound of 1/16 of the
   barrier is shorter than that below about 250 µs and misses half the cohort (0.40-0.45 at 20 µs).
   A bound that covers the wake recovers 0.85-0.92 at 200 µs, but only 0.51-0.62 at 20 µs, where the
   wake itself is a third of each turn. Loops that spin through the barrier recover 0.91-0.95 at
   20 µs, at up to 1.6 times the CPU per commit of sleeping loops (7.0 against 4.3 µs at L=8; one
   loop holding every group spends 0.7); a spin shorter than the barrier only deepens the wake
   (0.50-0.57 at 200 µs).
4. At macOS's 21 ms `F_FULLFSYNC` a 15 µs wake is 0.07% of a turn. On a fast device the wake
   matters, and one log per loop with L flushes the device overlaps would pay none of it: task 10's
   Linux floor runs `probe-f.sh` with `probe-e.sh`, and D6 lands there only if it still wins.

The protocol, for task 10:

1. **Records are pulled under the group's lock.** The owner's `seal_turn` calls every sealer on the
   owner's thread. strata's sealer already guards its state with the group's mutex, shared with
   `join` on the writer's thread (`index.hh:492-536`), so nothing is marshaled per record and apply
   stays on the index's loop. Journal's sealer contract changes from "on the loop" to "on the log
   owner's loop, under no lock of the driver's; the sealer guards its own state".
2. **The driver's state takes one mutex, never across the barrier.** `seal`, `unseal`, `stage`,
   `link`, `unlink` and `forget` from any loop take it (`sealers_mutex_` grows to cover `pending_`
   and the waiter list); `start()` stages under it, the write and barrier run outside it, and
   `completed()` and `failed()` take it again. strata's `turn_mu_` becomes that mutex and stops
   covering the barrier, so a create or a seal on any loop waits for one staging, not a barrier.
3. **Completion: one wake per loop per turn.** `completed()` resumes each waiter on its home loop,
   as `Wait` already posts to `waiter.home` (`turn_driver.h:424-433`), batched per loop; watchers
   (strata's `durable_through`) run on the owner's thread under their group's lock.
4. **Failure: every index on the device.** A failed write or barrier fails the log
   (`turn_driver.h:259-261`), and every operation staged in or after that turn fails on its home
   loop: every index on the device fences with `outcome_unknown`, not one loop's. This is D9's
   fail-stop at the scope of the device whose flush failed.
5. **The cohort across loops.** D5's n is the writers the turn released on every loop, and the
   arrivals are joins on any loop: two counters per log, each on its own cache line. The bound is
   the larger of 1/16 of the last barrier and three times the log's recent fan-out wake (probe F:
   1/16 alone misses half the cohort below about 250 µs barriers). A loop whose last barrier took
   under 50 µs spins until the turn completes, at most 50 µs, before it sleeps; after a slower
   barrier it sleeps at once.
6. **D7's admission counter** is one more atomic per log, on its own cache line.

Test (task 10): K loops (2, 4, 8) on one device log, under TSan, with a power cut at every
operation (faultfs) of a mixed `ack=sync` and `ack=none` run: every acknowledged record recovers,
and a failed barrier fences every index on the device. Task 10's Linux floor reruns `probe-f.sh`
beside `probe-e.sh`.

### D6's damage scope

Where no manifest reads back for a damaged block, recovery loses every group that wrote nothing
after it, and on any damage it loses every open group with no records (`turn_log.h:568`, `:604`).
With a log per loop that is one loop's groups; with one log per device it is every group on the
device, L times wider. A2's mechanism must hold on the shared log, and its tolerant-open test runs
on one log holding groups of at least two loops: a damaged unmanifested block opens every group its
checkpoint covers and fails only the uncovered ones. Durable parking (D10 item 3) takes parked
groups out of the second rule. What could narrow the first:

- *Per-loop manifests* do not. A block's manifest, in the next block's header, already names its
  groups; the wide case is a manifest that does not read back (the damaged block was the last one
  written, or the next one is damaged too), and splitting its entries by loop does not make it read
  back. Separate manifest files per loop would add a write per loop to every turn, which D6 removes.
- *A roster record* does. At each segment's first turn the log writes every live group's last
  sequence, 16 bytes a group. Damage before a roster then costs only the groups whose records were
  in the damaged span: a group that wrote nothing after the damage keeps its records when the roster
  names the sequence it recovered. Cost: 16 bytes per live group per segment, 160 KiB at 10k groups
  and 1.6 MiB at 100k (0.24% and 2.4% of a 64 MiB segment), written in turns of at most a quarter
  segment; with parking, live groups are only those with records in the retention window. Damage in
  the newest segment, after its roster, stays wide. Not needed for A6: task 14, after parking, a log
  format change.

## Tails

**`wal` p99.99, 130-193×.** Not a flush: the `wal1` rows' buffers never reach a flush or a
checkpoint (11-22 MiB of commands against a 50.8 MiB buffer limit and a 64 MiB checkpoint), and
probe A's `wal` runs wrote no index segment until settle. The cause is the `ack=none` cap that
D107 and D108 set: journal calls a group's sealer once per turn and it returns one record
(`index.hh:483`, `turn_driver.h:447-506`), so a writer faster than one 1 MiB command per barrier
blocks in `await_sealed` until the loop seals (`index.hh:3522-3540`). At 100 B values the writer
makes about 30 MB/s, a 1 MiB record every 35 ms, against a 20-36 ms barrier (M4: p50 20.2 ms, p99
36.2 ms): the ten slowest of 100k puts are those waits (p99.99 9.5-17.2 ms). Slower barriers widen
it: a turn that rolls into a new segment also preallocates it inline, 4 MiB of zeros, a full sync
and a directory sync on the turn (`turn_log.h:382-387`; probe A counts 84 directory syncs in one
1 KiB load), and the VMs' barriers are slower still, which is the `wal1` VM loss.

**D7. Pack the turn; bound the window per log, in bytes.** Journal calls a sealer again with the
room left until it returns nothing (D108's request to Detent). strata's bound becomes the log's
unsealed bytes, every group's, not one record and not one group's: one admission counter per log
holds the cost (`record_cost`, framing included) of every record joined or staged and not yet
sealed, at most one turn's room after its first record (`turn_room(segment_bytes / 2)`,
`turn_driver.h:455`), which the segment geometry already sizes for a largest command
(`turn_seal.hh`'s two-command rule). A join that would pass it waits, neither admitted nor applied,
in arrival order, until a seal frees room. Every admitted record then fits the next turn, so the
window stays one barrier plus one turn (D107's contract) at any group count, and under D6 the
counter is one atomic per device. With D1's off-turn spares no turn pays a preallocation.

A bound per group at the room lets N saturating groups hold N rooms, and the oldest record waits N −
1 turns: probe G reproduces the review's 7 turns at N=8 and 31 at N=32, and with the bound per log
every admitted record seals in the next turn at N = 1 to 256, each group sealing its share (8,000
records each at N=2, 62 at N=256, of 16,000 per run). An `ack=sync` record waits in arrival order
behind the writers ahead of it, at most ⌈W·c / room⌉ turns for W writers of commands of cost c (4
turns behind 31 bulk writers of an eighth of the room), then seals in the next.

The resume rule changes with it. Today a sealer that returns nothing under less than the whole room
resumes the next turn at its own group (`turn_driver.h:492`), so a group bounded past the room
starves the groups after it (probe G: 0 of 2,000 records sealed; with the rule below, all 2,000,
each within 1 turn). A sealer that returned nothing after sealing in this turn has drained; the next
turn resumes after it. One that returned nothing with a record waiting resumes at its group, which
then gets the whole room. With the per-log bound no group holds past the room, so the rule guards
records `stage()` adds; those take from the counter too.

Beats RocksDB: its unsynced WAL window is unbounded in time; strata's is one barrier, and the writer
still never waits below `room / barrier` (32 MiB per 20 ms at the default geometry). Class:
mechanism (journal `turn_driver.h` `seal_turn`, strata `log_state::join` and `seal`). Test (task 5):
8 groups saturating `ack=none` and one `ack=sync` group; every record seals in the turn after its
admission and within 2 turns of its join when the log has room, counted per record, and a mutant
with the per-group bound fails it.

**p99 tier none (`tenx1` fill and overwrite 1.19-2.08×, `twox1` overwrite 1.64×) and 1 KiB
fillrandom 0.78×.** The writer flushes inline: at 12 MiB the budget is 3 MiB, so the buffer flushes
every few thousand puts on the writer's thread (`index.hh:992-993` calls `flush_point()` inside
`apply`), and every put due during the flush waits; `docs/research/no-stalls.md` measured the same
stall at 80 µs per document. The 1 KiB row's profile (`8c6f59`, document-gate journal) shows it.
D4's background flush removes it from the writer.

**p99 sync/100 (1.13-1.30×).** A sync/100 batch is applied at `durable()` as one transaction, so
the put that waits is the batch's last, and both engines wait one barrier there. The gate's own
note says these p99s move by 2× between trials for both engines (`docs/perf/standalone-turn-log.md`,
"Four axes"). Not a mechanism loss; A6's exclusive rerun decides it, and D4 removes the flush that
can land in a batch.

**Time to durable (`fits1` overwrite 0.79-1.14×).** The settle's commit: an inline flush of the whole
buffer and its manifest barriers. D4 shortens it, since the background flush has written most of the
buffer by the settle.

## CPU per operation

**Measured (probe C, job 261120-072004-d21c38, `sample` over the run phase, 1M operations;
shares are of the samples not blocked in a wait or a barrier).**

| Row | strata CPU/op | RocksDB CPU/op | where strata's busy CPU goes |
|---|---|---|---|
| `wal` fillrandom 100 B | 4.29 µs | 10.59 µs | copies (`memmove`) 45%, `madvise` 21%, malloc and free 8%, `pwrite` 9% |
| `tenx1`-scale fillseq | 3.51 µs | 1.63 µs | building segments: `pwrite` 24%, LZ4 21%, filters 7.5%, CRC 7.5% |
| `tenx1`-scale overwrite | 9.42 µs | 7.85 µs | key lookups before the write 12.6% (`keys::may_hold` per segment, `fst` floor, term dictionary), LZ4 8.7%, term dictionary build 6%, copies and `madvise` 8.3% |

1. **`ack=none` and the VM rows (1.4-1.6× there).** On the host strata already spends 2.5× less
   than RocksDB, but three quarters of what it spends is the turn path's copies and the allocator
   returning their megabyte buffers (`madvise`), which D3 removes. In a VM both cost more (a
   `madvise` is a trap to the hypervisor's page tables, and `wal1` wrote 1.9-2.9× RocksDB's bytes in
   the VMs). The VM profile itself is not taken (light probes only); task 12 takes it.
2. **fillseq at 10× (1.97×).** RocksDB's leveled compaction moves non-overlapping SSTs down without
   rewriting them; strata's tiered policy merges by size alone (`merge_policy.hh:16-60`, Lucene's
   TieredMergePolicy), so sequential keys are rewritten at every tier: 156.5 against 70.2 MiB written
   in the run. **D9. Sorted runs in the merge policy.** A segment whose key range overlaps no other
   segment of its tier joins the next tier as it is, as RocksDB's trivial move does; the policy
   counts overlapping runs, not segments. Reads lose nothing: scans skip a segment by its key
   range (`index.hh:2506`) and so do point gets (`keys.hh:211`). Beats RocksDB:
   strata's keys section and filter are per segment, so a moved segment needs no rewrite at all,
   and random keys still merge by size. Class: mechanism (`merge_policy.hh`, `index.hh`'s merge
   scheduling and `lookup_key`). It closes `tenx1` and `twox1` fillseq's bytes written (3.2× and
   2.5×) too.
3. **overwrite at 10× (1.42×).** strata reads before it writes: a replace looks up the key's live
   copy in every buffer and segment, newest first (`index.hh:4567-4585`), because the operation
   returns the document's version and keeps its docid; RocksDB writes blindly and resolves at
   compaction. The rest is segment building, which D4 moves off the writer and D9 reduces.
   **D11. Resolve unversioned upserts at flush.** An upsert whose caller asks for no version and
   no condition records only the key in the buffer; the flush resolves every such key against the
   segments in one sorted pass (the buffer's keys are ordered, and so are each segment's), setting
   the delete bits the per-operation lookup sets today. Reads already see the buffer first. Beats
   RocksDB: the delete is resolved once, at flush, and compaction never carries the old value
   forward. Class: mechanism and a public API addition (an unversioned upsert operation), so the
   owner's: Q4. Without it, D4 and D9 still take overwrite's CPU to about 1.1× by this profile's
   shares; that is a loss A6 would report.
4. **sync/1 fillrandom (1.15×).** Not reproduced: probe B's K=1 rows spent 228-275 µs per
   operation against RocksDB's 274, with the same throughput and p50. Most of either engine's CPU
   there is the kernel's flush path. A6's exclusive rerun decides the row; no mechanism here.

## A changed loop count (owner, 2026-11-20)

The owner asked whether logs survive a different loop count: a disk moved to a server with another
CPU count, or a changed configuration. They do today, by a mechanism that lives in journal, not in
strata's docs: journal `docs/design/host-layout.md`, "Placement and re-partition", and
`include/journal/host.h`.

**What happens today** (journal `cc95ba6`; `main` is the same):

1. **Placement.** A group's slot is its id through splitmix64's finalizer into 4,096 slots, and its
   loop is `(slot × L) >> 12` (`host.h:81-87`): each loop owns a contiguous run of slots. Nothing
   stores a group's loop. The group table is sharded by slot (`groups-<slot>`), not by loop, so it
   never moves.
2. **Open.** The `layout` file holds L and the log generation; a host directory with groups or logs
   and no layout refuses to open (`host.h:424-436`). Logs of any other generation are leftovers of an
   unfinished re-partition and are removed (`:438-446`).
3. **Another L re-partitions before serving** (`host.h:447-448`, `:481-520`): it reads every log of
   the stored generation, all `L_old` of them, so a log whose loop no longer exists is replayed;
   marks the ids recovery calls lost; writes each group's live records into its new loop's log of
   generation + 1 in turns of at most a quarter segment, each synced; replaces `layout`; removes the
   old logs. A crash before the layout's replace leaves the old generation authoritative, and the next
   open starts over. Journal's `placement_test` re-partitions one host through 3, 2, 5 and 4 loops,
   cuts power at every operation of each, and checks every record comes back
   (`test/placement_test.cc:169-205` at `main`).
4. **strata's side.** An index stores its host identity and group in its `turn_group` attribute
   (`index.hh:582-605`), never a loop; the host's `loop(group)` routes it (`turn_seal.hh` `mine()`
   checks it), so a re-partitioned group reopens on its new loop.

**Gaps found:**

1. Re-partition holds every group's live records of every log in one map
   (`std::map<group, vector<TurnRecord>>`, `host.h:482`) until it has written them all: memory and
   open time grow with the live bytes of all logs: +30 MiB for 100k idle groups (probe D). Its worst
   case is the logs' retained bytes, L × `retain_segments` × `segment_bytes`: 8 GiB at 32 loops of
   64 MiB segments, enough to fail an open.
2. Every open group keeps its latest checkpoint record live in the log until it closes
   (`turn_log.h` `note()`: a checkpoint supersedes only the group's earlier records), so the live
   bytes, the forward copies (D2) and the re-partition's copy all grow with the number of open
   indexes, not with what was written.
3. A log file of the current generation whose loop is L or more is neither read nor removed: open
   creates drivers for loops below L only (`host.h:450-454`) and removes other generations only.
   Reachable only by copying files by hand, but silent; it should refuse as corruption.
4. No strata test reopens a logged index at another loop count, and `kv_strata` derives its group
   ids from L (`bench/kv/kv_strata.hh:288-290`), so its data reopened under another `KV_LOOPS` names
   other groups than its indexes recorded. Production must never derive a group id from L.
5. Creating a group rewrites its slot's whole table shard and pays two barriers, the shard file's
   and the directory's (`host.h:307-314`, `:377-381`, `replace_file` at `:141-147`; probe D counted
   three sync calls per create, 30,023 for 10k). At 20 ms a barrier, a million indexes take 11 hours
   to create one at a time.
6. Every open starts a new segment per loop, and the old ones stay: idle groups' checkpoints keep
   them live and nothing forwards without turns. Probe D's directory grew by L segments per open at
   the same L (16.5 → 32.5 MiB at L=4, 64.5 → 96.5 MiB at L=8, 4 MiB segments); at the default
   64 MiB and 32 loops that is 2 GiB per restart.

**D10. Logs recoverable under any loop count; idle groups cost nothing to re-home.**

1. With D6, logs belong to devices, not loops: a changed loop count re-homes groups in memory
   (placement by hash at open, as today) and copies nothing; only a changed device count
   re-partitions, which a disk moved to another server does not cause. Without D6 (Q1 declined),
   keep journal's protocol (hash placement, one copy into the next generation, the layout as the
   commit point): it is crash-tested and costs one copy of the live bytes. *Adopting the old logs
   read-only* until each group checkpoints anew loses: every later recovery reads two generations
   and stitches each group's sequence across them, and an idle group never checkpoints, so the old
   logs never go.
2. Stream the copy: read one old log, append each record to its new loop's open turn, and sync every
   new log's open turn when their sum reaches a quarter segment. Memory is then a quarter segment
   (16 MiB at the default geometry) plus one record, not every live record, at L_new syncs per
   quarter segment copied.
3. Park idle groups in the table, durably. Once a group's last record is a checkpoint and a
   retention window has passed without a write, the host parks it: the group's table entry takes
   that record (its `base` and payload, as `close_group` does, `host.h:324-336`) and a new state,
   `parked` (`GroupState` 4, a table format change: `host.h:56`, `:185`); the shard is durable
   before the log forgets the group. Its next write unparks it: the entry returns to `open` in or
   before the turn that carries the group's first record after parking (item 5 makes it ride that
   turn, and the turn is acknowledged only once the shard is durable too), so that record is never
   durable while the table still says parked. Recovery exempts parked groups from the damage rule:
   the table lists them apart from the open ones (`host.h:398-409`), so damage anywhere in the log
   costs no parked group (`turn_log.h:604`). An unparked group is open again and the rule covers it:
   one whose only post-park record sat in a damaged block is lost, never reset to its base.
   Re-homing then copies only groups with records since their last checkpoint, at most the logs'
   retention windows, whatever the number of open indexes. Detent's `journal-idle-groups`
   (`TurnDriver::park`, `TurnLog::park`, its `docs/mechanisms/group-lifetime.md`) parks a released
   group in memory and keeps its records live in the log; this park moves its last checkpoint into
   the table and takes it out of the log. The two share a name across two layers: the journal text
   (task 9) goes to Detent's lead through the lead, who settles the name with them. Test (task 13):
   damage an unmanifested block; parked groups open from their base, and an unparked group whose
   only post-park record was in that block reports lost; a power cut at every operation between
   unpark's shard write and that record's barrier loses nothing acknowledged.
4. A strata test reopens logged indexes at L = 1 → 3 → 2 with a power cut during the
   re-partition, and the kv harness keeps each index's group id in its directory.
5. Group creations and closes join the loop's turn: the table shards they touch are written with
   the turn and published at its barrier, so creating many indexes costs one barrier per turn, not
   two per index. Parking (item 3) makes gap 6 go too: an open with no live records starts no
   segment.

Class: mechanism in journal (items 2, 3 and 5, coordinated with Detent's lead), tests and harness
in strata (item 4). Items 2, 3 and 5 are not needed for A6's rows; they are needed before a host
holds millions of indexes (many-indexes workload), so they go to the owner as Q3.

**The cost of re-homing** (probe D, job 261120-074846-356a48, `rehome.cc`: journal's Host on a real
directory, syncs made counted no-ops by `nosync.c`, so CPU, memory and syncs, not device time; idle
groups with one 48-byte checkpoint record each, 4 MiB segments):

| Idle groups | open at the same L | open at another L (re-partition) | extra CPU per group | extra syncs | peak memory added at open |
|---|---|---|---|---|---|
| 10k, 4 → 8 | 0.12 s | 0.21 s | 4.5 µs | 77 | +5.4 MiB |
| 100k, 4 → 8 | 0.32 s | 0.69 s | 3.8 µs | 85 | +42 MiB |
| 100k, 8 → 2 | 0.28 s | 0.65 s | 3.7 µs | 39 | +36 MiB |
| 100k, 10% with a 1 KiB batch | 0.30 s | 0.74 s | 4.0 µs | 93 | +76 MiB |
| 300k, 4 → 8 | 0.78 s | 2.06 s | 4.3 µs | 130 | +165 MiB |

Extrapolated linearly, a host with 1M open idle indexes pays 2.6 s of CPU and about 550 MiB to
recover its logs at every open, and 4.3 s more to re-partition, plus a few hundred syncs (several
seconds of barriers at 20 ms); 10M pay 70 s and 5.5 GiB. Every cost scales with idle groups,
which wrote nothing: item 3 (parking) takes all of it to the groups with records in the retention
window, and D6 takes the re-partition itself to zero. Creating those groups costs more than
re-homing them: 94 s of CPU and 900k sync calls for 300k (gap 5, item 5). The re-partition itself
raised the peak a further 30 MiB at 100k and 64 MiB at 300k (its in-memory map, gap 1). Those are
idle groups' bytes; the map's worst case is every log's retained bytes, L × `retain_segments` ×
`segment_bytes` (8 GiB at 32 loops of 64 MiB segments), which item 2's stream bounds at a quarter
segment and D6 removes on a loop-count change; it remains on a device-count change, and wherever
Q1 is declined.

## Decisions not taken here

- **Compressing log records.** RocksDB's bench WAL is uncompressed too, and the log's excess is
  copies and zeros (D2), not entropy. Worth a probe after D2 lands, on the 1 KiB rows only.
- **Changing the harness's budget split** (giving strata RocksDB's `memory / 8` buffer). That matches
  a setting rather than beating a mechanism; D4 bounds the active buffer at half the share instead.
- **Several turns in flight (`RFTW0002`).** Parked in standalone-turn-log; a device that flushes one
  at a time gains nothing from it (Group commit).
- **A fixed group-commit delay.** It delays K=1 by its length; D5's bound follows the cohort and the
  barrier.

## Questions for the owner

Each has a default the tasks follow until answered.

- **Q1. One log per device (D6).** Architecture: logs stop being per loop (Track C D11, D104).
  Options: (a) D6, one log and one flush per device, every loop appending to it; (b) per-loop logs
  with one device flush (`F_BARRIERFSYNC` per log), 31% slower at T=8 on macOS; (c) keep one log
  per loop and accept the K-log rows as losses (2.5-3.3× at K=4-8). Recommendation: (a): it is the
  fastest shape measured, and it makes a changed loop count free (D10). What (a) costs: a failed
  barrier fences every index on the device, damage with no readable manifest costs every quiet
  group on the device rather than one loop's (D6's damage scope; task 14's roster narrows it), and
  each turn pays the handoff (D6's handoff). Default: tasks 1-9 proceed, which help every option;
  task 10 waits for the answer (PLAN D148 took (a)).
- **Q2. Which memory the gate compares.** The harness reports the higher of RSS and the physical
  footprint; on macOS RSS counts freed pages the allocator keeps. Recommendation: keep it (it is
  what the host pays), and let D3 remove the pages; add the footprint as a column so the two
  causes stay apart. Default: as recommended.
- **Q3. Idle groups leave the log, re-partition streams, and group creation joins the turn (D10
  items 2, 3 and 5).** A journal change shared with Detent. Not needed for A6's rows; needed
  before a host holds millions of open indexes. Recommendation: yes, after A6, coordinated with
  Detent's lead. Default: task 9 only records the design in journal's docs.
- **Q4. An unversioned upsert (D11).** A public operation that returns no version and whose delete
  resolves at flush. Without it, overwrite at 10× stays about 1.1× in CPU. Recommendation: add it.
  Default: not built; A6 reports the overwrite CPU row.

## Tasks for turn-log-parity

In order; each is one landing with its test and the rows it moves.

| # | Task | Files | Rows |
|---|---|---|---|
| 1 | D5 cohort wait, bound 1/16 of the last write and barrier, arrivals counted by joins; a test counting barriers per writer at K=4 on K indexes and on one index, with a mutant that drops the wait | `include/strata/core/turn_seal.hh`, `tests/test_turn_seal*` | group commit K=2-8 on one log |
| 2 | D3 the turn path owns its buffers: per-log encode buffer, records encoded in place, framing straight into the turn | `include/strata/core/index.hh` (`log_state`), `turn_seal.hh`; journal `framing.h`, `turn_log.h` | peak RSS on logged rows, `wal` CPU, VM `wal1` |
| 3 | D1 segment ladder and off-turn preparation, D2 item 1 spare pool; D2 item 3's idle-checkpoint test | journal `turn_log.h`, `turn_driver.h`; strata `bench/kv/kv_strata.hh` geometry | disk on every logged row, bytes written, `wal` p99.99 (preallocation) |
| 4 | D2 item 2 checkpoint inside the window; D2 item 3's test again | `index.hh` (`checkpoint_log_bytes`, `log_due`) | bytes written (YCSB logged loads) |
| 5 | D7 pack the turn; one admission bound per log; the resume rule | journal `turn_driver.h` (`seal_turn`); `index.hh` (`log_state::join`, `seal`) | `wal` p99.99, `wal1` VM rows, logged load throughput |
| 6 | D4 background flush, active buffer at half the share; the checkpoint names X_f and covers only it | `index.hh` (`flush_point`, `freeze`, `flush`, `buffer_limit`, `stage_checkpoint`, attach and replay), host executor; journal `turn_log.h` (`covers` stored, supersession, recovery) | peak memory, 1 KiB fillrandom, tier-none p99, time to durable |
| 7 | D9 sorted runs in the merge policy | `include/strata/core/merge_policy.hh`, `index.hh` merge scheduling | `tenx1`/`twox1` fillseq CPU and bytes written |
| 8 | D10 item 4: reopen at L = 1 → 3 → 2 with power cuts; `kv_strata` stores group ids | `tests/` (logged index reopen), `bench/kv/kv_strata.hh` | loop count (owner) |
| 9 | D10 gap 3: refuse a current-generation log with loop ≥ L; record items 2, 3 (the durable `parked` state) and 5 in journal's `host-layout.md`, through the lead to Detent's lead | journal `host.h`, `docs/design/host-layout.md` | loop count (owner) |
| 10 | D6 one log per device, after Q1, with D6's handoff protocol and test; first `probe-e.sh`'s and `probe-f.sh`'s floors on bare-metal Linux | journal `host.h`, `turn_driver.h`; strata's loop wiring (`turn_seal.hh`, `kv_strata.hh`) | group commit on K logs; loop count |
| 11 | D11 unversioned upsert, after Q4 | `index.hh` (replace path, flush), public API | overwrite CPU at 10× |
| 12 | Profile `wal1` in the Linux and FreeBSD VMs after tasks 2-5 | none (a probe) | `wal1` VM CPU |
| 13 | D10 items 2, 3 and 5 after A6 (Q3): streamed re-partition, durable parking, creation in the turn, a checkpoint asked of quiet groups; then `forward_slice` goes | journal `host.h`, `turn_log.h`, `turn_driver.h`; `index.hh` (the checkpoint request) | loop count; many-indexes open and create |
| 14 | D6's roster record, after task 13 | journal `turn_log.h` | damage scope on one log per device |

Tasks 2, 3, 5, 6, 9, 10, 13 and 14 change journal: coordinated with Detent's lead before landing.

## Checks

Each axis reruns from committed scripts (`bench/kv/probes/rocksdb-parity/`), light, minutes each,
from a strata checkout with `build-rel` (and RocksDB's `kv_rocksdb` from
`bench/engines/rocksdb-11.11` at `~/.scratch/rocksdb-11.11-build`):

| Axis | Check | Pass |
|---|---|---|
| disk, bytes written | `probe-a.sh`: `wbytes.c` counts bytes per file class and zero-fill | log zero-fill 0 after warm-up; log records ≤ 1.1× commands; disk ≤ RocksDB's |
| memory | `probe-a.sh`: `vmmap -summary` at `KV_PAUSE_AFTER_RUN` | `MALLOC_MEDIUM (empty)` under 8 MiB; footprint ≤ RocksDB's |
| group commit, log count | `probe-b.sh`: K 1-8 on 1-8 logs, syncs per op | syncs per op ≤ 1/K + 0.05 on one log; ops/s ≥ RocksDB's at each K |
| log count floor | `probe-e.sh` (`devbarrier.c`), with and without the cohort wait | commits per flush = T on the shared file |
| handoff | `probe-f.sh` (`handoff.cc`), L = 2-8 loops against one | pull at 0.85 or more of one loop at L = 2-8, with 200 µs barriers and the wake-covering bound, and with 20 µs barriers and the spin |
| packed turn window | `d7_model.py` | `all ok`: every admitted record seals in the next turn at N = 1-256, and the `ack=sync` group is never starved |
| CPU | `probe-c.sh`: `sample` of the CPU-loss rows | copies and `madvise` under 10% of `wal`'s busy samples |
| loop count | `probe-d.sh` (`rehome.cc`, faultfs) and task 8's test | every record back at every L; re-partition memory bounded by turns, not live bytes |

The gate's rows stay the verdict: A6's exclusive rerun of `bench/kv/run.py` after the tasks land.
