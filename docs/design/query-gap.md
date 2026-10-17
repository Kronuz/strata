# Query gap: closing strata's query-speed gap

Revision 0, for the preliminary review. Track P's plan for the carried query rows (PLAN,
"Carried rows": Phase 4's gate does not pass while one still misses 1.0×), for Phase 6's 0.7×,
and for the owner's 0.5× stretch. Per query class it states where strata loses and by how
much, the hypotheses for why with the measurement that decides each, the candidate
mechanisms with estimated effects and costs, which of them format 13 must decide before it
freezes, and an ordered experiment queue for a performance engineer. Decisions are D1–D14,
hypotheses H1–H19, mechanisms M1–M23, experiments E0–E17, gate rows T1–T8, owner questions
Q1–Q3.

Nothing here was measured for this design. Every number is a recorded one, cited with its
file, or an estimate, labeled as one. The host was under a strict hold while this was written
(the gate VM's `night_gate.sh n1017`), so nothing was built or run, and the raw result files
that live only in the gate VM's `~/bench/results/` were not read; the per-class figures come
from the committed gate report.

Conventions. Code citations are `include/strata/*.hh` and `tools/*` at main `5691ec7`.
`F13:N` is `docs/design/format-13.md` on `phase4/format13` at `4266374` (revision 0, unchanged
since `e2ac3fe`), and `F13R:N` is that branch's `docs/reviews/format-13-design.md`. A ratio is
strata's time over the reference's, the floor-subtracted geometric mean over a class's queries
(BENCHMARKS.md:133-137); below 1.0 strata is faster.

Clean room. Lucene, Tantivy, and PISA enter from their papers, their documentation, and the
benchmark harness's description of each engine (`bench/phase0/query.json`, its `details`);
nothing comes from their source. No Xapian or Xapiand GPL source was read.

## Summary

- **Where strata stands.** After merging, strata is 1.35–1.37× the fastest baseline on every
  command's global class (five trials, docid order, `2e774db`). Reordering takes 17–27% off
  strata's own top-k times on every class but single terms, and up to 30% off its counts (P1
  T2). Projected onto the reordered index, the global rows sit at 1.07–1.14×, with the widest
  misses at intersection_union (1.20–1.56×), union (1.08–1.28×), single terms (1.15–1.22×), and
  phrases (1.14–1.20×). The projection is an estimate; E0 measures it.
- **Why, one line per class.** Union and conjunction bounds fall back to group and whole-term
  bounds over windows wider than a block (H4, H7); counted top-k scores every match (H6, H8);
  required-plus-optional and exclusion have no bulk path (H11, H12); on long lists the
  threshold starts at zero and converges late (H1); phrases verify positions at about 1.7×
  Tantivy's cost (H13); a counted block costs about 300 ns, a quarter of it skip entries (H15,
  H16); and cheap queries carry fixed costs that floor subtraction magnifies (H17).
- **What format 13 must decide before it freezes:** the advance entries' layout (M16); whether
  frontiers are ordered by score so that one pair bounds a list (M17, zero bytes) or reduced to
  one argmax pair per block (M18, saving bytes toward the carried postings row); and whether
  partition hooks (M19), threshold seeds (M20), and a positions codec field (M21) stay. Each is
  decided by an experiment that measures its ceiling before it is built (E3–E7), on top of the
  format-free bound fixes (E2). An item without its evidence at the freeze leaves format 13
  (D5).
- **After the freeze, or independent of it:** fixed per-query costs, counted top-k,
  bound-ordered traversal, bulk required-plus-optional and exclusion, vectorized block scoring,
  array intersection for counts, the phrase path, prefetching, and branch hints (E8–E17), in
  the order E1's attribution ranks them.

## What this depends on and what it changes

| Design or review | This design depends on | This design changes or asks |
|---|---|---|
| PLAN (Ratchets, Track P, "What waits") | The carried rows, the stretch, SYS-6's rule on branch A/Bs (PLAN.md:16-20, 103-116, 135-141) | Nothing in PLAN's text. Track P's items map to mechanisms: P2 to M6 and M7, P3 to M3 and M19, P4 to M22, P5 to M15, P6 to M11 and M16, P8 to M14, P9 to M7, P21 to M20; P1, P6's segment items, and P7 have landed |
| `format-13.md` and its preliminary review | F10–F14; F13-R7 (carried rows as freeze criteria) and F13-R8 (one byte budget) | Supplies F13-R7 item 2's attribution (E1) and item 4's candidate tables (E3–E7); adds M17, M18, and M23 to F13-R8's selection; asks that a group's impact entries stay contiguous and a block's bound be one read away |
| `layers.md` | L11 (search on plugin readers) and L15's decision rule | Nothing; which base the experiments build on is the lead's call ("For the lead", item 1) |
| `reordering.md` | D5's tie rule for reordered segments | M3 applies that rule to out-of-order collection in docid-ordered segments |
| `frozen-index.md` (P20) | Its route to PISA-like pruning | D7: toward the stretch, its quantized values count only as bounds |
| `docs/perf/p6-multisegment.md` and its review | P6-2's one collector; the fixed-point proposal | M8 carries P6-4's allocation items; M13 waits for the architect |
| `docs/reviews/phase-2-gate.md` | Ruling 2 (whole frontiers); C15's owed rows | M18 asks to revise ruling 2 at block level; E1's tooling serves C15's allocation and frontier rows |
| ARCHITECTURE | The codec rule on block maxima; the read path | Q2, only if E3 needs a stored bound; the read path's window text once M1 or M3 lands |
| BENCHMARKS and the system review of 2026-10-17 | Tiers, the keep rule, floor subtraction; SYS-1, SYS-6, SYS-7 | Nothing |

## Where strata stands

### The record

| Run | What | Commit | Where |
|---|---|---|---|
| n1, 2026-10-15 | After merging: strata force-merged in docid order against force-merged Lucene, Lucene `-bp`, and Tantivy (Xapian beside them); five trials over every gate query and the 200 stratified terms; pipe floor 67 µs | `2e774db` | `docs/gates/phase-3-ratchets.md:98-172` |
| n2, 2026-10-16 | The quick tier (181 sampled queries and 40 terms, three trials) with PISA 0.8.2; global ratios only in the committed record | `db0a891` | `phase-3-ratchets.md:286-322`; raw in the VM's `results/query-n2-quick` |
| P1 T2, 2026-10-17 | The quick tier, the reordered merged index over the docid-ordered one, interleaved; every class against the cached baseline reads DRIFT, since the anchor ran 1.21–1.31× slower | `c80996b` | `phase-3-ratchets.md:324-343`; raw `results/query-t2-quick` |
| In-process records | One `COUNT` profile; A/Bs of P6, P6a, P7, the union-count fix, and two rejected decoders | several | `docs/perf/query-gap-merged.md`, `p6-multisegment.md`, `p6a-conjunction-bulk-topk.md`, `p7-membership-probes.md` |

### Per class

Each cell is n1's floor-subtracted median, then the projection for the reordered index (n1
times T2's reordered-over-docid factor for the class and command). Negated has no T2 factor.
The two-phase-critic class is one query and is left out.

| Class (queries) | `TOP_10` | `TOP_100` | `TOP_1000` | `TOP_100_COUNT` | `COUNT` |
|---|---|---|---|---|---|
| term (201) | 1.19 → 1.18 | 1.26 → 1.22 | 1.15 → 1.15 | 0.89 → 0.82 | 1.00 → 1.00 |
| intersection (300) | 1.16 → 0.88 | 1.35 → 0.99 | 1.39 → 1.01 | 1.49 → 1.09 | 1.48 → 1.04 |
| union (301) | 1.60 → 1.28 | 1.52 → 1.23 | 1.38 → 1.15 | 1.30 → 1.08 | 1.34 → 1.18 |
| intersection_union (40) | 1.93 → 1.56 | 1.89 → 1.55 | 1.61 → 1.30 | 1.50 → 1.20 | 0.93 → 0.94 |
| phrase (300) | 1.46 → 1.20 | 1.41 → 1.14 | 1.42 → 1.14 | 1.43 → 1.14 | 1.43 → 1.16 |
| negated (19) | 0.84 | 1.20 | 1.36 | 1.50 | 1.37 |
| global (657) | 1.36 → 1.10 | 1.36 → 1.07 | 1.35 → 1.08 | 1.37 → 1.08 | 1.36 → 1.14 |

