> Status: ACCEPTED (2026-11-09) revision 1, building (first step 3): folds in the preliminary review
> (`docs/reviews/manifest-log-design.md` at `strata:057c47e`: L1 to L3, ML-R4 to ML-R14) under the
> owner's ruling of 2026-11-04, RocksDB's append-only manifests, replaced when they roll over and
> cleaned up lazily; the architect's re-check (`strata:79beaf4`) approved it with conditions C1 to
> C7, folded in below (C4: D6.6 and T4 case 9; C5: D7 and D8; C6: Limits and T4 case 3). The owner
> answered Q1 to Q3 on 2026-11-09 (Xapiand's campaign D41). Ledger: Xapiand's campaign journal
> `manifest-log`.

# The manifest log: commit metadata as an append-only log

Cited at strata `main` `2769928` (every `index.hh`, `files.hh` and `fields.hh` line below; `main`'s
head `9e344d8` changes no code); journal `main` `bfad97f` for the turn log
(`include/journal/turn_log.h`); completion `e2c6474`, strata's pin, for the file calls
(`include/completion/posix.h`); the sync-CPU note `docs/perf/sync-cpu.md` on `explore/sync-cpu`
`ed0dbee`; RocksDB 11.11's source (`~/.scratch/rocksdb-11.11.fb`), read for its MANIFEST, CURRENT
and obsolete-file code, none of which enters strata. Measured, on the development Mac (Intel
i9-9980HK, APFS, macOS 15.8.1) under strict holds: the floor probe `bench/manifest-log/mlog_probe.c`
as the architect reran it (`results/2026-11-04-floors-rerun.txt`), and revision 1's probes in one run
(`results/2026-11-04-r1.txt`): `tail_model.py`, D6's rule over a byte model of journal's framing;
`t8_model.py`, fields-and-keys T8's arithmetic; `open_probe.cc`, an open's file work and replay, with
strata's own decoders for `main`'s files and a model of D3's records for the log. Every other number
is the cited code's or document's, arithmetic, or a labeled estimate.

## Summary

An index's durable state, apart from its segment files, becomes one append-only file per index, the
**manifest log** `manifest-<g>`: a snapshot of the state at generation g, then one edit per commit. A
commit appends its edit in one write and issues one barrier. When the log grows past twice the bytes
of a snapshot of the current state, or holds more edits than that state has entries (each bound with
a floor), the next commit writes a fresh snapshot under a temporary name and renames it into place,
today's commit protocol run once per log instead of once per commit. The old log and obsolete
segments leave through the existing reclaim queue, lazily, with no directory sync owed per commit.

Live-docs, metadata and field-table files fold into the log, so a commit creates a file only for a
segment it flushed. The framing is journal's turn-log framing at 512-byte blocks, one implementation
for both durable logs. At 512 bytes no fragment crosses a sector, so the tail rule is exact: a
fragment that fails its check is torn only when its sector reads as never written, and a damaged edit
fails the open instead of being dropped. On a byte model of the framing, none of 219,672 single-bit
flips reads as torn and every honest crash image reads right.

An edit is the difference between two named generations, so it can be durability-modes D1's
checkpoint of a past generation. Retention reads a kept generation that lives in an older log from
that log, and removes nothing, as today, when it cannot.

The same file work in a plain C loop on the development Mac: a synchronous commit that flushes a
segment drops from **1,456 to 739 µs of CPU** (2.0× less) and from two device flushes to one (44.9 to
24.2 ms); a commit with no new segment, an edit alone, costs **183 µs** ("Measured"). Opening a small
index reads one file: by the open probe's model, 0.43× `main`'s CPU with a fresh log and 0.74× at
its replay floor.

The relation to the turn log: **the turn log builds on the manifest log.** The turn log stays the
write-ahead log (transactions, one barrier per core per turn); the manifest log is the checkpoint its
group record names. The 0-file synchronous commit at the append floor needs the commit's documents in
a log that already exists, which is the turn log's job (rocksdb-survey K2); without it a synchronous
commit still flushes one segment file. Q1 recommends the manifest log first, which the review agreed
with given L2.

## Revision 1

What the preliminary review asked, and where it is:

| Finding | Change |
|---|---|
| L1 (ML-R1), the end-of-file rule | D6: a fragment cut by the end of file is damage; torn only where its piece reads as never written, or at a 512-byte end between whole fragments. D4: one write per append; a failed append truncated back and synced. `tail_model.py` checks it on the model |
| L2 (ML-R2), the edit of a past generation | D3: an edit is the difference between two named states; deleted ordinals and metadata keys come from one list stamped with marks. T6's oracle takes a past target |
| L3 (ML-R3), retention across logs | D8: retention reads a kept generation from the log holding it, cached; one it cannot read blocks retention, as `index.hh:4716-4720` does. T4 pins across a rollover |
| ML-R4, `open(..., to)` | D6.8: `to` must replay whole |
| ML-R5, the allowance | D8: the commit allowance holds a snapshot and an edit; the rollover's transient in D8's table and T5 |
| ML-R6, T8 | D3's presence mask and D7's current-state bound: T8 at 2.46× and 3.38× by arithmetic |
| ML-R7, T7 | D7's replay floor R; T7's targets at most 1.0× `main` |
| ML-R8, the open's writes | D6.6: the open creates nothing and syncs the directory and the log (ML-R15); retention waits for the first commit or `reclaim()` |
| ML-R9, one record in flight | D4: an invariant of the log writer, the batcher included; T4 |
| ML-R10, Q3's premise | Limits and Q3: the design extends media-faults' lost-write row to the index's metadata |
| ML-R11, T3's noise | T3: interleaved pairs after an A/A, the barrier mode named; K2's targets leave for K2's design |
| ML-R12, quadratic checks | D3: running sums |
| ML-R13, the classifier's home | D2: journal's component, parameterized by block size |
| ML-R14, T9's target | T9: at most `main`'s files and blocks |
| The floor numbers | "Measured": the rerun's medians replace the first run's |

## Why

The sync-CPU explorer measured 1.8 to 2.1 ms of CPU per synchronous commit on `main` `356acbc`,
almost all system time in file metadata calls (`sync-cpu.md:11-24`): per commit 2.1 files created (a
segment and the manifest temporary), 2 unlinks, 1 rename, 5 `fsync`s and 2 `F_FULLFSYNC`s, at 126 to
241 µs of thread CPU per create, unlink or rename, against 33 µs to append to an open file and `fsync`
it. That is the loss on every synchronous row against RocksDB, 4.76 to 5.09× slower
(`core-vs-rocksdb-f14.md:20-22`). The two cheap switches the explorer tried, lazy retention and a
manifest written under its final name, removed at most 21% of the CPU together (`sync-cpu.md:26-36`).

The churn is the commit protocol itself: a live-docs file per segment with new deletes, a metadata
file when it changed and the field table's appended block (`index.hh:4556-4603`), the manifest under
a temporary name (`:4647-4651`), one ordering point, the rename and a directory sync (`:4652-4656`),
then retention's unlink of the previous manifest and another directory sync (`:4734-4762`), and
reclaim's unlinks in the same call (`:944`).

## What this builds on, and what it changes

| Document | Used | Changed |
|---|---|---|
| `phase-3.md` D1 to D5 (`:44-227`) | generations, pins, `retained_predecessors`, `open(..., to)`, install, fencing on a failed sync | the directory loses `*.live`, `meta-*.kv`, `fields-*.kv`; commit is an append (D4); recovery replays (D6); retention drops "manifests leave first" (`:177-178`) and its lifecycle condition 9 (D8) |
| `fields-and-keys.md` D2, T8 (`:582`) | the append-only table; T8's bound | the table lives in the log (D1); T8 restated in the log's terms with its 4× bound kept (D7, T8) |
| `durability-modes.md` D1 (`:825-955`), T16 | commit splits into durability, visibility, checkpoint; the checkpoint of a past generation (DM-R3); retention on the durable head (DM-R23) and the recorded generation (DM-R43); the batch entry's snapshot (SI-R19) | its checkpoint, "today's commit protocol" (`:831`), is D4's edit from the durable head to the checkpointed generation (D3, L2); the recorded generation is a kept generation (D8); D17's file sets carry segments only |
| `track-c.md` D11 (`:460-625`) | the turn log; one turn in flight (`:506-509`); its group checkpoint record names an application checkpoint (`:518-522`, `:582-587`) | the reference is a manifest-log generation; the framing is shared (D2); one record in flight is the log writer's invariant too (D4) |
| `workload-profiles.md` item 8 (`:100-106`), `rocksdb-survey.md` K2 (`:207`) | standalone durability through the turn log, a group of one | none; D9 states how the two logs meet, and K2's targets leave this design |
| small-indexes (`perf/small-indexes` `39ba962`) D1 to D3 | the owner part released at commit; one descriptor per idle index; the commit batcher | the batcher syncs logs, one record in flight each (D4); D3-C's create residue is `manifest-1.tmp` alone |
| `cold-tier.md` D10, D11 | warm writes nothing and holds one descriptor; open checks names and sizes | demotion closes the log's descriptor; open reads one file and syncs it and the directory (D6) |
| `media-faults.md` (`:16-23`, `:104-108`) | one retry then damage; repair from a predecessor; lost writes outside the model for the consensus journal | the tail rule (D6); the lost-write row extended to the index's metadata (Q3); what a predecessor protects (Q2) |
| `limits.md` | every cap names its mechanism | D7's bounds and floors; D8's sync rule |

## Decisions

### D1. One log per index: a snapshot, then one edit per commit

- The index's directory holds its segments (`seg-<id>.sx`) and its manifest logs. The current log is
  `manifest-<g>`, named by the generation of its snapshot, its first record. Each commit appends one
  edit, generations g+1, g+2, ... in order.
- **An edit is everything one commit makes durable:** the scalars that changed, segments removed and
  added, documents newly deleted in existing segments, metadata entries set and removed, field
  declarations appended, pins added and removed (D3).
- **Live-docs, metadata and field-table files retire.** Their content moves into the log in the
  encodings strata already has: a segment's deleted set as the live-docs encoding (a bitset or LEB128
  deltas, whichever is smaller, `files.hh:355-380`), a metadata pair as `files.hh:420-432` writes it,
  a declaration as the field table writes it (`fields.hh:101-106`). Folding them is what makes a delete
  or a metadata change cost no file: today a replace that supersedes a copy in an older segment writes
  that segment a live-docs file at the next commit (`index.hh:4558-4569`).
