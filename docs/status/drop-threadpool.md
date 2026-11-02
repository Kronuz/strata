> Status: CURRENT (2026-11-02) workstream: one pool for the Kronuz libraries (developer), strata's part.

# drop-threadpool (strata)

- Task: threadpool deletes the `ThreadPool` family; strata pins the new head.
- Branch: `phase7c/drop-threadpool`, from origin/main `strata:64d3298`.
- State: `CMakeLists.txt` pins threadpool `db38594` (threadpool branch `phase7c/drop-threadpool`) in place of `65bc0bc`. strata uses
  `WorkPool`/`WorkGroup` only (`include/strata/core/work_pool_executor.hh`); no source file names `ThreadPool`, `TaskGroup` or a
  `ThreadPolicyType`, so nothing else changes. The two `TaskGroup` mentions in `docs/design/` describe the pool at pin `628679a`
  and stay as history.

## Done

- Pin moved (this branch's commit).

## Proof

See the commit message for the preflight and CI runs.

## Next

Prospect pins this branch's head; the lead lands threadpool, scheduler, logger, then strata, then prospect.

## Blocked on

Nothing.
