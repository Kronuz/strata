# Query gap, E2: M1, M2, and M9

The design's E2 (`docs/design/query-gap.md`): three format-free bound items, each its own
A/B in this order, each kept by D14. E2's measured M1 and M2 are two of E3's calibration
points.

## Plan (written before the A/Bs)

- **M1, range-max bounds.** `max_score(from, upto)` takes the largest block bound among the
  blocks a range covers, a group's bound for each group inside it, and reads a partly covered
  last group's blocks only when its group bound could raise the maximum; the whole-term bound
  only for a range that reaches the tail. The phrase's `range_bound` follows the same rule.
  Hypotheses: H4 (intersection windows), H7 (union windows). E1's counters: 61–63% of
  intersection `max_score` calls resolve at group or term level; E3 projects M1 (exact maxima
  at 128) at −24%, −17%, −12% of intersection `TOP_10`, `TOP_100`, `TOP_1000`, −0.7% to
  −1.6% of union `TOP_k`, −0.6% to −5% of intersection_union.
- **M2, block-max AND.** In the conjunction's bulk top-k, a candidate that passes the window
  check is checked against its lead score plus the bounds of the blocks holding it in every
  other term (skip data, no block decode); when even the lead's block bound cannot compete,
  the lead skips to the end of the shortest of those blocks.
- **M9, bound arithmetic.** A pair's score from its length code directly (`by_code`: under
  `bm25`, `norm_k[code]` depends only on the decoded length), and the skip test
  division-free (`may_beat`) with a margin of 2^-49 that never refuses a competing pair.
- **Target rows** (named before the A/Bs): M1 and M2, union, intersection, and
  intersection_union `TOP_10`, `TOP_100`, `TOP_1000`, and phrase `TOP_k`; M9, term and union
  `TOP_k`.
- **Tests** (`search_test`, built with each switch): M1, a range bound is at least every
  score in 400 random ranges per term and preset and at most today's smallest-covering-entry
  bound, over three synthetic lists of 300,000 documents (densities 1/2, 1/9, 1/70); M2, every
  posting's block bound is at least its score; M9, `by_code` equals today's
  decode-then-score for all 256 codes bit for bit, and `may_beat` never refuses a pair whose
  score beats min. All pass on the gate VM for every variant (`base`, `m1`, `m1m2`, `m9`,
  `m1m2m9`).
- **A/Bs:** in-process, `bench/quick/ab.sh OUT A IDX B IDX 6 3` with S's files as `QUERIES`
  and `TERMS`, on the reordered merged index and the 16-segment index, under `bm25` and
  `SCHEME=bm25_xapian`; then the harness A/B on the target rows beside the fastest engine.
- **Keep:** D14 item 1 on a target row (upper bound below 1.0, geometric mean at most 0.97),
  item 2 on every set and class row of both indexes and both schemes (upper bound at most
  1.03); otherwise the item is removed with its numbers here.
- **Builds:** this branch, Release, clang 18, `-march=native`, one build directory per
  variant: `-DSTRATA_M1=1`, `-DSTRATA_M1=1 -DSTRATA_M2=1`, `-DSTRATA_M9=1`, and all three.
