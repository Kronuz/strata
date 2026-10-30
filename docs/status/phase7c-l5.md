> Status: CURRENT (2026-10-29) Phase 7c step L5 (`defer_io`, the WorkPool executor binding); supersedes nothing.

# phase7c-l5

- Design: Xapiand `docs/design/phase7c.md` D4, D5, D6, D7 and section 5's L5 (Xapiand:4ef308995); review `docs/reviews/phase7c-design.md` R5, C15, C16 and re-check 1. Reviews of this branch: none yet.
- Branch / worktree: `phase7c/l5-defer-io` at `/Users/kronuz/code/Kronuz/strata-wt/phase7c-l5`, from main 8fac1bc; head: see `git log`.
- State: implementing
- Done criteria (the launch message's proof list; the lead sets, contributors tick):
  - [ ] K1 `defer_io` option; `apply` makes no device call on each owed path (flush, drain, owed chores incl. merge publication and retention's manifest read): counted with faultfs `calls()`, each with a control that shows the I/O without the option.
  - [ ] K2 The owed verdicts, and running them later through a chore (`refresh`, `reclaim_for_space`, `check_in`), then the retry accepted.
  - [ ] K3 C15: an oversize transaction and a full disk end after at most one owed round.
  - [ ] K4 A mutant per claim dies.
  - [ ] K5 ASan+UBSan and TSan builds clean; the existing suite green.
  - [ ] K6 The default (`defer_io` off) behaves byte for byte as before.
  - [ ] K7 `WorkPool` executor binding through the executor concept; `cpp-libraries/tools/preflight.sh` before the push.
  - [ ] K8 How-it-works note `docs/mechanisms/defer-io.md`.
- Done: (see log)
- Doing: tests and proof.
- Next: mutants, sanitizers, preflight, push; the lead asks for the code review and the architect's scoped check.
- Blocked on: nothing.
- Findings folded in: none yet.
- Last verified: nothing yet.

## Decisions to review (mine, within the design)

- `install()` still drains inline: the design says it "defers its drain the same way", but install reads and writes whole files, so it is a chore already and its callers are hosts' chores; deferring would need a new exception type for a `void` call. Not done; 7c does not call it. Architect's call.
- A segment not yet mapped (`eager_segments` off) maps at its first read inside `apply` (an `open` and a map). Documented on the option; Xapiand opens with the default (eager).
- The executor binding keeps `pool_executor` (ThreadPool) beside it: eight test files and `bench/kv` use it. Retiring it is a migration for the lead to schedule; `background_merge_test workpool` runs the binding under the existing merge suite.
- threadpool is pinned to `2ae90f0` (`phase7c/l2-scheduler`, under review); repin when it lands.

## Log

- 2026-10-29: branch from main 8fac1bc; read the design, R5, C15, C16 and the L2 `WorkPool` API (`work_pool.hh` at threadpool:2ae90f0).
