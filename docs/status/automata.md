> Status: CURRENT (2026-10-18) implementing on phase5/automata; the code is not on main.

# automata
- Design: docs/design/automata.md revision 1 at 4010392 (folds AUT-R1 to AUT-R14); review:
  docs/reviews/automata-design.md (preliminary, APPROVE WITH CONDITIONS)
- Branch / worktree: phase5/automata at /Users/kronuz/code/Kronuz/strata-wt/automata,
  rebased onto main d36be16 (past adb329e)
- State: implementing (revision 1's code on the branch; CI validating; no local build under
  the strict hold)
- Done criteria: none written yet (C1 is the lead's; the review proposes K1 to K12). Evidence
  toward the proposed ones, unticked until the lead writes them and CI is green:
  - K2: 65433fe (use-after-free fixed, rvalue decode deleted), c6e5d9b (4-byte cuts, depths
    0 to 4,096)
  - K3: 4b9bad4 (deleted-only terms under every limit); keys wait for format 13 (D8a)
  - K4: 4b9bad4 (the accessor); layers' half waits for layers on main
  - K5: c6e5d9b, 4b9bad4 (seek targets checked against every skipped term; 8 of 2,046
    entries for "?a")
  - K6: 4b9bad4 (budget permit, cancellation, pause and resume)
  - K7: 722592b, c6e5d9b, 4b9bad4 (fuzz_automata, seek targets, corrupted segments, replayed)
  - K9: 4b9bad4 (constant score counts with deletes)
  - K11: 4b9bad4 (limits.md's Phase 5 rows); parity.md's T8 rows not yet
- Done:
  - Design 7d2d30e; revision 1 4010392 (D5 contract and next, D6 seek targets, D7 options,
    D8 liveness and merge, D8a keys, D9 constant score, D11 and D12, T1 to T9, O1 to O6)
  - D1-D6 47dba32; D7-D9 ca4e6d0
  - 65433fe: the test's dangling decode fixed, decode's rvalue overload deleted (AUT-R10.1)
  - 722592b: fuzz_automata, its corpus, and its replay test (T3)
  - 4b01b9b: ledger corrected after the rebase (AUT-R14)
  - c6e5d9b: next(), seek targets, charge callbacks, uncapped k; tests for seek targets,
    4-byte cuts, depths, threads, charges (AUT-R1, R2, R6, R10)
  - 4b9bad4: expand over a reader accessor with walk_options (seeks, cancellation, pause,
    budget), liveness rule, one-map merge with a cutoff, constant score, query_budget_error;
    tests for each; corrupted-segment fuzz path; limits.md rows (AUT-R3, R5, R6, R7, R10, R11)
- Doing: waiting for CI on 4b9bad4 (nothing has compiled yet: the strict hold)
- Next:
  - Fix whatever CI finds on 4b9bad4, until all eight jobs are green
  - parity.md's T8 rows (Lucene's and Tantivy's automaton tests), a test-engineer task
  - After layers lands: the seekable term cursor in inverted_plugin::reader (AUT-R3 step 2),
    removing expand.hh's run replay
  - After format 13's keys reach the planner: D8a's keys source and its T2 row (AUT-R4)
  - After termdict's cursor::seek: seek on every dead result (D7), C3's A/B for the preset
- Blocked on:
  - Local build, tests, sanitizers: the strict gate hold (night_gate.sh n1017, until about
    07:04); CI validates meanwhile
  - Done criteria (C1, the lead)
  - termdict's additive reader::cursor::seek (O2, decided) and string-similarity's
    code-point OSA distance (O5, decided): each a branch in its own repository, outside this
    worktree; the lead assigns them
  - The lead: O4 (synonym scorer owner); PLAN.md:223-224 and 236 amended for O6;
    ARCHITECTURE.md:324 corrected to D4's simulation (AUT-R14); C3's A/Bs and C4's tail
    profile (T9) under granted holds; C6 and C7
  - The oracle (GPL side, the lead assigns): O3's vectors and the expansion mode T4 needs
    (AUT-R9)
  - Long fuzzing (T3's CPU-hour of fuzz_automata): only under a hold the lead grants
  - T6's harness (query set, engine configurations, strata_engine syntax): after revision 1
    is re-checked (AUT-R8)
- Findings folded in: AUT-R1 (D5 contract, c6e5d9b), R2 (next and seek targets, c6e5d9b,
  4b9bad4), R3 step 1 (4b9bad4), R4 (D8a, design only: waits for format 13), R5 (4b9bad4),
  R6 (c6e5d9b, 4b9bad4, limits.md), R7 (4b9bad4), R8 and R9 (design: T4, T6, T7), R10 (all
  five gaps: 65433fe, c6e5d9b, 4b9bad4), R11 (4b9bad4), R12 and R13 (design), R14 (rebase,
  ledger, T9 to T8)
- Touches existing files: CMakeLists.txt (additive: automata, expand, and fuzz_automata
  targets); docs/design/limits.md (an appended Phase 5 section)
- Last verified: nothing built locally (strict hold); CI pending on 4b9bad4 (2026-10-17)
- CI red at 4b9bad4 on all eight jobs (run 39090261627; lead, 2026-10-18): one check,
  `test/expand_test.cc:376`, `CHECK(df_errors > 0 && other == 0)`. A local diagnostic prints
  df_errors 0 and other 0: every byte flip of the term dictionary (with checksums refreshed)
  either decodes or throws corrupt_segment for another reason, and none reaches the
  "document frequency" check. So nothing escapes as the wrong exception; the test's premise
  fails, likely because the rebase onto main changed where a term's df is stored. Next: find
  where df lives now and corrupt that, or prove the df check is unreachable and say so.