The same classes as n1's mean time per query, strata against the fastest baseline (µs). Means
weigh the expensive queries, while the ratios above weigh the typical one, so the two rank the
classes differently:

| Class | `TOP_10` | `TOP_100` | `TOP_1000` | `TOP_100_COUNT` | `COUNT` |
|---|---|---|---|---|---|
| term | 292 / 255 | 487 / 407 | 891 / 670 | 494 / 710 | 199 / 200 |
| intersection | 1,046 / 1,020 | 1,329 / 1,033 | 1,542 / 1,084 | 1,652 / 1,032 | 981 / 847 |
| union | 1,547 / 1,236 | 2,460 / 1,970 | 4,188 / 3,880 | 12,474 / 6,408 | 1,403 / 1,098 |
| intersection_union | 4,053 / 3,028 | 4,422 / 3,099 | 4,790 / 3,533 | 4,576 / 3,403 | 261 / 282 |
| phrase | 2,355 / 1,803 | 2,521 / 1,806 | 2,576 / 1,809 | 2,565 / 1,800 | 2,547 / 1,767 |
| negated | 509 / 719 | 862 / 726 | 1,144 / 875 | 1,036 / 718 | 665 / 514 |

The projection is an estimate, good to about ±5%:

1. n1 and T2 ran different commits. The union-count fix landed between them (0.932× in-process
   on the 301 unions' `COUNT`, `query-gap-merged.md:11-16`), so the `COUNT` union and global
   projections are high.
2. n2's sampled global ratios read above n1's (1.39–1.44× raw at `TOP_k` against 1.33–1.34×,
   with `-bp` setting most rows, `phase-3-ratchets.md:291-301`). n2 times T2 gives 1.11–1.16×
   at `TOP_k` and 0.97× at `COUNT`; the report's own anchor-scaled estimate is about 1.06× at
   global `TOP_10` (`:340-343`).
3. T2's factors are ratios of raw harness times on the quick sample; applying them to
   floor-subtracted medians is approximate.

Where strata loses, in short:

- **Term:** 15–26% at `TOP_k`, untouched by reordering; it wins `TOP_100_COUNT` (0.89×) and
  ties `COUNT`, both of which take the count from the document frequency. At Phase 2 the gap
  sat in the frequent stratum: rare and medium terms within 1.06× in every command, frequent
  `TOP_10` 1.44× and `TOP_100` 1.47× (`docs/reviews/phase-2-gate.md:854-860`). The split after
  merging is not recorded.
- **Intersection:** reordering helps it most (0.70–0.76), which leaves `TOP_1000` and the
  counted commands near or above 1.0×.
- **Union:** the largest pruned gap, and the one PISA leads by 2.1–3.9× (only its `TOP_10` rows
  are exact, D13). `TOP_100_COUNT` loses 6 ms a query on average against Tantivy.
- **intersection_union:** the worst class, 1.0–1.3 ms a query behind at every top-k command.
- **Phrase:** flat across commands in every engine, so per-candidate verification sets it;
  Tantivy is the reference everywhere.
- **Negated:** wins `TOP_10` and loses 20–50% at larger k and counted.

### What the targets ask

Reductions of strata's time from the projection (estimates):

| Target | Median needed | Union `TOP_10` | intersection_union `TOP_10` | Phrase `TOP_10` | Term `TOP_100` | Global |
|---|---|---:|---:|---:|---:|---:|
| Phase 4's gate: ≤ 1.0× by the upper bound | about 0.95 (n1's intervals are 1–4% wide on most rows) | −26% | −39% | −21% | −22% | −11% to −17% |
| Phase 6: ≤ 0.7× | about 0.67 | −48% | −57% | −44% | −45% | −37% to −41% |
| Stretch: the 35-row geometric mean at ≤ 0.5× | 0.5 | | | | | −54% from about 1.08 (n2's 1.34 times T2's global factors) |

PISA shows what pruning, quantized scores, and reordering reach together. On n2's global
`TOP_10`, which is exact for PISA, strata is 1.43× the fastest baseline and 2.30× PISA, so PISA
sits at about 0.62× the fastest baseline (the two ratios divided; an estimate, since each
geometric mean is against its own engine per query). PISA gets there with recursive graph
bisection, block-max data per 40 postings, quantized BM25, and a precomputed 10th-best seed
(`phase-3-ratchets.md:309-322`). The stretch therefore asks for roughly 20% better than PISA's
own `TOP_10` while staying exact and updatable. strata already leads PISA on single terms
(0.39–0.87×) and counts (0.48× at global `COUNT`), where PISA neither prunes nor counts fast.

## How a query runs

```
serve, bench               strata_engine.cc:203-226, 572-587
  run(segs)                engine_query.hh:205-213
    term_table             searcher.hh:116-177   one dictionary walk per term and segment;
                                                 a short list's pairs computed on first use
    build                  engine_query.hh:112-143   term and phrase scorers, then conjunction,
                                                 disjunction, required_optional, exclusion
    search (one segment)   searcher.hh:185-195 -> top_k, search.hh:1267-1327
      collect_top_k        term 336-346; conjunction of terms 542-591; disjunction 1348-1433
      generic loop         search.hh:1306-1323: everything else, and every counted conjunction
    COUNT                  count() in 4,096-ordinal windows, search.hh:1330-1340
```

Each 128-posting block has a level-0 entry: LEB128 fields and its whole Pareto frontier of
(wdf, length code) pairs (`writer.hh:785-793`). Entries group by 32 under a fixed-width level-1
entry (`writer.hh:818-828`). Reaching a block in a new group decodes and validates that group's
32 entries (`reader.hh:830-874`). A bound evaluates a frontier pair by pair
(`search.hh:379-383`; `format.hh:358-383`), and `max_score(from, upto)` returns the bound of the
smallest single skip entry covering the range: the block, its group, or the whole term
(`search.hh:351-358`).

## Hypotheses

Each names its evidence in the code or the record and the measurement that confirms or refutes
it. E1's counters and profiles answer most of them at once.

### Single terms

- **H1. The threshold converges late on long lists.** A top-k query starts at θ = 0
  (`search.hh:195-306`), and `term_scorer::collect_top_k` walks groups and blocks in ordinal
  order (`search.hh:336-346, 403-430`), so blocks are scored at a θ below the final one until
  enough good postings have been seen. Whole frontiers leave 21 of `the`'s 32,563 blocks above
  the final top-10 threshold (`phase-2-gate.md:84-87`); how many are scored before θ gets
  there is not recorded. *Measure:* E1's count of blocks scored whose bound lies below the
  final θ_k. E4's oracle seed (θ_k from the first posting) is this hypothesis's ceiling in time.
- **H2. Bounds cost one round trip and one division per pair.** A skipped block evaluates every
  pair of its frontier (`reader.hh:590-594`; `format.hh:358-383`); a group bound decodes the
  group's frontier into a vector first (`reader.hh:625-630, 762-768`). Each pair goes through
  `decode_length`, then `encode_length` inside `weight::operator()`, then a division
  (`search.hh:105-111`). Frontier lengths per level were asked for at Phase 2 and never
  recorded (`phase-2-gate.md:974-976`, C15). *Measure:* pairs evaluated per query; frontier
  length histograms per level; the profile share of `bound`, `any_sized_impact`, and `decode`.
