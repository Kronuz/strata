# merge-wait: a merge never sleeps for the writer's budget

- Task (the lead's, 2026-10-20): the RocksDB audit (`docs/perf/core-vs-rocksdb.md`, section 3) found
  the 2x settle taking 15 s because `run_merge` slept up to `merge_memory_wait` (5 s) per round for
  room on the indexing budget that never came. Fix it in the merge admission and wait path only.
- Branch: `phase4/merge-wait` (worktree `strata-wt/merge-wait`), from main `4d36aba`.
- Out of bounds: the merge's ordering and key merge (`phase4/f14-step1`).

## State

Fixed and tested locally; pushed; CI dispatched (see "CI"). Not merged.

## Cause

`run_merge` polled `budget->pressure(estimate)` in 10 ms `sleep_until` slices for up to
`merge_memory_wait`, with `estimate` the sources' file bytes (the audit's IS-2). The estimate is not
the working set (the output streams in chunks, positions spill, sources are mapped), and when it
exceeds the budget's capacity no flush can make room, so every merge slept the full 5 s and then drew
past the budget anyway. At 2x (M = 5 MiB, budget 1.25 MiB) the settle ran three such rounds.

## Fix

The wait is deleted; `merge_memory_wait` is removed from the options (and from the merge's
environment). A merge charges its working memory to the budget as it builds, past the capacity if it
must, as a flush does, and `pressure()` makes the buffers flush sooner.

Why this and not the alternatives:

1. A separate merge share (option 1) is the no-stalls direction, but it is a node-level budget API
   the hosts must supply; nothing in this fix precludes it, and merges already charge the same
   `flush_charge` it would replace.
2. Fewer or smaller inputs (option 2) cannot help: the estimate is not what the merge holds, a forced
   merge must take every input, and fewer inputs cost more rewrites without lowering the peak by much.
3. A condition signalled on release (option 3) is a new cross-thread wait. It cannot work on the
   inline and stepping executors (one baton, a manual clock: nothing else runs to release room), and
   a waiter with no releaser waits forever. The fix removes a wait and adds no wait or signal, so it
   adds no concurrency.

## Done

- The fix, the tests (below), and docs (`phase-3.md`, `reordering.md`); `docs/reviews/*` are
  historical and unchanged.

## Evidence

- Reproduction on main (`.scratch/before/background_merge_test`, new test, old code): a merge under a
  16 KiB budget over about 72 KB of inputs sleeps 500 times (inline) and 395 (pool), 5.00 s each run.
  With the fix: 0 sleeps, well under 1 s.
- Revert check: main's `index.hh` under the new tests fails `budget_below_inputs` and `memory_held`
  on all three executors; the fix passes them.
- Mutant: a single 1 ms sleep under pressure after the charge is caught by `memory_held`
  (`counted.sleeps() == 0`) on all three executors.
- Suites, all 19 tests: RelWithDebInfo pass (28 s); TSan (Homebrew LLVM 23, -O1) pass; ASan+UBSan
  (Debug -O1) pass.
- KV harness, 100,000 records, one thread, durability none, 3 interleaved trials per binary, under a
  light hold (`kv_strata`, document layout; the settle phase, seconds):

| Row | Before (3 trials) | After (3 trials) | Median |
|---|---|---|---|
| fillrandom 2x (M = 5 MiB) | 15.271, 15.306, 15.282 | 0.246, 0.222, 0.392 | 15.28 s to 0.25 s, 62x shorter |
| ycsb_f 2x (1 KiB, M = 49 MiB) | 9.721, 9.785, 10.026 | 0.353, 0.372, 0.239 | 9.79 s to 0.35 s, 28x shorter |
| fillrandom fits (M = 256 MiB), control | 0.254, 0.278, 0.240 | 0.242, 0.268, 0.232 | unchanged |

  (The audit's 15.3 s is the search layout's `kv_strata_search`; main builds the document layout's
  `kv_strata`, which reproduced 15.27 s.)

## Next

- The lead lands on green CI. Branches that set `merge_memory_wait` (`phase4/f14-step1` mentions it
  in `index.hh`'s `merge_env` initializer) need that argument and member dropped when they merge.

## Blocked on

Nothing.

## CI

Run 39516877171 on `eca2d8c` (Kronuz/strata, push-triggered): all eight jobs (freebsd-x86_64,
linux-tsan, linux-x86_64-gcc, linux-arm64-gcc, macos-arm64, linux-asan-ubsan, linux-x86_64-clang,
macos-x86_64) were in progress when I stopped; the lead watches it.
