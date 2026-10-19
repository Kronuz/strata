# Query gap, E4: threshold seeds

The design's E4 (`docs/design/query-gap.md`). E4a is the oracle: H1's ceiling, M3's and
M20's. E4b (F13's dominance-pair seeds) runs only when E4a passes.

## Plan (written before the A/B)

- **E4a.** `strata_engine` reads θ_k per query, k, and scheme from a `dump` file named by
  `STRATA_SEED_FILE` and passes the double just below it to `search()` as a floor, which seeds
  the collector (`top_collector::seed`) and the scorer's minimum (`top_k`'s floor). A is the
  flag off, B on: the same binary (`build-e2-base`, this branch), the same index. The seed
  file is the base check's dumps of the reordered merged index (gate queries and stratified
  terms, SHA-256s in `query-gap-base.md`), which contain S.
- **Target rows:** term, union, and intersection_union `TOP_10`, `TOP_100`, `TOP_1000`
  (seeds do not apply to conjunctions or phrases, and counting ignores a floor).
- **Keep or remove:** E10 (M3) is built when E4a passes D14 on a term or union `TOP_k` row;
  otherwise H1 is refuted, and M3 and M20 are out. M20 also needs E4b to pass on a target row
  with its bytes inside D15.
