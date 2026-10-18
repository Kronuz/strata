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
- State: implementing (E1 kept on its first run, rerun owed: its rounds overlapped other
  builds during the strict hold).
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
- Doing: the E1 rerun under a strict hold once the VM's other measurements end.
- Next:
  1. E1's rerun, then E2: merges on a background pool (the engine passes `o.background`),
     shown where merges run (a scaled budget at 100k, then 1M at the gate's).
  2. The interface with perf/inverter for C's partitioned flush: partitions encoded on T
     threads, the assembler's one segment, the add phase on receiving threads with an
     ordinal base per batch, and the flush off the owner core.
- Blocked on: nothing.
- Findings folded in: none yet.
- Last verified: 2026-10-17. Local Release build (Apple clang, nice 19, 4 jobs): segments
  byte-identical across the inline and pipelined paths at 1 and 8 threads (30k synthetic
  documents); CI run 39092408546 on 934811b: six of eight jobs green, ASan and TSan running.
