# strata review: the manifest log (preliminary design review)

- Date: 2026-11-04.
- Reviewed: `docs/design/manifest-log.md` (D1 to D11, T1 to T9, Q1 to Q3, first steps) at
  `strata:3ab5b78` and its ledger `docs/status/manifest-log.md` at `strata:9d43e76` (branch
  `design/manifest-log`, HEAD equals `origin`); the floor probe `bench/manifest-log/mlog_probe.c`
  and `results/2026-11-04-floors.txt` at `strata:3ab5b78`; the designer's unrun decode proxy
  `.scratch/mlog/decode_probe.cc`. Every code citation re-read at strata `main` `strata:2769928`
  (`include/strata/core/index.hh`, `files.hh`), journal `journal:bfad97f` (`turn_log.h`,
  `codec.h`), RocksDB 11.11 (`include/rocksdb/options.h`, `db/version_set.cc`), and
  `docs/perf/sync-cpu.md` at `strata:ed0dbee` (`explore/sync-cpu`).
- Checked against: the owner's ruling of 2026-11-04 as the design states it; durability-modes D1
  (`durability-modes.md:825-881`: DM-R3, DM-R23, DM-R43); Track C D11 (`track-c.md:487-491`,
  `:506-509`, `:556-563`, `:582-587`, `:606-612`); phase 3's retention rule
  (`phase-3.md:170-178`) and the lifecycle review's item 5 (`phase-3-lifecycle.md:142-147`);
  `media-faults.md:18-26`, `:104-108`; fields-and-keys T8 (`fields-and-keys.md:582`); the limits
  rule; every calibration row (one mechanism twice, cited baselines with raw files, quoted cells,
  mechanisms present at the cited commit).
