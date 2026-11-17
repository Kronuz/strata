> Status: APPROVED (revision 1, review round 2), with its nits R1, R2, R3 and R5 folded in and R4
> left as Q5 (2026-11-16). Quest `document-gate`, criterion A1. First steps 1 to 4 are built, D7's
> join and leave included; step 5 has `kv_strata` on a host and M1; step 6 (the real seal) is in. "Built"
> says what and where.

# Standalone durability through the turn log

A standalone index (no consensus, `rf = 1`) acknowledges a transaction at one turn-log barrier
instead of a full commit. The turn log is Track C D11's journal (`RFTW0001`, Kronuz/journal), the
index is one group in it, and the manifest log is its checkpoint.

Cited at strata `491a1bdc` (this branch's base); journal `5bbeedc` (strata's pin,
`CMakeLists.txt:29`) for `turn_log.h`'s rules; journal `main` `7c08540` for the host layer this
design consumes (`host.h`, `turn_driver.h`'s `run()` and `durable()`, `blocking.h`); completion
`e49f288`. Every number comes from a cited note or from the review's probes P1 and P2 (job
`261116-203118-487c1b`). This design measures nothing itself.

## Revision 1

| Finding | Change | Test |
|---|---|---|
| F1, the checkpoint's supersede rule | D5: the lease spans seal to checkpoint record; the true stall; acks need no lease (D4) | T1 with racing applies; M-g |
| F2, replay verdicts and mid-replay flushes | D6: owed verdicts run their chore; replay flushes publish no checkpoint record | T7b; M-h, M-i |
| F3, seal and position | D3: the position is taken with the append, in one critical section; the driver seals | T12; M-j; TSan |
| F4, transactions past a segment | D3: the command limit at admission, and a turn budget | T8; M-l |
| F5, `refresh()` above V | D4: every flush on a logged index waits for the log first | T4; M-k |
| F6, uncaught mutants | T1 writes after each recovery; T10 refuses at runtime | M-b fails T1; M-f fails T10 |
| N1, Detent's Q1/Q2 decision | D7 and D9 use the host's group table and fail-stop | T3, T6 |
| N2, N logs on one device | Q1 rewritten with P2; an N-log row in M3 | M3 |
| N4, tails | the Summary claims throughput and median parity only | M3, M5 at p99.9 |
| N6, group-id reuse | D7: ids are never reused | T6 |
| N7, a failed turn's outcome | D9: `outcome_unknown` | T3 |
| N3, N5 | Q3 and Q4, for the owner | |

## Summary

Today the only power-loss-safe acknowledgement is `commit()`: flush the write buffer to a segment,
order the segment and the directory, then append and sync a manifest-log edit. On the macOS
`--batch 1` row that costs about 5.2 `F_FULLFSYNC` per operation, with 91% of wall time in syncs
(cpp-libraries `docs/research/sync-cost.md` §2, job `261116-150926-6ba536`). Every synchronous row
runs 4.76 to 5.09× slower than RocksDB (`docs/perf/core-vs-rocksdb-f14.md:20-22`).

Under this design, a durable write is:

1. Apply the transaction in memory.
2. Join the index's open record.
3. Let the turn write every group's sealed record with one `pwrite` into a preallocated segment and
   one `File::sync_data`.

No file is created, renamed or removed per acknowledgement. `commit()` becomes a checkpoint, run
once per write-buffer flush and published by a checkpoint record that lets the log reclaim.

The expected result, by arithmetic from the cited floors (M2 and M3 measure it), is one device
flush per turn instead of about five per operation. The synchronous rows should then reach
flush-bound parity with RocksDB on **throughput and median latency**, because both engines pay one
`F_FULLFSYNC` per write group. The tail percentiles are not claimed: each checkpoint stalls
strata's writer (D5), while RocksDB flushes its memtable in the background. M3 and M5 report
p99.9.

What this design settles from earlier documents:

1. Manifest-log D9's three open items (`manifest-log.md:454-484`): the targets (Cost), the tail
   classifier (D10), and how an index leaves the log (D7).
2. The ack point that rocksdb-survey K2 (`:207`) and workload-profiles item 8 (`:100-106`) leave
   open: apply overlaps the barrier, and the acknowledgement waits for it (D4).
3. `RFTW0001` and D11's rules stay unchanged: one barrier per turn (`track-c.md:497-505`), one turn
   in flight (`:506-509`), the torn tail (`:510-516`), and checkpoint records under Condition 25
   (`:582-587`, `:608-612`).
4. The owner's rocksdb-survey decision (`:386`): no new tier. RocksDB's WAL-unsynced row runs
   against `ack=none` on the turn log.

## Decisions

### D1. The turn log is the write-ahead log; `commit()` becomes a checkpoint

```
client  -- apply(t) -->  owner: stage 1, verdict; accepted: join the open record at {s, j}
loop:     driver.run(): seal each group's open record -> pwrite -> sync_data -> completed()
watcher:  co_await driver.durable(group, s) -> publish V through s -> resolve tickets (the ack)
owner:    commit() = checkpoint (D5) -> checkpoint record -> the log reclaims
```

