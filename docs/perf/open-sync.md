# The open's syncs

`basic_index::open` runs no `fsync` or `F_FULLFSYNC` (D60c, `docs/design/sync-at-first-commit.md`).
What a crashed writer left unsynced, a tail edit (T4 case 9) or a rollover's rename (case 10), is
made durable by the handle's first commit: it puts the log and the directory in its ordering point
ahead of its own edit, and its log sync makes all of it durable before it returns. Until then every
directory sync is that sync, and no file an inherited generation names is removed before it.

The first commit's cost after an open, and the open's against its one-flush predecessor, are under
"Sync at first commit" below. The sections between measure that predecessor (strata:c35ce7e0): the
open ran one `sync_files_and_directory({log})`, one device flush on macOS where it had issued two;
`git show 8499e781:bench/open_sync/mutant.sh` rebuilds its mutants.

## One device flush instead of two

On macOS the open issued `fsync` and `F_FULLFSYNC` on the directory, then `F_FULLFSYNC` on the
log: two device flushes. `IO::sync_files_and_directory` stages the log and the directory with
`fsync` and flushes the device once. On Linux it issues `fsync` on the log (after a
`sync_file_range` writeback hint) where the open issued `fdatasync`, then the directory's `fsync`.

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