- Checked by: probes in `.scratch/mlogrev/` on the branch's worktree:
  - `tail_probe.py`: a byte model of journal's framing at the log's parameters (512-byte blocks,
    no block manifest, epoch 1), validated against journal's own `RFTW0001` fixtures (16 block
    headers and 21 fragments verify), with D6's classifier as worded and a refined rule; output
    `tail_probe.out`. Fixtures exported to `turn-log-fixtures/`.
  - `inflight_probe.py`: two edits in flight on one log, on the same model; output
    `inflight_probe.out`.
  - `t8_model.py`: fields-and-keys T8 under D3's edit and D7's trigger; output `t8_model.out`.
  - `run_probes.sh`: `mlog_probe.c` (copied unchanged) for 3 rounds and `decode_probe.cc` (the
    designer's, built against strata `main`'s `files.hh`), under
    `quiet strict mlogrev 20 "manifest log review"`; output `run-20261104-005519.txt`.
  - Not run: no strata build or test (nothing is built yet); fuzzing; faultfs; Linux, FreeBSD and
    arm64; the `kv_strata` A/B (T3 is the lead's, on `main`).

## Verdict

**REJECT for first step 3 (the landing unit) until L1 to L3 are folded in; steps 1 and 2 may start
now, step 1 with L1's tail rule.** The mechanism is right: one append-only log per index, 512-byte
blocks so an all-zero sector means an unwritten one, the rename as the only pointer, lazy reclaim,
and the turn log as the write-ahead log beside it. Three holes remain: the end-of-file rule reads a
corrupted length as a torn tail, the edit cannot be DM D1's checkpoint of a past generation, and
retention does not say how it learns a kept generation's files in an older log.

What holds, and reproduces:

- The motivation: `sync-cpu.md:11-24` states the 1.8 to 2.1 ms per commit, the per-call costs and
  the 2.1 creates, 2 unlinks, 1 rename, 5 `fsync`s and 2 `F_FULLFSYNC`s; probe H issues exactly
  that set (`mlog_probe.c:166-198`), probe I exactly T2's macOS set (`:200-220`), and the rerun
  reproduces I at 0.51× H (the design's 0.47×) with one device flush instead of two.
- D2's 512-byte block is load-bearing and necessary: of the single-bit flips over the blocks
  holding the last two records, 4 of 6,472 read as torn at 512 bytes, all in one field (ML-R1),
  and 16,905 of 22,960 at the turn log's 32 KiB, where a payload's zero run fills whole sectors and
  the block is the whole 2,870-byte log (`tail_probe.out` checks 2 and 3).
- Honest crashes read right: every subset of sectors persisted and every size a crash can leave
  (E, E+n, any 512-byte multiple between) for the last three appends, a 700-byte edit spanning
  blocks included, opens the last whole generation and never reports damage (check 1, 28 images,
  under D6 and under L1's rule).
- D5 holds: `recover` already lists the directory (`index.hh:4889-4893`), so the highest
  final-named log is found without a CURRENT file, and every crash row of D6's table reads as
  stated.
- D8's dropped order holds: the lifecycle review already found `retain()`'s order not durable and
  harmless only because `recover --to` fails closed (`phase-3-lifecycle.md:142-147`); `open()`
  reads only the highest log, which a rollover's directory sync makes durable before the old log
  is queued.
- The RocksDB citations are exact: `max_manifest_space_amp_pct` is
  `max(max_manifest_file_size, est × (1 + pct/100))`, default 500 (`options.h:1071-1111`), over
  1 GiB (`:1032`); `reuse_manifest_on_open` makes a fresh MANIFEST when the size differs from the
  last valid record's end (`version_set.cc:6731-6746`).
- D9's split matches DM D1's three steps and Track C's Condition 25 order (`track-c.md:606-612`):
  the edit is durable before the checkpoint record is staged, and obsolete files leave after the
  record's barrier (DM-R43). Sharing journal's framing keeps one framing implementation, as
  RocksDB's WAL and MANIFEST share `log::Writer`.
- D10's descriptor, heap and disk costs hold by arithmetic (its open cost is ML-R3 and ML-R7):
  one descriptor more only while hot, closed at demotion; 32 bytes of heap; 4 to 16 KiB on disk
  against `main`'s 12 to 16 KiB. No user-visible cap is added: a record has no size limit
  (fragments chain), and a log reaches 2 TiB (a 32-bit block number at 512 bytes) before its block
  number runs out.

What blocks: L1 (ML-R1), the end-of-file rule's silent loss; L2 (ML-R2), the edit of a past
generation; L3 (ML-R3), retention across logs.

## Findings

**ML-R1. D6's end-of-file rule reads a corrupted length as a torn tail (blocking, L1).**
D6's table (`manifest-log.md:202-206`) calls a record torn when it "runs past the end of the
file". A fragment's CRC covers its length (`turn_log.h:152-157`), but it cannot be checked when the
length points past the end of the file, so a flipped length bit in the last fragment that grows it
inside its block opens the previous generation with no error: `tail_probe.out` check 2, 4 of 6,472
single-bit flips over the last two records, all in the fragment length field. T4's own damage clause
("any bit flipped in any synced record fails the open", `:380`) fails on D6 as written.
The rule can be exact at no format cost. One write per append leaves a crash's file size at E, at
E+n, or at a page multiple (ext4 and XFS raise the on-disk size by written-back pages, capped at
the in-memory size, by their sources; APFS, ZFS and UFS not checked here), and at 512-byte blocks
every page boundary is a block boundary, so an honest crash never cuts a fragment. Check 4 applies
that rule: 0 of 6,472 flips read as torn, and check 1's 28 honest images still read right. Where a
filesystem left another size, the open would fail closed, an availability loss and never a silent
one, which is the side to err on.
Fix (L1): torn by the end of file only when the file ends at a 512-byte multiple between whole
fragments of an incomplete record; a fragment whose declared extent passes the end of file is
damage. The writer issues one write per append, and a failed or short append is truncated back to
E and synced before the commit reports, the log's form of `index.hh:927-928`'s "the partial file
removed" (a failure there fences, and a crash inside that window fails closed). T4's torn-tail
fuzz enumerates the sizes a crash can leave, not every byte truncation, and adds an `ENOSPC` short
write followed by a crash.

**ML-R2. D3's edit cannot be DM D1's checkpoint of a past generation (blocking, L2).**
D9 makes the edit DM D1's checkpoint (`manifest-log.md:286-290`). DM checkpoints "the newest
refresh generation whose position is at or below `commit_durable`", because apply runs ahead and a
checkpoint of the current state "would hold writes D6 can remove", which `Repair` would restore
while skipping their replacements, and the replica would diverge silently (DM-R3,
`durability-modes.md:836-842`). D3 builds "the difference between the durable state and the next
one" from one per-segment list that `apply_pending` fills "for the next edit"
(`manifest-log.md:141-147`), so deletes applied after the checkpointed generation enter its edit.
For a standalone `commit()` the head is the current state and nothing changes; as K2's
checkpoint, the mechanism produces exactly the divergence DM-R3 forbids.
Fix (L2): an edit is the difference between two named generations, the durable or recorded one d
and a target c at or below the current; the list of new deletes keys each ordinal by the
generation that applied it (or the edit diffs c's deleted sets against d's), and the same holds
for metadata keys. T6's oracle includes a target c behind pending deletes and metadata changes
(DM's T16 shape), with "the list without its generation key" as the mutant.

**ML-R3. Retention does not say how it learns a kept generation's files in an older log
(blocking, L3).** D8 keeps "the head, the pinned ones, the `retained_predecessors` before the
head and the recorded generation", and obsoletes "every file no kept generation names"
(`manifest-log.md:256-262`). A kept generation can sit in an older log: a pin taken before a
rollover, a predecessor across it, the recorded generation while a rollover outruns its
checkpoint record. `main` reads each kept manifest (`manifest_files`, `index.hh:4770-4785`) and
removes nothing when one cannot be read (`retention_blocked_`, `:4717-4720`); after a reopen the
log design has only the head's state in memory. Retention that knows only the current log's
generations reclaims a pinned generation's merged-away segments, and D6.2's "one read" and D10's
"open reads one log" then hold only when no kept generation is outside the current log.
Fix (L3): D8 states that retention replays each log holding a kept generation up to it (cached, as
`manifest_files` caches), that a log that cannot be read or ends before that generation blocks
retention, and D10 and T7 count the extra read; or the snapshot carries the file list of each
kept generation in an older log, so the open reads one log. T4 adds a pin taken before a rollover,
a merge, a reopen and a retention round, with retention from the current log alone as the mutant.

**ML-R4. `open(..., to)` on a log that ends before `to`.** D6.8 fails only on a missing file
(`manifest-log.md:217-220`). An obsolete log can be dirty, or truncated by reclaim's steps
(`index.hh:3582-3592`) before its unlink; replaying it to its end opens a generation below `to`.
State that `to` itself must replay whole, else "generation not retained"; add it to T4.

**ML-R5. The commit allowance does not cover a rollover.** `commit_allowance()`
(`index.hh:3671`) holds what the next commit writes so "a commit that frees space can always be
written". Any commit can roll over (D7), and D4 forces a rollover after a failed append
(`manifest-log.md:171-173`), so under `ENOSPC` the commit that frees space needs a whole snapshot
while the old log stays until reclaim. Count the snapshot plus the edit in the allowance (L1's
truncate-back also keeps the retry an append), and add the transient snapshot to D8's disk table
and T5.

**ML-R6. Fields-and-keys T8 does not hold as claimed (`manifest-log.md:64`, `:384`).** The
rollover rule names its mechanisms (the open's replay for 2 S, the rollover's fixed cost for the
floor, probes F and G) and cites RocksDB exactly, but neither constant is checked where it binds:
the floor against the open's replay (ML-R7), and 2 S against this row, the one where S grows with
every edit. T8 bounds the bytes written for 2,000 and 100,000 declarations, one per commit, at 4×
the final table (`fields-and-keys.md:582`). Every edit carries every scalar (D3's table, `:126`),
about 45 bytes by estimate, plus 24 of fragment header around a 20-byte declaration, and each 2 S
rollover rewrites the whole table after S bytes of edits, so the table grows only about 20%
between rewrites:
`t8_model.out` gives 5.3× and 6.2× the final table in table bytes, 10.7× and 11.8× in all bytes.
Edits carrying only the scalars that changed and a 4 S trigger give 2.0× and 4.4×. The rule's
"at most one snapshot byte per edit byte" (`:241-242`) is per edit byte; where every edit grows S
(declarations, a large segment's deleted set) the multiple is set by how much S grows per
rollover. In all bytes the log still writes far less than `main`, which rewrites a 700-byte
manifest per commit. Fix: a presence mask for changed scalars, and T8 restated in the log's terms
with its predicted multiple; a change to T8's 4× is an amendment for the lead and the owner.

**ML-R7. T7's targets accept a loss, and the replay is unmeasured.** A fresh log is one file where
`main`'s open reads the manifest, metadata and field table (`index.hh:4933`, `:5034-5047`), at
about 12 µs per file (probe L), so "within 1.1× of `main`" (`manifest-log.md:383`) accepts a loss
where a win is expected, and "1.25×" at the floor accepts a 25% slower open on the many-indexes
workload, whose metric is per-open-index cost. The designer's decode proxy, run here, puts the
at-floor open at 1.3 to 1.9× `main`'s and the fresh one at about 0.4× ("Measured floors, rerun").
Fix: at most 1.0× for a fresh log; the at-floor target set from a measured replay of 125 edits;
Limits' "rolls over at close past half its floor" (`:402-404`), or a floor sized by replay time,
becomes D7's rule, its write per close counted in T9.

**ML-R8. "The open writes nothing" against today's open.** D6.6 says so (`:210-213`), while D6.1
removes `.tmp` orphans "as today" (`:193-194`), and today's open also runs `retain()` and
`reclaim()` (`index.hh:4958-4959`), which unlink and sync the directory (`:4749-4758`). State
what the open does: unlinks without a directory sync, retention queued or deferred to the first
commit.

**ML-R9. One record in flight under batching.** D4's premise holds for `commit()` under the write
lease (`:168-170`). DM D1's background checkpoints and small-indexes' batcher must also never
append a second edit to a log before the first's barrier returns, or an honest crash leaves a bad
record before a whole one, which D6 reads as damage (`track-c.md:506-509`'s reason):
`inflight_probe.out`, two unsynced edits over two sectors, 1 of 4 honest crash images fails the
open. Make it an invariant of the log writer, with a T4 case under the batcher.

**ML-R10. Q3's premise.** The Limits cite `media-faults.md:22` for lost writes being outside the
model; that row scopes it to the consensus journal, and `:104-108` gives the reason ("without an
independent record"). `main` fails closed on a lost write of a manifest's bytes, since a renamed
manifest is synced whole and zeros fail its CRC. The log moves the index's metadata into the
journal's position: that is what Q3 asks the owner, and the Limits should say the design extends
that row's scope to the index's metadata. With L1's rule, the silent case is exactly an all-zero
512-byte piece of the last record.

**ML-R11. T3's threshold sits in its procedure's noise.** The explorer's A/A spread is 17%
(`sync-cpu.md:33`); the expected result, if the unattributed 0.9 ms stays, is about 0.71× (1,904 µs
less the 548 µs between H and I), against a 0.75× target. T3 needs more rounds or a count-based
bound, and must state the barrier mode for its latency half (the explorer ran `BARRIERS_WEAK`).
K2's targets in T1 and T3 belong to K2's design; its 170 µs sits 8% above the rerun's K' floor
(158 µs).

**ML-R12. Replay checks are quadratic as worded.** "After each record the totals equal the sums
over segments" (`:138-139`) recomputed per record costs edits × segments at open; keep running
sums.

**ML-R13. The classifier belongs to journal's component.** D2 places the growing-file read and the
tail classifier there (`:112-114`); keep it so. K2's standalone group of one will need a
torn-versus-damage rule, and the turn log's rule reads any damage in the last turn as torn
(`track-c.md:560-562`); one classifier parameterized by block size lets K2 choose 512-byte blocks
instead of a second rule. The shared layer is blocks and fragments: the turn log's 25-byte record
header (`turn_log.h:82`, `:118`) stays its own.

**ML-R14. T9 has no target.** "Files and filesystem blocks per small index against `main`"
(`:385`) needs one: at most `main`'s.

## Measured floors, rerun

`run_probes.sh` under `quiet strict mlogrev` on the development Mac, 2026-11-04 00:55 to 00:58,
three rounds, `run-20261104-005519.txt`; CPU µs per operation, median (range):

| Probe | Design | Rerun |
|---|---:|---:|
| A append, `fsync` | 30.4 | 25.4 (24.9-58.9) |
| F rollover, 1 KiB | 444.8 | 693.6 (692.5-732.0) |
| H `main`'s commit | 1,041.3 | 1,455.8 (1,331.5-1,519.2) |
| I the log's commit | 492.8 | 738.7 (731.7-744.5) |
| J edit alone | 137.0 | 183.1 (176.6-188.4) |
| K' turn-like, `F_FULLFSYNC` | 135.0 | 158.0 (156.2-183.5) |
| L open and read 4 / 64 KiB | 12.1 / 15.4 | 15.6 / 16.9 |

- The ratio reproduces: I is 0.51× H (the design's 0.47×), outside both spreads, with one device
  flush instead of two (23 to 24 ms of wall against 45 ms). The absolute numbers do not: all but A
  ran 10 to 58% higher on the same host, so the design's floors are one run's, and T3's K2 target
  of 170 µs sits 8% above this run's K' (ML-R11).
- The decode proxy (`decode_probe.cc`, strata's `decode_manifest`): 0.32 µs for a one-segment
  manifest of 113 bytes, an edit's size; 3.4 µs for 30 segments (1,217 bytes); CRC-32C 0.008 µs
  per 113 bytes. Replaying 125 edits costs at least 40 µs beside one 16 KiB read (about 16 µs),
  before applying them, while `main` opens a small index by reading two or three files at about
  15 µs each: by this proxy an at-floor log opens at 1.3 to 1.9× `main`'s time, past T7's 1.25×,
  and a fresh log at about 0.4× (ML-R7).

## Questions for the owner

- **Q1 (order).** Option 1, the manifest log first: K2 needs a checkpoint either way, this is it,
  and it lands a win alone; option 2 builds K2 on today's manifest and migrates it later. It
  holds only with L2 folded in, so K2 builds on the edit instead of reworking it.
- **Q2 (`retained_predecessors`, the owner's: it narrows an option).** Option 1 keeps the option
  at default 0 and costs nothing; option 2 keeps today's protection in kind (damage to the newest
  metadata falls back to an older separate file) for one more log and its last generation's files;
  option 3 retires it. Under option 1, the head's generation is unknown after damage, so
  `fallback()` reports "damage at generation x" instead of a count of dropped generations, and the
  host's recorded newest generation (`phase-3.md:176-177`) chooses `to`. Option 1 or 2; not a
  blocking item either way.
- **Q3 (lost write).** Option 1 now, with ML-R10's framing, and option 3 once K2 lands. Option 2
  adds a barrier per commit and undoes D4.

## Blocking items / Required changes

- **L1 (ML-R1).** D6's end-of-file rule: torn only at a 512-byte-multiple end between whole
  fragments; a fragment cut by the end of file is damage. One write per append; a failed append
  truncated back to E and synced. T4: crash sizes enumerated, a short write then a crash, and
  `tail_probe.py`'s flip set with zero silent results.
- **L2 (ML-R2).** D3: an edit is a diff between two named generations; new deletes and metadata
  keys keyed by generation; T6's oracle covers a past target with later changes pending.
- **L3 (ML-R3).** D8: retention replays each log holding a kept generation, or the snapshot carries
  those file lists; an unreadable one blocks retention; D10 and T7 count the reads; T4's pin across
  a rollover.
- Required, not blocking: ML-R4 to ML-R14 folded into D6, D8, D10 and the gate rows.

## Conditions

- **C1.** Step 1 lands the classifier in journal's component with L1's rule and a fuzz target over
  it, and changes no byte of `RFTW0001` (its fixtures byte for byte).
- **C2.** At the landing review: T4 includes ML-R4's short log, ML-R5's `ENOSPC` rollover, ML-R9's
  batched commits and L3's pin across a rollover; T7 and T8 carry measured numbers against the
  targets ML-R6 and ML-R7 set.
- **C3.** K2's design adopts or rejects the shared classifier explicitly (ML-R13).

- Not run: strata builds and tests, fuzzing, faultfs crash runs, Linux, FreeBSD and arm64, the
  `kv_strata` A/B and every gate row (the lead's, on `main`).
