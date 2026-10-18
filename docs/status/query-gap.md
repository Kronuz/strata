# query-gap

- Design: docs/design/query-gap.md, revision 1 at c2ca6b4, revision 2 in progress; revision 0 at d8a660c, amended at 4fee3e1 (a489a72 and 8a6503f before the rebase onto main ff4320b on 2026-10-17); reviews: docs/reviews/query-gap-design.md (preliminary, APPROVE WITH CONDITIONS, 2026-10-17, 60b1d03, formerly dec5f4d: QG-R1 to QG-R20, C1 to C4; its re-check of revision 1 at 643fdb9, APPROVE WITH CONDITIONS: QG-R21 to QG-R26, C5 to C7, no blocking items)
- Branch / worktree: design/query-gap at /Users/kronuz/code/Kronuz/strata-wt/query-gap
- State: designing (folding in the re-check: QG-R21 to QG-R26, C5 to C7)
- Done criteria (K1 to K12, drafted by the designer from QG-R19 and the design's T1 to T9 for the lead to confirm or edit; contributors tick, never edit):
  - [ ] K1 E1's tooling is reviewed and on the experiments' base: the STRATA_COUNTERS build (off by default, never in an A/B binary) counting with kronuz::metrics::Counter what the design's E1 lists; allocation counts through the Kronuz/allocators extension (additive, both tracking modes tested, its CI green, the cpp-libraries entry updated); strata_engine bench's counter columns; poststats' frontier-length histograms per level
  - [ ] K2 T1: docs/perf/query-gap-attribution.md splits every row above 1.0x by E0's upper bound into named parts with their counters, profile shares, mechanisms, and ceilings, states the format-dependent part with E6's and E7's unit costs, and gives each of H1 to H20 its verdict by the design's rule, with raw files and their SHA-256s
  - [ ] K3 E3 runs as a mode of the real scorers (a bound source and top_k's floor in the counting build, no second traversal), calibrated against E2's and E4a's A/Bs before it decides M19; every projection it reports carries its calibrated range
  - [ ] K4 T4: before format 13's freeze, docs/perf/query-gap-format13.md holds each of M16, M17, and M19 to M23 with its table, its bytes against D15's budget from strata_engine sections and poststats, and a keep or drop line; M18 is recorded as out; each kept item's text goes to format 13's contributor as "What format 13 must record" states
  - [ ] K5 If M17 is kept: its property test passes, and fails for both mutants that drop the correction; check() verifies domination and argmax-first; E5's multi-segment index (segments of different averages, one with STRATA_DELETE_EVERY) passes verify, dump identity, and parity; the bm25_xapian rows meet D14's item 2
  - [ ] K6 If M23 is kept: the 64-bit sum check and its 25-bit lane form, a unit test of a block whose gaps wrap by 2^32, that block in fuzz_segment's corpus, and E6's timing against check_increasing
  - [ ] K7 E9a runs first after E0 and E1, with M24's A/B recorded on union TOP_100_COUNT, and M4 (E9b) is measured against it on unions
  - [ ] K8 Every kept item meets D14: its target rows named in its docs/perf/ entry before its A/B; the win on the reordered merged index; no row above 1.03 on either index, with bm25_xapian rows for weighting and bound items; reruns final; the A/B repeated on the FreeBSD VM and the host's macOS with no row above 1.03; SIMD items marked provisional until arm64 runs
  - [ ] K9 Every kept item passes T2: verify at zero mismatches on the four indexes, dump identity against E0's references, and parity at 1, 4, and 16 segments and reordered at 419 of 419; only M13 re-pins dump, once
  - [ ] K10 D14's cumulative guard holds at every third kept item and before the freeze: the tip against the base on every row at an upper bound of at most 1.03, and for writer-side items indexing and merge time likewise
  - [ ] K11 Every kept item meets T6 (allocations per query by class at 1 and 16 segments not above before and not growing with matches; serving memory; writer-side time; disk per section inside D15) and T9 (ASan and UBSan; TESTING.md's coverage targets; all eight CI jobs at its commit, SIMD paths built on linux-arm64-gcc and macos-arm64; any decoder it changes fuzzed through format 13's T2)
  - [ ] K12 Every losing item is recorded in docs/perf/ with its numbers and removed (PLAN.md:383-387), and every measurement names its raw files with their paths and SHA-256s (C4)
  - [ ] K13 T9 holds for every kept item (ASan and UBSan, coverage, all eight CI jobs, fuzzing through format 13's T2), and each kept item is measured on Linux, FreeBSD, and macOS, with SIMD items provisional until arm64 confirms them
  - Confirmed by the lead on 2026-10-17 (K1 to K12 as written; K13 added for the cross-platform rule).
- Done:
  - Design revision 0: d8a660c, amended at 4fee3e1 (the indexing gap's place in the queue).
  - The preliminary review: 60b1d03 (APPROVE WITH CONDITIONS); the owner's Q1 to Q3, recorded by the lead: d374a6d.
  - Rebased onto main ff4320b, which had moved by docs only (PLAN, calibration, the index); force-pushed with lease.
  - Design revision 1: c2ca6b4. Every finding answered by ID (the design's "Changes from revision 0"); what format 13 must record, per candidate with its F numbers (F13-R21's preset record for M17; M18 out); the byte budget (D15); the verdict rule and confirming levels for H1 to H20 (H20 new, for the before-merging rows); D14 rewritten per class with the cumulative guard; D16 for layers' step-6 A/B; M24, the two-pass plan, as E9a; the queue E0, E1, E9a, the freeze set, then the format-free items; T9; Q4.
  - The lead confirmed K1 to K12 and added K13: cad1ddb. The architect's re-check: 643fdb9.
  - QG-R22, first step 1 (this commit): `bench/quick/ab.py` scores per set as before (same rows, same seed, so recorded A/Bs rescore identically) and adds a table per tag over both sets, classes and strata alike (a tag with a colon is a row beside its class: union:num_tokens_3, union:num_tokens_>3, term:frequent), each row's bootstrap seeded by its command and tag; queries are keyed as `strata_engine bench` prints them (`strata_engine.cc:552-555`), and queries without tags are counted, never dropped silently. `ab.sh` copies the query files it ran into OUT. `bench/quick/ab_test.py` over `test/fixtures/ab` (two rounds, two commands, both sets, exact per-query ratios, a mismatch, an untagged query, an escaped key) runs as ctest `ab_scorer` wherever CMake finds Python 3.
- Doing: design revision 2: QG-R21, QG-R23 to QG-R26, and C5 to C7 into docs/design/query-gap.md; then CI on the QG-R22 commit
- Next:
  1. The architect's re-check of revision 1, appended to docs/reviews/query-gap-design.md; fold its findings in, citing their IDs.
  2. The lead confirms or edits K1 to K12.
  3. When the host frees and the lead says so: the design's first steps on the base the lead names (the layers tip, "For the lead", item 1), each under code review.
  4. E0 (the lead's second night), then E1, E9a, the freeze set (E3, E6, E7, E2, E5, E4, and E10 when E4 calls for it), and the format-free items, under holds the lead grants (performance-engineer).
  5. "What format 13 must record" to format 13's contributor; Q4 to the owner once the before-merging rows are measured.
- Blocked on: nothing for revision 1. The experiments wait on the lead (the base, E0, holds) and on the re-check; Q4 waits on the before-merging rows' measurement.
- Findings folded in: QG-R1 to QG-R20 and C1 to C4 (design revision 1, c2ca6b4); Q1 to Q3 as decided (d374a6d); QG-R22 (the per-class ab.py, this commit).
- Last verified: 2026-10-17, QG-R22's commit: `ruff format`, `ruff check`, and `pyrefly check --preset strict` (one thread, nice -n 19) clean on ab.py and ab_test.py; the test not run locally (`quiet check` exits 1: strata-trials, `night_gate.sh n1017`, strict until 07:04), so CI runs it. Revision 1: nothing built, tested, or timed (design only; `quiet check` exited 1 under the strict hold strata-trials, `night_gate.sh n1017`, until about 01:17). Code citations checked against main ff4320b, whose code is 5691ec7's; format 13's against origin/phase4/format13 41730a3 and its review at bae7fc1; layers' against origin/phase4/layers-refactor 1b6a148; the libraries at allocators 2ebd873, metrics c975765, completion 5251cf9, bitpack abf8972, docset 0859e43. The format13, refactor, and main worktrees match origin (no unpushed commits). Recorded-result arithmetic only: the review's scripts (n1means.py, n1split.py, twopass.py, headroom.py, n2rows.py, stretch.py, pisa.py) rerun at nice -n 19 over ~/.scratch/n1 and ~/.scratch/quick-n2.md reproduce the review's figures; the per-row needed medians are ratios of the committed n1 table (phase-3-ratchets.md:133-172).

## Owner decisions (2026-10-17, by the lead under the owner's standing permission)

- Q1: (c) each index's configured scoring preset orders its frontiers, defaulting to bm25,
  recorded per segment, with M17 argmax-first (QG-R1).
- Q2: (b) conditional, expected to close as (a): a stored partition bound only if a variant
  fits the byte budget (QG-R6), stored as g1 rounded up, used only to skip.
- Q3: (a) P5's next-word bytes count in the disk row.
- The experiment queue follows QG-R14: E9 (two-pass counting for 3+-clause unions) first after
  E0 and E1, since the record projects strata's own time to 0.26 to 0.37x there.
