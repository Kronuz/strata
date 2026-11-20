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
| B | K writers on 1 to 8 logs, syncs per second, and a cohort-wait prototype | PROBE_B |
| C | `sample` profiles of the rows that spend more CPU | PROBE_C |
| D | re-homing groups at another loop count, 10k to 1M groups | The owner asked whether logs survive a different loop count: a disk moved to a server with another
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
   open starts over. Journal's `placement_test` re-partitions one host through 3, 2, 5 and 4 loops, cuts power at every
   operation of each, and checks every record comes back (`test/placement_test.cc:169-205` at `main`).
4. **strata's side.** An index stores its host identity and group in its `turn_group` attribute
   (`index.hh:582-605`), never a loop; the host's `loop(group)` routes it (`turn_seal.hh` `mine()`
   checks it), so a re-partitioned group reopens on its new loop.

**Gaps found:**

1. Re-partition holds every group's live records of every log in one map
   (`std::map<group, vector<TurnRecord>>`, `host.h:482`) until it has written them all: memory and
   open time grow with the live bytes of all logs. PROBE_D_GAP
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

**D10. Logs recoverable under any loop count; idle groups cost nothing to re-home.**

1. Keep journal's protocol (hash placement, one copy into the next generation, the layout as the
   commit point). It is crash-tested and costs one copy of the live bytes; its alternatives lose:
   - *Adopting the old logs read-only* until each group checkpoints in the new generation writes
     nothing at open, but every later recovery reads two generations and must stitch each group's
     sequence across them, and an idle group never checkpoints, so the old logs never go.
   - *One log per host* (no re-partition at all) is D6's question, decided on numbers.
2. Stream the copy: read one old log, append each record to its new loop's open turn, and sync a turn
   when it reaches a quarter segment; memory is then L_new turns, not every live record.
3. Park idle groups in the table: once a group's last record is a checkpoint and a retention window
   has passed without a write, the host moves that record into the group's table entry (its `base`
   and payload, as `close_group` does, `host.h:324-336`) and the log forgets it; its next write
   starts from `base`. Re-homing then copies only groups with records since their last checkpoint,
   at most the logs' retention windows, whatever the number of open indexes.
4. A strata test reopens logged indexes at L = 1 → 3 → 2 with a power cut during the re-partition, and
   the kv harness keeps each index's group id in its directory.

