# automata
- Design: docs/design/automata.md at 7d2d30e (rebased twin of fa544a6); review:
  docs/reviews/automata-design.md (preliminary, APPROVE WITH CONDITIONS, AUT-R1 to AUT-R14)
- Branch / worktree: phase5/automata at /Users/kronuz/code/Kronuz/strata-wt/automata,
  rebased onto main d36be16 (past adb329e)
- State: implementing (folding AUT-R1 to AUT-R14; revision 1 of the design)
- Done criteria: none written yet (C1 is the lead's; the review proposes K1 to K12)
- Done:
  - Design 7d2d30e (D1-D12, T1-T8, O1-O5)
  - D1-D6 47dba32 (automata.hh, automata_reference.hh, automata_test.cc)
  - D7-D9 ca4e6d0 (expand.hh, expand_test.cc)
  - 65433fe: the test's dangling decode fixed, decode's rvalue overload deleted (AUT-R10.1)
  - 722592b: fuzz_automata, its 25-input corpus, and its replay test (T3, part of AUT-R10)
- Doing: revision 1 of the design, then AUT-R2's `next` and seek targets
- Next: design revision 1 (AUT-R1, R2, R4 to R9, R11 to R14); `next` and seek targets (AUT-R2);
  expand.hh over an inverted-reader accessor (AUT-R3 step 1); the liveness rule and k-way merge
  (AUT-R5); uncapped k and the budget permit (AUT-R6); cancellation (AUT-R7); constant score
  (AUT-R11); AUT-R10's tests (4-byte cuts, depth invariance, 2 KiB terms, corrupt segments,
  threads); termdict's cursor::seek and string-similarity's distance on their own branches
- Blocked on:
  - Local build and tests: the strict gate hold (night_gate.sh n1017, until about 07:04); CI
    validates meanwhile
  - Done criteria (C1, the lead)
- Findings folded in: AUT-R10 item 1 (65433fe); AUT-R14 rebase and ledger (this commit)
- Touches existing files: CMakeLists.txt (additive: automata, expand, and fuzz_automata targets)
- Last verified: nothing built locally (strict hold); CI pending on the rebased branch (2026-10-17)
