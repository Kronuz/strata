# Retention default 0 (phase4/retention)

Branch `phase4/retention` from main, worktree `strata-wt/retention`. Owner decision
(2026-10-20): `retained_predecessors` defaults to 0; damage to the current manifest or
newest segment is refused and reported; 1 or 2 stay an option, and any open that falls back
to a predecessor reports it as data loss. Measurements: `docs/perf/retention.md` on
`explore/retention`. Landing: durability policy, so one code review and a scoped architect
check come first.

## State

Implementing; first build pending.

## Done

- Default 0, `rollback()` report, design text, `test/retention_test.cc`, suites that assume
  predecessors set 2 explicitly (media, index, reclaim, lifecycle).

## Next

Build (ASan+UBSan), full ctest, revert checks per behaviour, one mutant (silent fallback),
push, then remove builds and the explorer worktree.

## Blocked on

Nothing.
