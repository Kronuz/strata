# cold-tier

> Status: CURRENT (2026-10-30) the designer's ledger for `docs/design/cold-tier.md`.

- Design: `docs/design/cold-tier.md` revision 3 (hot, warm, cold and remote tiers; the
  per-partition catalog; demotion by unmapped twins; D9 to D11 for step 1; warm under `defer_io`
  by `promote()` and `verdict::map_owed`, CT-R21).
- Review: `docs/reviews/cold-tier-design.md` (preliminary, APPROVE WITH CONDITIONS; re-check 1,
  GO WITH CONDITIONS for step 1; re-check 2 closed C5 and C6, again at `main` `c82ac9f`, which
  opened CT-R21 and made item 5 no-go until the design states it).
- Branch / worktree: `design/cold-tier` from main `98ce743`, at
  `/Users/kronuz/code/Kronuz/strata-wt/cold-tier`; citations checked at main `c82ac9f`.
- State: designing (revision 3 ready for preliminary review of CT-R21).
- Step 1 on `main` (developer's ledger `docs/status/cold-tier-step1.md`): items 1 to 3 and D11's
  open check (strata:d0ab622, merged strata:66ddb16; code review `cold-tier-step1-code.md`, its
  fixes strata:444c26f); CT-R5's rollback (strata:eb87572, with strata:d3a59ee for CT-R5a).
- Done:
  - strata:4bc1a06 revision 0.
  - strata:8459c7b the architect's preliminary review.
  - strata:e6bdf25 revision 1: folds in CT-R1 (D3), CT-R2 (D2, D9), CT-R3 (D10), CT-R14 (D11,
    Q3) and CT-R16 (the criteria below); records CT-R9 as N1; lists CT-R4, R6 to R8, R10 to R13 and R15 as open
    for step 2 and step 3; adds Q5. The architect's probes rerun under a light hold with output
    identical to theirs (`twin_race`, `quiescence_probe`, `apply_throw_probe`,
    `warm_bytes_probe`). No build or benchmark (design only).
  - strata:46ce154 the architect's re-check 1.
  - strata:e1bfb7f revision 2: CT-R2's output maps and `install()`'s copies in D9, the `:4032`
    citation; CT-R17 owner-written facts (D2); CT-R18 the twin's fields (D3); CT-R19 commit with a
    marked record, `needs_heal()`, the prepare's skips (D9, D11); CT-R20 the swallowed-round scope
    and count (D9); the marker's lifetime (D2); the prepare's outcome and place (D9); Q5 ruled (a)
    with its corrections and T2w; C6 in K1, K2, K5. Text only, no build.
  - strata:e44d98b the architect's re-check 2 at `main` `c82ac9f` (C5, C6 closed; CT-R21).
  - Revision 3 (this ledger's commit): CT-R21 in D3, D9, D10, I8, T1, T2, T5, T6 (`promote()`,
    the owed flag, `verdict::map_owed` under `defer_io`); every `index.hh` citation at `c82ac9f`,
    by symbol where step 1 rewrote it; T5 on `work_pool_executor`; D10's close wording against
    `defer-io.md`'s depth-2 waits; step 1's list marks items 1 to 3 on `main`; K3 and K5 below.
    Text only, no build.
- Next: a preliminary review of revision 3's CT-R21 text; then item 5 (`demote()`, `promote()`,
  `map_owed`) can start against K5. K4 (the twelve damage cases) can start now.
- When the cold tier lands (owner, 2026-10-28): the hybrid-search go/no-go runs next. Rerun C1
  with strata included and 1,000-vector tenants, then whatever experiments show whether
  optimization can open a clear lead in idle cost and p99 over pgvector, Qdrant, LanceDB and
  Elasticsearch. The outcome is go (hybrid steps 1 to 4, `docs/design/hybrid-search.md` on
  `design/hybrid`) or dropping "best at AI retrieval" for good.
