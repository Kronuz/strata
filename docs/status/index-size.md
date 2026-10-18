# index-size

- Design: none (a Track P footprint item: index size and merging during indexing);
  experiments in docs/perf/index-size.md, each written before it runs.
- Branch / worktree: perf/index-size at /Users/kronuz/code/Kronuz/strata-wt/index-size,
  from origin/main at 4c9ebfd.
- State: implementing (E8 and E3b's 5M confirmation running in `.scratch/e1/e8.sh`).
- Done criteria (set by the lead; contributors tick, never edit): none set yet.
- Done:
  - Evidence read from the gate VM (30e2beb).
  - E1, where the bytes are (e79b9c9, fed378e): one segment at 100k is 1.028× Lucene
    (postings 1.095×, terms 1.152× above their counterparts); at 5M in the gate's
    configuration the manifest names 7 segments, 2.82 GB, and 16 merged-away sources,
    2.21 GB, stay on disk (44% of T4's bytes); 7 segments against the docid-order force
    merge cost +1.95% (2,824,094,984 against 2,770,003,548).
  - E2, reclaim after the final commit (6248c3d): kept. 5M: T4's one-thread bytes from
    5.03 GB to 2.824 GB (1.0016× Lucene's force-merged index, 0.921× Tantivy's); at 1M,
    511 MB freed in 0.40 to 0.57 s.
  - E3, background merges, STRATA_MERGE_THREADS (2f54077, 42d997f): no effect at 100k (B/A
    0.948× [0.879–1.067]), where merges are small. E3b at 1M (1485aaf), where inline merges
    are 27% of the run (IS-6), a deviation from the 100k rule stated in the perf file: B/A
    0.913× [0.858–0.960], peak RSS +2.8%, bytes +0.32%: kept by its rule; the layout varies
    run to run. The engine's default moves only after the 5M confirmation of peak RSS.
  - E4, MinHash order at flush, for size (c8daf44, 97a998a): whole segment 0.980× at 100k,
    0.982× at a 271k flush, 0.974× at 5M (bisection 0.917×); the permuted docids section and
    stored records take about 40% of the postings-and-positions gain; docid-order merges
    give it back. Evidence for the architect's ruling.
  - E5, the final merge, STRATA_FINAL_MERGE (40f41df, 6d2d409): kept as the schedule. At 1M
    one segment, 2.7% smaller; no natural merges plus one final merge writes about 2.08×
    (Tantivy's T4 2.0, Lucene's 3.0) and is not slower today; peak RSS +31% from today's
    merge (5.80 GB at 5M against T4's 4.6 GB), which the copy merge removes.
  - E6, the merge factor (6d2d409, 883aa2e): refuted; 8 per tier stays.
  - E7, adjacent merges, merge_policy::adjacent (e35acaa, 13e33e9, 883aa2e): opt-in. Bytes
    within 0.33% and writes within 3% of the tiered policy at 1M; final segment counts +20%
    and +33% on two of three simulated workloads (peaks within 4%), past its 10% rule; it is
    the copy merge's prerequisite, so its default goes with the copy merge's review. The
    tiered path decides exactly as before (its test output diffed against main's).
  - Format 13 notes N1 to N4 (5986033, 0fe613b): Lucene's fold of wdf 1 into tail deltas
    closes the carried postings row (1.014× to 0.997× keyed -bp on the reordered 5M merge);
    Rice-coded tails 0.993×; implied positional headers; Rice positional tails.
  - Findings IS-1 to IS-6 (728ce8b, 6cf9439): for the layers owner IS-1 to IS-3, for the
    lead IS-4 and IS-5, for perf/indexing-gap IS-6 (inline merges are about a third of T4's
    one-thread run at 5M).
  - IS-4 measured (3bf4e66): before merging, keyed Lucene leaves 3 segments of 2,829,753,418
    bytes and Tantivy 3 of 3,095,480,261; strata with E2 is 0.998× and 0.912× of them, and
    0.982× and 0.903× force-merged.
- Doing: E8 (eight-thread bytes at 1M, two budgets) and E3b's 5M confirmation (A inline, B
  one merge thread, `STRATA_BUDGET_MB=2056`), one run each, informational.
- Next:
  1. E8's and the 5M confirmation's tables; if B's peak RSS at 5M stays within 10% of A's,
     propose `STRATA_MERGE_THREADS=1` as the engine's T4 default to the lead.
  2. CI on the head (run 39099120364 at e35acaa: 7 jobs pass, ASan at its 60-minute limit).
  3. The copy merge, once format 13's items land: E5's schedule and E7's adjacency with it.
- Blocked on: nothing.
- Host note: at 21:39 a one-file engine compile (about 25 s, `nice 19`, 4 jobs) ran inside
  indexing-gap's strict hold `ig-e2` (21:35 to 22:05, an eight-thread A/B on the gate VM);
  that window's trial may carry it. Since then `quiet check` runs alone before any build.
- Coordination: perf/indexing-gap edits `tools/strata_engine.cc` too (its E1 parse
  threads); this branch's engine changes sit after the read loop and in the options block.
  perf/inverter owns the one-thread hot loop. research/indexing's 5.1 (MinHash) and 5.3 (copy
  merge) are measured or scheduled here; its format list goes to the format 13 designer
  through the lead. research/codecs has not pushed yet. `merge.hh` changes wait for layers;
  `index.hh` has one changed line (run_merges' candidates), which conflicts trivially with
  layers' `seg.core()` form of the same line.
- Findings folded in: none yet (no review of this branch yet).
- Last verified: 2026-10-17. Release build at e35acaa (macOS x86_64, Apple clang 17): all
  33 ctest tests pass; merge_policy_test's tiered lines equal main's. CI run 39095598017 at
  2f54077: 7 jobs pass, linux-asan-ubsan cancelled at its 60-minute limit (main's same job
  took 55 min 21 s at 584cab1). CI run 39099120364 at e35acaa queued.