- **H3. Scoring is a scalar division per posting.** A competitive block scores every posting
  through `score_of` and `collect` (`search.hh:342-343, 373-375, 211-222`); at `TOP_1000` θ
  stays low and most of a frequent term's blocks compete. *Measure:* postings scored per query;
  the profile share of `for_each_until`, `by_norm`, and `collect`.

### Intersections

- **H4. Window bounds fall back to group and whole-term bounds.** The conjunction's bulk path
  takes the lead's block as the window and bounds every other term over it
  (`search.hh:547-558`). The lead is the rarest term, so its block spans (other's density over
  lead's density) of another term's blocks: more than one whenever the other is denser, more
  than a group of 32 when it is 32 times denser. `max_score` then returns the group's bound or
  the term's (`search.hh:351-358`), which skips few windows and rejects few candidates at
  `s + rest` (`search.hh:564-571`). *Measure:* `max_score` resolutions (block, group, term)
  inside conjunction windows; windows skipped; candidates rejected by the bound.
- **H5. Every candidate advances every other term, and a rare lead lands those advances in new
  blocks.** An advance outside the decoded block decodes 128 postings, and outside the decoded
  group 32 level-0 entries (`reader.hh:542-568, 881-891, 893-944`). *Measure:* advances per
  candidate, split into in-block, new block, and new group; ns per advance.
- **H6. Counted top-k scores every match.** A counted conjunction cannot take the bulk path
  (`search.hh:543`) and runs the generic loop, which skips scoring only when the whole-list
  bound cannot compete (`search.hh:1306-1311`, with `current_bound` defaulting to it at `:141`).
  Ranking while counting adds 671 µs over `COUNT` on the intersection class's mean for strata,
  474 µs for Lucene, and 185 µs for Tantivy (n1, `phase-3-ratchets.md:158, 166`). *Measure:*
  candidates scored over matches in `TOP_100_COUNT`.

### Unions

- **H7. Window bounds degenerate for dense clauses.** The windowed MaxScore bounds each clause
  over 4,096 ordinals (`search.hh:1373-1377`). A block of a term with density ρ spans about
  128/ρ ordinals, so above ρ = 1/32 a window covers more than one block and gets the group's
  bound; a group spans 4,096/ρ ordinals, so a window crosses a group boundary with probability
  about ρ and then gets the term's bound. A term in 10% of documents takes its group bound in
  about 90% of windows and its whole-term bound in the other 10% (derived from the layout, not
  measured). The per-window essential split and the per-document consult
  (`search.hh:1379-1389, 1414-1424`) work from those bounds. *Measure:* per-window bound
  resolutions by clause density; windows skipped whole.
- **H8. Counted unions score every match, and two-clause counts take an extra pass.** When
  counting, every clause is essential and every match is scored and collected
  (`search.hh:1360, 1383-1425`); a two-clause union first counts a full conjunction
  (`search.hh:1450-1465`). The class's `TOP_100_COUNT` mean is 12,474 µs against Tantivy's 6,408
  (n1), the largest absolute gap in the record. *Measure:* candidates scored over matches; the
  two passes' times.
- **H9. Per-query setup weighs on cheap unions.** The bulk path allocates and zero-fills 32 KiB
  of accumulators per clause per query (`search.hh:1353`), on top of H17's costs. *Measure:*
  H17's breakdown, by union cost tercile.
- **H10. Bounds are coarser than PISA's.** PISA keeps block-max data per 40 postings
  (`phase-3-ratchets.md:311-312`); strata per 128, and its groups per 4,096. *Measure:* E3's
  simulated postings scored at 32 to 256 postings per partition and with variable partitions.

### intersection_union and negated

- **H11. Required-plus-optional has no bulk path and never drives from the optional side.**
  Each required document pays a virtual `next`, a bound of the optional side over the required
  block, `score`, then an optional advance and score (`search.hh:865-920`), and the optional
  bound falls back as in H4 (`search.hh:906`). Where the required side alone cannot reach θ,
  only documents holding both sides can compete, yet the loop still visits every required
  document. *Measure:* required documents scored, optional advances, bound rejections.
- **H12. Exclusion has no bulk path.** Each candidate advances the excluded terms
  (`search.hh:964-974`); `TOP_1000` and `TOP_100_COUNT` run the generic loop. *Measure:* the
  profile.

### Phrases

- **H13. Verification dominates, at about 1.7× Tantivy's cost.** Phrase rows are flat across
  commands in every engine (n1: strata 2,355–2,576 µs, Tantivy 1,767–1,809 µs). The phrase and
  intersection classes are the same 300 term sets, and over its intersection `COUNT`, strata's
  phrase `COUNT` adds 1,566 µs and Tantivy's 920 µs (n1 means).
  Per candidate, strata decodes the rarest slot's positions (`reader.hh:1130-1156`), steps
  position-block headers from the postings block's first position block
  (`reader.hh:1187-1207`), reads counts or wdfs (`reader.hh:1117-1121`), and sets up
  `phrase_freq` (`search.hh:1144-1191`). *Measure:* the profile's split; candidates, matches,
  positions decoded, and headers stepped per candidate.
- **H14. The positions encoding costs decode time.** Positions restart at each document
  (`docs/design/phase-2.md:140-142`), so each document's first delta is an absolute position
  inside a PFOR block. P13's candidates (FOR, PFOR, and Elias-Fano per block; deltas against
  the document's length) are unmeasured (F13:345-351). *Measure:* E7's offline statistics
  (exceptions per block, widths) and decode microbenchmark.

### Counting

- **H15. Skip entries take about a quarter of a counted block.** The one profile
  (`+time +for +kids`, `COUNT`) puts about 300 ns on each touched block, 23% of it in
  `decode_group`, `block_index_for`, and `skip_sized_impacts` (`query-gap-merged.md:24-28`;
  F13R:430-436); removing all of it leaves about 230 ns. The record's "roughly 90 ns for
  Lucene" has no recorded derivation, and Lucene 10.4's blocks hold 256 documents
  (`bench/phase0/query.json`, `details`), so the comparison must say per how many postings
  (calibration, third row). *Measure:* E1 re-derives the figure as Lucene's time for the
  query over the 128-posting blocks strata touches, labeled an estimate; E6 times the
  advance-entry candidates.
- **H16. Bit-filling and per-window overhead.** `fill` sets one bit per posting
  (`reader.hh:710-734`; 22% of that profile), the conjunction ANDs per window (10%), and every
  window recomputes `prefer_windows()` (`search.hh:485`) and clears and counts 64 words
  (`search.hh:1333-1338`). Accumulating a word in a register lost to mispredictions (1.092×,
  `query-gap-merged.md:17-19`). *Measure:* the profile; windows and blocks per query; E13's
  array intersection against fill-and-AND.

### Every class

- **H17. Fixed per-query costs, magnified by floor subtraction.** For a query a few µs above the
  67 µs floor, a 2 µs difference is a ratio of 1.2–1.7×. strata pays per query: parsing into
  vectors of strings (`engine_query.hh:26-54`); a term table of strings
  (`searcher.hh:148-167`); for every term under 128 postings, a full decode to compute its
  frontier before the scorer decodes it again (`searcher.hh:140-143`; `reader.hh:345-347,
  1075-1080`); a 256-entry length table per term, copied by value into its scorer
  (`search.hh:74-83, 315, 434`), five of them for a three-term phrase (`search.hh:999, 1021,
  1249`); two copies of each term's pair vector (`engine_query.hh:66, 70`; `reader.hh:478`); a
  heap allocation per scorer; a reserve of 2k + 1 hits (`search.hh:196`); and H9's
  accumulators. Allocations per query by class are owed since Phase 2 (C15), and no
  allocation-counting build is in the tree at `5691ec7`. *Measure:* in-process time of three
  probe queries (an absent term, a df-1 term, a union of two df-1 terms) per command;
  allocations per query by class; each class's ratio split by the reference's cost tercile.
- **H18. Memory stalls on block jumps.** A leapfrog lands on a block that is not in cache, and
  hardware prefetchers follow sequential scans and miss jumps. *Measure:* last-level misses per
  touched block, if the gate VM's guest exposes hardware counters (unverified: E1 tries
  `perf stat` first and falls back to the macOS host's counters); E15's prefetch A/B.
- **H19. Mispredicted branches.** The rejected LEB128 decoder showed skip-entry streams bound by
  mispredicted value lengths (`query-gap-merged.md:29-33`); `advance`, `collect`, and
  `skip_blocks` branch per posting or per block. *Measure:* branch misses per function (H18's
  caveat); E16.

## Mechanisms

Every effect below is an estimate: none is measured, and pruning gains do not add (D6).

### Format-free

| Id | Mechanism | Source | Rows | Estimated effect | Cost |
|---|---|---|---|---|---|
| M1 | Range-max bounds: `max_score(from, upto)` returns the largest block bound among the blocks the range covers, with group bounds for whole groups inside it, so the whole-term bound applies only to a range that reaches the tail or covers every group | Block maxima (Ding & Suel, 2011) applied to windows | union, intersection, intersection_union, phrase `TOP_k` | 5–25% on union `TOP_k`, 5–15% on intersections (estimate; E1's resolution counts decide) | Small: `term_scorer::max_score`, `phrase_scorer::range_bound`; block bounds are already cached per group (`search.hh:385-394`) |
| M2 | Block-max AND: a candidate is checked against its lead score plus the bound of the block holding it in every other term, read from skip data without decoding the block | Ding & Suel, 2011 | intersection, intersection_union, two-phase-critic | 5–20% on intersection `TOP_k` (estimate) | Small; cheaper once M16 lands |
| M3 | Bound-ordered traversal: a term's groups, or a union's docid ranges, in descending bound order, stopping when the next bound cannot beat θ | P3's anytime half (Mackenzie et al., 2022) | term (frequent), union `TOP_k` | Capped by E4's oracle seed | Medium: out-of-order collection uses the docid tie rule of reordered segments (`reordering.md:209-253`) |
| M4 | Counted top-k: one pass counts every match by windows and scores only the candidates whose bound competes | New | `TOP_100_COUNT` intersection, union, negated, intersection_union | 20–40% on those rows (estimate) | Medium |
| M5 | Bulk required-plus-optional, driving from the optional side in windows where the required side alone cannot reach θ; bulk exclusion | Ding & Suel's AND, and a new driver rule | intersection_union, negated | 20–40% on intersection_union `TOP_k` (estimate) | Medium |
| M6 | Vectorized block scoring: a decoded block scored with SIMD and compared against θ, survivors collected; the conjunction's lead likewise | P2's vectorization; Lucene documents vectorized block-max MaxScore and AND | term, intersection, `TOP_1000` most | 10–30% where scoring dominates (estimate) | Medium; scores must stay bit-identical (D4, Risks) |
| M7 | Array intersection of decoded FOR blocks for counts, through `docset`'s SIMD merge and galloping; bitset AND and P7's probes kept for bitset blocks; the choice per block pair | P2, P9 | `COUNT` and `TOP_100_COUNT` intersection, two-clause union counts | 10–25% (estimate) | Medium; the kernels exist in `docset::detail` (`docset.hh:187-236` at the pinned `0859e43`), and exposing them is an additive `docset` change (D8) |
| M8 | Fixed-cost cuts: a trivial bound for terms without skip blocks (every score is below the term's weight factor, `w.idf`, under both presets), one length table per query shared by its terms, spans instead of copied pair vectors, per-thread accumulators and collector buffers kept zeroed, scorers from a per-query arena | P6-4; Phase 6's allocation row (PLAN.md:156) | the cheap tercile of every class | 1–5 µs a query (estimate; E1) | Small |
| M9 | Bound arithmetic: `by_norm` on the pair's code directly, bit-identical for every code a writer produces (0–247), since `encode_length(decode_length(c)) == c` there (`format.hh:258-268`); division-free skip tests with a rounding margin | New | term, union | Part of H2's share | Small |
| M10 | Phrase path: per-candidate setup out of the loop; a postings block's position headers stepped once when several candidates share the block; counts from the wdf block once | New | phrase | 10–20% (estimate) | Medium |
| M11 | Prefetch the next block in scans, and a jump's target block as soon as its skip entry resolves | P6 | every class | 0–5% (estimate) | Small |
| M12 | In-code branch and layout hints from measured profiles: `[[likely]]`, `[[unlikely]]`, cold paths split out, hot fields of `postings_iterator` together | The owner's direction (D9) | every class | 0–5% (estimate) | Small per hint |
| M13 | Fixed-point clause sums: one accumulator in the union window instead of one per clause | P6's open proposal (`p6-multisegment.md:261-294`) | union, intersection_union | Up to the 4–5% the clause-order sum costs (`p6-multisegment.md:252-254`) | Changes scores at the grid's resolution: the architect rules (D4) |
| M14 | Phrase prefilters | P8 | phrase | Unmeasured | Its reserved section kind (PLAN.md:38-41): after the freeze |
| M15 | Next-word entries for frequent pairs | P5 (Williams et al., 2004) | phrase over stopwords | Unmeasured | Disk (Q3) |

### Format 13

| Id | Mechanism | Source | Rows | Estimated effect | Bytes |
|---|---|---|---|---|---|
| M16 | Advance entries (F10, F13:236-272): fixed width per block, as A2 (narrow per group), A3 (FOR-packed), or lasts and offsets in separate arrays for an in-place SIMD search; one entry per 128 or per 256 postings; validation per block decoded (F13R:473-476) | F10, P6 | `COUNT`; every advance and window bound | Up to H15's 23% on counted intersections (F13R:430-436); the top-k share from E1 | A1 adds about 4 bytes a block over today's two LEB128 fields (estimate); A2 about none |
| M17 | Score-ordered frontiers: the writer orders every frontier by score under the index's preset at the segment's average length, and a reader bounds the whole list by its first pair (the bound below) | New | term, union, intersection `TOP_k` | H2's share | None; Q1 names the preset |
| M18 | One argmax pair per block for each preset, whole frontiers kept per group and per term | New: Tantivy's documented single pair per block, made a guaranteed bound by M17's factor | As M17 | As M17, and bytes back for the postings row | Saves what block frontiers spend past one pair: at three pairs a block, about 4 bytes, 17 MB over 4.37 million blocks (an estimate on an unmeasured length; E1's histograms give it) |
| M19 | Partition hooks (F12, F13:300-313): variable or finer partitions for block-max data, carrying M18's single pairs or a one-byte bound per partition (Q2) | P3 (Mallia et al., 2017) | union, term `TOP_k` | E3 decides | Per partition |
| M20 | Threshold seeds (F13:315-343) | P21 | union, term `TOP_k` | E4's ceiling, less what M3 captures | Per term with skip blocks |
| M21 | A positions codec field (F14, F13:345-351) | P13 | phrase | E7 decides | Either way |
| M22 | Elias-Fano and run containers (F11's E2 and E3): a probe into a dense block without decoding it whole | P4 (Ottaviano & Venturini, 2014; Chambi et al., 2016) | intersections with rare leads | E6's probe microbenchmark | Decode cost per block |
| M23 | Ordinal deltas stored as gap − 1, so a block's ordinals increase by construction and the reader checks only their sum against the entry | New | every decode (`check_increasing` was 4% of H15's profile) | At most about 4% on counted rows (estimate) | Fewer bits on dense blocks (unmeasured) |

### M1 and M2: bounds at block granularity

M1 keeps `max_score(from, upto)`'s contract, an upper bound on every score in the range from skip
data alone (`scorer.hh:37-45`), and changes what it computes. Today's smallest-covering-entry rule
reaches for the term's whole-list bound as soon as a range crosses a group boundary; M1 takes
the maximum of the block bounds the range covers, using group bounds only for groups entirely
inside it. A 4,096-ordinal window over a term in 10% of documents covers about three blocks
(H7), so M1 replaces a 32-block bound with a three-block one in most windows. The conjunction
window (H4), the required-plus-optional window (H11), and the phrase's lead bound
(`search.hh:1221-1233`) all go through it.

M2 is Ding and Suel's block-max AND: the window still comes from the lead, but each candidate is
checked against the sum of the bounds of the blocks that hold it, one per term, blocks that
`advance` would reach anyway. Both keep results exact by construction, and E2 checks them by
`dump` identity.

### M3: bound-ordered traversal

For a single term, compute every group's bound (today's `skip_blocks` computes nearly all of them
anyway, one at a time, `search.hh:403-430`), visit groups in descending bound order, score the
competitive blocks of each, and stop at the first group whose bound cannot beat θ. The best
groups come first, so θ reaches its final neighborhood within a few groups and nearly every
other group is skipped by its bound. For a union, the same over docid ranges aligned across the
clauses (64Ki ordinals to start, a preset), each range's bound being the sum of its clauses'
M1 range bounds, with today's windowed MaxScore inside a range. Hits arrive out of ordinal order,
so the collector compares docids on ties and hands scorers the score just below the k-th, the
rule reordered segments already use (`search.hh:291-298, 1285-1301`; `reordering.md:236-253`).
M3 buys what an exact seed would buy, without stored bytes; E4 measures that ceiling and E3
projects how much of it M3 reaches.

### M4 and M5: counted top-k and the required-plus-optional driver

M4 merges the counting window and the bulk top-k into one pass. Per window, every clause fills
its match bits (P7's probes and the bitset AND as today), the bits are counted, and only the
candidates whose block bounds can beat θ are scored. Conjunctions score their candidates through
M2's check; unions score only their essential clauses' documents, consulting the others while a
document can still compete. The two-clause union stops counting through a separate conjunction
(H8).

M5 gives required-plus-optional a bulk path with two drivers. In a window where the required
side's bound plus the optional side's can beat θ but the required side's alone cannot, only
documents holding both can compete, so the optional side leads and the required side confirms,
as in a conjunction. Elsewhere the required side leads, rejecting candidates whose score plus
the optional side's M1 bound cannot compete before the optional side moves. Exclusion's bulk path
scores the included side's block, filters by θ, and checks exclusions only for survivors.

### M17 and M18: one pair bounds a list

Both presets score a posting with wdf t at length ℓ as w · g(t, ℓ; A), where g(t, ℓ; A) =
t / (t + K(ℓ, A)) and K(ℓ, A) = k1 · ((1 − b) + b · max(ℓ / A, m)). Here w ≥ 0 is the term's
factor (its idf with the boost, and for Xapian the k1 + 1 and query factors), A is the average
length as the preset computes it (`search.hh:79, 95`), and m is 0 for `bm25` and `min_normlen`
for `bm25_xapian` (`search.hh:60-111`). A pair (t, ℓ) bounds its postings because g rises with t
and falls with ℓ, and a pair's ℓ is a decoded length code, never above a document's length
(`format.hh:256-257`).

Let A be the segment's average length, A' the snapshot's, and c = min(1, A / A'). Then K(ℓ, A')
≥ c · K(ℓ, A): if A' ≤ A, every part of K grows or stays; if A' > A, max(c · ℓ / A, m) ≥ c ·
max(ℓ / A, m) because m ≥ c · m, and (1 − b) ≥ c · (1 − b). And for c ≤ 1, t / (t + c · K) ≤
(1 / c) · t / (t + K), because c · t ≤ t. So g(t, ℓ; A') ≤ g(t, ℓ; A) · max(1, A' / A) for every
pair.

If the writer orders a frontier so that its first pair has the largest g at the segment's own A
under the index's preset, then every posting the list covers scores at most w · g₁ · max(1, A' /
A), where g₁ is the first pair's g at A. One evaluation bounds a block, a group, or a whole
term. In one merged segment A' = A and the bound is the exact block maximum; across segments it
loosens by the ratio of averages, a few percent for similar segments (unmeasured). The reader
multiplies by (1 + 2⁻⁴⁰) to cover rounding between the writer's ordering and its own
evaluation, which use the same expression (`weight::by_norm` and its Xapian counterpart with
w = 1). A query under the other preset, or under parameters the order did not use, evaluates
every pair as today.

M18 goes one step further: if one pair bounds a block under the index's preset, the block needs
only that pair, one per preset that must be fast. Tantivy stores exactly that, the argmax pair
under its segment's statistics, and documents it as an approximation once global statistics are
injected (`bench/phase0/query.json`, `details`); the factor above makes it a guaranteed bound.
Group and term frontiers stay whole, so any scheme or parameter set still has exact bounds, at
group granularity. M18 reverses Phase 2's accepted whole frontiers at block level
(`phase-2-gate.md:84-92`) for the presets it covers; that ruling measured merged dominating
pairs, which loosen bounds, while M18's pair loosens nothing at A' = A. It is the architect's
call.

### What the query path needs from format 13

1. A target's block located without parsing: fixed-width lasts searched in place (M16).
2. A block's bound from one read: M17's first pair at a fixed offset, or M18's argmax pair beside
   the block's advance entry.
3. M1's range bounds walk consecutive blocks' bounds, so a group's impact entries stay
   contiguous.
4. Every byte inside the carried postings row: 845.6 MB against keyed `-bp`'s 833.7 MB `.doc`
   today, 11.9 MB over (F13R:457-458), in F13-R8's single selection, where the postings row is
   the constraint and ns per touched block the objective (F13R:478-482).

## Decisions

- **D1. Scope.** The carried after-merging rows and the quick tier's classes (term,
  intersection, union, intersection_union, negated, phrase, and global, each at `TOP_10`,
  `TOP_100`, `TOP_1000`, `TOP_100_COUNT`, and `COUNT`) on one node, under the gate's `bm25`
  preset. Phase 4's and Phase 5's classes, P10's shared thresholds, and `bm25_xapian`'s rows
  (Xapiand's default, a Phase 7 seam row, `query-gap-merged.md:34-38`) are out, except that M17
  and M18 must name a preset (Q1).
- **D2. E0 sets the starting table.** The per-class table above is a projection across three
  runs at three commits. The lead's five-trial after-merging rows on the reordered index (the
  re-recording F13-R7 item 4 projects from, F13R:448-453) and a quick tier with PISA replace
  it before any mechanism is ranked; the quick tier becomes the cached baseline that later
  quick-tier nights score against (BENCHMARKS.md:195-204).
- **D3. Attribute before optimizing.** E1 runs before any mechanism is measured. Its record is
  also F13-R7 item 2's attribution (F13R:441-444): for a class that misses at the freeze, the
  time it assigns to code outside the format, each part named with its mechanism and projected
  effect.
- **D4. Results never change.** Every mechanism keeps `strata_engine dump` byte-identical on the
  reordered index (docids and `%a` scores at k = 10, 100, and 1,000 under both schemes, and
  counts; `strata_engine.cc:470-493`) and `verify` at zero mismatches. Bounds may change;
  scores, ties, and counts may not. M13 is the one candidate that changes scores, and it waits
  for the architect.
- **D5. Format items are decided by their ceilings before they are built.** E3 simulates the
  work (postings scored, blocks touched) of partitions, seeds, and traversal orders; E4 times
  the oracle seed; E5 tests the pair order in format-12 bytes (any order is valid format 12, so
  measuring it needs no new format); E6 and E7 microbenchmark layouts and codecs. An item whose
  ceiling projects below 3% on every row it targets, or whose evidence is missing at the
  freeze, leaves format 13 with its decoder and hooks (F13R:490-491).
- **D6. Format items are measured on the strongest format-free base.** Top-k innovations do not
  add (Mackenzie & Moffat, 2020), so seeds, partitions, and the pair order are measured after M1
  and M2 (E2), and seeds are worth their bytes only by what they add to M3.
- **D7. Bounds stay pairs, made cheap.** Block, group, and term bounds remain (wdf, length) pairs,
  as ARCHITECTURE requires (ARCHITECTURE.md:151-152), and M17's factor keeps one pair a valid
  bound under any snapshot's statistics. Quantized values count toward the stretch only as
  bounds, since the stretch is exact (PLAN.md:135-141); P20's quantized scores would change
  results (`frozen-index.md:29-32`), so P20 counts toward it only with exact scoring.
- **D8. Kernels live in the libraries that own them.** SIMD intersection and galloping come from
  `docset` (`intersect_merge`, `intersect_gallop`), decoding from `bitpack`; a new kernel lands
  there, additively, never as a second copy in strata (calibration, first row).
- **D9. No profile-guided optimization.** Branch and layout hints go in the code, each from a
  measured profile and each A/B'd like any other item (M12).
- **D10. No caching across queries and no parallelism inside one.** The benchmark repeats
  queries, and both baselines serve single-threaded (`p6-multisegment.md:165-168`).
- **D11. Who measures what.** The performance engineer runs in-process A/Bs (`bench/quick/ab.sh`,
  `ab.py`), counters, profiles, and microbenchmarks under holds the lead grants. They are
  evidence for keeping an item, never gate rows (PLAN.md:16-20). Ratios against the baselines
  and every gate row are the lead's, on main.
- **D12. Indexes and platforms.** Every A/B runs on the reordered merged 5M index and on the
  16-segment index (`p6-multisegment.md:176-186`); the docid-ordered merged index joins for
  order-dependent items (M3, M23). A kept item repeats its A/B on the FreeBSD VM; arm64 is owed
  until its host exists (PLAN.md:439-440).
- **D13. PISA is a report row, at `TOP_10`.** Its `TOP_100` and `TOP_1000` union rows are not
  exact (`phase-3-ratchets.md:313-318`), and its intersection_union ratio is not cited until E0
  records how the harness wrapper translates `+a b`.
- **D14. The keep rule.** A mechanism stays when, on at least one row it targets, its A/B
  interval excludes 1.0 with the median at or below 0.97 (BENCHMARKS.md:15-19), and no row's
  upper bound exceeds 1.03, rerun with ten rounds when one does (`layers.md:733-740`); together
  with D4's checks, T6's other axes, and D12's platforms. An item that loses is recorded with
  its numbers and removed (PLAN.md:375-379).

## Before the freeze, and after

| Format-13 item | Decides | Evidence | Without evidence at the freeze |
|---|---|---|---|
| M16 advance entries | Layout, widths, granularity, per-block validation | E1 (per-block shares, access traces), E6 | Chosen by F13's T3 and T4 alone |
| M17 pair order | In or out; the preset (Q1) | E5 | Out: format 12's order |
| M18 argmax pairs | In or out | E1 (frontier bytes), E5 | Out |
| M19 partition hook | In or out; pairs or one byte (Q2) | E3 | Out: F12's P0, no bytes (F13R:470-472) |
| M20 seeds | In or out | E3, E4 | Out |
| M21 positions codec field | In or out | E7 | Out: F14's default |
| M22, M23 | In or out | E6, with F11's size rows | Out |

Every other mechanism (M1–M15) is format-free and lands on its own A/B, before or after the
freeze. M1 and M2 go first because D6 measures the format items on top of them.

## Experiment queue

E0–E7 have a deadline: format 13's freeze. Each runs on the base the lead names (for the lead,
item 1), records its table in `docs/perf/` with its raw files, and passes D4's checks wherever it
changes code.

- **E0. The starting table (the lead).**
  - Procedure: the reordered merged 5M index (`strata_engine merge`, which reorders by default,
    `STRATA_REORDER_THREADS=8`) as `engines/strata-merged`; the five-trial after-merging rows
    (the query step of `bench/overnight.sh`, scored by `bench/phase2/gate.py --md --target 1.0
    --strata strata-merged`); the quick tier with every engine, PISA included, as
    `night_p1.sh`'s step 1 runs it, rebuilt into the cached baseline with
    `bench/phase2/baseline.py`. Recorded per class
    and command: per-query floor-subtracted ratios, the class's ratio by the reference's cost
    tercile, and the term class by stratum. Also how PISA's harness wrapper turns `+a b` into a
    query (D13).
  - Output: `docs/gates/phase-3-ratchets.md`.
  - Rows: every row; a measurement, kept by definition.
- **E1. Attribution.**
  - Tooling, under code review: a `STRATA_COUNTERS` build option (off by default, never in A/B
    binaries) counting per query: blocks decoded by kind (FOR, bitset, tail); groups decoded;
    advances (in block, new block, new group); `max_score` resolutions (block, group, term) by
    caller; pairs evaluated; windows (total, skipped); candidates (scored, rejected by a bound,
    collected); postings scored; positions decoded and headers stepped; allocations and bytes
    through a counting `operator new` in the tool, which also serves C15's owed rows.
    `strata_engine bench` prints them as extra columns. `poststats` (F22, on
    `phase4/format13`), or a main-side mode until it lands, gains frontier-length histograms per
    level.
  - Procedure: counters over both query sets, every command, on the reordered merged index;
    counts are deterministic, so they need no hold, only a free host at `nice -n 19`. Under a
    strict hold: `perf record -g` over `strata_engine bench IDX COMMAND 20` per class and
    command (hardware events if the guest has them, else `cpu-clock`); the three probe queries
    of H17; H15's re-derivation from E0's Lucene times; the access traces E6 replays.
  - Output: `docs/perf/query-gap-attribution.md`: per class and command, the share of time by
    component (T1), the counters, and each of H1–H19 marked confirmed, refuted, or open.
  - Keep or remove: not applicable; it orders E8–E17.
- **E2. M1 and M2.**
  - Procedure: unit tests in `search_test` (a range bound is at least every score in the range
    and at most today's bound, over synthetic lists and the stratified terms; a candidate's
    block-max AND bound is at least its score); `verify` on the four indexes and `dump` identity
    (T2); `bench/quick/ab.sh OUT A_BIN IDX B_BIN IDX 6 3` on both indexes; E1's resolution
    counts before and after.
  - Keep or remove: D14.
  - Rows: union, intersection, and intersection_union `TOP_10`, `TOP_100`, `TOP_1000`; phrase
    `TOP_k`.
- **E3. The work simulator.**
  - Tooling: `strata_engine prunesim IDX < dump.txt`. Per query and k, θ_k is the k-th score on
    `dump`'s line. An exact block-max traversal over the decoded postings counts blocks touched
    and postings scored under: partitions of 32, 40, 64, 128, and 256 postings, and variable
    partitions at average sizes 64 and 128 (Mallia et al., 2017); seeds none, oracle (θ_k), and
    F13's dominance pairs; ordinal order or bound order (groups for terms, 64Ki-ordinal ranges
    for unions). Bytes per variant under whole frontiers, M18's argmax pairs, and one byte.
  - Procedure: offline and untimed; it needs a free host and no hold.
  - Output: postings scored and blocks touched per class and variant, against today's; time
    projected through E1's per-block and per-posting costs, labeled an estimate.
  - Keep or remove: M19 stays only if an affordable variant projects at least 3% on a union or
    term row over E2's base; M20 only if seeds project at least 3% on top of bound order; E10
    is built only if bound order projects at least 3%.
  - Rows: none directly.
