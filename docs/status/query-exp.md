# query-exp: the query gap's experiments (E1, the base check, the freeze set, H22, key positions)

- Design: `docs/design/query-gap.md` revision 2 (branch `design/query-gap`, e272ec2); the
  lead's rulings and E0's quick-tier calibration in that branch's `docs/status/query-gap.md`.
- Branch / worktree: `perf/query-exp` from origin/perf/query-counters b7823cd, at
  `~/code/Kronuz/strata-wt/query-exp`.
- Done criteria: the query-gap ledger's K2 to K4 (and K5, K6 if M17 or M23 are kept) are the
  lead's; ticked there, never edited here.

## State

Started 2026-10-19: worktree created; E1's plan written (`docs/perf/query-gap-attribution.md`).

## Done

## Next

1. E1: counting and Release builds on the gate VM; counters over S per command; profiles per
   class and command; verdicts H1 to H22.
2. The base check, then E3, E6, E7, E2 (M1, M2, M9), E5, E4 if E3 says so.
3. H22: decode cost per posting against SIMD-BP128 on real 5M postings.
4. Format 13's key-position question with keybench.

## Blocked on

Nothing.
