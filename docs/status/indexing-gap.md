# indexing-gap

- Design: none (a Track P attribution item, Queued item 0 in docs/status/README.md);
  experiments in docs/perf/indexing-gap.md, each written before it runs.
- Branch / worktree: perf/indexing-gap at /Users/kronuz/code/Kronuz/strata-wt/indexing-gap,
  from origin/main.
- State: implementing (E0, the profile, waits for the gate VM: the targeted T3 run holds
  it until T3_DONE in ~/bench/results/t3-targeted.log).
- Done criteria (set by the lead; contributors tick, never edit): none set yet.
- Done:
  - The n1017 T4 raw files read and attributed as far as they go (docs/perf/indexing-gap.md,
    "Evidence"): `STRATA_THREADS` reaches the index; the eight-thread run is serial (132%
    CPU; S = 1,033 s serial against P = 400 s parallel by the Amdahl fit); the one-thread
    baselines merge on background threads and force-merge inside the timed run.
  - Code reading (docs/perf/indexing-gap.md, "What runs where"): parse, tokenize,
    evaluate, and the buffer copy on the owner thread; inversion only at flush, one segment
    per run; merges inline on the owner thread because the engine passes no executor;
    reclaim capped at 64 MB per commit.
- Doing: E0 setup. The VM's copy will be `~/libs/indexing-gap` (never `~/libs/strata`), its
  builds and scratch under `~/ig/`.
- Next:
  1. On T3_DONE: sync, build the profiling engine, the 20k dry run, E0 at 100k at 1 and 8
     threads, then the 1M pair for the mix at scale.
  2. Calibration at 100k: strata, Tantivy, Lucene at 1 and 8 threads, once; name the anchors.
  3. The first fix for the eight-thread bottleneck E0 names, written into the perf file
     before its A/B (100k, five interleaved trials against the anchor; 8-thread timing
     under a strict hold of at most 30 minutes).
- Blocked on: nothing.
- Findings folded in: none yet.
- Last verified: 2026-10-17. Nothing built or run; raw files read from the gate VM.