- **E4. The oracle seed, timed.**
  - Procedure: an experiment flag reads θ_k per query and k from a `dump` file and seeds the
    collector with the double just below it (`top_k`'s floor, `search.hh:1281-1284`;
    `top_collector::seed`, `search.hh:201`). A is the flag off, B on, same binary and index, over
    the term, union, and intersection_union queries (seeds do not apply to conjunctions or
    phrases, F13:333-338). A second B uses F13's dominance-pair seeds, computed by E3.
  - Keep or remove: the oracle is M3's and M20's ceiling in time. M20 keeps its bytes only if the
    realistic seed passes D14 on some row and E3 projects it at least 3% on top of bound order
    (M3).
  - Rows: term, union, intersection_union `TOP_k`.
- **E5. Score-ordered frontiers in format-12 bytes.**
  - Procedure: an experiment writer orders every frontier (block, group, term) by `bm25` score
    at the segment's average length, ties by wdf; the 16-segment index merged with it (untimed)
    must match the unordered merge's `strata_engine sections` sizes and pass `check`, `verify`,
    and `dump` identity. An experiment reader bounds a list by its first pair times max(1, A' /
    A) times (1 + 2⁻⁴⁰). Each binary serves its own index, as `layers.md` L15 does. Counters:
    pairs evaluated per query. From E1's histograms, M18's bytes.
  - Keep or remove: D14 puts M17 into format 13's impact entries with Q1's preset; M18 enters
    F13-R8's selection with its bytes.
  - Rows: term, union, intersection `TOP_k`.