- One log per index, not one per host as RocksDB keeps one MANIFEST per database: the directory stays
  self-contained (install reads a pinned generation from another index's directory,
  `index.hh:1587-1596`), an index opens alone, and a cold index costs nothing outside its directory.

### D2. Framing: journal's blocks and fragments, at 512-byte blocks

- The log's bytes are the turn log's framing (`turn_log.h:107-160` writes it, `:175-221` reads it):
  blocks, each with a header (magic, identity, segment number, block number, epoch, CRC-32C); records
  split into fragments (full, first, middle, last), each under its own CRC-32C and bound to its
  segment and epoch. RocksDB shares one `log::Writer` between its WAL and its MANIFEST the same way. A
  second framing in strata would be the duplicate mechanism calibration's first row flags.
- For a manifest log: the magic is `STRLOG01`, the identity is the index UUID, the segment number is
  g, the epoch is 1 (a log is never recycled), and no block manifest is written. Its block size is
  **512 bytes**; the turn log keeps 32 KiB.
- **Why 512: no fragment crosses a 512-byte boundary.** Every block is one 512-byte piece that starts
  with its header, and the emitter fits every fragment inside its block, padding a tail too short for
  one (`turn_log.h:116-132`). A crash leaves each 512-byte piece old or new (Limits), so it leaves
  every fragment either whole or in a piece the append never wrote, and an unwritten piece reads as
  zeros from where the append began in it to its end: past the old end of file a filesystem returns
  zeros, and a block header or a fragment header is never all zero. D6 relies on exactly this. At the
  turn log's 32 KiB a fragment spans many sectors and a payload's zero run fills whole ones: on the
  model, 16,905 of 22,960 flips over a log's last two records read as torn there (the review's
  `tail_probe.out`, check 3).
- The cost is framing bytes: 62 per block header without a manifest and 24 per fragment
  (`turn_log.h:82-84`), so a block carries at most 426 bytes of one record and a snapshot takes about
  1.2× its payload. The open probe's model edits are 56 bytes of payload and about 96 on disk for a
  commit that flushed a segment, 25 and about 57 for a delete alone.
- **The component** (ML-R13): journal gains one log component that both logs use: the emitter, the
  scanner, the growing-file read (a partial last block; the turn log reads whole preallocated blocks,
  `turn_log.h:180`) and D6's tail classifier, parameterized by block size and exact only where a block
  is the 512-byte piece. Nothing changes a byte the turn log writes, and the turn log's 25-byte record
  header (`turn_log.h:82`, `:118`) stays its own. K2's design adopts or rejects the classifier
  explicitly (the review's C3).

### D3. Records

A record is a kind (1: snapshot, 2: edit), its generation (LEB128), and a payload of LEB128 fields
unless noted. The decoder follows `decode_manifest_for` (`files.hh:279-351`): readability first (the
files version, then the mask and segment versions against what this binary reads,
`unsupported_index`), then every length bounded by the bytes left, then the values.

| Part | Snapshot | Edit |
|---|---|---|
| Files version | first field | absent: the snapshot's governs the file |
| Scalars: UUID, revision, last docid, next segment id, applied position, schema version, configured, mask, segment versions, document count, total length, live count, live length | all | a presence mask, then each scalar that differs from the previous record's state; the UUID never (the blocks' identity) |
| Sections | all, in order | a mask of the sections that follow |
| Pins | the set | added; removed |
| Segments | each entry: id, identity, size, footer CRC, document count, live count, live length, its deleted set (none, bitset or deltas), values generation, attributes | ids removed; entries added, as in a snapshot |
| New deletes in existing segments | (in the entries) | per segment: id, the ordinals newly deleted (ascending deltas), its live count and live length after them |
| Metadata | every pair, keys ascending | pairs set; keys removed |
| Field table | every declaration | the table size they extend, then the declarations |
| Segment attributes | (in the entries) | per segment, the set, when it changed (no tag is defined yet, `files.hh:66-78`) |
| Index attributes | the set | the set, when it changed |

- **Only what changed** (ML-R6): revision 0 carried every scalar in every edit, about 45 bytes (the
  review's estimate), which is what made fields-and-keys T8 fail (D7). The probe's model edit for a
  commit that flushed a segment carries its 8 changed scalars and the segment's entry in 56 bytes of
  payload.
- **Replay checks:** generations contiguous within a log; a removed id is present; an added id is
  absent and below the record's next segment id, and goes in id order (a merge's output takes its id
  when it is scheduled, below segments flushed since, `index.hh:3358-3362`); a newly deleted ordinal
  is below its segment's document count and not yet deleted; a removed key is present; the table size
  an edit extends is the table's; pins at most the generation. The document, live and live-length
  totals are running sums, updated as entries are added and removed and compared with the carried
  values when an edit carries them, never recounted per record (ML-R12); total length is carried,
  since segments do not record it. A violation is `corrupt_index`.
