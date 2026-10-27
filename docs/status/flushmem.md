# flushmem

Branch `perf/flushmem` (strata-wt/flushmem), rebased on main `39c9cc1`. Record: `docs/perf/flushmem.md`.
Libraries (additive, pushed as branches): fusefilter `perf/flushmem` (755b392), termdict `perf/flushmem` (8367263).

## State

Experiments 1 and 2 (filter build takes its hashes over; dictionary blocks stream to the section) built, byte-identical, full suite green except one allocation-count test updated. Measurement and policy (experiment 3) in progress.

## Done

1. fusefilter `filter8::build(vector&&)`; strata's `sorted_encoder` uses it, `filter_build_bytes` is 32 n + 1 MiB.
2. termdict `builder` takes a block sink; the keys section streams into the section sink in merge and flush.

## Next

See `docs/perf/flushmem.md`.

## Blocked on

Nothing.