- **Logged or unlogged.** An index is logged when its options name a journal `Host`
  (`host.h:251`) and a group id. Otherwise it is unlogged, the `none` tier, as today.
- **One `apply` signature.** `apply(t, at)` stays as it is (`index.hh:794`):
  - a replicated host passes the position;
  - an unlogged index passes none;
  - a logged standalone index passes none, and the index assigns it (D3).

  Replay passes the logged position, and its skip at or below the applied position (`:803`) is
  unchanged.
- **Turns are journal's, not strata's.** The host's loop runs `TurnDriver::run()`
  (`turn_driver.h:184` at `7c08540`). A blocking host, such as the kv driver or the tests, uses
  `journal::run` (`blocking.h:90-113`) to commit turns inline. Callers await durability with
  `TurnDriver::durable(group, sequence)` (`:205`). strata adds no turn runner of its own.
- **The kv driver.** `kv_strata`'s `durable()` (`bench/kv/kv_strata.cc:136`) stops calling
  `commit()` and awaits its batch's last ticket instead.

Rejected:

- **A cheaper per-batch commit.** The edit alone still needs the documents in a file
  (manifest-log D9's floor).
- **Documents in the manifest log.** That is a second WAL, which item 8 rules out.
- **A turn log per index.** It would preallocate a 64 MiB segment per index, at millions of
  indexes, and no two indexes would share a barrier.

### D2. Apply first, then log the accepted transactions

The owner runs stage 1, and only an `accepted` transaction joins the open record. Log-first, the
replicated order, loses here for three reasons:

1. **Some verdicts depend on the environment, not the state:** `no_space`, `flush_owed`,
   `drain_owed` and `map_owed` (`index.hh:84-87`). A transaction logged and then refused for space
   would replay as accepted.
2. **A stage 1 can throw.** It then rolls back and rethrows (`index.hh:790-793`), but a staged
   record cannot be withdrawn, so replay would apply what the client was told had failed.
3. **A rejection leaves no trace** (`index.hh:82`), so omitting it replays the same state.

D6 gives the rules for verdicts at replay.

### D3. The open record, its bytes, and the limits

- **The position is taken with the append** (F3). On a logged index with no position given, stage
  1 runs without one. If it accepts, one critical section under the record mutex does three
  things: it takes the open record's sequence s and the next j, appends the encoded transaction,
  and sets the applied position to {s, j}. A rejection takes no position and leaves the open record
  unchanged, so j counts only logged transactions. A record takes its sequence at its first append:
  the group's last staged sequence plus one.
- **The driver seals, on its loop.** A journal addition, `TurnDriver::seal(group, fn)`, registers
  a sealer that `start()` calls for each group before it encodes a turn, under the same record
  mutex. The sealer turns the open record into a staged `TurnRecord`, and the next append opens
  record s + 1.
  - Sealed records follow the records already pending for the turn, so a checkpoint record staged
    before them keeps its lower sequence.
  - The addition is API only, with no format change, and Detent reviews it (the quest's decision).
- **The command limit, at admission** (F4). Before stage 1, a logged index refuses with `invalid`
  any transaction whose encoding passes `command_bytes`, 64 MiB. That is both the replicated
  command's ceiling (`limits.md:24`, consensus `Limits::command_bytes`) and the stored record's cap
  (`format::max_stored_record`, `limits.md:23`), so the log admits what the replicated path admits.
  The host's segments must hold two such commands with their framing (the turn budget below),
  checked when the host opens. Unlogged indexes keep today's limits (Q5).
- **Records stay whole.** A record takes transactions while its payload stays within
  `command_bytes`; the transaction that would pass it closes the record and opens the next. The
  first transaction of a record always joins, so a record's payload is at most `command_bytes`
  plus 13 bytes of header, count and length.
- **The turn budget.** The sealer adds whole records, oldest first, while the turn's framed bytes
  stay within `segment_bytes / 2`. The first record of a turn is always admitted, whatever its
  size; the rest stay for the next turn. A record of payload P frames to at most E(P) = L +
  ⌈L / (B − 126)⌉ × 126 + B/2 bytes, with L = P + 25 its logical bytes and B the block size: each
  block it crosses costs a block header naming one group (78), a fragment header (24) and at most
  24 bytes of tail padding, and one block header may name many groups, up to half a block
  (`framing.h:107`). At B = 32 KiB, E(64 MiB) is about 64.27 MiB. With `segment_bytes ≥
  2 × E(command_bytes + 32) + E(4)`, 129 MiB at 32 KiB blocks, every turn fits an empty segment, so
  `encode()`'s `length_error` (`turn_log.h:337-340`) cannot fire on one index's command in a turn
  shared by every index on the loop.
- **The bytes.** This is strata's command encoding, version 1. The replicated path proposes the
  same transaction bytes (durability-modes D16).

| Record | Payload |
|---|---|
| Batch (`TurnRecordKind::Batch`) | `u8 1` (transactions), `u8 1` (version), a LEB128 count, then per transaction its LEB128 length and bytes; transaction i of record s sits at {s, i} |
| A transaction | a LEB128 op count; per op a `u8` kind (`command.hh`) and the fields that kind reads (below); then a `u8` presence mask, bits 1, 2, 4 and 8 for `expected_schema`, `set_schema`, a non-empty `fields` and `expected_fields`, each present member in that order: the two schemas as LEB128, the fields as a count and `put_decl`'s bytes, the mark as its size and chain |
| Checkpoint (`TurnRecordKind::Checkpoint`) | `u8 2`, `u8 1`, the index UUID (16 bytes), the manifest-log generation g, and the position X (two LEB128) |

- **An operation's fields.** `set_meta`: the key and value, each LEB128-sized; `delete_meta`: the
  key. Every other kind: a `u8` flags byte (bit 1, `expected_version` follows as LEB128); then
  the key for `replace_key` and `delete_key`, the docid for `replace_docid` and `delete_docid`,
  and the document for `add`, `replace_key` and `replace_docid`: its core key, then each part
  through its plugin's `E::encode_part` / `E::decode_part` hook (`stored`: its record, sized).
- **Unknown bytes.** A reader refuses an unknown kind or version as `unsupported_index`, never as
  damage. Bytes no encoding produces (an unknown op kind, flag or mask bit, an overlong integer,
  an empty batch or field list, trailing bytes) are `corrupt_index`. A layout whose plugin lacks
  the hook cannot be logged at all, which is a compile-time error.

### D4. The ack point and visibility, per tier

| Tier (RocksDB row) | Index | Acknowledged at | Visible at | Survives |
|---|---|---|---|---|
| `none` (`disableWAL`) | unlogged | `apply` returns | refresh, as today | what the last `commit()` made durable |
| `ack=none` (WAL written, not synced) | logged | `apply` returns | its turn's barrier, so its writer misses its own acknowledged write for up to one barrier (Q3) | everything whose turn's barrier returned; the loss window is one barrier plus one write after `apply` |
| Group commit (`sync=1`, N writers) | logged | the barrier of the turn holding its record | that barrier | every acknowledged write |
| Sync per batch (`sync=1`, one writer) | logged | the same, with one transaction per turn | that barrier | every acknowledged write |

- **Pipelined (K2).** Stage 1 keeps running while a turn's barrier is in flight, and each
  transaction sees every applied one. With one turn in flight (`turn_log.h:198-202`), a crash loses
  a suffix of the apply order, never a hole in it.
- **V and the acknowledgement need no lease** (F1). V is the count of transactions through the
  last synced record:
  1. The sealer records each record's cumulative transaction count.
  2. A watcher coroutine per logged index awaits `driver.durable(group, s)`.
  3. The watcher then stores V, that count, with a release store, and resolves the record's
     tickets.

  A logged index's writer never stores the applied count where readers look (`index.hh:884`,
  `:1539`). It still publishes the cover at apply, as today (`publish_cover`, `:3078`). The cover
  is published before the seal, so V never passes it. An acknowledged client therefore reads its
  own write, and no acknowledgement waits behind a lease holder.
- **No flush publishes above V** (F5). Every flush on a logged index publishes only after the log
  is durable through the applied position, so the segment it publishes holds only synced
  transactions. The build waits before it writes: `flush()`, and the move of a buffer's pending
  deletes into segment bits (`apply_pending()`, which `merge()` and `force_merge()` run too). That
  covers `refresh()`, apply's inline flush (`index.hh:800-801`) and a checkpoint. A host's refresh
  pays at most the in-flight turn plus one more; flushes during replay pay nothing (D6).
- **Index-level state follows the log too.** A view carries the metadata, field table, schema
  version and last docid whole, not filtered by V. While the applied position is past the log's
  durable sequence, a logged index's new view keeps its predecessor's, so a cover published
  between apply and the barrier shows no unsynced metadata. The next view built with the log
  caught up carries them.
- **Rejected: an ack at the write's return** (durability-modes D3's `memory`). Under `RFTW0001` the
  next write still waits for the previous barrier, and several unsynced turns need `RFTW0002`,
  which is parked.

