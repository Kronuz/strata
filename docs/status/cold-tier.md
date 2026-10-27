# cold-tier

> Status: CURRENT (2026-10-27) the designer's ledger for `docs/design/cold-tier.md`.

- Design: `docs/design/cold-tier.md` revision 1 (hot, warm, cold and remote tiers; the
  per-partition catalog; demotion by unmapped twins; D9 to D11 for step 1).
- Review: `docs/reviews/cold-tier-design.md` (preliminary, APPROVE WITH CONDITIONS).
- Branch / worktree: `design/cold-tier` from main `98ce743`, at
  `/Users/kronuz/code/Kronuz/strata-wt/cold-tier`; citations checked at main `8798fe1`, whose
  `include/` equals `98ce743`'s.
- State: designing (revision 1 ready for the architect's re-check).
- Done:
  - strata:4bc1a06 revision 0.
  - strata:8459c7b the architect's preliminary review.
  - Revision 1 folds in CT-R1 (D3), CT-R2 (D2, D9), CT-R3 (D10), CT-R14 (D11, Q3) and CT-R16
    (the criteria below); records CT-R9 as N1; lists CT-R4, R6 to R8, R10 to R13 and R15 as open
    for step 2 and step 3; adds Q5. The architect's probes rerun under a light hold with output
    identical to theirs (`twin_race`, `quiescence_probe`, `apply_throw_probe`,
    `warm_bytes_probe`). No build or benchmark (design only).
- Next: the architect's re-check (high) of revision 1's text. Step 1's items 1 and 2 may start
  with `eager_segments` on by default.
- Blocked on:
  - CT-R5's rollback (`fix/apply-atomic`) landing on `main`: lazy segments become the default only
    after it (step 1, item 6; K3).
  - The lead's restatement of T1 and T2 (CT-R9, CT-R11) before they gate; Q5 for the owner (not
    blocking step 1: (a) is buildable).

## Done criteria for step 1

Each with its evidence on the branch, re-checked before landing.

- **K1.** Record and body, compare-exchange install, a loser that destroys only its own body;
  T6 over the install under TSan, with the mutants "body cleared in place", "body stored without
  compare-exchange" and "loser reads the body it destroyed".
- **K2.** D9's table in code, each map point's failure rule with a faultfs-injected test (open,
  `mmap` and check failures): no fence for host errors, damaged-and-skipped merge sources,
  commit's prepare leaving the index unchanged and unfenced.
- **K3.** CT-R5's rollback on `main`, `apply_throw_probe` as its test, failing before and passing
  after.
- **K4.** The twelve damage cases with lazy segments; a missing or resized manifest-named file
  fails the open (D11).
- **K5.** `demote()` twinning every record and forcing its publication, under D10's warm
  predicate; T6's mutants including "twin only bodied records" and "close with a posted delete";
  T5 on one index with readers holding views across `demote()` and an index idle after posting
  deletes.
- **K6.** T1's step-1 rows: hot within 5% of today; demoted with its own target (N1); 1 and 10
  segments.
- **K7.** prospect's construction sites and Xapiand's seam listed, with the pin bump that adopts
  `seg()`.
- **K8.** CI green on every platform, ASan, UBSan and TSan.

## Log

- 2026-10-27: revision 0 drafted. Decisions D1 to D8, invariants I1 to I7, gate rows T1 to T7,
  questions Q1 to Q4. No build or benchmark run (design only).
- 2026-10-27: revision 1 after the preliminary review: D9 to D11, D3 twins every record, N1,
  K1 to K8, Q5; Q1 to Q4 marked ruled.
