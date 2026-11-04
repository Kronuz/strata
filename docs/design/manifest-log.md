> Status: DRAFT (2026-11-04) designing (ready for preliminary review); the owner's ruling of
> 2026-11-04 asks for RocksDB's append-only manifests, replaced when they roll over and cleaned up
> lazily. Ledger: `docs/status/manifest-log.md`.

# The manifest log: commit metadata as an append-only log

Cited at strata `main` `2769928` (every `index.hh`, `files.hh` and `fields.hh` line below is at
it); journal `main` `bfad97f` for the turn log (`include/journal/turn_log.h`); completion `e2c6474`, strata's pin, for the
sync calls (`include/completion/posix.h`); the sync-CPU note `docs/perf/sync-cpu.md` on
`explore/sync-cpu` `ed0dbee`; RocksDB 11.11's source (`~/.scratch/rocksdb-11.11.fb`), read for its
MANIFEST, CURRENT and obsolete-file code, none of which enters strata. Measured here: the floor
probe `bench/manifest-log/mlog_probe.c` (a plain C loop, no strata code) on the development Mac
(Intel i9-9980HK, APFS, macOS 15.8.1) under a strict hold; every other number is the cited code's
or document's, arithmetic, or a labeled estimate.

## Summary

An index's durable state, apart from its segment files, becomes one append-only file per index,
the **manifest log** `manifest-<g>`: a snapshot of the state at generation g, then one edit per
commit. A commit appends its edit and issues one barrier. When the log grows past twice its
snapshot (with a floor), the next commit writes a fresh snapshot under a temporary name and
renames it into place, which is today's commit protocol run once per log instead of once per
commit. The old log and obsolete segments leave through the existing reclaim queue, lazily, with
no directory sync owed per commit.

Live-docs, metadata and field-table files fold into the log, so a commit creates a file only for a
segment it flushed. The framing is journal's turn-log framing at 512-byte blocks, one
implementation for both durable logs, and its 512-byte granularity makes the torn-tail rule exact:
a damaged edit fails the open instead of being dropped as torn.

The same file work in a plain C loop on the development Mac: a synchronous commit that flushes a
segment drops from **1,041 to 493 µs of CPU** (2.1× less) and from two device flushes to one
(45.5 to 23.2 ms); a commit with no new segment, an edit alone, costs **137 µs** ("Measured
floors").

The relation to the turn log: **the turn log builds on the manifest log.** The turn log stays the
write-ahead log (transactions, one barrier per core per turn); the manifest log is the checkpoint
its group record names. The 0-file synchronous commit at the append floor needs the commit's
documents in a log that already exists, which is the turn log's job (rocksdb-survey K2); without
it a synchronous commit still flushes one segment file. Q1 asks the owner for the order.

## Why

The sync-CPU explorer measured 1.8 to 2.1 ms of CPU per synchronous commit on `main` `356acbc`,
almost all system time in file metadata calls (`sync-cpu.md:11-24`): per commit 2.1 files created
(a segment and the manifest temporary), 2 unlinks, 1 rename, 5 `fsync`s and 2 `F_FULLFSYNC`s, at
126 to 241 µs of thread CPU per create, unlink or rename, against 33 µs to append to an open file
and `fsync` it. That is the loss on every synchronous row against RocksDB, 4.76 to 5.09× slower
(`core-vs-rocksdb-f14.md:20-22`). The two cheap switches the explorer tried, lazy retention and a
manifest written under its final name, removed at most 21% of the CPU together
(`sync-cpu.md:26-36`).

The churn is the commit protocol itself: a live-docs file per segment with new deletes, a metadata
file when it changed and the field table's appended block (`index.hh:4556-4603`), the manifest
under a temporary name (`:4647-4651`), one ordering point, the rename and a directory sync
(`:4652-4656`), then retention's unlink of the previous manifest and another directory sync
(`:4734-4762`), and reclaim's unlinks in the same call (`:944`).

## What this builds on, and what it changes

| Document | Used | Changed |
|---|---|---|
| `phase-3.md` D1 to D5 (`:44-227`) | generations, pins, `retained_predecessors`, `open(..., to)`, install, fencing on a failed sync | the directory loses `*.live`, `meta-*.kv`, `fields-*.kv`; commit is an append (D4); recovery replays (D6); retention drops "manifests leave first" (`:177-178`) and its lifecycle condition 9 (D8) |
| `fields-and-keys.md` D2, T8 | the append-only table and its growth row | the table lives in the log (D1); T8 holds |
| `durability-modes.md` D1 (`:825-955`), T16 | commit splits into durability, visibility, checkpoint; the recorded generation (DM-R43); retention on the durable head (DM-R23) | its checkpoint, "today's commit protocol" (`:831`), is D4's edit; the recorded generation is a kept generation (D8); D17's file sets carry segments only |
| `track-c.md` D11 (`:460-625`) | the turn log; its group checkpoint record names an application checkpoint (`:518-522`, `:582-587`) | the reference is a manifest-log generation; the framing is shared (D2) |
| `workload-profiles.md` item 8 (`:100-106`), `rocksdb-survey.md` K2 (`:207`) | standalone durability through the turn log, a group of one | none; D9 states how the two logs meet |
| small-indexes (`perf/small-indexes` `39ba962`) D1 to D3 | the owner part released at commit; one descriptor per idle index; the commit batcher | the batcher syncs logs (D4); D3-C's create residue is `manifest-1.tmp` alone |
| `cold-tier.md` D10, D11 | warm writes nothing and holds one descriptor; open checks names and sizes | demotion closes the log's descriptor; open reads one file |
| `media-faults.md` | one retry then damage; repair from a predecessor | the torn-tail rule (D6) and its residual (Q3); what a predecessor protects (Q2) |
| `limits.md` | every cap names its mechanism | D7's trigger is a preset; D8's sync rule |

## Decisions

### D1. One log per index: a snapshot, then one edit per commit

- The index's directory holds its segments (`seg-<id>.sx`) and its manifest logs. The current log
  is `manifest-<g>`, named by the generation of its snapshot, its first record. Each commit appends
  one edit, generations g+1, g+2, ... in order.
- **An edit is everything one commit makes durable:** the manifest's scalars as of its generation,
  segments removed and added, documents newly deleted in existing segments, metadata entries set
  and removed, field declarations appended, pins added and removed (D3).
- **Live-docs, metadata and field-table files retire.** Their content moves into the log in the
  encodings strata already has: a segment's deleted set as the live-docs encoding (a bitset or
  LEB128 deltas, whichever is smaller, `files.hh:355-380`), a metadata pair as `files.hh:420-432`
  writes it, a declaration as the field table writes it (`fields.hh:182-189`). Folding them is what
  makes a delete or a metadata change cost no file: today a replace that supersedes a copy in an
  older segment writes that segment a live-docs file at the next commit (`index.hh:4558-4569`).