### D5. The checkpoint: its lease span, stall and invariants

**Triggers:**

1. Each write-buffer flush, so the retained log is about one buffer, as RocksDB keeps its WAL
   until the memtable flushes.
2. The group's log bytes since its last checkpoint record passing `checkpoint_log_bytes` (D17's
   share, by default the buffer budget).
3. `close()` and demotion.

**Order.** The writer lease is held from step 1 until step 4 has staged the record (F1):

1. Seal the open record through the driver's loop. X is the applied position, in record s.
2. Flush. The segment is written while the log's barriers run.
3. Await `driver.durable(group, s)`, which takes at most the in-flight turn plus the one carrying
   s. The wait is on the driver, not on V, so it cannot wait on itself. Then `commit_files()`
   (`index.hh:4970`): the segment's ordering point, the edit and its sync.
4. Stage the checkpoint record {UUID, g, X} at sequence s + 1 on the loop, then release the lease.

**Why the lease spans all four steps.** `RFTW0001` accepts a group's checkpoint only at the
group's last sequence plus one (`turn_log.h:209-211`), and it drops every earlier record of the
group when the checkpoint lands (`:263-269`). A batch record staged between step 1 and step 4
would sit between X and the checkpoint and be dropped at its barrier. P1 reproduced this on the
real `turn_log.h`: an acknowledged sequence 2 was gone after a checkpoint naming sequence 1. With
the lease held, no apply runs, so no record can exist there. A past-generation checkpoint has the
same flaw. Avoiding the stall needs a checkpoint that supersedes only through a named sequence,
which is a later journal change (the host table's `base` already has that meaning,
`turn_log.h:72-80` at `7c08540`).

