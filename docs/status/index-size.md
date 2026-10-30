> Status: PARKED (2026-10-18) stopped by the lead until the copy merge; reclaiming merged-away sources landed separately as the background reclaim.

# index-size

- Design: none (a Track P footprint item: index size and merging during indexing);
  experiments in docs/perf/index-size.md, each written before it runs.
- Branch / worktree: perf/index-size at /Users/kronuz/code/Kronuz/strata-wt/index-size,
  rebased onto origin/main at 088a64f on 2026-10-18 (commits below are the rebased ones).
- State: implementing, stopped by the lead until format 13's copy merge and indexing-gap's
  flush land (E1 to E8, E3c, and T3's re-score recorded; CI green at e7d94fc).
- Done criteria (set by the lead; contributors tick, never edit): none set yet.
- Done:
  - Evidence read from the gate VM (85bf0fb).
  - E1, where the bytes are (c50742b, 4e93100): one segment at 100k is 1.028× Lucene
    (postings 1.095×, terms 1.152× above their counterparts); at 5M in the gate's
    configuration the manifest names 7 segments, 2.82 GB, and 16 merged-away sources,
    2.21 GB, stay on disk (44% of T4's bytes); 7 segments against the docid-order force
    merge cost +1.95% (2,824,094,984 against 2,770,003,548).
  - E2, reclaim after the final commit (8496316): kept. 5M: T4's one-thread bytes from
    5.03 GB to 2.824 GB; 2.21 GB freed in 1.1 to 1.3 s on APFS.
  - IS-4 measured (28ab012): like for like, strata with E2 is 0.998× keyed Lucene's and
    0.912× Tantivy's before-merging bytes (3 segments each), 0.982× and 0.903× force-merged.
  - E3 and E3b, background merges (68b760c, d9a8ed6, 696c65a, d0bd7ef): no effect at 100k,
    where merges are small; at 1M B/A 0.913× [0.858–0.960] over five interleaved rounds,
    peak RSS +2.8%, bytes +0.32% (kept by its rule; the 1M run is a deviation from the 100k
    rule, stated in the perf file with its reason, IS-6); at 5M one run each 613 s against
    739 s, peak RSS lower, bytes +0.34%. The engine's default is now one merge thread
    (`STRATA_MERGE_THREADS=0` for inline). Layouts vary run to run with it (about 0.01%).
  - E4, MinHash order at flush, for size (596549f, 536a170): whole segment 0.980× at 100k,
    0.982× at a 271k flush, 0.974× at 5M (bisection 0.917×); the permuted docids section and
    stored records take about 40% of the postings-and-positions gain; docid-order merges
    give it back. Evidence for the architect's ruling.
  - E5, the final merge, STRATA_FINAL_MERGE (3e05d55, 3b078f3): kept as the schedule; at 1M
    one segment, 2.7% smaller; no natural merges plus one final merge writes about 2.08×
    and is not slower today; peak RSS +31% from today's merge (IS-3), which the copy merge
    removes. The default stays off: whether T4 force-merges is the lead's call (IS-4).
  - E6, the merge factor (3b078f3, fdedaa9): refuted; 8 per tier stays.
  - E7, adjacent merges, merge_policy::adjacent (7282ad7, 6a5e344, fdedaa9): opt-in; bytes
    within 0.33% and writes within 3% at 1M; final segment counts +20% and +33% on two of
    three simulated workloads (peaks within 4%), past its 10% rule; the copy merge's
    prerequisite, so its default goes with the copy merge's review.
  - Bytes per component at 5M before merging, after the kept changes (this commit): strata
    1.0014× Lucene's whole (0.998× with lengths excluded), 0.915× Tantivy's; terms 1.56× and
    postings 1.037× are above Lucene's, positions 0.962× and stored 0.486× below. E3b's
    default costs +0.34% bytes (9 segments against 7) for 17% less time, stated both ways.
  - The branch's shared-file changes listed for the landing merge (8cfded9).
  - IS-4 ruled by the lead and recorded (dce3660): time rows without a force merge (baselines
    `NO_MERGE=1`); disk rows natural and force-merged for every engine; wording proposed below.
  - E3c, the merge thread on the other platforms (857bbf7, 368ae74): reruns alone with a
    discarded warm-up at 500k, light holds. Linux (gate VM) wall 0.865× [0.653–1.000], CPU
    0.992× [0.747–1.135]: the default stays. FreeBSD wall 0.907× [0.830–1.003], CPU 1.136×
    [1.039–1.281], nearly three times the kernel time: it loses CPU, so the default reverts
    there (`#if defined(__FreeBSD__)`, confirmed by a 20k run). Peak RSS +13% to +17% on the VMs.
  - T3 re-scored with the reclaim (ff922ac): by each process's own written bytes, strata 1.001
    and 1.000 at 100k and 1M against Lucene's 2.003 and 2.001 without a force merge (its
    flushes copy into compound files); the device counts carry other agents' writes. IS-5 shows
    only where merges run: n1017's 5M strata T3 is about 2.16, not 1.26.
  - Flush points depend on the C++ library (e7d94fc): one segment's bytes match across
    platforms, multi-segment bytes differ by about 0.1% (libstdc++ against libc++).
  - E8, eight-thread bytes (52d687a): with E2 the eight-thread index is 0.3% to 2.6% above
    the one-thread one at 1M; without E2 its directory held up to 3× the index.
  - Format 13 notes N1 to N4 (eb1ed4a, 0c4c090): Lucene's fold of wdf 1 into tail deltas
    closes the carried postings row (1.014× to 0.997× keyed -bp on the reordered 5M merge);
    Rice-coded tails 0.993×; implied positional headers; Rice positional tails.
  - Findings IS-1 to IS-6 (3b41e0b, 935549e): IS-1 to IS-3 for the layers owner, IS-4 and
    IS-5 for the lead, IS-6 for perf/indexing-gap.
- Doing: nothing.
- Next:
  1. Under main's new rows (PLAN: indexing per index on one thread, CPU per document;
     flush and merge work as tasks on the shared pool), E3b is neutral on CPU (1.003×) and
     wins wall for a lone index; the copy merge is what lowers CPU per document (IS-6).
  2. The copy merge, once format 13's items land: E5's schedule and E7's adjacency with it,
     and the final merge's memory (IS-3).
  3. E8 again on perf/indexing-gap's branch once its one-segment flush lands (the lead's
     word: rerun there, do not reimplement it here).
  4. research/codecs' winners, when its notes land.
  5. Profile FreeBSD's extra kernel time with the merge thread (E3c), for completion's
     platform notes, when the lead restarts this item.
