# query-counters: E1's counting build (query-gap K1)

Branch `perf/query-counters` from origin/main `32d1d52`, worktree
`~/code/Kronuz/strata-wt/query-counters`. Design: `docs/design/query-gap.md` (branch
`design/query-gap`), E1 and "First steps" item 4; done criterion: `docs/status/query-gap.md` K1.

## State

Built and checked locally; awaiting CI and the code review (K1 needs one before E1's counts).
Allocation counts and `poststats`' histograms remain (Next).

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

- Zero cost when off: `strata_engine.cc.o` (Release, Apple clang, macOS arm64) built with the
  option off is byte-identical to origin/main `32d1d52`'s, same build directory and path:
  SHA-256 `11bc6dbea28492275030b2bea21d2d2f9e0e71e1c1aff478d3eaaff1e38adeca` for both.
- `counters_test` passes locally (exact counts, including the bitset case).
- 100k smoke (`idx-base-100k`, one segment, the first 40 phase-2 queries, TOP_10 and COUNT):
  totals are consistent. `the` TOP_10: 487 bitset blocks and a tail decoded, 432 blocks and 7
  groups skipped by bound, 501 bound evaluations over 8,379 pairs, 6,311 postings scored, θ
  0.170. Term COUNT is all zeros (exact count). Skipping a block by `advance` decodes the block
  it lands on, so skipped blocks still show as decoded ones.

## Next

- Code review of this branch (K1), then E1's runs by the performance engineer.
- Not yet counted (E1 lists them): `max_score` resolutions by caller and clause density;
  candidates whose block bound competes (H6); H11's required visits; positions decoded and
  headers stepped (H13); the phrase scorer's own bounds and scores (its rows show candidates
  without postings scored).
- Allocation counts through `Kronuz/allocators` (additive change in that repository).
- `poststats`' frontier-length histograms per level: no `poststats` tool on main yet.

## Blocked on

Nothing.
