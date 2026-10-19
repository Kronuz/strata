# Query gap, E1: attribution (T1)

The design's E1 (`docs/design/query-gap.md`, branch `design/query-gap`): split every row above
1.0x into named parts with counters and profile shares, and give each of H1 to H22 its verdict
by the design's rule. It keeps nothing; it orders the rest of the queue.

## Plan (written before the runs)

- Rows: every class and command of the quick-tier calibration (`docs/status/query-gap.md`,
  the lead's "Calibration, quick tier", gate VM `~/bench/results/query-cal1`) whose
  floor-subtracted ratio against the reference set is above 1.0x.
- Index: the cached 5M reordered merged index (`engines/strata-reordered/idx`, `manifest-3`,
  one segment `seg-200.sx`), Q5; the 16-segment index is that directory's `manifest-2` for
  H20.
- Queries: S, the calibration's `queries-sample.txt` (181) and `terms-sample.txt` (40).
- Counters: a `STRATA_COUNTERS=ON` `strata_engine bench IDX COMMAND 1` over S at every
  command; counts are deterministic, so one run each.
- Profiles: a Release `strata_engine` (counters off) under `perf record -g`, single-threaded,
  `bench IDX COMMAND 20 < CLASS.txt` per class and command, under a light hold.
- Verdicts: the design's rule (refuted below a 3% ceiling on every target row, confirmed at a
  5% ceiling with the counter at its level, open otherwise or while a later experiment is
  owed).
- What would change the queue: a hypothesis with a ceiling under 3% everywhere drops its
  mechanism from the queue; one confirmed above 5% on a freeze item moves it first.
