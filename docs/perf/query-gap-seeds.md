> Status: HISTORY (2026-10-19) code not on main; its attribution predates the prospect query wins, verify before citing.

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

## E4b plan (written before its A/B)

- **Seeds:** `qgbench seeds` computes, for each term of S's term and union queries and each k,
  the largest f(w, l) over every dominance pair (at least k postings with wdf ≥ w and exact
  length ≤ l), under each preset at the segment's statistics; a union's seed is its terms'
  largest. That is the best any stored pair set can do (F13 stores at most three pairs per
  k), so E4b here is an upper bound on M20. Intersections, phrases, and intersection_union get
  none. Over S: 392 seeds, none above the oracle θ, median 0.831 of it (p10 0.587, p90 0.982).
- **A/B:** as E4a, B reading these seeds instead of the oracle's.
- **Target rows:** term and union `TOP_10`, `TOP_100`, `TOP_1000`.

## Results (2026-10-19, gate VM, one thread pinned, light holds)

A and B are the same binary (`build-e2-base`), B started through a wrapper that sets
`STRATA_SEED_FILE`; the rows the seeds cannot touch (intersection, phrase, negated, every
`COUNT`) are therefore an A/A of the run, and their spread is its noise: within 0.96–1.02 on
the reordered merged index, up to 1.17 on the 16-segment run of E4b. Raw:
`~/bench/results/qexp-e4/` and `~/bench/results/qexp-e4b/ab/`, SHA-256s in
`query-gap-seeds.sha256`.

| Target row (B/A, 95%) | E4a oracle, merged `bm25` | E4a, merged `bm25_xapian` | E4a, 16 segments | E4b pairs, merged `bm25` | E4b, merged `bm25_xapian` | E4b, 16 segments |
|---|---|---|---|---|---|---|
| term `TOP_10` | 0.444 [0.366–0.535] | 0.554 | 0.795 | 0.734 [0.654–0.818] | 0.777 | 0.749 |
| term `TOP_100` | 0.504 [0.423–0.599] | 0.640 | 0.636 | 0.716 [0.641–0.805] | 0.817 | 0.818 |
| term `TOP_1000` | 0.632 [0.536–0.735] | 0.767 | 0.775 | 0.759 [0.680–0.839] | 0.836 | 0.880 |
| union `TOP_10` | 0.705 [0.653–0.756] | 0.739 | 0.811 | 0.923 [0.879–0.965] | 0.977 | 0.893 |
| union `TOP_100` | 0.571 [0.502–0.651] | 0.587 | 0.587 | 0.773 [0.697–0.846] | 0.768 | 0.900 |
| union `TOP_1000` | 0.591 [0.502–0.673] | 0.598 | 0.642 | 0.786 [0.692–0.861] | 0.737 | 0.882 |
| intersection_union `TOP_10`–`TOP_1000` | 0.832, 0.879, 0.905 | 0.861, 0.923, 0.940 | 0.957, 0.850, 0.980 | (no seeds) | | |

E3's counters with the oracle seed (stored bounds) say where it comes from: a term's
`TOP_10` decodes 0.322 of the blocks it decodes unseeded and scores 0.147 of the postings;
a union's 0.735 and 0.853. **H1 is confirmed**: at least 26% of the blocks a union scores,
and 68% of a term's, are blocks the final threshold would have skipped.

## Verdicts

- **E4a passes** D14's item 1 on every term and union `TOP_k` row, under both presets and on
  both indexes. So E10 (M3, bound-ordered traversal) is to be built, and H1's ceiling is
  large: 30–56% of term and union `TOP_k` time.
- **E4b passes** item 1 on term `TOP_10`, `TOP_100`, `TOP_1000` and union `TOP_100`,
  `TOP_1000` (the merged index, `bm25`), with seeds that are the best any stored pairs can
  give (an upper bound on F13's three pairs per k). Item 2 cannot be read from this run's
  untouched rows, which are an A/A at up to 1.17 on the 16-segment index; the seeds change
  nothing outside term and union queries by construction.
- **M20 stays open.** The design keeps M20 only if E4b also passes on top of M3 (because E4a
  passed), which puts E10 before the freeze; E10 is not built. Its bytes: at most 9 pairs of 2
  to 4 bytes per term with skip blocks, 103,578 terms in this segment, so 1.9 to 3.7 MB against
  the whole index's 3.2 MB headroom. If E10 cannot run before the freeze, M20 is out (D5).

## M20 decided (after E10, 2026-10-19)

E10 ran before the freeze (the lead's ruling) and E4b was measured again on top of M3t
(`query-gap-m3.md`): term `TOP_10`, `TOP_100`, `TOP_1000` 0.869, 0.821, 0.879 on S′ on the
reordered merged index and 0.855, 0.808, 0.862 on 16 segments; union 0.840, 0.768, 0.773 and
0.889, 0.832, 0.830. Every row over 1.03 in those runs is one the seeds cannot change (the
two sides are one binary, and a counted multi-segment search takes no seed), inside the
session's A/A spread (to 1.129).

**What the stored form needs.** The seeds measured are, per term and k, the largest f(w, l)
over all dominance pairs under each preset at the segment's own statistics. Two stored pairs
per k reproduce them exactly where the query's statistics are the segment's (one segment, as
here): the pair that maximizes f under `bm25` and the one under `bm25_xapian`, inside F13's
three. Across segments with other averages a stored pair gives a weaker seed than E4b's, never
an unsafe one. `bm25`'s pair stores its length as a code (exact, as here); `bm25_xapian`'s
needs the exact length or the code rounded up, which F13 already specifies.

**Bytes:** two pairs of 2 to 4 bytes for each of k = 10, 100, 1,000, per term with skip blocks
(103,578 in this segment): 1.2–2.5 MB, against the whole index's 3.2 MB headroom, or with the
freeze set's other bytes (A3@128 −1.5 MB, M23 −0.3) about 5 MB.

**Verdict: M20 is in**, at two pairs per k. Owed with it: its D4 checks, which need the
`STRATA_DELETE_EVERY` index (seeds are off in segments with deletes, F13), and the reader's
seed path itself (E4b here read seeds from a file through `search()`'s floor).
