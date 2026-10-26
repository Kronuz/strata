# reclaim-bg: reclaim's file deletes on the host's executor

- Task (the lead's, queue item 3): move reclaim's deletes (truncate in steps, sync, unlink) off the owner
  thread onto the host's executor (`docs/reviews/reclaim-backlog.md`, CR3: the 64 KiB `ycsb_a` run phase was
  9 to 17% below main's because of owner-thread deletes; the reviewer's `reclaim_bg` prototype recovered it).
- Branch / worktree: `phase4/reclaim-bg` from main `1d76408`, `strata-wt/reclaim-bg`. Concurrency in file
  deletion: one code review before landing; this step stops at "in review".

## State

Building.

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

## Next

## Blocked on

Nothing.
