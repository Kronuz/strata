# scale-threads

- Item: why one index's indexing barely scales with threads; prototype the best split.
  Notes in docs/perf/scale-threads.md, written before each run.
- Branch / worktree: perf/scale-threads at /Users/kronuz/code/Kronuz/strata-wt/scale-threads,
  from perf/inverter 75b8715.
- State: merged with main 734949e (the layers refactor, the inverter on layers); building
  the largest-writer flush policy for the Mac 1M slowdown at the default budget (2026-10-18).
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
- Next: code review of perf/scale-threads; the lead's call on the Mac 1M slowdown at the
  default budget (slots bounded by the share, or flushes that do not stop every writer)
  before keep or remove; the pool design's review.
- Blocked on: nothing.