- **An edit is the difference between two named states** (L2). The durable head d is the state the
  log's last record describes; the target c is at or below the current state: a standalone
  `commit()`'s current state, or durability-modes D1's checkpoint of the newest refresh generation at
  or below `commit_durable`, whose batch entry snapshots c (DM-R3 and SI-R19,
  `durability-modes.md:836-845`, `:884-901`). Scalars, segments added and removed, pins, index
  attributes and the field table's new declarations (from d's table size to c's) come from the two
  snapshots. Deleted ordinals and metadata keys come from one per-index **change list**, each entry
  stamped with a mark, the list's running count:
  - `apply_pending` appends each ordinal it newly deletes in an existing segment
    (`index.hh:4293-4305`), the one site: a flush's own deletes (`:4341-4346`) and a merge's carried
    ones (`:3344-3349`) land in segments born since the head, which carry their whole deleted set as
    added entries. Deletes never clear: no site in `include/` or `src/` resets a deleted bit.
  - A transaction's `set_meta` and `delete_meta` append their key (`index.hh:4072-4086`); a rollback
    truncates the list with the rest of its undo (`:3865-3887`).
  - Every snapshot records the mark at its creation: a view at `build_view` (`index.hh:2794-2825`),
    which already shares its segments' deleted bits, its metadata and its field table
    (`:1339-1343`); a batch entry; a standalone commit's state.
  - The edit d→c takes the entries with marks after d's and at or below c's: the ordinals of segments
    in both d and c, ascending per segment; each key once, as set with c's value when c's map holds it
    and d's held another, as removed when d's held it and c's does not, and not at all when the two
    agree (the writer keeps the head's metadata map, a shared copy as a view keeps one,
    `index.hh:2805`). Once the edit is durable, entries at or below c's mark leave and c is the head.
    A delete applied after c stays in the list for the next edit, so a checkpoint of c never holds a
    write durability-modes' D6 can remove.
- **One oracle** (T6): for every state d and target c, replaying the edit d→c onto d gives c, and a
  snapshot of the replayed state encodes byte for byte as a snapshot of c, with c a past view behind
  later deletes and metadata changes among the cases (durability-modes T16's shape).

### D4. The commit: one write and one barrier

```
main (index.hh:4535-4676, 4697-4769)            manifest log
  write segment, live-docs, meta, fields block     write segment (when the commit flushed one)
  write manifest-<g>.tmp                           order it and the directory: fsync each,
  order all + directory: fsync each, fsync dir,      fsync dir, barrier (only when a file was written)
    barrier                                        append the edit at the log's end: one write
  rename tmp -> manifest-<g>                       log.sync_data(): fdatasync (Linux),
  sync dir: fsync dir + F_FULLFSYNC                  F_FULLFSYNC (macOS), fsync (FreeBSD)
  retain: unlink manifest-<g-1>, sync dir          retain: queue obsolete files and logs (D8)
```

- The ordering point then the append is Apple's documented two-phase `F_BARRIERFSYNC` protocol that
  `main` already follows for the rename (`docs/reviews/commit-barriers-durability.md:35-40`): no edit
  naming a segment reaches the device ahead of the segment and its directory entry. `sync_data` is
  completion's (`posix.h:169-177`, `:358`).
- **One write per append** (L1). The edit, with any padding and block headers before it, goes to the
  log's end E in one `write_at` (`posix.h:345-351`: one `pwrite`, retried only on `EINTR`), never
  through `write_all_at`'s loop (`index.hh:227-233`), which continues a short write. The file's size
  then stands only at E, at E+n, or at a page boundary between: ext4 and XFS raise the on-disk size by
  written-back pages, capped at the in-memory size, by their sources (APFS, ZFS and UFS are not checked
  here). Every page size is a multiple of 512, so a crash never cuts a fragment. An edit too large for
  one write (Linux caps one at 2 GiB less a page) goes in pieces that each end on a 512-byte boundary,
  which keeps the same sizes.
- **A failed append is undone before the commit reports** (L1). On an error or a short count the
  writer truncates the log back to E (`posix.h:352-356`) and syncs it. Then `ENOSPC` reports
  `space_error` with the index unchanged, as `write_unsynced` does for a partial file
  (`index.hh:2700-2712`), and any other error fences, as today (`:926-929`); a failed truncate or sync
  fences. The log stays clean, so the retry appends at E with no rollover and needs no snapshot's space
  under `ENOSPC` (ML-R5). Left in place, a short write reads as damage at 21,846 of the 21,962 sizes it
  can leave on the model (`tail_model.py` check 6). A failed `sync_data` is undone the same way
  before it fences: Linux keeps pages whose writeback failed clean in the page cache and reports the
  error once, so left in place the edit would read back in a same-boot reopen whose own sync then
  succeeds without writing it, and the next acknowledged edit would follow bytes a power loss turns
  to zeros (T4 case 11).
- **One record in flight** (ML-R9). The log writer starts an append only after the previous one's
  barrier returned or its failure was undone, and every caller goes through it: `commit()` under the
  write lease, small-indexes' batcher (an entry's sync is its log's `sync_data`; macOS's `files_sync`
  stages each and flushes each device once, `posix.h:186-225`), and durability-modes D1's background
  checkpoints. Two edits in flight can leave a bad edit before a whole one, which D6 reads as damage,
  the reason Track C allows one turn in flight (`track-c.md:506-509`): 33 of 76 honest crash images of
  two unsynced edits read as damage on the model (check 5).
- **Failures, unchanged in kind** (`index.hh:926-929`, `2654-2670`): an `ENOSPC` while writing a
  segment or the edit leaves the index as it was; any failed sync, rename or directory sync fences.

### D5. Rollover: the name is the pointer

- A rollover writes the state as a snapshot to `manifest-<g>.tmp` (g the generation it commits), syncs
  it at the commit's ordering point with any new segment, renames it to `manifest-<g>`, and syncs the
  directory. That is today's commit protocol (`index.hh:4644-4656`), so a rollover costs the manifest
  half of `main`'s commit: 694 µs of CPU in the rerun's probe F.
- **No CURRENT file.** The highest final-named `manifest-<g>` is the current log, and the rename is
  the atomic switch. RocksDB writes CURRENT through a temporary and a rename
  (`file/filename.cc:458-496`) to find its MANIFEST without listing the directory; strata's open lists
  it anyway (`index.hh:4889-4913`), so CURRENT would add a file and a read per index and an extra
  rename and sync per rollover, for nothing. At millions of indexes a file is at least one filesystem
  block each (probe M).
- A rollover never removes the old log; D8 does, lazily.

### D6. Recovery: replay, the torn tail, and an open that writes nothing

1. The directory scan (`index.hh:4889-4913`) finds the logs; `.tmp` names are orphans.
2. Read the highest log whole: one read, bounded by D7.
3. Its snapshot must read whole and name g. A log is renamed only after its snapshot was synced, so a
   bad snapshot is damage: `corrupt_index`, after `read_checked`'s one retry (`index.hh:4986-5008`).
4. Replay edits in order. At the first fragment or block header that does not verify, the tail
   classifier (L1) decides from that point alone; the first row that matches decides:

