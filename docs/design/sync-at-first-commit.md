# Sync at first commit: an open never syncs

Status: design (quest sync-at-first-commit, ruling D60c of the Xapiand campaign). An open takes the
exclusive directory lock as today and issues no `fsync` or `F_FULLFSYNC`. The state it inherited
from an earlier process becomes durable inside the handle's first commit, before that commit
returns, through the commit's own ordering point and log sync. This replaces manifest-log D6.6's
"the open syncs the log and the directory".

## Why

`basic_index::open` runs one `sync_files_and_directory({log})` before it reports the head
(`index.hh:5265`, `docs/perf/open-sync.md`). Under other indexes' write load on macOS that is one
`F_FULLFSYNC`, which flushes the whole drive cache: 22.9 to 23.1 ms median per open (job
261112-030638-fcc49d). A host serving millions of indexes pays it on every open, including the many
that are only searched and closed again. The sync protects a writer: it makes durable the state the
next commit builds on. So the writer pays it, once, folded into its first commit.

## What the open inherits

Two states a process crash leaves that read clean to the replay (T4 cases 9 and 10,
`test/index_test.cc`):

1. **An unsynced tail edit.** The crashed writer's `pwrite` of edit g returned and its `sync_data`
   never ran. The open reads g; a power loss can take it back. g was never acknowledged.
2. **An unsynced rollover rename.** The crashed writer renamed `manifest-N.tmp` to `manifest-N`
   and never synced the directory. The open reads N; a power loss can revert the name, leaving the
   predecessor log M as the head. N's bytes are durable (Linux) or ordered (macOS) ahead of the
   rename, by that rollover's own ordering point.

A chain of crashes compounds them: a handle that inherited g, wrote g+1 and crashed before its sync
leaves two unsynced edits. None of these generations was acknowledged by any `commit()`.

## D1. What an open does

Everything it does today except the sync: the scan with sizes, the replay, the segments by name and
size, counters above every name on disk, the appender at the replayed end (or a rollover owed), the
`.tmp` orphans unlinked, the open's retention round owed. It records that the inherited state is
owed a sync, and the segment id below which every segment was on disk at open.

An open at `to` below the newest generation still commits at open (manifest-log D6.8): that is an
explicit rollover commit, with its own syncs, and it leaves nothing owed.

## D2. What a handle may report before its first commit

The head the log holds whole, which may be any of the unacknowledged generations above. Searches,
`manifest()`, `applied()` and views report it. **A generation is durable once a commit on this
handle has returned.** A host must not record an open's generation as durable (its recorded newest
generation before an `open(..., to)`, a replica's position, durability-modes' recorded generation)
until then; a host that needs it durable commits.

## D3. The first commit

The commit's two paths (manifest-log D4, D5), with the inherited state:

| Path | Today | After an open, while owed |
|---|---|---|
| Append | `order_files_and_directory(new segments)` only when it wrote one; `pwrite` the edit; the log's `sync_data` | `order_files_and_directory(new segments + log)`, always; `pwrite`; the log's `sync_data`; owed cleared |
| Rollover | write `manifest-K.tmp`; `order_files_and_directory(new segments + tmp)`; rename; `sync_directory` | unchanged; owed cleared after `sync_directory` |

**Append.** The ordering point must include the log, not only the directory. Without it two records
are in flight at once, the inherited g and the new g+1, and a power loss can keep g+1's sectors and
lose g's. The replay then finds a whole record after a torn one, which manifest-log D6.4 reads as
damage: `corrupt_index`, not a lost tail. Ordering the log first restores D4's premise that one
record is in flight. The directory in the same ordering point covers the inherited rename, which a
commit that writes no file would otherwise never order (case 10). The log's own sync after the write
makes all of it durable before `commit()` returns:

- Linux: the ordering point is `fsync(log)`, `fsync(dir)`, already durable; the edit's `fdatasync`
  follows.
- macOS: `fsync(log)`, `fsync(dir)`, one `F_BARRIERFSYNC` (no cache flush), then the edit's
  `F_FULLFSYNC`, which flushes the drive with everything staged ahead of it. One device flush, as in
  any commit.

