# Read-only open: a search opens with no lock and no flush

Status: design (quest read-only-open, ruling D60 of the Xapiand campaign). An index opened for
search takes no lock and syncs nothing; the first call that writes upgrades it to the owner's
open: the lock, then the one `sync_files_and_directory` that `basic_index::open` runs today.

## Why

`basic_index::open` makes the manifest log and the directory durable before it reports the head
(manifest-log D6.6, `docs/perf/open-sync.md`). After open-one-flush that is one device flush per
open: about 0.1 ms on a quiet device, 23 ms under a writer that keeps the device cache dirty (45 ms
before). A host serving millions of indexes opens most of them to search and never writes them
before they close again, and pays that flush per open for a durability promise only a writer needs.

## The contract

`options::for_search` (default false). `open(io, opts)` with it set returns a **search handle**;
`search_only()` is true until the handle upgrades, then false for good. A plain open is unchanged.
`create` ignores the option: a create writes. An open at `to` with it set throws
`std::invalid_argument`: a fallback commits at open (manifest-log D6.8).

## D1. What a search open does

It runs the open's recovery read as it is: the directory scan with each file's size, the newest
log's replay, the segments by name and size (lazy unless `eager_segments`), the counters above every
name on disk, and the rollover a dirty tail owes. It checks the directory's trust (a stat, no lock).
It does not:

- take the directory lock, or classify the filesystem for it (both move to the upgrade);
- open the log for appending or sync the log or the directory;
- unlink `.tmp` orphans, or owe the open's retention round (`retain_owed_` stays false).

Nothing it does mutates the directory: on faultfs, `mutations()` does not move across the open, a
`reader()`, reads, `reclaim()`, `demote()` and `close()` (A2).

## D2. What a search handle may observe

