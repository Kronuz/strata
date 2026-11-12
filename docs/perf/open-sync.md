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

- A plain `open` takes the owner lock and opens the log for appending. A reopen that commits
  nothing is T4 case 9's shape, and its reported generation needs the log sync. An open with
  `options::for_search` takes neither and syncs nothing, reports nothing durable, and runs these
  syncs at its first write (`docs/design/read-only-open.md`).
- Both hazards read clean to the replay: the crashed process's edit and the renamed log are whole,
  so no torn tail, nothing past `to`, nothing to truncate. A mutant that syncs only when recovery owes
  a rollover (`rollover_owed_`) fails `index_test` (job below).
- Skipping the open's syncs is ML-R15 option 2 (report nothing durable until a barrier of this
  process covers the log, and state in Limits that a reopen after a process crash can serve a
  generation a power loss takes back). The review chose option 1 for the plain open; D60 takes
  option 2 for search handles only.

## One device flush instead of two

On macOS the open issued `fsync` and `F_FULLFSYNC` on the directory, then `F_FULLFSYNC` on the
log: two device flushes. `IO::sync_files_and_directory` stages the log and the directory with
`fsync` and flushes the device once. On Linux it issues `fsync` on the log (after a
`sync_file_range` writeback hint) where the open issued `fdatasync`, then the directory's `fsync`.
The order between the two does not matter: both are durable before the open returns.

Harness (`bench/open_sync/`): `build.sh` builds `idle_index` at main and at the change, and 1,000
indexes of 100 documents; `run.sh` opens all 1,000 in one process with each binary, in interleaved
pairs after one warm-up run each, under a strict hold; `loaded.sh` runs `run.sh` beside a writer
that keeps the device's cache dirty (4 MiB written and `fsync`'d in a loop, no `F_FULLFSYNC`), as
other indexes' commits do on a host serving many.

## Evidence (strata:c35ce7e0, main strata:d8ae10b8)

| Job | Commit | What | Result |
|---|---|---|---|
| 261112-022003-37891b | c35ce7e0 | `index_test` | passes, T4 cases 9 and 10 included |
| 261112-022005-2de6e1 | 65fd2afd (mutant) | `index_test` with the open syncing only when `rollover_owed_` | fails, as expected: case 9's check at `test/index_test.cc:137` (the reopened generation lost to the power loss) |
| 261112-022007-45e212 | c35ce7e0 | strata suite, Release | passes |
| 261112-022029-add889 | c35ce7e0 | strict, quiet device, 6 pairs | no regression: no difference (below) |
| 261112-023556-0291d4 | c35ce7e0 | strict, concurrent fsync writer (the many-indexes host), 6 pairs | the median open halves, p99 lower in 5 of 6 pairs (below) |
| 261112-030638-fcc49d | 69b76e9c | `loaded.sh`, binaries from build 261112-025326-d8e7e1 | the median open halves, p99 lower in 6 of 6 pairs (below) |
| 261112-025328-896ded | f8747be0 | the same, earlier | agrees: p50 45.3 to 45.8 ms against 23.0 to 23.1 ms, p99 lower in 6 of 6 pairs, 277.9 s against 138.4 s |
| 261112-025813-0362d3 | 8499e781 | `mutant.sh M1`: the open syncs the log only | fails, as expected: case 10's check at `test/index_test.cc:182` |
| 261112-025815-cc2c50 | 8499e781 | `mutant.sh M2`: the open syncs the directory only | fails, as expected: case 9's check at `test/index_test.cc:137` |

## Timing (macOS APFS)

Each run opens 1,000 indexes in one process; six interleaved pairs after a warm-up each.

On a quiet device a full flush finds nothing to write, and the two sides match: 0.13 to 0.18 s per
1,000 opens, p50 102 to 117 µs, p99 187 to 300 µs, lower after in 3 of 6 pairs; both sides show one
stall of about 21 ms per run.

With the device's cache dirty, each `F_FULLFSYNC` waits for the drive, and the open pays it once
instead of twice (job 261112-030638-fcc49d):

| Pair | Before: total, p50, p99 | After: total, p50, p99 |
|---|---|---|
| 1 (before first) | 45.97 s, 45.18 ms, 66.1 ms | 23.08 s, 23.11 ms, 27.2 ms |
| 2 (after first) | 45.61 s, 45.14 ms, 60.5 ms | 22.99 s, 22.91 ms, 27.7 ms |
| 3 | 45.71 s, 45.17 ms, 63.0 ms | 22.92 s, 22.89 ms, 28.0 ms |
| 4 | 45.77 s, 45.20 ms, 62.1 ms | 23.07 s, 22.96 ms, 33.2 ms |
| 5 | 45.95 s, 45.15 ms, 68.4 ms | 23.44 s, 23.06 ms, 39.3 ms |
| 6 | 45.49 s, 45.15 ms, 51.1 ms | 23.00 s, 22.96 ms, 37.2 ms |

The median open drops from 45.1 to 45.2 ms to 22.9 to 23.1 ms (about 2.0× faster), p99 is lower
after in all six pairs (27 to 39 ms against 51 to 68 ms), and the six pairs total 274.5 s before
against 138.5 s after (50% less). The earlier probe (261112-023556-0291d4, the same harness)
agrees: p50 45.1 to 46.0 ms against 23.0 to 25.9 ms, p99 lower in 5 of 6 pairs. Linux is not
measured; there the log's `fdatasync` becomes `fsync`, and the directory's `fsync` stays.
