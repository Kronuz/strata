# bisection-pool

- Design: docs/design/reordering.md D4 ("Threads": independent halves run on the executor's
  workers, bounded per node) and docs/design/phase-3.md D18 (strata starts no threads); queued
  item 6 of docs/status/README.md. Tier: fix (code review before landing).
- Branch / worktree: `phase3/bisection-pool` at `~/code/Kronuz/strata-wt/bisection-pool`, from
  `origin/main` at `32222a7`.
- State: implementing
- Done criteria (set by the lead; contributors tick, never edit):
  - [ ] K1 no std::thread left in bisection.hh; parts run on the shared pool.
  - [ ] K2 a test that saturates the pool (every worker busy in a merge that bisects) completes,
    and fails or hangs with a timeout without the deadlock fix.
  - [ ] K3 T7's pinned hash identical at 1 and 8 threads (the existing determinism test).
  - [ ] K4 CI green on all 8 jobs on the branch (dispatch with `gh workflow run` if pushes
    don't trigger it).
  - [ ] K5 performance neutral is measured later by the lead under a hold; note the A/B to run
    under Next.
- Done: (none yet)
- Doing: step 1, the code (bisection.hh on an executor group, the executor's bounded open,
  reorder/index/tool plumbing, the saturation test), drafted without compiling: a strict
  gate-run hold (`strata-trials`, until 07:04) is active, so CI validates.
- Next: see the plan below.
- Blocked on: nothing.
- Findings folded in: none yet.
- Last verified: nothing built locally (strict hold).

## Plan

1. `bisection::order` takes an `executor*`; with `threads` above 1 it opens a group of it that
   runs at most `threads - 1` helpers at once (`TaskGroup` capped, through `pool_executor`),
   and the caller bisects parts too. Helpers take one part each and repost while parts remain.
2. The deadlock fix: the caller never waits for work that has not started. Once no part is
   left or running, it cancels the group (dropping queued helpers) and waits only for running
   ones, which hold no part. On a saturated pool the caller does every part itself. No
   threadpool change is needed; the separate-pool alternative is not taken, since it puts the
   parts beside the shared pool again (K1).
3. `executor::open_bounded(width)` (additive, default `open()`), `reorder::order` and the
   merge pass the executor; `strata_engine merge` runs on a pool when
   `STRATA_REORDER_THREADS` is above 1, so the tool keeps bisecting on that many threads.
4. Tests: bisection_test on a pool at 2, 4, 8 threads; the saturated-pool case under a
   watchdog (K2); T7 at 8 threads on a pool (K3). K2's "without the fix" evidence: a throwaway
   CI run with the caller waiting on the group instead of cancelling it.
