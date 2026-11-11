# The open's syncs

`basic_index::open` makes the manifest log and the directory durable before it reports the head
(manifest-log D6.6, review condition C4). Each sync buys one crash-safety property:

1. The log's sync. A process killed after an edit's `pwrite` and before its `sync_data` leaves the
   edit in the page cache; the reopen in the same boot reads it whole and reports its generation,
   and without the sync a power loss takes that generation back after readers, the host's recorded
   newest generation, or durability-modes' recorded generation saw it (ML-R15). The sync also
   reports a writeback that failed earlier, failing the open. T4 case 9 (`test/index_test.cc`).
2. The directory's sync. A rollover stopped between its rename and its directory sync (a fault
   there fences; a process kill leaves the same state) leaves a head whose name a power loss can
   revert; a commit after the reopen that writes no file orders nothing, so it would be
   acknowledged on a name that can vanish. T4 case 10.

## Neither can be skipped by what recovery sees

- `open` has no read-only mode: it always takes the owner lock and opens the log for appending. A
  reopen that commits nothing is T4 case 9's shape, and its reported generation needs the log sync.
  `index_detail::read_state` is the read-only path, and it syncs nothing and reports nothing durable.
- Both hazards read clean to the replay: the crashed process's edit and the renamed log are whole,
  so no torn tail, nothing past `to`, nothing to truncate. A mutant that syncs only when recovery owes
  a rollover (`rollover_owed_`) fails `index_test` (job below).
- Skipping the open's syncs is ML-R15 option 2 (report nothing durable until a barrier of this
  process covers the log, and state in Limits that a reopen after a process crash can serve a
  generation a power loss takes back). The review chose option 1; changing it is a contract change.

## One device flush instead of two

On macOS the open issued `fsync` and `F_FULLFSYNC` on the directory, then `F_FULLFSYNC` on the
log: two device flushes. `IO::sync_files_and_directory` stages the log and the directory with
`fsync` and flushes the device once; on Linux it issues the same `fsync`s as before. The order
between the two does not matter: both are durable before the open returns.

Harness: `bench/open_sync/build.sh` builds `idle_index` before and after and 1,000 indexes of 100
documents; `bench/open_sync/run.sh` opens all 1,000 in one process with each binary, in
interleaved pairs after one warm-up run each, under a strict hold.

## Evidence

| Job | Commit | What | Result |
|---|---|---|---|
| 261110-232637-c2f46f | strata:e08935a3 (mutant) | `index_test` with the open syncing only when `rollover_owed_` | fails, as expected: T4 case 9's check at `test/index_test.cc:133` fails in 31 of 64 seeds (the reopened generation lost to the power loss) |
| 261110-232635-d8a446 | strata:54f9e0af | strata suite, Release | 73 of 74 pass; `catalog_stress` failed once at `test/catalog_stress_test.cc:701` (`burst_closes`: queue sizes after a policy pass), a test that never touches the open's sync order; 40 earlier campaign runs of it passed |
| 261111-002353-8b99cc | strata:71750940 (base) | `catalog_stress` 25 times | 0 of 25 fail |
| 261111-002355-0e20f8 | strata:54f9e0af | `catalog_stress` 25 times | 0 of 25 fail |
| 261110-232638-e0f83a | strata:54f9e0af | builds `idle_index` before and after, and 1,000 indexes | passed |
| 261110-232645-bfc432 | strata:54f9e0af | strict, 1,000 opens before and after, interleaved pairs | below |

The `catalog_stress` failure is unattributed: 1 of 26 runs at the head (the one inside the `-j4`
suite), 0 of 25 at the base. `burst_closes` runs on faultfs, where the change only swaps the order
of one file sync and one directory sync inside the open; its check is on the catalog's queue sizes
after closes the pool's workers end together. A rare flake under suite contention fits, but
neither side reproduces it alone, so it is not shown pre-existing either.

## Timing (job 261110-232645-bfc432, macOS APFS, under the host's load)

Each run opens 1,000 indexes in one process; six interleaved pairs after a warm-up each.

| Pair | Before: total, p50, p99 | After: total, p50, p99 |
|---|---|---|
| 1 (before first) | 22.99 s, 20.95 ms, 57.8 ms | 6.77 s, 0.24 ms, 21.4 ms |
| 2 (after first) | 8.93 s, 0.22 ms, 42.2 ms | 20.97 s, 20.94 ms, 36.7 ms |
| 3 | 32.82 s, 40.16 ms, 63.1 ms | 21.38 s, 21.05 ms, 36.8 ms |
| 4 | 24.14 s, 20.87 ms, 57.0 ms | 6.68 s, 0.25 ms, 29.1 ms |
| 5 | 5.04 s, 0.25 ms, 41.7 ms | 21.33 s, 21.01 ms, 37.1 ms |
| 6 | 24.32 s, 21.02 ms, 58.1 ms | 8.39 s, 0.21 ms, 36.9 ms |

Runs are bimodal by the device's state, not by the binary: a fast mode with a 0.21 to 0.25 ms
median on both sides, and a slow mode where the median open waits about 21 ms, before and after
alike. The change shows in the tail: p99 57 to 63 ms before against 36.7 to 37.1 ms after in the
slow mode (about 1.6× lower), max 67 to 77 ms against 38 to 41 ms. The six pairs total 118.2 s
before against 85.5 s after (28% less), but 4 of 6 pairs favor after and the mode noise dominates
that total. Honest magnitude: one device flush less per open cuts the tail; the median open under
load still pays one flush, about 21 ms. Linux is not measured.
