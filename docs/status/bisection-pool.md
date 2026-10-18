# bisection-pool

- Design: docs/design/reordering.md D4 ("Threads": independent halves run on the executor's
  workers, bounded per node) and docs/design/phase-3.md D18 (strata starts no threads); queued
  item 6 of docs/status/README.md. Tier: fix (code review before landing).
- Branch / worktree: `phase3/bisection-pool` at `~/code/Kronuz/strata-wt/bisection-pool`, from
  `origin/main` at `32222a7`.
- State: in review (every criterion met but K5, which is the lead's measurement)
- Done criteria (set by the lead; contributors tick, never edit):
  - [x] K1 no std::thread left in bisection.hh; parts run on the shared pool: `git grep
    'std::thread\|<thread>' include/strata/bisection.hh` finds nothing at `d706910`; helpers
    go to `pool_executor::open_bounded`, a `TaskGroup` on the host's pool (bisection_test
    checks they were posted, at most 7 at once for 8 threads, and TSan passes on them). The
    test prints how many ran, but ctest keeps stdout, so CI shows no count.
  - [x] K2 a test that saturates the pool (every worker busy in a merge that bisects) completes,
    and fails or hangs with a timeout without the deadlock fix: bisection_test's saturated
    case (2 workers, each in a merge bisecting at 8 threads, 120 s watchdog) passes on all 8
    jobs of run 39090439479; with the caller waiting on its group instead of cancelling it
    (probe `657e85f`, run 39090448057, linux-arm64-gcc) it fails
    `bisection_test.cc:222: bisection on a saturated pool hangs` after 120.29 s, the only
    test of 33 to fail. The probe run was then cancelled and its branch deleted.
  - [x] K3 T7's pinned hash identical at 1 and 8 threads (the existing determinism test):
    reorder_test passes on all 8 jobs of run 39090439479 with the pin unchanged
    (`0xed10efddd8c1bc39`), its 8-thread merge now on an 8-worker pool.
  - [x] K4 CI green on all 8 jobs on the branch (dispatch with `gh workflow run` if pushes
    don't trigger it): run 39090439479 on `d706910`, success on all 8 (TSan 41 min, ASan+UBSan
    within its limit).
  - [ ] K5 performance neutral is measured later by the lead under a hold; note the A/B to run
    under Next. (The A/B is under Next, item 2.)
- Done:
  - Step 1, drafted without compiling (a strict gate-run hold, `strata-trials`, until 07:04):
    bisection on an executor group, `executor::open_bounded`, the reorder/merge/tool plumbing,
    the tests (`d706910`).
  - Step 2, CI: run 39090439479, all 8 jobs green on `d706910`.
  - K2's negative probe: `657e85f` on a throwaway branch, run 39090448057, failed on the
    watchdog as expected; branch and worktree removed.
- Doing: nothing; waiting for the code review.
- Next:
  1. The lead launches the code review (fix tier); findings fold in citing their IDs.
  2. K5, the lead's A/B under a hold, on `main` once landed against its parent: the forced
     merge of the 5M index, `strata_engine merge` (T3's procedure, `/usr/bin/time -v`),
     interleaved, three trials each, at `STRATA_REORDER_THREADS` unset (1: the gate's
     setting; the change adds one uncontended lock pair per part there) and 8 (now a pool of
     8 workers through `pool_executor`, before 8 std::threads). Wall time and peak RSS;
     neutral means within trial noise, and the 1- and 8-thread outputs byte-identical
     (`cmp` of `seg-*.sx`).
- Behavior change to note: a host that sets `reorder_threads` above 1 but passes no
  `background` executor now bisects on its merge's thread alone (the index's own executor is
  inline). `strata_engine merge` is the one such host in the repository and now passes a pool.
- Blocked on: nothing.
- Findings folded in: none yet.
- Last verified: 2026-10-18, CI run 39090439479 (all 8 jobs, 33 of 33 tests each) on
  `d706910`; nothing built locally (strict hold `strata-trials` until 07:04).

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