The head the log holds whole in the page cache. After a process crash that can be an edit whose
`pwrite` returned and whose `sync_data` never ran (T4 case 9's shape), or a rollover's log whose
rename never reached a directory sync (case 10's). A power loss can take either back. This is
ML-R15's option 2, taken for search handles only: **a search handle reports nothing durable**.
`manifest()`, `applied()` and its readers' generations are the observed head; they are the durable
head only once `search_only()` is false. A host must not record a search handle's generation as
durable: not as its recorded newest generation (phase-3), not as durability-modes' recorded
generation (DM-R43), not to a replica. A search that read such a generation read data no commit
acknowledged: the commit that wrote it never returned.

## D3. When it upgrades

At the first call that writes the directory or promises durability, before that call writes or
syncs anything:

| Upgrades | Does not upgrade |
|---|---|
| `refresh()` with records buffered (and `apply()`'s flush, which is a `refresh()`) | `apply()`: it buffers in memory, and under `defer_io` it may make no device call |
| `commit()`, so `checkpoint()` and `release()` | `reader()`, `publish_through()` with nothing buffered |
| `merge()`, `force_merge()`, `force_merge_deletes()` | `maybe_merge()`: skipped, the policy's merges wait for a writer |
| `install()` | `reclaim()`, `reclaim_for_space()`: free released views, remove nothing |
| | `demote()`, `promote()`, `close_ready()`, `heal()`, `close()`, `check()` |

The upgrade runs at the call's top, outside `guarded()`, so a refused lock throws without fencing.
An `apply()` whose flush runs a refused upgrade throws before it evaluates its transaction: nothing
of it is applied, its position does not move, and the records buffered before it stay buffered.
Retention is the owner's: `retain()` queues nothing on a search handle, so the chores' and
`reclaim()`'s rounds stay empty until the upgrade owes the open's round.

## D4. The upgrade's order

1. **The lock.** `lock_directory`, as the plain open takes it: the filesystem's class, then the
   directory's lock (D2 of phase-3). Busy, or any refusal, throws as the open would and changes
   nothing: the handle stays a search handle, its reads go on, and the next write tries again.
2. **The check.** The directory scanned again, every strata name with its size. It must equal the
   record the open made (`files_`): nothing writes the directory before the upgrade, so any
   difference is another writer's commit, flush, rollover or reclaim since the open. A difference
   releases the lock and fences the handle (`index_fenced`, "the index changed since it was opened
   for search"); the host reopens. A log only grows or rolls to a new name, and a segment or
   temporary is a new name, so names and sizes see every change a writer makes.
3. **The syncs.** The log opened again and `io_->sync_files_and_directory({log})`: one device flush
   on macOS, the same call and the same two properties as the plain open (`open-sync.md`): the
   observed head becomes durable, and a rollover's rename is durable before any commit is
   acknowledged on it. A failed sync fences, as it fails the plain open.
4. **The appender.** The log open for appending at the replayed end, unless a rollover is owed.
5. **The open's leftovers.** `.tmp` orphans unlinked (durable at the next directory sync, D8 of
   manifest-log), and the open's retention round owed. `search_only()` becomes false.

The lock comes first so that nothing changes between the check and the sync; the check comes before
the sync so that a stale handle syncs nothing and writes nothing; the sync comes before the call's
own work so that the first commit builds on a durable head: a commit that writes no segment orders
no directory (case 10), and an upgrade at a `refresh()` reports its head durable with no commit at
all (case 9). From step 3 on the handle is the plain open's, and every later call is today's code.

## D5. A crash at any step

| Crash after | On disk | After a reopen |
|---|---|---|
| the search open, or step 1 or 2 | nothing changed (locks are not on disk) | what the crash left, as today |
| step 3 | the observed head durable | the plain open's state |
| step 5 | orphans unlinked, the unlink not yet durable | the orphans back or gone; the next open unlinks them |
| the first commit's files or edit | today's commit protocol | the head before it, or after it once acknowledged |

No commit of this handle is acknowledged before its `commit()` returns, after step 3. Every commit
acknowledged earlier was made durable by its own commit's syncs; a generation that only the page
cache holds was never acknowledged. So every acknowledged commit survives a crash at any point, as
long as the upgrade syncs before the first commit returns (A3: faultfs's `power_off_after` at every
mutation of the upgrade and the first commit, then `crash(seed)`).

## D6. What concurrent readers see

**Views of the same handle.** The upgrade runs under the writer lease on the owner's thread, like
every writer call. It publishes nothing and replaces no segment record: readers keep the view they
hold and see the same head before and after. Their lazily mapped segments map read-only either way.

**Another handle on the directory.** A search handle excludes no writer, here or in another process.
A writer that opens after it holds the lock, commits, and may reclaim files the search handle's
head names. The search handle keeps serving the head it opened: a mapped segment survives the
unlink; one it never mapped reads `ENOENT`, which `read_checked` rethrows as the host's error, never
as `corrupt_index`, and the host reopens. Its upgrade then fails busy while that writer holds the
lock, or fences at step 2 once the writer has gone. Two writers never write at once: every write to
the directory still happens under the lock. Xapiand's catalog holds one handle per index, so on one
host the case needs a second process on the same directory.

## D7. Cost

A search open makes no device flush and no lock call; the faultfs count is zero mutations. Under a
storage budget it holds its files' bytes and no commit allowance (the edit's bound, and the
snapshot a dirty tail owes): the allowance is held from its first applied transaction on. The
upgrade pays one lock, one directory scan with a size per file (the open's own scan again), and the
flush the plain open pays today, once, on the indexes that write. A4 times opens under the
catalog_tiers load before and after (D59: one exclusive job, at most 10 minutes).

## Evidence

- A2: `index_test` counts faultfs mutations across a search open and its reads, on a clean index and
  on case 9's and case 10's states with a `.tmp` orphan, acquires the directory lock while the
  handle is open, and counts the lock calls the open makes (zero).
- A3: case 9 with the upgrade at a `refresh()` (the reported head survives every seed), case 10 with
  the upgrade at a commit that writes no file (the acknowledged commit survives every seed), a
  power-off at every mutation of the upgrade and its first commit, the busy and stale-handle
  refusals. Mutants, each failing its named check: the upgrade syncs the log only, the directory
  only, or skips the check of D4 step 2; the search open takes the lock.

## Pending: another handle beside a search handle (review F1, F2)

The design review (`read-only-open-design.md` in the Xapiand campaign) found D6's account of a
writer on another handle false: the scan-then-read window can report a false `corrupt_index` at
open, a missing file marks a lazily mapped segment failed for good, and reclaim's truncation of a
file another handle maps raises SIGBUS. D4 step 2 also misses a failed append truncated back and
refilled to the same size (F2). The owner decides between (a) a shared lock on search handles
(recommended by the lead: writers stay busy as today, the upgrade converts it) and (b) a host
contract that nothing else writes a directory while a search handle is open. D6 and D4 step 2 are
revised once that ruling lands.

## Left open

Xapiand's catalog setting `for_search` in its `index_options`, and what it records from a handle,
is the Xapiand side's (D2's host rule). Linux's syncs are not timed here.
