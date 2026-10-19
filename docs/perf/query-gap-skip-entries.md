# Query gap, E6: advance entries, probes, and M23's check

The design's E6 (`docs/design/query-gap.md`): F10's advance-entry layouts against format 12's
skip data, probes into a block for one candidate, and M23's block check, microbenchmarked
over the real reordered merged 5M segment. Decides M16, M22, M23.

## Procedure

- `bench/quick/qgbench` (this branch), Release, clang 18, `-march=native`, one thread pinned
  on the gate VM under light holds; best of 7 or 21 passes per measurement, repeated three
  or five times (the spread below is across repeats).
- Terms: the 50 most frequent terms of S (`top50.txt`, by document frequency; `the`, `a`,
  `of` first), whose skip data E1's counters touch most.
- Traces: **leapfrog**, each S intersection's lead postings sent to its other terms
  (5,978,819 lookups); **sequential**, each block's first ordinal in order (323,866);
  **window**, one lookup per 4,096 ordinals (61,371). Every layout is checked against the
  format on every lookup (the block it names must be the block that holds the target).
- Layouts, built from the segment's lasts and block sizes: F12, the reader's own
  `block_index_for` (level-1 search with its group hint, `decode_group`, a search of the
  decoded entries); A1 (last u32, offset u16); A2 (last − group base u16, or u32 for a wide
  group, offset u16); A3 (lasts, offsets, flags bit-packed at the group's widths); A4 (A2's
  fields as two arrays, the lasts searched with AVX2). Each with one entry per 128 postings
  and per 256. Every layout uses the reader's group lookup (hint, successor, then a search).
- Bytes: over all 103,578 terms with skip blocks (4,372,741 blocks, 220,366 groups), against
  format 12's level-0 last and length fields (LEB128), which F10 replaces.

## Results

ns per lookup, three repeats (min–max):

| Layout | Leapfrog | Sequential | Window | Bytes (MB) | Against format 12's 18.185 MB |
|---|---|---|---|---:|---:|
| F12 (format 12) | 7.2–7.3 | 21.2–24.8 | 89–98 | 18.185 | 0 |
| A1@128 | 10.5–10.8 | 10.0–11.7 | 19.7–21.4 | 26.236 + 0.547 flags | +8.6 |
| A2@128 | 9.4–9.5 | 8.9–10.8 | 17.5–19.1 | 22.037 + 0.547 | +4.4 |
| A3@128 | 14.9–15.2 | 13.9–16.7 | 25.0–27.3 | 16.653 (flags inside) | −1.5 |
| A4@128 | 7.4–7.9 | 7.5–8.9 | 10.7–11.8 | 22.037 + 0.547 | +4.4 |
| A1@256 | 8.8–9.2 | 8.3–9.9 | 13.1–14.2 | 13.312 + 0.547 | −4.3 |
| A2@256 | 8.1–8.4 | 7.7–8.9 | 11.2–12.2 | 11.212 + 0.547 | −6.4 |
| A3@256 | 13.4–13.8 | 12.9–14.9 | 19.1–20.9 | 8.427 | −9.8 |
| A4@256 | 6.4–6.8 | 6.4–7.4 | 7.8–8.4 | 11.212 + 0.547 | −6.4 |

An entry per 256 postings names a pair of blocks, so reaching a target in its second block
decodes the first to learn its last ordinal: half a block decode per touched block on average,
about 28 ns at the FOR decode and check cost below, which outweighs the 1–3 ns its lookup saves.
Projected ns per touched block (lookup plus decode, 57 ns for a FOR block):

| Layout | Leapfrog | Sequential | Inside D15? |
|---|---:|---:|---|
| F12 | 64 | 79–82 | (today) |
| A4@128 | 65 | 65 | no: +4.4 MB against the whole index's 3.2 MB headroom, unless another item pays |
| A3@128 | 72 | 71–74 | yes: −1.5 MB |
| A4@256 | 92 | 92 | yes: −6.4 MB |

The window trace has no block decode (a bound reads entries only), where A4@128 is 8x and A3@128
3.5x faster than format 12.

**Probes** (one target in each of the 50 terms' 323,866 blocks; five repeats at 21 rounds):

| Probe | ns per probe | Ordinal bytes per block |
|---|---|---:|
| FOR: decode and search (`decode_delta`, `lower_bound`) | 64–76 | |
| Bitset: rank by popcount (bitset blocks only) | 7.4–8.9 | |
| Elias-Fano: successor (select0 of the bucket, then a scan) | 24.6–29.2 | 61.7 |
| Stored (FOR or bitset, as the writer chose) | | 38.5 |

Elias-Fano is smaller than the stored codec in 52,214 of the 323,866 blocks (16.1%), saving
871,795 bytes (7% of those terms' ordinal bytes); chosen per block by size, it costs no bytes.

**M23** (127,120 FOR blocks; five repeats at 21 rounds; ns per block):

| Check | Min–max | Median |
|---|---|---:|
| Today: `decode_delta`, then `check_increasing` | 52.6–59.3 | 57.0 |
| Gap − 1 decode (`unpack_delta`, + 1 per lane), header width ≤ 25 bits, last equal to the entry's | 48.7–52.8 | 50.6 |
| Gap − 1 decode with a scalar prefix sum and a 64-bit sum check (as implemented here) | 71.8–83.4 | 77.5 |

Paired by repeat, the width form over today reads 0.85–0.94, below 1 in all five. Bytes: FOR
ordinal blocks at gap − 1 take 426.130 MB against 426.429 MB today (−0.299 MB). The 64-bit
form here is scalar and loses; a SIMD widening sum would be its fair form.

## Verdicts

- **M16: in, A3@128** under D15 as it stands: the lowest projected ns per touched block among
  the layouts inside the budget (72 against A4@256's 92), −1.5 MB, and 3.5x format 12 on window
  bounds, though 12% slower than format 12 on leapfrogs, where format 12's decoded group is
  reused. **A4@128 is the fastest everywhere** (format 12's speed on leapfrogs, 2.8x on scans,
  8x on window bounds) at +4.4 MB: it is the better choice if format 13's joint selection
  finds the bytes (F13-R8, "Choosing F10 to F13 together"); E7's Elias-Fano positions (−35 MB)
  are one place they exist. Validation per decoded block as F10 states.
- **M22: in (Elias-Fano as a block codec, chosen by size).** Its probe beats FOR's decode and
  search 2.6x on the blocks E1's traces touch, and by-size choice costs no bytes; it applies
  to the 16% of blocks where it is smaller, so its projected query effect is about 1% on
  intersections with rare leads (1,473 new-block advances per intersection `TOP_10` query, at
  about 45 ns saved on a sixth of them). Run containers (F11's E3) were not measured: out.
- **M23: in**, by E6's rule (the width form beats `check_increasing` in every paired repeat,
  0.85–0.94) and −0.3 MB; its in-query effect is bounded by `check_increasing`'s share in E1,
  2.3–4.5% of `COUNT` intersection, union, and negated, and 3.4% of term `TOP_10`. K6's tests
  (the 64-bit sum and 25-bit lane forms, a block whose gaps wrap by 2^32, `fuzz_segment`'s
  corpus) go with it to format 13's contributor.

Raw: `~/bench/results/qexp-qg/` (`skip-*.txt`, `skipR-*.txt`, `sizes.txt`, `phases.log`),
SHA-256s in `query-gap-microbench.sha256`.