- **E6. Advance entries and probes, microbenchmarked.**
  - Procedure: a microbenchmark under `bench/quick/` over real skip data (the 50 terms E1's
    counters touch most) replaying E1's traces (sequential scans, leapfrogs, window bounds):
    format 12's `decode_group` and search; A1; A2; A3; separate lasts and offsets; at 128 and 256
    postings per entry; probes per candidate into FOR (decode and search), bitset (rank), and
    Elias-Fano (select) blocks; `check_increasing` against M23's sum check. ns per lookup and per
    probe, bytes per entry. Under a strict hold.
  - Keep or remove: the layout with the lowest projected ns per touched block inside F13-R8's
    budget; the table goes to format 13's candidate report.
  - Rows: decides M16, M22, M23.
- **E7. Positions, offline.**
  - Procedure: from E1's phrase profiles, the share of position decoding and header stepping;
    over the 5M positions, exceptions per block and widths under today's PFOR, FOR, Elias-Fano,
    and deltas against the document's length (P13); a decode microbenchmark per codec, under a
    strict hold.
  - Keep or remove: M21 stays only if a codec projects at least 3% on a phrase row with the
    positions row still at or below 1.0× keyed `-bp` (0.980× today,
    `phase-3-ratchets.md:205`).
  - Rows: decides M21.

