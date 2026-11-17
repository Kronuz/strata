> Status: DESIGN, ready for review (2026-11-16). Quest `document-gate`, criterion A1. No code yet.

# Standalone durability through the turn log

A standalone index (no consensus, `rf = 1`) acknowledges a transaction at one turn-log barrier
instead of a full commit. The turn log is Track C D11's journal (`RFTW0001`, Kronuz/journal), the
index is one group in it, and the manifest log is its checkpoint.

Cited at strata `491a1bdc` (this branch's base), journal `5bbeedc` (strata's pin,
`CMakeLists.txt:29`) and completion `e49f288` (strata's pin). Every number comes from a cited note.
This design measures nothing yet.

## Summary

Today the only power-loss-safe acknowledgement is `commit()`: flush the write buffer to a segment,
order the segment and the directory, then append and sync a manifest-log edit. On the macOS
`--batch 1` row that costs about 5.2 `F_FULLFSYNC` per operation, with 91% of wall time spent in
syncs (cpp-libraries `docs/research/sync-cost.md` §2, job `261116-150926-6ba536`). Every
synchronous row runs 4.76 to 5.09× slower than RocksDB (`docs/perf/core-vs-rocksdb-f14.md:20-22`).

Under this design a durable write works like this:

1. Apply the transaction in memory.
2. Seal the index's transactions of the turn into one record.
3. Issue one `pwrite` into a preallocated segment and one `File::sync_data`.

No file is created, renamed or removed per acknowledgement. `commit()` becomes a background
checkpoint, once per write-buffer flush. A checkpoint record publishes it and lets the log reclaim.

Expected, by arithmetic from the cited floors (M2 and M3 measure it): one device flush per turn
instead of about five per operation. The synchronous rows should then reach flush-bound parity
with RocksDB, which also pays one `F_FULLFSYNC` per write group. CPU per operation then decides
which engine is faster.

What this design settles from earlier documents:

1. Manifest-log D9's three open items (`manifest-log.md:454-484`): the targets (Cost), the tail
   classifier (D10), and whether an index leaves the log (D7).
2. The ack point that rocksdb-survey K2 (`:207`) and workload-profiles item 8 (`:100-106`) leave
   open: apply overlaps the barrier, and the acknowledgement waits for it (D4).
3. It keeps `RFTW0001` and D11's rules unchanged: one barrier per turn (`track-c.md:497-505`), one
   turn in flight (`:506-509`), the torn tail (`:510-516`), and checkpoint records published under
   Condition 25 (`:582-587`, `:608-612`).
4. It takes the owner's rocksdb-survey decision (`:386`): no new tier. RocksDB's WAL-unsynced row
   runs against `ack=none` on the turn log.

It uses nothing from the parked durability-modes design beyond D1's split of commit into
durability, visibility and checkpoint.

## Decisions

### D1. The turn log is the write-ahead log; `commit()` becomes a checkpoint

```
client  -- apply(t) -->  owner: stage 1, verdict, t joins the open record at {s, j}
host turn loop:  seal open records -> driver.begin() -> pwrite -> sync_data -> completed()
                 -> index.turn_synced(s): publish V through s, wake waiters      (the ack)
background:      commit() = checkpoint (D5) -> checkpoint record -> the log reclaims
```

- An index is **logged** when its options carry a turn group: a `TurnDriver*` and a group id
  (group 0 is reserved, `turn_log.h:205`). Without one it is **unlogged**, the `none` tier, as today.
- `apply(t, at)` keeps its single signature (`index.hh:794`):
  - a replicated host passes the position;
  - an unlogged index passes none;
  - a logged standalone index passes none and the index assigns the position.

  Replay's skip of positions at or below the applied one (`:803`) is unchanged.
- The host drives turns through journal's `TurnDriver` (`turn_driver.h:57-99`), as consensus's
  `Worker` takes a `TurnDriver&` (consensus `include/consensus/worker.h:112`). strata adds a small
  turn runner for hosts that have none, such as the kv driver and the tests: a mutex over the
  driver and one turn at a time on the host's executor.
- `kv_strata`'s `durable()` (`bench/kv/kv_strata.cc:136`) stops calling `commit()` and awaits the
  ticket of its batch's last transaction.

Rejected alternatives:

- **A cheaper per-batch commit.** The edit alone still needs the documents in a file
  (manifest-log D9's floor).
- **Documents in the manifest log.** That is a second WAL, which item 8 rules out.
- **A turn log per index.** It preallocates a 64 MiB segment per index, at millions of indexes, and
  shares no barrier.

### D2. Apply first, then log the accepted transactions

The owner runs stage 1, and only an `accepted` transaction enters the open record. Log-first, the
replicated order, loses here for three reasons:

1. **Some verdicts depend on the environment, not on state:** `no_space`, `flush_owed`,
   `drain_owed` and `map_owed` (`index.hh:87`). A transaction logged and then refused for space
   would replay as accepted.
2. **A stage 1 that throws** rolls back and rethrows with the position unmoved
   (`index.hh:790-793`). A staged turn cannot withdraw its record, so replay would apply what the
   client was told failed.
3. **A rejection leaves no trace** (`index.hh:82`), so omitting it replays the same state.

Replay requires every logged transaction to come back `accepted`. Any other verdict fails the open
with `replay_diverged`, naming the position; the transaction is never skipped. A `no_space` at
replay reports as such: the host frees space and opens again.

### D3. One record per index per turn, and its bytes

- **The open record.** The index keeps one open record whose sequence s is the group's last staged
  sequence plus one. Each `apply` call takes position {s, j}, j counting calls, because a rejection
  also moves the position (`index.hh:878-880`).
- **Sealing.** The turn loop seals every open record before `driver.begin()`, so a turn carries one
  record per index whatever the transaction count. A mutex guards the open record between the owner
  and the sealer. A record seals early at `record_bytes` (1 MiB, a preset).
- **Bytes.** This is strata's command encoding, version 1. The replicated path proposes the same
  transaction bytes (durability-modes D16), so they are defined once, here:

| Record | Payload |
|---|---|
| Batch (`TurnRecordKind::Batch`) | `u8 1` (transactions), `u8 1` (version), LEB128 count, then per transaction the LEB128 delta of j (strictly increasing), its LEB128 length, and its bytes |
| A transaction | LEB128 op count; per op a `u8` kind (`index.hh:62`) and its fields; a document as its core key and each layout plugin's part through a new `E::encode_part` / `E::decode_part` hook; a presence mask for `expected_schema`, `set_schema`, `fields`, `expected_fields` |
| Checkpoint (`TurnRecordKind::Checkpoint`) | `u8 2`, `u8 1`, the index UUID (16 bytes), the manifest-log generation g, the position X (two LEB128), the index's key (LEB128 length, bytes), so a host can map a recovered group to its directory |

- **Decoding.** A reader refuses an unknown kind or version as `unsupported_index`, never as
  damage.
- **Layouts.** A layout whose plugin lacks the hook cannot be logged. That is a compile-time error.

### D4. The ack point and visibility, per tier

| Tier (RocksDB row) | Index | Acknowledged at | Visible at | Survives |
|---|---|---|---|---|
| `none` (`disableWAL`) | unlogged | `apply` returns | refresh, as today | what the last `commit()` made durable |
| `ack=none` (WAL written, not synced) | logged | `apply` returns | its turn's barrier | everything whose turn's barrier returned: the loss window is one barrier plus one write after `apply` |
| Group commit (`sync=1`, N writers) | logged | the barrier of the turn holding its record | that barrier | every acknowledged write |
| Sync per batch (`sync=1`, one writer) | logged | the same, with one transaction per turn | that barrier | every acknowledged write |

- **Pipelined (K2).** Stage 1 keeps running while a turn's barrier is in flight. Each transaction
  sees every applied one, including versions and conditional puts. With one turn in flight
  (`turn_log.h:198-202`), a crash loses a suffix of the apply order, never a hole in it.
- **Visibility waits for the barrier** on a logged index. Readers see V, the transaction count
  through the last synced record, in place of the applied count published at `index.hh:884`.
  `turn_synced` runs as an owner chore through the `check_in` path (`index.hh:925-940`). It
  publishes V before it wakes the waiters, so an acknowledged client reads its own write. Making a
  write visible at apply would let a reader act on a write that a crash then removes; RocksDB with
  `sync=1` and Raft both expose only synced writes.
- **Rejected: ack at the write's return** (durability-modes D3's `memory` level). Under `RFTW0001`
  the next write still waits for the previous turn's barrier, so a closed-loop writer gains
  nothing. Several unsynced turns need `RFTW0002`'s frontier, and that design is parked.

### D5. The checkpoint and its two invariants

**Triggers:**

1. Each write-buffer flush, so the retained log is about one buffer, as RocksDB keeps its WAL until
   the memtable flushes.
2. The group's log bytes since its last checkpoint record passing `checkpoint_log_bytes` (D17's
   share; by default the buffer budget).