- One log per index, not one per host as RocksDB keeps one MANIFEST per database: the directory
  stays self-contained (install reads a pinned generation from another index's directory,
  `index.hh:1587-1596`), an index opens alone, and a cold index costs nothing outside its directory.

### D2. Framing: journal's blocks and fragments, at 512-byte blocks

- The log's bytes are the turn log's framing (`turn_log.h:107-160` writes it, `:175-221` reads
  it), moved into a component of journal that both logs use: blocks, each with a header (magic,
  identity, segment number, block number, epoch, CRC-32C); records split into fragments (full,
  first, middle, last), each under its own CRC-32C and bound to its segment and epoch. RocksDB
  shares one `log::Writer` between its WAL and its MANIFEST the same way. A second framing in
  strata would be the duplicate mechanism calibration's first row flags.
- For a manifest log: the magic is `STRLOG01`, the identity is the index UUID, the segment number
  is g, the epoch is 1 (a log is never recycled), and no block manifest is written. Its block size
  is **512 bytes**; the turn log keeps 32 KiB.
- Why 512: every 512-byte piece of an append then begins, past any block padding, with a block
  header (its magic) or a fragment header (its length), never all zeros, so a piece that reads as
  all zeros is a sector the crash never wrote.
  That is what lets D6 tell a torn append from damage. The cost is framing bytes: 62 per block
  header without a manifest and 24 per fragment (`turn_log.h:82-84`), so a block carries at most
  426 bytes of one record, a snapshot takes about 1.2× its payload on disk (512/426), and an edit
  of about 70 payload bytes takes about 100 to 130 bytes with its share of a block header
  (estimates; T8 measures them).
- What the component needs that the turn log does not: reading a growing file, whose last block is
  partial (the turn log scans whole preallocated blocks only, `turn_log.h:180`), and D6's tail
  classifier. Neither changes a byte the turn log writes.

### D3. Records

A record is a kind (1: snapshot, 2: edit), its generation (LEB128), and a payload of LEB128 fields
unless noted. The decoder follows `decode_manifest_for` (`files.hh:279-351`): readability first
(the files version, then the mask and segment versions against what this binary reads,
`unsupported_index`), then every length bounded by the bytes left, then the values.

