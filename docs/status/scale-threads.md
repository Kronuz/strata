# scale-threads

- Item: why one index's indexing barely scales with threads; prototype the best split.
  Notes in docs/perf/scale-threads.md, written before each run.
- Branch / worktree: perf/scale-threads at /Users/kronuz/code/Kronuz/strata-wt/scale-threads,
  from perf/inverter 75b8715.
- State: largest-writer flush built and measured (2026-10-19); the flush no longer limits
  scaling, the inline merges do. Waiting on the lead's call on merges. No hold held.
- Done:
  - 30c8446: the profile at 100k on the Mac; the serial fraction about 9%.
  - c6bafd4: the prototype (index_options::writers, apply_to, flush_writers; engine
    STRATA_WRITERS, STRATA_TIMERS, digest DIR).
  - 1addcd5: the lead's curves (gate VM 1M: 3.47x at 8 writers; Tantivy 1.88x).
  - One writer byte-identical to 75b8715 at 20k and 100k, before and after the merges, and
    no slower (docs/perf/scale-threads.md, Landing checks).
  - 54bf2b6, 2b520ea: index_test checks 1, 2, 4, 8 writers (replaced keys, deletes,
    rejected transactions, mid-run flushes, auto-merge, force_merge) equal one writer's
    digest and contents. index, multisegment, tuple_buffer pass.
  - TSan: engine at 4 writers and 16 MB, and the three tests: no reports (pre-merge head;
    CI's linux-tsan covers the merged head).
  - Merge cost at 1M on the Mac: 9 segments merge in 34.5 s against 6 in 31.8 s (reorder
    off); at the default budget 8 writers were 2.3x slower than one (frequent full flushes).
  - e803a4d: design note, writers on the shared pool (WorkPool), not built.
  - 611b8af, 328855a: merges of perf/inverter and main (ci.yml from main).
  - Merge of main 734949e: main's inverter, tuple_buffer, and layers code taken whole; the
    writer-per-thread delta (index.hh, index_test, strata_engine) replayed onto it.
  - The largest-writer flush (Lucene's DWPT policy): past the share only the writer holding
    the most flushes, outside the lock, while the others index; flushes past twice the share
    stall stage 1. Late deletes reach the flushed segment at publish; slot buffers take
    main's max_run_bytes. index, multisegment, tuple_buffer pass (late deletes and the
    flagged-while-inverting path both exercised); one writer byte-identical to main 734949e
    at 20k, 100k (2,056 MB and default budgets) and 1M default (6 segments, `ba8fa820cf8d9bb4`).
  - A writer's batch scaled to the share: at most a quarter of it across the writers (2^17
    tuples a batch at 8 writers and 256 MB, 2^20 at one writer or 2 GB); each slot's batch
    scratch alone was about 30 MB at 2^20, so eight filled the default share by themselves.
  - 460751d, c784f1c, 547aedf, and this commit: the procedure, then the Mac 1M rows (two
    rounds; one beside Tantivy and with merges off) and the gate VM's, all at the default
    share. With merges off 8 writers are about 3.0x one writer on both (9.3 and 9.7 s); with
    them on 0.76 to 0.85x, the tiered policy merging 21 to 29 s inline under the lock.
    37 to 39 segments at 8 writers. Digest at 4 writers, 1M, merges on: one writer's.
  - CI on 460751d (the code head): green on every job, TSan and ASan+UBSan included.
- Next: the lead's call: merges off the writers' lock (pool_executor, the design note's
  merge class) or a larger share with several writers; code review of perf/scale-threads;
  the pool design's review. The first run of each ab_threads round is slow at one writer
  (41-42 s against 27-29 s, its flushes slower), a harness effect to fix before gate runs.
- Blocked on: nothing.
- Lead's ruling (2026-10-19): merges move off the index's lock. Reuse perf/index-size's
  background merge (E3b, one merge thread by default, kept there at c88f2de), landing that
  branch first, rather than a second mechanism; it later moves onto the shared pool's merge
  class (Kronuz/threadpool feature/scheduler) and the no-stalls research's controller.
  Also fix ab_threads.py's slow first run per round (a warm-up unit) before any gate run.