**Rollover.** The new log never appends to the inherited one. Its snapshot holds the whole current
state, so the inherited tail's durability stops mattering once the new name is durable; its ordering
point already syncs the directory (the inherited rename with it) ahead of its own rename, and its
`sync_directory` makes both durable.

A failure keeps today's handling: a failed ordering point or sync fences the writer; ENOSPC on the
edit's write truncates back and leaves the sync owed for the next commit.

## D4. Directory syncs and removals before the first commit

Retention removes files no kept generation names, and it judges that against the observed head. If
that head is not durable, a power loss returns to an older head that may name what was removed: an
inherited edit that dropped segment S, then `reclaim()` unlinking S, then a power loss reverting the
edit, leaves a head naming a missing file. So while the sync is owed, `retain()` queues a `manifest-*`
file, or a segment whose id is below the open's mark, only after the **inherited sync**:
`sync_files_and_directory({log})`, opening the log by name when a demotion closed its descriptor.
Files this handle created (flushed or merged segments, spills, scratch) were never named by a log
and need no sync.

The inherited sync also stands in for **every directory sync while it is owed**: the drain's after
the open unlinked a `.tmp` orphan, a failed write's or flush's cleanup, a rollover's. On Linux a
directory `fsync` reports a writeback error once per descriptor, and some of those callers swallow
their error (the drain returns false; a failed flush's cleanup ignores it), so a plain directory sync
there could take the inherited rename's error and leave the first commit's `fsync(dir)` to succeed on
a name a power loss reverts. As the inherited sync, its failure fences the handle; its success makes
the log and the directory durable and clears the owed state. A merge thread's cleanup sync goes to
the job's error, which fences at publication, so it never hides an error either.

A failed inherited sync fences, as a commit's does: after an `fsync` error, a retry proves nothing.

The removal gate's sync runs only when a handle finds inherited garbage, typically a crash's leftovers or a
previous handle's unfinished reclaim queue; it then removes the garbage, so the next open finds none.
The open's unlinked `.tmp` orphans keep today's rule: never named by a head, durable at the next
directory sync, which the drain runs to return their space; while owed, that is the inherited sync.

## D5. Crash at each step

Crash model: faultfs, a power loss after any mutation (`power_off_after`, then `crash(seed)`).

| Crash after | After a reopen | Acknowledged lost? |
|---|---|---|
| the open, any reads, reclaims that removed nothing | whatever the earlier crash left: inherited generations kept or lost | none existed |
| the inherited sync (D4) | the observed head, durable | no |
| D4's unlinks | the observed head; the unlinked files gone or back | no |
| the first commit's ordering point | inherited state ordered (macOS) or durable (Linux), new segments too | no: not returned |
| its edit's `pwrite` | the head before it, or the edit whole, or a torn tail (a rollover owed); never the edit without the inherited bytes | no: not returned |
| its log sync (returned) | the new generation and everything under it | no |
| rollover: the rename | the old head or the new log | no: not returned |
| rollover: `sync_directory` (returned) | the new log | no |

Every generation a `commit()` acknowledged is durable before it returned, and no removal is durable
before the head it was judged against. That is A3's property: every acknowledged commit survives a
crash at each step, with each inherited state.

## D6. Accepted losses (D60c)

1. **A search may return an unacknowledged commit** a crashed writer left, which a later power loss
   erases. Before, the open made it durable; now nothing does until a commit.
2. **EIO surfaces late.** A writeback that failed earlier fences the first commit, or the first
   inherited sync before it (a removal of inherited files, a drain's or a cleanup's directory sync),
   instead of failing the open. Reads go on until then.
3. **The first write pays.** On macOS the first commit costs one device flush, as every commit does;
   on Linux it adds `fsync(log)` and `fsync(dir)` before its edit (one more pair than a later commit
   that writes no file).

## faultfs

faultfs's crash model mixes a node's durable bytes with its current ones even when the node was
staged and the staging did not survive, so a write after an ordering point can persist without the
staged bytes before it. No path wrote a staged file before; this one stages the log and appends to
it. The model changes to match the IO contract: when the staged bytes do not survive, a staged
node's bytes mix its durable and staged ones, never later writes (`faultfs_test`: 512 seeds, the
later write never survives without the staged tail; both outcomes of the tail occur). Two more
changes follow from an inherited ordering point meeting the first commit's:

