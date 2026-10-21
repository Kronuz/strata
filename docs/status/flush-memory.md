# flush-memory
- Task: cut the flush's peak memory (explorer's verified win, docs/perf/peak-memory.md): size the stored copy and output up front, borrow stored records from the buffer, stream every flush shape to its segment file. Segment bytes unchanged.
- Branch / worktree: phase4/flush-memory from main 55453ae, at /Users/kronuz/code/Kronuz/strata-wt/flush-memory
- State: implementing; one review (lifetime across threads) before landing.
- Done: (see below)
- Next: revert checks and mutants, sanitizers, the measurement.
- Blocked on: nothing.
