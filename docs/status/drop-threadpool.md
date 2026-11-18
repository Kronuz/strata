> Status: HISTORY (2026-11-08): superseded by Xapiand's `.campaign/xapiand/` (`campaign map`); kept for its results and reasoning. Was: (2026-11-02) workstream: one pool for the Kronuz libraries (developer), strata's part.

# drop-threadpool (strata)

- Task: threadpool deletes the `ThreadPool` family; strata pins the new head.
- Branch: `phase7c/drop-threadpool`, from origin/main `strata:64d3298`.
- State: `CMakeLists.txt` pins threadpool `eb4751a` (threadpool branch `phase7c/drop-threadpool`) in place of `65bc0bc`. strata uses
  `WorkPool`/`WorkGroup` only (`include/strata/core/work_pool_executor.hh`); no source file names `ThreadPool`, `TaskGroup` or a
  `ThreadPolicyType`, so nothing else changes. The two `TaskGroup` mentions in `docs/design/` describe the pool at pin `628679a`
  and stay as history.
- Second commit: task moves `8cad661` to `b406766` (task main, cancellation step C1: `lock(token)` replaces `lock_until` and
  `lock_for`; strata names neither, nor `async_mutex`). One task commit across the family: Xapiand pins it too.

## Done

- Pin moved (this branch's commit).

## Proof

- `preflight.sh` (mac hardened, mac ASan+UBSan, Linux, FreeBSD) ok on the pin commits. CI at `3f564dc`: run 41546532591, all 8 jobs
  green.

## Next

Prospect pins this branch's head; the lead lands threadpool, scheduler, logger, then strata, then prospect.

## Blocked on

Nothing.
