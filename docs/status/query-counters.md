# query-counters: E1's counting build (query-gap K1)

Branch `perf/query-counters` from origin/main `32d1d52`, worktree
`~/code/Kronuz/strata-wt/query-counters`. Design: `docs/design/query-gap.md` (branch
`design/query-gap`), E1 and "First steps" item 4; done criterion: `docs/status/query-gap.md` K1.

## State

In progress: counters, bench columns, and the unit test written; build and zero-cost check
pending the host hold.

## Done

- `include/strata/counters.hh`: per-thread `kronuz::metrics::Counter`s behind
  `STRATA_COUNTERS`; every `STRATA_COUNT` is `((void)0)` when it is off. Counted: blocks decoded
  by kind (FOR, bitset, tail) and bitset blocks OR'd undecoded; groups decoded; advances (moving,
  in block, new block, new group); `max_score` resolutions (block, group, term); bound
  evaluations and pairs evaluated; blocks, groups, and terms skipped by bound; windows (total,
  skipped); postings scored; candidates (scored, rejected by a bound, collected); excluded
  advances and included candidates (H12); the final threshold θ.
- CMake: `STRATA_COUNTERS` (OFF) defines it for `strata_engine` only; `counters_test` always
  counts.
- `strata_engine bench`: in the counting build, the last run's counters as extra columns after
  the query, a `#` header line, and `# class` sums per first tag (queries, sums, mean θ).
- `test/counters_test.cc`: exact counts for a full scan, an advance sequence, and a pruned
  top-1 under a floor on a 20,508-document segment.

## Next

- Zero-cost evidence: `strata_engine.cc.o` with the option off against origin/main's.
- 100k smoke check of the counter totals.
- Not yet counted (E1 lists them): `max_score` resolutions by caller and clause density;
  candidates whose block bound competes (H6); H11's required visits; positions decoded and
  headers stepped (H13); phrase scorers' own bounds.
- Allocation counts through `Kronuz/allocators` (additive change in that repository).
- `poststats`' frontier-length histograms per level: no `poststats` tool on main yet.

## Blocked on

Nothing.
