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

## D4. Removals wait for the inherited sync

Retention removes files no kept generation names, and it judges that against the observed head. If
that head is not durable, a power loss returns to an older head that may name what was removed: an
inherited edit that dropped segment S, then `reclaim()` unlinking S, then a power loss reverting the
edit, leaves a head naming a missing file. So while the sync is owed, `retain()` queues a `manifest-*`
file, or a segment whose id is below the open's mark, only after
`sync_files_and_directory({log})`. Files this handle created (flushed or merged segments, spills,
scratch) were never named by a log and need no sync. A failed sync fences, as a commit's does: after
an `fsync` error, a retry cannot be trusted.

This sync runs only when a handle finds inherited garbage, typically a crash's leftovers or a
previous handle's unfinished reclaim queue; it then removes the garbage, so the next open finds none.
The open's unlinked `.tmp` orphans keep today's rule: never named by a head, durable at the next
directory sync, which the drain runs to return their space.

## D5. Crash at each step

Crash model: faultfs, a power loss after any mutation (`power_off_after`, then `crash(seed)`).

| Crash after | After a reopen | Acknowledged lost? |
|---|---|---|
| the open, any reads, reclaims that removed nothing | whatever the earlier crash left: inherited generations kept or lost | none existed |
| D4's sync | the observed head, durable | no |
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
   removal of inherited files, instead of failing the open. Reads go on until then.
3. **The first write pays.** On macOS the first commit costs one device flush, as every commit does;
   on Linux it adds `fsync(log)` and `fsync(dir)` before its edit (one more pair than a later commit
   that writes no file).

## faultfs

faultfs's crash model mixes a node's durable bytes with its current ones even when the node was
staged and the staging did not survive, so a write after an ordering point can persist without the
staged bytes before it. No path wrote a staged file before; this one stages the log and appends to
it. The model changes to match the IO contract: when the staged bytes do not survive, a staged
node's bytes mix its durable and staged ones, never later writes. faultfs also gains a sync counter
(`sync`, `sync_directory`, ordering points) for A2.

## Evidence

- A2: `index_test` counts faultfs syncs across an open of a clean index, of case 9's and case 10's
  states, and of one with a `.tmp` orphan, and across reads: zero. Mutant: the open's sync kept.
- A3: cases 9 and 10 with the first commit appending with no file, appending a segment, and rolling
  over, and with `power_off_after` at every mutation of that commit; a reopen after the
  acknowledgement holds the commit and the inherited state, and no reopen fails. Case 9 with an
  inherited edit dropping a segment, then `reclaim()`, then a crash at every mutation: every reopen
  succeeds. Mutants: the inherited sync dropped (the log and directory left out of the ordering
  point), the inherited sync after the acknowledgement (deferred to the next commit), and the
  removal gate dropped.
- A4: `bench/open_sync/loaded.sh` open latency before and after, and the first commit after an open
  (`idle_index open ... commit`), one exclusive job each, at most 10 minutes (D59).

## Left open

Xapiand records no open's generation as durable today as far as this quest reads; its adoption is
xapiand-strata-bump's. Linux timing is not measured. manifest-log D6.6 and the faultfs test file sit
outside this quest's files; their text and a direct faultfs model test are the lead's to place.