- **Epochs.** Ordering points chain: a crash keeps the epochs before some point whole, that point's
  epoch in part, and nothing after it. With one mark, the first commit's ordering point re-staged
  over a crashed rollover's, so its rename could survive without the bytes ordered ahead of it
  (case 10's state read as "no whole snapshot"). With one ordering point the model draws the same
  outcomes from the same seeds as before.
- **A failed sync keeps what was ordered.** It drops the bytes no ordering point staged; before, it
  also dropped the staged ones, again letting a later rename survive without them.
- **A power-off sync does nothing.** `power_off_after` fails every later operation before its
  effect, and the crash decides what storage keeps; a sync failing that way no longer drops the
  cache first, which hid the crash between an edit's write and its sync (the `after` mutant passed).

faultfs also gains a sync counter (file syncs, a staged file's `fsync`, directory syncs and ordering
points) for A2.

## Evidence

- A2: `index_test` counts faultfs syncs across an open, a reader, a read and `reclaim()` (which runs
  `retain(true)`, a freed view's pass) over a clean index and an unsynced tail, each with a `.tmp`
  orphan: zero. Over inherited garbage (a segment the unsynced head dropped; case 10's predecessor
  log): zero across the open and reads, then one inherited sync (two faultfs syncs: the log's and the
  directory's) at the first `reclaim()`, and none at the next. Mutant: the open's sync kept.
- A3: cases 9 (a 9000-byte tail edit, across sector boundaries) and 10, with the first commit
  appending no file, appending a segment, and rolling over; `power_off_after` at every mutation of
  that commit, 128 seeds each (a torn tail under a whole edit shows at about 1 seed in 16): no reopen
  fails, and an acknowledged commit survives with the inherited state. An unsynced head that dropped
  a segment, a power loss at every mutation of `reclaim()`, 128 seeds: every reopen reads. The
  removal gate's sync failing fences and queues nothing (review F4); the drain's directory sync after
  a `.tmp` orphan, and a failed flush's cleanup, under a directory sync failure, leave the next commit
  fenced (review F1). Mutants (`bench/open_sync/first_commit_mutant.sh`): `drop`, the inherited sync
  dropped; `after`, the inherited sync after the edit's sync, so the edit is synced with the tail
  unordered; `gate`, the removal gate dropped; `dir`, directory syncs outside a commit left plain.
- A4: `bench/open_sync/loaded.sh` open latency before and after, and the first commit after an open
  (`idle_index open ... commit`), one exclusive job each, at most 10 minutes (D59).

## Review fixes (design review at strata:633e39b0)

| Finding | Fix |
|---|---|
| F1, a swallowed directory sync takes the inherited writeback error | D4: every directory sync while owed is the inherited sync, fencing on failure |
| F2, too few seeds; tail edit in one sector | A3: 128 seeds per crash point, a 9000-byte tail edit |
| F3, one ordering epoch in faultfs | faultfs models a chain of ordering epochs |
| F4, no test of the gate's EIO | A3: the gate's sync fault fences and queues nothing |
| F5, does the gate clear owed; the log after a demotion | D4: it clears it; it opens the log by name |
| F6, A2 only over clean states | A2 adds inherited garbage; A4 reports how often opens meet it |
| F7, Xapiand's `committed_revision` from a reopen | xapiand-strata-bump's |

## Left open

Xapiand stores `manifest().revision` after a reopen as its committed revision
(`src/engine/strata/shard.cc`); whether that treats an unacknowledged revision as durable is
xapiand-strata-bump's question (review F7). Linux timing is not measured.
