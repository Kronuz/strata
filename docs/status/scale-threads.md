# scale-threads

- Item: why one index's indexing barely scales with threads; prototype the best split.
  Notes in docs/perf/scale-threads.md, written before each run.
- Branch / worktree: perf/scale-threads at /Users/kronuz/code/Kronuz/strata-wt/scale-threads,
  from perf/inverter 75b8715.
- State: stopped by the coordinator (2026-10-18) after the prototype and its digest checks.
  No timing run yet; no hold held.
- Done:
  - 30c8446: the profile at 100k on the Mac (sampled). The best configured eight-thread path
    (a2d6e68) burns 3.5x C's one-thread CPU and is slower than C alone. Under a writer per
    thread, the serial fraction is about 9% (stage 1, reading, commit files): Amdahl 1.83x
    at 2, 3.15x at 4, 4.9x at 8.
  - c6bafd4: the prototype (index_options::writers, apply_to, flush_writers; engine
    STRATA_WRITERS, STRATA_TIMERS, digest DIR). Digests equal at 1, 2, 4, 8 writers at 100k
    (2,056 and 64 MB) and on 27k with replaced keys, deletes, mid-run flushes and merges.
- Left (in order):
  1. Build 75b8715 untouched (.scratch/src-75b8715, build killed mid-way) and confirm one
     writer's segment is byte-identical to it and no slower.
  2. Build the Tantivy builder with TANTIVY_THREADS (.scratch/engines/tantivy-0.26-mt,
     patched, not built).
  3. The Mac curve: .scratch/tools/ab_threads.py (written, not run), strata-1/2/4/8
     (STRATA_WRITERS) against tantivy-1/2/4/8 (TANTIVY_THREADS, NO_MERGE=1), five rounds,
     under a strict hold of at most 15 minutes; one STRATA_TIMERS run at 8 for the idle map.
  4. The gate VM once (perf/inverter's vm_put.sh and vm_ab.sh, adapted to this worktree).
  5. Unit tests (index_test, multisegment_test, tuple_buffer_test) and a TSan engine run
     with 4 writers and a small budget.
  6. Optional: the extra segments' merge cost.
- Blocked on: nothing; stopped by priority.
