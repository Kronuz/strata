# core-lifecycle-tests: the core's lifecycle suites on the document layout

- Task (the lead's, 2026-10-20): the lifecycle suites went to prospect with search
  (`docs/status/repo-split.md`, "Next"); strata covered the core only through `core_kv`. Port index,
  lifecycle, reclaim, directory, media, and background merges, with their `_mapped` runs and
  `directory_tmpfs`, to `strata::document_layout`, core headers and `strata::core` only. Drop only
  assertions that are about search. prospect's copies stay.
- Branch: `phase4/core-lifecycle-tests` (worktree `strata-wt/core-lifecycle`), from main `426cc59`.
- Non-sensitive (tests only): no review; lands by fast-forward or merge once CI is green.

## State

Started. Baseline on this host (macOS x86_64, RelWithDebInfo): 9 tests pass.

## Done

## Next

- Port, in order: media, reclaim, directory, lifecycle, background_merge, index.
- One mutant per test file; sanitizers; push; dispatch CI.

## Blocked on

- Nothing.