E8–E17 follow in the order E1's attribution ranks them; the default order is below. Each keeps
by D14 and passes T2 and T6.

- **E8. Fixed per-query costs (M8).** Allocations per query by class before and after (fewer,
  and none per match); the three probe queries; the A/B. Rows: every class's cheap tercile, the
  term class's rare and medium strata, unions and intersections of rare terms.
- **E9. Counted top-k (M4).** Rows: `TOP_100_COUNT` intersection, union, negated, and
  intersection_union.
- **E10. Bound-ordered traversal (M3),** if E3 projects at least 3%. Tests: ties straddling the
  k-th with groups visited out of order, one segment and many (`multisegment_test`'s and
  `engine_query_test`'s R2 cases). Rows: term and union `TOP_k`.
- **E11. Bulk required-plus-optional and exclusion (M5).** Rows: intersection_union and negated
  `TOP_k` and `TOP_100_COUNT`.
- **E12. Vectorized block scoring (M6).** An added test: SIMD and scalar scores bit-identical on
  every posting of the stratified terms under both presets, with floating-point contraction off
  in scoring code. Rows: term and intersection `TOP_k`, `TOP_1000` most.
- **E13. Array intersection for counts (M7) and window overhead (H16).** Rows: `COUNT` and
  `TOP_100_COUNT` intersection and negated; two-clause union counts.
- **E14. The phrase path (M10).** Rows: phrase, two-phase-critic.
- **E15. Prefetching (M11),** with last-level misses before and after where counters exist.
  Rows: every class.
- **E16. Branch and layout hints (M12),** one function at a time from E1's branch profile.
  Rows: the classes the hinted functions dominate.
- **E17. Fixed-point clause sums (M13),** only after the architect's ruling.

After the freeze: M19's partitions on format 13 if E3 kept the hook, M14 in its reserved
section, and M15 as Q3 rules.

## Gate rows

| Row | Target | Procedure |
|---|---|---|
| T1 Attribution | Per class and command of the quick tier, at least 90% of strata's in-process time on the reordered merged index assigned to named components, each with its counters; H1–H19 each confirmed, refuted, or open | E1's counters build and `perf record -g` over `strata_engine bench IDX COMMAND 20 < CLASS.txt`; recorded with raw files in `docs/perf/query-gap-attribution.md` |
| T2 Exactness | After every kept item: `verify` at zero mismatches on the reordered merged, docid-ordered merged, 16-segment, and `STRATA_DELETE_EVERY` indexes (962 + 200 queries, both schemes, k = 10, 100, 1,000); `dump`'s output on the reordered index byte-identical to the previous kept commit's | `strata_engine verify DIR < queries`, `strata_engine dump DIR < queries \| sha256sum`; ctest |
| T3 Item A/Bs | D14 on the reordered merged and the 16-segment indexes; the FreeBSD VM for kept items | `bench/quick/ab.sh OUT A_BIN IDX B_BIN IDX 6 3`, `bench/quick/ab.py OUT`, under a hold the lead grants; the table in `docs/perf/<item>.md` |
| T4 Format-13 candidates | Before the freeze, every format-13 item of this design (M16–M23) has its ceiling or candidate table and a keep or drop line, with bytes from `strata_engine sections` and `poststats` | E3–E7; recorded in `docs/perf/query-gap-format13.md` and folded into format 13's candidate report (F13-R7 item 4, F13-R8) |
| T5 The carried rows | On main, every class and command after merging at or below 1.0× the fastest baseline by the interval's upper bound, by Phase 4's gate (PLAN.md:103-116) | The lead: five trials, every engine, `bench/overnight.sh` and `bench/phase2/gate.py --target 1.0 --strata strata-merged` |
| T6 Other axes | Per kept item: allocations per query by class not growing with matches and not above before; serving memory (`bench/serve_mem.py`) at or below before; indexing and merge time at or below 1.03× before for writer-side items (M17–M23); disk exact per section inside the joint budget | The counting build; `bench/serve_mem.py`; `bench/index_trials.sh` and a timed `strata_engine merge`; `strata_engine sections` |
| T7 Phase 6 | Every class and command at or below 0.7× | As T5, at Phase 6's gate |
| T8 Stretch | Reported at every gate: the quick tier's geometric mean over its 35 rows against the fastest baseline, and its `TOP_10` rows against PISA | The quick tier (BENCHMARKS.md:188), `gate.py --md` |

## Open questions for the owner

1. **Q1. Which preset orders the frontiers (M17) and keeps argmax pairs (M18)?**
   - Options: (a) `bm25`, the gate's preset and the default (ARCHITECTURE.md:13-18); (b)
     `bm25_xapian`, Xapiand's default; (c) each index's configured default preset, recorded in
     its segments; (d) both, with M18 keeping two argmax pairs per block, one when they coincide.
   - Recommendation: (c), defaulting to `bm25`. The order costs no bytes, so an index that serves
     Xapian's formula gets the fast path for it and the gate's indexes keep `bm25`'s; a query
     under the other preset stays exact on whole frontiers. (d) only if E5 shows indexes serving
     both presets and the bytes fit.
2. **Q2. May the format store a score bound?** ARCHITECTURE says block maxima are pairs, never
   precomputed scores (ARCHITECTURE.md:151-152). If E3 shows finer partitions pay but even
   M18's single pairs do not fit the postings row, a one-byte bound per partition, rounded up,
   computed under the index's preset at the segment's average length, and scaled by M17's factor
   at query time, keeps the rule's reason (valid under any snapshot's statistics) but not its
   letter.
   - Options: (a) pairs only, as the rule stands; (b) a preset's bound beside the group and term
     pairs, used only to skip, never to score.
   - Recommendation: (b), conditional on E3. Without E3's win the question closes as (a).
