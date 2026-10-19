# Query gap, E5: score-ordered frontiers (M17) in format-12 bytes

The design's E5 (`docs/design/query-gap.md`), on E2's base (no E2 item was kept, so the base
itself, `5b67776`'s query path).

## Plan (written before the A/B)

- **Index.** Every block and group frontier of the reordered merged 5M segment rewritten
  argmax-first under `bm25` at the segment's own clamped average (264.46), the rest in
  `competitive()`'s order (`qgbench m17`): a pair's encoding does not depend on its place in
  the list, so every offset and size holds, and only the postings section's checksums, the
  table of contents, and the footer change. Term-level pairs (in the dictionary) keep their
  order. 4,593,107 frontiers, 1,314,719 of them (28.6%) reordered. A' = A: one segment.
- **Reader** (`STRATA_M17`): a block's or group's bound under `bm25` is its first pair's,
  w · t₁ / (t₁ + c · K(ℓ₁, A)) · (1 + 2⁻⁴⁰), c = min(1, A / A'), one pair read and evaluated;
  the skip test reads one pair; `bm25_xapian` evaluates every pair, as today. A build flag
  stands in for F7's preset record.
- **Experiment writer** (`STRATA_M17` in `inverted_builder`): the same order at flush, so
  `search_test`'s brute-force top-k and count checks run against argmax-first segments.
- **Tests:** `search_test` under `STRATA_M17` passes (every existing scorer check, both
  presets, on argmax-first segments), and a property over 20,000 random frontiers with A'/A
  from 1/4 to 4 checks the bound against every pair's score at A'; the two mutants (c set to
  1, and the first pair's score read at A') each fail it somewhere.
- **D4 on the rewritten segment:** `check` passes (5,032,104 documents, 591,115,847 postings,
  the same counts as the original); `verify` (pruned top-k at 10, 100, and 1,000 against full
  scans, both presets) at 0 mismatches over the 962 gate queries and the 200 stratified terms.
  The multi-segment index with different averages and the `STRATA_DELETE_EVERY` index are not
  built yet (disk), so the 16-segment rows and those checks are owed if M17 wins here.
- **Target rows:** term, union, and intersection `TOP_10`, `TOP_100`, `TOP_1000`.
- **A/B:** in-process on S, A the base binary on the original segment, B the M17 binary on
  the rewritten one (each binary serves its own index, as L15 does), under `bm25` and
  `SCHEME=bm25_xapian`, six rounds of three runs, one thread pinned. Keep by D14.

## Results (2026-10-19, gate VM, one thread pinned, light holds)

In-process on S (`ab.sh … 6 3`, scored per tag by `32d598a`'s `ab.py`), raw in
`~/bench/results/qexp-e5/ab/` on the gate VM (SHA-256s in `query-gap-pair-order.sha256`).

| Row, reordered merged | `bm25` on S | `bm25` on S′ (rerun) | `bm25_xapian` on S |
|---|---|---|---|
| term `TOP_10` | 0.980 [0.964–0.996] | 0.976 [0.969–0.982] | 1.002 [0.991–1.012] |
| term:frequent `TOP_10` (not named before) | 0.950 [0.936–0.965] | 0.958 [0.954–0.962] | 1.009 |
| term `TOP_100` | 0.983 [0.970–0.994] | 0.990 [0.985–0.995] | 1.005 |
| term `TOP_1000` | 1.017 [1.005–1.028] | 1.008 [1.003–1.013] | 1.005 |
| union `TOP_10`, `TOP_100`, `TOP_1000` | 0.993, 1.010, 1.008 | 0.998, 1.002, 1.006 | 0.994, 0.990, 0.987 |
| intersection `TOP_10`, `TOP_100`, `TOP_1000` | 0.999, 1.001, 1.011 | 0.994, 0.996, 0.998 | 0.989, 0.989, 1.010 |
| intersection `TOP_100_COUNT` (guard) | 1.028 [1.022–1.035] | 0.997 [0.995–1.000] | 1.001 |

S′ here is the quick tier's sampler at 100 per class (`bench/sample_queries.py`, seed 7):
361 gate queries and 100 stratified terms (`~/bench/results/sprime/`, SHA-256s
`b58d248d…862c` and `bd196670…510a`), not D17's nested draw, which is not on this base.

**Verdict: removed by D14.** No named target row meets item 1: term `TOP_10`, the best,
reads 0.976 on S′ with its interval above 0.97. The S run's one guard failure (intersection
`TOP_100_COUNT`, a path M17 does not touch) reran at 0.997 on S′, so it was noise; item 2
holds. The effect is real and where H2 put it: one pair read and evaluated per bound instead
of about 3.6, worth 4.2% on frequent terms' `TOP_10` and 2.4% on the term class, nothing on
unions and intersections, whose bound evaluations are 2–4% of their time (E1). Under
`bm25_xapian` it is neutral by construction.

**M1 on top of M17** (A: M17's binary, B: M17 with M1b, both on the rewritten segment, S,
`bm25`): intersection `TOP_10` 0.962 [0.914–1.017], `TOP_100` 1.038, intersection_union
`TOP_10` 0.954 [0.935–0.972], `TOP_100` 0.965; that run's own control moved too (`COUNT`
intersection 0.889 on code neither binary changes), so it shows only that M17 removes most of
M1's cost (M1 alone read 1.13–1.20 on intersection `TOP_10`), not a win. A clean rerun of
M1 over M17 is the measurement that would reopen both; by the queue's rule, M17 is out of
format 13 and M1 stays removed.

**The checks that ran:** `check` on the rewritten segment, `verify` (pruned top-k at 10,
100, 1,000 against scans under both presets) at 0 mismatches over the 962 gate queries and
200 stratified terms, `search_test` with the experiment writer and reader, and the property
test with its two mutants failing. Not built (disk, and moot once item 1 failed): the
multi-segment index with different averages and the `STRATA_DELETE_EVERY` index, and the
16-segment rows.
