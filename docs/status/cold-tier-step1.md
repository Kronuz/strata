# cold-tier step 1: record and body, D9's map points

> Status: BUILDING (2026-10-27) the developer's ledger for `docs/design/cold-tier.md` step 1,
> items 1 and 2, with D9's table and failure rules (K2).

- Design: `docs/design/cold-tier.md` revision 2 on `design/cold-tier` (strata:f5bd03d); review
  `docs/reviews/cold-tier-design.md` (preliminary, re-check 1, re-check 2). Neither is on this
  branch; read them in `strata-wt/cold-tier`.
- Branch / worktree: `phase/cold-tier-step1` from main strata:d0717a2, at
  `/Users/kronuz/code/Kronuz/strata-wt/cold-tier-step1`.
- Scope: items 1 (record, body, failed marker, compare-exchange install, owner-written facts,
  atomic counters, `options::eager_segments`) and 2 (`segment_view::seg()`), and item 3's D9
  table with its failure rules, `needs_heal()` over markers and the swallowed-round count, since
  K2 asks for them. D11's open-time name and size check and first-map checks are in because lazy
  segments need them to open at all. Out of scope: `demote()` (item 5), the twelve damage cases
  (K4), lazy as the default (item 6, waits for CT-R5), prospect's and Xapiand's call sites (K7).
- Default: `eager_segments` is on. `STRATA_EAGER_SEGMENTS=0` turns it off for a whole test run.
- State: building.
- Done:
- Next:
- Blocked on:

## Done criteria carried from the design (ticked with evidence; never edited)

- [ ] **K1.** Record and body, compare-exchange install, a loser that destroys only its own body;
  T6 over the install under TSan, with the mutants "body cleared in place", "body stored without
  compare-exchange", "loser reads the body it destroyed" and "facts written by a reader's map"
  (CT-R17).
- [ ] **K2.** D9's table in code, each map point's failure rule with a faultfs-injected test (open,
  `mmap` and check failures): no fence for host errors, damaged-and-skipped merge sources,
  commit's prepare leaving the index as `refresh()` leaves it and unfenced. Flush and merge output
  maps under faultfs `map()` failures keep the bytes and never fence (`output_map_probe` as the
  oracle). A damaged segment's first map inside commit's prepare: `corrupt_segment`, unfenced,
  `needs_heal()` true, a commit after `heal()` succeeds (CT-R19). An `EMFILE` at scheduling:
  `refresh()` succeeds, the swallowed-round count rises, the next round merges (CT-R20).
- [ ] **K4** (the part item 1 touches). A missing or resized manifest-named file fails the open (D11).
- [ ] **K7** (listing only). prospect's construction sites and Xapiand's seam, with the pin bump
  that adopts `seg()`.
- [ ] **K8** (the part this branch can show). ASan, UBSan and TSan clean, with eager on and with lazy
  forced on; CI green on every platform (the lead watches CI).

## Log

- 2026-10-27: branch from main strata:d0717a2; configured rel, asan and tsan builds under
  `.scratch/`.
