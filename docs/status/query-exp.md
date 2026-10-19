# query-exp: the query gap's experiments (E1, the base check, the freeze set, H22, key positions)

- Design: `docs/design/query-gap.md` revision 2 (branch `design/query-gap`, e272ec2); the
  lead's rulings and E0's quick-tier calibration in that branch's `docs/status/query-gap.md`.
- Branch / worktree: `perf/query-exp` from origin/perf/query-counters b7823cd, at
  `~/code/Kronuz/strata-wt/query-exp`.
- Done criteria: the query-gap ledger's K2 to K4 (and K5, K6 if M17 or M23 are kept) are the
  lead's; ticked there, never edited here.

## State

E1 measured on the gate VM (counters, profiles, in-process times on the reordered merged and
16-segment 5M indexes); its record holds every row's split and H1 to H20's verdicts, H21 and
H22 pending. The base check's dumps are running on the VM.

## Done

- Plan and ledger: 6013d2c. Merged the cleared counters (re-check 750ce06): 5b67776.
- E1 (T1): `docs/perf/query-gap-attribution.md` and its `.sha256` manifest. Confirmed: H2
  (term `TOP_10`), H3, H6, H13, H16, H20; refuted: H9; open: the rest, each naming what it
  waits on. A part no hypothesis names: region verification, 10-13% of two-phase-critic.

## Next

1. E1: counting and Release builds on the gate VM; counters over S per command; profiles per
   class and command; verdicts H1 to H22.
2. The base check, then E3, E6, E7, E2 (M1, M2, M9), E5, E4 if E3 says so.
3. H22: decode cost per posting against SIMD-BP128 on real 5M postings.
4. Format 13's key-position question with keybench.

## Blocked on

Nothing blocks the queue. For the lead: the quick-tier calibration ran `queries-sample.txt`
only, so the term rows rest on one query and term:rare/medium/frequent have no reference
(H17's ceiling waits on it); E0's `dump` references and the `STRATA_DELETE_EVERY` index are
not on the VM, so the base check compares E0's binary (aec8d11) with the base directly.