3. **Q3. Do P5's next-word entries count in the disk row?** The whole index sits at 0.999×
   keyed `-bp` (`phase-3-ratchets.md:203`). BENCHMARKS rule 1 excludes data strata keeps
   beyond the baselines (BENCHMARKS.md:7-11), a rule written for semantics such as exact lengths
   and term lists.
   - Options: (a) P5's bytes count, so P5 lands only with savings elsewhere; (b) P5 is an opt-in
     index option, its bytes reported in a row of their own and left out of the ratio; (c) P5 is
     dropped.
   - Recommendation: (a). A speed structure left out of the disk row would buy the phrase rows
     with uncounted bytes. Under (a), P8's prefilters (M14) are measured first.

## For the lead

1. **The base.** The experiments touch `search.hh` and `reader.hh`, which
   `phase4/layers-refactor` rewrote (steps 5a and 6) and which format 13 must stay out of until
   its rebase (F13R:153-157). Recommended: build them on the layers tip once its CI is green,
   with A and B both on that base. Layers changes no format-12 byte and lands before format 13,
   so the ratios carry to main. The alternative, main now and a rebase later, conflicts in
   `postings_iterator` and the scorers.
2. **E0** is the lead's (D2, D11), and the five-trial run is the one F13-R7 item 4 projects from.
3. **Tooling under code review:** E1's counters and counting allocator (which also close C15's
   owed allocation rows), E3's `prunesim`, and E6's microbenchmark.
