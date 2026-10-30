> Status: CURRENT (2026-10-29) Phase 7c step L5 (`defer_io`, the WorkPool executor binding); supersedes nothing.

# phase7c-l5

- Design: Xapiand `docs/design/phase7c.md` D4, D5, D6, D7 and section 5's L5 (Xapiand:4ef308995); review `docs/reviews/phase7c-design.md` R5, C15, C16 and re-check 1. Reviews of this branch: none yet.
- Branch / worktree: `phase7c/l5-defer-io` at `/Users/kronuz/code/Kronuz/strata-wt/phase7c-l5`, from main 8fac1bc; head: see `git log`.
- State: in review (needs the code review and the architect's scoped check)
- Done criteria (the launch message's proof list; the lead sets, contributors tick):
  - [x] K1 `defer_io` option; `apply` makes no device call on each owed path, counted with faultfs `calls()` (new), each with a control showing the I/O without the option: flush (`flush_owed`), drain (`drain_owed`), merge publication and retention's manifest read among the owed chores (`merge_publication_owed`, `retention_read_owed`: the apply that meets owed chores makes 0 calls, then `check_in()` makes >0 and publishes). `test/defer_io_test.cc`.
  - [x] K2 The owed verdicts and their chores (`refresh`, `reclaim_for_space`, `check_in`), then the retry accepted at the same position (not skipped).
  - [x] K3 C15: oversize (document cap, lane cap) ends: invalid on an empty buffer, one `flush_owed` on a non-empty one; a full disk with nothing queued answers `no_space` at once; a failed removal is owed once, the retry answers `no_space`, the next apply starts a new round. C16: `reclaim_after_release` (host mode, no check-in).
  - [x] K4 Mutants (`.scratch/mut/run.py`, results in `.scratch/mut/results.txt`): 12 of 12 killed, see Log.
  - [x] K5 `preflight.sh` mac (hardened; ASan+UBSan), Linux and FreeBSD VMs all ok on 27c944e (61 tests each); TSan on defer_io (2), work_pool_executor, background_merge (3, one on the WorkPool binding), write_buffer (2), shared_view (2): 10 of 10 pass, 0 reports.
  - [x] K6 `default_path_unchanged` (golden values captured on main 8fac1bc: verdicts, 163 mutating calls, file hash, digest).
  - [x] K7 `work_pool_executor` through the executor concept; `background_merge_test workpool` runs the merge suite on it; preflight mac ok.
  - [x] K8 `docs/mechanisms/defer-io.md`.
- Done: strata:4563ed1 (the option, the verdicts, `reclaim_for_space`, `work_pool_executor`, faultfs `calls()`, tests, note); strata:b58078c (ledger); strata:0bc8969 (cancel drops queued work before requesting the stop; FreeBSD found it); strata:27c944e (pin threadpool:0244bd3, AC1, the chore-waits-on-merge test, wait audit).
- Doing: nothing; pushed, CI not watched.
- Next: the lead asks for the code review and the architect's scoped check; repin threadpool when L2 lands.
- Blocked on: nothing.
- Findings folded in: none yet.
- Last verified: 2026-10-29 on 27c944e: ctest 61 of 61 (mac, arm64); preflight mac, linux, freebsd ok; TSan subset clean. CI: pushed, results not watched.

## Found outside this step

- threadpool:2ae90f0 `WorkGroup::cancel_and_wait()` (`work_pool.hh`, after `wait`) counts the unlinked items and never frees the boxed ones `enqueue` allocated: five tracked callables stay alive after it returns (probe: `strata/.scratch/l5-probes/workgroup_cancel_and_wait_leak.cc`). The destructor frees them; the adapter here frees what `cancel()` returns itself and does not use `cancel_and_wait`. For the L2 reviewer.

## Wait audit (threadpool L2 re-check, architect ruling on B2/N1, AC1)

Every `executor::group::wait()` or `cancel()` the binding reaches, at `index.hh` of this branch. All run in the index's owner (under the write lease, an atomic token, not a lock) on the index's own child groups; none runs from a group's own work.

| Site | Group | Caller | Lock held across the wait |
|---|---|---|---|
| `publish_merges(wait)` (`:3105`) via `merge`, `force_merge`, `force_merge_deletes`, `wait_merges` | merges (`group_`) | owner | none (lease only) |
| `finish_deletes` (`:3395`) via `drain`, `wait_deletes` | deletes (`del_group_`) | owner | none |
| `end_deletes` (`:3405`) via `close`, the destructor, `end_merges` | deletes | owner | none |
| `abandon_merges` (`:3288`), `end_merges` (`:3277-3278`) | merges, run builds (`runs_group_`) | owner (`cancel()` and the group's destructor) | none |

The posted work never waits: `run_merge`, `run_delete`, and the run builds (posted, polled by `take_runs`, never joined). Its only locks are leaves held for a few statements: `delete_chan::mu` (`report`, `collect_deletes`), `rate_limiter::mu_` (released before `sleep_until`), and the binding's `mu_` (released before `g_.wait()`). A group's destructor and `cancel()` wait after dropping `mu_`. Inline depth under Xapiand: a chore of the index's group, then a merge or delete item: 2. `executor.hh` states the rule (AC1). `defer_io_test chore_waits_on_merge_chore` runs a flush chore on a host group that calls `force_merge` and `commit` while the merge chores sit on the index's group, at 1 thread with no reserve, 2 with one reserved and 4, at width 1 and 2: it deadlocks at threadpool:2ae90f0 (width 1: a waiter could not help a group whose ticket was in the ring; reproduced at 1 and 2 threads) and passes at threadpool:0244bd3, which the branch pins. The suite does not depend on rescue threads.

## Decisions to review (mine, within the design)

- `install()` still drains inline: the design says it "defers its drain the same way", but install reads and writes whole files, so it is a chore already and its callers are hosts' chores; deferring would need a new exception type for a `void` call. Not done; 7c does not call it. Architect's call.
- A segment not yet mapped (`eager_segments` off) maps at its first read inside `apply` (an `open` and a map). Documented on the option; Xapiand opens with the default (eager).
- `pool_executor` is retired (C4, N6): its eight callers moved to `work_pool_executor` and `pool_executor.hh` is deleted. Every migrated test keeps its checks; none relied on helping other groups or on self-waits. `background_merge_workpool` was the same suite on the binding, so with the plain run on it too the ctest entry is removed: 61 tests become 60.
- threadpool is pinned to `e961e447` (main, the landed WorkPool).

## Log

- 2026-10-29: branch from main 8fac1bc; read the design, R5, C15, C16 and the L2 `WorkPool` API (`work_pool.hh` at threadpool:2ae90f0).
- 2026-10-29: 4563ed1: the option, `flush_owed`/`drain_owed`, `reclaim_for_space`, the binding and tests. ctest 61/61. Mutants, each killed by the test named: M1 apply refreshes inline (flush test), M2 flush owed on an empty buffer (oversize), M3 apply drains inline (drain), M4 apply runs owed chores at its end (merge publication), M5 handoff checks in (merge publication), M6 drain_owed repeats and M8 the round never ends (failed removal), M7 drain owed with nothing queued (full disk), M9 replay check removed (default-path identity), M10 cancel leaks queued items, M11 work in the query class (reserve), M12 cancel does not request stop (timeout).
- 2026-10-29: 0bc8969: FreeBSD failed the cancel test 3 of 3 (a stop requested before the unlink let the next queued item start); fixed by unlinking first. 27c944e: coordinator's L2 conditions folded in; threadpool pin 0244bd3; the new chore-waits test deadlocked at 2ae90f0 (width 1, 1 and 2 threads) and passes at 0244bd3; mutant 13 (group wait returns at once) killed. All preflights ok; pushed.
