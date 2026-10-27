# cold-tier

> Status: CURRENT (2026-10-27) the designer's ledger for `docs/design/cold-tier.md`.

- Design: `docs/design/cold-tier.md` revision 0 (hot, warm, cold and remote tiers; the
  per-partition catalog; demotion by unmapped twins). Concurrency and lifetimes: the architect's
  preliminary review comes next.
- Branch / worktree: `design/cold-tier` from main `98ce743`, at
  `/Users/kronuz/code/Kronuz/strata-wt/cold-tier`.
- Evidence: `docs/perf/idle-index.md` on `perf/idle-index` `2562e8f`; shared-reader at
  `design/shared-reader` `15b801a`; write buffer at `design/write-modes` `ae2cf70`.
- State: designing (ready for preliminary review).
- Next: the architect's preliminary review; fold its findings in. Step 1 (cut 1 and `demote()`
  without the catalog) is buildable once the review clears it; only Q3 touches it.
- Blocked on: the owner's answers to Q1 to Q4 for steps 2 to 5 (none blocks step 1 beyond Q3's
  default for `eager_segments`).

## Log

- 2026-10-27: revision 0 drafted. Decisions D1 to D8, invariants I1 to I7, gate rows T1 to T7,
  questions Q1 to Q4. No build or benchmark run (design only).
