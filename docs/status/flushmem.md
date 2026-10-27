# flushmem

Branch `perf/flushmem` (strata-wt/flushmem), rebased on main `39c9cc1`. Record: `docs/perf/flushmem.md`.
Libraries (additive, pushed as branches): fusefilter `perf/flushmem` (755b392), termdict `perf/flushmem` (8367263); strata's pins point at them.

## State

Done and measured on macOS x86_64: `budget_peak` 30% smaller (fill), 38% (read), 14% (ycsb_a); one 1M-key merge 101 to 61 MiB; segment bytes identical, throughput unchanged, RSS and dirty footprint unchanged. Full ctest (53) green. The cap itself does not hold (a merge's scratch is 61 B per output key). Linux and FreeBSD not measured.

## Done

1. fusefilter `filter8::build(vector&&)`; `sorted_encoder` uses it; `keys::filter_build_bytes` is 32 n + 1 MiB.
2. termdict `builder` takes a block sink; the keys section streams into the section sink in merge and flush.
3. A streamed merge frees its versions before the filter build.
4. Policy: waiting and serializing measured as no help (single merge dominates); smaller merges argued, not built.

## Next

1. Bound a merge's scratch: a key filter partitioned by hash prefix (format change, needs its own proof) and built at the tail after the merge order is released.
2. Linux and FreeBSD measurement of the same A/B.
3. Bump Detent's pins when it wants the libraries.

## Blocked on

Nothing.