**The invariants:**

- **I1. A head names only what the log made durable** (step 3 comes before the edit). Without I1,
  a crash can leave the head ahead of the log. Under D3 the index numbers its records after the
  log's durable sequence, so the writes after that reopen take positions at or below X: the
  applied position regresses, and the next replay skips those writes as already applied (M-b). An
  assert keeps the applied position from moving back, and D6's cross-check fails such an open
  with `log_lost`.
- **I2. The record is staged only after its edit is durable** (Condition 25). Otherwise the record
  would supersede records that no durable head covers (M-a).

**The stall.** The writer waits for the flush, then for the remaining in-flight barriers, then for
`commit_files()`. Manifest-log's probe I puts `commit_files()` at two device flushes (24.2 ms);
`main`'s synchronous rows count about five `F_FULLFSYNC`, about 100 ms, per commit (sync-cost §2).
M5 says which is right. Acknowledgements of transactions already applied keep flowing (D4); only
new applies wait.

Recovery opens the head (D6), so durability-modes' recorded-generation keep (DM-R43) is not needed:
until the record's barrier the log holds every record at or below X, and replay skips them.

### D6. Recovery

1. **The host opens.** `Host`'s constructor recovers every loop's log, then marks the ids that
   recovery calls lost in the group table before it serves (`host.h:247-250`, `mark_lost` at
   `:454`).
2. **The index opens** as today (manifest-log D6), at head H with applied position X_H, writing
   and syncing nothing. Its `turn_group` attribute (D7) must name this host; otherwise the open
   fails with `log_mismatch`.
3. **What happens next depends on `host.status(group)`** (`host.h:272`):
   - **`stored`:** the group's latest checkpoint record must carry the index's UUID and an
     X_c ≤ X_H. Replay follows the rules in step 5, then the host calls `driver.release(group)` and
     `host.attach(group)`.
   - **`closed`:** the table's base covers every batch (D7). Open at H; the open reopens the
     group and stages the table's record again as base + 1, as journal's Store does.
   - **`none`:** the group's first turn never became durable, so nothing was acknowledged under
     it. Open at H and join again (D7): a new incarnation, with the attribute's edit only when the
     head lacks it.
   - **`lost`:** the open fails with `log_lost`, since no replica can repair it (Q4).
4. **Cross-check.** A group whose last durable sequence is below the head's record sequence (the
   index of X_H) lost records that the head proves were durable. The open fails with `log_lost`
   (D10).
5. **Replay** (F2):
   - Each logged transaction applies at its position {s, i}; at or below X_H it is skipped.
   - **Owed verdicts.** `flush_owed`, `drain_owed` and `map_owed` say only that a chore is owed
     (`index.hh:84-87`). Replay runs the chore (`refresh()`, `reclaim_for_space()` or `promote()`)
     and applies again. Each is answered at most once per transaction, so the loop ends.
   - **`no_space` after the drain** fails the open with `no_space`, and the host retries once it
     has freed space.
   - **Any other verdict** (`version_conflict`, `schema_conflict`, `invalid`, `fields_conflict`)
     fails the open with `replay_diverged`, naming the position. Nothing is skipped.
   - **No flush is a checkpoint until the group is released.** An inline or owed flush writes and
     publishes its segment, waiting for nothing because every replayed record is already durable,
     and it stages neither an edit nor a checkpoint record. A checkpoint record staged mid-replay
     would supersede records not yet replayed (M-h). The first checkpoint runs after `release`.
6. **Replay is eager.** A host replays every stored group at startup, not lazily, which releases
   recovery's memory and the segments its records pin.

### D7. Joining and leaving: the host's group table

Membership lives in one place, the host's group table (`host.h`; journal `docs/design/host-layout.md`
at `7c08540`; Detent's decision), never in the index.

- **The `turn_group` index attribute** holds identity only: the host's 16-byte identity and the
  group id, in the index-level attribute list (`manifest_log.hh:354`, edited at `:404`). It sits
  under a critical tag, so a build that lacks the tag refuses the index, and no build can write to a
  logged index without its host. Read-only tools read the head as today.
- **Group ids are never reused** (N6). The host draws an id that no table entry holds, and a closed
  group's entry keeps its id, so a group can never carry a foreign UUID.
- **Join:**
  1. `host.create_group(id)` (`host.h:307`), durable when it returns.
  2. An edit sets the attribute.
  3. The first record is a checkpoint record at the head.

  A crash before that record's barrier leaves the group `none` (D6 step 3).
- **Leave** (`close()`, demotion):
  1. A checkpoint covering every batch (D5), with nothing staged after it.
  2. `host.close_group(id, its sequence, its payload)` (`:321`). The table takes the record as the
     group's base, and the log forgets the group (`TurnLog::forget`), so a cold index holds nothing
     in the log.
