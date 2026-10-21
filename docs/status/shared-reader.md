# shared-reader

- Design: docs/design/shared-reader.md (revision 0); reviews: none yet. Concurrency tier: the
  architect's preliminary review comes before any build.
- Branch / worktree: `design/shared-reader` from main `75143f9`, at
  `/Users/kronuz/code/Kronuz/strata-wt/shared-reader`. Not merged by the designer.
- State: designing (ready for preliminary review)
- Done criteria (set by the lead; contributors tick, never edit):
  - (none yet)
- Done:
  - Draft revision 0: D1 to D11, T1 to T7, Q1 and Q2, first buildable step (`f466701`).
- Doing: nothing; waiting for the architect's preliminary review.
- Next:
  1. Fold in the architect's findings (cite their IDs) and the owner's answers to Q1 and Q2.
  2. After review: step 1 on `phase4/shared-reader` (strata core: hazard registry, copy-on-write
     live bits, views, publication, `retain()`, T1 and T2 on `document_layout`).
- Blocked on: the architect's preliminary review (concurrency); the owner for Q1 (visibility of
  unrefreshed writes) and Q2 (`close()` with reads in flight). Neither blocks step 1 if the
  recommendations stand.
- Findings folded in: none yet.
- Last verified: nothing compiled or run (design only); every `file:line` checked against main
  `75143f9`, prospect `59f36fe`, `design/durability-modes` `95357d9`, RocksDB 11.11, and Lucene
  `9983b7c` on 2026-10-21.
