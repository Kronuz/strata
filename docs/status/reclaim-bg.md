# reclaim-bg: reclaim's file deletes on the host's executor

- Task (the lead's, queue item 3): move reclaim's deletes (truncate in steps, sync, unlink) off the owner
  thread onto the host's executor (`docs/reviews/reclaim-backlog.md`, CR3: the 64 KiB `ycsb_a` run phase was
  9 to 17% below main's because of owner-thread deletes; the reviewer's `reclaim_bg` prototype recovered it).
- Branch / worktree: `phase4/reclaim-bg` from main `1d76408`, `strata-wt/reclaim-bg`. Concurrency in file
  deletion: one code review before landing; this step stops at "in review".

## State

In review (concurrency in file deletion). Branch pushed; sanitizer runs and the A/B done. Not merged.

## Shape

- With a host executor (`options::background`), `reclaim_files` pops queued names up to the bytes owed and does the
  accounting at once: the file leaves `files_`, its bytes go to `inflight_` (still in the storage permit). It posts
  truncate (in `reclaim_step` steps), sync, unlink to `del_group_`, a group of its own, so `wait_merges()` does not
  wait on deletes.
- The job reports through a shared `delete_chan`: an atomic of finished bytes (the owner moves them from
  `inflight_` to `unlinked_`, where a directory sync frees them) and, on a failure, an atomic count plus a locked
  list (name, bytes posted, size left) that the owner requeues at its next round.
- `drain()` waits for the group before it reports space; `close()` and the destructor wait, never cancel.
- With no executor the deletes run inline, unchanged.

## Done criteria

1. Tests (`reclaim_bg_test`, ctest `reclaim_bg`, `reclaim_bg_mapped`; `reclaim_test` with a `bg` run, `reclaim_bg_crash`
   and `_mapped`):
   a. Posted: with an executor a publish posts the deletes and returns before they run (files still on disk, queue
      empty, bytes counted); running the executor deletes them and the disk equals the live segments.
   b. Failure: a faultfs fault fails a background step; the file is requeued, no bytes are lost or counted twice
      (the permit covers disk before and after), and a later round deletes it.
   c. `drain()` waits: with the deletes queued, `drain()` returns after they ran and the space is freed.
   d. `close()` waits: the deletes queued at close have run when it returns (nothing queued, no orphan).
   e. `wait_merges()` does not run the deletes.
   f. Crash mid-delete: at each mutation of a delete-heavy workload, power off; the reopen reclaims the orphan, no
      file unnamed, permit covers disk.
2. Per test a revert check (test fails with the engine change reverted), and mutants: accounting after the post, a
   swallowed failure, a drain that does not wait, each caught.
3. Full ctest under ASan+UBSan and under TSan.
4. The review's A/B (`ycsb_a`, 64 KiB, 2,000 records, 20,000 ops, 256 MB, 1 thread, durability none), main against
   the branch, at least 5 interleaved rounds under `quiet strict`: run-phase ratio with its direction, disk after
   settle, identical get digest. The delete CPU moves to the pool; it does not vanish.

## Done

- Engine (`include/strata/core/index.hh`): `reclaim_posted`, `run_delete` (the job: touches only the IO and
  the channel), `delete_chan`, `collect_deletes`, `requeue_delete`, `finish_deletes`, `end_deletes`; `drain()`
  waits; `settle_storage()` counts `inflight_`; `end_merges()` (close and the destructor) waits after it ends the
  merge groups; new public `wait_deletes()` (waits and collects; a host's settle, and the tests') and
  `deletes_pending()`. An executor whose `post` throws leaves the file queued.
- Behavior to know: with an executor the owner posts whole files, so a publish posts the cap plus at most one file
  (limits.md row updated); `open()` posts its orphans' deletes, so it returns before they are gone
  (`wait_deletes()`); `reclaim()` returns the bytes posted.
- Tests: `reclaim_bg_test` (ctest `reclaim_bg`, `reclaim_bg_mapped`): posted (30 publishes with the deletes
  queued; accounting balanced after each: files on record sum to `on_disk_`, posted sum to `inflight_`, permit
  covers disk, commits queue nothing twice), failed truncate / sync / remove, drain (admission on a tight
  budget), close and destructor, crash mid-delete (100 power-off points, 362 truncated orphans reclaimed by the
  reopen), dropped deletes, quota (the cap with an executor), threaded (2-thread pool, merges, commits, injected
  failures). `reclaim_test bg` (ctest `reclaim_bg_crash`, `_mapped`): the seeded power-loss sweep with the deletes
  posted to an inline executor and run between steps (3,871 points, 1,707 truncated orphans; mapped 1,585
  points). Adapted: `reclaim_backlog_test` sections 4a/4b run without an executor (the owner's step bound), 3b
  runs the executor after the publish; `background_merge_test` waits for the deletes after its reopens.
- Evidence (macOS x86_64, Homebrew clang 23): full ctest, 53 tests, passes at HEAD in RelWithDebInfo, under
  ASan+UBSan (Debug -O1) and under TSan (-O1). `reclaim_bg_test` ran 6 more times under TSan, clean.
- Revert and mutants (`.scratch/mut.sh`, logs `.scratch/mut-*.log`): each applied to `index.hh`, rebuilt, both
  tests run; every one fails `reclaim_bg_test`:
  | mutant | fails |
  |---|---|
  | revert (no delete group: deletes inline) | posted, failed x3, quota, crash/dropped preconditions |
  | accounting after the post (file stays on record until done) | posted (balance: posted sum != inflight), commit rounds |
  | swallowed failure (a failed step reports success) | failed x3 (file left on disk, bytes off record) |
  | drain does not wait | drain (admission refused) |
  | deletes share the merges' group | posted (wait_merges runs them), drain |
  | close cancels | close/destructor (queued deletes dropped) |
  | permit blind to in-flight bytes | posted (permit below disk) |
  | dropped delete not requeued | dropped |
  `reclaim_test bg` kills none of them: it passes on the inline path too; it is the crash sweep for the
  posted path (its `orphans > 0` count shows the sweep cuts deletes mid-way). A TSan mutant (the channel's lock
  removed) was not flagged in one run: the TSan check is the clean threaded run, not a kill.
- A/B (`.scratch/ab/ab.sh`, `quiet strict`, 8 interleaved rounds, both arms built from the same driver and flags,
  main `1d76408` headers against the branch's; `ycsb_a`, 64 KiB, 2,000 records, 20,000 ops, 256 MB, 1 thread,
  durability none, `--background 2`): run-phase throughput main 10,619 ops/s mean, branch 12,319 (1.16x main, every
  round faster: per-round 1.13x to 1.20x; slowest branch round 1.11x the fastest main round); total time 2.75 s to
  2.47 s; disk after settle 81,654,036 bytes and 10 files in both arms (settle waits for the deletes in both);
  get digest `a46c3543d251394d` in all 16 runs. The delete CPU moved to the pool; it did not vanish (a host with
  one thread per index pays it on the pool).

## Next

Code review (concurrency). Open for the reviewer: whole-file posting vs the step bound with an executor;
`open()` returning before its orphans are gone; the new `wait_deletes()` surface; a pool that never runs the
group stalls `drain()`.

## Blocked on

Nothing.