- **Reopen:** `host.reopen_group(id)` (`:315`), after which the journal stages the table's record
  again as base + 1.

### D8. Where the log lives

The log lives in the journal host's directory: one directory per strata host, with one loop log
per core (the names at `host.h:28`). The host directory takes the directory's own owner lock, the
same lock strata's cold tier takes (`host-layout.md`, "The lock"), so it cannot also be an index's
directory. A single-index library therefore gives the host a sibling directory, as the kv driver
will. Index directories are unchanged.

### D9. Faults

| Fault | Result |
|---|---|
| A turn write or barrier fails | fail-stop (Detent's decision). `TurnLog` fails (`turn_log.h:271` at `7c08540`), `run()` rethrows, and every index on the loop fences. Every operation staged with or after the turn fails with `outcome_unknown` (N7), because its bytes may be durable and a whole last turn is kept. Nothing appends until the host reopens |
| The reopen after it | the failed turn is the newest epoch's last turn, and the torn-tail rule cuts it when it reads back torn. Nothing is zeroed, because nothing appends without a reopen |
| Linux kept the failed turn's pages clean but unwritten | a same-boot reopen reads the turn whole and keeps it. A later power loss then damages a turn that the new open record names, and its groups open as `log_lost`: loud, never silent (Limits) |
| `ENOSPC` | turns never grow a file (`turn_log.h:317-326` at `5bbeedc`), and spares are prepared off the turn. Storage admission counts the log's segments, so `apply` answers `no_space` before a turn must prepare one inline (`:327-329`) |
| A checkpoint fails | as `commit()` fails today (`index.hh:1027-1030`); the log still holds everything above the head |

### D10. The torn tail: Track C's rule, not the manifest log's classifier

The manifest log's classifier reads a never-written piece as zeros (manifest-log D6.4). A recycled
turn-log segment keeps its old bytes, because reclamation recycles by rename
(`turn_log.h:302-305`). Those bytes fail their checks without being zeros, so that classifier would
call every torn turn in a recycled segment damage. The turn log therefore keeps D11's rule: the
newest epoch's last turn is the torn tail, whatever its bytes show (`turn_log.h:473-474`).

Under faultfs's model, that turn can be torn only if its barrier never returned, so it was never
acknowledged. D11's residual, media damage to that synced turn (`track-c.md:560`), is bounded by
D6's cross-check to the writes since the last checkpoint.

### D11. How the syncs batch

1. **One barrier per turn for every index on the loop**, with every sealed record in one write.
2. **Adaptive group commit, with no timer.** What was applied during a barrier becomes the next
   turn, so the batch grows with barrier latency (20.79 ms per `F_FULLFSYNC` at p50, sync-cost
   §1), as RocksDB's write group does.
3. **Checkpoints off the acknowledgement path.** Their device flushes are paid once per buffer
   flush, and acknowledgements never wait for them (D4).
4. **No metadata in the barrier.** Segments are preallocated, so Linux needs only `fdatasync`.
5. **Nothing weaker than durable.** `F_BARRIERFSYNC` costs 0.46 ms against `F_FULLFSYNC`'s 20.79 ms
   (sync-cost §1), but it survives no power loss.
6. **Several logs on one device coalesce.** Concurrent `F_FULLFSYNC` calls share a flush (P2,
   Q1), so per-core logs cost latency, not N times the flushes.

A closed-loop writer with one transaction per batch has nothing to batch. On macOS that row stays
flush-bound for both engines, at one flush per batch instead of five.

## Cost per durable batch

| | `main` (`commit()` per batch) | This design | Source |
|---|---|---|---|
| Files created, renamed or removed | one segment per commit that flushed, plus rollovers | 0 | `index.hh:4970-5040`; D1 |
| Device flushes, macOS | about 5.2 `F_FULLFSYNC` per op at `--batch 1`; 373 over 72 batches at `--batch 100` | 1 per turn, shared by the loop's indexes | sync-cost §2; D11 |
| CPU in file calls | 739 µs for an edit with a segment; 183 µs for an edit alone | the append-and-sync floor, about 25 µs, plus encoding | `manifest-log.md` "Measured", probes I, J and A |
| Bytes beyond the data | the segment | per turn, two 24-byte fragment headers and two 25-byte logical headers; the segment at the checkpoint | `framing.h:43`, `turn_log.h:80` |
| Writer stall | the whole commit, every batch | D5's stall, once per checkpoint | D5 |

These figures come from the cited probes on the development Mac (Intel, APFS). Linux `fdatasync`
on a preallocated segment is not measured.

**Targets** (manifest-log D9 asks for them):

1. 0 files per acknowledgement.
2. CPU per durable batch at most 1.25× the append-and-sync floor K'.
3. On the synchronous rows, at most 1.0× RocksDB's time and at least 1.0× its throughput (A4), with
   the tails reported.

| # | Measurement | Job |
|---|---|---|
| M1 | Calls per acknowledgement | faultfs counters (`faultfs.hh:226-237`): one sync and no create, rename or unlink per turn in steady state; a ctest assertion |
| M2 | `F_FULLFSYNC` per op, macOS | `bench/kv/barriers.c` on the `sync1` and `wal1` sets (`bench/kv/run.py:85-89`), `main` against the branch |
| M3 | The rows | `run.py` `sync1` and `wal1`; a group-commit row (4 and 16 writers on one index); an N-log row (1, 2, 4 and 8 loops on one device, one index each). Exclusive hold, A/A first, interleaved A/B against `main` and RocksDB, at p50, p99 and p99.9; macOS, then the Linux VM |
| M4 | The floor K' | `pwrite` and `sync_data` on a preallocated file in a C loop, shaped like `mlog_probe.c`, under a strict hold |
| M5 | The checkpoint | the writer's wait and the acknowledgement latency across a checkpoint, at p50, p99 and p99.9, on M3's fill rows |

## Crash model and tests

**The model** is faultfs's (`faultfs.hh:1-29`):

- **Power loss:** each unsynced 512-byte sector keeps its old bytes, takes its new ones or reads as
  zeros, and each namespace change since the last directory sync survives or not, in order.
- **A process crash** keeps every byte written.
- **A failed sync** may leave pages clean but not durable (`fault::keep`, `:61-67`).
- **Short writes, `ENOSPC` and `EIO`** can hit any operation.

The host and its logs run on faultfs unchanged: they take any `kronuz::journal::IO`, and
`File::sync_data` falls back to `sync` (completion `io.h:84`).

**The oracle** replays the transaction stream into a key-value map. Every case checks two things:

1. Each acknowledged transaction is present.
2. The state equals the map after some prefix of the apply order, a prefix at least as long as the
   acknowledged one.

| Test | Case |
|---|---|
| T1 | A power-off sweep over a run with turns, flushes, checkpoints with applies racing them (a stepping executor), a manifest-log rollover and recycled 64 KiB segments: `power_off_after(k)` for every mutation k (`:222`), then `crash(seed)` over 20 seeds. After each recovery the index writes again, and a second crash and recovery are checked too |
| T2 | A process crash at every mutation (a reopen with no `crash()`), then writes and a second check |
| T3 | A failed barrier (`EIO`, with and without `keep`): no acknowledgement, every index on the loop fenced, `outcome_unknown`. Then a reopen and a power loss: the turn's writes are all present or all absent, or the open reports `log_lost`, never a partial state |
| T4 | A read and a `refresh()` between `apply` and its barrier: neither shows the write; after the acknowledgement, both do |
| T5 | Log damage below the head's position: `log_lost` |
| T6 | Crashes between `create_group`, the attribute's edit and the first record; between the leave's checkpoint and `close_group`; and after a reopen of a closed group, before the restaged record's barrier |
| T7 | `no_space` at replay (capacity set before the reopen): the open fails with `no_space`, then succeeds once space is restored |
| T7b | Replay under `defer_io` into a smaller buffer budget than the run had, so replay meets `flush_owed` and `drain_owed` and flushes, with a power-off sweep across the replay |
| T8 | A transaction past `command_bytes` is refused with `invalid` and leaves no trace; two loop-mates that each commit a transaction near `command_bytes` in the same turn split across two turns |
| T9 | Fixtures for each record kind; an unknown version reads as `unsupported_index`; the decoders under the fuzzer |
| T10 | Runtime refusals (faultfs capacity for `no_space`; a read fault in stage 1 that throws), then accepted transactions, a crash and recovery: the refused transactions are absent |
| T11 | `RFTW0001`'s rule as D5 relies on it: a checkpoint at the last sequence plus one drops every earlier record of the group (P1's case, kept as a test) |
| T12 | A forced interleaving, through a test seam between stage 1 and the append: the sealer seals between them, the transaction's position names the record it joined, and replay is exact |

