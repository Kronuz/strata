# shared-reader

- Design: docs/design/shared-reader.md (revision 1); reviews: `docs/reviews/shared-reader-design.md`
  (preliminary, APPROVE WITH CONDITIONS, 2026-10-21; probes in `.scratch/review/`, kept).
  Concurrency tier: a scoped architect re-check of revision 1 comes before step 1 is built.
- Branch / worktree: `design/shared-reader` from main `75143f9`, at
  `/Users/kronuz/code/Kronuz/strata-wt/shared-reader`. Not merged by the designer.
- State: designing (ready for preliminary review)
- Done criteria (from the review's C1, written at the lead's direction; contributors tick, never
  edit):
  - [ ] K1 `hazard.hh` with the litmus (SR-D4, T8), `parking` and `drift`, and the claim and
    release tests, clean under TSan, ASan, and UBSan.
  - [ ] K2 T1 at its stated counts on Linux x86_64, macOS, and FreeBSD, plus the stuck-reader
    test (SR-D7, T9).
  - [ ] K3 T2 (a) and (b) with every mutant of T2 and SR-D4 killed by a named test.
  - [ ] K4 `install()`, `heal()`, `close()`, destruction, and move-assignment under live guards
    (SR-D1, SR-D2, SR-D10).
  - [ ] K5 Retention timing unchanged with an idle handle, and no truncation under a mapping, on
    faultfs with mapping on and on the POSIX IO (SR-D6).
  - [ ] K6 T4's allocation test against the restated target (SR-D12).
  - [ ] K7 The owner-thread destruction assertion for `open_segment` (SR-D5).
  - [ ] K8 CI green on every platform.
- Done:
  - Draft revision 0: D1 to D11, T1 to T7, Q1 and Q2, first buildable step (`f466701`).
  - Revision 1: SR-D1 to SR-D14 answered (the design's "Changes in revision 1"); Q2 settled by
    SR-D11's non-blocking close unless the owner overrides; T8 and T9 added; the review file
    committed with it.
- Doing: nothing; waiting for the architect's scoped re-check of revision 1.
- Next:
  1. Fold in the re-check's findings, citing their IDs.
  2. Step 1 on `phase4/shared-reader` (strata core: hazard registry, copy-on-write live bits and
     merge-job source clearing, views and the publishable rule, `retain()` and commit's order,
     non-blocking close, `publish_through`, T1, T2, T8, T9 on `document_layout`).
- Blocked on: the architect's scoped re-check (concurrency). Q1 (visibility of unrefreshed
  writes) stays open for the owner and blocks nothing in step 1; an owner answer to Q2 other than
  SR-D11's would change D11's close and destructor rules.
- Findings folded in: SR-D1 to SR-D14 in revision 1.
- Last verified: nothing compiled or run (design only); every `file:line` checked against main
  `75143f9`, prospect `59f36fe`, `design/durability-modes` `95357d9`, RocksDB 11.11, and Lucene
  `9983b7c` on 2026-10-21.