| What the reader finds | Verdict |
|---|---|
| Nothing: the last record ends at the end of file | Clean |
| A record whose fragments all verify but which has no last fragment, the file ending at a 512-byte multiple | Torn |
| A fragment or block header cut by the end of file: its header, or the length it declares, passes the end | Damage: one write per append never leaves a cut (D4) |
| A fragment or block header whose bytes, from its start to its block's end (or the end of file when that comes first), are all zeros | Torn: a piece the crash never wrote |
| Anything else: bytes there, and a check fails | Damage: the bytes changed after they were written |

   A torn verdict becomes damage when a whole record follows the failure (one record is in flight,
   D4), and any failure in the snapshot is damage. A torn tail is dropped and the log marked dirty.
   The rule reads only the failing fragment's own piece, never the bytes after it, so a later
   record's unwritten piece never excuses an earlier record's damage: on the model, 7,504 images of a
   torn last append with a bit flipped in the record before it all read as damage (check 4).
   **Checked on the model** (`tail_model.py`, four logs: the review's, records ending on block
   boundaries and before padding, edits of up to 2,100 bytes over five blocks, thirty random ones;
   the framing checked against journal's `RFTW0001` fixtures, 16 block headers and 21 fragments):
   1,020 honest crash images (every append, every subset of its sectors persisted, every size a crash
   can leave) all open at the last whole generation, 672 of them as torn; of 219,672 single-bit flips
   over every byte of the four logs, none reads as torn or clean with fewer records, and 584 land in
   block padding no reader reads. Revision 0's wording drops 18 of them silently.
5. The head must find every file it names, with its size, as today (segments by the scan's names and
   sizes, cold-tier D11).
6. **The open creates nothing and issues one directory sync and one `sync_data` on the log** (ML-R8,
   ML-R15 option 1). It
   unlinks `.tmp` orphans without a directory sync (durable at the next one, D8) and leaves retention
   and reclaim, which today's open runs (`index.hh:4954-4959`), to the first commit or the host's next
   `reclaim()`. Before it reports the head, it syncs the log it opened for writing once: after a
   process crash the page cache can hold an edit whose `pwrite` returned and whose `sync_data` never
   ran, and the open must not report that generation durable while a power loss can still take it
   back. The directory sync comes first, for the same reason: a rollover that stopped between its
   rename and its directory sync (a fault there fences; a process kill leaves the same state) has a
   head a power loss can still revert, and an edit that writes no file orders nothing, so the
   commits after the reopen would be acknowledged on a name that can vanish. On a log with no dirty pages the sync costs 0.3 to 2 µs of CPU over open and close alone
   (the re-check's `clean_sync_probe.c`); it pays a device flush only after a crash left dirty pages,
   the case it exists for. A failed sync, a writeback that failed earlier included, fails the open.
   A dirty log rolls over at the next commit (D7), as RocksDB's `reuse_manifest_on_open` makes a
   fresh MANIFEST when the file's size differs from its last valid record's end
   (`db/version_set.cc:6731-6746`). A crash costs one rollover later.
7. Counters start above every name on disk (phase-3 D3, `index.hh:4941-4946`); after a dirty tail the
   next generation skips one, so a torn record's generation is never reused, as today never reuses a
   failed attempt's manifest name (`index.hh:4649`).
8. **`open(..., to)`** replays the log with the greatest snapshot generation at or below `to`, up to
   `to`. `to` itself must replay whole (ML-R4): a log that ends before it, dirty or shortened by
   reclaim's truncation steps (`index.hh:3582-3592`) before its unlink, fails as "generation not
   retained", as does a file its state names that is gone. It then rolls over to commit that state as
   the newest generation, as today commits the older manifest again (`index.hh:4947-4953`), and
   `fallback()` counts the readable generations it dropped. `install` and retention (D8) read a
   generation with the same function.

Crash points:

| Crash during | Left on disk | The open finds |
|---|---|---|
| A segment's write or its ordering point | an orphan segment, the log unchanged | the previous generation; the orphan reclaimed later |
| The edit's append, before `sync_data` returns | the edit absent, torn, or whole | the previous generation (dirty when torn), or the new one |
| A failed append's truncate-back, before its sync | the log at E, at E+n, or cut inside a fragment | the previous generation; a cut fails closed (Limits) |
| A rollover's temporary, before the rename | `manifest-<g>.tmp` | the old log's head; the temporary unlinked |
| The rename, before its directory sync | either name | the old log's head or the new log's snapshot; the commit was not yet acknowledged |
| After the directory sync, before cleanup | both logs | the new log's head; the old log is obsolete and queued |
| Lazy cleanup | any subset of obsolete files and logs gone | the head, which names none of them |
| The open's own orphan removal | some orphans gone | the same head |

### D7. The rollover trigger

A commit rolls over when its log is dirty (D6), when it replaces the content (install,
`open(..., to)`), when the index has no log yet (its first commit), or when the log would pass either
bound:

1. **Space:** max(F, 2 S) bytes, S the bytes a snapshot of the current state would take.
2. **Replay:** max(R, n, S / c) edits since its snapshot, n the entries that snapshot would hold
   (segments, metadata pairs, declarations, pins), c the snapshot bytes that decode in about one
   edit's replay time (ML-R16).

- **S is the current state's** (ML-R6). RocksDB's option names the same quantity, "an estimate of how
  big a new compacted version of the current manifest would be", and estimates it by the last snapshot
  written (`include/rocksdb/options.h:1071-1081`, `db/version_set.cc:5817-5822`), as revision 0 did.
  Where every edit grows the state, that estimate rewrites the state each time the log doubles in all
  bytes: fields-and-keys T8 reads 6.56× and 7.23× its final table by arithmetic (`t8_model.py`). The
  writer keeps S exactly instead, adding and removing what each edit's parts add to or remove from a
  snapshot. With it and D3's presence mask, T8 reads 2.46× and 3.38×, under its 4×; either change
  alone leaves 4.19× to 6.03×.
- At 2 S the open reads at most twice what a fresh log of the same state holds, and the space bound
  alone adds at most one snapshot byte written per byte of edits the state no longer holds (RocksDB's
  `max_manifest_space_amp_pct` at 100%; RocksDB defaults to 500% over a 1 GiB minimum,
  `options.h:1032`, `:1111`, for a few large databases, while strata opens millions of indexes
  lazily).
- **Where bytes do not bound the replay.** The open probe's model replays edits at about 3 ns of CPU
  per byte (0.18 µs per 56-byte commit-mix edit, 0.07 µs per 25-byte delete) and a fresh 332-byte
  snapshot at about 5 ns per byte (1.78 µs, framing included), so for most states twice the bytes is
  about twice the work. A snapshot whose bytes are mostly deleted bitsets decodes at memory speed
  (estimate), and twice its bytes in edits would replay many times slower: 125 KiB of delete-only
  edits, about 2,200 of them, replay in about 160 µs at the model's per-edit cost. The second bound
  caps those at max(R, n, S / c) edits: past its floor a log holds no more edits than its state has
  entries, or than its snapshot's decode time allows. The S / c term keeps a large deleted set from
  forcing a whole snapshot per R edits (the re-check's ML-R16: 50 segments of 10 million documents
  wrote 278 KB of snapshot per commit under max(R, n); at c = 512, 512 bytes).
- **R = 128 edits**, the replay floor (ML-R7). It keeps revision 0's intent of about 125 edits per
  rollover, counted in edits so small ones cannot stretch it. By the probe's model the floor costs a
  small index's open little: after 128 commit-mix edits it takes 59.3 µs of CPU, 0.74× `main`'s
  79.6 µs, and after 128 delete-only edits 44.1 µs (0.55×); at the 16 KiB floor alone, 171 and 288
  edits, 69.2 µs (0.87×) and 58.5 µs (0.74×). The review's 1.3 to 1.9× compared the replay with
  `main`'s reads alone; `main`'s open also opens every file for its size (`index.hh:4899`). Each edit
  carries 1/128 of a rollover's fixed cost: 5.4 µs at the rerun's probe F (694 µs), 0.7% of a commit
  that flushed a segment (I, 739 µs), 3.0% of an edit made durable alone on macOS (J, 183 µs), and
  21% of an append and an `fsync` (A, 25 µs), an upper estimate of Linux's share, since Linux's
  rollover issues no `F_FULLFSYNC`; T3 measures it there.
- **F = 16 KiB**, the space floor: the 2 S bound alone would roll a small index over every few commits
  (the probe's small index snapshots in 332 bytes), and an index whose edits are large replays at most
  16 KiB of them.
- **c = 512 bytes**, the decode ratio: a snapshot's deleted bitsets decode at an estimated 0.25 to 1 ns
  per byte against about 0.18 µs per edit, so 512 bytes of snapshot cost about one edit's replay
  (estimate; T7 measures it).
- **The reader enforces the bounds** (SR2-strata): a log whose edits pass twice either bound, taken
  against the state its replay has reached, is damage, so a crafted log cannot make the open's
  replay cost more than a small multiple of the bytes it reads.
- All five are presets with a named mechanism (`limits.md`'s rule); T3, T5, T7 and T8 move them. Under
  K2 a rollover runs in a background checkpoint instead of the commit that triggers it (D9).

### D8. Retention and lazy cleanup

- **Kept generations:** the head, the pinned ones, the `retained_predecessors` generations before the
  head (`index.hh:435-439`; what they protect is Q2), and, under durability-modes D1, the recorded
  generation (DM-R43). A kept generation stays openable: the log holding it and every file its state
  names stay on disk.
- **How retention learns a kept generation's files** (L3). The head's are in memory, and so are those
  of every kept generation this process made durable: the writer keeps the head's segment ids and the
  log holding it, which stay as a predecessor's or the recorded generation's as the head moves on, and
  a pin is taken on the head (`index.hh:1558-1566`). After a reopen, retention reads a kept generation k
  from the log holding it, the one with the greatest snapshot generation at or below k, replayed up to
  k with D6.8's function, and caches the result as `manifest_files` caches an older manifest
  (`index.hh:4772-4785`). A kept generation's log that cannot be read, or that ends before k, blocks
  retention: it removes nothing that round and reports `retention_blocked()` with k, as `retain()`
  does for a kept manifest it cannot read (`index.hh:4716-4720`); a kept generation with no log on
  record names nothing to keep, as there (`:4716`).
- A log holding no kept generation is obsolete, and so is every file no kept generation names and no
  unpublished flush, pending merge or view holds (`index.hh:4724-4733`). Both join the reclaim queue
  that already frees data files (`index.hh:3573-3621`: truncated in `reclaim_step`s, unlinked, posted
  to the host's executor when it has one), with **no ordering between logs and data files**. `open`
  picks the highest log and `open(..., to)` refuses a generation it cannot replay whole or whose files
  are gone (D6.8), so nothing promises an obsolete log's generations open. That drops phase 3's rule
  that a manifest leaves before the data files only it named (`phase-3.md:177-178`), the lifecycle
  review's condition 9 that enforces it (`reviews/phase-3-lifecycle.md:143-147`), and `retain()`'s
  directory sync between the two passes (`index.hh:4749-4762`).
- **Removals are lazy.** A commit syncs the directory only when it wrote a file, so an unlink becomes
  durable at the next directory sync: a commit's ordering point, a rollover, `drain()`, `close()`, or
  the reclaim path's own sync, which it issues once its unsynced removals pass `reclaim_slice`. Until
  then the storage permit counts them (`index.hh:3649-3661`, `3723-3733`). The open's orphan unlinks
  are the same (D6.6).
- **The commit allowance** (ML-R5, the re-check's C5). `commit_allowance()` holds what the next commit
  writes, so a commit that frees space can always be written (`index.hh:3667-3685`). It holds the
  largest edit the pending changes make, and a snapshot of the current state (S) only while a rollover
  is forced (a dirty log, install, `open(..., to)`, the first commit). D7's two bounds are soft: under
  space pressure (`ENOSPC` from the rollover's temporary) the commit appends its edit instead and the
  rollover waits for the next commit, so an index with large deleted sets does not reserve S, about
  75 MB on the re-check's large shape, against every commit.
- **The bound on extra disk:**

| What waits | Bound | Default |
|---|---|---|
| The current log | max(F, 2 S) bytes, max(R, n, S / c) edits | 16 KiB and 128 edits for a small index |
| A rollover in progress | the new snapshot, S, beside the old log until the old log's reclaim slice | |
| Obsolete logs | one log per kept generation outside the current log, each within its own bound | none: a rollover's old log is queued in the same commit and unlinked in its reclaim slice |
| Unlinked, not yet durable | `reclaim_slice` plus one file | 64 MiB plus one file |
| Obsolete data files | `limits.md:26`'s reclaim row, unchanged | as today |

### D9. The turn log builds on the manifest log

- **The turn log is the write-ahead log.** Track C D11's journal holds transactions, each acknowledged
  at its turn's barrier, one barrier per core per turn (`track-c.md:497-505`); a standalone index uses
  it as a group of one (`workload-profiles.md:100-106`, rocksdb-survey K2). The manifest log never
  holds a transaction.
- **The manifest log is the checkpoint.** Durability-modes D1's third step, "today's commit protocol"
  (`durability-modes.md:831-835`), becomes D4's edit from the durable head to the checkpointed
  generation, which D3 builds for a past generation as DM-R3 requires (L2). The group checkpoint record
  names the edit's generation (`track-c.md:518-522`, `:582-587`) and is staged only after the edit is
  durable (Condition 25's order, `:608-612`); replay opens that generation and applies the batches
  after it (`durability-modes.md:864-881`).
- **Not subsumed by the turn log.** Writing the snapshot and edits as turn-log records would remove the
  manifest file, but an index's directory would no longer open alone (install and tools read it), every
  cold index would hold a resident entry in its core's turn log and have its snapshot copied forward by
  the turn log's reclamation (`track-c.md:523-535`), and the owner's ruling asks for manifest files
  that roll over.
- **Not a write-ahead log itself.** Carrying the commit's documents in the edit would reach 0 files per
  commit without the turn log, as a second log of transactions beside it, which workload-profiles item
  8 rules out ("the same mechanism as the replicated path, not a second one").
- **So the synchronous commit's floor is K2's.** Without a turn log, `commit()` stays a checkpoint: a
  segment file per commit that flushed documents, the ordering point, the edit. With it, a commit is
  one turn-log append and one barrier, and checkpoints run in the background at the share D11 gives
  each group (`track-c.md:725-727`). K2's design owns its targets (0 files created per commit, its CPU
  against the append-and-barrier floor K'), adopts or rejects D2's classifier, and settles whether an
  index whose checkpoint covers its last batch leaves the turn log, so a cold index holds nothing
  there.
- Under K2 the checkpoint record is the durable frontier the manifest log lacks alone, which closes
  Q3's residual for every checkpointed generation.

### D10. Cost at millions of indexes per host

| Per index | `main` | Manifest log |
|---|---|---|
| Files besides segments | the manifest, `meta-<g>.kv`, `fields-<g>.kv`, a live-docs file per segment with deletes | one log |
| Disk for them, small index | one 4 KiB block each (probe M): 16 KiB with metadata, fields and one live-docs file | 4 KiB after a rollover, up to 16 KiB before the next one |
| Descriptors, hot | the directory (small-indexes D2) | the directory and the log, while it appends |
| Descriptors, warm or cold | 1 or 0 | 1 or 0: demotion closes the log (cold-tier D10's warm writes nothing and keeps the lock only) |
| Heap per open index | the manifest, the file table | the same less the live, metadata and fields references, plus the log's state (generation, end offset, S, n, edits since the snapshot, dirty flag: 48 bytes) and the change list's entries since the head (16 bytes each, estimate) |
| Commit allowance | a manifest, live-docs files, metadata, the table's block | a snapshot and an edit (D8) |
| Open | every file opened for its size (`index.hh:4899`), then the manifest, metadata, field table and each live-docs file read | the log and the segments opened for their sizes, one log read; by the probe's model, 0.43× `main`'s CPU fresh and 0.74× at R |
| First retention after a reopen | each kept manifest, read once (`index.hh:4772-4785`) | each older log holding a kept generation, replayed to it once (D8) |

Disk is no win for a small index that commits often: its log saws between one block and four. It is a
win for an idle index (one block) and for an index with deletes in many segments, which `main` gives a
file each. Reopening the log for each commit would cost about 36 µs of CPU (the rerun's probe D
against A), more than the append itself, so the log stays open while the index is hot and closes when
it demotes; a host's descriptor budget counts it (`limits.md:59`).

### D11. Format

`files::pre_freeze` moves to `0x8000'0008` (`files.hh:38-45`), so a build refuses another pre-freeze
build's index (F2, F20); strata has no release, so nothing migrates. A `manifest-<g>` that starts with
the retired `STRMAN01` is another build's index: `unsupported_index`, never damage. The magics
`STRLIV01` and `STRMET01` and the field table's file header retire; the manifest's fields become the
snapshot's. Fixtures: a log with a snapshot and every kind of edit; a torn tail of each D6 kind (a
never-written piece, a 512-byte end between fragments); a fragment cut by the end of file (damage); a
damaged middle record; a dirty tail then its rollover; a short append truncated back. One CPU-hour of
fuzzing on the log reader, framing, classifier and records together (TESTING's row).

## Measured

### Floors: the architect's rerun

`bench/manifest-log/mlog_probe.c`: the file work alone, in a plain C loop (no strata code), three
rounds under a strict hold, rerun by the architect for the review
(`results/2026-11-04-floors-rerun.txt`). It supersedes the first run (`results/2026-11-04-floors.txt`):
the rerun reads 8 to 58% higher on every probe but A, which reads 16% lower. CPU is the process's user
and system time per operation, median (range); wall is the median round's.

| Probe | The file work | CPU µs | Wall |
|---|---|---:|---:|
| A | append 100 bytes to an open file, `fsync` | 25.4 (24.9-58.9) | 55 µs |
| B | the same, `F_FULLFSYNC` | 198.6 (183.6-203.4) | 22.1 ms |
| C | the same, `F_BARRIERFSYNC` | 85.1 (49.5-85.7) | 1.07 ms |
| D | open, append, `fsync`, close | 61.2 (49.7-71.0) | 130 µs |
| E | open an existing file, close | 14.9 (12.4-16.3) | 15 µs |
| F | rollover: a 1 KiB snapshot to a temporary, `fsync`, rename, directory `fsync` and `F_FULLFSYNC` | 693.6 (692.5-732.0) | 22.6 ms |
| F' | unlink an old log, no directory sync | 108.1 (104.9-110.7) | 109 µs |
| G | rollover with a 64 KiB snapshot | 792.1 (727.9-796.2) | 22.9 ms |
| H | `main`'s commit: a 2 KiB segment and a 700-byte manifest temporary, the ordering point, the rename, a directory sync, the old manifest unlinked and another directory sync, an older segment unlinked | 1,455.8 (1,331.5-1,519.2) | 44.9 ms |
| I | the log's commit: the same segment, its ordering point, a 100-byte edit appended with `F_FULLFSYNC`, an older segment unlinked | 738.7 (731.7-744.5) | 24.2 ms |
| J | the log's edit alone, `F_FULLFSYNC` | 183.1 (176.6-188.4) | 22.6 ms |
| K | a turn-log-like append: 150 bytes into preallocated space, `fsync` | 28.8 (24.5-43.4) | 68 µs |
| K' | the same, `F_FULLFSYNC` | 158.0 (156.2-183.5) | 22.3 ms |
| L | open, `fstat`, read 4 KiB or 64 KiB from the page cache, close | 15.6 or 16.9 | 16 or 17 µs |
| M | the disk a 16-byte file takes | 4,096 bytes | |

I against H is the manifest log's commit at 0.51× of `main`'s file work (2.0× less), outside both
spreads, and one device flush instead of two. strata's own commit measured 1.8 to 2.1 ms
(`sync-cpu.md:11`), part of it outside the calls H models, which the explorer could not attribute
(`sync-cpu.md:19-23`) and which may or may not shrink with the files. K' is the floor a turn-log
commit approaches, below RocksDB's 219 µs per operation on the same host (`sync-cpu.md:24`). These
are one platform's floors and two runs that differ by up to 58%; T1 to T3 measure strata on all three
platforms.

### Open cost: the open probe

`bench/manifest-log/open_probe.cc`, three rounds of 3,000 opens under a strict hold
(`results/2026-11-04-r1.txt`). `main`: the scan opens four files for their sizes (the manifest, one
segment, the metadata and the field table, as `index.hh:4895-4913` opens every file), then reads the
manifest, the metadata and the table and decodes them with strata's own `decode_manifest`,
`decode_metadata` and `decode_fields`, for a small index after 200 synchronous commits of a model mix
(each flushes a segment of 1 to 3 documents; every tenth merges the ten newest; one in six deletes
three documents; one in twenty sets a metadata key). The live-docs files `main` would also hold are
left out, which understates `main`. The log: the scan opens the log and the segment, then one read,
the framing's CRCs, and a model decoder replaying D3's records onto the state with running totals;
each replay is checked equal to the model's state. CPU per open, median (range):

| Log | Edits | CPU µs per open | Against `main` |
|---|---:|---:|---:|
| `main` | | 79.6 (79.1-83.5) | 1 |
| fresh: a 332-byte snapshot | 0 | 34.2 (34.1-36.2) | 0.43× |
| commit mix, 4 KiB | 43 | 46.4 (44.2-46.9) | 0.58× |
| commit mix, 8 KiB | 86 | 52.5 (52.3-53.5) | 0.66× |
| commit mix, R | 128 | 59.3 (58.2-59.5) | 0.74× |
| commit mix, 16 KiB | 171 | 69.2 (67.0-69.8) | 0.87× |
| commit mix, 32 KiB | 343 | 101.2 (99.0-104.4) | 1.27× |
| delete-only, R | 128 | 44.1 (43.8-44.9) | 0.55× |
| delete-only, 16 KiB | 288 | 58.5 (54.8-59.1) | 0.74× |

Replay alone, from memory: 1.78 µs for the snapshot, then 0.18 to 0.20 µs per commit-mix edit and
0.07 per delete-only edit. The decoder is a model, not strata's, and the open leaves out what both
opens share (the lock, the segments' records): these numbers set D7's floor and T7's expectations, and
T7 measures the real open.

### T8: arithmetic

`bench/manifest-log/t8_model.py`, from the encodings (a declaration as `fields.hh:101-106` writes it,
`bench/field_table`'s names, journal's framing at 512 bytes) and one estimate, revision 0's 45 bytes
of scalars per edit. Bytes written for the table over the final table, declaring N fields one per
commit:

| Edit, trigger | 2,000 fields | 100,000 fields |
|---|---:|---:|
| Revision 0: every scalar, 2× the log's own snapshot | 6.56× | 7.23× |
| Changed scalars, 2× the log's own snapshot | 4.19× | 4.47× |
| Every scalar, 2× a snapshot of the current state | 4.82× | 6.03× |
| Revision 1: changed scalars, 2× a snapshot of the current state | 2.46× | 3.38× |

In all bytes the revision-1 log writes 5.94× and 6.94× the final table file; `main` writes 1.37× and
1.34× to its `fields-*` file and, with a manifest of about 70 bytes per commit beside it (estimate),
6.57× and 6.04× in all.

## Gate rows

Counts need no hold; timings are the lead's, on `main`, under a strict hold. "Sync commit" is
`kv_strata` fillrandom, 100-byte values, durability sync, batch 1, 600 commits per run; "overwrite" is
the same over loaded keys.

| Row | Target | Procedure |
|---|---|---|
| T1 Files per commit | Per sync commit: 1 file created when the commit flushed a segment, 0 otherwise (a delete-only, a metadata-only, a pin); 0 renames and 0 unlinks inside `commit()`'s durable step; reclaim's unlinks reported apart. `main`: 2.1 created, 2 unlinks, 1 rename | `bench/kv/barriers.c` (`explore/sync-cpu` `ed0dbee`) counting `openat(O_CREAT)`, `unlinkat`, `renameat` per commit after the first rollover, fillrandom and overwrite |
| T2 Durability calls per commit | macOS: an edit alone 1 `F_FULLFSYNC`; with a segment 2 `fsync`s, 1 `F_BARRIERFSYNC`, 1 `F_FULLFSYNC`; `main` 5 `fsync`s and 2 `F_FULLFSYNC`s. Linux: 1 `fdatasync`, or 2 `fsync`s and 1 `fdatasync`. FreeBSD: the same with `fsync`. A rollover: `main`'s commit count | `barriers.c` on macOS; `strace -c -f -e trace=fsync,fdatasync` on Linux; `truss -c` on FreeBSD |
| T3 CPU per sync commit | At most 0.75× `main`'s CPU per commit and its p50 commit latency at most 0.6×, rollovers included, under completion's default barriers (`F_FULLFSYNC` on macOS), and reported under `BARRIERS_WEAK` as the explorer ran | `cpu_us_per_op` and the commit p50 from `kv_strata`: an A/A of the same length first, then 7 interleaved run pairs; the row passes when the worst pair meets the target and the A/A's spread is smaller than the gap; Linux and FreeBSD on the gate VMs |
| T4 Crash safety | After any crash the open yields the last acknowledged generation or the one in flight, its digest a never-crashed run's; no single-bit flip in a synced log opens silently; the cases below | faultfs crash points (`lifecycle` tests); a torn-tail fuzzer over journal's classifier from `tail_model.py`'s cases; `media_test` rerun |
| T5 Disk bound | After every commit of a churn run: the current log within max(F, 2 S) bytes and max(R, n, S / c) edits; during a rollover the new snapshot beside the old log until its reclaim slice; unlinked bytes not yet durable at most `reclaim_slice` plus one file; bytes written per commit for a large deleted-set index beside `main`'s (C5) | `bench/soak` and kv overwrite, the directory's bytes and the permit's counters sampled per commit |
| T6 Identity | The digest equal to `main`'s on the same operation streams; D3's oracle over random operation sequences, targets behind later deletes and metadata changes included; mutants: the change list without its marks, a metadata key emitted without comparing d's value | strata's index, kv and lifecycle suites; a property test |
| T7 Open cost | A 1- and a 100-document index: with a fresh log, open p50 at most 1.0× `main`'s (the model: 0.43×); at the replay floor, 128 edits of the commit mix and 128 delete-only ones, at most 1.0× (the model: 0.74× and 0.55×); a large index (`bench/soak`'s, at its trigger) within 1.1×; the first retention after a reopen with a pin across a rollover reported | open-many's open loop (small-indexes M0), five rotated rounds, strict hold; the logs built by commit loops |
| T8 Bytes | fields-and-keys T8 in the log's terms: the bytes the log writes for the table, each declaration in its edit and the table in every snapshot, at most 4× the final table for 2,000 and 100,000 declarations one per commit, on T8's workload (arithmetic: 2.46× and 3.38×); all bytes the log writes reported beside `main`'s `fields-*` and manifest bytes, and a run with a segment flush in each declaring commit (C6); bytes on disk per edit and per snapshot byte | `bench/field_table` counting the log's records by kind |
| T9 Many indexes | 10,000 open indexes: 1 descriptor per warm index; per small index (1 segment, metadata, fields), files and filesystem blocks at most `main`'s | open-many with `du` and the descriptor count |

T4's cases, each with a mutant that fails it:

1. A kill (faultfs power loss) at every mutation of an edit, a rollover, a truncate-back and lazy
   cleanup, over 3 to 100 indexes, with the batcher's batched commits among them (ML-R9; mutant: two
   edits of one log in flight).
2. Torn tails: for every append, every subset of its 512-byte sectors persisted and every size a crash
   can leave (E, E+n, each 512-byte multiple between): the open keeps the last whole generation and
   never reports damage (`tail_model.py` check 1).
3. Damage: every single-bit flip in a synced log fails the open, or passes on `read_checked`'s retry
   when transient; zero silent (check 2; mutant: revision 0's end-of-file rule, 18 silent on the
   model). Lost writes of the log's tail are the one silent class (Q3, ML-R17), reported: a sector
   the device acknowledged and never wrote, or a size update it lost, drops every edit from the
   earliest lost sector or size, which can be more than the last edit when edits share a sector.
4. An `ENOSPC` short write: the truncate-back leaves a clean log whose retry appends; a crash before
   the truncate's sync opens the previous generation or fails closed, never silently (check 6;
   mutant: no truncate-back).
5. `open(..., to)` on a log that ends before `to`: "generation not retained" (ML-R4).
6. A pin taken before a rollover, a merge, a reopen and a retention round: the pinned generation opens
   with `open(..., to)` and its merged-away segment is still there (L3; mutant: retention from the
   current log alone); an unreadable older log blocks retention and reports the generation.
7. The commit that frees space under `ENOSPC`, with a rollover due (ML-R5).
8. `media_test` over every file of the new layout (782 offsets on `main`'s): the digest never differs
   from the undamaged one.
9. A process kill after an edit's `pwrite` and before its `sync_data`, a reopen, then a power loss:
   the generation the reopen reports survives (ML-R15, C4; mutant: the open syncs nothing and reports
   the head durable).
10. A rollover stopped between its rename and its directory sync (the rename failing after its
    effect, or the directory sync failing), a reopen, a commit that writes no file (metadata only or
    deletes only), then a power loss: the acknowledged commit survives at every seed (mutant: the
    open syncs the log only).
11. An edit's `sync_data` failing with its pages kept clean but not durable (Linux's writeback
    error, faultfs's `keep`), a reopen in the same boot, a commit, then a power loss: the reopen
    reports the head before the failed edit and the acknowledged commit survives (mutant: a failed
    barrier not truncated back).

## Limits

- **Silent: lost writes of the log's tail** (ML-R10, ML-R17; the owner's Q3, D41). A sector the device
  acknowledged and never wrote reads as zeros, and a lost size update ends the file before an edit or
  at a block boundary inside it; either way the open reads a torn tail and returns the generation
  before the earliest lost sector or size. That can drop more than the last edit: a zeroed sector
  drops every edit that shares it (up to 3 on the re-check's model), and a size reverted past several
  appends drops them all (up to 39 there). `main` fails a manifest's lost bytes closed, since a renamed manifest is synced whole and
  zeros fail its CRC. `media-faults.md:22` puts lost writes outside the model for the consensus
  journal, because a lost write cannot be told from a torn tail without an independent record
  (`:104-108`); this design extends that row's scope to the index's metadata, which is Q3's question,
  and the turn log has the same limit for its last turn (`track-c.md:560-562`).
- **Sector atomicity.** An append rewrites the sector that holds the previous record's end with the
  same bytes; a device that tears a sector into neither old nor new bytes can damage an acknowledged
  record, which then fails closed. Phase 3 assumes sector-atomic writes for the field table already
  (`phase-3.md:21-28`); the log takes the assumption over, as the turn log does when a later turn
  continues a block (`track-c.md:487-491`). A tear inside a sector that leaves zeros after written
  bytes fails a check and reads as damage, never as torn.
- **File sizes.** D4's sizes hold by ext4's and XFS's sources. A filesystem that leaves another size
  after a crash makes the open fail closed: an availability loss, never a silent one. APFS, ZFS and UFS
  are not checked here.
- **A filesystem that shows stale bytes after a crash** (ext4 `data=writeback`) makes a torn tail read
  as damage: the open fails closed after an honest crash. ext4's default `data=ordered`, XFS, APFS, ZFS
  and UFS with soft updates do not, by their documentation (not tested here).
- **A crash inside a failed append's truncate-back** can leave a cut fragment, and the open fails
  closed. It takes an `ENOSPC` or an I/O error and then a crash before the truncate's sync.
- **Replay at the floor.** An index that committed up to R and went idle replays 128 edits at every
  open (59.3 µs of CPU by the model, 0.74× `main`'s); T7 bounds it.
- **A rollover writes the whole snapshot inside the commit that triggers it.** For an index whose
  snapshot is large (the deleted sets of big segments), that commit's latency includes writing S
  bytes (estimate: 50 to 100 ms for 100 MB at 1 to 2 GB/s); one such commit comes per R edits or 2 S
  bytes. Under K2 the rollover runs in a background checkpoint instead.
- **What a predecessor protects narrows** (Q2): damage before the last `retained_predecessors` edits
  of the current log leaves no readable generation whose files are kept.
- **Without the turn log, each synchronous commit still flushes a segment** that a merge later
  unlinks: about one create and one unlink per commit at steady state (D9).
- **The probes' floors are one platform's.** Linux and FreeBSD costs, and arm64's, are T1 to T3's.

## Rejected

- **CURRENT.** D5.
- **Appending the whole manifest per commit** (every record a snapshot). It needs no replay, but each
  commit writes every segment's entry and keeps live-docs and metadata files, so deletes and metadata
  changes keep creating files.
- **The manifest under its final name without a rename** (`sync-cpu.md`'s `STRATA_X_DIRECT`). It still
  creates a file per commit.
- **Lazy retention alone** (`STRATA_X_LAZY`). 7% less CPU, inside the A/A spread (`sync-cpu.md:34`).
- **Small segments inline in the log**, which would also reach 0 files per commit without the turn log:
  they keep a segment per commit for merges to undo, and they change how segments are stored, mapped
  and transferred. They belong to small-indexes' packing direction, if anywhere.
- **The manifest log as records of the turn log, or as a write-ahead log.** D9.
- **The end of file as a torn tail** (revision 0's D6): a flipped length past the end reads as torn,
  18 silent flips on the model. So would any rule that looks for zeros past the failing fragment: a
  later piece the crash never wrote would excuse the damage before it.
- **Edits that diff the head's deleted bitsets** (L2's other option). It needs no change list, but the
  writer would hold the head's bitsets, so the first delete after each commit copies a large segment's
  whole bitset (copy on write, `deleted_bits.hh`) and every edit scans the changed segments' words; the
  change list costs per change.
- **A file list per kept generation in the snapshot** (L3's other option). The open would read one log
  whatever is pinned, but it records again what the older log holds, and `open(..., to)` of that
  generation needs the older log anyway.
- **Rolling over at close past half the floor** (ML-R7's other option). It writes once per close of a
  recently written index, against cold-tier's warm state that writes nothing; R bounds the replay with
  no write outside commits.
- **One bound without the other.** Bytes alone let cheap-to-decode state stretch the replay (D7); a
  fixed edit count alone rewrites a growing table every R commits, 389× the final table at 100,000
  fields one per commit (arithmetic), against T8's 4×.

## Questions for the owner

Answered on 2026-11-09 (Xapiand's campaign D41): Q1 option 1, the manifest log first and K2 after it
as its own design and build; Q2 option 1, `retained_predecessors` stays a count of generations,
default 0, and `fallback()` reports the damaged generation; Q3 option 1, a lost write of the log's
tail reads as torn, as the turn log's last turn does.

**Q1. What lands first: the manifest log alone, or with the standalone turn log?** The manifest log
alone halves the file work of a synchronous commit (1,456 to 739 µs of CPU, two device flushes to
one, probe H against I) and makes a commit without a new segment an append and a barrier (183 µs, J);
only the turn log (K2) takes every synchronous commit to the append floor with no file (158 µs, K').

1. The manifest log first, then K2 as its own design and build, from durability-modes D1 and
   workload-profiles item 8.
2. K2 first, the manifest log after it as K2's checkpoint.
3. Both as one milestone, landing together.

Recommendation: 1, which the architect agreed with given L2. K2 needs a checkpoint either way and this
is it, since D3's edit can checkpoint a past generation; the manifest log lands a measurable win alone;
and K2 needs its own design pass (the transaction codec, group ids, the host's turn driver, replay into
the buffer). Option 2 builds K2 on today's manifest and migrates it later.

**Q2. `retained_predecessors` protects less.** Today a predecessor is a separate file, so damage to
the newest manifest falls back to it; in a log, damage to an edit loses every generation after it,
predecessors included. A damaged segment still falls back as today (`media-faults.md`'s test).

1. Keep the option as a count of generations: it covers damage to a segment the predecessors do not
   name and damage within the last k edits, and its comment says so. Default 0, so the default changes
   nothing. After damage the head's generation is unknown, so `fallback()` reports "damage at
   generation x" instead of a count of dropped generations, and the host's recorded newest generation
   (`phase-3.md:176-177`) chooses `to`.
2. With k above 0, also keep the previous log and the files of its last generation: damage anywhere in
   the current log falls back to the last rollover, losing every commit since; today's protection in
   kind, for one more log.
3. Retire the option (the owner set its default to 0 on 2026-10-20; RocksDB keeps none).

Recommendation: 1. The review rates 1 or 2 acceptable and blocks on neither; the option is
user-visible, so the choice is the owner's, and D8's mechanism serves any answer.

**Q3. A lost write of the last edit reads as torn** (Limits). Today the same lost write fails the
open.

1. Accept it: the design extends media-faults' lost-write row from the consensus journal to the
   index's metadata, as the turn log accepts it for its last turn.
2. Write a durable frontier elsewhere on every commit: a second barrier per commit, which undoes D4.
3. Rely on K2's checkpoint record, which closes it for checkpointed generations once K2 lands.

Recommendation: 1 now, with 3 when K2 lands, which the architect agreed with.

## First steps

Steps 1 and 2 may start before the architect's re-check, as the review allows; step 3 is the landing
unit and waits for it.

1. **journal: the log component** (D2, D6). Extract the turn log's emitter and scanner into a public
   component with the growing-file read and the tail classifier at L1's rule (D6's table), parameterized
   by block size; rebase the turn log on it with `RFTW0001`'s fixtures byte for byte and its T9 rows
   unchanged (the review's C1); a fuzz target over the classifier, seeded with `tail_model.py`'s
   logs, and its checks 1 to 6 ported as tests. Additive for Detent, which does not use the turn log
   (`track-c.md:592-596`); the lead tells Detent's lead before it starts. May start now.
2. **strata: the log codec** (D3), with no behavior change: the snapshot and edit encoders (the
   presence and section masks) and the decoder with D3's replay checks and running sums, fixtures, the
   fuzz target, and a round-trip property test over random pairs of states (replaying the encoded
   difference onto one gives the other, and their snapshots encode equal). Waits for step 1's
   interface, not its landing. May start now.
3. **strata: commit, recovery and retention on the log** (D3's change list, D4 to D8, D11): the log
   writer (one write per append, the truncate-back, one record in flight); `commit_files` appends the
   edit from the head to its target or rolls over by D7; `recover` replays and syncs the directory and the log (D6.6); `retain`
   keeps generations by D8, reading older logs and blocking as `main` does; `check()` replays the log
   and compares it with memory; the live-docs, metadata and field files retire; `commit_allowance`
   holds a snapshot and an edit; `pre_freeze` moves; T4's cases, T6's oracle, `media_test` updated.
   The landing unit, behind the architect's re-check of this revision and the security reviewer's
   decoder-safety review of step 2's reader (the floor for a format change).
4. **The lead measures** T1 to T3, T5, T7 to T9 on `main` after step 3 lands.
5. **K2**, if Q1 says so: a designer turns durability-modes D1 and workload-profiles item 8 into the
   standalone turn log, with this design's edit as its checkpoint, and owns K2's targets.

## What the re-check should look at

1. L1: D6's table and D4's size rule, against `tail_model.py`'s checks; whether "one write per
   append" holds on the filesystems the Limits leave unchecked, or fails closed there.
2. L2: the change list's two sites and its rollback, the marks a view and a batch entry record, and
   the metadata comparison against the head's map.
3. L3: D8's reads of older logs and the blocking rule against `index.hh:4716-4720`, with the open
   deferring retention (ML-R8).
4. D7's two bounds: S kept exactly by the writer, R's measured basis, and whether T7's and T8's
   targets follow from them.
