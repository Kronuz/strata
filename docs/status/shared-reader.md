> Status: CURRENT (2026-10-26) revision 3; the views step implements it (landed strata:23e78bb).

# shared-reader

- Design: docs/design/shared-reader.md (revision 3); reviews: `docs/reviews/shared-reader-design.md`
  (preliminary, APPROVE WITH CONDITIONS, 2026-10-21; re-check of revision 1, APPROVE WITH
  CONDITIONS, 2026-10-21, C6; review of revision 2, APPROVE WITH CONDITIONS, 2026-10-21, C7 and
  C8; probes in `.scratch/review/`, kept). Revision 3 is C7's text for SR-D19 and SR-D20, as the
  views code review (`docs/reviews/shared-reader-views-code.md` on `phase4/shared-reader-views`,
  `6e8c1a5`) ruled; per C7 the code reviewer confirms it, no architect re-check.
- Branch / worktree: `design/shared-reader` from main `75143f9`, at
  `/Users/kronuz/code/Kronuz/strata-wt/shared-reader`. Not merged by the designer.
- State: designing (ready for preliminary review): revision 3, SR-D19 and SR-D20 for C7.
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
  - The architect's review of revision 2, committed as written (`e9c823e`).
  - Revision 3 (C7): the seven decisions of the views code review's ruling, each checked against
    `phase4/shared-reader-views` at `6e8c1a5`:
    1. Leaf check is "some lease on this thread" (D4, SR-D19, CR5): `owner.hh:57-61`, header
       `:8-11`.
    2. Reads outside the lease, the holder's and unchecked (D2, SR-D19, SR-D20, CR5):
       `index.hh:1216`, `:1227`, `:1266-1289` (`get`, `scan`), `:856` (`segments()`), `:1050`
       (`views_held()`) take no scope; the holder-only contract is `:1256-1259`.
    3. Scopes nest as one hold; a moved lease starts free (D4, D11, SR-D19): `owner.hh:102-123`
       (outermost compare-exchange `:110`, handoff `:121`), `:72-82`; move-assignment destroys then
       constructs, `index.hh:540-545`.
    4. Release builds: the host's serialization orders holders (D3, D4, SR-D20, CR6): no token
       without `STRATA_LEASE_CHECKS`, `owner.hh:19-25`, `:97-99`; the layout switch is
       `index.hh:3551` (`write_lease` as a member).
    5. A marked pool thread that takes a lease is owner work (D4, SR-D19): `owner.hh:11`, `:60`;
       the mark `pool_executor.hh:71-72`.
    6. A rejected transaction moves the applied position and publishes (D1, CR1): see Blocked on.
    7. D2 and D9 "under the lease"; each holder rechecks after taking it (SR-D20):
       `publish_through`, `index.hh:1032-1041`; `reader()`, `:1018-1030`. Entry scopes at
       `index.hh:525`, `:552`, `:577`, `:603`, `:633`, `:683`, `:695`, `:705`, `:712`, `:720`,
       `:731`, `:750`, `:767`, `:801`, `:1021`, `:1036`, `:1115`, `:1127`, `:1142`, `:3227`.
- Doing: nothing; revision 3 waits for the code reviewer's confirmation (C7).
- Next:
  1. The code reviewer confirms revision 3 at file:line (C7); fold in any finding, citing its ID.
  2. The lead: K7's wording follows SR-D19 ("under the write lease"), per C7.
  3. Step 1 on `phase4/shared-reader` (strata core: hazard registry, copy-on-write live bits and
     merge-job source clearing, views and the publishable rule, `retain()` and commit's order,
     non-blocking close, `publish_through`, T1, T2, T8, T9 on `document_layout`).
- Blocked on: the code and the ruling disagree in two places, at `6e8c1a5`; revision 3 states
  the ruling, and the code owes the fix (not chosen here):
  - CR1, behavior: `apply()` calls `changed()` only for an accepted transaction
    (`index.hh:673`) but moves the applied position for any evaluated verdict (`:676`), and
    `changed()`'s comment lists accepted transactions only (`:1762`). D1 (revision 3) says a
    rejected transaction publishes; the views branch's CR1 fix makes the code agree.
  - CR6, comment: `deleted_bits.hh:4-6` says the lease's handoff orders holders; D3 and D4 say
    the host's serialization does in release builds. No note beside `STRATA_LEASE_CHECKS`
    (`owner.hh:19-25`) says all translation units use one setting.
  Owner questions: none open (Q1 answered (a) for step 1, superseded by D12 when the write buffer
  lands; Q2 answered, the non-blocking close).
- Findings folded in: SR-D1 to SR-D14 in revision 1; SR-D15 to SR-D17 in revision 2; SR-D19 and
  SR-D20 (C7) and the views code review's CR1, CR5, CR6 in revision 3.
- Last verified: nothing compiled or run (design only). Revision 3's citations in this ledger
  checked against `phase4/shared-reader-views` `6e8c1a5` on 2026-10-26. Earlier: every
  `file:line` checked against main `75143f9` (`41fde48`'s `include/` is identical), prospect `59f36fe`, `design/durability-modes` `95357d9`, RocksDB 11.11, and Lucene
  `9983b7c` on 2026-10-21.
