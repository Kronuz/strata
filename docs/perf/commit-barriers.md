# Commit barriers: one device flush for a commit's data files

A commit makes its new data files durable (flushed segments, live-docs generations, the
metadata file) before it writes the manifest. Each file was synced on its own. On macOS
`File::sync` is `F_FULLFSYNC`, a flush of the drive's whole cache, so a commit's cost grew
with its file count: under the comparative soak's churn (replaces and deletes spread
over every segment), each segment gets a live-docs file per commit, and a commit with 8
segments synced 9 files.

## Breakdown before the change

`.scratch/commitcost/commit_cost.cc` replays the soak's op stream (bench/soak, 3,000 live
documents, 15 cycles) with flush, merge, and commit timed apart and every sync timed
through a wrapping IO. On macOS x86_64 APFS, a commit with 8 segments: 9 file syncs
(301 ms) and 3 directory barriers (129 ms) of a 488 ms commit phase. Merges, when they
run, add 266–326 ms; flushes 19–46 ms.

## The change

- `completion` `64d958b`: `IO::sync_files`, one call for many files. `PosixIO` batches its
  own: on macOS `fsync` each, then one `F_FULLFSYNC`; on Linux writeback starts on every
  file (`sync_file_range`) before the first `fsync` waits; FreeBSD keeps one `fsync` per
  file (its best batched form, `fdatasync` each, gained 1.55× in the probe and is not
  adopted). The finding and its numbers are shared with every consumer in completion's
  `docs/PLATFORM_FINDINGS.md`.
- `commit_files` writes every new data file, syncs them with one `sync_files`, and only
  then records them (live-docs references, the metadata reference, the dirty flags), so
  a failed write or sync leaves the next commit to write them again under new names.
  `index_test`: ENOSPC on the second of three live-docs files, a successful retry, then
  power loss under 20 seeds reopens to the same contents; with the flags set before the
  sync, the reopen fails ("live docs differ from the manifest").

## Results

The probe on the soak, before and after interleaved three times (A, B, A, B, A, B), 48
commits each. Commit phase is `commit()` alone: data files, manifest, retention.

| Platform | Commit phase median | p90 | File-sync time median | Whole commit median |
|---|---:|---:|---:|---:|
| macOS 15.8 x86_64 APFS | 186.8 → 106.0 ms | 274.8 → 119.9 ms | 117.6 → 42.7 ms | 225.9 → 117.4 ms |
| Linux 6.8 x86_64 ext4 (gate VM) | 24.8 → 16.2 ms | 31.4 → 22.1 ms | 19.9 → 10.1 ms | 38.7 → 28.1 ms |
| FreeBSD 14.4 x86_64 UFS | unchanged (one `fsync` per file) | | | |

The macOS run shares the host with compiles, so its absolute numbers are noisy; the
interleaving keeps the comparison fair.

`.scratch/commitcost/sync_probe.cc`, 9 new files of 200 KB made durable plus a directory
barrier, median of 15 rounds: APFS x86_64 214 → 46 ms; APFS arm64 (M1 Pro) 45 → 12 ms;
ext4 31.9 → 9.5 ms; UFS 13.4 ms (11.1 ms with `aio_fsync`, 8.6 ms with `fdatasync`,
neither adopted). The M1's ssh closed before the soak comparison could run there.

## Next

A macOS commit still pays about four device flushes: the data batch, the directory
barrier (`fsync` plus `F_FULLFSYNC`), the manifest temporary, and the final directory
barrier. Folding the first directory barrier into the batch and an ordering-only barrier
(`F_BARRIERFSYNC`) for the manifest temporary are the candidates; the probe measured the
barrier form at 10.7 ms for the same 9 files.
