# scale-threads

- Item: why one index's indexing barely scales with threads; prototype the best split.
  Notes in docs/perf/scale-threads.md, written before each run.
- Branch / worktree: perf/scale-threads at /Users/kronuz/code/Kronuz/strata-wt/scale-threads,
  from perf/inverter 75b8715.
- State: profiling done; prototype (a writer per thread) in progress.
- Done:
  - Profile at 100k on the Mac (sampled): the best configured eight-thread path (a2d6e68)
    burns 3.5x C's one-thread CPU and is slower than C alone; under a writer per thread the
    serial fraction is about 9% (stage 1, reading, commit files), Amdahl 4.9x at 8.
- Next: the prototype, digests at 2/4/8 writers, the Mac curve against Tantivy 1-8, the VM.
- Blocked on: nothing.