- Blocked on:
  - Item 6 (lazy as the default): CT-R21's tests (item 5, K5), K4's twelve cases, K7's two pin
    bumps, and K3's rechecker run.
  - For the lead, outside this branch: `defer-io.md` "Not covered" still says a segment not yet
    mapped maps inside `apply`, which `88b7713` made untrue (suggested: "`defer_io` forces
    `eager_segments`, so `apply` finds every segment mapped; a replace-by-key read of a mapped
    segment can page-fault"); `index.hh:519-521` still says lazy waits "until apply() rolls back"
    (suggested: "until K4, K7 and item 5's CT-R21 tests are in (cold-tier step 1, item 6)").
    Item 5's branch updates both when `map_owed` lands.
  - The lead's restatement of T1 and T2 (CT-R9, CT-R11) before they gate; Q5's (b) decided on
    T2w's numbers before step 2.

## Done criteria for step 1

Each with its evidence on the branch, re-checked before landing.

- **K1.** Record and body, compare-exchange install, a loser that destroys only its own body;
  T6 over the install under TSan, with the mutants "body cleared in place", "body stored without
  compare-exchange", "loser reads the body it destroyed" and "facts written by a reader's map"
  (CT-R17).
- **K2.** D9's table in code, each map point's failure rule with a faultfs-injected test (open,
  `mmap` and check failures): no fence for host errors, damaged-and-skipped merge sources,
  commit's prepare leaving the index as `refresh()` leaves it and unfenced. Flush and merge output
  maps under faultfs `map()` failures keep the bytes and never fence (`output_map_probe` as the
  oracle). A damaged segment's first map inside commit's prepare: `corrupt_segment`, unfenced,
  `needs_heal()` true, a commit after `heal()` succeeds (CT-R19). An `EMFILE` at scheduling:
  `refresh()` succeeds, the swallowed-round count rises, the next round merges (CT-R20).
- **K3.** CT-R5's rollback on `main`, `apply_throw_probe` as its test, failing before and passing
  after. Satisfied in code (strata:eb87572, strata:d3a59ee; `apply()` `index.hh:697`, `rollback()`,
  `reserve_undo`, `test/apply_atomic_test.cc`'s allocation sweep); still owed: a rechecker with a
  build runs the probe, the sweep and the revert check on `main` (no re-check of CT-R5a's fix is on
  file).
- **K4.** The twelve damage cases with lazy segments; a missing or resized manifest-named file
  fails the open (D11).
- **K5.** Item 5: `demote()`, `promote()` and `map_owed` (D3, D10, I5, I8). Each rule has a test
  and a mutant the test kills, built and run like step 1's (`.scratch/mutants.py`):
  1. Warm's predicate: `demote()` returns false and changes nothing (the same record pointers,
     view and digest) with a buffered record, a pending merge, an unsynced record, or closing.
     Mutants: drop each clause (four).
  2. Every record twinned: after `demote()` and `free_views()`, with a reader that held the old view
     and maps late, no record in `segs_` has a body (the `twin_race` shape, T6). Mutant: twin only
     bodied records.
  3. The twin's fields: mutants "twin drops `deletes_dirty`" (oracle: commit, reopen, the delete
     holds; CT-R18), "twin drops the marker" (`needs_heal()` false after `demote()`), "twin drops
     the bits" (a deleted document readable by `get`).
  4. Forced publication and freeing: after the last guard releases, the index holds 0 maps.
     Mutants: skip the forced publication (`publish_point()` keeps the old view); skip
     `free_views()`.
  5. Invisible to reads (I5): position and digest equal before `demote()`, after it, and after
     `promote()`, with an uncommitted delete. Mutant: build the twin's bits from the live-docs file.
  6. `map_owed` (CT-R21, I8): on a `defer_io` index, `demote()` then an `apply` that replaces by key,
     and one that deletes by docid, answer `map_owed`, apply nothing, and leave
     `faultfs::filesystem::calls()` unchanged; `promote()`, then the same transaction at the same
     position is accepted with `calls()` unchanged. Mutants: drop the check in `apply` (calls rise;
     the debug build's `no_input_map_scope` assertion fires); `demote()` never sets the flag
     (same); answer `map_owed` without `defer_io` (`default_path_unchanged` fails).
  7. The answer ends: after a completed `promote()`, no `apply` answers `map_owed`. Mutant:
     `promote()` leaves the flag set. A faultfs `map()` failure (`ENOMEM`) inside `promote()`
     rethrows unchanged, unfenced, with the flag set; the next `apply` answers `map_owed` and a
     second `promote()` completes. Mutant: `promote()` clears the flag on a host error (the next
     `apply` maps: calls rise). A record whose first map fails its checks gets the marker,
     `promote()` completes, and an `apply` that reaches it throws `corrupt_segment`, rolled back,
     with `calls()` unchanged.
  8. T6's mutant "close with a posted delete" (D10's cold predicate, carried for step 2's sweep).
  9. T5 on one index with readers holding guards across `demote()` and `promote()` and releasing on
     other threads, an index idle after posting deletes, and the same run under `defer_io` answering
     `map_owed` by `promote()`; zero ASan, UBSan and TSan reports.
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
- 2026-10-27: revision 2 after re-check 1: C5's text (CT-R2 outputs, CT-R17 to CT-R20, the
  marker's lifetime, the prepare's wording, Q5) and C6's tests in K1, K2, K5.
- 2026-10-30: revision 3 after re-check 2 at `c82ac9f`: CT-R21 decided (warm admits `defer_io`;
  `promote()` and `map_owed`, with step 2's catalog promoting at the wake), citations refreshed, K3
  and K5 restated. No owner question. No build or benchmark (design only).
