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
