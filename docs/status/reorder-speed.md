# Reorder speed (perf/reorder-speed)

Worktree `strata-wt/reorder-speed`, branch `perf/reorder-speed`, performance engineer.
Measurements: docs/perf/reorder-speed.md.

## State

Exact bisection changes in (A, E2, E3, E4): 1M bisection alone 142 s -> 80 s on one
thread, the same order. Next: the interleaved merge series, then fewer iterations.

## Done

- `9be7931` phase times for the reordering merge (forward, bisection, the rest).
- `068b9ac` (A) a term's move gain once per iteration and side.
- `12add4e` (E2) degree-step table, logarithms hoisted.
- `e4636ea` (E3) prefetch rows ahead.
- `69c6fa1` (E4) partial sorts of the gain lists.

## Next

- Interleaved 1M merges, base vs kept, one thread (light) and eight (strict); byte
  identity of the merged segments.
- Fewer iterations: postings bytes and the quick query sample.
- Top levels on all threads.
- The gate VM at 1M against Lucene `-bp`.

## Blocked on

Nothing.