3. `close()` and demotion.

**Order**, under the writer lease:

1. Seal, then wait until the log is durable through the applied position X. That wait is at most
   the in-flight turn plus one more.
2. Flush, then `commit_files()` (`index.hh:4970`): the segments' ordering point, the edit, its
   sync.
3. Stage the checkpoint record {UUID, g, X}. After its barrier, the log supersedes the group's
   earlier records (`turn_log.h:263-269`) and reclaims them.

- **I1. A head names only what the log made durable.** Without I1, a crash can leave the head ahead
  of the log. The log's next sequence would then reuse positions at or below X, and `apply` would
  skip acknowledged writes as replays (mutant M-b). I1 costs the writer one or two barriers per
  checkpoint (M5). Checkpointing a past generation would avoid that stall, but neither DM-R3 nor
  manifest-log D3's past-generation edit is built.
- **I2. The record is staged only after its edit is durable** (Condition 25). Otherwise the record
  could supersede records that no durable head covers (mutant M-a).
- **No recorded-generation keep is needed** (DM-R43), because recovery opens the head (D6). Between
  the edit and the record's barrier, the log still holds every record at or below X, and replay
  skips them.
- **An oversized transaction** is one whose encoding passes `segment_bytes / 4` (16 MiB by
  default). `apply` checkpoints right after it, later transactions wait for that checkpoint, and the
  prefix order holds. The log adds no cap beyond what `apply` admits today.

