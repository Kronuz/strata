# Search latency over sorted runs (rocksdb-parity msg-1)

Question: sorted runs (`docs/mechanisms/sorted-runs.md`) keep a sequential load's flushes as up to
`max_runs` (64) segments that never merge, where the tiers kept a few. A search query visits every
segment. What does that cost term, phrase and AND queries, at p50 and p99?

## Method

Probe `bench/kv/probes/rocksdb-parity/search-runs.sh` (corpus and report in `search_runs.py`):

- prospect's `prospect_engine` (prospect `c99e6be`) built against this strata checkout, with
  task at strata's own pin and one added line: `STRATA_MAX_RUNS` sets `merge_policy::max_runs` for
  its `index` command. Release, Ninja.
- Corpus: 550,000 documents of 20 to 60 Zipfian words over 30,000 (prospect's quick-tier synthetic
  shape), ids zero-padded so the load is in key order (the key is `Q` + id). `index` with a 4 MiB
  buffer, auto merge on, one commit at the end.
- Indexes at the same commit: A and A2 at `max_runs = 0` (the tiers alone, built apart: the A/A),
  B at 64 (the default), C at 16 (a lower cap).
- Queries, 200 each: terms at log-uniform frequency rank, two-word phrases sampled from documents,
  and `+a +b` over two log-uniform terms. `TOP_10` (BM25 top ten) and `COUNT` (every match; its
  count is the identity check against A).
- `prospect_engine bench` over the index snapshot, best of 3 runs per query, the side order
  rotating each round. A query's latency is its median over rounds; p50 and p99 are over queries;
  "per query /A" is the geometric mean of per-query ratios to A with a 95% bootstrap interval.

## Light probe (shared host, not evidence)

`quiet light`, load 30 to 60, 2 rounds, 500,000 documents; A2 was the same index as A (timing noise
only). Segments: A 4, B 62 (every flush a run).

| Command | Set | A p50/p99 µs | B p50/p99 µs | p50 B/A | p99 B/A | per query B/A [95%] | A/A per query |
|---|---|---|---|---:|---:|---|---|
| TOP_10 | term | 59/317 | 440/802 | 7.46 | 2.53 | 6.27 [5.79, 6.73] | 0.85 |
| TOP_10 | phrase | 424/35,907 | 1,555/42,079 | 3.67 | 1.17 | 3.41 [3.04, 3.83] | 1.02 |
| TOP_10 | AND | 175/3,931 | 1,120/5,923 | 6.41 | 1.51 | 5.26 [4.82, 5.75] | 0.93 |
| COUNT | term | 7.1/10.2 | 102/132 | 14.4 | 12.9 | 14.0 [13.6, 14.5] | 1.07 |
| COUNT | phrase | 459/85,166 | 1,328/79,781 | 2.90 | 0.94 | 2.70 [2.43, 3.02] | 0.98 |
| COUNT | AND | 258/758 | 1,030/1,580 | 3.99 | 2.08 | 4.63 [4.25, 5.03] | 0.98 |

Counts agree on every query (0 mismatches). Indexing the same corpus took 20.7 s at 64 runs and
43.8 s at the tiers (`/usr/bin/time`, light): the merges the runs skip.

## Exclusive A/B

Pending: strict smoke 261125-041712-dedde3, exclusive A/B with A/A 261125-041728-c7bab8 (both at
strata `3292724f`, 550,000 documents, A, A2, B and C).

## Reading

The per-run cost is a fixed setup per segment and per query: from the light rows, about 6.6 µs a
run for a `TOP_10` term query ((440 − 59) / 58 more segments) and 1.6 µs for a term `COUNT`. A
query whose own work is small (a rare term, a selective AND) pays it in full, so p50 moves most;
a query dominated by postings work (a common phrase, 36 to 85 ms) hides it, so the p99 of phrases
ties. Runs share no term range: keys order the runs, not the words, so every run holds postings
for every common term and no per-run term bound can skip one on text. Past 10% p99 on every
`TOP_10` row, so the cap follows the layout (lead's decision, 2026-11-25; strata `60e6cf55`):
`segment_policy<Layout>::max_runs` is 64 for `document_layout` and 0, the tiers alone, for a layout
with postings (`docs/mechanisms/sorted-runs.md`, "Why 64").
