# index-size

- Design: none (a Track P footprint item: index size and merging during indexing);
  experiments in docs/perf/index-size.md, each written before it runs.
- Branch / worktree: perf/index-size at /Users/kronuz/code/Kronuz/strata-wt/index-size,
  from origin/main at 4c9ebfd.
- State: implementing (E1 to E8 recorded; CI run 39105051370 on the head).
- Done criteria (set by the lead; contributors tick, never edit): none set yet.
- Done:
  - Evidence read from the gate VM (30e2beb).
  - E1, where the bytes are (e79b9c9, fed378e): one segment at 100k is 1.028× Lucene
    (postings 1.095×, terms 1.152× above their counterparts); at 5M in the gate's
    configuration the manifest names 7 segments, 2.82 GB, and 16 merged-away sources,
    2.21 GB, stay on disk (44% of T4's bytes); 7 segments against the docid-order force
    merge cost +1.95% (2,824,094,984 against 2,770,003,548).
  - E2, reclaim after the final commit (6248c3d): kept. 5M: T4's one-thread bytes from
    5.03 GB to 2.824 GB; 2.21 GB freed in 1.1 to 1.3 s on APFS.
  - IS-4 measured (3bf4e66): like for like, strata with E2 is 0.998× keyed Lucene's and
    0.912× Tantivy's before-merging bytes (3 segments each), 0.982× and 0.903× force-merged.
  - E3 and E3b, background merges (2f54077, 42d997f, 1485aaf, c88f2de): no effect at 100k,
    where merges are small; at 1M B/A 0.913× [0.858–0.960] over five interleaved rounds,
    peak RSS +2.8%, bytes +0.32% (kept by its rule; the 1M run is a deviation from the 100k
    rule, stated in the perf file with its reason, IS-6); at 5M one run each 613 s against
    739 s, peak RSS lower, bytes +0.34%. The engine's default is now one merge thread
    (`STRATA_MERGE_THREADS=0` for inline). Layouts vary run to run with it (about 0.01%).
  - E4, MinHash order at flush, for size (c8daf44, 97a998a): whole segment 0.980× at 100k,
    0.982× at a 271k flush, 0.974× at 5M (bisection 0.917×); the permuted docids section and
    stored records take about 40% of the postings-and-positions gain; docid-order merges
    give it back. Evidence for the architect's ruling.
  - E5, the final merge, STRATA_FINAL_MERGE (40f41df, 6d2d409): kept as the schedule; at 1M
    one segment, 2.7% smaller; no natural merges plus one final merge writes about 2.08×
    and is not slower today; peak RSS +31% from today's merge (IS-3), which the copy merge
    removes. The default stays off: whether T4 force-merges is the lead's call (IS-4).
  - E6, the merge factor (6d2d409, 883aa2e): refuted; 8 per tier stays.
  - E7, adjacent merges, merge_policy::adjacent (e35acaa, 13e33e9, 883aa2e): opt-in; bytes
    within 0.33% and writes within 3% at 1M; final segment counts +20% and +33% on two of
    three simulated workloads (peaks within 4%), past its 10% rule; the copy merge's
    prerequisite, so its default goes with the copy merge's review.
  - E8, eight-thread bytes (dd099b2): with E2 the eight-thread index is 0.3% to 2.6% above
    the one-thread one at 1M; without E2 its directory held up to 3× the index.
  - Format 13 notes N1 to N4 (5986033, 0fe613b): Lucene's fold of wdf 1 into tail deltas
    closes the carried postings row (1.014× to 0.997× keyed -bp on the reordered 5M merge);
    Rice-coded tails 0.993×; implied positional headers; Rice positional tails.
  - Findings IS-1 to IS-6 (728ce8b, 6cf9439): IS-1 to IS-3 for the layers owner, IS-4 and
    IS-5 for the lead, IS-6 for perf/indexing-gap.
- Doing: nothing running; CI run 39105051370 at c88f2de queued.
- Next:
  1. CI 39105051370 to its terminal state (ASan's 60-minute limit is main's known chore 9;
     the 26 tests it reached at e35acaa and the 7 it did not, run locally, all pass).
  2. The copy merge, once format 13's items land: E5's schedule and E7's adjacency with it,
     and the final merge's memory (IS-3).
  3. E3b's time on the gate VM and FreeBSD, under holds the lead grants.
  4. research/codecs' winners, when its notes land.
- Blocked on: nothing.
- Host note: at 21:39 a one-file engine compile (about 25 s, `nice 19`, 4 jobs) ran inside
  indexing-gap's strict hold `ig-e2` (21:35 to 22:05, an eight-thread A/B on the gate VM);
  that window's trial may carry it. Since then `quiet check` runs alone before any build.
- Coordination: perf/indexing-gap edits `tools/strata_engine.cc` too (its E1 parse threads);
  this branch's engine changes sit after the read loop and in the options block, and its
  default merge thread changes T4's one-thread configuration (stated for the lead).
  perf/inverter owns the one-thread hot loop. research/indexing's 5.1 (MinHash) and 5.3 (copy
  merge) are measured or scheduled here; its format list goes to the format 13 designer
  through the lead. research/codecs has not pushed yet. `merge.hh` changes wait for layers;
  `index.hh` has one changed line (run_merges' candidates), which conflicts trivially with
  layers' `seg.core()` form of the same line.
- Findings folded in: none yet (no review of this branch yet).
- Last verified: 2026-10-17. Release build at e35acaa (macOS x86_64, Apple clang 17): all 33
  ctest tests pass; merge_policy_test's tiered lines equal main's. ASan and UBSan (Homebrew
  LLVM 23, RelWithDebInfo, macOS x86_64) at c88f2de: search, engine_query, reclaim_mapped,
  lifecycle_mapped, background_merge_mapped, fuzz_segment_replay, fuzz_files_replay pass. CI
  run 39099120364 at e35acaa: 7 jobs pass (TSan included); linux-asan-ubsan cancelled at its
  60-minute limit after 26 of 33 tests passed (main's same job took 55 min 21 s at 584cab1).
