# lanebuf (write-buffer bake-off, step 0)

- Task: build and run the bake-off prototype of `docs/design/write-buffer.md`; record in
  `docs/perf/write-buffer-bakeoff.md`.
- Branch / worktree: `perf/lanebuf` from `design/write-modes` at `acdc908`,
  `/Users/kronuz/code/Kronuz/strata-wt/lanebuf`. Not merged.
- State: building the prototype
- Owner's answers folded in: Q1 (visible once acknowledged; no log here, so at group end), Q2,
  Q3 (1.5x requests/s or 0.5x p99, at most 3% at one writer, at most 5% CPU per request).
- Done: nothing committed yet beyond the plan.
- Next: lanebuf.hh/.cc, model check under TSan, T1 to T4/T6/T11 runs, verdict.
- Blocked on: nothing.
