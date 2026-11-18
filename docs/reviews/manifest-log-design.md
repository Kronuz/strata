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

## Re-check

- Date: 2026-11-04.
- Reviewed: revision 1 of `docs/design/manifest-log.md` at `strata:1c8ff50` and its ledger at
  `strata:b2358de` (`git diff 057c47e..HEAD`: the design, the ledger, `bench/manifest-log/`
  `tail_model.py`, `t8_model.py`, `open_probe.cc`, `run_r1.sh`, `results/2026-11-04-r1.txt` and
  `results/2026-11-04-floors-rerun.txt`). Code citations re-read at strata `main` `strata:2769928`
  (`index.hh`), completion `completion:e2c6474` (`posix.h`), `durability-modes.md` and
  `fields-and-keys.md` at `strata:2769928`.
- Checked by: one run of `.scratch/mlogrev/run_recheck.sh` under
  `quiet strict mlogre 20 "manifest log re-check"`, 2026-11-04 02:57 to 03:00, exit 0, output
  `.scratch/mlogrev/recheck-20261104-025758.txt`:
  - `tail_probe_r1.py`: D6's revision-1 table implemented from the design's words, independently of
    the designer's classifier, over this review's framing model (`tail_probe.py`) and four logs of
    its own (the review's, fragment ends on and near block ends, edits over seven blocks, 40 random
    edits). `stale_wrong.py` explains its check 6.
  - The designer's `tail_model.py` (with journal's `RFTW0001` fixtures at `journal:bfad97f`),
    `t8_model.py` and `open_probe.cc` (3 rounds of 3,000), rerun to this directory, not to
    `bench/manifest-log/results/`.
  - `t8_mixed.py`: T8's declarations with other edits in the same commits, on the designer's
    arithmetic (imported).
  - `rollover_model.py`: D7's revision-1 trigger by index shape, against a replay bound scaled by S.
  - `clean_sync_probe.c`: one `fsync`, `F_BARRIERFSYNC` or `F_FULLFSYNC` at open on a log with no
    dirty pages, against open and close alone (ML-R15's price).
  - Not run: strata builds and tests (nothing is built), fuzzing, faultfs, crash sizes on APFS, ZFS
    or UFS, Linux, FreeBSD and arm64, `bench/manifest-log/run_r1.sh` itself (it writes the
    committed results file; its three parts were rerun instead).

### Verdict

**APPROVE WITH CONDITIONS: first step 3 may build on revision 1; L1 to L3 are closed, and three new
findings fold in as conditions before step 3 lands, ML-R15 before its recovery code is written.**
Steps 1 and 2 may start now, as before: step 1 with D6's table as revision 1 words it.

What holds, and reproduces:

- **The tail rule (L1).** The independent implementation reads every case right: 4,444 honest crash
  images (every append, every sector subset, every size a crash can leave), 0 misread, 3,686 torn;
  307,752 single-bit flips over every byte of four logs, 0 silent, 192 in padding no reader reads;
  50,000 random 1-to-16-byte corruptions, 0 silent; 202,000 images of a torn last append with a
  flip anywhere in the record before it, 0 silent. Stale bytes in unwritten sectors (ext4
  `data=writeback`) fail closed in 3,150 of 4,444 images and open the right generation in the rest;
  the 3 the probe first flagged are whole records whose stale bytes sit in block padding
  (`stale_wrong.py`), read right. The designer's `tail_model.py` reproduces its committed output
  line for line (1,020 images, 0 misread; 219,672 flips, 0 silent; revision 0's wording 18 silent;
  33 of 76 two-in-flight images damage; 21,846 of 21,962 short writes fail closed).
- **The change list (L2).** Every site D3 names is where it says: `apply_pending` the one place a
  bit is newly set in an existing segment (`index.hh:4293-4305`); `set_meta` and `delete_meta`
  push undo steps (`:4072-4086`) that `rollback` reverses (`:3879-3886`); `build_view` shares the
  bits, metadata and table a mark would stamp (`:2794-2825`). No site clears a deleted bit: the
  others move a set whole (`:1040`, `:1642`) or recount one (`:5141`).
- **The open model (T7).** `open_probe.cc` reproduces within 5%: `main` 82.1 µs, a fresh log 36.1
  (0.44×), 128 commit-mix edits 63.6 (0.78×), 128 delete-only edits 45.9 (0.56×), 16 KiB of the
  commit mix 72.3 (0.88×), medians of three rounds. The design's correction of the review's 1.3 to
  1.9× stands: that proxy left out `main`'s scan, which opens every file for its size
  (`index.hh:4899`). The decoder is a model; T7 measures strata's.
- **T8's arithmetic** reproduces exactly (2.46× and 3.38×; either change alone 4.19× to 6.03×).

### Status

- **L1 (ML-R1): closed.** D4 makes one `write_at` per append and truncates a failed one back to E
  and syncs it before the commit reports (`manifest-log.md:234-248`); D6's table decides from the
  failing fragment's own piece, a cut fragment is damage (`:279-301`); T4 cases 2 to 4 carry the
  crash sizes, the flip set and the short write with mutants (`:593-602`); D11's fixtures include
  each torn kind, a cut and a truncated-back append (`:481-484`). The rule differs from the
  review's refined one (zeros in the failing piece instead of a 512-byte-multiple end) and is the
  better of the two: both read check 4's two faults right, and the design's rule never consults
  bytes after the failure (`:290-294`, "Rejected" `:660-662`).
- **L2 (ML-R2): closed.** D3 builds the edit between a durable head d and a target c at or below
  the current state from the two snapshots and one marked change list (`:189-212`), which keeps a
  delete applied after c out of c's checkpoint; T6's oracle takes past targets with the two
  mutants (`:213-215`, `:583`); D9 names the edit as durability-modes D1's checkpoint (`:432-437`).
- **L3 (ML-R3): closed.** D8 reads a kept generation in an older log by replaying it to k, caches the
  result, and blocks retention on an unreadable or short log as `retain()` does
  (`:387-396` against `index.hh:4716-4720`); D10 counts the first retention's reads (`:467`); T4
  case 6 pins across a rollover with "the current log alone" as the mutant (`:604-606`); T7 reports
  it (`:584`).
- **ML-R4: closed.** `to` must replay whole, else "generation not retained" (`:313-319`); T4 case 5
  (`:603`).
- **ML-R5: partially closed.** The truncate-back keeps a retry an append under `ENOSPC`
  (`:242-247`), and the rollover's transient is in D8's table, T5 and T4 case 7 (`:421`, `:582`,
  `:607`). Open: D8 says an allowance of S plus an edit "come[s] to about the same" as today's
  (`:411-415`). It does not for an index with large deleted sets: `main`'s standing allowance counts
  the deleted bits of segments with deletes since their last live-docs file, metadata only when it
  changed, and only the table's new block (`index.hh:3667-3685`), while S counts every segment's
  deleted set, all metadata and the whole table. On `rollover_model.py`'s large shape (50 segments
  of 10 million documents) the log's standing allowance is about 75 MB against about 1.25 MB per
  segment a commit's deletes touch. Fix (C5): hold S only while a rollover is forced (a dirty log,
  install, `open(..., to)`) and let D7's soft bounds wait one commit under space pressure; or state
  the larger standing reservation in D8 and D10 and count it in T5.
