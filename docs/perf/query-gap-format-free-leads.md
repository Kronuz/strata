# Query gap: format-free decode leads

Two measurements from H22's microbenchmark (`query-gap-codec.md`) that change no stored byte
and so belong to the format-free queue, not to format 13. Neither is built; each line below
is a lead with its measured kernel gain and its projected share, for the queue to schedule.

## The ordinal decode kernel: simdcomp's d1 over bitpack's integrated delta

- **What:** bitpack's FOR block is SIMD-BP128's vertical layout (128 integers in four 32-bit
  lanes, 16 · b bytes), and both decode a block of deltas with an integrated prefix sum. On
  the same bytes, simdcomp's `simdunpackd1` (BSD-3-Clause; PISA bundles it) decodes 0.223–0.261
  ns per posting against bitpack's `unpack_delta` at 0.280–0.331: about 20% faster
  (`qgbench codec`, 323,866 blocks of the 50 most frequent terms of S, three repeats, one
  thread on the gate VM).
- **Where it would land:** `bitpack`'s `unpack_delta_simd` (its four-lane prefix and carry,
  `bitpack.hh:152-176` at the pinned `abf8972`), a change in the `bitpack` library, additive
  for Detent, ported under simdcomp's notice or reimplemented from its method.
- **Projected effect:** `bitpack::decode_delta` is 8.8% of union `COUNT`, 4.8% of
  intersection `COUNT`, and 1% or less of the `TOP_k` rows in E1's profiles (inclusive
  shares, `chainshare.py`), so 20% of it is at most about 1.8% on union `COUNT` and under 1%
  elsewhere: below D14's 3% on every row as a single item; worth carrying only with the next
  `bitpack` change or as part of E12's vectorized block work.
- **Platforms:** SSE2 and NEON paths both; provisional until arm64 runs (D12).

## wdf blocks: FOR instead of PFOR

- **What:** the writer stores each block's wdfs with `bitpack::encode_pfor`, which takes
  exceptions when they make the block smaller. Decoding those wdfs costs 0.233–0.262 ns per
  posting; the same wdfs as FOR decode in 0.148–0.158 (about 38% less time) and take 21% more
  wdf bytes (0.613 against 0.506 bytes per posting over those blocks). 81,992 of the 127,120
  stored FOR blocks' wdf blocks carry exceptions.
- **Why format-free:** `bitpack::decode` reads FOR or PFOR from the block's header byte, so a
  writer that chose FOR, or chose by a size margin, writes valid format-12 (and 13) segments
  and needs no reader change.
- **Projected effect:** wdf decoding (`postings_iterator::wdfs`) is 3.2–3.7% of intersection
  `TOP_k` and 1% or less of the other rows profiled (E1), so 38% of it is about 1.4% at most;
  the bytes are the larger question: 21% more wdf bytes is several MB on the 5M postings
  section, which carries 11.9 MB over keyed `-bp` already. A per-block rule (FOR unless PFOR
  saves more than a margin) is the shape that could pay for itself; it needs `poststats`'
  byte projection before an A/B.

Raw: `~/bench/results/qexp-qg/codec-{1,2,3}.txt`, SHA-256s in `query-gap-microbench.sha256`;
profile shares from E1's `prof/` (`query-gap-attribution.sha256`).
