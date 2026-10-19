# Query gap: coverage the freeze rows depend on

The lead's ruling of 2026-10-19: before any freeze row is final, run the term rows on more
than one query, and say what is missing on the gate VM and whether the freeze decisions
depend on it.

## The term rows on the full stratified term set

The quick-tier calibration ran `queries-sample.txt` only, whose term class is one query, so
its term rows (`TOP_10` 0.72x, `TOP_100` 1.02x, `TOP_1000` 1.29x) described one term. These
are the 200 stratified terms (`terms-stratified.txt`, SHA-256 `79255c23…8e65`), strata
reordered merged (E0's binary, `aec8d11`) against Lucene, Lucene `-bp`, and Tantivy, five
trials with the engine order rotated, `NUM_ITER=5`, `WARMUP_TIME=5`, under a light hold;
572 s in all. PISA is left out: its term `TOP_k` rows carry a per-query harness cost (D13).
Raw: `~/bench/results/query-qexp-terms/`; `gate.py --md --target 1.0 --strata
strata-reordered` and the per-stratum split (`strata_split.py`: each query's per-trial best,
its median over trials, less the 88 µs pipe floor; the geometric mean against each
reference, the worst shown).

`gate.py`, class rows:

| Command | strata µs | Lucene µs | Lucene `-bp` µs | Tantivy µs | Floor-subtracted |
|---|---:|---:|---:|---:|---|
| `TOP_10` | 277 | 302 | 259 | 210 | 1.28x [1.02–1.49] |
| `TOP_100` | 502 | 538 | 447 | 384 | 1.17x [0.99–1.45] |
| `TOP_1000` | 986 | 928 | 803 | 761 | 1.17x [1.15–1.20] |
| `TOP_100_COUNT` | 491 | 1,063 | 1,012 | 764 | 0.77x [0.69–1.16] |
| `COUNT` | 149 | 171 | 172 | 156 | 0.93x [0.78–1.21] |

By stratum (fastest reference: Tantivy on every row but medium `TOP_100`):

| Command | Stratum | n | strata µs | fastest | ratio (floor-subtracted) | each reference |
|---|---|---:|---:|---|---:|---|
| `TOP_10` | term:rare | 67 | 177 | tantivy-0.26 | 1.17x | Lucene 0.73, Lucene-bp 0.81, tantivy-0.26 1.17 |
| `TOP_10` | term:medium | 67 | 237 | tantivy-0.26 | 1.16x | Lucene 0.78, Lucene-bp 0.96, tantivy-0.26 1.16 |
| `TOP_10` | term:frequent | 66 | 397 | tantivy-0.26 | 1.85x | Lucene 0.95, Lucene-bp 1.13, tantivy-0.26 1.85 |
| `TOP_10` | term | 200 | 269 | tantivy-0.26 | 1.36x | Lucene 0.82, Lucene-bp 0.96, tantivy-0.26 1.36 |
| `TOP_100` | term:rare | 67 | 197 | tantivy-0.26 | 0.97x | Lucene 0.69, Lucene-bp 0.72, tantivy-0.26 0.97 |
| `TOP_100` | term:medium | 67 | 341 | lucene-10.4.0-bp | 1.02x | Lucene 0.85, Lucene-bp 1.02, tantivy-0.26 1.02 |
| `TOP_100` | term:frequent | 66 | 944 | tantivy-0.26 | 1.60x | Lucene 0.98, Lucene-bp 1.30, tantivy-0.26 1.60 |
| `TOP_100` | term | 200 | 492 | tantivy-0.26 | 1.16x | Lucene 0.83, Lucene-bp 0.98, tantivy-0.26 1.16 |
| `TOP_1000` | term:rare | 67 | 203 | tantivy-0.26 | 1.03x | Lucene 0.64, Lucene-bp 0.66, tantivy-0.26 1.03 |
| `TOP_1000` | term:medium | 67 | 554 | tantivy-0.26 | 1.09x | Lucene 0.98, Lucene-bp 1.08, tantivy-0.26 1.09 |
| `TOP_1000` | term:frequent | 66 | 2,225 | tantivy-0.26 | 1.39x | Lucene 1.09, Lucene-bp 1.33, tantivy-0.26 1.39 |
| `TOP_1000` | term | 200 | 988 | tantivy-0.26 | 1.16x | Lucene 0.88, Lucene-bp 0.98, tantivy-0.26 1.16 |
| `TOP_100_COUNT` | term:rare | 67 | 198 | tantivy-0.26 | 0.96x | Lucene 0.63, Lucene-bp 0.69, tantivy-0.26 0.96 |
| `TOP_100_COUNT` | term:medium | 67 | 347 | tantivy-0.26 | 0.91x | Lucene 0.75, Lucene-bp 0.90, tantivy-0.26 0.91 |
| `TOP_100_COUNT` | term:frequent | 66 | 943 | tantivy-0.26 | 0.59x | Lucene 0.40, Lucene-bp 0.42, tantivy-0.26 0.59 |
| `TOP_100_COUNT` | term | 200 | 494 | tantivy-0.26 | 0.80x | Lucene 0.57, Lucene-bp 0.64, tantivy-0.26 0.80 |
| `COUNT` | term:rare | 67 | 149 | tantivy-0.26 | 0.88x | Lucene 0.72, Lucene-bp 0.73, tantivy-0.26 0.88 |
| `COUNT` | term:medium | 67 | 150 | tantivy-0.26 | 0.89x | Lucene 0.73, Lucene-bp 0.73, tantivy-0.26 0.89 |
| `COUNT` | term:frequent | 66 | 149 | tantivy-0.26 | 0.90x | Lucene 0.73, Lucene-bp 0.73, tantivy-0.26 0.90 |
| `COUNT` | term | 200 | 149 | tantivy-0.26 | 0.89x | Lucene 0.73, Lucene-bp 0.73, tantivy-0.26 0.89 |

**What it changes.** The term rows are above 1.0x on `TOP_10`, `TOP_100`, and `TOP_1000`
(not 0.72x and 1.02x), against Tantivy, and the gap is in frequent terms: 1.85x, 1.60x, 1.39x.
That is the row E4a's oracle seeds (frequent terms 0.22–0.35 of today's time in-process),
E4b's dominance seeds, and E10's term half (frequent terms 0.535) address: the term rows are
where M3 and M20 earn their place, which raises the stakes of E10 and the M20 decision but
changes no other freeze line. **H17** gets its reference: a rare term's floor-subtracted
reference time is about 76 µs at `TOP_10` (strata's 89 µs over 1.17x) and about 68 µs at
`COUNT`, so the probes' fixed 1.3–2.0 µs in-process are 1.9–2.6%, under 3%: **H17 is
refuted**.

## What is missing on the gate VM, and what depends on it

- **E0's `dump` references.** E0 made none on the VM. The base check (`query-gap-base.md`)
  made them with E0's binary (`aec8d11`) beside the base's, on the reordered merged,
  docid-ordered merged, and 16-segment indexes over the gate queries and stratified terms,
  and every pair is byte-identical. No freeze decision depends on E0's own copies; the
  references now exist (`~/bench/results/qexp-base/`).
- **The `STRATA_DELETE_EVERY` index.** Not on the VM (and the disk has about 3 GB free with
  the experiments' copies). It is D4's fourth index: the `verify` and `dump` identity checks
  of an item that lands, and E5's delete case for M17. No freeze decision rests on it: M17 is
  out on timing; M16, M22, and M23 are format items whose D4 checks run on format 13's own
  branch (its T2); and M20's seeds do not apply to segments with deletes (F13), so the delete
  index is M20's correctness check, owed if M20 stays, not an input to its keep line.
- **D17's nested S′ and harness tooling** (the third engine in `bench/ab.sh`): not on this
  base. S′ here is the quick tier's sampler at 100 per class; the harness A/B beside the
  fastest engine ran for none of the items, since none won its in-process A/B before E10.