- **ML-R6: closed for the mechanism; T8's measure is the lead's amendment.** The presence mask and a
  snapshot of the current state are in D3 and D7 (`:167`, `:177-180`, `:344-351`) and the arithmetic
  holds. T8's restatement (`:585`) keeps the 4× and changes what is counted: the declarations in
  edits and the table in snapshots, without the fragment headers, block headers and edit heads that
  carry them, where `main`'s 1.37× counts its file's own framing. That fits T8's purpose, written
  into the row as "rewriting it whole on each commit writes O(F²)" (`fields-and-keys.md:582`): both
  measures show the quadratic term gone. Two numbers belong beside it. In all bytes the log writes
  5.94× and 6.94× the final table file, against `main`'s estimated 6.57× and 6.04× with its
  manifests (`t8_model.py`; the log 10% less at 2,000 fields, 15% more at 100,000). And the 4× holds
  on T8's own workload only: a rollover rewrites the table whenever any edits fill 2 S, so with a
  segment flush in each declaring commit the table bytes reach 5.37× and 6.55×, and with three
  deletes in each 4.25× and 4.84× (`t8_mixed.py`), still linear. Accepting the restatement is the
  lead's call; C6 makes T8 report both.
- **ML-R7: closed.** R = 128 edits with its measured basis (`:365-374`); T7's targets at most 1.0×
  fresh and at R, 1.1× for a large index (`:584`); the open probe in "Measured" (`:522-550`),
  reproduced above.