| Part | Snapshot | Edit |
|---|---|---|
| Files version | first field | absent: the snapshot's governs the file |
| Scalars: UUID, revision, last docid, next segment id, applied position, schema version, configured, mask, segment versions, document count, total length, live count, live length | all | all but the UUID (the blocks' identity), as of this generation |
| Pins | the set | added; removed |
| Segments | each entry: id, identity, size, footer CRC, document count, live count, live length, its deleted set (none, bitset or deltas), values generation, attributes | ids removed; entries added, as in a snapshot |
| New deletes in existing segments | (in the entries) | per segment: id, the ordinals newly deleted (ascending deltas), its live count and live length after them |
| Metadata | every pair, keys ascending | pairs set; keys removed |
| Field table | every declaration | declarations appended, after the table size they extend |
| Segment attributes | (in the entries) | per segment, the set, when it changed (no tag is defined yet, `files.hh:66-78`) |
| Index attributes | the set | the set, when it changed |

- **Replay checks:** generations contiguous within a log; a removed id is present; an added id is
  absent and below the edit's next segment id, and goes in id order (a merge's output takes its id
  when it is scheduled, below segments flushed since, `index.hh:3358-3362`); a newly deleted
  ordinal is below its segment's document count and not yet deleted; after each record the totals
  equal the sums over segments (total length is carried, as segments do not record it); pins at most
  the generation. A violation is `corrupt_index`.
- **The writer's edit is the difference between the durable state and the next one**, built where
  `commit_files` builds the next manifest today (`index.hh:4605-4643`). Ordinals newly deleted in an
  existing segment come from one site, the freeze's `apply_pending` (`index.hh:4293-4310`), which
  records each into its segment's list for the next edit; a segment born since the last edit (a
  flush, whose newer buffers may delete in it, `index.hh:4341-4346`, or a merge carrying deletes,
  `:3344-3349`) carries its whole set as an added entry. Metadata keys set or removed since the last
  edit are kept the same way, where `meta_dirty_` is a flag today (`index.hh:5198`).
- **One oracle:** for every state s and next state n, replaying `diff(s, n)` onto s gives n, and a
  snapshot of the replayed state encodes byte for byte as a snapshot of n (T6).

### D4. The commit: one append and one barrier

```
main (index.hh:4535-4676, 4697-4769)            manifest log
  write segment, live-docs, meta, fields block     write segment (when the commit flushed one)
  write manifest-<g>.tmp                           order it and the directory: fsync each,
  order all + directory: fsync each, fsync dir,      fsync dir, barrier (only when a file was written)
    barrier                                        append the edit at the log's end
  rename tmp -> manifest-<g>                       log.sync_data(): fdatasync (Linux),
  sync dir: fsync dir + F_FULLFSYNC                  F_FULLFSYNC (macOS), fsync (FreeBSD)
  retain: unlink manifest-<g-1>, sync dir          retain: queue obsolete files and logs (D8)
```

- The ordering point then the append is Apple's documented two-phase `F_BARRIERFSYNC` protocol
  that `main` already follows for the rename (`docs/reviews/commit-barriers-durability.md:35-40`):
  no edit naming a segment reaches the device ahead of the segment and its directory entry.
  `sync_data` is completion's (`posix.h:169-177`, `:358`).
- **One record in flight.** Commits run under the write lease and the edit's barrier returns
  before the next commit can append, so at most the last record of a log can be torn: D6's premise,
  as one turn in flight is the turn log's (`track-c.md:506-509`).
- **Failures, unchanged in kind** (`index.hh:926-929`, `2654-2670`): an `ENOSPC` while writing a
  segment or the edit leaves the index as it was; a failed append also marks the log's tail dirty,
  so the retry rolls over (D7). Any failed sync, rename or directory sync fences.
- **Batching across indexes** (small-indexes D3-A): an entry's sync is its log's `sync_data`, and
  macOS's `files_sync` stages each and flushes each device once (`posix.h:186-225`).

### D5. Rollover: the name is the pointer

- A rollover writes the state as a snapshot to `manifest-<g>.tmp` (g the generation it commits),
  syncs it at the commit's ordering point with any new segment, renames it to `manifest-<g>`, and
  syncs the directory. That is today's commit protocol (`index.hh:4644-4656`), so a rollover costs
  the manifest half of `main`'s commit: 445 µs of CPU in probe F.
- **No CURRENT file.** The highest final-named `manifest-<g>` is the current log, and the rename is
  the atomic switch. RocksDB writes CURRENT through a temporary and a rename
  (`file/filename.cc:458-496`) to find its MANIFEST without listing the directory; strata's open
  lists it anyway (`index.hh:4889-4913`), so CURRENT would add a file and a read per index and an
  extra rename and sync per rollover, for nothing. At millions of indexes a file is at least one
  filesystem block each (probe M below).
- A rollover never removes the old log; D8 does, lazily.

### D6. Recovery: replay, the torn tail, and an open that writes nothing

1. The directory scan (`index.hh:4889-4913`) finds the logs; `.tmp` names are orphans, removed
   after a successful open as today (`:4954-4957`).
