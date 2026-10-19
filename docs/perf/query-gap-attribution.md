# Query gap, E1: attribution (T1)

The design's E1 (`docs/design/query-gap.md`, branch `design/query-gap`): split every row above
1.0x into named parts with counters and profile shares, and give each of H1 to H22 its verdict
by the design's rule. It keeps nothing; it orders the rest of the queue.

## Plan (written before the runs)

- Rows: every class and command of the quick-tier calibration (`docs/status/query-gap.md`,
  the lead's "Calibration, quick tier", gate VM `~/bench/results/query-cal1`) whose
  floor-subtracted ratio against the reference set is above 1.0x.
- Index: the cached 5M reordered merged index (`engines/strata-reordered/idx`, `manifest-3`,
  one segment `seg-200.sx`), Q5; the 16-segment index is that directory's `manifest-2` for
  H20.
- Queries: S, the calibration's `queries-sample.txt` (181) and `terms-sample.txt` (40).
- Counters: a `STRATA_COUNTERS=ON` `strata_engine bench IDX COMMAND 1` over S at every
  command; counts are deterministic, so one run each.
- Profiles: a Release `strata_engine` (counters off) under `perf record -g`, single-threaded,
  `bench IDX COMMAND 20 < CLASS.txt` per class and command, under a light hold.
- Verdicts: the design's rule (refuted below a 3% ceiling on every target row, confirmed at a
  5% ceiling with the counter at its level, open otherwise or while a later experiment is
  owed).
- What would change the queue: a hypothesis with a ceiling under 3% everywhere drops its
  mechanism from the queue; one confirmed above 5% on a freeze item moves it first.

## How it ran (2026-10-19)

- Gate VM (Ubuntu 24.04 under QEMU/HVF on the host, 8 vCPUs), one thread pinned with
  `taskset -c 2`, under light holds (`quiet run`, `QUIET_LEVEL=light`).