**Mutants.** Each fails its test, run as a `--fails-without` job:

| Mutant | Fails |
|---|---|
| M-a: the checkpoint record staged before the edit's sync (breaks I2) | T1 |
| M-b: the checkpoint does not await the log through X (breaks I1) | T1 |
| M-c: V published at apply | T4 |
| M-d: no cross-check against the head | T5 |
| M-e: replay skips a non-accepted verdict | T7 |
| M-f: log-first (staged before stage 1) | T10 |
| M-g: the lease released after step 1 | T1 |
| M-h: a checkpoint record staged during replay | T7b |
| M-i: an owed verdict at replay read as divergence | T7b |
| M-j: the position taken outside the append's critical section | T12 |
| M-k: `refresh()` flushes without awaiting the log | T4 |
| M-l: no command limit at admission | T8 |

## The A1 checks as jobs

| Check | Job |
|---|---|
| The suite: T1 to T12 and M1 | `campaign verify document-gate --for A1` (the build-test check) |
| A long seed sweep | `campaign job submit --quest document-gate --for A1 --minutes 30 --build "cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j4 --target standalone_turn_log_test" -- build/standalone_turn_log_test --seeds 2000` |
| Each mutant, M-a to M-l | the same job with `--fails-without --commit <mutant>` |
| Sealing and the watcher under TSan | the same build with `-fsanitize=thread`, running T1, T12 and the group-commit path, `--for A1` |
| M2 to M5 | exclusive and strict jobs, recorded in `docs/perf/standalone-turn-log.md`; A3's rows reuse them |

