# indexing-gap

- Design: none (a Track P attribution item, Queued item 0 in docs/status/README.md);
  experiments in docs/perf/indexing-gap.md, each written before it runs.
- Branch / worktree: perf/indexing-gap at /Users/kronuz/code/Kronuz/strata-wt/indexing-gap,
  from origin/main.
- Scope (the lead, 2026-10-17): eight-thread scaling only. The one-thread document path,
  stage 1, the buffer, and the inverter are perf/inverter's; index size and merging are
  perf/index-size's. The threading change takes the shape of term-partitioned flushes over
  inverter C's partitions (docs/research/indexing.md 5.4 and 6), coordinated with
  perf/inverter on the interface.
- State: implementing, its next step on perf/inverter's side (the drain hand-off); this agent
  moves to perf/small-indexes (the lead, 2026-10-18).
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
  - E0 (the profile, 100k, base 21336b4): 75% of the eight-thread wall is stage 1 on the
    owner, 19% the flush (eight runs, ~5 CPUs busy), 5% the tail; the flush's inversion
    takes 12 CPU-seconds in one run and 27 in eight. Calibration at 100k: Tantivy is the
    anchor at 1 and 8 threads (17.0 s, 9.9 s; strata 38.5 s, 24.9 s).
  - E1 (934811b, the engine's parse and tokenize on STRATA_THREADS threads): first run
    1.607x [1.444, 1.848] over A at 8 threads, byte-identical segments; rerun owed.
  - The VM's copies: `~/libs/ig-base` (git archive of 21336b4) and `~/libs/indexing-gap`
    (never `~/libs/strata`); builds and scratch under `~/ig/`.
  - E1's rerun on a quiet host: 1.992x [1.967, 2.446] over the base at 8 threads, 100k.
  - E2 (c4b0202) and E2b (62d235e): one segment per parallel flush, the one-thread bytes;
    E2b ties E1 at 100k (0.968x [0.925, 1.019], the 0.97 rule missed by its lower bound) and
    wins 1.310x [1.289, 1.417] at 1M (5 segments against 40 files). Kept.
  - Quiet recalibration at 100k: Tantivy 6.8 s (1 thread), 3.4 s (8); strata base 13.3 s, 10.8 s.
  - Merges of perf/inverter at a6d8cbb (c4d1c06) and at 10eca3e, E19 (a2d6e68): at
    a2d6e68 eight threads are 1.431x [1.332, 1.470] Tantivy's eight-thread builder at 100k.
  - E3 (4e9ec89) reverted unmeasured (b2f4d45): it reimplemented C's stages in
    tuple_buffer.hh, which perf/inverter owns. The interface proposed to perf/inverter is in
    the perf note.
  - E4 (40a1310, C at eight threads with its one-thread flush): 0.888x [0.719, 0.915] of
    a2d6e68's docid runs; removed (this commit).
  - E5 (424eb87): C's flush staged on the host's executor with help-while-waiting; one index
    alone at 100k with no setting 1.174x Tantivy's multi-threaded builder, 0.822x the configured
    docid runs (its rule failed; kept as the base of the drain hand-off, judged with it).
  - E7 (9c6cf0e, index-many and bench/index_many.sh): eight indexes x 100k on one pool 1.787x
    [1.781, 1.830] eight Tantivy builders on the same cores, level with one thread each
    (0.995x [0.965, 1.003]); CPU per document 86.8 us (pool), 80.4 (one thread each), 140.4
    (Tantivy).
- Doing: nothing on this branch; the lead's next item is perf/small-indexes.
- Next (on this branch, when it resumes):
  0. perf/inverter answered the interface at 339acf7 (C's flush steps public: drain_for_flush,
     plan, add_documents, alphabet, splitters, each_term over [lo, hi) on several threads):
     merge it, then E8, C's flush by key range on the pool (flush_grouped's ranges over
     each_term, replacing E5's serial merger), one index alone and N = 8 as for E5 and E7.
  1. When C hands off a batch whole (the interface section), drain it on the pool and measure
     one index alone again.
  2. Cleanup owed: the docid runs (flush_runs, flush_grouped, index::flush_documents,
     flush_threads) are unreachable with C at every load; E1's dedicated parse threads go or
     return as pool tasks.
  3. The 1M confirmation of the adaptive path, one index alone and eight at once.
- Blocked on: perf/inverter for the batch hand-off (C's side of the interface in
  docs/perf/indexing-gap.md); nothing else.
- Findings folded in: none (no review yet). Lead's rules of 2026-10-17: ownership, merging
  perf/inverter after its kept items, the interface section.
- Last verified: 2026-10-18. Local Release build (Apple clang, nice 19, 4 jobs) at E5 and
  the index-many harness (byte-identical with and without the pool, one index and eight); segment_test, tuple_buffer_test, multisegment_test, index_test pass; the engine
  writes the one-thread bytes at 8 threads (30k synthetic documents, budgets 64 and 2056 MB).
  CI: 934811b, c4b0202, 62d235e seven of eight jobs green each (TSan green on 62d235e); their
  ASan jobs were cancelled by the next dispatch on the same ref.
