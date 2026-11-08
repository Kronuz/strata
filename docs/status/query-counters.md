# query-counters: E1's counting build (query-gap K1)

Branch `perf/query-counters` from origin/main `32d1d52`, worktree
`~/code/Kronuz/strata-wt/query-counters`. Design: `docs/design/query-gap.md` (branch
`design/query-gap`), E1 and "First steps" item 4; done criterion: `docs/status/query-gap.md` K1.

## State

The code review's REJECT (`docs/reviews/query-counters-code.md`) reworked, CR1 to CR4 and
CR6 to CR8, on origin/main `69a3f8c`; awaiting the review's re-check. CR5 is the architect's.

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

- Zero cost when off: `strata_engine.cc.o` (Release, Apple clang, macOS x86_64) built with the
  option off is byte-identical to origin/main `32d1d52`'s, same build directory and path:
  SHA-256 `11bc6dbea28492275030b2bea21d2d2f9e0e71e1c1aff478d3eaaff1e38adeca` for both.
- `counters_test` passes locally (exact counts, including the bitset case).
- 100k smoke (`idx-base-100k`, one segment, the first 40 phase-2 queries, TOP_10 and COUNT):
  totals are consistent. `the` TOP_10: 487 bitset blocks and a tail decoded, 432 blocks and 7
  groups skipped by bound, 501 bound evaluations over 8,379 pairs, 6,311 postings scored, θ
  0.170. Term COUNT is all zeros (exact count). Skipping a block by `advance` decodes the block
  it lands on, so skipped blocks still show as decoded ones.

- CI run 39168536497 at `52a1cd4`: all eight jobs green (linux gcc, clang, arm64-gcc, ASan and
  UBSan, TSan; macOS arm64 and x86_64; FreeBSD). Merged origin/main `d8d219c` (ASan at -O1):
  `a0fdda3`.

- Review rework: merged origin/main `69a3f8c` (layers, the inverter); `counters_test`
  ported to the plugin API, `0aa306a` (CR3). θ is the k-th score at a collection's end on
  every path, with an exactly-k case on the collector's and the heap's (CR1).
- CR2: `search()`'s generic loop counts `rejected_by_bound` (candidates + rejected = matches
  on one segment and on two); `count()`'s windows are their own counter, `count_windows`
  (a 4,096-ordinal fill, not a top-k window; `TOP_*_COUNT` runs both); the exclusion's
  counting window counts its excluded advance and each included document once, by
  popcount; a short term's bound walk counts as `bound_tails`, leaving `blocks_tail` the
  iterators'. Each check fails on the old hooks.
- CR4: cases for a union top-k (windows, the skipped one, `max_score` by group and term), a
  conjunction top-k (its lead's 41 windows, 4 skipped, `max_score` by block), a term ended
  by its bound (`skipped_terms`), and a negated top-k (`included` = candidates). Mutation
  check: dropping any one of the 27 counters' `STRATA_COUNT`s fails `counters_test`.
- CI job `linux-x86_64-gcc-counting`: configures `STRATA_COUNTERS=ON`, builds
  `strata_engine` and `counters_test`, runs the test, and benches two queries on the
  `wide-skips.sx` fixture, checking the header, the class lines, and nonzero counts (CR4).
- CR6: a counting `strata_engine` refuses `serve` and `churn`, the timed modes. CR7: the
  identity evidence above names the host's platform (x86_64, not arm64). CR8: strata declares
  `metrics` at `completion`'s own pin (`c975765`); `counters.hh` states the
  one-definition rule; the counters' includes sit in the project block.
- Zero cost when off, after the merge: every strata object of `bdb072e` (Release, Apple
  clang, x86_64, from scratch, one source path and build directory, the review's
  `objid.sh`) is byte-identical to origin/main `69a3f8c`'s, 34 of 34 (`counters_test`
  aside); `strata_engine.cc.o` `67ee80f5…05b7` in both.
- CI run 39215775159 at `bdb072e`: the counting job, linux gcc, arm64 gcc, and macOS arm64
  green; the rest were still running at handoff.

## Next

- The code review's re-check (K1), then E1's runs by the performance engineer.
- CR5's definitions, the architect's to rule before E1's verdicts: `bound_evals` mixes
  memoized bounds over a list's pairs with block tests that stop at the first competing pair
  (H2); `advances_new_group` counts an unstarted iterator's first advance, a move into the
  tail, and an advance on an exhausted iterator (H5). θ is settled as the k-th score (CR1).
- `tail_last()` decodes a tail's ordinals uncounted (CR2 item 4's last note).
- Not yet counted (E1 lists them): `max_score` resolutions by caller and clause density;
  candidates whose block bound competes (H6); H11's required visits; positions decoded and
  headers stepped (H13); the phrase scorer's own bounds and scores (its rows show candidates
  without postings scored).
- Allocation counts through `Kronuz/allocators` (additive change in that repository).
- `poststats`' frontier-length histograms per level: no `poststats` tool on main yet.

## Blocked on

Nothing.
