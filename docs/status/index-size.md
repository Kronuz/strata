# index-size

- Design: none (a Track P footprint item: index size and merging during indexing);
  experiments in docs/perf/index-size.md, each written before it runs.
- Branch / worktree: perf/index-size at /Users/kronuz/code/Kronuz/strata-wt/index-size,
  from origin/main at 4c9ebfd.
- State: implementing (E5 running at 1M; E1 to E4 recorded).
- Done criteria (set by the lead; contributors tick, never edit): none set yet.
- Done:
  - Evidence read from the gate VM (30e2beb).
  - E1, where the bytes are (e79b9c9, fed378e): one segment at 100k is 1.028× Lucene
    (postings 1.095×, terms 1.152× above their counterparts); 15 segments of 100k cost
    +18.5%; at 5M in the gate's configuration the manifest names 7 segments, 2.82 GB, and
    16 merged-away sources, 2.21 GB, stay on disk (44% of T4's bytes); 7 segments against
    the docid-order force merge cost +1.95% (2,824,094,984 against 2,770,003,548).
  - E2, reclaim after the final commit (6248c3d): kept. At 1M, 511 MB freed in 0.57 s and
    the directory equals the manifest's files; at 5M it takes T4's one-thread bytes from
    5.03 GB to 2.824 GB (1.0016× Lucene's force-merged index, 0.921× Tantivy's).
  - E3, background merges behind STRATA_MERGE_THREADS (2f54077): not kept as a default.
    At 100k B/A 0.948× [0.879–1.067], no time effect; 3 segments instead of 8; RSS +13%.
    The knob stays off by default for E5.
  - E4, MinHash order at flush for size (c8daf44, 97a998a): 0.980× of the whole segment at
    100k, 0.982× at a 271k flush, 0.974× of the 5M one-segment index (bisection 0.917×); the
    permuted docids section and stored records take about 40% of the postings and positions
    gain; natural merges in docid order give it back. Evidence for the architect's ruling.
  - E5's engine option STRATA_FINAL_MERGE (40f41df): force_merge(1) in docid order after
    the last flush, as the baselines end their T4 runs.
  - Format 13 notes N1 to N4 (5986033, 0fe613b): on the reordered 5M merge, Lucene's fold
    of wdf 1 into tail deltas takes postings from 1.014× to 0.997× keyed -bp (the carried
    row closes); Rice-coded tails reach 0.993×; implied positional headers save 0.5% of
    positions at 5M (3% at 100k); Rice positional tails 4.1 MB.
  - Findings IS-1 to IS-5 (728ce8b): for the layers owner, sources reclaimable only after a
    commit, the 5 s wait of a merge larger than the budget, the merge's per-posting
    allocation; for the lead, T4's mixed semantics and T3's inflated denominator.
- Doing: E5 at 1M (`.scratch/e1/e5.sh`): (a) E2 only, (b) background merges and the final
  merge, (c) no natural merges and the final merge, (d) inline merges and the final merge.
- Next:
  1. E5's table, the schedule's write amplification and critical-path merge time today and
     with the copy merge's cost (research 5.3).
  2. Lucene and Tantivy at 5M with NO_MERGE=1 (bytes, segments), for IS-4.
  3. CI on the branch's head (run 39095598017 covers 2f54077).
  4. The copy merge, once format 13's items land: E5's schedule measured with it.
- Blocked on: nothing.
- Host note: at 21:39 a one-file engine compile (about 25 s, `nice 19`, 4 jobs) ran inside
  indexing-gap's strict hold `ig-e2` (21:35 to 22:05, an eight-thread A/B on the gate VM);
  that window's trial may carry it.
- Coordination: perf/indexing-gap edits `tools/strata_engine.cc` too (its E1 parse
  threads); this branch's engine changes are after the read loop and in the options block.
  perf/inverter owns the one-thread hot loop. research/indexing's 5.1 (MinHash) and 5.3 (copy
  merge) are measured or scheduled here; its format list goes to the format 13 designer
  through the lead. research/codecs has not pushed yet. Merge-speed changes wait for layers.
- Findings folded in: none yet (no review of this branch yet).
- Last verified: 2026-10-17. Release builds at 4c9ebfd and the branch's engine (macOS
  x86_64, Apple clang 17); the runs above. CI run 39095598017 at 2f54077: freebsd-x86_64,
  macos-arm64, macos-x86_64, linux-x86_64-gcc, linux-x86_64-clang, linux-arm64-gcc pass;
  linux-tsan and linux-asan-ubsan running. No local ctest run (the branch changes only the
  engine tool, which no test links).
