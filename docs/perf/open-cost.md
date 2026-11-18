> Status: PARKED (2026-10-20) the one-barrier create is a durability change and folds into the turn log (exploration log).

# Opening many indexes: where the 85 ms goes

Question: why does the many-index row's open phase cost about 85 ms per index (RocksDB 22 ms),
and what removes it. Main `8af7d92`, development Mac (APFS), one thread, `quiet run`, light.

## Finding

The "open" phase of the many row is `create()` (a first commit), not a reopen: reopening 1,000
indexes takes about 0.2 ms each. A create is 4 `F_FULLFSYNC` (about 21 ms each on this Mac):
meta file, directory, manifest tmp, directory. Everything else is noise: with `F_FULLFSYNC`
replaced by `fsync` (an interposer, measurement only) the same 100 creates take 0.74 ms each,
56.7 ms CPU in total. The cost is 99% device flushes.

## Experiment 1: manifest tmp written before, and synced with, the data files

`commit_files` builds the manifest without mutating state, writes the tmp beside the data files and
syncs both in one `sync_files` (macOS: one device flush for the batch). The pre-rename directory
sync stays when new data files exist and is skipped when a commit has none; the post-rename
directory sync stays. The rename still follows every sync. Create goes 4 to 3 flushes; a commit
with data 4 to 3; an empty commit 3 to 2.

20 creates, open phase seconds, interleaved, 5 rounds:

| variant | runs | spread |
|---|---|---|
| base | 1.617 1.651 1.686 1.694 1.815 | 12% |
| base (A/A) | 1.624 1.645 1.647 1.652 1.676 | 3% |
| exp 1 | 1.223 1.233 1.241 1.260 1.262 | 3% |

Median 1.68 s to 1.24 s: 1.35x faster, 26% less open time (82 ms to 62 ms per index), as
predicted by 3/4 of the flushes. `index_test` (with its crash points) passes. Failure paths need
review: a tmp ENOSPC now happens before the data files sync (they stay unsynced and are written
again by the next commit).

## Not done: group create

The remaining three flushes per index are required one by one, but not per index: F_FULLFSYNC
flushes the whole device, so N creates can share three (stage every file with `fsync`, one
flush, directory fsyncs, one flush, renames, directory fsyncs, one flush). The floor
measured above (0.74 ms per index) is what that approaches. It needs an API: commit split into
stage and finish phases, and a device-level sync across PosixIO instances, in the host
(create_many) and the completion library. Linux (fdatasync per file) gains from exp 1 only
through fewer fsyncs; not measured here.
