# reclaim-backlog: merged-away files wait for the host's commits

- Task (the lead's, 2026-10-21): the explorers (`explore/bigval`, `explore/bigdisk`) found 44 files
  / 600 MB (64 KiB overwrites) and 193 to 210 files (1M fills at 10x RAM) waiting in the reclaim
  queue after settle: `commit()` reclaims `reclaim_slice` (64 MiB) per call, and with durability
  none no commit happens during a run. Draining took disk from 660-790 to 79-88 MB and from
  184-208 to 72 MB, level with RocksDB, data identical.
- Branch / worktree: `phase4/reclaim-backlog` from main `d1bbc42`, `strata-wt/reclaim-backlog`. It
  deletes files, so it takes one code review before landing.

## State

Engine change and tests in; proofs and the explorers' rows in progress (see Done).

## Done

- Engine (`include/strata/core/index.hh`): `install_merge` adds the bytes it obsoleted (sources
  removed from the list, or a discarded output) to `obsoleted_`; `publish_merges` then runs
  `retain()` and `reclaim(owed)`. The step rule: a publish reclaims at least what it obsoleted, in
  `reclaim_step` (16 MiB) truncations; `commit()` keeps its slice for the rest. Rationale and the
  reference engines in `docs/design/limits.md` ("Reclaim step and quota").
- What it keeps: `retain()` runs first, so a file a kept manifest names (a committed segment
  stays until the next manifest drops it), an unflushed segment, or a pending merge's source is
  never queued. Segments with no manifest naming them (flushed since the last commit, merged
  away since) are freed at once; that is why the no-commit run drains.
- Driver (`bench/kv/kv_strata.cc`): `settle()` calls `reclaim()` by default (`KV_SETTLE_RECLAIM=0`
  leaves the queue); `KV_SEGREPORT` prints per-segment docs, dead and section bytes and the
  queue on stderr.
- Test `test/reclaim_backlog_test.cc` (ctest `reclaim_backlog`, `reclaim_backlog_mapped`):
  1. 80 rounds of merges, no commit, `reclaim_slice` 0: disk segment files equal the live
     segments after every publish, queue empty. On main: 79 dead files (fails).
  2. Commits in between: the newest manifest on disk is intact after every step.
  3. Merge A publishes while merge B is pending over a source that left the list.
- Mutants: reclaim before retain fails test 1; queueing sources directly fails test 2; dropping
  the pending-merge sources from `retain()` aborts test 3 in the mapped run ("a file truncated
  below a live mapping").

## Next

Sanitizer ctest runs; the explorers' rows (A/A then A/B); review.

## Blocked on

Nothing.