## Limits

- **`ack=none` against RocksDB's WAL-unsynced row.** strata's record waits in process memory until
  its turn's write, so a process crash can lose up to one barrier plus one write (about 21 ms on
  macOS). RocksDB's record is already in the kernel when `Write` returns. Under power loss the
  comparison reverses. The write is also invisible to its own writer until that barrier (Q3).
- **Silent:** media damage confined to the newest epoch's last turn, above every checkpoint, reads
  as a torn tail and drops acknowledged writes (D10).
- **Loud:** after a failed barrier whose pages Linux kept, a same-boot reopen followed by a power
  loss opens the groups in that turn as `log_lost` (D9). Separately, one damaged block whose
  manifest does not read back loses every open group on the loop that holds nothing after it
  (`turn_log.h:416-418`), even an index whose head covers its last record (Q4).
- **The writer stalls** for D5's checkpoint, once per buffer flush, and for up to two barriers per
  host refresh (D4).

## Questions for the owner

Q1 and Q2 are ruled (D104, Xapiand master); Q3 to Q5 are ruled (D103).

**Q1. Several logs on one device (macOS).** The review's probe P2 measured `F_FULLFSYNC` from 1, 2,
4 and 8 threads on their own files, under a light hold, so the shape matters more than the values:

| Threads (logs) | p50 ms | Calls per second, all |
|---:|---:|---:|
| 1 | 21.0 | 46.3 |
| 2 | 42.0 | 47.3 |
| 4 | 42.0 | 94.0 |
| 8 | 42.0 | 185.0 |

Concurrent calls coalesce: from two logs up, a barrier costs about 2× one log's latency, flat to
eight, while the flushes served grow 4.0×. A per-device log would keep the 21 ms, but it would
serialize every core on one mutex and one turn in flight. Recommendation: per core, as D11 says.
The gate's single-index rows run one log, and M3's N-log row measures the rest on macOS and Linux.

Ruled (lead, D104): one log per core, as D11 says.

**Q2. The `wal1` row's label.** Recommendation: run it as `ack=none` on the log, with D4's crash
windows stated in the row rather than claimed as equal durability.

Ruled (lead, D104): `ack=none` on the log, with D4's crash windows stated in the row.

**Q3. Read-your-writes under `ack=none`** (N3). The acknowledgement comes at apply but visibility
comes at the barrier, so a client can miss its own acknowledged write for up to one barrier.
RocksDB's WAL-unsynced row reads its own writes. Options:

1. Keep it, and state it in the row.
2. Let V follow apply on `ack=none` writes, which shows readers writes that a crash can remove.
3. Read-your-writes for the writing client only, through a session token checked against the
   applied position.

Ruled (owner, D103, Xapiand:1935c388d): option 1. The `ack=none` row states the window; quest
`turn-log-parity` closes the gap after `document-gate` lands.

**Q4. A tolerant open after log damage** (N5). One damaged block can fail every idle index on a
loop with `log_lost`, even an index whose head covers its last record. Options:

1. Keep `log_lost` for every such index, since no replica can repair it.
2. Open at H and report a possible loss, as RocksDB's WAL recovery modes do, when the head's
   position is at or past the group's last sequence that the damaged block's neighbors name.

Ruled (owner, D103): option 1, `log_lost`; quest `turn-log-parity` closes the gap after
`document-gate` lands.

**Q5. The command limit on a logged index** (review R4). A logged index refuses as `invalid` a
transaction whose command passes 64 MiB, which an unlogged index admits up to its lane and
buffer caps. The limit is the replicated path's own, so a logged index admits what a replicated
one does. Options:

1. Keep it, and note the narrowing in the gate report.
2. Log a larger transaction through a direct checkpoint of it, its position covered by the
   checkpoint record (review F4's third option), at the cost of a stall per such transaction.

Ruled (owner, D103): option 1, the 64 MiB limit, with the narrowing noted in the gate report;
quest `turn-log-parity` closes the gap after `document-gate` lands.

## Built

At this quest's branch (strata `quest/xapiand/document-gate`), first steps 1 to 3:

- **The pins.** journal `7c08540`, with the completion (`7731315`) and task (`95798a0`) it pins,
  and `completion::task`, which journal's turn driver needs, switched on.
- **The codec** (D3): `include/strata/core/command.hh`, with the transaction types moved there from
  `index.hh`, and `stored`'s part hook. Fixtures under `test/fixtures/command/` come from an
  encoder written apart from it; `fuzz/fuzz_command.cc` with its corpus under `fuzz/corpus/command/`.
- **The seal** (D3): journal `555b69f`'s `TurnDriver::seal`, `unseal` and `wake`, which replaced
  the shim of the first steps. The driver calls each index's sealer once per turn (one record per
  group per turn), with the room in `record_cost` units; `turn_framed_bytes` is a stricter bound.
  `turn_sealer` (`include/strata/core/turn_seal.hh`) stays as the synchronous host loop a blocking
  owner needs: its turn mutex is the driver's loop context, so `seal`, `records`, `release` and
  every turn run under it; it writes turns inline or serves them from a loop thread, and runs each
  group's watcher (V, D4) after a barrier. Contract changes from the shim: `unseal` returns only
  once no call of that (group, owner)'s sealer runs, so it is never called holding the record
  mutex; every checkpoint record carries `covers`, the group's last record its state includes,
  and the log refuses one behind a record it would drop (P1's case, now in T11).
- **Logged mode** (D2 to D4): `basic_index_options::log` and `log_group`; the command limit at
  admission; the append in one critical section after the cover (`index_detail::log_state`); V
  stored by the watcher with no lease; and the flush rule.
- **The checkpoint** (D5): a logged index's `commit()` awaits the log through X before the edit
  (I1) and stages the checkpoint record at the group's next sequence after it (I2), under one
  writer lease; `log_state::checkpoint` refuses a record while a batch is not yet durable, which
  is what the lease guarantees. Triggers: apply's inline write-buffer flush, and
  `checkpoint_log_bytes` of commands since the last checkpoint (not under `defer_io`, whose host
  owns its flushes).
- **Join and leave** (D7): the `turn_group` index attribute (tag 1, required: the host's identity
  and the group), and `turn_sealer`'s group-table calls on the host (`status`, `create`, `reopen`,
  `close`, `stage`, `attach`). A create joins at its first commit, whose edit carries the attribute,
  and stages the first record, a checkpoint at the head; an open of an index without the attribute
  joins with an edit of its own. `close()` leaves: a checkpoint, its record durable, then
  `close_group`; a failed leave throws before the teardown and the group stays stored. An open of
  a closed group reopens it at once, not at its first write (D6.3; accepted with D104), and
  stages the table's record as base + 1, as journal's Store does. The attribute must name
  this host and group, and an index with it opens only on its host (`log_mismatch`).
- **Recovery** (D6): `open()` replays eagerly. A lost group, a head past the log's last durable
  record, or a head older than the group's checkpoint record (a fallback open) is `log_lost`; a
  checkpoint record of another UUID is `log_mismatch`. Owed verdicts run their chore and apply
  again; `no_space` fails the open with `ENOSPC`; no flush stages a checkpoint until release.
- **Fail-stop** (D9): a failed turn fails the shim; awaits throw `outcome_unknown`, and every
  logged index on the loop fences at its next apply or log wait. A reopen keeps the failed turn
  whole or drops it (T3 on faultfs: kept with `keep` on the same boot, dropped without, and
  `log_lost` after a later power loss, as the D9 table says).
- **Acknowledgement off the owner thread:** `await_durable(position)`, for group commit.
- **`kv_strata`** (step 5, first half): `wal` and `sync` rows log through a `journal::Host` in
  `<dir>/host` with a serving loop thread; `sync` awaits its batch's last record. The kv rows' 1 MiB
  command limit keeps the host's segments at 4 MiB. A smoke run of both rows completes; M1 to M5
  are not measured yet.
- **Not built:** M-g (the lease span is one
  `writer_scope` around `commit()`, so no apply can run inside it; the invariant check in
  `log_state::checkpoint` stands in); racing applies through a stepping executor in T1.
- **Tests** (`test/standalone_turn_log_test.cc`): T1 (every mutation × 8 power-loss seeds, with
  recycled 64 KiB segments; `t1r` across a manifest-log rollover), T2, T3, T4, T5, T6 (two lives,
  join to leave and reopen to leave, cut at every mutation as a process crash and under two
  power-loss seeds, recovering groups that are none, stored and closed), T7, T7b, T8, T9, T10, T11,
  T12, M1 (32 acknowledgements: 32 writes, 32 syncs, no namespace operation), and `served`: a loop, four writers taking turns as owner with group commit and
  checkpoints, and a reader, for TSan. Mutants M-a to M-f, M-h to M-l, and M-m to M-o (fail-stop's
  fence, the checkpoint's age check, its UUID check), M-p (the attribute not checked against the
  host and group) and M-q (`close()` does not leave) run as `--fails-without` jobs at their own
  commits.

## First steps

1. The lead widens this quest's file set by `include/strata/core/**`, `test/**`, `CMakeLists.txt`
   and this file. Today it holds `docs/gates/document-profile.md`, `bench/kv/**`, `docs/perf/**`
   and `docs/mechanisms/**`. strata's journal pin moves to journal `main` (`7c08540` or later) for
   the host layer.
2. The command codec (D3), with its fixtures and fuzz target; T9 and T11.
3. Logged mode in the index: the position taken with the append, the per-transaction record, the
   watcher and V, and the flush rule; T2, T4, T8, T10 and T12.
4. The checkpoint, recovery, and joining and leaving (D5 to D7); T1, T3, T5 to T7b, and the mutants.
5. `kv_strata` on a host (`sync` through the log, `wal` as `ack=none`), plus the group-commit and
   N-log rows; M1 to M5.
6. `TurnDriver::seal` in journal, reviewed by Detent; then one record per index per turn.