2. Read the highest log whole: one read, bounded by D7.
3. Its snapshot must read whole and name g. A log is renamed only after its snapshot was synced, so
   a bad snapshot is damage: `corrupt_index`, after `read_checked`'s one retry
   (`index.hh:4986-5008`).
4. Replay edits in order and stop at the first one that does not read whole. Classify the bytes
   from there to the end of the file:

| What the tail shows | Verdict |
|---|---|
| Nothing | Clean |
| The record runs past the end of the file, or one of its 512-byte pieces (from its first fragment, past any block padding) reads as all zeros, and no whole record follows | Torn: a crash cut the last append. Dropped; the log is marked dirty |
| Anything else: every piece holds bytes yet a check fails, or a whole record follows a bad one | Damage: `corrupt_index`. A record whose every piece was written and that fails a check changed after it was written; a bad record with a whole one after it was acknowledged, since one record is in flight (D4) |

5. The head must find every file it names, with its size, as today (segments by the scan's names
   and sizes, cold-tier D11).
6. **The open writes nothing.** A dirty log rolls over at the next commit (D7), as RocksDB's
   `reuse_manifest_on_open` makes a fresh MANIFEST when the file's size differs from its last valid
   record's end (`db/version_set.cc:6731-6746`). A crash costs one rollover later; reopening an
   idle index never writes.
7. Counters start above every name on disk (phase-3 D3, `index.hh:4941-4946`); after a dirty tail
   the next generation skips one, so a torn record's generation is never reused, as today never
   reuses a failed attempt's manifest name (`index.hh:4649`).
8. `open(..., to)` replays the log with the greatest snapshot generation at or below `to`, up to
   `to`; a file it names that is gone fails it as "generation not retained". It then rolls over to
   commit that state as the newest generation, as today commits the older manifest again
   (`index.hh:4947-4953`), and `fallback()` counts the readable generations it dropped.

Crash points:

| Crash during | Left on disk | The open finds |
|---|---|---|
| A segment's write or its ordering point | an orphan segment, the log unchanged | the previous generation; the orphan removed |
| The edit's append, before `sync_data` returns | the edit absent, torn, or whole | the previous generation (dirty when torn), or the new one |
| A rollover's temporary, before the rename | `manifest-<g>.tmp` | the old log's head; the temporary removed |
| The rename, before its directory sync | either name | the old log's head or the new log's snapshot; the commit was not yet acknowledged |
| After the directory sync, before cleanup | both logs | the new log's head; the old log is obsolete and queued |
| Lazy cleanup | any subset of obsolete files and logs gone | the head, which names none of them |
| The open's own orphan removal | some orphans gone | the same head |

### D7. The rollover trigger

A commit rolls over when its log is dirty (D6, or a failed append in this process), when it
replaces the content (install, `open(..., to)`), when the index has no log yet (its first commit),
or when the log would pass **max(floor, 2 S)** bytes, S the current snapshot's.

