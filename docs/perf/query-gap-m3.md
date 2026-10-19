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

## M3t: terms only, reordered segments only

`STRATA_M3=2` at the A/B (`build-e2-m3t`; the code now carries only this half, under
`STRATA_M3`). `search_test` passes; `verify` reads 0 mismatches over the 1,162 gate queries
and stratified terms on the reordered merged segment. A docid-ordered segment takes today's
path, so on the 16-segment index the two binaries run the same code and that index's rows
are a layout and noise check. Raw: `~/bench/results/qexp-m3t/`. Phase times: 138–212 s per
index and preset (212 s for the first, which ran slow; its untouched rows read up to 1.14).

| Term row (B/A, 95%), S | merged `bm25` | merged `bm25_xapian` | 16 segments `bm25` | 16 segments `bm25_xapian` |
|---|---|---|---|---|
| `TOP_10` | 0.744 [0.664–0.822] | 0.833 [0.766–0.907] | 0.998 | 1.031 [1.018–1.043] |
| `TOP_100` | 0.834 [0.767–0.904] | 0.963 | 0.925 | 0.971 |
| `TOP_1000` | 0.842 [0.784–0.897] | 0.969 | 1.038 [1.011–1.062] | 0.994 |
| `TOP_100_COUNT` | 0.820 [0.758–0.881] | 0.917 | 0.971 | 0.971 |
| term:frequent `TOP_10` | 0.522 [0.454–0.595] | 0.664 | 1.029 | 1.048 |

E4b's dominance seeds on top of M3t (A: M3t, B: M3t with the seeds), S:

| Row (B/A, 95%) | merged `bm25` | merged `bm25_xapian` | 16 segments `bm25` |
|---|---|---|---|
| term `TOP_10`, `TOP_100`, `TOP_1000` | 0.897 [0.852–0.942], 0.867, 0.923 [0.879–0.958] | 0.878, 0.891, 0.920 | 0.828, 0.869, 0.870 |
| union `TOP_10`, `TOP_100`, `TOP_1000` | 0.950 [0.921–0.980], 0.798 [0.724–0.868], 0.816 [0.716–0.898] | 0.864, 0.786, 0.812 | 0.925, 0.873, 0.839 |
| term `TOP_100_COUNT` | 0.873 | 0.885 | 1.024 [1.010–1.037] |

Both pass item 1 on their target rows on S. Rows over 1.03 on S, by item 3, rerun on S′ and
the rerun stands: M3t's 16-segment term rows (whose code is unchanged there), the first
run's untouched classes, and the seeds' 16-segment term `TOP_100_COUNT`.

## The S′ reruns and the session's A/A

Item 3's reruns on S′ (361 gate queries, 100 terms; six rounds; 191–349 s each, slower than
the S runs). Raw: `~/bench/results/qexp-m3t/sprime/`.

| Row (B/A, 95%), S′ | M3t, merged `bm25` | M3t, 16 segments `bm25` | M3t, 16 segments `bm25_xapian` | seeds on M3t, merged `bm25` | seeds on M3t, 16 segments `bm25` |
|---|---|---|---|---|---|
| term `TOP_10` | 0.776 [0.729–0.826] | 0.924 | 1.026 [1.016–1.036] | 0.869 [0.838–0.902] | 0.855 [0.821–0.889] |
| term `TOP_100` | 0.842 [0.797–0.887] | 1.006 | 0.986 | 0.821 [0.789–0.855] | 0.808 [0.778–0.842] |
| term `TOP_1000` | 0.856 [0.823–0.888] | 1.039 [1.022–1.056] | 0.996 | 0.879 [0.848–0.910] | 0.862 [0.832–0.892] |
| term:frequent `TOP_10`, `TOP_100`, `TOP_1000` | 0.544, 0.644, 0.679 | 0.935, 1.021, 1.053 | 1.047, 0.976, 1.025 | 0.811, 0.799, 0.765 | 0.706, 0.668, 0.722 |
| union `TOP_10`, `TOP_100`, `TOP_1000` | 1.045 [1.038–1.052], 1.029, 1.017 | 0.981, 1.011, 1.032 | 1.047, 0.998, 0.978 | 0.840, 0.768, 0.773 | 0.889, 0.832, 0.830 |
| term `TOP_100_COUNT` | 0.807 | 1.006 | 1.015 | 0.830 | 1.051 [1.040–1.063] |

Every row over 1.03 in these reruns is on code the B side does not execute differently:
union and every other non-term class under M3t (only a single term's bulk top-k changes), the
16-segment index under M3t (its segments are docid-ordered, so both binaries take today's
path), and counted rows under the seeds (a multi-segment counted search takes no seed). To
read them, the same session ran the base binary against itself on S′ (`~/bench/results/qexp-aa/`,
195 and 238 s): **the A/A's own worst rows are 1.074 [1.061–1.088] (intersection `TOP_1000`,
merged) and 1.129 [1.107–1.150] (`COUNT` intersection_union, 16 segments)**, and the E4b runs,
whose two sides are one binary, read up to 1.17 on untouched 16-segment rows. `ab.py`'s
interval is over queries, not runs, so it does not carry run-to-run drift of this size, and
D14's 1.03 guard sits inside this host's A/A spread today.

## Verdicts

- **M3t: wins its target rows** on both samples (term `TOP_10`, `TOP_100`, `TOP_1000` 0.776,
  0.842, 0.856 on S′; frequent terms 0.54–0.68; `bm25_xapian` 0.83–0.97 on S) **and no row
  over 1.03 is attributable to it**: each is on a path it does not change and inside the
  session's A/A envelope. By the letter of item 2 the guard is not met, and a format-free item
  lands on its own later A/Bs (D12's platforms included); for the freeze, M3t is the base E4b
  had to pass on, and it is real. The union half is removed (above).
- **E4b on top of M3t passes** item 1 on term and union rows on S′ and on both indexes
  (term 0.808–0.879, union 0.768–0.889), its over-1.03 rows being ones the seeds cannot
  reach. So M20's timing condition
  holds; its line is in `query-gap-format13.md`.