### D6. Recovery

1. **Recover the log.** The host constructs the `TurnDriver`. The `TurnLog` constructor recovers
   each group from its latest checkpoint and makes the new epoch's open record durable
   (`turn_log.h:168-181`).
2. **Open the index.** Each logged index opens as today (manifest-log D6) at head H, with applied
   position X_H. The open writes and syncs nothing.
3. **Check the log identity.** The index's `turn_group` attribute (D7) must name this log, or the
   open fails with `log_mismatch`.
4. **Find the group in `TurnRecovery`:**
   - **It has records.** Its latest checkpoint record must carry the index's UUID and an
     X_c ≤ X_H. Replay applies each transaction at {s, j} into the write buffer, skipping every
     position at or below X_H, and writes nothing until the first commit (sync-at-first-commit D1).
     Then the host calls `driver.release(group)`.
   - **It is lost, or absent from a damaged log.** The open fails with `log_lost`; there is no
     replica to repair from.
   - **It is absent from an undamaged log.** Its join record never reached a barrier, so nothing
     was acknowledged under it: open at H.
5. **Cross-check against the head.** If the group's last kept record is below X_H, the log lost
   records the head proves were durable: `log_lost` (D10).
6. **Replay eagerly.** A host replays every group holding batch records at startup, which releases
   recovery's memory and the segments those records pin.

