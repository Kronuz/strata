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
