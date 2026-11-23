# Parity with RocksDB on every row

Every loss in `docs/gates/document-profile.md` (strata `d956c0f4`) gets a measured cause, the
mechanism that beats RocksDB rather than matching it, and its class: tuning, a mechanism change, or
an architecture change. The owner's PLAN D146 sets the bar (turn-log-parity A6): at par or better on
every row and axis. The quest's other three gaps, read-your-writes, the tolerant open and large
transactions (A1-A3), rest on these mechanisms and are designed here too.

Code is cited at strata `d956c0f4` (the `document-gate` head; its `bench/kv` and `include/` are the
code `dd02c63d` timed; `index.hh` is unchanged at the quest head, `turn_seal.hh` cites the quest
head) and journal `cc95ba6` (strata's pin), unless a line names journal `main` (`1222e3b`) or
Detent's idle-groups design (`journal:c3fb805`, `docs/design/idle-groups.md`, "idle-park"). RocksDB
is 11.11 as `bench/engines/rocksdb-11.11/kv_rocksdb.cc` configures it. Every number below is either
from the gate's raw rows (`~/.scratch/document-gate/rows-*-dd02c63d-*`) or from this note's light
probes and models, named by job:

| Probe | What | Job |
|---|---|---|
| A | memory map (`vmmap`, `heap`), bytes written per file class and zero-filled, disk per file; sync1, wal1 and YCSB A rows at small scale | 261120-071503-0c741b |
| B | K writers on 1 to 8 logs, syncs per second, and a cohort-wait prototype | 261120-072003-b88d0c |
| C | `sample` profiles of the rows that spend more CPU | 261120-072004-d21c38 |
| D | re-homing groups at another loop count, 10k to 300k groups | 261120-074846-356a48 |
| E | a file per thread, one shared file, and files per thread with one shared flush, with and without the cohort wait, as a C floor (`devbarrier.c`) | 261120-074947-257624 |
| F | handing records from L loops to one log's owner: under each group's mutex, through per-loop rings, or asked of each loop, against one loop holding every group (`handoff.cc`) | 261120-083804-ec1239 |
| G | journal's `seal_turn` with D7's repeated calls: a per-group bound against one bound per log, the resume rule, and a large transaction's chunks (`d7_model.py`) | 261120-133248-df88a3 |
| B2 | probe B again with phase marks that reach `wbytes.c`: syncs per operation in the run phase alone | 261120-083211-0cbbce |
| H | D4's checkpoint, close and leave: every event sequence of one logged group to depth 11, a power cut after each prefix (`d4_covers_model.py`, extended from the design review's) | 261120-133243-d4f975 |
| I | D4 with two builds in flight: install order and the log wait, depth 12 (`d4_install_order_model.py`, extended from the design review's) | 261120-133244-e59453 |
| J | D6: a wake on a loop that does not own the device log, depth 14 (`d6_wake_model.py`) | 261120-133245-22acfd |
| K | D1: turn sizing on the segment ladder, with journal's arithmetic (`d1_ladder_model.py`) | 261120-133247-a1f4a0 |

The probes ran under light holds on a shared host, so their latencies are not row numbers; their
counts (bytes, syncs, records, pages) are host-independent, and the models (G to K) are exact. The
scripts are in `bench/kv/probes/rocksdb-parity/`, each named in its row.

## Summary

| Loss (gate report) | Measured cause | Mechanism | Class |
|---|---|---|---|
| Disk up to 39.5× (sync/1), 3.3-7.9× (sync/10, /100), 2.6-3.7× (`wal1`) | the turn log's preallocated 4 MiB segments, current and spare: the index alone is at par or smaller | D1: a log sized by its live bytes | mechanism (journal) |
| Bytes written 36% to 3.2× | zero-fill of every new segment, on the turn; forward copies of live records past `retain_segments`; merges rewriting disjoint runs (`tenx1` fillseq) | D2, D9 | mechanism (journal), tuning, mechanism (merge policy) |
| Peak RSS 2.1-3.4× | freed megabyte buffers that macOS's medium malloc zone keeps resident; then a write buffer that flushes at 79% of the budget, inline | D3, D4 | mechanism (turn path), mechanism (background flush) |
| Group commit 1.39× at K=4, one log | a turn starts on the first writer back; the rest of the cohort waits a whole extra barrier | D5: cohort wait | mechanism |
| Group commit 1.9-2.1× on K logs | one flush per log per turn; the device coalesces them only partly | D6: one log, one flush per device | architecture (PLAN D148) |
| `wal` p99.99 130-193× | one record per group per turn caps `ack=none` at one command per barrier (D107, D108); segment preallocation inline on the turn | D7 | mechanism (journal packing), D1 |
| p99 tier none, 1 KiB fillrandom 0.78× | flushes inline on the writer | D4 | mechanism |
| CPU per op: `wal` and the VM rows 1.4-1.6×, `tenx1` fillseq 1.97×, overwrite 1.42× | copies and `madvise` on the turn path (75% of `wal`'s busy CPU); merges rewriting key-disjoint runs; the read before every upsert | D3, D9, D11 | mechanism; D11 adds a public operation (PLAN D148) |
| `wal1` slower in the VMs | the same `ack=none` cap, at the VMs' slower barriers; CPU in copies and zero-fill (not profiled in the VMs) | D7, D2, D3 | as above |

The lead's five suspicions, against the measurements:

1. **The write buffer grows to the whole budget.** Partly: it flushes at 79% of the index's share,
   inline (D4), but most of the RSS loss is freed turn-path buffers macOS keeps resident (D3).
2. **The log and the segment duplicate data, with no compression.** As RocksDB's WAL does (neither
   compresses); the excess is forward copies and zero-fill inside the log (D2), and the segments are
   smaller than RocksDB's SSTs.
3. **Group commit loses at K=4 on one log and on four.** Confirmed: the first writer back starts the
   next turn (D5), and each log pays its own barrier (D6).
4. **The `wal` p99.99 and the 1 KiB 0.78× come from inline flush.** Refuted for `wal` (the
   one-record cap, D7), confirmed for 1 KiB (D4).
5. **VM CPU 1.4-1.6×.** On the host 75% of `wal`'s busy CPU is copies and `madvise` (D3); task 12
   profiles the VMs.

Two owner sections follow the losses, how many logs (D6) and a changed loop count (D10), with A1-A3
before the second.

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

**D1. The log's disk follows its live bytes, and every turn is sized for the segment it writes.**

1. Segments come in a ladder that doubles up to `segment_bytes` from the smallest rung, the
   smallest power of two whose half room admits one framed largest chunk: 4 MiB for 1 MiB chunks at
   32 KiB blocks. A3 makes the chunk the unit, so the rung does not follow the transaction limit.
2. **The room follows the segment (F13).** Journal sizes every turn for the prepared spare, the
   segment a turn opens when it does not fit the current one's remainder, never for
   `options.segment_bytes`, so a turn always fits a fresh segment. That covers `seal_turn`'s whole
   and half rooms, `whole_room()`, `refuse()`, `fitting()`, `fits()` and encode's fresh segment
   (journal `main` `turn_driver.h:502-505`, `:550-575`; `turn_log.h:334`, `:405-407`, `:475-482`).
   strata's D7 counter reads `turn_room(spare / 2)` from the driver. A turn that starts with a join
   waiting for admission has the loop prepare the spare one rung up, off the turn; the room grows
   once it is ready.
3. **A step down never strands admitted bytes.** Once reclamation empties the log, the spare drops
   one rung after each turn with no join waiting, never below the rung whose half room holds the
   admitted bytes.
4. Segments are preallocated off the turn, when the loop is idle or item 2 asks; only an empty log's
   first segment is prepared at open. A reclaimed segment is recycled when it is the size the ladder
   wants, else removed.

Probe K runs journal's arithmetic with eight writers of 1 MiB chunks, from an empty log:

| Rooms | Writers | First throw | Turns to seal: admitted, joined | Rung | Sealed at 21 ms barriers |
|---|---|---|---|---|---|
| from `segment_bytes`, on the ladder | either | turn 1 | | | |
| follow the segment | 0.6 chunks a turn each | none | 1, 2 | 16 MiB | 229 MiB/s, all they write |
| follow the segment | saturating | none | 1, 3 (2 once climbed) | 64 MiB from turn 4 | 1,476 MiB/s |
| the smallest rung | either | none | 1, 9 | 4 MiB | 48 MiB/s |

The throw is encode's "turn exceeds a segment": it fails the driver and fences every index on the
log. Stepping straight down to the smallest rung leaves admitted records waiting 5 turns; the
guarded step down keeps every one at 1, and an idle log returns to one 4 MiB segment.

Test (task 3): from an empty log, eight groups saturate `ack=none`. No turn is refused, every
admitted record seals in the next turn and every record within 3 turns of its join, and once idle
the log is one smallest segment; the mutant with rooms from `segment_bytes` fails. `wal` p99.99
reruns from a fresh log.

Beats RocksDB: the log costs one small segment at rest. RocksDB's WAL is one empty file on macOS; on
Linux it preallocates with `fallocate(FALLOC_FL_KEEP_SIZE)` in blocks of 1.1 × `write_buffer_size`,
capped by the write buffer manager's size (`db_impl_write.cc:3428-3445`, `env/io_posix.cc`): 35.2
MiB at the bench's 32 MiB memtable (N10). The index is smaller than RocksDB's files (58.2 against
101.0 MiB at 1 KiB, 1.32 against 1.63 MiB at 100 B). Class: mechanism in journal (`turn_log.h`,
`turn_driver.h`), coordinated with Detent's lead.

Alternatives that lose:

- Truncating the tail after a checkpoint: the next growth zero-fills again, which is D2's cost.
- `fallocate` or `F_PREALLOCATE` instead of zeros: on ext4 and XFS a write into an unwritten extent
  converts it, which puts metadata in the barrier that `fdatasync` must journal (the reason
  standalone-turn-log D11 item 4 zero-fills).
- A smaller fixed segment: the large rows then copy forward more (D2).
- Rooms bounded by the smallest rung: no turn throws, but each seals one chunk, 48 MiB/s against
  the 229 MiB/s the eight writers produce (probe K).

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
2. Checkpoint inside the window. A logged index's `checkpoint_log_bytes` is the window,
   `retain_segments - 1` segments, divided among the groups that wrote since the last checkpoint,
   with a floor: without one, 10k writing groups on 64 MiB segments checkpoint every 19.7 KiB each,
   and every checkpoint is an index commit with its own syncs (N9). The floor defaults to 1 MiB, one
   forward slice; task 4 sets it after measuring bytes written and checkpoints per second at 10k
   writers on the many-indexes workload for floors of 256 KiB, 1 MiB and 4 MiB.
3. Forward copies stay (idle-park D8, review F11). A quiet open group's newest checkpoint stays
   live, and `reclaim()` frees only segments with no live record (`turn_log.h:352-354`): without
   the copy, it pins its segment until ENOSPC fences every index on the log. Parking does not remove
   the case, since every open strata index holds its group and idle-park parks only released groups
   (its D2); closed and parked groups have no live record. Task 13's per-segment live-record index
   makes a turn's slice walk the oldest segment's live records instead of every live group, whose
   setup grows with the square of the group count (Detent, msg-176).

Test (tasks 3 and 4): an open idle index's checkpoint sits in the oldest segment while another group
writes 10 × `retain_segments` segments of turns. The segment count stays at or below
`retain_segments + spare_segments + 1`; the mutant without the forward copy fails.

Beats RocksDB: the log writes its commands once plus framing (RocksDB writes its WAL once too), and
strata's merges already write less than RocksDB's compactions on every YCSB row (gate report, 52-78%
less). Class: tuning (items 1-2: options, `index.hh`'s trigger); item 3 keeps journal's mechanism,
which task 13 makes cheaper. `tenx1` fillseq's 3.2× is merges, not the log: D9.

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
building `logical()` first; a forward copy is read into the turn buffer, not its own string (D2
item 3). Beats RocksDB, whose WAL writer builds each `WriteBatch` and copies it into its log
buffer: strata copies each logged byte once. It also cuts the turn's CPU (see "CPU per operation").
Class: mechanism (`index.hh` `log_state`, `turn_seal.hh`, journal `framing.h` and `turn_log.h`).

**D4. Background flush with a smaller active buffer.** A flush freezes the active buffer (O(1),
`freeze()`, `index.hh:5056`) and hands it to the host's executor, which builds the segment while the
writer continues into a new active buffer; the owner installs it at its next publish point, as
merges publish today. The active buffer's limit becomes half the index's share, and the writer
waits only when a second freeze would pass the share. The limit triggers a flush and caps no
transaction: a larger one waits until no build is in flight and applies with the whole share, as
today (A3). The frozen buffer stays readable (`frozen_`, `index.hh:4571`). Beats RocksDB: the same
two-buffer bound, and strata's buffer holds the inverted, key-ordered form a flush writes directly.
Class: mechanism (`index.hh:5087-5110`; phase-3 D11 and D15 specify the frozen buffer). It closes
the 1 KiB fillrandom 0.78× and the tier-none p99 losses too (Tails).

Off `commit()`, the flush keeps the orderings `commit()` keeps: it waits for the log through the
applied position, then flushes, commits, and stages the checkpoint there (`index.hh:1256-1273`).
The writer has moved past the freeze by then, so:

1. **The freeze closes the open record (F8).** `freeze()` captures X_f, the position of the frozen
   buffer's last transaction, and closes the group's open record, so X_f ends its record. No freeze
   falls inside a transaction: the writer holds the lease from a transaction's first chunk to its
   last (A3). `covers` = X_f.index − 1 when that record holds a later position also loses nothing
   (probe H), but leaves heads inside records; closing the record makes every head end one, so A2
   compares sequences and a checkpoint supersedes exactly what its segment holds. Cost: one record
   header per flush, about 65 bytes.
2. **An install waits for the log (F9a).** A build's `commit_files` and publish wait until the log
   is durable through X_f.index, as `commit()` waits in `await_log` (`index.hh:1258-1259`, `:3546`);
   a build outlasts a barrier, so the wait is nearly always over. Without it a view at V reads
   crash-removable writes through the segment, and a power cut leaves the head past the log
   (`log_lost`).
3. **Builds install in freeze order (F9b).** A newer build that finishes first waits for the older
   one's install; otherwise its head passes the older buffer, whose transactions replay then skips.
4. **The checkpoint covers X_f's record.** It is staged once its install's `commit_files` is durable
   (I2), at the group's next sequence N, and supersedes the records through X_f.index (its `covers`)
   and the group's earlier checkpoints; batches X_f.index + 1 to N − 1 stay live. Journal today
   drops all of a group's earlier records at a checkpoint (`turn_log.h:321`) and refuses one whose
   `covers` is below its last sequence (`:257`), so task 6 stores `covers` in the checkpoint record
   (a log format change, pre-release), supersedes through it, and keeps the records after it.
   strata's attach reads the newest checkpoint wherever it sits; replay skips it (`index.hh:3375`,
   `:3461`).
5. **Pending deletes move to the install (N14).** The freeze stops calling `apply_pending()`, which
   waits on the writer's thread for the log whenever the buffer holds pending deletes
   (`index.hh:5056-5068`): a barrier per logged overwrite freeze. A frozen buffer keeps its pending
   deletes, reads mask every frozen buffer's set (`pending_index()`, `segments()`: `:3114`, `:1495`),
   a merge carries them through its ordinal map (`carry_pending`, `:4111`), and the install applies
   them after item 2's wait.
6. **`leave()` drains, and the close checks (F10).** `leave()` (`index.hh:3437-3445`), and so every
   close and park, waits until no build is in flight and every frozen buffer is installed, then
   freezes and installs the active buffer and closes with a checkpoint that is the group's only live
   record. `close_group` supersedes everything through its base, so a partial checkpoint would lose
   the batches after its `covers` silently. Task 6's journal change makes `close_group`, and `park`
   through the same `close_into_table`, refuse a checkpoint with a batch above its `covers`: the
   condition idle-park D2 checks before it parks. The review's form, `covers` = sequence − 1, also
   refuses closes that lose nothing: a reopened group's restaged checkpoint sits at base + 2
   covering base, because base + 1 is the retired floor and is never written; and a leave whose
   final checkpoint follows a build's checkpoint covers the batch below both, two below its own
   sequence.

Probe H explores every sequence of joins, two-chunk transactions, turns, freezes, installs, closes,
leaves and reopens of one group to depth 11, with a power cut after each prefix; probe I runs two
builds in flight to depth 12. "Refused wrongly" counts closes refused although their checkpoint is
the group's only live record; a blank is a case the probe does not model.

| Variant | States | Silent loss | Torn | `log_lost` | Exposed past V | Refused wrongly | First failure |
|---|---|---|---|---|---|---|---|
| H: the design (items 1, 2, 6) | 117,029 | 0 | 0 | 0 | 0 | 0 | |
| H: `covers` = X_f.index, record left open | 131,748 | 5,547 | 1,208 | 0 | 0 | 0 | `JFJTIT`: a write joins X_f's record after the freeze; `JFCTJIT`: a chunk does |
| H: `covers` one back when the record is shared | 131,379 | 0 | 0 | 0 | 0 | 0 | |
| H: the close takes any checkpoint | 117,715 | 328 | 0 | 0 | 0 | 0 | `JTFJITL`: a batch after `covers` |
| H: the close needs `covers` = sequence − 1 | 103,589 | 0 | 0 | 0 | 0 | 709; `leave()` refused in 30,013 | `JDRT`: closed, reopened, closed; `JFJD` |
| H: no wait before the install | 161,086 | 0 | 0 | 20,030 | 20,030 | 0 | `JFI` |
| I: the design (items 2, 3) | 18,059 | 0 | | 0 | 0 | | |
| I: a newer build installs first | 21,427 | 1,916 | | 0 | 0 | | `JFJTFO` |
| I: no wait before the install | 36,060 | 0 | | 10,329 | 10,329 | | `JFI` |

Tests (task 6), each with a power cut where named:

- F8: a write joins the open record between a freeze and that record's seal; power is cut after the
  checkpoint's barrier. The write recovers, and so does a two-chunk transaction across the freeze's
  record. The mutant that leaves the record open fails both.
- F9: a view at V during a build with `ack=none` writes frozen sees none of them before their
  barrier. Two builds run with the newer finishing first; power is cut after its install, and no
  acknowledged write is lost. The mutants that install before the wait, or in completion order,
  fail.
- F10: a close with batches after the newest checkpoint's `covers` drains or refuses, and the reopen
  holds every write; a reopened group that wrote nothing closes. The mutant without the refusal
  fails the first, and the sequence − 1 form fails the second.
- An `ack=sync` write lands in the active buffer during a build; power is cut after the install and
  after the checkpoint's barrier. The write recovers; the mutant staging `cur_.applied` fails.

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

The device takes about 45 flushes a second in every configuration with one log (36-48 syncs/s); what
differs is how many writers each flush carries. strata's loop writes the next turn as soon as one
record is open: after a turn it calls `turn()` again at once (`turn_seal.hh:281`), finds nothing,
sleeps, and the first writer back wakes it (`turn_seal.hh:156-162`). That writer's record leaves
alone, microseconds ahead of the others, which then wait out its barrier and their own: the p50 of
two barriers at K=2 and K=4. RocksDB's leader carries more of the cohort; why is not measured here,
since its followers block through a 21 ms barrier as strata's writers do (`AwaitState` spins 200
times and yields for at most 100 µs, `write_thread.cc:76`, `:144-189`). The rows stand (N11).

