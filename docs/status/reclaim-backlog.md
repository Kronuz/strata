# reclaim-backlog: merged-away files wait for the host's commits

- Task (the lead's, 2026-10-21): the explorers (`explore/bigval`, `explore/bigdisk`) found 44 files
  / 600 MB (64 KiB overwrites) and 193 to 210 files (1M fills at 10x RAM) waiting in the reclaim
  queue after settle: `commit()` reclaims `reclaim_slice` (64 MiB) per call, and with durability
  none no commit happens during a run. Draining took disk from 660-790 to 79-88 MB and from
  184-208 to 72 MB, level with RocksDB, data identical.
- Branch / worktree: `phase4/reclaim-backlog` from main `d1bbc42`, `strata-wt/reclaim-backlog`. It
  deletes files, so it takes one code review before landing.

## State

Review (`docs/reviews/reclaim-backlog.md`, approve with conditions) folded in at the second fix round: CR2, CR3, CR4 closed; waiting for the re-check. Not merged.

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

- Evidence (macOS x86_64, Homebrew clang 23): full ctest, 39 tests, passes under ASan+UBSan
  (Debug -O1) and TSan (-O1) at the first commit; after round 1, 40 tests under ASan+UBSan. Revert check: with the `index.hh` change reverted, `reclaim_backlog`
  fails ("worst dead files on disk 79"); tests 2 and 3 pass on main too (they guard the new path).
- Explorer rows, strict `quiet run reclaim 20`, 3 interleaved rounds, scripts in
  `.scratch/reclaim-backlog/` (main checkout). A = main `d1bbc42`, B0 = fix without the settle
  drain, B = fix + settle drain, A2 = main again. Get digests identical in all 24 runs
  (sizes1 `a46c3543d251394d`, big1 `1ac557eb7ac4608e`).

  | row | arm | disk after settle | files | run ops/s | total s |
  |---|---|---|---|---|---|
  | sizes1 ycsb_a v65536 | A / A2 | 681-759 MB | 57-64 | 8552-9113 | 3.11-3.30 |
  | | B0 | 80-83 MB | 13-16 | 7312-7651 | 3.56-3.76 |
  | | B | 79-87 MB | 6-14 | 7288-7651 | 3.52-3.71 |
  | big1 fillrandom v100 1M | A / A2 | 187-235 MB | 202-235 | 392k-400k | 5.35-6.05 |
  | | B0 | 72.3-72.4 MB | 5-8 | 389k-396k | 5.36-5.89 |
  | | B | 72.3 MB | 5-10 | 388k-395k | 5.42-5.97 |

  The A/A spread is under 4% on sizes1 run ops/s and 20% on big1 disk. Throughput is not
  unchanged on sizes1: the run phase is 14-17% slower and total time +0.3 to 0.5 s, because
  the truncate/unlink of about 600 MB now happens inside the timed run (the explorers' drain
  cost +0.3 s at settle); big1 is within noise. B0 drains already (the engine alone), so the
  settle `reclaim()` only mops up the last files.

## Review round 1 (CR2, CR3, CR4)

- CR2 (the call was unbounded): `install_merge` still counts the bytes it obsoleted, now into a
  debt (`debt_`). Each publish pays `min(debt_, reclaim_cap)` (option, 64 MiB), after `retain()`;
  `reclaim()` lowers the debt by what it freed and forgives it when the queue is empty, so a
  commit's slice and an explicit `reclaim()` pay it too, oldest file first. What holds: one
  syscall frees at most a step; one publish frees at most the cap plus a step; the queue stays
  bounded while a call's merges obsolete no more than the cap on average; a merge past the cap
  is paid over later calls. Text corrected at `index.hh` (options comment) and the limits row.
  The test counts the call's mutations (truncate, sync, unlink): cap 2,048, step 256: the publish
  freed 2,048 bytes in 11 mutations, against 8 segments obsoleted. The reviewer's `bound_probe`
  (step 16,384, `reclaim_cap` 262,144) at the new header: merges of 2 MiB, 8 MiB and 32 MiB
  each free 262,144 bytes in the publish, in 19, 17 and 17 mutations (before: 1,020 mutations
  and 16.5 MB freed at the 32 MiB merge); the rest stays queued (3 to 4 files) and is paid by
  later calls. Log: `.scratch/bound_probe_cap.log`.
- CR4: `reclaim_backlog_test` 3b (a merge whose sources left the list: its discarded output is
  gone after that publish; kills dropping `obsoleted_ += job.output->ref.size`), 4a (the cap and
  the mutation count; kills removing the cap and `reclaim()` for the bounded call), 4b (a
  backlog of six unpaid files, then a small merge: it frees what it obsoleted and a step, not the
  backlog; kills `reclaim(owed)` to `reclaim()`), and `reclaim_backlog_view_xfail`: a view held
  across `refresh()` at retention 0 under mapping. That one cannot survive until shared-reader
  D7 (the views' segments in `retain()`) lands: it aborts ("a file truncated below a live
  mapping"), so ctest runs it as WILL_FAIL; when D7 lands it passes unexpectedly and the flag
  comes off. Mutants, each failing the test named: drain-all, no-cap, no-discard-count,
  reclaim-before-retain (plain and mapped).
- CR5 (for the shared reader's step): D7's gate is `retain()`, which the publish now calls; the
  scan-and-free of views must run before that `retain()`, and the reader-visible publication
  before the reclaim in `publish_merges`.
- CR3 (cost): the 64 KiB `ycsb_a` run-phase throughput is 9 to 17% below main's because the
  truncates and unlinks of about 600 MB run on the owner thread inside the timed run (reviewer 9%
  in six rounds, ours 14 to 17% in three; total time +0.3 to 0.5 s). The number is the owner-thread
  delete, not the engine's: the reviewer's `reclaim_bg` prototype (deletes posted to the
  executor) recovered it, 1.17x the branch and 1.06x main, with the same final disk.

## Owed

Move the already-unreferenced deletes to the executor (the `reclaim_bg` prototype in the
review's `.scratch/review/bg-inc`): the accounting stays on the owner, a background failure
needs a channel back to requeue, `drain()` must wait for in-flight deletes, `close()` waits or
leaves orphans to the next open, and deletes get a group of their own. It is concurrency in
file deletion, so it takes its own review. Until it lands, read the 64 KiB row's run phase as
including the deletes.

## Next

The re-check of CR2 to CR4; then land; then the executor delete.

## Blocked on

Nothing.
