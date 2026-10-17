# format13
- Design: docs/design/format-13.md at 41730a3, revision 1 (revision 0 at 1bc0a38, amended at 96083a4, 7c2c9bd, e2ac3fe; what changed is the design's "Changes from revision 0" table); reviews: docs/reviews/format-13-design.md (preliminary, APPROVE WITH CONDITIONS, 2026-10-17, 4266374: F13-R1 to F13-R17, C1 to C6). Revision 1 also folds in the system review (main, docs/reviews/system-2026-10-17.md: SYS-1, SYS-4, SYS-6, SYS-7, SYS-9, SYS-16), the automata review (phase5/automata 9b9e151: AUT-R3, AUT-R4, AUT-R13, C4, C5), the query-gap design (design/query-gap 8a6503f, revision 0: M16 to M23), and PLAN's owner decisions at main bcf89c8 (PLAN.md:16-20, 34-41, 47-48)
- Branch / worktree: phase4/format13 at /Users/kronuz/code/Kronuz/strata-wt/format13; base 3ec4bdf. It rebases onto phase4/layers-refactor at 707066b, or main once layers lands, by the design's "The rebase onto layers"
- State: designing (ready for preliminary review)
- Done criteria (K1 to K14; K1 to K12 written by the designer from F13-R16's proposal and confirmed by the lead on 2026-10-17, K13 and K14 added by the lead after the revision 1 re-check; contributors tick, never edit):
  - [ ] K1 T1's cases pass on all eight CI jobs: the prefix and version classification (F2), every open rule of F3 to F6, each structure bit both ways and each reserved bit refused as unsupported (F4, F23), the canonical layout (F5), the free-and-absent cases (F4), the index-level and manifest cases (F2, F20), and the format-12 refusal fixtures
  - [ ] K2 Every decoder F3 adds has a fuzz_segment section-selector value and a checked-in corpus, replayed in CI under ASan and UBSan, and refresh_checksums reseals every checksum the reader checks, the header prefix included (T2)
  - [ ] K3 F22's fixtures and the format-12 refusal fixtures are pinned and byte-identical on all eight jobs, and a reordering merge's output hash is equal at reorder_threads 1 and 8 (T8)
  - [ ] K4 T9's lifecycle tests pass over format 13: Phase 3's T1 crash points, T2's digest schedules, and T5's disk-full rows, keyed, keyless, and with the document profile
  - [ ] K5 The full suite completes under ASan and UBSan, and under TSan, on CI at every commit a criterion cites (C2)
  - [ ] K6 media_test's sweep covers bytes 0 to 15 of every segment: a transient flip is retried and healed, a sticky one fails as damage (F2)
  - [ ] K7 Each decision's tests cite its F number in the commit that adds them
  - [ ] K8 Step 7c changes the digest's value in exactly one commit (F21), which re-pins digest.txt and runs T2's schedules; every other commit leaves digest.txt unchanged
  - [ ] K9 A fresh open touches the header page and one run of pages ending at the footer, and no byte of lengths (T7)
  - [ ] K10 The key path (F8): stage 1 and the inverted builder refuse a key-prefixed term; check() finds none in the dictionary; a term query on a key and the keys reader's scan from any lower bound agree with a brute-force model over keyed and keyless indexes; verify and parity stay clean
  - [ ] K11 F20's checks: a mask bit the binary cannot read and a version range beyond it refuse open and install with unsupported_index; a segment outside the mask or the range is corruption; a version-1 manifest is unsupported_index
  - [ ] K12 The freeze evidence of F1 (items 2 to 6) is recorded in docs/perf/ as candidate tables under holds the lead grants (C3, SYS-6), before the full review of the freeze candidate
  - [ ] K13 F13-R18 is fixed in the rebase plan before 7c-4, and each F13-R25 item is in the design before the 7c commit it names
  - [ ] K14 Points take up to 16 bytes per dimension with the mechanism stated and tested (F13-R22), and the freeze report's baseline raw files are copied into docs/perf/ (C9)
- Done:
  - Design revision 0 (F1 to F22, T1 to T11): 1bc0a38.
  - Code on main's segment, written before SYS-1's rebase ruling: F2 to F6 d1da0c9; F7 96083a4; F8 360dc7f and 7c2c9bd; F9 e2ac3fe; F22's tools e779835. The design's rebase table says which survive as they are (keys.hh, format.hh as a starting point, the documentation), which change, and which layers' steps replace.
  - Preliminary review: 4266374 (APPROVE WITH CONDITIONS).
  - Design revision 1: 41730a3. Every finding answered by ID; the owner's references and reservations, with one adding rule (F23); the freeze criteria, carried rows included (F1); one selection over F10 to F13 against the postings budget; the keys reader's lower-bound scan; the rebase plan, one line per commit, and step 7c's seven commits.
- Doing: nothing; revision 1 waits for the architect's re-check
- Next:
  1. The architect's re-check of revision 1, appended to docs/reviews/format-13-design.md; fold its findings in, citing their IDs.
  2. The owner's Q5 and Q6 (the design's "Questions for the owner"); neither blocks a step.
  3. When the host is free and the lead says so: the design's first steps 1 to 3 (codec units with unit tests, the tools, termdict's options), staying out of reader.hh, writer.hh, index.hh, merge.hh, search.hh, searcher.hh, and fuzz/fuzz_segment.cc until the rebase (C4).
  4. After layers lands and the re-check passes: the rebase and steps 7c-1 to 7c-7 in the design's order (developer).
  5. After 7c-3: the joint selection's projection and keysim's run on a branch build's reordered 5M merge, under a hold the lead grants.
- Blocked on:
  1. Step 7c, the code in reader.hh, writer.hh, index.hh, and merge.hh: layers landing, or the lead's word to rebase onto 707066b first; and revision 1's re-check of F13-R1 to R5 and R11 (the review's required changes).
  2. F10's code: the re-check of F13-R7 and F13-R8.
  3. For the lead: confirm or edit K1 to K12 (C1); K5 needs the ASan job to finish within its limit on main (F13-R16: it stopped at its 60-minute limit at e2ac3fe and passed in 49 min 15 s at 4266374); keybench on main for T5's baseline (F22); whether poststats also lands on main.
  4. For the owner: Q5 (a structure bit for R19's continuation), Q6 (bytes per point dimension).
- Findings folded in: F13-R1 to F13-R17, C1 (K1 to K12 written for the lead), C6, Q1 to Q4 as the owner answered them, SYS-1, SYS-4, SYS-6, SYS-7, SYS-9, SYS-16, the owner's key-scan ruling (automata C5), AUT-R3, AUT-R4, AUT-R13 with C4, and the query-gap design's M16 to M23: design revision 1, 41730a3. Their code lands with step 7c and the first steps.
- Last verified: 2026-10-17. CI run 39069758885 at 4266374 (the code of e779835): linux-x86_64-clang, linux-x86_64-gcc, linux-arm64-gcc, linux-asan-ubsan (49 min 15 s), and linux-tsan pass; macos-x86_64, macos-arm64, and freebsd-x86_64 fail to build on bisection.hh:541's std::jthread, pre-existing at this branch's base 3ec4bdf and fixed on main by adb329e, which the rebase brings. No local build or test since e2ac3fe (ctest 34/34 then): the host is under a strict hold until about 01:17 (quiet check exits 1), and revision 1 changes documentation only. Revision 1's one computation, format 13's header-prefix CRC-32C (0x4ff2df7b), came from a pure-Python CRC-32C checked against the standard check value.