### D7. Joining and leaving the log

- **The `turn_group` index attribute** is an entry in the index-level attribute list
  (`manifest_log.hh:354`, edited at `:404`). It holds the log's 16-byte identity and the group id,
  under a critical tag: a binary that does not know the tag refuses the index, so no build opens a
  logged index while ignoring its log.
- **Join:** an edit sets the attribute, then the index writes its first record, a checkpoint record
  at the current head.
- **Leave** (`close()`, demotion): a checkpoint covering the last batch, then an edit that clears
  the attribute. After it the directory opens alone, for install and tools.
- **The cost of leaving.** The group's last checkpoint record stays live, because `RFTW0001` cannot
  drop a group (`turn_log.h:263-269`). That is Q1.

### D8. Where the log lives

The log lives wherever the host's `IO` points.

- **A host with many indexes** keeps one log per core for all of them.
- **A single-index library** may give the log the index's own directory, as the kv driver does. The
  segment names (`turns-%016llx`) do not collide with `seg-` and `manifest-`, and the index's scan
  skips names it does not parse (`index.hh:5318`).

### D9. Faults

| Fault | Result |
|---|---|
| A turn write or barrier fails | the `TurnLog` fails (`turn_log.h:250`); every operation staged with or after it fails (`turn_driver.h:87-89`); every index in the log fences; reopening is recovery |
| A failed barrier whose pages Linux keeps clean but unwritten | a same-boot reopen reads the turn back as kept, and a later power loss then loses its groups. The fix is journal's (Q2) |
| `ENOSPC` | turns never grow a file (`turn_log.h:317-326`); spare segments are prepared off the turn; storage admission counts the log's segments, so `apply` answers `no_space` before a turn has to prepare one inline (`:327-329`) |
| A checkpoint fails | as `commit()` fails today (`index.hh:1027-1030`); the log still holds everything above the head |

### D10. The torn tail: Track C's rule, not the manifest log's classifier

The manifest log's classifier reads a never-written piece as zeros (manifest-log D6.4). A recycled
turn-log segment holds stale bytes from its earlier use, which fail their checks without being
zeros, so that classifier would call every torn turn in a recycled segment damage. The turn log
therefore keeps D11's rule: the newest epoch's last turn is the torn tail, whatever its bytes show
(`turn_log.h:473-474`).

D11's stated residual (`track-c.md:560`) is that damage there reads as torn. For a standalone index
the manifest head is a durable frontier kept elsewhere, and D6.5 fails an open whose log ends
below it.

### D11. How the syncs batch

1. **One barrier per turn for every index on the core**, with every sealed record in one write.
2. **Adaptive group commit with no timer.** Whatever was applied during a barrier becomes the next
   turn, so the batch grows with barrier latency: 20.79 ms per `F_FULLFSYNC` on this Mac at p50
   (sync-cost §1). RocksDB's write group works the same way.
3. **Checkpoints off the acknowledgement path.** Their two device flushes are paid once per buffer
   flush, not once per batch.
4. **No metadata in the barrier.** Segments are preallocated, so Linux needs only `fdatasync`, and
   no turn syncs the directory.
5. **Nothing weaker than durable.** `F_BARRIERFSYNC` costs 0.46 ms against `F_FULLFSYNC`'s 20.79 ms
   (sync-cost §1), but it survives no power loss, so no tier uses it.

A closed-loop writer with one transaction per batch has nothing to batch. On macOS that row stays
flush-bound for both engines, with the sync share near today's 83 to 91%, at one flush per batch
instead of five.

## Cost per durable batch