- **2 S** (RocksDB's `max_manifest_space_amp_pct` at 100%, `include/rocksdb/options.h:1071-1111`):
  the open reads and replays at most twice the snapshot plus the floor, and snapshots add at most
  one byte written per byte of edits. RocksDB defaults to 500% over a 1 GiB minimum
  (`options.h:1032`, `:1111`) for a few large databases; strata opens millions of indexes lazily, so
  the open's replay is what bounds the log.
- **The floor, 16 KiB, amortizes the rollover's fixed cost**, so a tiny snapshot does not roll over
  every few commits. A rollover costs 445 µs of CPU with a 1 KiB snapshot and 502 µs with 64 KiB
  (probes F and G), nearly all of it fixed; 16 KiB holds about 125 edits of 130 bytes, so each edit
  carries 3.6 µs of it:
  0.7% of a commit that flushed a segment (I, 493 µs), 2.6% of an edit made durable alone on macOS
  (J, 137 µs), 12% of an edit priced as an append and an `fsync` (A, 30 µs), which is what Linux's
  `fdatasync` may cost and T3 measures there.
- Both are presets with a named mechanism (`limits.md`'s rule); T3, T5 and T7 move them.

### D8. Retention and lazy cleanup

- **Kept generations:** the head, the pinned ones, the `retained_predecessors` before the head
  (`index.hh:435-439`), and, under durability-modes D1, the recorded generation (DM-R43). A kept
  generation stays openable: its log and every file its state names stay on disk.
- A log holding no kept generation is obsolete, and so is every file no kept generation names. Both
  join the reclaim queue that already frees data files (`index.hh:3573-3621`: truncated in
  `reclaim_step`s, unlinked, posted to the host's executor when it has one), with **no ordering
  between logs and data files**. `open` picks the highest log and `open(..., to)` refuses a
  generation whose files are gone (D6), so nothing promises an obsolete log's generations open.
  That drops phase 3's rule that a manifest leaves before the data files only it named
  (`phase-3.md:177-178`),
  the lifecycle review's condition 9 that enforces it (`reviews/phase-3-lifecycle.md:143-147`), and
  `retain()`'s directory sync between the two passes (`index.hh:4749-4762`).
- **Removals are lazy.** A commit no longer syncs the directory unless it wrote a file, so an unlink
  becomes durable at the next directory sync: a commit's ordering point, a rollover, `drain()`,
  `close()`, or the reclaim path's own sync, which it issues once its unsynced removals pass
  `reclaim_slice`. Until then the storage permit counts them (`index.hh:3649-3661`, `3723-3733`).
- **The bound on extra disk:**

| What waits | Bound | Default |
|---|---|---|
| Obsolete logs | one log per kept generation outside the current log, each at most max(floor, 2 S) of its own snapshot | none: a rollover's old log is queued in the same commit and unlinked in its reclaim slice |
| Unlinked, not yet durable | `reclaim_slice` plus one file | 64 MiB plus one file |
| Obsolete data files | `limits.md:26`'s reclaim row, unchanged | as today |

### D9. The turn log builds on the manifest log

- **The turn log is the write-ahead log.** Track C D11's journal holds transactions, each
  acknowledged at its turn's barrier, one barrier per core per turn (`track-c.md:497-505`); a
  standalone index uses it as a group of one (`workload-profiles.md:100-106`, rocksdb-survey K2).
  The manifest log never holds a transaction.
- **The manifest log is the checkpoint.** Durability-modes D1's third step, "today's commit
  protocol" (`durability-modes.md:831-835`), becomes D4's edit. The group checkpoint record names
  the edit's generation (`track-c.md:518-522`, `:582-587`) and is staged only after the edit is
  durable (Condition 25's order, `:608-612`); replay opens that generation and applies the batches
  after it (`durability-modes.md:864-881`).
- **Not subsumed by the turn log.** Writing the snapshot and edits as turn-log records would remove
  the manifest file, but an index's directory would no longer open alone (install and tools read
  it), every cold index would hold a resident entry in its core's turn log and have its snapshot
  copied forward by the turn log's reclamation (`track-c.md:523-535`), and the owner's ruling asks
  for manifest files that roll over.
- **Not a write-ahead log itself.** Carrying the commit's documents in the edit would reach 0 files
  per commit without the turn log, as a second log of transactions beside it, which
  workload-profiles item 8 rules out ("the same mechanism as the replicated path, not a second
  one").
- **So the synchronous commit's floor is K2's.** Without a turn log, `commit()` stays a checkpoint:
  a segment file per commit that flushed documents, the ordering point, the edit. With it, a commit
  is one turn-log append and one barrier, and checkpoints run in the background at the share D11
  gives each group (`track-c.md:725-727`). K2 must also settle whether an index whose checkpoint
  covers its last batch leaves the turn log, so a cold index holds nothing there. Q1 asks for the
  order.
- Under K2 the checkpoint record is the durable frontier the manifest log lacks alone, which closes
  Q3's residual for every checkpointed generation.

### D10. Cost at millions of indexes per host

| Per index | `main` | Manifest log |
|---|---|---|
| Files besides segments | the manifest, `meta-<g>.kv`, `fields-<g>.kv`, a live-docs file per segment with deletes | one log |
| Disk for them, small index | one 4 KiB block each (probe M): 16 KiB with metadata, fields and one live-docs file | 4 KiB after a rollover, up to 16 KiB just before the next one |
| Descriptors, hot | the directory (small-indexes D2) | the directory and the log, while it appends |
| Descriptors, warm or cold | 1 or 0 | 1 or 0: demotion closes the log (cold-tier D10's warm writes nothing and keeps the lock only) |
| Heap per open index | the manifest, the file table | the same less the live, metadata and fields references, plus the log's state (its generation, end offset, snapshot bytes, dirty flag: 32 bytes) and each segment's list of new deletes until the next edit |
| Open reads | the manifest, the metadata, the field table and each live-docs file | one log, at most max(floor, 2 S) |

Disk is no win for a small index that commits often: its log saws between one block and four. It
is a win for an idle index (one block) and for an index with deletes in many segments, which `main`
gives a file each. Reopening the log for each commit would cost 20 µs of CPU (probe D against A),
two thirds of an append, so the log stays open while the index is hot and closes when it demotes;
a host's descriptor budget counts it (`limits.md:59`).

### D11. Format

`files::pre_freeze` moves to `0x8000'0008` (`files.hh:38-45`), so a build refuses another
pre-freeze build's index (F2, F20); strata has no release, so nothing migrates. A `manifest-<g>`
that starts with the retired `STRMAN01` is another build's index: `unsupported_index`, never
damage. The magics `STRLIV01` and `STRMET01` and the field table's file header retire; the
manifest's fields become the snapshot's. Fixtures: a log with a snapshot and every kind of edit; a
torn tail; a damaged middle record; a dirty tail then its rollover. One CPU-hour of fuzzing on the
log reader, framing and records together (TESTING's row).

## Measured floors

`bench/manifest-log/mlog_probe.c`: the file work alone, in a plain C loop (no strata code), on the
development Mac (Intel i9-9980HK, APFS, macOS 15.8.1), under a strict hold, three rounds; CPU is the
process's user and system time per operation, median (range). Output:
`bench/manifest-log/results/2026-11-04-floors.txt`.

| Probe | The file work | CPU µs | Wall |
|---|---|---:|---:|
| A | append 100 bytes to an open file, `fsync` | 30.4 (25.3-37.1) | 65 µs |
| B | the same, `F_FULLFSYNC` | 139.1 (127.6-159.4) | 23.2 ms |
| C | the same, `F_BARRIERFSYNC` | 56.4 (50.3-77.2) | 0.50 ms |
| D | open, append, `fsync`, close | 50.3 (48.5-71.9) | 111 µs |
| E | open an existing file, close | 12.5 (12.5-15.6) | 13 µs |
| F | rollover: a 1 KiB snapshot to a temporary, `fsync`, rename, directory `fsync` and `F_FULLFSYNC` | 444.8 (438.6-445.8) | 22.9 ms |
| F' | unlink an old log, no directory sync | 100.0 (75.7-104.2) | 101 µs |
| G | rollover with a 64 KiB snapshot | 502.4 (499.7-509.1) | 22.9 ms |
| H | `main`'s commit: a 2 KiB segment and a 700-byte manifest temporary, the ordering point, the rename, a directory sync, the old manifest unlinked and another directory sync, an older segment unlinked | 1,041.3 (966.4-1,062.3) | 45.5 ms |
| I | the log's commit: the same segment, its ordering point, a 100-byte edit appended with `F_FULLFSYNC`, an older segment unlinked | 492.8 (486.3-540.3) | 23.2 ms |
| J | the log's edit alone, `F_FULLFSYNC` | 137.0 (126.3-146.1) | 21.8 ms |
| K | a turn-log-like append: 150 bytes into preallocated space, `fsync` | 24.6 (18.8-34.0) | 57 µs |
| K' | the same, `F_FULLFSYNC` | 135.0 (107.6-151.0) | 22.5 ms |
| L | open, `fstat`, read 4 KiB or 64 KiB from the page cache, close | 12.1 or 15.4 | 12 or 15 µs |
| M | the disk a 16-byte file takes | 4,096 bytes | |

What they say: I against H is the manifest log's commit at 0.47× of `main`'s file work, outside
both spreads, and one device flush instead of two; strata's own commit measured 1.8 to 2.1 ms
(`sync-cpu.md:11`), so about 0.9 ms of it is outside the calls H models, which the explorer could
not attribute either (`sync-cpu.md:19-23`) and which may or may not shrink with the files. K' is
the floor a turn-log commit approaches, below RocksDB's 219 µs per operation on the same host
(`sync-cpu.md:24`). These are one platform's floors; T1 to T3 measure strata on all three.

## Gate rows

Counts need no hold; timings are the lead's, on `main`, under a strict hold, three interleaved
rounds with an A/A first (`sync-cpu.md:28-36`'s procedure). "Sync commit" is `kv_strata`
fillrandom, 100-byte values, durability sync, batch 1, 600 commits per run; "overwrite" is the same
over loaded keys.

| Row | Target | Procedure |
|---|---|---|
| T1 Files per commit | Manifest log alone: per sync commit 1 file created when the commit flushed a segment, 0 otherwise (a delete-only, a metadata-only, a pin); 0 renames and 0 unlinks inside `commit()`'s durable step; reclaim's unlinks reported apart. With K2: 0 created per commit in steady state. `main`: 2.1 created, 2 unlinks, 1 rename | `bench/kv/barriers.c` (`explore/sync-cpu` `ed0dbee`) counting `openat(O_CREAT)`, `unlinkat`, `renameat` per commit after the first rollover, fillrandom and overwrite |
| T2 Durability calls per commit | macOS: an edit alone 1 `F_FULLFSYNC`; with a segment 2 `fsync`s, 1 `F_BARRIERFSYNC`, 1 `F_FULLFSYNC`; `main` 5 `fsync`s and 2 `F_FULLFSYNC`s. Linux: 1 `fdatasync`, or 2 `fsync`s and 1 `fdatasync`. FreeBSD: the same with `fsync`. A rollover: `main`'s commit count | `barriers.c` on macOS; `strace -c -f -e trace=fsync,fdatasync` on Linux; `truss -c` on FreeBSD |
| T3 CPU per sync commit | Manifest log alone: at most 0.75× `main`'s CPU per commit and its p50 commit latency at most 0.6× (the floors give 0.47× and one device flush instead of two, I against H; the unattributed 0.9 ms may not shrink). With K2: at most 170 µs on the development Mac, 1.25× the append-and-barrier floor K' (135 µs), against RocksDB's 219 µs | `cpu_us_per_op` and the commit p50 from `kv_strata`, the explorer's A/B procedure; Linux and FreeBSD on the gate VMs |
| T4 Crash safety | A kill (faultfs power loss) at every mutation of an edit, a rollover and lazy cleanup, over 3 to 100 indexes: every open yields the last acknowledged generation or the one in flight, its digest equal to a never-crashed run's, never `corrupt_index`. Torn-tail fuzz: for each edit, every subset of its 512-byte sectors persisted and every truncation of the append: the open keeps exactly the synced prefix and calls it torn. Damage: any bit flipped in any synced record fails the open (or passes on `read_checked`'s retry when transient); a zeroed 512-byte piece of the last edit is the one silent case (Q3), reported as such. `media_test` over every file of the new layout (782 offsets on `main`'s): the digest never differs from the undamaged one | faultfs crash points (`lifecycle` tests); a torn-tail fuzzer over the log reader; `media_test` rerun |
| T5 Disk bound | Over a churn run, after every commit: the current log at most max(floor, 2 S), plus the old log until the commit's reclaim slice removes it; unlinked bytes not yet durable at most `reclaim_slice` plus one file | `bench/soak` and kv overwrite, the directory's bytes and the permit's counters sampled per commit |
| T6 Identity | The digest equal to `main`'s on the same operation streams; `diff`/apply round trip and encode equality over random operation sequences (D3's oracle); a revert of the per-segment delete list fails it | strata's index, kv and lifecycle suites; a property test |
| T7 Open cost | A 1- and a 100-document index with a fresh log, and with a log at its floor: open p50 within 1.1× of `main`'s and 1.25× of it respectively; a large index at 2 S within 1.1× | open-many's open loop (small-indexes M0), five rotated rounds, strict hold |
| T8 Bytes | Bytes on disk per edit and per snapshot byte reported; fields-and-keys T8: 2,000 and 100,000 declarations one per commit write at most 4× the final table | `bench/field_table`; the log's size per commit |
| T9 Many indexes | 10,000 open indexes: 1 descriptor per warm index; files and filesystem blocks per small index (1 segment, metadata, fields) against `main` | open-many with `du` and the descriptor count |

## Limits

- **Silent: a lost write of the last edit.** A sector of the last edit that the device acknowledged
  and never wrote reads as zeros, so the edit reads as torn and the open returns the generation
  before it. `main` fails such a manifest's checksum instead. Lost writes are outside the media
  model (`media-faults.md:22`), and the turn log has the same limit for its last turn
  (`track-c.md:560-562`). Q3.
- **Sector atomicity.** An append rewrites the sector that holds the previous record's end with the
  same bytes; a device that tears a sector into neither old nor new bytes can damage an
  acknowledged record. Phase 3 assumes sector-atomic writes for the field table already
  (`phase-3.md:21-28`); the log takes the assumption over, as the turn log does when a later turn
  continues a block (`track-c.md:487-491`).
- **A filesystem that shows stale bytes after a crash** (ext4 `data=writeback`) makes a torn tail
  read as damage: the open fails closed after an honest crash. ext4's default `data=ordered`, XFS,
  APFS, ZFS and UFS with soft updates do not, by their documentation (not tested here).
- **An index that committed up to its floor and went idle replays about 125 edits at every open.**
  If T7 misses, the sweep's close rolls over a log past half its floor (one write per close of an
  index written since its last rollover), or the floor drops.
- **A rollover writes the whole snapshot inside the commit that triggers it.** For an index whose
  snapshot is large (the deleted sets of big segments), that commit's latency includes writing S
  bytes (estimate: 50 to 100 ms for 100 MB at 1 to 2 GB/s); one such commit comes per S bytes of
  edits. Under K2 the rollover runs in a background checkpoint instead.
- **What a predecessor protects narrows** (Q2): damage before the last `retained_predecessors`
  edits of the current log leaves no readable generation whose files are kept.
- **Without the turn log, each synchronous commit still flushes a segment** that a merge later
  unlinks: about one create and one unlink per commit at steady state (D9).
- **The probe's floors are one platform's.** Linux and FreeBSD costs, and arm64's, are T1 to T3's.

## Rejected

- **CURRENT.** D5.
- **Appending the whole manifest per commit** (every record a snapshot). It needs no replay, but
  each commit writes every segment's entry and keeps live-docs and metadata files, so deletes and
  metadata changes keep creating files.
- **The manifest under its final name without a rename** (`sync-cpu.md`'s `STRATA_X_DIRECT`). It
  still creates a file per commit, and a torn one cannot be told from a damaged one.
- **Lazy retention alone** (`STRATA_X_LAZY`). 7% less CPU, inside the A/A spread (`sync-cpu.md:34`).
- **Small segments inline in the log**, which would also reach 0 files per commit without the turn
  log: they keep a segment per commit for merges to undo, and they change how segments are stored,
  mapped and transferred. They belong to small-indexes' packing direction, if anywhere.
- **The manifest log as records of the turn log, or as a write-ahead log.** D9.

## Questions for the owner

**Q1. What lands first: the manifest log alone, or with the standalone turn log?** The manifest
log alone halves the file work of a synchronous commit (1,041 to 493 µs of CPU, two device
flushes to one, probe H against I) and makes a commit without a new segment an append and a
barrier (137 µs, J); only the turn log (K2) takes every synchronous commit to the append floor
with no file (135 µs, K').

1. The manifest log first, then K2 as its own design and build, from durability-modes D1 and
   workload-profiles item 8.
2. K2 first, the manifest log after it as K2's checkpoint.
3. Both as one milestone, landing together.

Recommendation: 1. It is self-contained, lands a measurable win alone, and K2 needs its own design
pass (the transaction codec, group ids, the host's turn driver, replay into the buffer); K2 then
builds on its edits.

**Q2. `retained_predecessors` protects less.** Today a predecessor is a separate file, so damage to
the newest manifest falls back to it; in a log, damage to an edit loses every generation after it,
predecessors included. A damaged segment still falls back as today (`media-faults.md`'s test).

1. Keep the option as a count of generations: it covers damage to a segment the predecessors do not
   name and damage within the last k edits, and its comment says so. Default 0, so the default
   changes nothing.
2. With k above 0, also keep the previous log and the files of its last generation: damage anywhere
   in the current log falls back to the last rollover, losing every commit since.
3. Retire the option (the owner set its default to 0 on 2026-10-20; RocksDB keeps none).

Recommendation: 1.

**Q3. A lost write of the last edit reads as torn** (Limits). Today the same lost write fails the
open.

1. Accept it, as the turn log does: lost writes are outside the model.
2. Write a durable frontier elsewhere on every commit: a second barrier per commit, which undoes D4.
3. Rely on K2's checkpoint record, which closes it for checkpointed generations once K2 lands.

Recommendation: 1 now, with 3 when K2 lands.

## First steps

1. **journal: the framing as a component** (D2). Extract the turn log's emitter and scanner into a
   public component with the growing-file read and the tail classifier; rebase the turn log on it
   with `RFTW0001`'s fixtures byte for byte and its T9 rows unchanged; extend its fuzz target to the
   classifier. Additive for Detent, which does not use the turn log (`track-c.md:592-596`); the lead
   tells Detent's lead before it starts.
2. **strata: the log codec** (D3), with no behavior change: snapshot and edit encoders and the
   decoder, fixtures, the fuzz target, and D3's round-trip property test. Waits for step 1's
   interface, not its landing.
3. **strata: commit, recovery and retention on the log** (D4 to D8, D11): `commit_files` appends or
   rolls over; `recover` replays; `retain` keeps generations; `check()` replays the log and compares
   it with memory; the live-docs, metadata and field files retire; `pre_freeze` moves; T4's crash
   and torn-tail tests, T6's oracle, `media_test` updated. The landing unit, behind the
   architect's review of this design and the security reviewer's decoder-safety review of step 2's
   reader (the floor for a format change).
4. **The lead measures** T1 to T3, T5, T7 to T9 on `main` after step 3 lands.
5. **K2**, if Q1 says so: a designer turns durability-modes D1 and workload-profiles item 8 into
   the standalone turn log, with this design's edit as its checkpoint.

## What the preliminary review should look at

1. D6's tail classifier: whether 512-byte blocks make "an all-zero piece" mean exactly "a sector the
   crash did not write", block padding and partial last blocks included.
2. D8's dropped ordering between logs and data files, and what `open(..., to)` may still promise.
3. D9: whether the two logs share enough (the framing) to count as one mechanism, and the checkpoint
   record's order against the edit.
4. D3's replay checks and the per-segment list of new deletes as the only source of an edit's
   ordinals.
