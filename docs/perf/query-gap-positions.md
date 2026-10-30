> Status: HISTORY (2026-10-19) code not on main; its attribution predates the prospect query wins, verify before citing.

# Query gap, E7: positions, offline

The design's E7 (`docs/design/query-gap.md`): what position decoding costs the phrase rows,
the positions' encoding statistics, and the candidates' bytes and decode speed. Decides M21
(a positions codec field, F14).

## Procedure

- Shares from E1's phrase and two-phase-critic profiles (`chainshare.py` over the inline
  chains; inclusive, so they overlap): `positions_reader::decode_block` (the block decode,
  stepping over headers to reach it, and its region checks), `bitpack::encoded_size` (the
  stepping alone), `positions_reader::read`, `phrase_freq`.
- Statistics and bytes over every position of the reordered merged 5M segment (1,330,791,234
  positions, `qgbench sizes` with `POSITIONS=1`): each term's stream as stored (per document,
  the first position absolute, the rest as gaps) re-encoded per 128-value block as today's
  PFOR (`bitpack::encode_pfor`), FOR, Elias-Fano over the block's prefix sums, and "split
  firsts" (documents' first positions in a stream of their own, the gaps in another: the
  part of P13's "deltas against the document's length" that changes the gap stream).
- Decode speed over the 50 most frequent terms' 2,308,629 full blocks (295,507,553
  positions), with the prefix sum, best of 7 (`qgbench positions`): PFOR and FOR.

## Results

| Profile share (E1) | phrase rows | two-phase-critic |
|---|---|---|
| `decode_block` (decode, stepping, region checks) | 8.1–10.0% | 29.2–39.5% |
| `encoded_size` (stepping over headers) | 0.9–1.8% | 9.3–10.8% |
| `positions_reader::read` (incl. the above) | 27.3–29.6% | 43.2–57.7% |
| `phrase_freq` (incl. reads) | 62.6–63.7% | 54.2–75.7% |

| Whole segment | Bytes (MB) | Against PFOR |
|---|---:|---:|
| PFOR (today) | 1,467.5 | |
| FOR | 1,563.3 | +95.7 |
| Elias-Fano per block | 1,432.3 | −35.3 |
| Split firsts (PFOR, two streams) | 1,462.2 | −5.3 |

79.2% of the segment's position blocks carry exceptions (90.0% of the frequent terms'
blocks, 6.5 exceptions each). Widths of the frequent terms' blocks: 5 bits 27.6%, 6 bits
27.6%, 7 bits 26.2%, 8–9 bits 10.4%, wider 6.1%. Decode with the prefix sum: PFOR 1.187 ns per
position, FOR 1.077 (−9%).

## Verdict: M21 out

No codec projects 3% on a phrase row. Decoding is at most 8–10% of a phrase row (including
stepping and region checks), so FOR's 9% faster decode projects under 1%, and FOR costs
95.7 MB, which the positions row (0.980x keyed `-bp`) cannot carry. Elias-Fano saves 35.3 MB
but decodes by select rather than by a SIMD unpack; its decode was not measured, and it
would need to be several times faster than PFOR, which no Elias-Fano decode is, to reach 3%
on a phrase row. Split firsts saves 5.3 MB and changes nothing on the decode path. Format 13
keeps format 12's positions and no codec field (F14's default).

Two things E7 hands on. Two-phase-critic spends 29–40% in `decode_block` and 9–11% stepping
over position-block headers from the postings block's first position block on every
candidate: that is M10's (format-free: step once per postings block). And Elias-Fano's 35.3 MB
of positions is the largest byte source measured for format 13's joint selection, if a
selection needs bytes and EF's decode passes its own A/B.

Raw: `~/bench/results/qexp-qg/` (`qg-positions.txt`, `sizes.txt`) and E1's profiles,
SHA-256s in `query-gap-microbench.sha256` and `query-gap-attribution.sha256`.