4. **Holds and order:** E1's profiles, E4–E7's timings, and every A/B take a strict hold each,
   minutes to an hour (BENCHMARKS.md:187), between gate nights. The indexing gap is queued first
   for the same agent type and host (the index at main `5a8e239`, "Queued", item 0); E0–E7 carry
   the freeze's deadline, so the lead orders the two.
5. **Format 13:** E3–E7's tables are inputs to F13-R7 item 4 and F13-R8's selection; the
   format-13 contributor folds them into its candidate report.

## For the architect

1. M17's bound, and M18's departure from whole frontiers at block level (`phase-2-gate.md:84-92`).
2. M13's fixed-point sums, P6's open question (`p6-multisegment.md:286-294`).
3. M3's out-of-order collection in docid-ordered segments, under the rule reordered segments use.

## First steps

None of these waits on an open question:

1. E1's tooling: the counters option, the counting allocator, the probe queries, and `poststats`'
   histograms.
2. E3's `prunesim`, reading `dump`'s output.
3. M1 and M2 with E2's unit tests, `verify`, and `dump` identity.
4. M8's items, each bit-identical by construction.
5. E4's seed flag, a few lines over `top_k`'s existing floor.

## Risks

- **The starting table is a projection.** Three runs, three commits, a drift-flagged night. E0
  replaces it; until then every reduction above is an estimate.
- **Gains do not add.** Each item is measured on top of the ones kept before it (D6), so the
  queue's order changes what each one shows.
- **Floor subtraction magnifies fixed costs.** Cheap queries can dominate a class's geometric
  mean through a few µs of setup; E1's tercile split keeps that from passing as a pruning gap, or
  the reverse.
- **The freeze's deadline.** E0–E7 share the gate VM with gate nights and with the indexing gap,
  queued first. A format item without its evidence at the freeze leaves format 13 (D5); a later
  win then costs format 14 and its N − 1 reader (F13:56-58).
- **Writer-side work while indexing trails.** M17–M23 add work at flush and merge while
  one-thread indexing runs at about 0.46× Tantivy's throughput in T4's first two rounds (the
  index at main `5a8e239`, "Queued", item 0; not yet a gate row), so T6's 1.03× bound on
  indexing and merge time applies to each of them.
- **Bit identity under vectorization.** A SIMD path whose floating-point contraction or operation
  order differs from the scalar one changes scores. E12's test and T2's `dump` identity catch it;
  scoring code compiles with contraction off.
- **Profiles in the VM.** Hardware counters under QEMU with HVF are unverified; without them E1
  samples on `cpu-clock` and takes branch and cache counts on the macOS host.
- **Presets outside the order.** Under M17 or M18, a query under the other preset or other
  parameters stays exact but bounds blocks by whole frontiers (M17) or by group frontiers (M18).
- **Rebase churn.** Query-path code conflicts with the layers branch until it lands (for the
  lead, item 1).
- **A figure without a source.** H15's "roughly 90 ns for Lucene" has no recorded derivation;
  no decision here rests on it until E1 re-derives it.

## References

- Broder, Carmel, Herscovici, Soffer, Zien. Efficient query evaluation using a two-level
  retrieval process. CIKM 2003. (WAND.)
- Chambi, Lemire, Kaser, Godin. Better bitmap performance with Roaring bitmaps. Software:
  Practice and Experience, 2016.
- Dhulipala, Kabiljo, Karrer, Ottaviano, Pupyrev, Shalita. Compressing graphs and indexes with
  recursive graph bisection. KDD 2016.
- Ding, Suel. Faster top-k document retrieval using block-max indexes. SIGIR 2011. (Block-max
  WAND and block-max AND.)
- Lemire, Boytsov. Decoding billions of integers per second through vectorization. Software:
  Practice and Experience, 2015.
- Lemire, Boytsov, Kurz. SIMD compression and the intersection of sorted integers. Software:
  Practice and Experience, 2016.
- Mackenzie, Moffat. Examining the additivity of top-k query processing innovations. CIKM 2020.
- Mackenzie, Petri, Moffat. Anytime ranking on document-ordered indexes. ACM TOIS, 2022.
- Mallia, Ottaviano, Porciani, Tonellotto, Venturini. Faster BlockMax WAND with variable-sized
  blocks. SIGIR 2017.
- Mallia, Siedlaczek, Mackenzie, Suel. PISA: performant indexes and search for academia.
  OSIRRC at SIGIR 2019.
- Mallia, Siedlaczek, Suel. An experimental study of index compression and DAAT query processing
  methods. ECIR 2019.
- Ottaviano, Venturini. Partitioned Elias-Fano indexes. SIGIR 2014.
- Petri, Moffat, Mackenzie, Culpepper, Beck. Accelerated query processing via similarity score
  prediction. SIGIR 2019. (Threshold estimation.)
- Turtle, Flood. Query evaluation: strategies and optimizations. Information Processing and
  Management, 1995. (MaxScore.)
- Williams, Zobel, Bahle. Fast phrase querying with combined indexes. ACM TOIS, 2004.
- The search-benchmark-game harness's engine descriptions for Lucene 10.4.0 and Tantivy 0.26,
  as recorded in `bench/phase0/query.json` (`details`): block sizes, skip levels, impact
  layouts, and the top-k algorithms each implements.
