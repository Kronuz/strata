# Query gap, H22: decode cost per posting against SIMD-BP128

The lead's ruling QG-R31 adds H22; F13-R44 makes its verdict a freeze criterion (format 13's
F11, the H22 row): strata's decode cost per posting against SIMD-BP128, PISA's codec.

## Procedure

- `qgbench codec` over the 50 most frequent terms of S on the reordered merged 5M segment:
  323,866 full blocks (127,120 stored FOR, 196,746 stored bitset; 81,992 of the FOR blocks'
  wdf blocks carry PFOR exceptions), 41.5 million postings. One thread pinned, best of 7,
  three repeats (min–max below).
- **The reader as stored:** a fresh `postings_iterator` per term through `for_each_until`,
  wdfs read: skip data, bitset and FOR blocks, checks, everything a scan pays.
- **Kernels, each codec's blocks back to back in one buffer** (every kernel streams only its
  own bytes), re-encoded from the same decoded postings: bitpack's FOR with its integrated
  delta decode (today's ordinal kernel), with and without `check_increasing`; bitpack's PFOR
  for wdfs (what the writer stores) and FOR; SIMD-BP128 from simdcomp (BSD-3-Clause, PISA's
  bundled copy, compiled into `qgbench`, not vendored) as PISA's `simdbp_block` uses it
  (`simdunpack` of gaps − 1, then PISA's scalar prefix sum) and simdcomp's integrated
  `simdunpackd1`; SIMD-BP128 for freqs − 1.

## Results (ns per posting)

| Decode | Min–max |
|---|---|
| strata reader, as stored, wdfs read | 1.680–1.692 |
| bitpack FOR ordinals, integrated delta | 0.280–0.331 |
| bitpack FOR ordinals + `check_increasing` | 0.359–0.416 |
| SIMD-BP128 docs as PISA decodes them (unpack, scalar prefix) | 0.684–0.786 |
| SIMD-BP128 d1, simdcomp's integrated kernel | 0.223–0.261 |
| bitpack PFOR wdfs (as stored) | 0.233–0.262 |
| bitpack FOR wdfs | 0.148–0.158 |
| SIMD-BP128 freqs − 1 | 0.108–0.131 |

Bytes per posting over the same blocks, all re-encoded as FOR: ordinals 0.522, wdfs PFOR 0.506
(FOR 0.613); SIMD-BP128 docs and freqs together 0.848 against bitpack's 1.028, because PISA
stores gaps − 1 and freqs − 1 (M23's gap − 1 is the ordinal half of that; it saves 0.3 MB on
the whole segment's FOR blocks, E6) and because these 50 terms' dense blocks, which strata
stores as bitsets, are here forced into FOR.

## What it says

1. **The layout is already SIMD-BP128's.** bitpack's block is 128 integers in four 32-bit
   lanes, 16 · b bytes, the same vertical layout (`bitpack.hh`'s header comment), so
   SIMD-BP128 would change no stored byte: no codec code and no format field.
2. **PISA's own decode path is slower than strata's kernel**: unpack and a scalar prefix sum
   cost 0.68–0.79 ns per posting against bitpack's integrated 0.28–0.33. PISA's lead on the
   query rows is not its codec.
3. **simdcomp's integrated d1 kernel is about 20% faster than bitpack's** (0.22–0.26 against
   0.28–0.33) on identical bytes. Porting it into `bitpack` (under its notice) is a
   format-free kernel change; at E1's shares (`decode_delta` at most 8.8% of union `COUNT`,
   1–5% elsewhere) it projects at most about 2% on any row.
4. **wdf blocks:** PFOR's exception patching costs 0.08–0.10 ns per posting over FOR (60%
   more), and FOR costs 21% more wdf bytes; `bitpack::decode` reads either from its header,
   so the writer's choice is format-free too.
5. **The reader as stored costs 1.68 ns per posting**, five times the ordinal kernel: the
   rest is the iterator around it (skip data, bitset expansion, wdfs, checks), which is what
   F10's entries, M23, and the query items address, not the codec.

**Verdict for the freeze: no change.** Format 13 keeps format 12's FOR block (it is
SIMD-BP128's layout) and needs no codec code or reserved structure for H22; the faster d1
kernel and the wdf codec choice are format-free and go to the format-free queue (E12's
neighborhood) with their projections above. H22 is refuted as a format-dependent part of
PISA's lead.

Raw: `~/bench/results/qexp-qg/codec-*.txt`, SHA-256s in `query-gap-microbench.sha256`.