- **ML-R8: closed as asked** (`:304-309`), and the change opens ML-R15.
- **ML-R9: closed.** One record in flight is the log writer's invariant over `commit()`, the batcher
  and background checkpoints (`:249-255`); T4 case 1 with its mutant (`:590-592`).
- **ML-R10: closed.** The Limits extend media-faults' lost-write row to the index's metadata in so
  many words (`:613-620`), and Q3 asks it (`:711-719`). ML-R17 sharpens the wording.
- **ML-R11: closed.** T3: an A/A first, seven interleaved pairs, the worst pair against the target,
  the barrier mode named (`:580`); K2's targets leave for K2's design (`:449-450`).
- **ML-R12: closed.** Running sums, compared when carried (`:185-188`).
- **ML-R13: closed.** One log component in journal, parameterized by block size, the turn log's
  record header its own, K2 adopting or rejecting it explicitly (`:150-155`).
- **ML-R14: closed.** T9: at most `main`'s files and blocks per small index (`:586`).

### New findings

**ML-R15. The open reports a head that may not be durable (condition C4).** D6.6 has the open sync
nothing (`:304-309`). After a process crash, not a power loss, the page cache still holds an edit
whose `pwrite` returned and whose `sync_data` never ran: the reopened index reads it whole and
reports its generation, and a power loss before the next barrier takes that generation back after
readers, the host's recorded newest generation (`phase-3.md:176-177`) or durability-modes' recorded
generation (DM-R43) saw it. A page whose writeback failed is worse: clean in the cache and absent on
disk, so the next commit's `sync_data` never rewrites it, and after a power loss the log reads as
damage with a whole record after it. `main` with the default `retained_predecessors` 0 does not have
the first case: the manifest's bytes are synced before the rename (`index.hh:4652-4656`), and when a
crash leaves the previous manifest, the open's `retain()` unlinks it and syncs the directory
(`:4750-4758`, called at `:4958`). Durability-modes already ruled on the same window for the turn
log, DM-R2: recovery rewrites the turns after the last synced position and syncs them, and a voter
reports `synced` only for turns a barrier of its own process covered (`durability-modes.md:1019-1029`).
Options, one to pick in D6.6:
1. The open issues one `sync_data` on the log when it opens it for writing (recommended). On a log
   with no dirty pages it costs 0.3 to 2 µs of CPU over open and close alone, and 30 to 300 µs of
   wall for `F_FULLFSYNC` on this Mac (`clean_sync_probe.c`: 13.1 to 13.9 µs open and close, 14.2
   to 15.1 µs with `F_FULLFSYNC`); it pays a device flush only after a crash left dirty pages, the
   case it exists for. A failed writeback then reports at that sync and fences. Linux's `fdatasync`
   on a clean file is T2's to measure.
