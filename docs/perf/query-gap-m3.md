# Query gap, E10: bound-ordered traversal (M3)

The design's E10 (`docs/design/query-gap.md`), run before the freeze because E4a passed and
the lead ruled (2026-10-19) that M20 is decided on evidence: E4b is measured again on top of
it.

## Plan (written before the A/Bs)

- **Mechanism** (`STRATA_M3` for the A/B builds): in a collection whose hits may arrive out
  of ordinal order (the collector was given the segment, as every reordered segment and every
  multi-segment search is, so ties compare docids), a term scores its tail, then its groups in
  descending group-bound order, each from a fresh iterator, with today's per-block skip
  inside a group, and stops at the first group whose bound cannot beat θ. A union cuts the
  segment into ranges of 64Ki ordinals, bounds each by its clauses' largest group bounds over
  the range (a clause's term-level bound where the range reaches its tail), visits ranges in
  descending summed bound, and runs today's windowed MaxScore inside each range on fresh
  clauses, until a range cannot compete. A docid-ordered single segment keeps today's path.
- **Costs it pays** (QG-R18): a fresh iterator per group or range (each re-decodes a group's
  level 0), and every group's bound computed up front.
- **Target rows:** term and union `TOP_10`, `TOP_100`, `TOP_1000`.
- **Checks:** `search_test` built with `STRATA_M3` (its multi-segment cases take the new
  path); `verify` (pruned top-k against scans, both presets) over the gate queries and the
  stratified terms on the reordered merged segment.
- **A/Bs:** in-process on S, the base against M3, reordered merged and 16-segment indexes,
  `bm25` and `bm25_xapian`, six rounds of three runs, one thread pinned, light hold; then
  E4b's dominance seeds on top of M3 (A: M3, B: M3 with the seeds). Keep by D14.

## Results: M3 for terms and unions (2026-10-19)

In-process on S, six rounds of three runs, one thread pinned, light hold; phase times
127–174 s per index and preset. Raw: `~/bench/results/qexp-m3/`.

| Row (B/A, 95%) | merged `bm25` | merged `bm25_xapian` | 16 segments `bm25` | 16 segments `bm25_xapian` |
|---|---|---|---|---|
| term `TOP_10` | 0.761 [0.684–0.838] | 0.840 | 0.965 [0.937–0.993] | 0.970 |
| term `TOP_100` | 0.819 [0.753–0.887] | 0.925 | 0.916 | 1.007 |
| term `TOP_1000` | 0.898 [0.837–0.960] | 0.984 | **1.039 [1.010–1.066]** | 1.001 |
| term `TOP_100_COUNT` | 0.824 | 0.932 | **1.095 [1.069–1.118]** | 0.998 |
| union `TOP_10` | **1.722 [1.554–1.914]** | **1.691** | **1.560** | **1.320** |
| union `TOP_100` | **1.191** | **1.304** | **1.227** | **1.228** |
| union `TOP_1000` | **1.295** | **1.238** | **1.111** | **1.105** |

**The union half loses badly and is removed.** The likely cause, not counted: a 64Ki range's
bound from its clauses' group bounds is close to the whole-term bound for every dense clause,
so few ranges are left unvisited, and each visit pays fresh clauses and a restarted window
loop, while the windowed MaxScore already skips inside one pass what bound order would.

**The term half wins on the reordered merged index** (term `TOP_10` 0.761, frequent terms
0.535) **and loses on the 16 docid-ordered segments** (`TOP_1000` 1.039, `TOP_100_COUNT`
1.095), where a reordering's skew of high bounds into few groups is absent and each segment
pays its fresh iterators. So the variant that can meet D14 is **M3t: terms only, in
reordered segments only** (a docid-ordered segment keeps today's path, as the design's
"docid-ordered segments on this path" cost note anticipates); it is measured next, then E4b
on top of it.

E4b on top of this M3 (A: M3, B: M3 with the dominance seeds), for reference until M3t's
reruns: terms 0.86–0.90 on the merged index and 0.85–0.89 on 16 segments, union `TOP_1000`
0.866: the seeds still add on top of bound order for terms.
