# Read-only open: a search opens with a shared lock and no flush

Status: design (quest read-only-open, rulings D60 and D60a of the Xapiand campaign). An index
opened for search takes a shared lock on its directory and syncs nothing; the first call that
writes upgrades it to the owner's open: the exclusive lock, then the one `sync_files_and_directory`
that `basic_index::open` runs today.

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
name on disk, and the rollover a dirty tail owes. It checks the directory's trust (a stat).
It does not:

- take the exclusive lock: it takes the directory's **shared** lock first, before the scan (D60a),
  so no writer runs while it reads and while it holds the lock; busy while a writer holds the
  exclusive lock, as today's second open is;
- open the log for appending or sync the log or the directory;
- unlink `.tmp` orphans, or owe the open's retention round (`retain_owed_` stays false).

Nothing it does mutates the directory: on faultfs, `mutations()` does not move across the open, a
`reader()`, reads, `reclaim()`, `demote()` and `close()`, and the only lock call is the shared one
(A2). A plain open beside a search handle fails busy, as beside a writer today.

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

1. **The lock.** The shared lock released, then `lock_directory` as the plain open takes it: the
   filesystem's class, then the exclusive lock (D2 of phase-3), non-blocking. completion holds one
   directory lock per IO, so the upgrade is these two explicit steps and never a `flock`
   conversion (which drops the shared lock on Linux and, on macOS, keeps it when a non-blocking
   conversion fails: `directory-locks.md` in completion); another holder may lock between them.
   Busy, or any refusal, takes the shared lock again and throws as the open would: the handle stays
   a search handle and its reads go on. When search handles upgrade at once, at most one wins and
   all can lose: each loser gets the busy error and its shared lock back, unless a winner holds
   the exclusive lock by then; such a loser holds no lock until its next upgrade, D6's hazards
   apply to it meanwhile, and that upgrade's check fences it if the winner changed anything.
2. **The check.** The directory scanned again, every strata name with its size. It must equal the
   record the open made (`files_`). Under the shared lock no writer ran, so the open's scan saw a
   quiescent directory; a difference now is a writer that ran in step 1's window (or while a loser
   held no lock). A difference releases the lock and fences the handle (`index_fenced`, "the index
   changed since it was opened for search"); the host reopens. A writer's successful commit grows
   the log or rolls it to a new name, its flush or a temporary adds a name, and a failed append is
   truncated back to the bytes it found, so names and sizes see every change that matters.
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

**Another handle on the directory.** The shared lock excludes every writer while a search handle
holds it (D60a): a plain open fails busy, so no commit lands between the open's scan and its log
read, no reclaim removes or truncates a file a lazy map has yet to open, and no truncation reaches
a segment the handle maps. Search handles share the lock with each other. The design review's F1
and F2 close here: F1's three outcomes need a writer beside the search handle, and F2 needs an
append caught half-written by the open's scan.

The exposure that remains is a handle holding no lock: in step 1's window, or a loser that has not
got its shared lock back. A writer then may commit and reclaim. A lazily mapped segment whose file
it removed reads as `corrupt_segment` and is marked failed, a truncated mapped file can raise
SIGBUS, and the host reopens on the busy error rather than reading on. Xapiand's catalog holds one
handle per index (`src/database/pool.cc:583`, a reopen only after `finish_close`), so on one host
this needs a second process on the same directory.

**The catalog's two keys on one directory.** CT-R15 (`catalog.hh:232`) finds two keys resolving to
one directory by the lock's busy error at the second open. Search handles share the lock, so that
error comes at the first write on either key, not at open.

## D7. Cost

A search open makes no device flush and one shared `flock` (1.7 µs with its release, job
261112-035141-dbef9e: 1.5% of a quiet device's flush, under 0.01% of a loaded one's); the faultfs
count is zero mutations. Under a
storage budget it holds its files' bytes and no commit allowance (the edit's bound, and the
snapshot a dirty tail owes): the allowance is held from its first applied transaction on. The
upgrade pays the lock's conversion, one directory scan with a size per file (the open's own scan again), and the
flush the plain open pays today, once, on the indexes that write. A4 times opens under the
catalog_tiers load before and after (D59: one exclusive job, at most 10 minutes).

## Evidence

- A2: `index_test` counts faultfs mutations across a search open and its reads, on a clean index and
  on case 9's and case 10's states with a `.tmp` orphan; counts its lock calls (one, shared); a
  plain open beside it fails busy, and a second search open beside it succeeds.
- A3: case 9 with the upgrade at a `refresh()` (the reported head survives every seed), case 10 with
  the upgrade at a commit that writes no file (the acknowledged commit survives every seed), a
  power-off at every mutation of the upgrade and its first commit, the busy refusal (the loser of
  two upgrades keeps reading), and the stale-handle fence. Mutants, each failing its named check: the upgrade syncs the log only, the directory
  only, or skips the check of D4 step 2; the search open takes the lock.

## The lock

completion's `IO::acquire_directory_reader()` (completion:504f409): `LOCK_SH | LOCK_NB` on the IO's
directory, busy and `not_supported` as the exclusive lock's, one directory lock per IO. faultfs
models it: any number of shared holders, none beside the exclusive one.

## Left open

Xapiand's catalog setting `for_search` in its `index_options`, and what it records from a handle,
is the Xapiand side's (D2's host rule). Linux's syncs are not timed here.