**D5. The cohort wait (task 1, landed).** After a turn that released n writers, the loop waits
until n of them have joined a record, or a bound passes, before it seals the next turn. The bound is
1/16 of the last barrier (1.3 ms at 21 ms), measured from submission to completion without the
wait, so it does not feed back into itself, and a writer that does not come back costs the others
at most 6% of a barrier. The prototype (`cohort.py`) carried every writer in every barrier: 0.26
syncs per op at K=4 and 0.13 at K=8, p50 one barrier, 1.23× faster than RocksDB at K=4 (180 against
146 ops/s, trial means) and 1.87× at K=8 (327 against 175), a tie at K=1 and K=2. Both counts are
per writer (N3, task `group-commit`): n counts the writers the turn released from `await()` and
`await_sealed()`, and an arrival is such a writer's next join, whether it opened the record or
joined one another writer opened (`turn_sealer::joined()`, through a thread-local mark the
released wait leaves). Counting groups started the turn on the first writer back to a shared
index, so its other writers rode the barrier after; counting every opened record let a writer
outside the cohort end the wait. In the test's counts (24 commits per writer over 64 ms barriers),
two writers per index on one log rode a second barrier on 8 of 168 commits at K=8 and 9 of 84 at
K=4 with the group count, eight writers on two indexes on 71 of 168 (40 turns for 24 rounds), and
two writers per index on two logs on 26 of 84 (68 turns); counted per writer, none does, in 26,
26, 26 and 51 turns. Under D6 both counts span every loop on the log (D6's handoff
item 5). Test: `test/test_barrier_cohort.cc` counts barriers per writer at K writers on K indexes
and on K/2 (two writers a group), on one log and on a log per loop, and for writers that arrive
during a barrier (task `group-commit`'s journal names the jobs). Alternatives that
lose: a fixed group-commit timer, which delays K=1 by its length, and several turns in flight
(`RFTW0002`, parked), which a device that flushes one at a time does not reward.

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
`F_FULLFSYNC` calls coalesce at a longer latency (p50 42-62 ms against 21 ms), so K logs reach
1.6-2.4× the syncs of one log and fewer operations. One log shared by every index, with D5, is 2.5×
faster than one log per loop at K=4 and 2.6× at K=8. On this host "one log per device" and "one
shared log" are the same configuration: there is one device. D104 chose a log per core on the
coalescing numbers (46 calls/s on one log against 185 on eight); those calls carried one record
each, the wrong currency: commits per second, not syncs per second, is the axis.

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
flush, never free. The third column's T=1 cell flushes in 5 ms, which suggests `F_FULLFSYNC`
returns early when `F_BARRIERFSYNC` left nothing dirty: a durability question, not a speedup.

**D6. One flush per device, carried by one log per Host directory, one Host per device.** All of a
Host's loops append to one turn log, owned by one loop; the owner's turn takes every loop's records
(the handoff below), and D5's cohort wait makes every writer on the device ride one flush. Two Host
directories on one device still split its flushes, so a deployment runs one Host per device.
Measured on this host it is the fastest shape in both probes (strata K=8: 327 ops/s against 124 for
a log per loop, 2.6×; the C floor: 369 against 111, 3.3×), and it beats RocksDB's one WAL by
1.23-1.87× at K=4-8 (Group commit). It also removes the loop count from the files: groups are
placed on loops in memory, and a changed loop count re-homes them with no copy (D10). Class:
architecture (Track C D11's per-core logs and D104 change); the owner took it (PLAN D148, Q1 (a)).

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
   takes it, and push seals at the join; so the lock wins.
2. Asking each loop loses in every row: its collect is a round trip to every loop, 21-28 µs a turn
   at L=8, and a busy loop would stall the whole device's turn.
3. The cost is the wake: a loop resumes 9-17 µs after the owner's notify (mean, under light holds),
   the order of a 20 µs barrier. A bound of 1/16 of the barrier is shorter than that below about
   250 µs and misses half the cohort (0.40-0.45 at 20 µs). A bound covering the wake recovers
   0.85-0.92 at 200 µs but 0.51-0.62 at 20 µs. Loops that spin through the barrier recover 0.91-0.95
   at 20 µs, at up to 1.6 times the CPU per commit of sleeping loops (7.0 against 4.3 µs at L=8); a
   spin shorter than the barrier only deepens the wake (0.50-0.57 at 200 µs).
4. At macOS's 21 ms `F_FULLFSYNC` a 15 µs wake is 0.07% of a turn. On a fast device it matters, and
   one log per loop pays none of it, so task 10's Linux floor runs `probe-f.sh` with `probe-e.sh`,
   and D6 lands there only if it still wins.

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
   covering the barrier.
3. **Completion: one wake per loop per turn.** `completed()` resumes each waiter on its home loop,
   as `Wait` already posts to `waiter.home` (`turn_driver.h:424-433`), batched per loop; watchers
   (strata's `durable_through`) run on the owner's thread under their group's lock.
4. **Failure: every index on the device.** A failed write or barrier fails the log
   (`turn_driver.h:259-261`), and every operation staged in or after that turn fails on its home
   loop with `outcome_unknown`: D9's fail-stop at the scope of the device whose flush failed.
5. **The cohort across loops.** D5's n and arrivals count every loop's groups and records, two
   counters per log, each on its own cache line. The bound is the larger of 1/16 of the last barrier
   and three times the log's recent fan-out wake. A loop whose last barrier took under 50 µs spins
   until the turn completes, at most 50 µs, before it sleeps.
6. **D7's admission counter** is one more atomic per log, on its own cache line.
7. **Transitions before records (F12).** Idle-park makes a create or a wake durable at the group's
   first turn: `before_turn` commits the loop's dirty table shards before the turn is staged
   (idle-groups D5). A shard belongs to its loop's thread, with no lock
   (`docs/mechanisms/host-ranges.md` rules 1-5, journal `1222e3b`), so the owner cannot commit
   another loop's. A pull that seals a woken group's records before its loop commits the wake
   leaves them durable under a `closed` entry, which recovery drops silently: probe J finds
   7,981 of 16,144 states losing an acknowledged batch (first `WJS`). The rule is idle-park's own
   ("each loop commits its own dirty slots before it hands its sealed records to the owner's turn",
   its assumptions about this quest), and a gate enforces it under pull:
   - `create_group` and `reopen_group` gate the group under the driver's mutex, and the owner's
     turn skips a gated group's records and sealer.
   - The group's loop commits its dirty shards in one batch at its next idle point, as
     `before_turn` does (the `.pending` files, one `sync_files`, the renames, one directory sync),
     then clears their gates under the mutex and wakes the owner. On the owner's own loop this is
     `before_turn` itself.
   - A woken group pays one shard commit before its first turn, as it does under idle-park, and
     nothing else pays: probe J finds 0 of 4,627 states losing and no owner turn paying a commit.
   - `Host::park(i)` runs on loop i's thread (idle-groups D10): it reads the device log's index of
     its groups under the driver's mutex, commits the shard outside it, and forgets the groups
     under it.

   The owner committing every loop's shards in `before_turn` under a table lock also loses nothing,
   but it breaks host-ranges' one-thread ownership and puts two syncs in the device's turn whenever
   any loop transitions (probe J: 0 of 17,137 states losing, 1,715 owner turns paying a commit).

Test (task 10): K loops (2, 4, 8) share one device log, under TSan. A mixed `ack=sync` and
`ack=none` run creates, closes and wakes groups on loops other than the owner's, with a power cut at
every operation (faultfs). Every acknowledged record recovers, none recovers under a `closed`
entry, and a failed barrier fences every index on the device; the mutant without the gate fails.

### D6's damage scope

Recovery loses every group that wrote nothing after a damaged block whose manifest does not read
back, and on any damage every open group with no records (`turn_log.h:568`, `:604`). With one log
per device that is every group on the device, L times wider than with a log per loop. A2 keeps
open every group whose head holds every record the damage could have taken, and parked groups are
closed entries, outside both rules (idle-park D1, D7). Per-loop manifests would not narrow the
first: the wide case is a manifest that does not read back, which splitting its entries by loop
does not change, and manifest files per loop add a write per loop to every turn. A roster record
would: at each segment's first turn the log writes every live group's last sequence, 16 bytes a
group (160 KiB at 10k groups, 0.24% of a 64 MiB segment; 1.6 MiB at 100k), so damage before a
roster costs only the groups whose records were in the damaged span. Damage after the newest
roster stays wide. Task 14, after task 13, a log format change; A6 does not need it.

## Tails

**`wal` p99.99, 130-193×.** Not a flush: the `wal1` rows' buffers never reach a flush or a
checkpoint (11-22 MiB of commands against a 50.8 MiB buffer limit and a 64 MiB checkpoint), and
probe A's `wal` runs wrote no index segment until settle. The cause is the `ack=none` cap that
D107 and D108 set: journal calls a group's sealer once per turn and it returns one record
(`index.hh:483`, `turn_driver.h:447-506`), so a writer faster than one 1 MiB command per barrier
blocks in `await_sealed` until the loop seals (`index.hh:3522-3540`). At 100 B values the writer
makes about 30 MB/s, a 1 MiB record every 35 ms, against a 20-36 ms barrier (standalone-turn-log's
M4 floor: p50 20.2 ms, p99 36.2 ms): the ten slowest of 100k puts are those waits (p99.99 9.5-17.2
ms). Slower barriers widen it: a turn that rolls into a new segment also preallocates it inline,
4 MiB of zeros, a full sync and a directory sync on the turn (`turn_log.h:382-387`; probe A counts
84 directory syncs in one 1 KiB load), and the VMs' barriers are slower still, which is the `wal1`
VM loss.

**D7. Pack the turn; bound the window per log, in bytes.** Journal's half is on `main` (6187a32,
N8): `seal_turn` calls a sealer again with the room left while it returns records, and the next turn
resumes after a drained group (`turn_driver.h:550-621`). Task 0 moves strata's pin there. strata's
half is the bound: one admission counter per log holds the cost (`record_cost`, framing included)
of every record joined or staged and not yet sealed, at most one turn's half room (`turn_room(spare
/ 2)`, D1 item 2), which every rung sizes for a largest chunk. A join that would pass it waits,
neither admitted nor applied, in arrival order, until a seal frees room. Every admitted record then
fits the next turn, so the window stays one barrier plus one turn (D107's contract) at any group
count. Under D6 the counter is one atomic per device, and D1's off-turn spares keep preallocation
off every turn.

Probe G: a bound per group lets N saturating groups hold N rooms, and the oldest record waits N − 1
turns (7 at N=8 and 31 at N=32, the review's numbers). With the bound per log every admitted record
seals in the next turn at N = 1 to 256, each group sealing its share (8,000 records each at N=2, 62
at N=256, of 16,000 per run). An `ack=sync` record waits in arrival order behind the writers ahead
of it, at most ⌈W·c / room⌉ turns for W writers of cost c (4 behind 31 bulk writers of an eighth of
the room), then seals in the next. At strata's pin a group bounded past the room starves the groups
after it, because the next turn resumes at a group that returned nothing under less than the whole
room (`turn_driver.h:492` at `cc95ba6`; probe G: 0 of 2,000 records sealed); `main`'s rule seals
all 2,000, each within 1 turn.

Beats RocksDB: its unsynced WAL window is unbounded in time; strata's is one barrier, and the writer
never waits below `room / barrier` (32 MiB per 20 ms at the default geometry). Class: mechanism
(strata `log_state::join` and `seal`). Test (task 5): 8 groups saturating `ack=none` and one
`ack=sync` group; every record seals in the turn after its admission and within 2 turns of its join
when the log has room, counted per record, and the mutant with the per-group bound fails.

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
   counts overlapping runs, not segments. Gets and scans skip a segment by its key range
   (`index.hh:2506`, `keys.hh:211`), but a get walks the segments to skip them where RocksDB
   binary-searches a level's files (`FindFile`), and disjoint runs stay unmerged (N13). So task 7
   keeps the settled runs' key ranges in one sorted array per index, which a get binary-searches,
   and caps the run count by per-segment fixed costs, merging the smallest key-adjacent runs past
   it. Beats RocksDB: strata's keys section and filter are per segment, so a moved segment needs no
   rewrite, and random keys still merge by size. Class: mechanism (`merge_policy.hh`, `index.hh`'s
   merge scheduling and `lookup_key`). It closes `tenx1` and `twox1` fillseq's bytes written (3.2×
   and 2.5×) too.
3. **overwrite at 10× (1.42×).** strata reads before it writes: a replace looks up the key's live
   copy in every buffer and segment, newest first (`index.hh:4567-4585`), because the operation
   returns the document's version and keeps its docid; RocksDB writes blindly and resolves at
   compaction. The rest is segment building, which D4 moves off the writer and D9 reduces.
   **D11. Resolve unversioned upserts in the build.** An upsert whose caller asks for no version
   and no condition records only the key in the buffer. The background build (D4) resolves every
   such key against the segments in one sorted pass (the buffer's keys are ordered, and so are each
   segment's) and records the hits as the frozen buffer's pending deletes (D4 item 5), not as delete
   bits, so a merge that replaces a segment during the build carries them through its ordinal map,
   and the install applies them under the lease (N12). Beats RocksDB: the delete is resolved once,
   at flush, and compaction never carries the old value forward. Class: mechanism and a public API
   addition (an unversioned upsert operation), taken by the owner (PLAN D148, Q4). Test (task 11):
   an upsert during a merge of its key's segment leaves one live copy.
4. **sync/1 fillrandom (1.15×).** Not reproduced: probe B's K=1 rows spent 228-275 µs per
   operation against RocksDB's 274, with the same throughput and p50. Most of either engine's CPU
   there is the kernel's flush path. A6's exclusive rerun decides the row; no mechanism here.

## Read-your-writes, tolerant open and large transactions (A1-A3)

The quest's other three gaps rest on D4, D6 and D7 (review F14); `standalone-turn-log.md` Q3-Q5
lists their options, and these are the mechanisms its design-note task records.

**A1. Read-your-writes.** The lease holder reads at applied: its `get`, `find_docid` and scans walk
the active buffer, the frozen buffers and the segments with no V cut (`index.hh:2029-2049`), so the
writer sees its `ack=none` write at once, as kv's gets do (`kv_strata.hh:113-114`). Every other
reader reads a view at V, which cuts the active and frozen buffers at V. D4 item 2 keeps views
honest: a segment installs only once the log is durable through its X_f, so no view reads a
crash-removable write through a segment (probe H: 0 states exposed with the wait, 20,030 without).
Under D6, V is `durable_through`'s, stored on the owner's thread with release and read on any loop
with acquire.

Contract: a write is visible to its lease holder when its call returns, and to views once V passes
it; a view never reads past V. Readers off the owner's thread are Q5.

Test (task `ryw`): an `ack=none` write runs with the barrier held (faultfs). The owner's get returns
it and a view opened after the write does not; after the barrier both do; a build forced during the
held barrier changes neither. The mutant whose owner read is cut at V fails the first check, and
the one that installs before the log wait fails the last.

**A2. Tolerant open.** A damaged block names its groups through its manifest. Recovery loses each of
them wholesale, and with no readable manifest every group that wrote nothing after the damage
(journal `main` `turn_log.h:562-568`, its `named` and `unknown` sets). A group whose head already
holds every record the damage could have taken lost nothing, so:

1. Journal's recovery treats a damaged record that its group's newest kept checkpoint covers as
   superseded, as it treats one at or below the table's base (task 6 stores `covers`). It reports
   each other damaged group with a bound, the highest sequence the damage could hold, taken from the
   damaged block's manifest when it reads back, from the group's first fresh record after the gap
   (the copy flag, task 3, tells a forward copy apart), or from a later roster (task 14). With no
   source the group stays lost. `TurnRecovery` reports these bounds, where today it reports only
   `lost`: a journal change.
2. strata opens a damaged group at its head H when the bound is at most X_H.index, replaying the
   records after it, and otherwise fails `log_lost`. D4 item 1 makes a head end its record, so the
   comparison is exact. "Covered" never means "the newest record is a checkpoint": the head can be
   past the newest durable checkpoint (I2 stages it after the head's commit), and on the shared log
   a checkpoint covers only through its `covers`.

Contract: after damage an index opens only if every record it could have lost is in its head;
otherwise it fails `log_lost`, never a silent loss.

Test (task `tolerant-open`): one log holds groups of two loops (one loop's log until task 10). One
damaged block holds records of three groups: C (its head is past them), U (its bound is above its
head) and W (no manifest, no later record). C opens with every record, and U and W fail `log_lost`.
The mutant that opens every damaged group fails U, and today's rule fails C.

Limit (owner, 2026-11-23): the bound survives only an open that completes. A power cut after the
first open writes its open record but before the host stores the bound forgets the damage (journal
`docs/mechanisms/lost-bound.md`, Limits). Quest `journal-bound-first-open` closes it.

**A3. Large transactions.** A logged transaction must fit one record today (`command_bytes`,
`index.hh:785`; `kv_strata` sets 1 MiB and applies a batch before it would pass it,
`kv_strata.hh:252-254`). The transaction streams instead:

1. **Admission.** A transaction is admitted up to the unlogged limit, what the write buffer's share
   holds; D4's active limit triggers a flush, not a refusal.
2. **Chunks.** Its encoding streams as records of the group of at most `chunk_bytes` (today's
   `command_bytes`), and batch payload version 2 carries a part byte per entry: whole, first,
   middle or last. Stage 1 checks and accepts the transaction under the lease before its first
   chunk joins (apply first, then log: standalone-turn-log D2), so a run needs no abort record.
3. **Position.** A chunk joins without moving applied. The last chunk joins at P, and applied moves
   to P only then, so the transaction becomes visible whole. The lease spans every chunk, so no
   freeze, install or checkpoint lands inside a run, and D4's `covers` never passes an unapplied
   chunk (probe H: 0 torn states; leaving the record open tears 1,208).
4. **Replay.** Replay buffers a run until its last chunk. A run with no last chunk at the log's end
   was never acknowledged and is dropped; a middle or last chunk with no run before it is
   `log_lost`.
5. **Stall.** Chunks join one at a time, each once the previous one is admitted, and take from D7's
   counter in arrival order, so another writer waits behind at most one chunk: its record seals
   within 2 turns of its arrival in every row of probe G's row 6. The transaction takes ⌈T / room⌉
   turns or more, the room being the counter's: four rooms seal in 5 turns in eighth-room chunks
   beside one `ack=sync` writer, in 7 in whole-room chunks (each enters only an empty counter), and
   in 32 in eighth-room chunks beside 7 bulk writers. The ladder climbs under a transaction as under
   any saturating writer (probe K).
6. **Memory.** A transaction of T bytes costs T, its applied form in the buffer, and at most one room
   of unsealed chunks, since the encoding streams into the chunk records. Replay holds a run and its
   applied form: 2T.

Contract: a transaction up to the unlogged limit is all or nothing across a crash, and a kv batch
past 1 MiB is one such transaction.

Test (task `large-tx`): a power-cut sweep at every operation (faultfs), with `chunk_bytes` lowered
to 4 KiB, over a transaction above 64 MiB and a kv batch above 1 MiB; after each cut the transaction
is all or nothing. The mutants that apply per chunk, leave the freeze's record open, or keep the
limit at `command_bytes` fail.

## A changed loop count (owner, 2026-11-20)

The owner asked whether logs survive a different loop count: a disk moved to a server with another
CPU count, or a changed configuration. They do today, by a mechanism that lives in journal
(`docs/design/host-layout.md`, "Placement and re-partition"; `include/journal/host.h`), not in
strata's docs:

- A group's loop is its slot's, `(slot × L) >> 12` of 4,096 hashed slots (`host.h:81-87`), and is
  never stored. The group table is sharded by slot, so it never moves.
- The `layout` file holds L and the log generation. Another L re-partitions before serving: it reads
  every log of the stored generation and writes each group's live records into its new loop's log
  of the next generation. The layout's replace is the commit point (`host.h:447-448`, `:481-520`).
  Journal's `placement_test` cuts power at every operation of re-partitions through 3, 2, 5 and 4
  loops and checks that every record comes back.
- strata stores a group, never a loop (`index.hh:582-605`), and the host's `loop(group)` routes it.

**Gaps found, and what closes each:**

1. Re-partition holds every live record of every log in one map (`host.h:482`) until it has written
   them all: +30 MiB for 100k idle groups (probe D), at worst every log's retained bytes (8 GiB at
   32 loops of 64 MiB segments). Idle-park D6 streams it one old log at a time, and D6 here removes
   it on a loop-count change.
2. Every open group keeps its newest checkpoint live until it closes, so live bytes, forward copies
   and the re-partition's copy grow with open indexes, not with writes. Closed and parked groups
   leave the log (idle-park D1-D3); an open idle index keeps one forwarded checkpoint (D2 item 3),
   whose turn cost task 13 bounds.
3. A current-generation log whose loop is L or more is neither read nor removed
   (`host.h:450-454`): task 9 refuses it as corruption.
4. Closed by task 8 (D10 item 2): `test/test_reopen_loops.cc` reopens four logged indexes at
   L = 1, 3, then 2, with a power cut before each reopen and at every mutation inside it, and
   `kv_strata` stores each index's group in `<dir>/<i>/kv-group`. Production never derives a
   group id from L.
5. Creating a group rewrites its slot's shard and pays two barriers: three sync calls per create in
   probe D, 30,023 for 10k, 11 hours for a million created one at a time. Idle-park D5 makes
   creates and wakes join the turn, and D6's handoff item 7 keeps that on one log per device.
6. Every open starts a new segment per loop, and the old ones stay while idle groups' checkpoints
   keep them live: probe D's directory grew by L segments per open, 2 GiB per restart at the default
   geometry and 32 loops. Parked groups pin none, an open idle index's checkpoint moves forward with
   the next turns (D2 item 3), and D1 makes each new segment the smallest rung.

**D10. Logs recoverable under any loop count; idle groups cost nothing to re-home.**

1. With D6, logs belong to devices, not loops: a changed loop count re-homes groups in memory
   (placement by hash at open, as today) and copies nothing; only a changed device count
   re-partitions, which a disk moved to another server does not cause. *Adopting the old logs
   read-only* until each group checkpoints anew loses: every later recovery reads two generations
   and stitches each group's sequence across them, and an idle group never checkpoints, so the old
   logs never go.
2. A strata test reopens logged indexes at L = 1 → 3 → 2 with a power cut during the re-partition,
   and the kv harness keeps each index's group id in its directory (task 8).
3. Parked is closed (idle-park D1; review F11). strata parks an idle index by closing it through
   `leave()`, after D4 item 6's drain, and wakes it by reopening. Journal's `Host::park` parks
   released groups, and strata leaves none of its groups released. There is no second table state.

Class: item 1 is D6's, items 2-3 are strata's tests, harness and policy; the streamed re-partition,
parking, and creation in the turn are idle-park's journal mechanisms, consumed here.

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
recover its logs at every open, and 4.3 s more to re-partition; 10M pay 70 s and 5.5 GiB. Every
cost scales with idle groups, which wrote nothing: closing idle indexes (item 3) takes it to the
groups with records in the retention window, and D6 takes the re-partition to zero. Creating the
groups costs more than re-homing them: 94 s of CPU and 900k sync calls for 300k (gap 5).

## Decisions not taken here

- **Compressing log records.** RocksDB's bench WAL is uncompressed too, and the log's excess is
  copies and zeros (D2), not entropy. Worth a probe after D2 lands, on the 1 KiB rows only.
- **Changing the harness's budget split** (giving strata RocksDB's `memory / 8` buffer). That matches
  a setting rather than beating a mechanism; D4 bounds the active buffer at half the share instead.

## Questions for the owner

PLAN D148 ruled Q1-Q4: Q1 (a), one log per device (D6); Q2, the gate keeps the higher of RSS and
footprint, with the footprint as a column; Q3, idle groups leave the log, now through idle-park
(D10 item 3); Q4, the unversioned upsert is built (task 11). One question is open:

- **Q5. Read-your-writes off the owner's thread (A1).** The lease holder reads its writes at
  applied, and a view reads at V. A client whose reads go through views, as Xapiand's do (Xapiand
  `src/engine/strata/shard.cc:241-245`), sees its `ack=none` write only after the barrier. Options:
  1. (a) Route a session's reads after its write to the owner, as kv already reads: no API, one hop
     to the owner's loop per read.
  2. (b) A view at the position the write returned, reading the frozen buffers and the owner's
     active buffer at it: a new public API, and the active buffer must take readers off its thread.
  3. (c) Wait for V to pass the write's position: a barrier per read.

  Recommendation and default: (a), which meets A1 under the quest's "No new public API". Take (b)
  only if the hop measures as a loss against RocksDB, whose readers read the memtable on their own
  threads.

## Tasks for turn-log-parity

In order; each is one landing with its test and the rows it moves. The quest's `ryw` and
`tolerant-open` follow task 6, and `large-tx` follows tasks 5 and 6: all three rely on D4's
orderings, and `large-tx` also on D7's counter.

| # | Task | Files | Rows |
|---|---|---|---|
| 0 | Move strata's journal pin from `cc95ba6` to `main` (`1222e3b`), with the suite. strata meets `main`'s contracts: repeat sealer calls (`turn_records = 1` keeps one record a turn until task 5); the retired floor at the close's sequence + 1, which attach's restage at `max(base, retired) + 1` meets; a failed close leaves the group in doubt until the host reopens; a hold needs a loop; an oversize record is refused. At idle-park's pin, create and reopen become durable at the group's first turn, and `turn_seal.hh`'s "durable when it returns" changes with it | `CMakeLists.txt` (the pin), `include/strata/core/turn_seal.hh` | none: contracts |
| 1 | D5 cohort wait (landed) | `include/strata/core/turn_seal.hh`, `test/test_barrier_cohort.cc` | group commit K=2-8 on one log |
| 2 | D3 the turn path owns its buffers: per-log encode buffer, records encoded in place, framing straight into the turn | `index.hh` (`log_state`), `turn_seal.hh`; journal `framing.h`, `turn_log.h` | peak RSS on logged rows, `wal` CPU, VM `wal1` |
| 3 | D1 segment ladder, the room following the segment, off-turn preparation; D2 item 1 spare pool and item 3's test; the copy flag and the per-fragment checksum (quest file) | journal `turn_log.h`, `turn_driver.h`, `framing.h`; strata `index.hh` (D7's counter reads the room), `bench/kv/kv_strata.hh` geometry | disk on every logged row, bytes written, `wal` p99.99 |
| 4 | D2 item 2 checkpoint inside the window, its floor measured at 10k writers; D2 item 3's test again | `index.hh` (`checkpoint_log_bytes`, `log_due`) | bytes written (YCSB logged loads) |
| 5 | D7 one admission bound per log | `index.hh` (`log_state::join`, `seal`), `turn_seal.hh` | `wal` p99.99, `wal1` VM rows, logged load throughput |
| 6 | D4 background flush, items 1-6 and their tests; journal stores `covers`, supersedes through it, and refuses a close or park whose checkpoint is not the group's only live record | `index.hh` (`flush_point`, `freeze`, `flush`, `buffer_limit`, `stage_checkpoint`, `leave`, `pending_index`, `segments`, attach and replay), host executor; journal `turn_log.h`, `host.h` | peak memory, 1 KiB fillrandom, tier-none p99, time to durable |
| 7 | D9 sorted runs, with the runs' range index and cap (N13) | `merge_policy.hh`, `index.hh` (merge scheduling, `lookup_key`) | `tenx1`/`twox1` fillseq CPU and bytes written |
| 8 | D10 item 2: reopen at L = 1 → 3 → 2 with power cuts; `kv_strata` stores group ids | `test/test_reopen*`, `bench/kv/kv_strata.hh` | loop count |
| 9 | D10 gap 3: refuse a current-generation log with loop ≥ L; refuse a `block_bytes` that is neither a multiple nor a divisor of 512 (quest file) | journal `host.h`, `turn_log.h` | loop count |
| 10 | D6 one log per device, with D6's handoff protocol (items 1-7) and test; first `probe-e.sh`'s and `probe-f.sh`'s floors on bare-metal Linux | journal `host.h`, `turn_driver.h`; strata's loop wiring (`turn_seal.hh`, `kv_strata.hh`) | group commit on K logs; loop count |
| 11 | D11 unversioned upsert, resolved into pending deletes | `index.hh` (replace path, build), public API | overwrite CPU at 10× |
| 12 | Profile `wal1` in the Linux and FreeBSD VMs after tasks 2-5 | none (a probe) | `wal1` VM CPU |
| 13 | Journal indexes live records by segment, so `forward_slice` walks the oldest segment's live records; the turn cost measured at 1k, 100k and 1M groups, most parked (quest file) | journal `turn_log.h` | many-indexes turn cost |
| 14 | D6's roster record, after task 13 | journal `turn_log.h` | damage scope on one log per device |

Tasks 2, 3, 6, 9, 10, 13 and 14 change journal, and so does `tolerant-open`. Each diff goes to
Detent's lead before it lands, with its journal test:

- Task 3: the room follows the segment. From an empty log, saturating groups; no turn throws, and
  every admitted record seals in the next turn.
- Task 6: `covers` in the checkpoint record and supersession through it, plus the close and park
  check. A close or park whose checkpoint leaves a batch above its `covers` is refused, and a
  reopened group that wrote nothing closes.
- Task 10: the sealer contract on the owner's loop, one driver mutex, and the transition gate. A
  wake on a non-owner loop with a power cut at every operation; nothing recovers under `closed`.
- `tolerant-open`: `TurnRecovery` reports a bound per damaged group. One damaged block, three
  groups; each group's bound matches what the damage could hold.
- Tasks 13 and 14: the live-record index and the roster. A turn's slice visits only the oldest
  segment's live records; damage before a roster loses only the groups in the damaged span.

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
| packed turn window | `d7_model.py` (probe G) | `all ok`: every admitted record seals in the next turn at N = 1-256, the `ack=sync` group is never starved, and a transaction's chunks keep other writers within 2 turns |
| durability orderings | `d4_covers_model.py 11`, `d4_install_order_model.py 12` (probes H, I) | `all claims hold`: the design loses, tears, exposes and wrongly refuses nothing |
| transitions on one device log | `d6_wake_model.py 14` (probe J) | `all claims hold`: the gate loses nothing and stalls no owner turn |
| turn sizing on the ladder | `d1_ladder_model.py` (probe K) | `all claims hold`: no turn throws, and every admitted record seals in the next turn |
| CPU | `probe-c.sh`: `sample` of the CPU-loss rows | copies and `madvise` under 10% of `wal`'s busy samples |
| loop count | `probe-d.sh` (`rehome.cc`, faultfs) and task 8's test | every record back at every L; re-partition memory bounded by turns, not live bytes |

The gate's rows stay the verdict: A6's exclusive rerun of `bench/kv/run.py` after the tasks land.
