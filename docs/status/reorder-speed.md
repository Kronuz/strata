# Reorder speed (perf/reorder-speed)

Worktree `strata-wt/reorder-speed`, branch `perf/reorder-speed`, performance engineer.
Measurements: docs/perf/reorder-speed.md.

## State

Done, ready for review. 1M force merge with reordering, Mac, interleaved: 311.9 s -> 92.5 s
on one thread, 104.1 s -> 54.4 s on eight; bisection 264.9 s -> 46.3 s and 57.5 s -> 7.9 s.
Gate VM, 1M: 58.3 s on eight threads and 94.7 s on one, against keyed Lucene `-bp`'s
103.1 s (5 sources). Postings +0.018% (parts of 64), queries within the A/A's spread;
docid-order segments byte-identical to main's. CI's fast jobs green at `ce011a7`.

## Done

- `9be7931` phase times for the reordering merge (forward, bisection, the rest).
- `068b9ac` (A) a term's move gain once per iteration and side.
- `12add4e` (E2) degree-step table, logarithms hoisted.
- `e4636ea` (E3) prefetch rows ahead.
- `69c6fa1` (E4) partial sorts of the gain lists.
- `ba8ec0e` (P) the top levels' large parts on all threads.
- `ce011a7` (F) parts of 64 documents; T7's pinned hash follows.
- Removed: fewer iterations (+0.11% to +0.28% postings), parts of 128 (top-10 1.05× to
  1.12× slower).

## Next

- The gate's T3 row at 5M (projected about 830 s on one thread, 500 s on eight).
- Starting from reordered sources' order (idea 3), with a benchmark that merges
  reordered segments.
- The owner's call: six iterations halve the bisection's CPU for +0.11% postings.

## Blocked on

- **The executor (architect).** D4 says the threads come from the host executor; as
  built (`7a4afca`, kept by P) they are bisection's own, started per merge. Moving them
  onto the executor needs a design call: `pool_executor` caps an index's group at one
  running task (`per_group`), and an index without a host executor runs merges on an
  inline executor, so posting helpers to the merge's group would serialize or deadlock.
  Options: a separate group per reordering merge, or an executor hook for fork-join work.

## Notes for the lead

- At 16:04 a light build (nice 19, 4 jobs, under a minute) overlapped the first minute of
  the `scale-merge` strict hold; I gate every step on `quiet check` since.
- On the gate VM, `engines/lucene-10.4.0-st/idx` was replaced (rm -rf, then a keyed
  `-bp` 1M index, `LUCENE_RAM_MB=256`). No query-trial marker referenced it; rebuild it
  if a later row expects the old one. My other VM files are removed.