- Binaries, both from this branch at `5b67776` (counting code compiled out when off, so the
  Release build is main's query path): `build-rel` (Release, clang 18, `-march=native
  -fno-omit-frame-pointer -g`; debug info only for symbolizing) and `build-cnt` (the same with
  `STRATA_COUNTERS=ON`, counts only, never timed).
- Index: `engines/strata-reordered/idx` at `manifest-3`, the reordered merged 5M index (one
  segment). The 16-segment index is the same directory's `manifest-2` copied with its 16
  segments into `~/bench/results/qexp-e1/idx16` (copies, since the journal refuses hard links);
  those segments are the pre-merge, docid-ordered ones.
- Phase times: counters over S, 10 runs, about 70 s in all; profiles, 40 runs of `perf record
  -e cpu-clock -F 4999 -g` (50 rounds per query, 400 for the cheap classes), 298 s; in-process
  times, 5 interleaved trials over both indexes, 5 runs per query (200 for the probes), 226 s.
  Per-run phase logs: `prof/phases.log`, `times/phases.log`.
- Raw files: `~/bench/results/qexp-e1/` on the gate VM, every file's SHA-256 in
  `query-gap-attribution.sha256` beside this file. Scripts: this worktree's `.scratch/e1/`
  (`profile.sh`, `times.sh`, `shares2.py`, `analyze.py`, `table.py`).
- Shares: in-query samples only (index open excluded), each symbolized with its inline chain
  (`llvm-symbolizer --inlining`) and named by the innermost frame a rule claims; arithmetic
  helpers (`by_norm`, `weight::operator()`, LEB128, `std::`) defer to their caller, so a pair
  evaluated for a bound counts as `bound` and the same pair for a score as `score`.
- Parts: score (`score_of`, `score_window`, `sum_parts`), coll (`top_collector`, docid
  lookups for ties), adv (`advance`, `next`, `count_below`, `advance_bits`, `align`), dec
  (`bitpack`, `load_next_block`, `load_bitset`, tails), skip (`decode_group`,
  `block_index_for`, skip-entry LEB128 and impacts), bound (`max_score`, `skip_blocks`,
  `block_bound`, `group_bound`, `prune`, `sum_upper`), loop (the scorer's own inlined loop:
  for unions the window's accumulator walk), fill (`fill`, `or_bitset`, popcounts), pos
  (`phrase_freq`, `positions_reader`, position headers), phr (the rest of the phrase scorer),
  other (mostly `residency::verified_regions::verify`).
- Counter definitions are the code's at `b7823cd` (CR5 is the architect's to rule):
  `bound_evals` mixes memoized bounds over a list's pairs with block tests that stop at the
  first competing pair, so pairs per bound is a mean over both; `advances_new_group` counts an
  unstarted iterator's first advance, a move into the tail, and an advance on an exhausted
  iterator. In a union's counting mode, windows skipped by their summed bounds count their
  documents unscored, so candidates + rejected = matches holds for conjunctions only (the
  re-check's note); the union `TOP_100_COUNT` row's unscored matches are matches less
  candidates less rejected.

## Every row above 1.0x, split

Ratios, strata's and the fastest engine's µs are the lead's quick-tier calibration (3 trials,
reordered merged, floor-subtracted against the reference set); the in-process µs and parts are
this run's, the mean over the row's queries of each query's median of five trials. Parts are
shown at 5% and above. The term rows' calibration rests on the gate set's one term query
(`queries-sample.txt`; `terms-sample.txt` was not in that run), while their parts and counters
are over the 40 stratified terms: they do not describe the same queries.

| Row | n | Ratio [interval] | strata µs | Fastest µs | Gap µs | In-process µs | Parts, µs (share) | Counters per query |
|---|---:|---|---:|---|---:|---:|---|---|
| TOP_10/intersection_union | 40 | 1.61× [1.58–1.69] | 2,909 | Lucene -bp 1,819 | 1,090 | 3,434 | score 1,152 (34%), adv 934 (27%), bound 569 (17%), dec 324 (9%) | blocks 1,829; adv 30,573 (4% new block); pairs/bound 2.6; max_score grp+term 35%; scored 100,091; cand 59,836 |
| TOP_10/phrase | 40 | 1.21× [1.21–1.29] | 3,340 | Lucene -bp 3,222 | 118 | 3,689 | pos 1,848 (50%), adv 681 (18%), dec 473 (13%), phr 260 (7%), other 189 (5%) | blocks 2,042; adv 60,921 (3% new block); cand 442 |
| TOP_10/union | 40 | 1.28× [1.28–1.40] | 1,258 | Lucene -bp 987 | 271 | 1,402 | loop 627 (45%), score 307 (22%), adv 176 (13%), dec 114 (8%) | blocks 968; adv 6,164 (9% new block); pairs/bound 4.1; max_score grp+term 30%; scored 51,723; cand 8,310; win 548 (16 skipped) |
| TOP_100/intersection | 40 | 1.04× [1.00–1.15] | 1,455 | Lucene -bp 1,388 | 67 | 1,609 | adv 383 (24%), score 381 (24%), loop 266 (17%), dec 256 (16%), skip 198 (12%) | blocks 2,157; adv 36,173 (5% new block); pairs/bound 4.3; max_score grp+term 61%; scored 48,227; cand 4,955; win 416 (49 skipped) |
| TOP_100/intersection_union | 40 | 1.54× [1.54–1.60] | 3,283 | Lucene -bp 2,164 | 1,119 | 3,975 | score 1,323 (33%), adv 1,136 (29%), bound 595 (15%), dec 371 (9%), coll 207 (5%) | blocks 1,939; adv 37,442 (3% new block); pairs/bound 3.1; max_score grp+term 35%; scored 105,509; cand 77,134 |
| TOP_100/negated | 19 | 1.11× [1.00–1.22] | 572 | Tantivy 529 | 43 | 484 | adv 165 (34%), score 100 (21%), dec 67 (14%), loop 43 (9%), bound 31 (6%), skip 28 (6%), coll 25 (5%) | blocks 629; adv 6,212 (9% new block); pairs/bound 1.9; scored 5,877; cand 5,877; excl/incl 0.53 |
| TOP_100/phrase | 40 | 1.14× [1.09–1.22] | 3,496 | Tantivy 3,256 | 240 | 4,060 | pos 2,026 (50%), adv 717 (18%), dec 560 (14%), phr 312 (8%), other 212 (5%) | blocks 2,173; adv 63,285 (3% new block); cand 665 |
| TOP_100/term | 1 | 1.02× [0.97–1.06] | 1,568 | Lucene -bp 1,595 | -27 | 285 | score 77 (27%), coll 57 (20%), dec 51 (18%), skip 39 (14%), bound 19 (7%), other 15 (5%) | blocks 591; adv 447 (100% new block); pairs/bound 3.3; scored 18,411; cand 18,411 |
| TOP_100/union | 40 | 1.28× [1.28–1.38] | 1,925 | Lucene -bp 1,637 | 288 | 2,219 | loop 887 (40%), score 458 (21%), adv 304 (14%), dec 238 (11%) | blocks 1,508; adv 12,273 (7% new block); pairs/bound 4.2; max_score grp+term 32%; scored 79,462; cand 24,572; win 614 (1 skipped) |
| TOP_1000/intersection | 40 | 1.15× [1.03–1.25] | 1,806 | Lucene -bp 1,686 | 120 | 1,945 | score 514 (26%), adv 468 (24%), dec 327 (17%), loop 315 (16%), skip 163 (8%) | blocks 2,571; adv 55,121 (4% new block); pairs/bound 4.4; max_score grp+term 63%; scored 61,321; cand 9,488; win 416 (9 skipped) |
| TOP_1000/intersection_union | 40 | 1.30× [1.30–1.37] | 3,656 | Lucene -bp 2,691 | 965 | 4,283 | score 1,402 (33%), adv 1,150 (27%), bound 568 (13%), dec 436 (10%), coll 311 (7%) | blocks 1,978; adv 40,384 (3% new block); pairs/bound 3.9; max_score grp+term 35%; scored 107,442; cand 89,173 |
| TOP_1000/negated | 19 | 1.20× [0.98–1.26] | 771 | Tantivy 687 | 84 | 700 | adv 189 (27%), score 141 (20%), dec 109 (16%), coll 82 (12%), other 54 (8%), loop 48 (7%), bound 40 (6%) | blocks 699; adv 7,663 (8% new block); pairs/bound 1.4; scored 7,098; cand 7,098; excl/incl 0.54 |
| TOP_1000/phrase | 40 | 1.21× [1.03–1.26] | 3,693 | Tantivy 3,329 | 364 | 4,049 | pos 1,971 (49%), adv 745 (18%), dec 576 (14%), phr 295 (7%), other 227 (6%) | blocks 2,245; adv 64,600 (3% new block); cand 787 |
| TOP_1000/term | 1 | 1.29× [1.28–1.42] | 4,694 | Lucene -bp 3,489 | 1,205 | 822 | score 227 (28%), coll 226 (27%), dec 153 (19%), other 64 (8%), skip 63 (8%) | blocks 855; adv 464 (100% new block); pairs/bound 2.8; scored 49,915; cand 49,915 |
| TOP_1000/two-phase-critic | 1 | 1.23× [1.09–1.25] | 31,479 | Tantivy 26,448 | 5,031 | 37,988 | pos 13,756 (36%), dec 10,485 (28%), other 5,774 (15%), adv 4,733 (12%) | blocks 21,778; adv 212,738 (10% new block); scored 660; cand 660 |
| TOP_1000/union | 40 | 1.19× [1.13–1.25] | 3,421 | Lucene -bp 3,035 | 386 | 4,033 | loop 1,409 (35%), score 933 (23%), dec 447 (11%), adv 425 (11%), coll 345 (9%) | blocks 2,476; adv 17,070 (8% new block); pairs/bound 4.3; max_score grp+term 35%; scored 152,922; cand 88,327; win 745 (0 skipped) |
| TOP_100_COUNT/intersection | 40 | 1.26× [1.15–1.33] | 2,308 | Lucene -bp 1,592 | 716 | 2,535 | adv 1,205 (48%), score 505 (20%), dec 326 (13%), bound 213 (8%), skip 131 (5%) | blocks 2,689; adv 74,797 (3% new block); scored 54,370; cand 15,754 |
| TOP_100_COUNT/intersection_union | 40 | 1.13× [1.09–1.20] | 3,359 | Lucene -bp 2,897 | 462 | 3,860 | score 1,286 (33%), adv 1,078 (28%), bound 607 (16%), dec 367 (10%) | blocks 1,939; adv 37,442 (3% new block); pairs/bound 3.1; max_score grp+term 35%; scored 105,509; cand 77,134 |
| TOP_100_COUNT/negated | 19 | 1.27× [1.15–1.39] | 691 | Tantivy 539 | 152 | 609 | adv 202 (33%), score 119 (19%), dec 81 (13%), loop 60 (10%), bound 54 (9%) | blocks 757; adv 8,351 (8% new block); pairs/bound 1.9; scored 6,824; cand 6,824; excl/incl 0.54 |
| TOP_100_COUNT/phrase | 40 | 1.17× [1.10–1.22] | 3,684 | Tantivy 3,249 | 435 | 4,088 | pos 2,011 (49%), adv 733 (18%), dec 570 (14%), phr 300 (7%), other 235 (6%) | blocks 2,256; adv 64,810 (3% new block); cand 808 |
| TOP_100_COUNT/two-phase-critic | 1 | 1.22× [1.20–1.28] | 31,888 | Tantivy 25,555 | 6,333 | 38,741 | pos 13,982 (36%), dec 10,851 (28%), other 5,869 (15%), adv 4,761 (12%) | blocks 21,778; adv 212,738 (10% new block); scored 660; cand 660 |
| TOP_100_COUNT/union | 40 | 1.12× [1.03–1.18] | 10,894 | Tantivy 6,367 | 4,527 | 13,213 | score 4,930 (37%), loop 4,310 (33%), coll 1,594 (12%), dec 1,097 (8%), adv 702 (5%) | blocks 7,700; adv 6,572 (8% new block); pairs/bound 4.6; max_score grp+term 44%; scored 894,146; cand 731,681; win 747 (0 skipped) |
| COUNT/intersection | 40 | 1.10× [1.03–1.15] | 746 | Lucene -bp 615 | 131 | 748 | fill 245 (33%), adv 169 (23%), dec 146 (20%), skip 117 (16%) | blocks 1,594; adv 7,303 (9% new block) |
| COUNT/negated | 19 | 1.08× [1.08–1.09] | 389 | Tantivy 396 | -7 | 282 | fill 140 (50%), dec 49 (18%), adv 40 (14%), skip 25 (9%) | blocks 578; adv 1,557 (12% new block); excl/incl 0.06 |
| COUNT/phrase | 40 | 1.16× [1.15–1.31] | 3,575 | Tantivy 3,235 | 340 | 3,956 | pos 1,916 (48%), adv 765 (19%), dec 554 (14%), phr 283 (7%), other 209 (5%) | blocks 2,256; adv 64,810 (3% new block) |
| COUNT/two-phase-critic | 1 | 1.17× [1.15–1.17] | 30,927 | Tantivy 26,785 | 4,142 | 37,006 | pos 13,396 (36%), dec 10,025 (27%), other 5,691 (15%), adv 4,800 (13%) | blocks 21,778; adv 212,738 (10% new block) |

Where each gap sits, in short:

1. **intersection_union `TOP_k`** (1.30–1.61x, about 1 ms a query): score 33–34% and adv
   27–29% over 59,836–89,173 candidates a query, and `required_optional_scorer::prune` alone
   7–8% (bound 13–17%). The required side is walked document by document; H11's counter is not
   built, so how many of those visits could not win is not yet known.
2. **phrase** (1.14–1.21x): verification is 56–58% (pos 49–50%, phr 7–8%), H13's share, flat
   across commands as the record says; postings work (adv, dec) is another 32%.
3. **union `TOP_k`** (1.19–1.28x, 271–386 µs): the window's accumulator walk (loop) is 35–45%
   and score 21–23%; bounds are 3–4%. Almost no window is skipped (16, 1 and 0 of 548–745),
   so the windows' bounds do not reach θ: the per-window split runs, and everything scores.
4. **union `TOP_100_COUNT`** (1.12x, 4.5 ms): 731,681 candidates scored of 810,566 matches
   against `TOP_100`'s 24,572: H8's one-pass counting cost, in score 37% and loop 33%.
5. **intersection `TOP_100_COUNT`** (1.26x, 716 µs): every match scored (candidates =
   matches = 15,754, H6), adv 48%.
6. **negated** (1.08–1.27x, 43–152 µs): adv 27–34%, the exclusion's own frames 9–13%; one
   excluded advance per two included candidates.
7. **two-phase-critic** (1.17–1.23x, 4–6 ms; one query): pos 36%, dec 27–28%, and region
   verification 10–13% (`residency::verified_regions::verify`), a part no hypothesis names.
8. **intersection `COUNT`** (1.10x, 131 µs): fill 33%, skip entries 16% at 253 ns per touched
   block (H15's level), dec 20%.
9. **term `TOP_1000`** (1.29x on one query): score 28% and coll 27%; the collector's heap
   with docid tie-breaks (`docids::detail::get_bits`, 8%) is as large as scoring.

## Verdicts, H1 to H22

By the design's rule: refuted below a 3% ceiling on every target row; confirmed at 5% or more
on a target row with the counter at its level; open otherwise, or while a measurement it needs
is owed. Ceilings are fractions of strata's in-process time on the row.

| H | Counter (level) | Ceiling, by row | Verdict |
|---|---|---|---|
| H1 | Blocks scored that the final θ would skip: needs E3's oracle mode | E4a's measured reduction | Open (E3, E4a) |
| H2 | Pairs per bound 2.8–3.7 (term), 4.1–4.3 (union), 4.3–4.4 (intersection); level 2 met | bound share x (1 − 1/pairs): term `TOP_10` 7.9%, `TOP_100` 4.7%, `TOP_1000` 2.0%; union 2.2–2.8%; intersection 0.9–2.1% | **Confirmed** on term `TOP_10` (and above 3% on term `TOP_100`); below 3% on every union and intersection row |
| H3 | Postings scored: 4,181–49,915 (term), 37,265–61,321 (intersection) | score share x (1 − 1/2): term 8.7–13.8%, intersection 10.6–13.2% (s = 2 an estimate until E12) | **Confirmed** |
| H4 | Non-lead window bounds at group or term: 61–63% of intersection `max_score` calls (all callers, mostly the conjunction's windows); level 30% met | E3's calibrated projection, then E2's A/B | Open (E2) |
| H5 | Advances into a new block: 3.4–8.7% of intersection advances (new group 0.2–1.0%); level 30% not met | skip + `load_next_block` share 7.5–20% | Open: few advances leave the block, but each that does costs enough that the share is large |
| H6 | Candidates scored over matches 1.00 on intersection `TOP_100_COUNT`; level 0.5 met | score share 20% x (1 − 0.31) = 13.7%, with `TOP_100`'s 4,955 pruned candidates standing in for the uncounted "block bound competes" | **Confirmed** (ceiling an estimate) |
| H7 | Clause density not counted; union `max_score` at group or term 30–44% over all clauses | E3, then E2 | Open (E2, E3) |
| H8 | The record's 0.37x and 0.26x; here 731,681 candidates of 810,566 matches against `TOP_100`'s 24,572 | 1 − E9a's ratio | Open (E9a) |
| H9 | 2 to 6 clauses x 32 KiB zero-filled | setup at most 0.4% of every union row; the union-of-two-df-1-terms probe, 6.4 µs in all, is 2.3% of the cheapest union tercile at `TOP_10` (283 µs) | **Refuted** |
| H10 | Postings scored under partition variants: needs E3 | E3 | Open (E3) |
| H11 | Required visits that cannot win: not counted | `prune` 7–8%, bound 13–17%, score 33% of intersection_union | Open (counter owed) |
| H12 | Excluded advances per included candidate 0.53–0.54; level 1 not met | exclusion frames 9.3–12.8% of negated `TOP_1000`, `TOP_100_COUNT` | Open |
| H13 | Candidates 442–808 a query; verification (pos + phr) 56–58% | x (1 − 920/1,566): 22.9–23.8% | **Confirmed** |
| H14 | Exceptions and widths per positions block: E7 | E7 | Open (E7) |
| H15 | 253 ns per touched block on intersection `COUNT`, 198 on union `COUNT`; level 200 met on intersection | skip share 16% (intersection), 12% (union), x (1 − E6's ratio) | Open (E6) |
| H16 | Count windows 233, blocks touched 2,957 a query (intersection `COUNT`) | fill share x (1 − 1/2): intersection `COUNT` 16.4%, negated `COUNT` 24.9% (s′ = 2 an estimate until E13) | **Confirmed** |
| H17 | Probes, in-process: absent term 1.3 µs, df-1 term 2.0, union of two df-1 terms 6.4; term:rare's mean is 2.2 µs, all fixed cost. Allocations: no allocation-counting build on this base | Probe over the row's floor-subtracted reference: no reference for term:rare (the calibration ran without `terms-sample.txt`) | Open (the terms file's calibration; the allocation build) |
| H18 | The guest exposes no hardware events (`perf stat`: cycles, instructions, LLC misses all "not supported") | E15 | Open (host counters, E15) |
| H19 | As H18 | E16 | Open (E16) |
| H20 | 16 segments over merged, in-process geometric means: 1.24–3.23x; the absent-term probe 1.3 → 12.7 µs, so about 0.7 µs per segment per term before any posting | per-segment fixed work is 11 of term `COUNT`'s 12 µs and 39 of term `TOP_10`'s 125 µs | **Confirmed** on the cheap rows; the 16 segments are docid-ordered, so the rest of their excess also carries order, not only per-segment work |
| H21 | Reordering quality against PISA's | below | see "H21" |
| H22 | Decode cost per posting against SIMD-BP128 | `docs/perf/query-gap-codec.md` | see that record |

A part no hypothesis names: `residency::verified_regions::verify`, the per-block region check
behind `need()`, is 10–13% of two-phase-critic (4–5 ms of its 4–6 ms gap) and 2–3% of phrase.
It is P15's verify-on-first-use: a query that touches 21,778 blocks walks the region table per
block. It goes to the lead as a named part for the queue.