2. The open reports nothing as durable until a barrier of this process covers the log (the first
   commit's `sync_data`, or a close), as DM-R2 does for voters, and the Limits state that a read-only
   reopen after a process crash can serve a generation a power loss takes back.
3. The first commit after an open rolls over, rewriting the state from memory as DM-R2 rewrites
   turns: about 694 µs (probe F) instead of 183 µs once per open that commits.
Either way, T4 adds a process kill after the append's `pwrite` and before its `sync_data`, a reopen,
then a power loss, with "the open syncs nothing and reports the head durable" as the mutant.

**ML-R16. The replay bound writes a whole snapshot per R edits, unbounded against the edits
(condition C5).** D7 says snapshots add "at most one byte written per byte of edits" (`:352-356`).
That holds for the 2 S bound; the max(R, n) bound fires first wherever S is large and n small, and
then each rollover writes S per max(R, n) edits. On `rollover_model.py`'s shapes (S by D3's
encodings, deleted sets as bitsets, 1.2× framing): 10 segments of 100,000 documents write 1,193
bytes of snapshot per commit, 12 to 21× the edit; 50 segments of 10 million documents write 278 KB
per commit, 2,894 to 4,874× the edit, plus a 75 MB write inside every 270th commit. It is not a loss
against `main`, which writes a live-docs file per segment a delete touches (up to 1.25 MB each on
the large shape), but it throws away most of the log's win on exactly the indexes with large
deleted sets, and no row sees it: T5 checks the bounds hold, not what they write, and T8 has no
deleted sets. The bound exists to cap replay where bitsets make S cheap to decode (`:357-364`), so
scale it by what a snapshot costs to decode: max(R, n, S / c) edits, c the snapshot bytes that decode
in one edit's replay time, measured in T7. At c = 512 the large shape writes 512 bytes of snapshot
per commit (5 to 9× the edit) and replays 10 to 29 ms of edits beside a snapshot that decodes in
16 to 63 ms (estimates: bitsets at 0.25 to 1 ns per byte, unmeasured); at c = 128, 128 bytes per
commit and up to 117 ms. Fix: that bound, or keep max(R, n) and correct D7's sentence; either way T5
reports bytes written per commit for a large deleted-set index under churn, beside `main`'s.

**ML-R17. The silent class is wider than "the last edit" (text, in C6).** The Limits and T4 case 3
name a lost write of the last edit (`:597-599`, `:613-620`). Two shapes lose more, both lost writes
within Q3's scope: a sector the last append rewrote that reads as zeros also drops the earlier edits
that share it (`tail_probe_r1.py` check 4: 11 silent zeroed sectors, all in the last record's
sectors, dropping 1, 2 or 3 acknowledged edits); and a size reverted past more than one append drops
every edit after it (check 5: one cut at a 512-byte multiple dropped up to 39). State the class as
lost writes of the log's tail: every edit from the earliest lost sector or size.

### New blocking items

None.

### Conditions

C1 to C3 stand. New:

- **C4 (ML-R15).** D6.6 picks one of ML-R15's options before step 3's recovery code is written, and
  T4 adds the kill between `pwrite` and `sync_data`, a reopen and a power loss, with its mutant.
- **C5 (ML-R5, ML-R16).** Before step 3 lands: D8's allowance holds S only while a rollover is forced,
  or D8 and D10 state the larger standing reservation; D7's replay bound scales with S, or D7's "one
  byte per edit byte" is corrected; T5 reports bytes written per commit for a large deleted-set
  index beside `main`'s.
- **C6 (ML-R6, ML-R17).** T8 reports all bytes beside `main`'s and a run with a segment flush in each
  declaring commit, and its 4× reads as "on T8's workload"; the lead records the restated measure as
  an amendment to `fields-and-keys.md` T8 or amends it. The Limits and T4 case 3 state the silent
  class as ML-R17 words it.
- **C7 (D7's S).** The writer's S is the encoded snapshot's size, framing included, after every
  edit: T6's property test checks it equal to `encode(snapshot).size()` with "S from the last
  snapshot written" as the mutant, since block headers and padding make the framed size depend on
  position and T5 trusts it.

- Not run: strata builds and tests (nothing is built), fuzzing, faultfs crash runs, crash sizes on
  APFS, ZFS and UFS, Linux, FreeBSD and arm64 (including `fdatasync` on a clean file), the
  `kv_strata` A/B and every gate row (the lead's, on `main`).
