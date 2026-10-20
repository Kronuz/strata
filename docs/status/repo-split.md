# repo-split: search moves to Kronuz/prospect

- Task (the lead's, 2026-10-20): move strata's search side into its own repository,
  [Kronuz/prospect](https://github.com/Kronuz/prospect) (PLAN, "Names"), with history; strata
  keeps the core. Design: `docs/design/core-split.md` (the targets, hooks, and boundary tests).
- Branches: strata `phase4/repo-split` (worktree `strata-wt/repo-split`), prospect
  `phase4/repo-split` (checkout `~/code/Kronuz/prospect`).
- State: strata's removal builds and its 9 tests pass on macOS; prospect in progress.
- Land order: prospect `main` first, then this branch once prospect's CI is green.

## Done

- prospect created (public, empty) at https://github.com/Kronuz/prospect.
- strata: search removed; `strata::core` is the only target (`strata::search`, `strata::strata`
  and the linked core include root are gone); `kv_strata_search` dropped.

## Next

- prospect: namespace, targets, callers, CI, docs; identity proof at 100k.

## Blocked on

- Nothing.
