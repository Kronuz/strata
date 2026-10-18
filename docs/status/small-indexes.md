# small-indexes

- Design: docs/design/small-indexes.md (a note, not yet a design); experiments in
  docs/perf/small-indexes.md, each written before it runs.
- Branch / worktree: perf/small-indexes at /Users/kronuz/code/Kronuz/strata-wt/small-indexes,
  from origin/main at 6e9ba58, with perf/inverter merged at 339acf7 (ad12899).
- State: implementing (M0's tools).
- Done criteria (set by the lead; contributors tick, never edit): none set yet.
- Done:
  - Worktree and branch; perf/inverter merged for its index path (ad12899).
  - M0 to M2 written before they run (docs/perf/small-indexes.md).
- Doing: `strata_engine build-small` and `open-many` (M0's tools).
- Next:
  1. M0 on the gate VM: 10,000 indexes at 1, 10, 100, 1,000 documents; then macOS.
  2. M1: builders and openers for Tantivy and Lucene; the fastest baseline per row.
  3. M2: N = 8 and N = 1,000 small indexes writing at once on 8 cores.
  4. The biggest per-index cost first: lazy open, idle close, shared machinery, or the
     small-segment format profile (notes to format 13's designer).
- Blocked on: nothing.
- Findings folded in: none yet.
- Last verified: 2026-10-18. Nothing built or run on this branch yet.