Class: mechanism in journal (items 2-3, coordinated with Detent's lead), tests and harness in strata
(item 4). Items 2-3 are not needed for A6's rows; they are needed before a host holds millions of
indexes (many-indexes workload), so they go to the owner as Q3 with a recommendation.

**The cost of re-homing.** PROBE_D |

The probes ran under light holds on a shared host, so their latencies are not row numbers; their
counts (bytes, syncs, records, pages) are host-independent. Scripts and outputs:
`~/.scratch/xapiand/turn-log-parity-design/` (`wbytes.c`, `probe-*.sh`, `cohort.py`, `rehome.cc`).

## Summary

| Loss (gate report) | Measured cause | Mechanism | Class |
|---|---|---|---|
| Disk up to 39.5× (sync/1), 3.3-7.9× (sync/10, /100), 2.6-3.7× (`wal1`) | the turn log's preallocated 4 MiB segments, current and spare: the index alone is at par or smaller | D1: a log sized by its live bytes | mechanism (journal) |
| Bytes written 36% to 3.2× | zero-fill of every new segment, on the turn; forward copies of live records past `retain_segments`; merges rewriting disjoint runs (`tenx1` fillseq) | D2, D9 | mechanism (journal), tuning, mechanism (merge policy) |
| Peak RSS 2.1-3.4× | freed megabyte buffers that macOS's medium malloc zone keeps resident; then a write buffer that flushes at 79% of the budget, inline | D3, D4 | mechanism (turn path), mechanism (background flush) |
| Group commit 1.39× at K=4, one log | a turn starts on the first writer back; the rest of the cohort waits a whole extra barrier | D5: cohort wait | mechanism |
| Group commit 1.9-2.1× on K logs | one barrier per log per turn | D6 | architecture: owner (Q1) |
| `wal` p99.99 130-193× | one record per group per turn caps `ack=none` at one command per barrier (D107, D108); segment preallocation inline on the turn | D7 | mechanism (journal packing), D1 |
| p99 tier none, 1 KiB fillrandom 0.78× | flushes inline on the writer | D4 | mechanism |
| CPU per op | see D8 | D8 | mechanism |
| `wal1` slower in the VMs | the same `ack=none` cap, at the VMs' slower barriers; CPU in copies and zero-fill (not profiled in the VMs) | D7, D2, D3 | as above |

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
3. Idle groups leave the log (D10 item 3), so forward copies never scale with open indexes.

Beats RocksDB: the log writes its commands once plus framing (RocksDB writes its WAL once too), and
strata's merges already write less than RocksDB's compactions on every YCSB row (gate report, 52-78%
less). Class: tuning (items 1-2: options, `index.hh`'s trigger) and a journal mechanism (item 3).

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
building `logical()` first; forward copies go (D2). Beats RocksDB, whose WAL writer builds each
`WriteBatch` and copies it into its log buffer: strata copies each logged byte once. It also cuts
the turn's CPU (D8). Class: mechanism (`index.hh` `log_state`, `turn_seal.hh`, journal
`framing.h` and `turn_log.h`).

**D4. Background flush with a smaller active buffer.** A flush freezes the active buffer (O(1),
`freeze()`, `index.hh:5056`) and hands the frozen buffer to the host's executor, which builds the
segment while the writer continues into a new active buffer; the owner installs it at its next
publish point, as merges publish today. The active buffer's limit becomes half the index's share,
and the writer waits only when a second freeze would pass the share. The frozen buffer stays
readable (reads already walk `frozen_`, `index.hh:4571`). Beats RocksDB: same two-buffer bound,
and strata's buffer holds the inverted, key-ordered form a flush writes directly. Class: mechanism
(the core's flush path, `index.hh:5087-5110`); phase-3 D11 and D15 already specify the frozen buffer
for it. It closes the 1 KiB fillrandom 0.78× and the tier-none p99 losses too (Tails).

## Group commit

**Measured (probe B, job 261120-072003-b88d0c).** fillrandom sync/1 at 100 B, K writers each on
its own index, one log, two 8 s trials each; syncs are `F_FULLFSYNC` calls on the log files:

| K | strata ops/s | p50 | syncs per op | cohort prototype ops/s | p50 | syncs per op | RocksDB ops/s | p50 |
|---|---|---|---|---|---|---|---|---|
| 1 | 43, 43 | 21-23 ms | 1.01 | 44, 46 | 21-22 ms | 1.01 | 44, 42 | 21-22 ms |
| 2 | 50, 64 | 22-44 ms | 0.67-0.88 | 88, 72 | 21-22 ms | 0.51 | 77, 85 | 21-22 ms |
| 4 | 97, 90 | 42-45 ms | 0.39-0.46 | 177, 183 | 21-22 ms | 0.26 | 139, 153 | 21-22 ms |
| 8 | 228, 241 | 22-42 ms | 0.19-0.21 | 360, 294 | 21 ms | 0.13 | 175, 176 | 42-43 ms |

The device takes about 45 flushes a second in every configuration with one log (36-48 syncs/s). What
differs is how many writers each flush carries. strata's loop writes the next turn as soon as one
record is open: after a turn it calls `turn()` again at once (`turn_seal.hh:281`), finds nothing,
sleeps, and the first writer back wakes it (`turn_seal.hh:156-162`). That writer's record leaves
alone, microseconds ahead of the others, which then wait out its barrier and their own: the p50 of
two barriers at K=2 and K=4. RocksDB's leader forms its group the same way but its followers spin
and yield before sleeping (`write_thread_max_yield_usec`), so more of them make the group.

**D5. The cohort wait.** After a turn that released n waiting writers, the loop waits until n
records have opened, or a bound passes, before it seals the next turn. The prototype
(`cohort.py`: `turn()` counts the writers it releases, `wake()` counts arrivals, `serve()` waits up
to 1 ms) carries every writer in every barrier: 0.26 syncs per op at K=4 and 0.13 at K=8, p50 one
barrier. Against RocksDB that is 1.23× faster at K=4 (180 against 146 ops/s, trial means), 1.87×
at K=8 (327 against 175), a tie at K=1 and K=2, with RocksDB's p50 or better. The bound in the
mechanism is a fraction of the last barrier (1/16: 1.3 ms at 21 ms, a few µs on a fast NVMe flush),
so a writer that does not come back costs the others at most 6% of a barrier. Writers that wait in
`await_sealed` count as released only when their record sealed. Class: mechanism
(`turn_seal.hh` `turn()`, `serve()`, `wake()`), A4's test (per-writer barrier count) with a mutant
that drops the wait. Alternatives that lose: a fixed group-commit timer (RocksDB's
`WriteOptions::sync` has none; a timer delays K=1 by its length) and several turns in flight
(`RFTW0002`, parked; it does not help a device that takes one flush at a time).

## How many logs (owner, 2026-11-20)

The owner asked whether one turn log per event loop is what makes the logs fast. Measured on the
macOS host (probe B, same rows), it is not: the device flushes about 45 times a second, and more
logs on one device only split those flushes.

| K writers | 1 log, today | 1 log, cohort | 2 logs, cohort | 4 logs, cohort | K logs (one per loop), today | RocksDB, one WAL |
|---|---|---|---|---|---|---|
| 2 | 57 / 44 | 80 / 41 | 46 / 47 | | 46 / 47 | 81 / 46 |
| 4 | 94 / 40 | 180 / 46 | 90 / 46 | 74 / 76 | 73 / 74 | 146 / 45 |
| 8 | 235 / 47 | 327 / 43 | 180 / 47 | 142 / 74 | 124 / 127 | 175 / 35 |

Each cell is ops/s / syncs/s, the mean of two trials.

With one log per loop each log syncs alone: syncs per op stay at 1.02, and concurrent
`F_FULLFSYNC` calls coalesce at a longer latency (p50 42-62 ms against 21 ms), so K logs reach 1.6-2.4×
the syncs of one log and fewer operations. One log shared by every index, with D5, is 2.5× faster
than one log per loop at K=4 and 2.6× at K=8. On this host "one log per device" and "one shared log"
are the same configuration: there is one device. D104 chose a log per core on the coalescing
numbers (46 calls/s on one log against 185 on eight); those calls carried one record each, which
the table shows is the wrong currency: commits per second, not syncs per second, is the axis.

PROBE_E

**D6. One barrier per device, appends per loop.** Each loop keeps its own turn log, so appends stay
on their core with no cross-core lock (Track C D11's reason for per-core logs), but the barrier is the
device's: a loop writes its turn and pushes it to the device without a cache flush
(`F_BARRIERFSYNC` on macOS, `sync_file_range(WRITE | WAIT_AFTER)` on Linux over a preallocated,
already-written extent, so no metadata is in flight), then joins the device's group flush, one
`F_FULLFSYNC` or `fdatasync` that the first loop to arrive issues and every loop whose push returned
before it started rides. The loop's turn completes at that flush. With D5 inside each loop, every
writer on the device shares one flush. Beats RocksDB, whose one WAL serializes every writer's append
through one leader; here appends run in parallel and only the flush is shared. Class: architecture
(the barrier leaves the loop; journal's `TurnBarrier` and host), so it is the owner's: Q1.

Alternatives:

- *One log per device, shared by every loop* (probe B's L=1): fastest measured today, but every
  loop's append crosses to the log's owner, a cross-core hand-off per turn, and a host with several
  devices needs one per device anyway. It is the fallback if the device barrier measures no better.
- *One log per loop, as today*: loses 2.5× at K=4 on macOS; on Linux with NVMe, concurrent
  `fdatasync` calls on several files may run in parallel, unmeasured here (no bare-metal Linux,
  D105). The VM's virtio flush is not a device's.

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

**D7. Pack the turn; bound the window in bytes.** Journal calls a sealer again with the room left
until it returns nothing (D108's request to Detent), and strata's bound becomes the unsealed bytes,
one turn's room (half a segment), not one record. The window stays one barrier plus one turn (D107's
contract). With D1's off-turn spares no turn pays a preallocation. Beats RocksDB: its unsynced WAL
window is unbounded in time; strata's is one barrier, and the writer still never waits below
`room / barrier` (32 MiB per 20 ms at the default geometry). Class: mechanism (journal
`turn_driver.h` `seal_turn`, strata `log_state::join` and `seal`).

**p99 tier none (`tenx1` fill and overwrite 1.19-2.08×, `twox1` overwrite 1.64×) and 1 KiB
fillrandom 0.78×.** The writer flushes inline: at 12 MiB the budget is 3 MiB, so the buffer flushes
every few thousand puts on the writer's thread (`index.hh:992-993` calls `flush_point()` inside
`apply`), and every put due during the flush waits; `docs/research/no-stalls.md` measured the same
stall at 80 µs per document. The 1 KiB row's profile (`8c6f59`, document-gate journal) shows it.
D4's background flush removes it from the writer. PROBE_C_P99

**p99 sync/100 (1.13-1.30×).** A sync/100 batch is applied at `durable()` as one transaction, so
the put that waits is the batch's last, and both engines wait one barrier there. The gate's own
note says these p99s move by 2× between trials for both engines (`docs/perf/standalone-turn-log.md`,
"Four axes"). Not a mechanism loss; A6's exclusive rerun decides it, and D4 removes the flush that
can land in a batch.

**Time to durable (`fits1` overwrite 0.79-1.14×).** The settle's commit: an inline flush of the whole
buffer and its manifest barriers. D4 shortens it, since the background flush has written most of the
buffer by the settle.

## CPU per operation

PROBE_C

## A changed loop count (owner, 2026-11-20)

PROBE_D

## Decisions not taken here

## Questions for the owner

## Tasks for turn-log-parity

## Checks
