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

## Results (2026-10-19, gate VM, one thread pinned, light holds)

In-process A/Bs, six rounds of three runs, B over A with 95% bootstrap intervals over
queries, scored per class by the per-tag `ab.py` (`32d598a`). Raw: `~/bench/results/qexp-e2/`
on the gate VM (`<item>/<index>-<scheme>/`, scored tables beside them). `COUNT` rows run code
no item touches, so they are the run's own control: their spread (to 1.03 on clean runs,
to 1.11 on M1's 16-segment runs, which overlapped a `qgbench` build) is the noise and layout
floor an item's `TOP_k` rows must clear.

### M1: removed

| Row (reordered merged, `bm25`) | B/A | 95% |
|---|---:|---|
| intersection `TOP_10` | 1.201 | 1.135–1.285 |
| intersection `TOP_100` | 1.127 | 1.085–1.185 |
| intersection `TOP_1000` | 1.038 | 1.017–1.062 |
| intersection_union `TOP_10` | 0.957 | 0.935–0.980 |
| union `TOP_10`, `TOP_100`, `TOP_1000` | 0.989, 0.989, 0.991 | each interval spans 1.0 |
| phrase `TOP_10` | 1.017 | 1.010–1.024 |
| global `TOP_10` | 1.041 | 1.016–1.071 |

The same holds under `bm25_xapian` (intersection `TOP_10` 1.174) and on the 16-segment index
(1.119, 1.235). Only intersection_union `TOP_10` meets D14's win, and every intersection row
fails its guard. The counters say why (counting builds with and without M1, S's 40
intersections at `TOP_10`): M1 cuts postings scored by 28% (1,490,617 to 1,079,984),
advances by 40%, and decoded blocks by 20%, the work E3 projected, but bound evaluations rise
6.5x (22,131 to 144,553) and pairs evaluated 7.2x (95,506 to 690,906). A conjunction's window
is its rare lead's block, which spans many of a dense term's blocks; M1 evaluates each covered
block's frontier, about 4.8 pairs each, where today's rule returns one group or term bound.

M1b, the same rule without reading a partly covered last group's blocks (its group bound
instead), keeps the pruning (1,099,784 postings scored) and still evaluates 125,243 bounds;
on the reordered merged index under `bm25` it reads intersection `TOP_10` 1.127
[1.067–1.202], `TOP_100` 1.068, `TOP_1000` 1.065, intersection_union `TOP_10` 0.958. Removed
with M1. What would make the rule pay is a block bound that costs one read, which is M17's
first pair (E5); M1 is worth re-measuring on top of M17 if M17 is kept.

### M2: removed

On the reordered merged index under `bm25`: intersection `TOP_10` 1.363 [1.302–1.433],
`TOP_100` 1.313, `TOP_1000` 1.137; every other class within 0.98–1.01. Each candidate that
passes the window check pays a skip-entry lookup and a block bound per other term
(`block_index_for` and the block's pairs), more than the leapfrog advances it saves. Removed;
its 16-segment and `bm25_xapian` runs were not needed once item 2 failed.

### M9: removed

Its target rows on the reordered merged index under `bm25`: term `TOP_10` 1.015
[1.007–1.022], `TOP_100` 1.012, `TOP_1000` 0.998 (term:frequent `TOP_10` 1.025); union
`TOP_10` 1.010, `TOP_100` 0.997, `TOP_1000` 0.996, each interval spanning 1.0 or above it.
No target row meets item 1 on either index or scheme. The rows it should not move moved as
much as those it targets (`COUNT` global 0.988, 16-segment `bm25_xapian` `COUNT` 0.979), so the
0.97–0.99 readings elsewhere are layout and noise, not M9. A first run that overlapped another
agent's light hold read every row about 2% faster, `COUNT` included; it is superseded by this
clean rerun. Removed: the decode-then-reencode it saves is cheap beside the division it
keeps, and the division-free test buys nothing measurable.

### What E2 leaves

No item is kept; E2's base, and so E5's and E4's, is the base itself (`5b67776`). As E3's
calibration points (D5), measured over projected reduction for M1 is negative on every
intersection row (projected −24%, −17%, −12%; measured +20%, +13%, +4%), because E3's exact
source prices a bound at nothing: κ falls outside [0.5, 2] for intersections, so E3 decides
nothing there, and on unions M1's measured change (0.989–0.991, inside noise) against a
projected 0.7–1.6% gives no usable κ either.

Raw files' SHA-256s: `query-gap-e2.sha256`. Builds: `build-e2-{base,m1,m1b,m2,m9}` on the gate
VM from `72a5588` (M1b from the next commit).
