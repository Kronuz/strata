# Query gap, E10: bound-ordered traversal (M3)

The design's E10 (`docs/design/query-gap.md`), run before the freeze because E4a passed and
the lead ruled (2026-10-19) that M20 is decided on evidence: E4b is measured again on top of
it.

## Plan (written before the A/Bs)

- **Mechanism** (`STRATA_M3` for the A/B builds): in a collection whose hits may arrive out
  of ordinal order (the collector was given the segment, as every reordered segment and every
  multi-segment search is, so ties compare docids), a term scores its tail, then its groups in
  descending group-bound order, each from a fresh iterator, with today's per-block skip
  inside a group, and stops at the first group whose bound cannot beat θ. A union cuts the
  segment into ranges of 64Ki ordinals, bounds each by its clauses' largest group bounds over
  the range (a clause's term-level bound where the range reaches its tail), visits ranges in
  descending summed bound, and runs today's windowed MaxScore inside each range on fresh
  clauses, until a range cannot compete. A docid-ordered single segment keeps today's path.
- **Costs it pays** (QG-R18): a fresh iterator per group or range (each re-decodes a group's
  level 0), and every group's bound computed up front.
- **Target rows:** term and union `TOP_10`, `TOP_100`, `TOP_1000`.
- **Checks:** `search_test` built with `STRATA_M3` (its multi-segment cases take the new
  path); `verify` (pruned top-k against scans, both presets) over the gate queries and the
  stratified terms on the reordered merged segment.
- **A/Bs:** in-process on S, the base against M3, reordered merged and 16-segment indexes,
  `bm25` and `bm25_xapian`, six rounds of three runs, one thread pinned, light hold; then
  E4b's dominance seeds on top of M3 (A: M3, B: M3 with the seeds). Keep by D14.