- Proposed wording for main (IS-4 as ruled, for the lead to land):
  - BENCHMARKS.md, "Indexing speed" (`:139`), after "each engine with its own merging": "and
    no force merge inside the timed run (the baselines' builders run with `NO_MERGE=1`): a
    force merge is not part of indexing."
  - BENCHMARKS.md, "Disk" (`:156`), and PLAN.md's Disk paragraph: "Bytes natural (each
    engine's own merging, no force merge) and force-merged (every engine to one segment;
    strata with `STRATA_FINAL_MERGE=1`), whole and per section, single exact measurements."
  - BENCHMARKS.md, "Write amplification (T3)" (`:158`), after "at equal memory as above":
    "with no force merge, over the index each engine leaves."
- Blocked on: nothing.
- Host note (2026-10-18): E3c's FreeBSD A/B started under a strict hold (03:10) and moved to a
  light hold tied to its job at 03:32, on the lead's word: a two-thread A/B needs only a
  light hold. The gate VM run is sized to about 15 minutes, also under a light hold.
- Host note: at 21:39 a one-file engine compile (about 25 s, `nice 19`, 4 jobs) ran inside
  indexing-gap's strict hold `ig-e2` (21:35 to 22:05, an eight-thread A/B on the gate VM);
  that window's trial may carry it. Since then `quiet check` runs alone before any build.
- Coordination: perf/indexing-gap edits `tools/strata_engine.cc` too (its E1 parse threads);
  this branch's engine changes sit after the read loop and in the options block, and its
  default merge thread changes T4's one-thread configuration (stated for the lead).
  perf/inverter owns the one-thread hot loop. research/indexing's 5.1 (MinHash) and 5.3 (copy
  merge) are measured or scheduled here; its format list goes to the format 13 designer
  through the lead. research/codecs has not pushed yet. `merge.hh` changes wait for layers;
  `index.hh` has two changed lines (run_merges' candidates), which conflicts trivially with
  layers' `seg.core()` form of the same line.
- Findings folded in: none yet (no review of this branch yet).
- Last verified: 2026-10-18. CI run 39126988161 at e7d94fc (the head's code; later commits
  change docs only): all 8 jobs pass, linux-asan-ubsan with 33 of 33 tests, linux-tsan, linux
  x86_64 gcc and clang, linux arm64, macOS x86_64 and arm64, FreeBSD (which builds the
  FreeBSD default). Release builds of the engine on the gate VM (Ubuntu 24.04, clang) and
  FreeBSD 14.4 (clang); on FreeBSD a 20k run confirms inline merges by default. Locally
  (macOS x86_64): Release, 33 of 33 ctest tests at 7282ad7; ASan and UBSan (Homebrew LLVM 23)
  on the 7 tests the earlier capped CI runs had not reached, all pass.