| | `main` (`commit()` per batch) | This design | Source |
|---|---|---|---|
| Files created, renamed or removed | one segment per commit that flushed, plus rollovers | 0 | `index.hh:4970-5040`; D1 |
| Device flushes, macOS | about 5.2 `F_FULLFSYNC` per op at `--batch 1`; 373 over 72 batches at `--batch 100` | 1 per turn, shared by every index on the core | sync-cost §2; D11 |
| CPU in file calls | 739 µs for an edit with a segment; 183 µs for an edit alone | the append-and-sync floor, about 25 µs, plus encoding | `manifest-log.md` "Measured", probes I, J and A |
| Bytes beyond the data | the segment | per turn, two 24-byte fragment headers and two 25-byte logical headers (`framing.h:43`, `turn_log.h:80`); the segment at the checkpoint | arithmetic |

The CPU and flush figures come from the cited probes on the development Mac (Intel, APFS). Linux
`fdatasync` on a preallocated segment is not measured.

**Targets** (manifest-log D9 asks for them):

1. 0 files per acknowledgement.
2. CPU per durable batch at most 1.25× the append-and-sync floor K'.
3. On the gate's synchronous rows, at most 1.0× RocksDB's time (A4).

| # | Measurement | Job |
|---|---|---|
| M1 | Calls per acknowledgement | faultfs counters (`faultfs.hh:226-237`): one sync and no create, rename or unlink per turn in steady state; a ctest assertion |
| M2 | `F_FULLFSYNC` per op, macOS | `bench/kv/barriers.c` on the `sync1` and `wal1` sets (`bench/kv/run.py:85-89`), `main` against the branch |
| M3 | The rows | `run.py` `sync1` and `wal1`, plus a new group-commit row (4 and 16 writers on one index); exclusive hold, A/A first, interleaved A/B against `main` and RocksDB; macOS, then the Linux VM |
| M4 | The floor K' | a C loop of `pwrite` and `sync_data` on a preallocated file, shaped like `mlog_probe.c`, under a strict hold |
| M5 | The checkpoint's stall | the writer's wait for I1's barriers against the flush's own time, p50 and p99, on M3's fill rows |

## Crash model and tests

**The model.** It is faultfs's (`faultfs.hh:1-29`):

- **Power loss:** each unsynced 512-byte sector keeps its old bytes, takes its new ones, or reads
  as zeros. Each namespace change since the last directory sync survives or not, in order.
- **A process crash** keeps every byte written.
- **A failed sync** may leave pages clean but not durable (`fault::keep`, `:61-67`).
- **Short writes, `ENOSPC` and `EIO`** can hit any operation.

The turn log runs on faultfs unchanged: it takes any `kronuz::journal::IO`, and `File::sync_data`
falls back to `sync` (completion `io.h:84`).

**The oracle.** A key-value map replays the transaction stream. Each case checks two things:

1. Every acknowledged transaction is present.
2. The state equals the map after some prefix of the apply order, a prefix at least as long as the
   acknowledged one.

| Test | Case |
|---|---|
| T1 | Power-off sweep over a run with turns, flushes, checkpoints, a manifest-log rollover and recycled 64 KiB segments: `power_off_after(k)` for every mutation k (`:222`), then `crash(seed)` over 20 seeds |
| T2 | A process crash at every mutation (reopen with no `crash()`) |
| T3 | A failed barrier (`EIO`, with and without `keep`): no acknowledgement, every index fenced; a same-boot reopen, then a power loss. Needs Q2 |
| T4 | A read between `apply` and its barrier sees the prior value; after the acknowledgement it sees the new one |
| T5 | Log damage below the head's position fails the open with `log_lost` |
| T6 | A crash between the join's edit and its record, and between the leave's checkpoint and its edit |
| T7 | `no_space` at replay fails the open with `replay_diverged`; with space restored, it opens |
| T8 | A transaction past `segment_bytes / 4` followed by small ones, under T1's sweep |
| T9 | Fixtures for each record kind; an unknown version reads as `unsupported_index`; the decoders under the fuzzer |

**Mutants.** Each one must fail its test:

| Mutant | Fails |
|---|---|
| M-a: the checkpoint record staged before the edit syncs (breaks I2) | T1 |
| M-b: a checkpoint that does not wait for the log through X (breaks I1) | T1 |
| M-c: V published at apply | T4 |
| M-d: no cross-check against the head (D6.5) | T5 |
| M-e: replay skips a non-accepted verdict | T7 |
| M-f: log-first (staged before stage 1) | T7 |

## The A1 checks as jobs

| Check | Job |
|---|---|
| The suite: T1 to T9 and M1 | `campaign verify document-gate --for A1` (the build-test check) |
| A long seed sweep | `campaign job submit --quest document-gate --for A1 --minutes 30 --build "cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j4 --target standalone_turn_log_test" -- build/standalone_turn_log_test --seeds 2000` |
| Each mutant, M-a to M-f | the same job with `--fails-without --commit <mutant>` |
| The turn runner under TSan | the same build with `-fsanitize=thread`, running T1 and the group-commit path, `--for A1` |
| M2 to M5 | exclusive and strict jobs, recorded in `docs/perf/standalone-turn-log.md`; A3's rows reuse them |

## Limits

- **`ack=none` against RocksDB's WAL-unsynced row.** strata's record waits in process memory until
  its turn's write, so a process crash can lose up to one barrier plus one write (about 21 ms on
  macOS); RocksDB's record is in the kernel when `Write` returns. Under power loss the comparison
  reverses: RocksDB loses whatever the kernel has not written back, and strata loses only that
  window (Q4).
- **Silent:** damage confined to the newest epoch's last turn, above every checkpoint, reads as a
  torn tail and drops writes that were acknowledged (D10).
- **Silent until Q2:** a same-boot reopen after a failed barrier, followed by a power loss, loses
  that turn's groups.
- **The writer stalls** for one or two barriers per checkpoint (I1).
- **A cold index** keeps one record resident in the log (Q1).

## Questions for the owner

**Q1. Group retirement.** An index that leaves the log keeps a record copied forward forever.

1. Add a `Retire` record kind to journal, so a group holds nothing live after the record's barrier.
   This is a format change, made with Detent: `RFTW0001` reads an unknown kind as a gap
   (`turn_log.h:446-447`).
2. Keep retired groups resident, and measure the cost at a million indexes.

Recommendation: option 1, in a journal quest after A1, which needs only one index.

**Q2. Undoing a failed turn.**

1. `TurnLog` overwrites the failed turn's range with zeros and syncs it before failing, as
   manifest-log D4 truncates a failed append back.
2. Hosts restart the process and never reopen in the same boot, which nothing can enforce.
3. Wait for durability-modes D3's same-boot rewrite in `RFTW0002`.

Recommendation: option 1, a journal change that T3 needs.

**Q3. One log per core, or per device, on macOS.** `F_FULLFSYNC` flushes the whole drive, so N
per-core logs cost N flushes per turn period. Recommendation: per core, as D11 says, until the
many-index row measures the difference.

**Q4. Labeling the `wal1` row.** Recommendation: run it as `ack=none` on the log, with the crash
windows above stated in the row rather than claimed as equal durability.

## First steps

1. The lead widens this quest's file set (today `docs/gates/document-profile.md`, `bench/kv/**`,
   `docs/perf/**`, `docs/mechanisms/**`) to add `include/strata/core/**`, `test/**`,
   `CMakeLists.txt` and this file.
2. The command codec (D3), with its fixtures and fuzz target.
3. Logged mode in the index: assigned positions, the open record, sealing, `turn_synced` and V; T2
   and T4.
4. The checkpoint, recovery, and joining and leaving (D5 to D7); T1, T5 to T8, and the mutants.
5. The turn runner and `kv_strata` on it (`sync` through the log, `wal` as `ack=none`), plus the
   group-commit row; M1 to M5.
6. Q1 and Q2 as journal quests.
