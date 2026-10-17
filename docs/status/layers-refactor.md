# layers-refactor
- Design: docs/design/layers.md at 0401904 (revision 2 plus P1-R4 (d)'s order source); reviews: docs/reviews/layers-design.md (APPROVE WITH CONDITIONS; revision checks 1 and 2 APPROVE), docs/reviews/reordering-design.md (P1-R4 (d); P1-R17: P1 lands ahead of step 5)
- Branch / worktree: phase4/layers-refactor at /Users/kronuz/code/Kronuz/strata-wt/refactor
- State: implementing (steps 0-6 written; waiting on CI, see Blocked on)
- Done:
  - Steps 0 to 4b rebased onto main 3ec4bdf (P1): 3e3fa1e (0), 8012a11 (1), 8e32985 (2), 3c4ab6f (3), 93b647b (perf notes), bed8d4a (4a), 8f7ebda (4b). The zstd pin commit dropped: main carries it. Resolutions keep main's P1 behavior: `scratch`/`string_scratch` in section_io.hh; core_segment on docids::reader (permuted kind, `docid_ordered`), region CRCs moved into spans and the rest dropped; `corrupt()` throughout; the stored reader carries main's frame API; `filter8_view`; segment_writer's `permuted` and `docids::encode`.
  - Step 5 (L6, M2): d42170e (5a, iterators on the inverted reader), 8f304ca (5b, keys reader find/scan with call-time deps, L3; lookup_key through it), 6181767 (5c, merge_segments as the core's plan plus inverted/keys/stored merges in layout order; the old merge kept as test/merge_oracle.hh; merge_oracle_test; digest unchanged across force_merge), 1f7dbe8 (5d, order_source<Layout>, P1-R4 (d)).
  - Step 6 (L11): 894c04a. basic_segment is core()/get<P>()/deps<P>()/holds<P>()/verify_all(); every forwarding member removed; scorers, iterators, and statistics take the inverted reader.
- Doing: nothing; waiting on CI and on the items below
- Next:
  1. CI green on every platform once main's jthread fix is rebased in (Blocked on 1).
  2. The measured rows, which this branch cannot claim (PLAN: branches make functional claims only): step 5's merge rows (timed forced merge of the 5M index, output hash equal, T3 before and after), step 6's full L15 plan against the recorded Phase 3 commit, both on the gate VM and FreeBSD.
  3. Full review of steps 0-6 (design plus code), then landing; at landing, delete test/merge_oracle.hh and merge_oracle_test (L6: kept until step 5 lands).
  4. Step 7 (format 13 on the plugins) after the format-13 design clears review (Blocked on 2).
- Blocked on:
  1. CI: main's include/strata/bisection.hh:541 uses std::jthread, which the macOS and FreeBSD libc++ lack, so macos-x86_64, macos-arm64, and freebsd-x86_64 fail to build on main and on this branch alike (main run 39056965273, branch run 39056226632). Not this branch's code; the lead fixes main, then this branch rebases.
  2. Step 7: the format-13 design (docs/design/format-13.md, lead's workstream phase4/format13) and its review; step 7's keys section, file order, structure flags, split digest, and manifest plugin set all wait on it.
- Deviations to review: keys_plugin::reader::scan takes a key prefix (format 12's keys are the terms under it), not a lower bound; segment_snapshot and search()/search_count() stay on the format-12 alias (one layout with `inverted` exists); `built` carries format 12's `legacy_stats` (deleted with format 12, L9).
- Finding (pre-existing on main, not changed here): keys_plugin's validate_versions rejects a segment whose versions span 64 bits with a nonzero minimum (lo > ~0 - mask), which segment_writer writes; unreachable with index-assigned versions, but writer and reader disagree. Found by merge_oracle_test with random 64-bit versions.
- Findings folded in: R1-R11, S1, S2 (steps 0-4b); R3 and L3 deps (5b); R8 not applicable yet (no copy paths); P1-R4 (b) and (d) (5c, 5d)
- Last verified: 2026-10-17, macOS host, 894c04a: cmake -G Ninja, full build, ctest 35/35 passed; ASan+UBSan (Homebrew LLVM, Debug) at 6181767: merge_oracle, merge, segment, fixtures, index, reorder, docids, cost, background_merge 9/9 passed
