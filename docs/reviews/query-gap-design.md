# strata review: query gap design (preliminary)

- Date: 2026-10-17.
- Reviewed:
  - `docs/design/query-gap.md` revision 0 at `8a6503f` on `design/query-gap` (`a489a72` the
    design, `8a6503f` the indexing gap's place in the queue) and the ledger
    `docs/status/query-gap.md` at `8a6503f`.
  - The code the design cites, at main `5691ec7`, which is the worktree's code (`git diff
    5691ec7 8a6503f` touches only the two docs): `include/strata/{search,searcher,reader,
    writer,format,scorer,check}.hh`, `tools/{engine_query.hh,strata_engine.cc,parity.cc}`,
    `bench/{quick/ab.sh,quick/ab.py,phase2/gate.py,overnight.sh,night_gate.sh,night_p1.sh}`;
    `bitpack.hh` at its pin `abf8972` and `docset.hh` at `0859e43` (`CMakeLists.txt:10-11`).
  - `docs/design/format-13.md` and `docs/reviews/format-13-design.md` on
    `origin/phase4/format13` at `4266374` (F1, F10 to F14; F13-R7, F13-R8, F13-R12).
  - `origin/phase4/layers-refactor` at `707066b`: `docs/status/layers-refactor.md`; main's
    `docs/design/layers.md` (L15, the copy paths, R8).
  - Raw results. n1: the five trials' ten JSON files, host copies in
    `~/.scratch/n1/q{1..5}-{queries,terms-stratified}.json`, which rescore to the committed
    table digit for digit (floor 67 µs, every ratio and every mean,
    `phase-3-ratchets.md:131-172`). n2: its `gate.py --md` output, `~/.scratch/quick-n2.md`.
    n2's and T2's raw JSON live only in the gate VM and were not read.
- Checked against:
  - `docs/reviews/calibration.md` at main `5a8e239`, all three rows: one mechanism twice
    (QG-R7, QG-R17, QG-R20); CI on every platform for the cited commits ("Checked by"); every
    cited figure traced to its raw file (QG-R11, QG-R12, QG-R20).
  - PLAN: the process rule and SYS-6 (PLAN.md:3-20), Goals 1 and 2, Ratchets (Passing,
    Carried rows, Query speed, the stretch), Every gate, Track P (PLAN.md:373-448).
  - BENCHMARKS rules 1, 3, and 4, "Tiers", cached baselines, "Ratchet rows"; TESTING.md's
    layers and coverage targets.
  - `docs/gates/phase-3-ratchets.md` (n1, n2, P1 T2, verify and parity, gate status);
    `docs/perf/` (query-gap-merged, p6-multisegment, p6a, p7, bm25-presets, reclamation).
  - Phase 2's D3 rule (`phase-2-design.md:52-54`) and gate ruling 2 (`phase-2-gate.md:84-92`);
    `reordering.md` D5; `frozen-index.md`; ARCHITECTURE's codec rules.
- Checked by:
  - Every code citation in the design read at its line at `5691ec7`; all land on what the
    design says, except where a finding says otherwise.
  - Rescoring n1's raw files with gate.py's rules (fastest of a query's runs, floor
    subtracted from both sides, per-class reference the largest geometric mean over Lucene,
    Lucene `-bp`, and Tantivy; median over trials; no bootstrap). Probes, in the reviewer's
    scratchpad `/private/tmp/claude-501/-Users-kronuz-code-Kronuz-Xapiand/b5982d78-0a0d-4769-bdb3-75c9e8136cd9/scratchpad/qg/`:
    - `n1means.py`: every engine's mean per class and command, and the reference engine per
      trial.
    - `n1split.py`: ratios by stratum (`term:rare`, `term:medium`, `term:frequent`) and by
      the reference engine's cost tercile.
    - `twopass.py`: per query, strata's `TOP_100_COUNT` against its own `COUNT` plus
      `TOP_100` (one floor subtracted).
    - `n2rows.py`, `stretch.py`, `pisa.py`: the n2 table's 35-row means, the stretch's
      starting point with per-class T2 factors, and PISA's position with and without
      intersection_union.
    - `headroom.py`: each n1 row's upper bound over its median.
    - `probes/m17_bound.py`, `probes/m17_loosen.py`, `probes/m17_tighter.py`: M17's bound
      in IEEE doubles, mirroring `weight` (`search.hh:60-111`), the length codes
      (`format.hh:258-268`), and `competitive()` (`format.hh:493-509`), over random blocks,
      both presets, and snapshot averages A'/A from 1/4 to 4.
  - CI through `gh`: main `5691ec7`, run 39071019926, all eight jobs pass (linux gcc, clang,
    arm64 gcc, ASan+UBSan, TSan; macOS x86_64 and arm64; FreeBSD). The layers tip `707066b`,
    run 39074035208, passes. `8a6503f` is docs-only and has no run, by design (`5691ec7`).
  - Not run: no build, test, sanitizer, fuzzing, or benchmark (`quiet check` exits 1:
    strata-trials, `night_gate.sh n1017`, strict until 01:17). The gate VM was not touched.
    The probes are Python models of strata's expressions, not strata's binary. No index was
    opened, so no frontier histogram exists.

## Verdict

**APPROVE WITH CONDITIONS: the method holds and M17's bound is correct, but format 13's
items need a tighter bound, a checked invariant, a memory-safe M23, and a byte budget before
they enter F13-R8's selection, and no item may be kept until oracle parity and a per-class
keep rule are in the gates.**

What holds, and reproduces:

- Attribute before optimizing (D3), format items decided by measured ceilings before the
  freeze (D5), and every result byte-identical (D4): the right frame for a format that
  freezes once (PLAN.md:16-18; F13-R7, F13-R8).
- The per-class projection's 35 cells reproduce as n1's floor-subtracted median times T2's
  class factor (`query-gap.md:85-93`; `phase-3-ratchets.md:133-172, 331-337`), and the
  reductions in "What the targets ask" follow from them (`query-gap.md:140-144`).
- n1's raw files rescore to the committed table exactly (`n1means.py`), so its ratios are
  sound evidence.
- M17's inequality chain is correct (`query-gap.md:413-427`): 0 of 200,000 random blocks
  bound below their true maximum (`m17_bound.py`).
- M9's identity holds for all 256 codes, not only 0 to 247: `norm_k` is built from
  `decode_length` of every code (`search.hh:81`), and `encode_length(decode_length(c))`
  decodes to the same length.
- The mechanisms the design builds on exist where it says: the seed path (`top_k`'s floor,
  `search.hh:1279-1284`; `top_collector::seed`, `:201`), the tie rule for out-of-order
  arrival (`search.hh:181-187, 288-298`; `reordering.md` D5), the per-group block-bound cache
  (`search.hh:385-394`), `docset`'s kernels (`docset.hh:187-236` at `0859e43`).
- D8 (kernels in the libraries that own them), D9, D10, and D11 are sound.

What blocks:

- QG-R1 to QG-R7 change what format 13 decides and come before any item of this design
  enters F13-R8's selection; QG-R7 also comes before E3 is built.
- QG-R8 to QG-R10 come before any item is kept.

## Findings

### Format 13

1. **QG-R1. M17's bound is correct and looser than its own proof allows; it needs stated
   preconditions, argmax-first order, and a per-segment record.**
   - The chain holds (`query-gap.md:413-427`). `m17_bound.py`: 0 of 200,000 bounds fall below
     the block's true maximum, and the tightest is 1 + 9.1e-13 above it. The factor is
     load-bearing: the first pair evaluated at A' without it understates 6,865 of 200,000.
   - The last step, t / (t + c·K) ≤ (1/c) · t / (t + K), is not needed. g orders pairs by
     K / t, and scaling every K by c keeps that order, so w · t₁ / (t₁ + c · K(ℓ₁, A)), with
     c = min(1, A / A'), bounds the list directly. By that same step it is never looser than
     the design's form.
   - Against today's whole-frontier bound at A', on synthetic 128-posting bm25 blocks: the
     design's form is looser by about A'/A itself when A' > A, 1.050×, 1.248×, and 1.992× at
     A'/A = 1.05, 1.25, and 2 (`m17_loosen.py`), and within 1.5% when A' ≤ A. The tighter form's
     median is at most 1.0013× in every bin from 1/4 to 4, where the design's reaches 2.54×
     (`m17_tighter.py`). Both are valid: 0 of 120,000 below the true maximum. With the
     design's form, every segment whose average is below the snapshot's prunes less on the
     before-merging rows.
   - Preconditions the design does not state: 0 ≤ b ≤ 1 (for (1 − b) ≥ c(1 − b)), k1 ≥ 0,
     m ≥ 0, and a boost ≥ 0, which `weight::bm25` does not enforce (`search.hh:76-78`). A and
     A' are the clamped averages the presets use: max(1, avg) for bm25 (`search.hh:79`), 1
     when zero for bm25_xapian (`:95`).
   - Only the first position matters. With the argmax first, a skip test decides on the first
     pair either way (today's `any_block_impact` reads on only to find a pair that passes,
     `reader.hh:590-594`; `format.hh:358-383`), so the order of the rest is never read. Store
     argmax-first and keep the rest in `competitive()`'s order (`format.hh:493-509`): the
     remaining pairs stay monotone and can be delta-coded, which F13-R8's budget may need
     (QG-R4, QG-R6).
   - "Bytes: None" (`query-gap.md:349`) is per block. A reader may trust the order only if
     the segment records it: the preset whose order holds, or none. Format 13's header flags
     and footer reserved word must be zero (F13:141-142), so this is a new field of a few bytes
     per segment, and Q1 (c) adds an index option naming the preset.
   - The bound covers stored frontiers only. Short lists compute theirs at query time
     (`reader.hh:345-350, 1075-1080`) in wdf order.
   - E5's base must include M9. Both attack H2's per-pair cost, and D6 measures a format item
     on the strongest format-free base (`query-gap.md:478-480`); without M9, E5 credits M17
     with what a format-free change gives.
   - The 2⁻⁴⁰ margin needs its derivation: each side's rounding error is under about 10 units
     of 2⁻⁵³, so the margin covers it about 400 times.
   - Fix: M17 is argmax-first, bounded by w · t₁ / (t₁ + c · K(ℓ₁, A)) · (1 + 2⁻⁴⁰), with
     the preconditions, the per-segment record, and M9 in E5's base, in revision 1 and in
     format 13's text.

2. **QG-R2. P17's verbatim copy breaks M17's and M18's invariant.**
   - P17 copies a postings block whose ordinals stay live, rewriting only its base
     (`layers.md:400, 413-420`; `reclamation.md:112-114`). The copied frontier is ordered, or
     its argmax chosen, at the source segment's average, not the merged segment's.
   - `m17_bound.py`, hazard 2: an argmax chosen at a source average within 3× of the merged
     one, used at the merged segment's A with the factor, understates the block's maximum in
     1,406 of 200,000 blocks.
   - Layers' R8 standard for copy paths names `check()`, `verify`, and `parity`
     (`layers.md:27`). None of them sees a misordered frontier (QG-R3).
   - Fix: format 13 states the invariant: every stored frontier's first pair is the argmax at
     its own segment's average under the recorded preset. Under M17, P17 re-sorts each copied
     frontier, a sort of a few pairs. Under M18, P17 must recompute the pair from the block's
     wdfs and norms, most of the decode P17 exists to avoid; T6's merge row prices it.

3. **QG-R3. Nothing checks the property M17 adds.**
   - `check_segment` decodes postings and positions and validates no competitive pair
     (`check.hh:20-41`), so E5's "pass `check`" (`query-gap.md:597`) checks nothing about
     frontiers.
   - E5 orders frontiers only in the merge of the 16-segment index into one segment
     (`query-gap.md:595-599`), where A' = A and c is 1. `verify` and `dump` identity there
     never exercise the snapshot correction; a mutant that drops it passes both, though it
     understates 3.4% of random blocks (QG-R1).
   - Fix: `check()` verifies for every block, group, and term that the stored frontier
     dominates every posting in its range and, in an ordered segment, that the first pair is
     the argmax at the segment's average under the recorded preset. E5 also builds a
     multi-segment index with the experiment writer (segments of different averages, and one
     with `STRATA_DELETE_EVERY`) and runs `verify` and `dump` identity on it. A property test
     over random blocks and averages fails when c is removed.

4. **QG-R4. M18 stays out of format 13 on this evidence** (the ruling the design asks for,
   `query-gap.md:714`).
   - It drops an accepted rule. Phase 2's D3 requires stored pairs to dominate the dropped
     ones (`phase-2-design.md:52-54`), and ruling 2 keeps the rule for any future cap
     (`phase-2-gate.md:92`). An argmax pair dominates nothing; it bounds the others only
     under one preset, with M17's correction.
   - It removes block bounds for every other weighting: Xapiand's default if Q1 picks bm25,
     and any later preset, which fall back to 32-block group frontiers. D1 leaves
     bm25_xapian's rows out (`query-gap.md:452-457`), so no row in this design sees the loss.
   - Its bytes rest on an unmeasured length. Three pairs a block give the design's 17 MB
     (`query-gap.md:350`); Phase 2's design assumed one or two (`phase-2.md:132-135`), which
     give 0 to 8.7 MB, 4.4 MB at 1.5.
   - P17 must recompute its pair (QG-R2).
   - Ruling: M18 enters F13-R8's selection only with all five: E1's block-frontier histogram;
     its bytes against the alternatives that keep whole frontiers (argmax-first with the rest
     delta-coded, QG-R1, and F11's E5, pairs that differ from the group's frontier, F13:294); a
     `SCHEME=bm25_xapian` A/B of the group-only fallback per row; P17's cost; and D3's rule
     amended by name in this design. Otherwise it is out.

5. **QG-R5. M23's check is not memory-safe as written.**
   - Gap − 1 coding makes each step at least 1 only in exact arithmetic. `decode_delta` sums
     in 32-bit lanes (`bitpack.hh:539-551` at `abf8972`). A crafted block whose gaps sum to
     (last − base) + 2³² ends at the entry's last, passes "only their sum against the entry"
     (`query-gap.md:355`), and wraps in the middle.
   - Today `check_increasing` rejects that block at load (`reader.hh:920, 1017-1024`). Without
     it, `fill` sets `bits[(d − base) >> 6]` for an out-of-order d (`reader.hh:715-718`) in a
     64-word window (`search.hh:1332-1337`): a write past the array. `count_below`'s search
     (`reader.hh:1000-1013`) and every advance assume order too.
   - Fix: the check is a 64-bit sum of the decoded values, the first block's base taken as
     −1, equal to last − base − 128, so no partial sum can wrap; or a width bound under which
     wrapping cannot happen. E6 times that check against `check_increasing`, and
     `fuzz_segment`'s corpus gains a wrapping block.

6. **QG-R6. The format items' bytes are not budgeted, and finer fixed partitions do not fit
   without savings no one has measured.**
   - The postings row must lose 11.9 MB (845.6 against 833.7 MB, `phase-3-ratchets.md:204`).
     Per-block costs multiply by 4.37 million full blocks (`phase-2.md:132`; df, and so the
     count, does not change with reordering):
     - A1's +4 bytes is +17.5 MB.
     - M18 saves 4.4 to 17.5 MB at 1.5 to 3 pairs a block.
     - Fixed partitions of 64 postings add 4.37 million partitions, at least 8.7 MB at one
       bound byte and one last byte each; partitions of 32 add at least 26 MB.
     - M20's bytes need the count of terms with skip blocks, at most 9 pairs each
       (F13:339-341). M17 adds nothing per block. M23's saving is unmeasured.
   - So a byte per fixed finer partition adds 8.7 to 26 MB to a row that must lose 11.9 MB; it
     fits only beside savings of that size elsewhere, which nothing measures yet. Variable
     partitions averaging 128 postings or more, or finer partitions on long lists only, fit
     without them.
   - M16's candidates also need F13-R8 item 2's wide form for terms past 32-bit offsets.
   - Fix: a budget table in the design (per item: bytes per unit, the unit count and its
     source, the total) before E3 simulates variants; E3 skips any variant that cannot fit
     with A2 and without M18.

7. **QG-R7. E3's simulator is a second traversal, and its projections are uncalibrated.**
   - D5 keeps or drops M19, M20, and E10 on E3's projections (`query-gap.md:580-582`). E3 is
     "an exact block-max traversal over the decoded postings" (`:572-573`) and names no
     algorithm. strata's are windowed MaxScore with per-window splits (`search.hh:1342-1433`),
     the conjunction's lead-block windows (`:542-591`), and the phrase's lead bound
     (`:1104-1130`). Counts from another algorithm do not transfer, and a second traversal is
     a second implementation (calibration, first row).
   - Its time projection goes through E1's unit costs with no stated error, and a 3%
     threshold decides a format item that freezes once.
   - Fix: E3 is a mode of the real scorers in the counters build, with a substitutable bound
     source (exact partition maxima from the decoded postings at each candidate granularity;
     seeds through `top_k`'s floor). It is calibrated before it decides anything: it projects
     M1, M2, and the oracle seed, and E2's and E4's A/Bs measure them; D5 then compares the
     projection's interval, not its point, with 3%.

### Correctness gates and the keep rule

8. **QG-R8. Oracle parity is missing, and `dump` identity covers one index.**
   - The design never mentions `parity` (0 occurrences). PLAN requires `verify` and oracle
     parity for every Track P item (PLAN.md:376-378) and for the stretch (PLAN.md:136-137);
     TESTING.md lists parity as its own layer (TESTING.md:14).
   - `dump` covers the 962 gate queries on the 5M index. `parity` covers the oracle's 419
     Xapian queries over 10,000 documents at 1, 4, and 16 segments
     (`phase-3-ratchets.md:280-284`), with operators the gate queries lack. M13, M17, M18,
     and M6 change what parity checks.
   - T2 compares `dump` on the reordered index only (`query-gap.md:657`), while M3 changes
     docid-ordered segments. The docid-ordered and reordered indexes already print identical
     dumps (`phase-3-ratchets.md:211-214`), so one reference file serves both and the
     16-segment index.
   - Fix: T2 adds `tools/parity.cc` at 1, 4, and 16 segments, counted and pruned, after every
     kept item; `dump` identity on every index against one reference made at E0's commit, and
     the `STRATA_DELETE_EVERY` index against its own.

9. **QG-R9. D14 cannot be evaluated per class, and it can be gamed.**
   1. `ab.py` scores per command and query set; the 962 gate queries are one row
      (`ab.py:37-66`). D14's "a row it targets" (`query-gap.md:504-505`) has no row, and a 10%
      gain on the 301 unions reads about 3% over the set.
   2. "At least one row" among classes, commands, terciles, and two indexes, chosen after the
      run, invites a chance win.
   3. "No row's upper bound exceeds 1.03" holds per item. Ten kept items may each cost 2.9%
      on rows they do not target. T6's 1.03 on writer time for M17 to M23 compounds the same
      way.
   4. The ten-round rerun runs only after a failure (`layers.md:733-740`, as cited): a second
      chance to pass and none to fail.
   5. "Median" names a statistic `ab.py` does not compute; it reports the geometric mean of
      B/A over queries, with a bootstrap over queries.
   6. Weighting and bound items (M6, M9, M13, M17 to M19) change bm25_xapian, which no row
      measures.
   - Fix: `ab.py` groups by class from `queries.txt`'s tags, as `gate.py` does
     (`gate.py:121-126`). Each item names its target rows in its `docs/perf/` entry before
     its A/B, and the win is on the reordered merged index. A cumulative A/B of the queue's tip
     against its base commit, every row's upper bound at most 1.03, runs at every third kept
     item and before the freeze; writer time likewise for M17 to M23. A rerun's result is
     final in either direction and also runs for wins whose upper bound lies between 0.97 and
     1.0. Weighting and bound items add `SCHEME=bm25_xapian` rows.

10. **QG-R10. D12 drops macOS.**
    - Every Track P measurement covers Linux, FreeBSD, and macOS, on x86_64 and arm64, and an
      item stays only if it does not lose on any (PLAN.md:439-440). D12 names Linux and
      FreeBSD (`query-gap.md:497-500`). The host itself is macOS x86_64.
    - Fix: kept items repeat their A/B on the host's macOS under a hold, or the owner waives
      it for this track in PLAN. SIMD items (M6, M7, M22) stay provisional until arm64 runs.

### Evidence

11. **QG-R11. The mean-time table cites the wrong baseline in 16 of its 30 cells.**
    - The committed n1 table has no Lucene `-bp` column (`phase-3-ratchets.md:131-172`):
      `gate.py` printed none then (the host copy of that output, `~/.scratch/n1/gate5.md`).
      The design's table takes the smaller of Lucene's and Tantivy's means
      (`query-gap.md:99-106`).
    - `n1means.py`: `-bp` is the reference engine in 20 of 35 rows, 16 of them in the
      design's table, and is faster by mean: intersection_union `TOP_10` 2,084 µs (cited
      3,028), union `TOP_10` 981 (1,236), intersection `COUNT` 585 (847), phrase `TOP_10`
      1,662 (1,803), negated `TOP_10` 599 (719).
    - So intersection_union trails by 1.5 to 2.1 ms a query at top-k, not 1.0 to 1.3
      (`query-gap.md:131`), and "Tantivy is the reference everywhere" for phrases (`:132-133`)
      is wrong at `TOP_10`, where `-bp` is the reference in 4 of 5 trials. The ratios are
      right: `gate.py` includes `-bp` (`gate.py:41, 158-159`).
    - Fix: rebuild the table from the raw files with each row's reference engine. For the
      lead: the gate report gains the `-bp` column, and calibration gains the row.

12. **QG-R12. The stretch's starting point and PISA's position rest on mismatched figures.**
    1. "n2's 1.34 times T2's global factors" (`query-gap.md:144`) applies the global class's
       factor to term, negated, and two-phase-critic rows, which reordering barely moves. Per
       class factors give 1.16 to 1.22 (`stretch.py`), so 0.5 needs 57% to 59% off, not 54%.
    2. The record's 1.34 is raw, over the 35 rows without global (`n2rows.py`: 1.342; 1.384
       floor-subtracted). PLAN's ratios are floor-subtracted, and the design's own table drops
       two-phase-critic and keeps global. T8 must name its rows and its ratio.
    3. "PISA sits at about 0.62×" (`query-gap.md:146-151`) divides a raw ratio (1.43) by a
       floor-subtracted one (2.30). The global class includes intersection_union, whose PISA
       semantics D13 calls unknown: without it PISA sits at about 0.78× and the stretch is 36%
       past PISA's `TOP_10`, not 20% (`pisa.py`). Negated is in the same position: nothing
       records how PISA's harness reads `-b`, and D13 does not name it.
    4. D13 needs no wrapper reading. `gate.py` without `--md` prints count mismatches per
       engine, and PISA answers real counts at `COUNT`: equal counts on intersection_union and
       negated settle their semantics from n2's raw files.
    5. PISA's term `TOP_k` times (2,112 to 3,347 µs against its own 123 µs `COUNT`,
       `quick-n2.md`) show a per-query cost in its harness, so "strata leads PISA on single
       terms" (`query-gap.md:152-153`) says nothing about pruning.

13. **QG-R13. Rows T5 must close are outside the design's scope.**
    1. two-phase-critic misses at four commands: 1.19×, 1.42×, 1.43×, and 1.38×
       (`phase-3-ratchets.md:147, 155, 163, 171`). T5 is every class (`query-gap.md:660`), and
       D1 drops it.
    2. `COUNT` term misses by its interval, 1.00× [0.99–1.41] (`phase-3-ratchets.md:170`). The
       target table uses one median, 0.95 (`query-gap.md:142`); n1's upper bounds sit a median
       3.3% above their medians, 5.2% at the 75th percentile, and 41% at most, with 16 of 35
       rows above 4% (`headroom.py`). The needed median is per row, from E0's intervals.
    3. Negated misses by 1.20× to 1.50× at `TOP_100` and beyond, absent from the summary's
       widest misses (`query-gap.md:30-35`).
    4. The before-merging rows are Phase 3 rows (PLAN.md:324-326), still owed
       (`phase-3-ratchets.md:356`), and in neither D1 nor T5.
    - Fix: D1 and T5 name all four, with what closes each.

14. **QG-R14. The record already ranks the queue: H17 down, H8 up.**
    1. `n1split.py`: in 24 of 35 class and command rows the most expensive tercile's ratio
       exceeds the cheapest's: `TOP_10` global 1.17, 1.41, 1.55; `COUNT` intersection 1.18,
       1.47, 1.87; `TOP_10` phrase 1.15, 1.57, 1.74. The cheapest terciles' reference times
       are 196 to 1,493 µs, so fixed costs of 1 to 5 µs (M8) are at most 4% of them. H17 is not
       a class-level gap (`query-gap.md:41, 298-310`); E8 serves the floor-bound term `COUNT`
       rows.
    2. The term split after merging is in the raw files (`query-gap.md:125-126` says it is not
       recorded): `TOP_10` rare 0.99, medium 1.13, frequent 1.52; `TOP_100` 1.05, 1.26, 1.65;
       `TOP_1000` 1.03, 1.11, 1.42. It points at H1 to H3.
    3. `twopass.py`, per query: strata's own `COUNT` plus `TOP_100` against its one-pass
       `TOP_100_COUNT` is 0.37× on 3-term unions (19,684 to 4,532 µs by mean; Tantivy 10,935)
       and 0.26× on longer ones (76,207 to 11,202 µs; Tantivy 26,058). Two-term unions already
       count through `exact_count` (1.09×), and intersections and negated lose with two passes
       (1.46× to 1.72×). M4's 20% to 40% (`query-gap.md:331`) understates what a plan choice
       already gives, and H8's "extra pass" for two clauses is not where the 6 ms is.
    - Fix: E9 moves first among the format-free items, with the two-pass plan for unions of
      three or more clauses as the baseline M4 must beat (or kept as the planner's choice); H17
      and E8 drop to the term `COUNT` rows. The order for the lead is under "Ordering" below.

15. **QG-R15. E1 decides nothing it can be held to.**
    - H1 to H19 end "confirmed, refuted, or open" (`query-gap.md:558-559`) with no threshold
      for either verdict. A count without a threshold confirms whatever its reader expects.
    - T1's 90% of in-process time "assigned to named components" (`query-gap.md:656`) is met
      by any profile, which assigns every sample to some function. F13-R7 item 2 asks for the
      gap, not strata's time: per class that misses at the freeze, the gap split into parts,
      each assigned to code outside the format, a mechanism, and its projected effect
      (F13R:441-444).
    - Fix: each hypothesis states, before E1 runs, the count or share that confirms it and the
      one that refutes it. T1 attributes the gap per missing class, with the format-dependent
      remainder stated and E6's and E7's microbenchmarks supplying its per-block costs.

16. **QG-R16. The base's ratios do not carry until layers' step-6 A/B.**
    - "Layers changes no format-12 byte ..., so the ratios carry to main"
      (`query-gap.md:699-700`) holds for bytes, not time. Layers' step 2 `COUNT` row on the
      stratified terms reads 1.156× from one round and is open, and step 6's full L15 A/B
      against the Phase 3 commit has not run (`docs/status/layers-refactor.md` at `707066b`).
      `COUNT` term already sits at 1.00× [0.99–1.41].
    - Fix: until layers lands with its step-6 A/B recorded, a projection is E0's ratio times
      layers' A/B for that row times the item's A/B.

17. **QG-R17. E1's allocation counting would duplicate the `allocators` library.**
    - Kronuz/allocators already overrides the global `new` and `delete` for accounting
      (`ALLOCATORS_TRACKED_MEM`; `total_allocated()`, `local_allocated()`, live bytes only).
      E1's "counting `operator new` in the tool" (`query-gap.md:549-550`) is a second override
      (calibration, first row). `kronuz::metrics::Counter` exists for counters.
    - Fix: extend `allocators` additively with per-thread allocation counts and cumulative
      bytes, and build E1's allocation rows on it.

18. **QG-R18. M3's ruling, and its unstated costs.**
    - Out-of-order arrival under D5's rule is exact: a segment through `begin()` breaks ties
      by docid and hands down the score just below the k-th (`search.hh:288-298`), as reordered
      segments do today. Sound.
    - Iterators only move forward (`reader.hh:542-568`; `:882`, "targets only move forward").
      Groups or ranges in bound order need a backward seek or a fresh iterator per range.
    - A docid-ordered segment on that path forfeits the equal-bound skip: at most 0.7% on the
      gate queries, 1.9% on bm25 terms, and 7.4% on bm25_xapian terms (`reordering.md`, D5's
      measurement). E10's A/B pays both.
    - "M3 buys what an exact seed would buy" (`query-gap.md:384`) overstates; the oracle seed
      is its ceiling, as M3's row says (`:330`).

19. **QG-R19. The proposed criteria omit what every gate requires.**
    - The ledger has no done criteria (`docs/status/query-gap.md:6-7`), and T1 to T8 name no
      sanitizer run, no coverage target, no CI check, and no fuzzing.
    - Fix (for the lead's criteria): each kept item's tests pass under ASan and UBSan; its
      added code meets TESTING.md's targets (scorers and collectors at least 95% of lines and
      90% of branches; decoders 98% and 95%); its commit's CI passes all eight jobs
      (calibration, second row), with SIMD paths built on `linux-arm64-gcc` and `macos-arm64`;
      any decoder an item changes is fuzzed through format 13's T2 with F13-R15 item 2's
      section selector.

20. **QG-R20. Smaller items.**
    - M8's trivial bound for short lists loosens the union's bound order and MaxScore split
      (`search.hh:704-715, 721-736, 1364`); measure it, or compute the pairs when a threshold
      first exists, as P6a does (`p6a-conjunction-bulk-topk.md:9-10`).
    - M20 applies only to segments without deletes (F13:336-337); state its reach.
    - M22's run containers and Elias-Fano land in `docset` and `bitpack`; `docset`'s Roaring
      layout has none (`docset.hh:856-857` at `0859e43`).
    - "4.37 million blocks" (`query-gap.md:350`) cites `phase-2.md:132`.
    - "Union: the largest pruned gap" (`query-gap.md:129`) contradicts intersection_union at
      1.93× against union's 1.60×.
    - D13 names union only; the record names union, intersection_union, and global at
      `TOP_100` and `TOP_1000` as inexact (`phase-3-ratchets.md:316-317`).
    - n1's raw files are in the VM and a host scratch copy, n2's and T2's in the VM only. E0's
      report records each raw file's path and SHA-256, so the next review can rescore it.

## Rulings for the architect's items

1. **M17's bound** (`query-gap.md:714`): sound, with QG-R1's tighter form, argmax-first order,
   preconditions, and per-segment record; QG-R2's copy-path rule; QG-R3's checks.
2. **M18's departure from whole frontiers** (`query-gap.md:714`): not in format 13 on this
   evidence (QG-R4).
3. **M13's fixed-point sums** (`query-gap.md:715`; `p6-multisegment.md:286-294`): acceptable as
   strata's scoring contract in principle, and it waits for E1. Conditions:
   - E1 shows the window path's per-clause buffers still cost at least 3% on the reordered
     index; the 4% to 5% was measured before reordering (`p6-multisegment.md:252-254`).
   - The grid is per query, scaled by a bound from the snapshot's statistics alone (the
     clauses' summed weight factors), so it stays the same in every segment; not an absolute
     2⁻⁴⁰. bm25_xapian's smoothed idf goes to zero as df approaches N (`search.hh:91-94`): a
     term in every one of 5M documents scores below 1e-7, where half of 2⁻⁴⁰ is 4.5e-6
     relative, 4,500 times parity's 1e-9.
   - Bounds round up to the grid; the API keeps returning doubles; the overflow argument
     (`p6-multisegment.md:274-275`) is for unboosted clauses, so boosted clause sums get a cap
     that names its mechanism, checked at build.
   - `dump` is re-pinned once, at that commit, with `verify` and `parity` at zero mismatches
     and `engine_query_test`'s segment independence unchanged.
   - It also makes M6's summation order irrelevant, which counts in its favor.
4. **M3's out-of-order collection** (`query-gap.md:716`): sound under D5's rule, with
   QG-R18's costs.

## Owner questions

1. **Q1. The preset that orders frontiers.** Recommended: (c), each index's configured
   preset, defaulting to bm25, recorded per segment, with M17 argmax-first (QG-R1). It costs
   no bytes per block, Xapiand's indexes get the fast path for their own formula, and a query
   under another preset evaluates every pair as today. It needs an index option naming the
   preset and the per-segment field. (d) cannot apply to M17, which has one first position,
   and M18 stays out (QG-R4).
2. **Q2. A stored score bound.** Recommended: the design's conditional (b), expected to close
   as (a). QG-R6 puts a byte per fixed finer partition at 8.7 to 26 MB against a row that must
   lose 11.9 MB, so only a variant that fits, and that E3, calibrated (QG-R7), projects at 3%
   or more, reopens it. If one does:
   - The byte stores g₁, the first pair's t / (t + K) at the segment's average, rounded up;
     never a score.
   - The reader bounds by w / (1 + c · (1/g₁ − 1)), QG-R1's tighter form, which rises with g₁,
     so rounding up stays valid.
   - It skips and never scores, and pairs stay at group and term level.
   - ARCHITECTURE's rule is restated by its reason: bounds valid under any snapshot's
     statistics (ARCHITECTURE.md:151-152).
3. **Q3. P5's bytes in the disk row.** Recommended: (a), as the design says: a speed
   structure outside the disk row buys the phrase rows with uncounted bytes. If P5 lands, the
   phrase rows also state whether Lucene's documented CommonGrams configuration joins the
   reference set, since each baseline runs in its fastest supported configuration
   (BENCHMARKS.md:60-61).

## Ordering

Within the lead's order (the indexing gap first, then E0, then E1 onward under holds):

1. E1's counters and E3 need no strict hold (`query-gap.md:554, 577`). They run at
   `nice -n 19` whenever no strict hold stands, beside the indexing gap; only E1's profiles
   and the timed experiments take strict holds.
2. E0's five-trial night is the Phase 3 gate's query night (the index's "five-trial query rows
   the next night"), which F13-R7 item 4 projects from. It runs once and serves both.
3. Before the freeze: E1, then E3 (as QG-R7 builds it), then E6 and E7, which are short and do
   not depend on E2, then E2, then E5 and E4 on E2's base with M9 in it.
4. Format-free, by the record: E9 first (QG-R14), then E11, E10 if E3 projects it, E12, E14,
   E13, E15, E16; E8 for the term `COUNT` rows; E17 after its conditions.

## Required changes

Revision 1, each answered by a re-check appended here:

- **Before E3 is built:** QG-R6 and QG-R7.
- **Before E5 runs:** QG-R1, QG-R3, and QG-R9's per-class `ab.py`.
- **Before any item enters F13-R8's selection:** QG-R1 to QG-R6, and E3's calibration
  (QG-R7).
- **Before any item is kept:** QG-R8, QG-R9, QG-R10, and the lead's criteria per QG-R19.
- **In the text:** QG-R11 to QG-R18 and QG-R20.

## Conditions

- C1. At format 13's freeze, every format item of this design has its T4 line with bytes in
  F13-R8's single selection; M17's invariant, if M17 is in, is in format 13's text, with
  `check()` verifying it and P17's rule stated.
- C2. Every kept item records, in its `docs/perf/` entry: `verify` on the four indexes,
  `dump` identity against E0's reference, `parity`, the revised D14 with its pre-named rows,
  and the cumulative guard's latest result.
- C3. Phase 4's gate measures T5 over every class, two-phase-critic included, by each row's
  upper bound, and the before-merging rows by the owner's carry decision.
- C4. E0's report records each raw file's path and SHA-256.

## Not run

No build, test, sanitizer, fuzzing, or benchmark (strict hold, `night_gate.sh n1017`, until
01:17); the gate VM not touched; n2's and T2's raw JSON not read; no index opened, so no
frontier histogram; the probes are Python models of strata's expressions, not strata's
binary; no Xapian or Xapiand GPL source read, and Lucene, Tantivy, and PISA only through the
harness's engine descriptions (`bench/phase0/query.json`), the record, and papers.

## Re-check

- Date: 2026-10-17.
- Reviewed: `git log 60b1d03..design/query-gap`, three commits: `d374a6d` (the ledger's Q1 to
  Q3), `c2ca6b4` (revision 1 of `docs/design/query-gap.md` and the ledger), and `cad1ddb` (K1 to
  K12 confirmed, K13 added). The branch was rebased onto main `ff4320b`: revision 0 is
  `d8a660c` and `4fee3e1` (this review's `a489a72` and `8a6503f`), and this review is
  `60b1d03`. Line numbers below are revision 1's at `cad1ddb` unless a file is named.
- Checked against: format 13 revision 1 at `41730a3` (F7, F10, F11, the joint selection), its
  re-check at `bae7fc1` (F13-R21); calibration's six rows at main `7831551`. Every branch read
  matches its origin with no unpushed commits and a clean tree (fifth row): `design/query-gap`
  `cad1ddb`, `phase4/format13` `ac255f8` (its design unchanged since `41730a3`, its review since
  `bae7fc1`), `phase4/layers-refactor` `1b6a148`, main `7831551` (past `ff4320b` in
  `tools/vm/quiet_trials.sh` only). The rulings earlier reviews addressed to this work (Phase 2's
  C7 and C15, P6's R6, P1-R9, F13-R7, F13-R8, F13-R21) are collected in revision 1's dependency
  table (:96-110) and answered (fourth row).
- Checked by: every changed claim read at its citation (the code at `5691ec7`, which `ff4320b`
  and `7831551` keep; `allocators.h:115-116` at `2ebd873`; `metrics.hh:33-42` at `c975765`,
  pulled into strata by `completion` at its pin `5251cf9`, `CMakeLists.txt:7-8, 14`); revision
  1's restated figures against this review's scripts' outputs; one new probe,
  `probes/m23_wrap.py` (gap − 1 values through 32-bit lane prefix sums, as `decode_delta`
  computes them).
  - Not run: no build, test, sanitizer, fuzzing, or benchmark (strict hold strata-trials,
    `night_gate.sh n1017`, 17:24 to 05:24); `poststats` on no segment.

### Verdict

**APPROVE WITH CONDITIONS: revision 1 closes QG-R1 to QG-R20 and carries C1 to C4; the three
checks the lead named hold, A1's floor aside; QG-R21 to QG-R26 are conditions, and none blocks
E0, E1, or E9a.**

### The three checks

1. **A1's cost (QG-R6).**
   - The attribution holds. The review's +17.5 MB was revision 0's own "+4 bytes a block"
     (`query-gap.md:348` at `8a6503f`) over 4.37 million blocks, pricing format 13 revision 0's
     A1: last u32 and offset u32 with the bitset flag in bit 0, 8 bytes (F13:264 at `4266374`).
     Revision 1's A1 is last u32 and offset u16, 6 bytes plus a flags field of its own (F13:591
     at `41730a3`). Its 16 bits hold: offsets are relative to the group's base, and 32 blocks
     span at most 53,376 bytes, 513 for an ordinal block and 1,155 for a wdf block
     (F13:576-584).
   - The floor is 0, not +4.4 MB. A full block's last lies at least 128 past the previous
     block's, so today's last delta takes 2 to 4 LEB128 bytes, not 1 to 3 (:741): 4 where 128
     postings span more than 2²¹ ordinals (terms under about 305 documents at 5M) and in a term's
     first block, whose delta is its last ordinal (`writer.hh:786`). The length field, bytes
     shifted with the bitset flag, takes 1 or 2 (`writer.hh:787`). Today's two fields are 3 to 6
     bytes, so A1 adds 0 to 3 a block, 0 to +13.1 MB. Most blocks belong to dense terms, with a
     2-byte delta and a 2-byte length, so A1 likely adds about 2 a block, about +8.7 MB (an
     estimate).
   - The flags come on top: +0.55 MB at one bit a block, +4.37 MB at a byte (:745-746). A1 with
     its flags is +0.55 to +13.7 MB, or +4.4 to +17.5 MB if E2's or E3's codec codes are kept.
   - Nothing is decided on it: `poststats` projects every width candidate's bytes exactly before
     code (F13:703-713). QG-R21.
2. **M17's bound and its margin (QG-R1).**
   - The bound holds (:604-612): K(ℓ, A') ≥ c · K(ℓ, A), and scaling every K by c keeps the
     pairs' order by K / t.
   - The margin holds (:624-630), with one step it leaves implicit. The writer's pick p₁ may sit
     a factor 1 + δ below the true argmax p* in g at A (δ its 20 units). The scaled form keeps
     that factor: 1 + K₁/t₁ ≤ (1 + δ)(1 + x), x = K*/t*, gives h(p*) / h(p₁) ≤
     1 + δ · c(1 + x) / (1 + c · x) ≤ 1 + δ for c ≤ 1, h being t / (t + c · K). So the 20
     units stand, the total is 41 units, about 2⁻⁴⁷·⁶, and 2⁻⁴⁰ covers it about 200 times.
   - The chain counts are right: a score is about ten correctly rounded operations from the
     stored integers (the average and its inverse, `norm_k`'s four, the score's three;
     `search.hh:57, 79-81, 101-103`), the reader's bound about as many, and w is one computed
     double on both sides, so its own rounding cancels.
3. **M23's 64-bit sum and its 25-bit form (QG-R5).**
   - The sum holds (:698-703): S is exact below 2³⁹, and S + n = last − base with every step at
     least 1 keeps every partial sum in (base, last], so no lane wraps.
   - The 25-bit form holds (:705-711): n ≤ 128 gaps of at most 2²⁵ put the exact final ordinal
     in (base, base + 2³²], one full residue class modulo 2³², and the entry's last, above the
     base and below the document count, is its one member congruent to the lane's final value.
     The bound is tight: `m23_wrap.py` finds no counterexample among 200,000 random and extreme
     25-bit blocks, and builds a 26-bit block whose gaps sum to 2³² + 1,000 from base 0: its
     lane ends on its last, 1,000, its ordinals run out of order, and the 64-bit equality fails.
   - One precision: the reader knows a block fits 25 bits from its header, not from its values:
     the FOR width, the width plus the exceptions' high bits for an exception block (at most 32
     together, `bitpack.hh:521-522` at `abf8972`), or the constant's width. Otherwise the form
     needs the scan it saves. QG-R25.

### Status

- QG-R1: closed. The bound with its margin (:604-630), preconditions (:620-622), argmax-first
  order with ties by wdf (:632-639), the record (:641-648; F13R:989-999), stored frontiers only
  (:650-652), M9 in E5's base (:1025; D6, :823-826).
- QG-R2: closed. The re-selection rule (:654-663), format 13's text (:775), layers' row
  (:1143-1144).
- QG-R3: closed. `check()` (:654-657); E5's two indexes, its extended `check`, `parity`, and
  both mutants (:1026-1037); K5 (ledger:11).
- QG-R4: closed. :665-685; F11's and the selection's rows removed (:776); K4 (ledger:10).
- QG-R5: closed. :687-711; format 13's text (:781); K6 (ledger:12); QG-R25's precision.
- QG-R6: closed. D15 (:886-888); the budget (:723-764); M19's bytes (:527); QG-R21's floor.
- QG-R7: closed. E3 in the real scorers (:969-993); D5's calibrated range (:813-822); K3
  (ledger:9).
- QG-R8: closed. D4 (:803-812); T2 (:1091); K9 (ledger:15); QG-R24 for the base.
- QG-R9: closed. D14 (:866-885); T3 (:1092); K8 and K10 (ledger:14, 16); QG-R22 and QG-R23
  complete it.
- QG-R10: closed. D12 (:853-858); K8, K13.
- QG-R11: closed in the design (:140-159; H6, :364-369; :222-225). For the lead, open on main:
  the gate report's n1 table still has no `-bp` column (`phase-3-ratchets.md:131` at
  `7831551`), and calibration has no row for it (:1130-1132).
- QG-R12: closed. The stretch (:254-260), PISA (:262-273), D13 (:859-865), T8 (:1097);
  QG-R25's count split.
- QG-R13: closed. The rows to close (:275-291), per-row medians (:232-252), D1 (:785-793), T5
  (:1094), H20 (:459-465), Q4 (:1114-1122).
- QG-R14: closed. The record's splits (:161-196), H8 (:381-386), H17 (:438-449), M24 (:518,
  570-577), the queue (:914-919), E9 (:959-968), K7 (ledger:13).
- QG-R15: closed. The verdict rule (:324-333), levels and ceilings (:467-493), T1 (:1090).
- QG-R16: closed. D16 (:889-895).
- QG-R17: closed. D8 (:839-844), E1's tooling (:943-948), the dependency row (:109), checked at
  the pins above.
- QG-R18: closed. :560-566; M3 (:505); E10 (:1053-1057).
- QG-R19: closed. T9 (:1098); K1 to K13 (ledger:6-20), confirmed by the lead.
- QG-R20: closed. M8 (:510), M20 (:528), M22 (:530), the block count's source (:734), the widest
  gap (:222), D13's rows (:859-860), raw files with SHA-256 (:930).
- C1 to C4: carried, by T4 and "What format 13 must record" (:1093, 766-781), D14 item 5 and K8
  to K12 (:883), T5 (:1094), and E0 with K12 (:930).
- Ruling 3: amended by QG-R26.

### New blocking items

None.

### New findings

21. **QG-R21. A1's floor is 0, and its flags add to it** (the first check above). The budget's
    A1 row (:741) and M16's row (:524) read 0 to +13.1 MB, likely about +8.7 MB, with the flags'
    +0.55 or +4.37 MB stated beside them. Text; `poststats` decides.
22. **QG-R22. The per-class `ab.py` drops the strata items may target.**
    - D14 has `ab.py` group "from the query files' tags as `gate.py` does" (:867-868), and
      `gate.py` keeps only tags without a colon (`gate.py:123-125`). `union:num_tokens_3`,
      `union:num_tokens_>3`, and `term:frequent` get no row, while E9a targets the first two
      (:962-964) and D14 allows a term stratum as a target (:866-867).
    - Fix: `ab.py` reports colon tags as rows of their own beside the classes, and its fixture
      test covers one. Before E9a's A/B is read on its strata; its class row, union
      `TOP_100_COUNT`, stands without it.
23. **QG-R23. D14's scheme rows miss the bound, traversal, and counting items.**
    - D14 item 2 runs `SCHEME=bm25_xapian` for M6, M9, M13, M17, and M19 (:874-876), and E2 for
      M9 only (:1019-1020), the short list this review's QG-R9 item 6 gave. M1 and M2 evaluate
      more block bounds per range; under bm25_xapian the pairs bound more loosely (length codes
      against exact lengths) and each costs more (`search.hh:105-111`), so a win under bm25 can
      lose there. M3 and M5 change pruning, and M4 and M24 counting, under both schemes.
    - Fix: item 2's scheme rows cover M1 to M6, M9, M13, M17, M19, and M24, and E2 runs them for
      M1 and M2. Before E2's A/B.
24. **QG-R24. E0's `dump` references and the layers base.**
    - D4 holds every item's `dump` to references made at E0's commit (:803-808), on main; the
      items run on the layers tip (D16, :889-895). Layers is checked by `verify`, `parity`,
      equal index bytes, and its digest (`layers.md:27`; L15), none of which compares query
      results byte for byte with main's. If the base prints other bytes, every item fails D4
      there, or a re-made reference absorbs an unexplained difference.
    - Fix: before E9a's A/B, the base's `dump` on the four indexes is compared with E0's
      references once, both SHA-256s recorded; a difference goes to layers' reviewer, and the
      references are re-made on the base only once it is explained.
25. **QG-R25. Smaller items.**
    - M23's 25-bit form reads the width from the block's header (the third check above), in
      format 13's text (:781).
    - E0's PISA count split (D13, :861-864) covers intersection and union too. T8 treats them as
      settled (:1097) and the PISA paragraph calls their semantics known (:265-266), which rests
      on the harness's query syntax, not on counts; the same `gate.py` output settles all four.
    - K13 (ledger:19) repeats K8's platforms and K11's T9, for the lead to merge.
26. **QG-R26. Ruling 3's cap on boosted sums is withdrawn.**
    - Ruling 3 asked for "a cap that names its mechanism" on boosted clause sums, and E17 carries
      it (:1077-1078). Every clause scores below its weight factor (bm25: idf · t / (t + K) <
      idf; bm25_xapian likewise, with K ≥ 0.75), so a query's sum stays below its summed weight
      factors, the snapshot-level bound ruling 3 already scales the grid by. That scale rules out
      overflow by construction, and a cap would be a limit Lucene does not have (`limits.md:1-8`;
      calibration, sixth row, applied to this review).
    - Fix: E17 drops the cap and scales the grid by the query's summed weight factors from the
      snapshot's statistics, with w ≥ 0 as M17 requires.

### Conditions

- C1 to C4: carried, as Status states.
- C5. Before E9a's A/B: QG-R22's `ab.py` with its fixture test; M24 and E9a's tests (:960-964)
  under code review; QG-R24's base check.
- C6. Before E2's A/B: QG-R23's scheme rows.
- C7. QG-R21, QG-R25, and QG-R26 fold into the design before its format-13 tables reach format
  13's candidate report, citing their IDs; they need no re-check unless the lead asks.

### E0, E1, and E9a once the host frees

- **E0: yes.** It is the lead's, the Phase 3 gate's query night (:1130-1132), and its record is
  complete (:922-930) with QG-R25's four-class count split.
- **E1: yes, behind K1's code review** of its tooling: the counters build, the `allocators`
  change in its own repository with its CI, the counter columns, and `poststats`' histograms.
  Its counts need only a light hold at `nice -n 19` (:951-953); its profiles take a strict
  hold.
- **E9a: yes, after C5.** Its keep decision can stand on union `TOP_100_COUNT`, where the record
  projects about 0.70× (:518).
- None starts under the current strict hold, which runs until 05:24.

### Not run

No build, test, sanitizer, fuzzing, or benchmark (the strict hold); the gate VM not touched;
`poststats` on no segment, so A1's and every byte figure stay estimates; `m23_wrap.py` is a
Python model of `decode_delta`'s lane arithmetic, not `bitpack`'s binary; no Xapian or Xapiand
GPL source read.

## Second re-check

- Date: 2026-10-18.
- Reviewed: `git log 643fdb9..3cba768` on `design/query-gap`, eight commits: `32d598a` (`ab.py`
  per class and per stratum, QG-R22), `9e4cd7f` (revision 2, QG-R21 to QG-R26 and C5 to C7),
  `38df698` (targeted runs: D17, Q5, Q6), `b1261f2` and `c12fd50` (E0's length, D16's references),
  and the ledger's `d85d143`, `37e279e`, and `3cba768` (the lead's rulings on Q5, Q6, and E0's
  engines). The design is unchanged since `c12fd50`. Line numbers are the design's at `3cba768`
  unless a file is named.
- Checked against: PLAN at main `199770f`: the baselines and the reference with PISA
  (PLAN.md:99-108, from `4692934` at 21:56 on 2026-10-17, after `3cba768` at 21:33), targeted runs
  (:109-122), carried rows (:126-136), and the many-indexes workload (:183-192); BENCHMARKS.md:58
  and :191 there; PISA's record (`phase-3-ratchets.md:296-321`); n2's `gate.py` output
  (`~/.scratch/quick-n2.md`); calibration's ten rows at main `088a64f`, the four new ones applied
  where they bear (the seventh: a mechanism a fix relies on is found or named as owed; the tenth:
  every table cell quoted by row and column).
- Checked by: every changed claim read at its citation; `gate.py:30, 41, 100-176` and
  `.scratch/sizing.py:15` (read, not rerun); `ab_test.py:53-59` and `CMakeLists.txt:159`;
  `night_p1.sh:31`; a `grep` of n1's raw `q1-queries.json`, which holds no `pisa-0.8.2` result;
  CI through `gh` (the branch's last run is `32d598a`'s, cancelled; main's `584cab1`,
  `ci.yml:17-18`). One probe, `qg2/sizes.py` in this reviewer's scratchpad
  (`/private/tmp/claude-501/-Users-kronuz-code-Kronuz-Xapiand/b5982d78-0a0d-4769-bdb3-75c9e8136cd9/scratchpad/qg2/sizes.py`):
  n2's per-engine mean times by class and command, times S's and S′'s class counts, five runs, and
  D17's 5 s warm-ups. It gives 116 s and 161 s a trial for strata, Lucene `-bp`, and Tantivy on S
  and S′, against D17's 120 and 175 (:1027, :1031), so its other sizes below are estimates of the
  same kind.
  - Not run: no build, test, sanitizer, fuzzing, or benchmark. `quiet check` exited 1 (scale-vm,
    strict, on the gate VM, until 11:17); `sizes.py`'s milliseconds of arithmetic at `nice -n 19`
    are the only process this review ran under it. `sizing.py`, `budget.py`, and `runs.py` not
    rerun, so D17's half-widths are read, not reproduced. n2's raw JSON (gate VM only) not read;
    the gate VM not touched. PISA's harness source is not on this host, so its algorithms, codec,
    and seed are the record's (`phase-3-ratchets.md:309-321`).

### Verdict

**APPROVE WITH CONDITIONS: revision 2 closes QG-R21, QG-R23 to QG-R26, and C7, closes QG-R22 but
for its CI, and carries C5 and C6; D17 follows PLAN's targeted runs except for its confirmation
sample's calibration (QG-R29); the Q5 and Q6 rulings are sound; E1's tooling starts now, and E0
starts once QG-R27 puts PISA in its calibration and in the reference set.**

What holds:
- Every QG-R21 to QG-R26 fold in the text, at the lines under Status.
- D17's shape (:972-1034): small stratified samples on the cached 5M indexes, one calibration,
  strata against each row's fastest with a drift band, a minutes-long confirmation as the gate row,
  and light holds for single-threaded runs.
- `ab.py`'s stratum rows and their test (`ab_test.py:53-59`; ctest `ab_scorer`,
  `CMakeLists.txt:159`).

What blocks: QG-R27, for E0 only.

### Status

- QG-R21: closed. A1 (:778), A2 and A4 (:779), the flags (:782-783) and the totals with them
  (:795-798), M16's bytes (:554). The arithmetic holds: 3, 2, and −1 bytes over 4.37 million blocks
  are +13.1, +8.7, and −4.4 MB, and the flags add 0.55 or 4.37.
- QG-R22: partially closed: the code and the text are in, CI is owed. `ab.py` per tag
  (`32d598a`), strata rows tested (`ab_test.py:53-59`), D14 (:918-926), E9a's targets
  (:1155-1158). CI: seven of eight jobs; ASan+UBSan was cancelled twice at the old limit before
  reaching `ab_scorer` (ledger:42); QG-R32.
- QG-R23: closed. D14 item 2 (:936-945), E2 (:1218-1219), E9a (:1152-1153).
- QG-R24: closed. D4 (:846-858), D16 (:960-972), the base check (:1128-1146), T2 (:1302).
- QG-R25: closed. M23 (:739-740) and its record row (:823); D13 (:907-917) with the per-class split
  as first step 2 (:1440-1443); T8 (:1308); K13 for the lead (:1380-1382), ruled in QG-R32.
- QG-R26: closed. M13 (:545), E17 (:1277-1290), a negative boost keeping today's sums (:1289-1290).
- C1 to C4: carried.
- C5: folded (E9a, :1148-1151; first steps 1 and 5); carried: M24 and E9a's tests are unbuilt, and
  `ab.py`'s ASan job is owed.
- C6: folded (D14 item 2; E2, :1218-1219); carried to E2's A/B.
- C7: closed: QG-R21, QG-R25, and QG-R26 are in the text, as above.

### The lead's checks

**D17 against PLAN's targeted runs** (PLAN.md:109-122):
1. Iterate small: consistent. PLAN's 100,000 documents are for indexing (:109-110), and D17
   holds the writer-side items to them (D14 item 4; T6, :1306). Queries iterate "on a small sampled
   set" (:110): S on the 5M indexes (:1033-1034).
2. The 20,000-document dry run: consistent, with PLAN's pass rule (:1065-1072; PLAN.md:110-112).
3. Calibrate every engine once: consistent in revision 2's E0 (:1079-1083), not in the ruling that
   leaves PISA out (QG-R27).
4. Then strata against the fastest, with the drift anchor: consistent (item 4, :997-1005), each row
   with its own anchor.
5. One larger confirmation as the gate row: consistent in shape (item 5, :1006-1009; T5): S′, five
   trials, about 14 minutes, 17 with PISA (`sizes.py`). Its anchors come from S, another sample
   (QG-R29).
6. Q5's 5M indexes are consistent: for queries PLAN shrinks the sample, not the corpus, and sizes
   the confirmation "for minutes, not hours" (:120), which a 5M run of S′ is.

**The fastest anchor per row with PISA.** From n2's floor-subtracted geometric means
(`quick-n2.md`, columns "Floor-subtracted", against the fastest of Lucene, Lucene `-bp`, and
Tantivy, and "Against PISA"; three trials of the 221-query quick sample, so E0 decides):

| Command | PISA anchors, once the count split settles the class | The fastest of the three anchors |
|---|---|---|
| `TOP_10` | union (2.06× PISA against 1.70×, :12), intersection (1.46 against 1.26, :6), intersection_union (4.79 against 2.03, :7), negated (1.57 against 0.86, :8), global (2.30 against 1.46, :5) | term (PISA 0.39×, :10), phrase, two-phase-critic |
| `TOP_100`, `TOP_1000` | intersection (1.77, 1.79; :14, :22) and negated (2.68, 3.56; :16, :24), if the lead admits ranked AND there (QG-R27) | union, intersection_union, global (PISA not exact), term, phrase, two-phase-critic |
| `TOP_100_COUNT` | none: PISA ranks nothing | every class |
| `COUNT` | negated (2.19 against 1.59, :40); term ties at the floor (0.95 against 0.94, :42) | union (PISA 0.40×, :44), intersection_union (0.09×, :39), intersection (1.52 against 1.73, :38), global (0.48×, :37), phrase, two-phase-critic |

"PISA is the fastest at top-k, but slow at `COUNT`" holds at `TOP_10` on every class it answers but
term. At `COUNT` it is slow on union and intersection_union and behind Lucene `-bp` on
intersection, but fastest on negated, whose PISA times are flat at 282 to 306 µs over all five
commands (:8, :16, :24, :32, :40), so the count split must settle that class before PISA anchors it.

**Q5 and Q6.** Both rulings are sound.
- Q5: the 5M indexes are where the gate rows live, and the mechanisms act on long lists (:1336-1350).
  The ruling needs a home on main, the Xapian floor needs its index, and the many-indexes workload
  needs a report row (QG-R32).
- Q6 (b): a row passes for its class, not for its draw, and the stricter bound is the honest one.
  Its bootstrap must follow the draw (QG-R30).

**Before E1's tooling and E0.** E1's tooling needs nothing changed; QG-R31's `poststats` log-gap
cost joins it before K1's code review. E0 needs QG-R27, in the text and in first steps 2 and 3,
which come before it. QG-R29 and QG-R30 shape first step 3's `baseline.py` and interval but not
E0's run, so they are conditions on that tooling; QG-R28 comes before T1's attribution and the
first gate row.

**PISA's place.** QG-R27 (a baseline on its comparable rows), QG-R28 (which gate it binds), and
QG-R31 (what H1-H20 miss).

### New blocking items

- QG-R27, before E0 and before first steps 2 and 3.

### New findings

27. **QG-R27. PISA is a baseline on main; revision 2 and the E0 ruling keep it a report row.**
    - PLAN puts PISA in every gate's baseline set as "the reference for query classes"
      (PLAN.md:99-105), the reference being the fastest engine per query class (:106-108).
    - Revision 2 keeps D13's "PISA is a report row, at `TOP_10`" (:907), and runs its later rows
      and its confirmation on strata, Lucene `-bp`, and Tantivy (:1027, :1031), with the warm-up
      check on those three (:1074-1078). Its tooling does the same: `gate.py`'s reference set
      `FAST` has no PISA (`gate.py:41`), which it reports "beside Xapian" (`gate.py:30`), and D17's
      half-widths come from n1, which has no PISA result (`sizing.py:15`; n1's `q1-queries.json`).
    - The lead's ruling names "Lucene and Tantivy only" for E0 (ledger:59-60). Its 23 minutes is
      revision 2's figure for every engine but Xapian (:1393), PISA included; by `sizes.py`, PISA
      is about 3 of them.
    - Why it matters: PISA is the fastest engine on most `TOP_10` rows (the table above). An E0
      without it names the wrong anchor there, every later run times the wrong engine, and
      "beating the fastest beats the rest" (PLAN.md:116) fails on the rows furthest from 1.0.
      Adding PISA after E0 is a new calibration by D17's own rule (:993-996).
    - Fix, before E0 and before first steps 2 and 3 are written:
      1. E0 calibrates strata, Lucene, Lucene `-bp`, Tantivy, and PISA (Xapian by the ruling), and
         the warm-up check covers every engine that anchors a row; the ruling and E0's text
         (:1079-1083) say so.
      2. D13 becomes PISA's reference rule, and `gate.py`'s and `baseline.py`'s reference sets
         apply it. PISA is a reference engine on a row where it does the row's work, once E0's
         count split settles the class: `TOP_10` of intersection, union, intersection_union,
         negated, and global, and `COUNT`. Never at `TOP_100_COUNT`, where it "counts and ranks
         nothing" (`phase-3-ratchets.md:318`); at union, intersection_union, and global `TOP_100`
         and `TOP_1000`, where its 10th-best seed drops hits (:314-317); or on phrase and
         two-phase-critic (unsupported, :319). Intersection and negated at `TOP_100` and
         `TOP_1000` run ranked AND without the seed (:318), exact up to its quantized scores
         (:313); the record calls only `TOP_10` fair (:321), so admitting them is the lead's call,
         recorded in D13. PISA's term `TOP_k` rows, slowed by a per-query harness cost (:917), back
         no claim that strata beats PISA there.
      3. E0 reports each PISA-anchored row's sampling half-width on S and S′ from its own
         per-query spreads; a row whose S′ half-width exceeds D17's worst, 7.0% (:990-992), is
         named, and Q6's bound carries it.
      4. For the lead: BENCHMARKS.md:58 on main still calls PISA "not a ratchet target", against
         PLAN.
28. **QG-R28. Which gate PISA binds is not ruled.**
    - T5 holds the carried rows to "1.0× its fastest baseline" at Phase 4's gate (:1305), and T7 to
      0.7× at Phase 6's (:1307). PLAN closes a carried row at "its original target"
      (PLAN.md:133), set at Phase 3 against Lucene, Lucene `-bp`, and Tantivy, while each gate
      "fixes its baseline set" (:99), now with PISA.
    - The readings differ by up to a factor of 2.4: against PISA, n2 puts intersection_union
      `TOP_10` at 4.79× and union `TOP_10` at 2.06× (`quick-n2.md:7, :12`, "Against PISA");
      against the three, at 2.03× and 1.70× ("Floor-subtracted"). "What the targets ask"
      (:258-304) and T1 are sized on the second.
    - Fix: a ruling, recorded as Q7. Recommended: T5 closes the carried rows against Phase 3's set,
      each row's ratio against PISA reported beside it; T7, Phase 6's 0.7×, and every later gate
      take PISA on its comparable rows (QG-R27 item 2). "What the targets ask" gains PISA's column
      from E0, and T1 attributes the `TOP_10` rows against PISA as well, by QG-R31's parts, since
      T7 binds there.
29. **QG-R29. S′ is a new sample, and no run calibrates it.**
    - D17 item 3: "A new engine version, corpus, index, or sample means a new calibration"
      (:993-996). Item 5 times strata on S′ against each row's fastest as S names it (:1006-1009),
      and T5 requires "its anchor inside its calibrated band" (:1305), a band E0 measures on S
      only.
    - S's naming need not hold on S′. By n2's means (`quick-n2.md`, the µs columns), Lucene `-bp`
      and Tantivy are within 1% of each other on term `TOP_10` (203 and 202 µs, :10), phrase
      `TOP_10` (3,290 and 3,270, :9), and intersection `TOP_1000` (1,699 and 1,709, :22); Lucene and
      Lucene `-bp` on intersection_union `COUNT` (179 and 180, :39); four engines within 7% on term
      `COUNT` (PISA 123 to Lucene 131, :42); S's median half-width is 2.9% (:990).
    - Fix: the first confirmation times every baseline on S′, five trials, making S′'s calibration
      and its gate rows one run, about 22 to 24 minutes under a light hold (`sizes.py`, an
      estimate). Later confirmations time strata against S′'s fastest per row and every engine
      within that row's half-width of it, with the drift test against that run; runs on S do the
      same with S's half-widths. `baseline.py` keys each calibration by its sample's SHA-256. D17
      item 3's "index" reads "a baseline's index": strata's own index, rebuilt untimed when format
      13 lands, recalibrates nothing.
30. **QG-R30. Q6's bootstrap must follow the draw.**
    - The ruling takes Q6 (b) (ledger:58; :1352-1364): bootstrap the trials and, within strata,
      the sample's queries. D17 takes intersection_union, negated, and two-phase-critic whole in
      both samples (:980-984), so their rows carry no draw error, and its half-width has the
      finite-population factor (N_h − n_h)/N_h (:1020).
    - A bootstrap that resamples a whole class's queries with replacement widens its bound for an
      error it does not have, on two of the rows furthest from 1.0 (intersection_union `TOP_10` at
      1.93×, :268; negated `TOP_100_COUNT` at 1.50×, :273). On a stratum drawn at 100 of 300, it
      overstates the draw's half-width by about 1/√(1 − 1/3) ≈ 1.22.
    - Fix: `gate.py`'s interval (first step 3, :1453-1454, whose "If Q6 takes (b)" is settled)
      resamples a stratum's queries only where the sample does not take it whole, with the
      finite-population correction (for example Rao and Wu's rescaled bootstrap), and its fixture
      test has a class taken whole, whose interval equals the trials' alone. Q5 and Q6 move to
      "Answered" with the rulings (:1334, :1352).
31. **QG-R31. H1-H20 miss two of PISA's sources and an exact counterpart to a third.**
    - Covered: block-max data per 40 postings (H10), the precomputed seed (H1, with M20 and E4),
      quantized bounds (H2, with D7 keeping them to bounds, since quantized scores change results,
      :881-884), ranked AND (H4 to H6), and the traversal (H7, M1, M3).
    - The algorithm family needs no hypothesis of its own. Lucene `-bp`, the fastest of the three on
      intersection and union `TOP_10` by n2's means, runs windowed block-max MaxScore and AND as
      strata does, and sits within 16% and 21% of PISA there (strata at 1.46× and 2.06× PISA,
      1.26× and 1.70× the three, `quick-n2.md:6, :12`). Most of strata's `TOP_10` gap to PISA is
      its gap to its own family. intersection_union is the exception (4.79× against 2.03×, :7),
      and the count split settles it first: PISA answers it in 697 µs, below its own union's 944
      (:7, :12), as if the optional clauses were dropped.
    - Missing:
      1. **H21, reordering quality.** The verdict rule runs on strata's reordered index (:354), and
         every ratio against PISA carries two bisections (`phase-3-ratchets.md:311`), which no
         hypothesis compares. Counter: the log-gap cost per posting, bisection's own objective,
         of both orders over the same postings, from `poststats`; PISA's order from its
         `reorder-docids` run, which the harness's PISA build must keep (owed, the lead's
         harness). Ceiling: when strata's cost is 3% or more above PISA's, an in-process A/B of a
         strata index built in PISA's order (a docid-ordered merge of the documents fed in that
         order) against the reordered one.
      2. **H22, decode per posting.** H5 counts the decodes an advance pays, and H15, H16, and H19
         skip entries, bit-filling, and branches; none prices a decoded posting against
         SIMD-BP128, PISA's codec (:311). Counter: E1's blocks decoded by kind, times E6's
         microbenchmark of `bitpack`'s FOR and exception decode with its prefix sum against a
         SIMD-BP128 decode of the same blocks (PISA's code may be read and ported under its notice,
         into `bitpack`, D8). Ceiling: decode's profile share times one minus their ratio. It is a
         freeze-set item: a docid codec is a format change, and M16 to M23 hold none beyond M22's
         dense containers, so H22's verdict comes before format 13 freezes, or a codec change waits
         for the next format.
      3. **An exact counterpart to precomputed impacts.** PISA's index holds an integer score per
         posting (:313); H3 prices strata's division per posting, and no mechanism removes it
         exactly. A per-term table of scores over (wdf up to a small bound, length code), each cell
         computed by `score_of` itself (`search.hh:342-343` at `5691ec7`, H3's citation), is
         bit-identical by construction; it joins M8 or M6 as H3's mechanism, its build per query
         costed in H17's terms.
    - Fix: H21 and H22 with their counters, levels, and ceilings under "Levels and ceilings"; item
      3 as H3's mechanism; `poststats`' log-gap cost in E1's tooling before K1's code review; H22
      in E6, before the freeze.
32. **QG-R32. Smaller items.**
    - Q5's reading lives in the ledger only. PLAN.md:121's "Full-corpus runs never gate a phase"
      reads, literally, against T5's 5M rows, so the lead records the reading (a rule about long
      runs) on main, in PLAN or in BENCHMARKS' tiers (BENCHMARKS.md:191), where a gate review looks.
    - The ruling's Xapian floor on S′ (ledger:59-60) needs Xapian's 5M index, which made room for
      PISA's build (`night_p1.sh:31`): rebuilt untimed before the first confirmation, or named as
      owed.
    - The many-indexes workload (PLAN.md:183-192) puts H17's fixed costs, which "can still move the
      rows nearest the floor" (:477-479), where no row measures them. A report row, S on the
      100,000-document indexes the indexing iteration builds anyway, against the same anchors,
      shows whether M8's items pay there; no gate rests on it.
    - K13 (ledger:19): K8 and K11 carry both its clauses (ledger:14, :17), so it can go; the lead's
      edit.
    - CI: main's `584cab1` gives ASan+UBSan 120 minutes (`ci.yml:17-18`), and the branch does not
      contain main `199770f`. Rebased and green on all eight jobs, `ab_scorer` among them, before
      `ab.py`'s code review closes C5's first item.
    - The "2.48× PISA" quoted for `TOP_10` global is not in n2's output, which reads 2.30×
      floor-subtracted (`quick-n2.md:5`); it enters the design with its raw file or not at all.

### Conditions

- C1 to C4, C5, and C6: carried, as Status states. C7: closed.
- C8. Before E0: QG-R27 in the design, the ledger's ruling, and first steps 2 and 3.
- C9. Before T1's attribution and the first gate row: QG-R28's ruling, as Q7.
- C10. Before first step 3's code review closes: QG-R29's per-sample calibrations and co-anchors,
  and QG-R30's interval with its fixture test.
- C11. Before K1's code review: QG-R31's hypotheses and `poststats`' log-gap cost; before format
  13 freezes: H22's verdict through E6.
- C12. Before `ab.py`'s code review closes: QG-R32's rebase and CI; the rest of QG-R32 folds into
  the next revision.

### E0, E1, and E9a

- **E1: yes, now,** behind K1's code review: its tooling is written now and built, like its counts
  run, once `quiet check` exits 0 or 2 (scale-vm's strict hold runs until 11:17).
- **E0: after C8,** about 23 minutes under a light hold with PISA in it.
- **E9a: after C5,** unchanged; its anchor at union `TOP_100_COUNT` stays Tantivy, since PISA ranks
  nothing there.

### Not run

No build, test, sanitizer, fuzzing, or benchmark (scale-vm's strict hold); `sizing.py`,
`budget.py`, and `runs.py` not rerun; n2's raw JSON and the gate VM not read or touched; PISA's
harness source not read, its methods taken from the record; `sizes.py` is arithmetic over n2's
printed means, not a timing; no Xapian or Xapiand GPL source read.
