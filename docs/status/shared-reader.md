# shared-reader

- Design: docs/design/shared-reader.md (revision 2); reviews: `docs/reviews/shared-reader-design.md`
  (preliminary, APPROVE WITH CONDITIONS, 2026-10-21; re-check of revision 1, APPROVE WITH
  CONDITIONS, 2026-10-21, C6: SR-D15 and SR-D16 into the text before step 1's items 3 and 5;
  probes in `.scratch/review/`, kept). Concurrency tier: revision 2's new text (the write lease
  of the checkout model, D4; D12 for the write buffer) goes to the architect with
  `docs/design/write-buffer.md`.
- Branch / worktree: `design/shared-reader` from main `75143f9`, at
  `/Users/kronuz/code/Kronuz/strata-wt/shared-reader`. Not merged by the designer.
- State: designing (ready for preliminary review): revision 2's checkout model and D12.
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
  - The architect's re-check of revision 1, committed as written (`f39dfc6`).
  - Revision 2: SR-D15 (view pointer, then position with release; acquire loads), SR-D16 (publish
    when anything changed, never keyed on documents flushed), SR-D17 (test-and-test-and-set
    probe), the closing index's accepted calls, the checkout model (the owner is the write-lease
    holder; one registry per pool partition), the owner's answers to Q1 and Q2 recorded, and D12
    (what the write buffer changes when it lands).
- Doing: nothing; waiting for the architect's review of revision 2's new text with the write
  buffer.
- Next:
  1. Fold in the re-check's findings, citing their IDs.
  2. Step 1 on `phase4/shared-reader` (strata core: hazard registry, copy-on-write live bits and
     merge-job source clearing, views and the publishable rule, `retain()` and commit's order,
     non-blocking close, `publish_through`, T1, T2, T8, T9 on `document_layout`).
- Blocked on: nothing for step 1's items 1, 2, and 4 (the re-check cleared them). Items 3 and 5
  build on D4's write lease, new in revision 2, which the architect reviews with the write buffer.
  Owner questions: none open (Q1 answered (a) for step 1, superseded by D12 when the write buffer
  lands; Q2 answered, the non-blocking close).
- Findings folded in: SR-D1 to SR-D14 in revision 1; SR-D15 to SR-D17 in revision 2.
- Last verified: nothing compiled or run (design only); every `file:line` checked against main
  `75143f9` (`41fde48`'s `include/` is identical), prospect `59f36fe`, `design/durability-modes` `95357d9`, RocksDB 11.11, and Lucene
  `9983b7c` on 2026-10-21.
