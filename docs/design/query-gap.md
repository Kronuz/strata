# Query gap: closing strata's query-speed gap

Revision 2. It folds in the re-check of revision 1 (`docs/reviews/query-gap-design.md` at
`643fdb9`: APPROVE WITH CONDITIONS, QG-R21 to QG-R26, conditions C5 to C7, no blocking items) on
top of revision 1 (`c2ca6b4`), which folded in the preliminary review (`60b1d03`: QG-R1 to QG-R20,
C1 to C4) and the owner's decisions on Q1 to Q3 (the ledger at `d374a6d`). Revision 0 is
`d8a660c`, amended at `4fee3e1`. The branch was rebased onto main `ff4320b` on 2026-10-17, so the
preliminary review's `a489a72` and `8a6503f` are those two commits, and `dec5f4d` is `60b1d03`.

This is Track P's plan for the carried query rows (PLAN, "Carried rows": Phase 4's gate does not
pass while one still misses 1.0×), for Phase 6's 0.7×, and for the owner's 0.5× stretch. Per query
class it states where strata loses and by how much, the hypotheses for why with the counts that
confirm or refute each, the candidate mechanisms with estimated effects and costs, what format 13
must record before it freezes, and an ordered experiment queue for a performance engineer.
Decisions are D1–D17, hypotheses H1–H20, mechanisms M1–M24, experiments E0–E17 with the base
check before E9a, gate rows T1–T9, owner questions Q1–Q6 (Q1 to Q3 answered), and done criteria
K1–K13 (the lead's, in the ledger). D17 applies PLAN's targeted runs (main `21336b4`): every timed
run is minutes on a small sample, the baselines are calibrated once (E0), and every later run
times strata against each row's fastest engine only.

Nothing here was measured for this design. Every number is a recorded one, cited with its file, or
an estimate, labeled as one. The arithmetic revision 1 added is of two kinds, each cited where it
appears: the review's scripts rerun over recorded results (n1's raw files, whose host copies in
`~/.scratch/n1` rescore to the committed table digit for digit, and n2's `gate.py` output in
`~/.scratch/quick-n2.md`), and ratios of the committed table's figures. Revision 2 adds byte
arithmetic over the record's block count (QG-R21) and one piece of code, the per-class `ab.py`
(`32d598a`), whose test runs in CI. The host was under a strict hold while both revisions were
written (the gate VM's `night_gate.sh n1017`), so nothing was built, tested, or timed on it.

Conventions. Code is cited at main `ff4320b`, whose code is `5691ec7`'s, the commit revision 0 and
the review cite (`git diff 5691ec7 ff4320b` touches only `docs/`). The experiments build on the
layers tip (`origin/phase4/layers-refactor` at `1b6a148`), which rewrote much of `reader.hh` and
parts of `search.hh`, so a developer finds each cited site there by name. `F13:N` is
`docs/design/format-13.md` on `origin/phase4/format13` at `41730a3` (revision 1, the text the
re-check read; format 13 has since folded F13-R18 to F13-R26, tip `b2f7600`, keeping its
F-numbers, as the dependency table's format-13 row states), and `F13R:N` is that branch's
`docs/reviews/format-13-design.md` at `bae7fc1`, with revision 1's re-check at lines 787–1153. A
ratio is strata's time over the reference's, the floor-subtracted geometric mean over a class's
queries (BENCHMARKS.md:133-137); below 1.0 strata is faster.

Clean room. Lucene (Apache-2.0), Tantivy (MIT), and PISA (Apache-2.0) may be read, with their
license notice kept for any code ported; this design cites only their papers, their documentation,
and the benchmark harness's description of each engine (`bench/phase0/query.json`, its `details`).
No Xapian or Xapiand GPL source was read.

## Changes from revision 1

| Finding | Change | Where |
|---|---|---|
| QG-R21 | A1 adds 0 to +3 bytes a block over today's 3 to 6, 0 to +13.1 MB, likely about +8.7 MB; A2 and A4 −1 to +3, −4.4 to +13.1 MB; F10's flags on top of each, +0.55 MB at a bit a block or +4.37 MB at a byte | "The byte budget", M16 |
| QG-R22 | `ab.py` scores per tag over both query sets, a stratum (a tag with a colon) as a row beside its class, with its fixture test (`32d598a`); D14 names which rows its items judge | D14, E9a, "First steps" |
| QG-R23 | D14 item 2's `bm25_xapian` rows cover every item that changes a bound, the traversal, or which documents are scored: M1 to M6, M8, M9, M13, M17, M19, M20, and M24; E2 runs them for M1, M2, and M9, E9a for M24 | D14, E2, E9a, E9b |
| QG-R24 | The base check: before E9a's A/B and at every move of the base, the base's `dump` on E0's four indexes against E0's references, both SHA-256s recorded; a difference goes to layers' code reviewer, and references are re-made on the base only once it is explained | D4, D16, "The base check", T2 |
| QG-R25 | M23's 25-bit form reads the width from the block's header; D13's count split settles intersection and union as well as intersection_union and negated, and needs a per-class mismatch count that `gate.py` does not print yet; K13's overlap with K8 and K11 is the lead's to merge | M23, "What format 13 must record", D13, T8, "For the lead" |
| QG-R26 | Ruling 3's cap on boosted clause sums is withdrawn: the grid scales by the query's summed weight factors, which bound every sum, and a query with a negative boost keeps today's sums | M13, E17 |
| C5 | Before E9a's A/B: the per-class `ab.py` (`32d598a`), M24 and E9a's tests under code review, the base check | E9a, "First steps" |
| C6 | Before E2's A/B: QG-R23's scheme rows | D14, E2 |
| C7 | QG-R21, QG-R25, and QG-R26 are in this text before the format-13 tables reach format 13's candidate report | As above |
| Format 13 at `b2f7600` | It folded F13-R18 to F13-R26 after the re-check: citations stay at `41730a3`, the text the re-check read, and M17's record follows F7's new encoding, 26 bytes for one preset; format 13 still lists M18, which "What format 13 must record" removes | Conventions, the dependency table, M17, the budget, "What format 13 must record" |
| PLAN's targeted runs (main `21336b4`; the lead's direction) | No night-long query rows: two stratified samples, S (40 per class, 220 queries) to iterate and S′ (100 per class, 461) to confirm; E0 is the one calibration of every engine on S, after a 20k dry run; every later run times strata against each row's fastest engine, its drift anchor, the rest placed by their calibrated ratios; each item lands on an in-process A/B and a harness A/B against the fastest on its target rows; the gate rows are the lead's confirmations on S′; single-threaded runs take a light hold; each run's size in minutes, from the record | D17, D2, D11 to D14, D16, E0, E1 to E9, T1, T3, T5 to T8, Q5, Q6, "First steps" |

## Changes from revision 0

| Finding | Change | Where |
|---|---|---|
| QG-R1 | M17 bounds a list by w · t₁ / (t₁ + c · K(ℓ₁, A)) · (1 + 2⁻⁴⁰) with c = min(1, A / A'): its preconditions, the margin's derivation, argmax-first order with the rest in `competitive()`'s order, stored frontiers only, a per-segment preset record and Q1 (c)'s index option; M9 is in E5's base | "M17: one pair bounds a list", "What format 13 must record", E5 |
| QG-R2 | The invariant is format 13's text; P17's copy path re-selects each copied frontier's first pair at the merged segment's average | M17, "What format 13 must record" |
| QG-R3 | `check()` verifies domination and argmax-first; E5 builds a multi-segment index of different averages and one with deletes; a property test fails without the correction | M17, E5 |
| QG-R4 | M18 leaves format 13; the five conditions that would readmit it are recorded and not pursued | "M18: out of format 13" |
| QG-R5 | M23's check is a 64-bit sum, with the 32-bit lane comparison as its exact equivalent for blocks whose values fit 25 bits; `fuzz_segment`'s corpus gains a wrapping block; E6 times both | "M23: a check that cannot wrap", E6 |
| QG-R6 | One byte budget with units, sources, and totals, A1 and finer partitions costed (D15); E3 simulates only variants that fit beside A2 without M18; F10's revision 1 already gives the wide path | "The byte budget", E3 |
| QG-R7 | E3 is a mode of the real scorers with a substitutable bound source, calibrated against E2's and E4's A/Bs; D5 compares a projection's calibrated range with 3%; bound order is judged by E4's timed oracle | E3, D5, E4 |
| QG-R8 | `parity` at 1, 4, and 16 segments and reordered after every kept item; `dump` identity on every index against references made at E0's commit | D4, T2 |
| QG-R9 | D14 rewritten: `ab.py` per class, target rows named before the A/B, the win on the reordered merged index, reruns final in either direction and run for marginal wins, a cumulative guard every third kept item and before the freeze, `bm25_xapian` rows for weighting and bound items, and `ab.py`'s statistic named | D14, T3 |
| QG-R10 | macOS (the host, x86_64) is back in D12; SIMD items stay provisional until arm64 runs | D12 |
| QG-R11 | The mean-time table is rebuilt against each row's reference engine, Lucene `-bp` in 16 of its 30 cells | "Mean times against each row's reference", "For the lead" |
| QG-R12 | The stretch from per-class factors (about 1.22, so 0.5 needs about 59% off); T8 names its 35 rows and its ratio; PISA at about 0.78× the fastest baseline without intersection_union; D13 covers negated and settles semantics by counts; PISA's term rows carry a harness cost | "What the targets ask", D13, T8 |
| QG-R13 | D1 and T5 name two-phase-critic, `COUNT` term by its upper bound, negated at `TOP_100` and beyond, and the before-merging rows (H20, Q4), each with what closes it; the needed median is per row | "The rows to close", D1, T5 |
| QG-R14 | The term strata, cost terciles, and two-pass record from the raw files; H8 restated for unions of three or more clauses, H17 narrowed; M24 (two passes) is E9's first step and the baseline M4 must beat; E9 goes first after E0 and E1 | "Within the classes", H8, H17, M24, the queue |
| QG-R15 | A verdict rule and a confirming level per hypothesis, stated before E1 runs; T1 attributes each missing row's needed reduction, with the format-dependent part from E6 and E7 | "Hypotheses", T1 |
| QG-R16 | A ratio carries to main only through layers' step-6 A/B (D16) | D16 |
| QG-R17 | Allocation counts extend `Kronuz/allocators` additively; counters are `kronuz::metrics::Counter` | D8, E1 |
| QG-R18 | M3's costs: forward-only iterators and the forfeited equal-bound skip; the oracle seed is its ceiling | M3, E10 |
| QG-R19 | T9 (sanitizers, coverage, all eight CI jobs, fuzzing through format 13's T2); done criteria K1–K12 in the ledger | T9, the ledger |
| QG-R20 | M8's short-list bound measured or computed at the first threshold; M20's reach; M22's libraries; the 4.37 million blocks' source; the widest pruned gap is intersection_union's; D13's inexact rows; E0 records raw files with SHA-256 | M8, M20, M22, D13, E0 |
| Q1 to Q3 | The owner's decisions applied: Q1 (c), Q2 (b) within the budget, Q3 (a) | M17, D7, M15, "Questions for the owner" |
| Format 13 revision 1 | Citations move to `41730a3` and its re-check: M16 is F10's A1 to A4 (offsets relative to the group's base, level 1's wide path), and F13-R21's record carries M17's preset | Throughout |

## Summary

- **Where strata stands.** After merging, strata is 1.35–1.37× the fastest baseline on every
  command's global class (five trials, docid order, `2e774db`). Reordering takes 17–27% off
  strata's own top-k times on every class but single terms, and up to 30% off its counts (P1 T2).
  Projected onto the reordered index, the global rows sit at 1.07–1.14×. The widest misses are
  intersection_union (1.20–1.56×), negated at `TOP_100` and beyond (1.20–1.50×, no reordering
  factor), two-phase-critic at the same commands (1.19–1.43×), union (1.08–1.28×), single terms
  (1.15–1.22×; 1.42–1.65× in the frequent stratum), and phrases (1.14–1.20×). `COUNT` term reads
  1.00× with an upper bound of 1.41×. The projection is an estimate; E0 measures it.
- **Why.** Counted unions of three or more clauses score every match, where strata's own two passes
  would take 0.26–0.37× of its time (H8). Union and conjunction bounds fall back to group and
  whole-term bounds over windows wider than a block (H4, H7). Required-plus-optional and exclusion
  have no bulk path (H11, H12), and counted conjunctions score every match (H6). On long lists the
  threshold starts at zero (H1) and every bound evaluates a whole frontier (H2). Phrases verify
  positions at about 1.7× Tantivy's cost (H13). A counted block costs about 300 ns, a quarter of it
  in skip entries (H15, H16). Fixed per-query costs are at most 4% of the cheapest terciles'
  reference times, so they matter only near the floor (H17).
- **What format 13 must record before it freezes** ("What format 13 must record"): the advance
  entries' layout (M16, F10); score-ordered frontiers with argmax-first order, the tighter bound, a
  per-segment preset record (F13-R21), and a checked invariant that P17's copy path keeps (M17);
  partitions (M19), seeds (M20), and a positions codec field (M21) only inside one byte budget
  (D15); and M23 with a 64-bit sum check. M18 leaves format 13 (QG-R4). Each item is decided by
  evidence before it is built: calibrated work projections (E3), A/Bs in format-12 bytes (E5),
  microbenchmarks (E6, E7), and timed seeds (E4), on top of the format-free bound fixes (E2).
- **How it is measured** (D17). Every timed run is minutes on a stratified sample of each class
  under a light hold. E0 calibrates every engine once; every later run times strata against each
  row's fastest engine only, which is also its drift anchor; an item lands on an in-process A/B and
  a harness A/B against the fastest on its target rows; and a row that passes on the small sample
  is confirmed once on a larger one, which is its gate row.
- **The queue.** E0 (the lead's one calibration), E1 (attribution), the base check (the layers
  base's results against E0's references), E9 (two-pass counting for unions of three or more
  clauses, the record's largest single projection), then the format-13 experiments before the
  freeze, then the format-free items in the order the record and E1 rank them.

## What this depends on and what it changes

| Design or review | This design depends on | This design changes or asks |
|---|---|---|
| PLAN | The process rule and SYS-6 (PLAN.md:16-20); carried rows (PLAN.md:111-124); passing by the interval's bound (PLAN.md:105-110); the stretch (PLAN.md:143-148); Every gate (PLAN.md:179-193); Track P's `verify` and parity rule (PLAN.md:383-387) and platforms (PLAN.md:447-448). At main `21336b4`: targeted runs, calibrate once and then only the fastest, and scale up once (PLAN.md:106-119 there), and Track P's measurement on a small targeted sample against the fastest baseline (:397-403 there) | Nothing in PLAN's text; D12 meets the platform rule, and D17 applies the targeted-run rules, with Q5 and Q6 for what they leave open. Track P's items map to mechanisms: P2 to M6 and M7, P3 to M3 and M19, P4 to M22, P5 to M15, P6 to M8, M11, and M16, P8 to M14, P9 to M7 and M24, P21 to M20 |
| `format-13.md` revision 1 and its re-check | F1's freeze criteria, items 2 and 4 (F13:125-160); F7 (F13:365-380); F10 to F13 and their joint selection (F13:554-747); F14, F20, F21, F22; F13-R7, F13-R8, F13-R21 (F13R:424-491, 989-999). At `b2f7600`, after F13-R18 to F13-R26 and with its F-numbers kept: F13-R21's record is in F7 (`format-13.md:406-415` there), 26 bytes for one preset; M18 is still a candidate (`:731, 826` there); and this design is cited at revision 0 (`:10, 91` there) | "What format 13 must record": M17's record, invariant, check, and copy-path rule; M18 out of F11; M23's 64-bit check; M19's bound form; M20's reach; D15's budget as the selection's units. It is written against `41730a3`'s sections, which `b2f7600` keeps by F-number, and brings format 13's text to this revision (M18 out, M17 argmax-first under the tighter bound, M23's checks). E1 is F1 item 4's attribution |
| `layers.md` (main) and its branch at `1b6a148` | L6's P17 row and R8's standard for copy paths (layers.md:27, 400, 413-422); L15's decision rule and platforms (layers.md:733-744); the open step-6 A/B and the re-check's C5 (layers' ledger and review at `1b6a148`). Layers checks itself by `verify`, `parity`, equal index bytes, and its digest, none of which compares query results with main's (QG-R24) | P17 re-selects a copied frontier's first pair under M17 (QG-R2); D16 measures an item's ratios on the layers base against the fastest engine, so layers' open step-6 A/B enters no projection; the base check compares the base's `dump` with E0's references, and a difference is a finding for layers' code reviewer (QG-R24). Layers' recorded step rows come from `bench/quick` (`docs/perf/layers-refactor.md` on its branch), whose per-set table `32d598a` leaves as it was; L15's rule scores harness trials with `bench/ab.py`, which it does not touch |
| `reordering.md` | D5's tie rule and the measured cost of the equal-bound skip (reordering.md:209-253) | M3 applies the rule to docid-ordered segments and pays that cost |
| `frozen-index.md` (P20) | Quantized scores (frozen-index.md:29-32) | D7: toward the stretch they count only as bounds |
| `docs/perf/p6-multisegment.md` and its review | P6-4's allocation items; the fixed-point proposal (p6-multisegment.md:261-294); R6's allocation rows at 1 and 16 segments | M8 carries P6-4; M13 waits on the review's ruling 3, as QG-R26 amends it (no cap on boosted sums) |
| `docs/reviews/phase-2-gate.md`, `phase-2-design.md` | Ruling 2 and D3's dominance rule (phase-2-gate.md:84-92; phase-2-design.md:51-54); C15's owed rows (phase-2-gate.md:970-976); C7, closed for required-plus-optional and owed at Phase 3's gate for conjunctions of disjunctions, with intersection_union held to ≤ 1.0× (phase-2-gate.md:726-729, 939-941; phase-3-ratchets.md:361) | M18 stays out by them; E1 delivers C15's rows; M5 is intersection_union's path to that ratchet, and conjunctions of disjunctions, no gate query class here, stay under Phase 3's test-parity row |
| ARCHITECTURE | Block maxima are pairs (ARCHITECTURE.md:151-152); one implementation per mechanism (ARCHITECTURE.md:11-18) | Q2 reopens only if a stored bound fits the budget (D7) |
| BENCHMARKS, TESTING | Rules 1 and 3, tiers, cached baselines (BENCHMARKS.md:7-19, 181-204); coverage targets (TESTING.md:33-37) | The cached baseline gains a per-row anchor, each row's fastest engine with its calibrated band (D17, a first step). BENCHMARKS' gate tier ("Every query, five trials", "One night", BENCHMARKS.md:189) and "Gate rows still run every engine" (:204) predate PLAN's targeted runs, for the lead to align |
| `Kronuz/allocators`, `Kronuz/metrics` | The live-byte counters of `ALLOCATORS_TRACKED_MEM` (allocators.h:115-116 at `2ebd873`); `kronuz::metrics::Counter` (metrics.hh:33-42), already in strata's build through `completion` (its CMakeLists.txt:7-8, 14 at strata's pin `5251cf9`) | An additive `allocators` change, per-thread allocation counts and cumulative bytes (QG-R17); its one consumer, Xapiand, pins `2ebd873` |
| The system review of 2026-10-17 | SYS-1, SYS-6, SYS-7 | Nothing |

## Where strata stands

### The record

| Run | What | Commit | Where |
|---|---|---|---|
| n1, 2026-10-15 | After merging: strata force-merged in docid order against force-merged Lucene, Lucene `-bp`, and Tantivy (Xapian beside them); five trials over every gate query and the 200 stratified terms; pipe floor 67 µs | `2e774db` | `docs/gates/phase-3-ratchets.md:98-172`; raw in the gate VM, host copies in `~/.scratch/n1` |
| n2, 2026-10-16 | The quick tier (181 sampled queries and 40 terms, three trials) with PISA 0.8.2 | `db0a891` | `phase-3-ratchets.md:286-322`; `gate.py --md`'s output in `~/.scratch/quick-n2.md`; raw in the VM's `results/query-n2-quick` |
| P1 T2, 2026-10-17 | The quick tier, the reordered merged index over the docid-ordered one, interleaved; every class against the cached baseline reads DRIFT, since the anchor ran 1.21–1.31× slower | `c80996b` | `phase-3-ratchets.md:324-343`; raw in the VM's `results/query-t2-quick` |
| In-process records | One `COUNT` profile; A/Bs of P6, P6a, P7, the union-count fix, and two rejected decoders | several | `docs/perf/query-gap-merged.md`, `p6-multisegment.md`, `p6a-conjunction-bulk-topk.md`, `p7-membership-probes.md` |

### Per class

Each cell is n1's floor-subtracted median, then the projection for the reordered index: n1 times
T2's reordered-over-docid factor for the class and command (`phase-3-ratchets.md:331-337`). T2 has
no factor for negated or two-phase-critic.

| Class (queries) | `TOP_10` | `TOP_100` | `TOP_1000` | `TOP_100_COUNT` | `COUNT` |
|---|---|---|---|---|---|
| term (201) | 1.19 → 1.18 | 1.26 → 1.22 | 1.15 → 1.15 | 0.89 → 0.82 | 1.00 → 1.00 |
| intersection (300) | 1.16 → 0.88 | 1.35 → 0.99 | 1.39 → 1.01 | 1.49 → 1.09 | 1.48 → 1.04 |
| union (301) | 1.60 → 1.28 | 1.52 → 1.23 | 1.38 → 1.15 | 1.30 → 1.08 | 1.34 → 1.18 |
| intersection_union (40) | 1.93 → 1.56 | 1.89 → 1.55 | 1.61 → 1.30 | 1.50 → 1.20 | 0.93 → 0.94 |
| phrase (300) | 1.46 → 1.20 | 1.41 → 1.14 | 1.42 → 1.14 | 1.43 → 1.14 | 1.43 → 1.16 |
| negated (19) | 0.84 | 1.20 | 1.36 | 1.50 | 1.37 |
| two-phase-critic (1) | 0.63 | 1.19 | 1.42 | 1.43 | 1.38 |
| global (657) | 1.36 → 1.10 | 1.36 → 1.07 | 1.35 → 1.08 | 1.37 → 1.08 | 1.36 → 1.14 |

### Mean times against each row's reference

n1's mean time per query, strata against the row's reference engine, in µs. The reference is the
engine `gate.py` scores against (the largest geometric-mean ratio over Lucene, Lucene `-bp`, and
Tantivy, `gate.py:41, 158-159`) in most of the five trials: (bp) is Lucene `-bp`, (T) Tantivy. The
committed table has no `-bp` column (`phase-3-ratchets.md:131-172`), and `-bp` is the reference in
16 of the 30 cells of the first six rows, so revision 0's table, which took the smaller of the
Lucene and Tantivy means, cited the wrong baseline there. These figures come from the raw files
through the review's `n1means.py`. Means weigh the expensive queries, while the ratios above weigh
the typical one, so the two rank the classes differently.

| Class | `TOP_10` | `TOP_100` | `TOP_1000` | `TOP_100_COUNT` | `COUNT` |
|---|---|---|---|---|---|
| term | 292 / 255 (T) | 487 / 372 (bp) | 891 / 670 (bp) | 494 / 710 (T) | 199 / 200 (T) |
| intersection | 1,046 / 827 (bp) | 1,329 / 1,033 (T) | 1,542 / 1,084 (T) | 1,652 / 950 (bp) | 981 / 585 (bp) |
| union | 1,547 / 981 (bp) | 2,460 / 1,724 (bp) | 4,188 / 3,071 (bp) | 12,474 / 6,408 (T) | 1,403 / 1,000 (bp) |
| intersection_union | 4,053 / 2,084 (bp) | 4,422 / 2,325 (bp) | 4,790 / 2,864 (bp) | 4,576 / 3,054 (bp) | 261 / 273 (bp) |
| phrase | 2,355 / 1,662 (bp) | 2,521 / 1,806 (T) | 2,576 / 1,809 (T) | 2,565 / 1,800 (T) | 2,547 / 1,767 (T) |
| negated | 509 / 599 (bp) | 862 / 726 (T) | 1,144 / 875 (T) | 1,036 / 718 (T) | 665 / 514 (T) |
| two-phase-critic | 16,816 / 26,549 (T) | 32,382 / 27,195 (T) | 38,496 / 26,717 (T) | 38,119 / 26,683 (T) | 37,926 / 27,336 (T) |

### Within the classes

From n1's raw files, through the review's scripts (`n1split.py`, `twopass.py`):

- **Term strata.** The term class's gap is in its medium and frequent strata:

  | Stratum | `TOP_10` | `TOP_100` | `TOP_1000` | `TOP_100_COUNT` | `COUNT` |
  |---|---|---|---|---|---|
  | rare | 0.99 | 1.05 | 1.03 | 0.99 | 0.99 |
  | medium | 1.13 | 1.26 | 1.11 | 1.05 | 1.01 |
  | frequent | 1.52 | 1.65 | 1.42 | 0.70 | 1.02 |

- **Cost terciles.** In 24 of the 35 class and command rows, the ratio of the tercile where the
  reference is slowest exceeds the cheapest tercile's: `TOP_10` global 1.17, 1.41, 1.55; `COUNT`
  intersection 1.18, 1.47, 1.87; `TOP_10` phrase 1.15, 1.57, 1.74. The cheapest terciles' median
  reference times are 196 to 1,493 µs (129 to 1,426 µs above the floor), so a fixed cost of 1 to
  5 µs a query is at most 4% of them. The other 11 rows are unions at `TOP_100` and `TOP_1000`,
  intersection_union at four commands, negated at `TOP_10` and `COUNT`, and term at the two counted
  commands, `COUNT` term (1.06, 0.98, 0.95) being the row nearest the floor; global
  `TOP_100_COUNT` reads 1.35 in both.
- **Two passes against one.** Per query, strata's own `COUNT` plus `TOP_100` (best of the five
  trials, one floor subtracted) against its one-pass `TOP_100_COUNT`, with the geometric mean of two
  over one and mean times in µs:

  | Queries | n | One pass | Two passes | Two over one | Tantivy, one pass |
  |---|---:|---:|---:|---:|---:|
  | union, 2 terms | 198 | 2,421 | 2,537 | 1.09 | 2,375 |
  | union, 3 terms | 83 | 19,684 | 4,532 | 0.37 | 10,935 |
  | union, more than 3 | 20 | 76,207 | 11,202 | 0.26 | 26,058 |
  | intersection | 300 | 1,592 | 2,146 | 1.62 | 992 |
  | intersection_union | 40 | 4,362 | 4,446 | 1.03 | 3,323 |
  | negated | 19 | 1,001 | 1,395 | 1.46 | 666 |

  Two-term unions already count through `exact_count` (|A| + |B| − |A ∧ B|, search.hh:1450-1465)
  and then prune, so they run two passes today. Unions of three or more terms carry 99% of the
  class's mean gap to Tantivy (5.7 of 5.8 ms over these best-of-trial means).

### How good the projection is

The projection is an estimate, good to about ±5%:

1. n1 and T2 ran different commits. The union-count fix landed between them (0.932× in-process on
   the 301 unions' `COUNT`, `query-gap-merged.md:11-16`), so the `COUNT` union and global
   projections are high.
2. n2's sampled global ratios read above n1's (1.39–1.44× raw at `TOP_k` against 1.33–1.34×, with
   `-bp` setting most rows, `phase-3-ratchets.md:291-301`). n2 times T2 gives 1.11–1.16× at `TOP_k`
   and 0.97× at `COUNT`; the report's own anchor-scaled estimate is about 1.06× at global `TOP_10`
   (`:340-343`).
3. T2's factors are ratios of raw harness times on the quick sample; applying them to
   floor-subtracted medians is approximate.

### Where strata loses, in short

- **Term:** 15–26% at `TOP_k`, untouched by reordering and almost all in the medium and frequent
  strata (frequent 1.42–1.65×, rare within 1.05×). It wins `TOP_100_COUNT` (0.89×) and ties
  `COUNT`, both of which take the count from the document frequency.
- **Intersection:** reordering helps it most (0.70–0.76), which leaves `TOP_1000` and the counted
  commands near or above 1.0×.
- **Union:** 1.08–1.28× projected at top-k, where PISA leads by 2.1–3.9× (only its `TOP_10` rows are
  exact, D13). `TOP_100_COUNT` is the record's largest absolute gap, 6.1 ms a query on average
  against Tantivy, almost all of it in unions of three or more terms.
- **intersection_union:** the widest pruned gap, 1.89–1.93× at `TOP_10` and `TOP_100`, and 1.5 to
  2.1 ms a query behind Lucene `-bp` at every top-k command, counted top-k included.
- **Phrase:** flat across commands in every engine, so per-candidate verification sets it. Lucene
  `-bp` is the reference at `TOP_10` and Tantivy at the other commands.
- **Negated:** wins `TOP_10` and loses 20–50% at larger k and counted.
- **Two-phase-critic** (`+"the who" +uk`, one query): wins `TOP_10` (0.63×) and loses 19–43% at
  the other commands. Its phrase is two stopwords.

## What the targets ask

A row passes when its interval's upper bound meets the target (PLAN.md:105-110), so the median a
row needs is the target over the row's width, its upper bound over its median. n1's widths are
1.033 at the median row, 1.052 at the 75th percentile, and 1.41 at most (`COUNT` term), with 16 of
the 35 rows above 1.04 (the review's `headroom.py`). With n1's widths as the estimate, which E0's
replace, against the projection:

| Row | n1 | Projection | Median for ≤ 1.0 | Off the projection | Median for ≤ 0.7 | Off the projection |
|---|---|---:|---:|---:|---:|---:|
| intersection_union `TOP_10` | 1.93 [1.92–1.94] | 1.56 | 0.995 | 36% | 0.696 | 55% |
| union `TOP_10` | 1.60 [1.57–1.65] | 1.28 | 0.970 | 24% | 0.679 | 47% |
| phrase `TOP_10` | 1.46 [1.42–1.49] | 1.20 | 0.980 | 18% | 0.686 | 43% |
| term `TOP_100` | 1.26 [1.20–1.33] | 1.22 | 0.947 | 22% | 0.663 | 46% |
| intersection `TOP_100_COUNT` | 1.49 [1.46–1.62] | 1.09 | 0.920 | 15% | 0.644 | 41% |
| negated `TOP_100_COUNT` | 1.50 [1.25–1.52] | 1.50 | 0.987 | 34% | 0.691 | 54% |
| two-phase-critic `TOP_100_COUNT` | 1.43 [1.29–1.47] | 1.43 | 0.973 | 32% | 0.681 | 52% |
| term `COUNT` | 1.00 [0.99–1.41] | 1.00 | 0.709 | 29% | 0.496 | 50% |
| global, every command | 1.35–1.37 | 1.07–1.14 | 0.951–0.979 | 10–16% | 0.666–0.685 | 37–41% |

`COUNT` term's need comes from its width. Its median already reads 1.00×, and the row sits near the
floor, where a few µs of noise per trial moves the bound; E0's interval decides how far E8 must take
it.

**The stretch** (T8) is the geometric mean over the 35 class and command rows (the seven classes,
global excluded) of each row's floor-subtracted ratio against the fastest of Lucene, Lucene `-bp`,
and Tantivy (PLAN.md:143-148; `gate.py`'s reference). n2's 35 rows give 1.384 (1.342 raw, the
record's figure, `phase-3-ratchets.md:303`; the review's `n2rows.py`). Times T2's per-class factors,
with negated and two-phase-critic at 1.0, that is about 1.22 (an estimate, the review's
`stretch.py`), so 0.5 needs about 59% off strata's time. The global factor alone would give 1.12 and
55%, crediting reordering to classes it barely moves.

**PISA.** n2's `TOP_10` rows are PISA's exact ones. Over intersection, negated, and union, weighted
by their queries, PISA sits at about 0.78× the fastest baseline: strata is 1.32× the fastest and
1.70× PISA, and the two divide (the review's `pisa.py`; an estimate, since each geometric mean is
against its own engine per query). Over intersection and union alone, whose PISA semantics the
harness's query syntax suggests, it sits at about 0.84× (1.46 over 1.73 from `quick-n2.md`'s rows),
and with intersection_union it would be 0.65×. No count has settled any of the four classes yet:
the syntax is not evidence that PISA answers the same query, and E0's count split settles
intersection, union, intersection_union, and negated alike (D13, QG-R25). PISA gets there
with recursive graph bisection, block-max data per 40 postings, quantized BM25, and a precomputed
10th-best seed (`phase-3-ratchets.md:309-322`). The stretch therefore asks for 36% to 41% below
PISA's `TOP_10` time on those classes, exact and updatable. strata's leads over PISA on single terms
(0.39–0.87×) say nothing about pruning: PISA's term `TOP_k` rows take 2,112 to 3,347 µs against its
own 123 µs at `COUNT` (`quick-n2.md`), a per-query cost in its harness. strata does lead it on
counts (0.48× at global `COUNT`), where PISA neither prunes nor counts fast.

## The rows to close

T5 measures every class and command (D1). What closes each missing row, by the hypotheses and
mechanisms below:

| Row | n1 | Projected | Closed by |
|---|---|---|---|
| intersection_union, `TOP_k` and `TOP_100_COUNT` | 1.50–1.93 | 1.20–1.56 | M5 (E11), M1 and M2 (E2), M4 (E9b) |
| union `TOP_k` | 1.38–1.60 | 1.15–1.28 | M1 and M2 (E2), M17 (E5), M3 if E4a pays (E10), M6 (E12) |
| union `TOP_100_COUNT`, `COUNT` | 1.30, 1.34 | 1.08, 1.18 | M24 (E9a) for three or more clauses; M7 (E13) |
| phrase, every command | 1.41–1.46 | 1.14–1.20 | M10 (E14); M21 if E7 keeps it; M14 and M15 after the freeze |
| two-phase-critic, `TOP_100` and beyond | 1.19–1.43 | no factor | M10 (E14) and M15, its phrase being a stopword pair; M4 (E9b) for its counted rows |
| term `TOP_k` (medium, frequent) | 1.15–1.26 | 1.15–1.22 | M17 (E5), M9 (E2), M6 (E12), M3 if E4a pays (E10) |
| intersection `TOP_1000`, `TOP_100_COUNT`, `COUNT` | 1.39–1.49 | 1.01–1.09 | M2 (E2), M4 (E9b), M7 (E13), M16 (F10) |
| negated, `TOP_100` and beyond | 1.20–1.50 | no factor | M5's exclusion path (E11), M4 (E9b), M7 (E13) |
| term `COUNT` | 1.00 [0.99–1.41] | 1.00 | M8 (E8); the interval's width (E0) |
| The before-merging rows | unmeasured | | The same items, and H20's per-segment costs through M8 (E8); carried by the owner's decision (Q4) |

## How a query runs

```
serve, bench               strata_engine.cc:203-226, 572-587
  run(segs)                engine_query.hh:205-213
    term_table             searcher.hh:116-177   one dictionary walk per term and segment;
                                                 a short list's pairs computed on first use
    build                  engine_query.hh:112-143   term and phrase scorers, then conjunction,
                                                 disjunction, required_optional, exclusion
    search                 searcher.hh:185-232   one segment: top_k; several: one collector
      top_k                search.hh:1267-1327   a count from exact_count() when known
                                                 (1273-1278), else counted while collecting
      collect_top_k        term 336-346; conjunction of terms 542-591; disjunction 1348-1433
      generic loop         search.hh:1306-1323: everything else, and every counted conjunction
    COUNT                  count() in 4,096-ordinal windows, search.hh:1330-1340
```

Each 128-posting block has a level-0 entry: LEB128 fields and its whole Pareto frontier of (wdf,
length code) pairs (`writer.hh:785-793`). Entries group by 32 under a fixed-width level-1 entry
(`writer.hh:818-828`). Reaching a block in a new group decodes and validates that group's 32 entries
(`reader.hh:830-874`). A bound evaluates a frontier pair by pair (`search.hh:379-383`;
`format.hh:358-383`), and `max_score(from, upto)` returns the bound of the smallest single skip
entry covering the range: the block, its group, or the whole term (`search.hh:351-358`). A counted
union with no exact count runs `collect_top_k` with every clause essential and every match scored
(`search.hh:1360, 1383-1425`).

## Hypotheses

Each names its evidence in the code or the record, the counter that ties time to it, and the
ceiling a mechanism addressing it could reach. One rule decides them, stated before E1 runs.

**The verdict rule.** Per class and command, on the reordered merged index (the 16-segment index
for H20):

1. **Refuted** when the hypothesis's ceiling, the share of strata's in-process time that a mechanism
   addressing it could remove at most, computed as the table at the end of this section states, is
   below 3% on every row it targets. No such mechanism could pass D14's 3%.
2. **Confirmed** when the ceiling is at least 5% on a row it targets and its counter reaches the
   stated level, which ties that time to the cause it names.
3. **Open** otherwise, and while a measurement it needs (a later experiment, hardware counters) is
   missing.

### Single terms

- **H1. The threshold converges late on long lists.** A top-k query starts at θ = 0
  (`search.hh:195-306`), and `term_scorer::collect_top_k` walks groups and blocks in ordinal order
  (`search.hh:336-346, 403-430`), so blocks are scored at a θ below the final one until enough good
  postings have been seen. Whole frontiers leave 21 of `the`'s 32,563 blocks above the final top-10
  threshold (`phase-2-gate.md:84-87`); how many are scored before θ gets there is not recorded. The
  frequent stratum carries the term class's gap (1.42–1.65× at `TOP_k`).
- **H2. Bounds cost one round trip and one division per pair.** A skipped block evaluates every pair
  of its frontier (`reader.hh:590-594`; `format.hh:358-383`); a group bound decodes the group's
  frontier into a vector first (`reader.hh:625-630, 762-768`). Each pair goes through
  `decode_length`, then `encode_length` inside `weight::operator()`, then a division
  (`search.hh:105-111`). Frontier lengths per level were asked for at Phase 2 and never recorded
  (`phase-2-gate.md:974-976`, C15).
- **H3. Scoring is a scalar division per posting.** A competitive block scores every posting through
  `score_of` and `collect` (`search.hh:342-343, 373-375, 211-222`); at `TOP_1000` θ stays low and
  most of a frequent term's blocks compete.

### Intersections

- **H4. Window bounds fall back to group and whole-term bounds.** The conjunction's bulk path takes
  the lead's block as the window and bounds every other term over it (`search.hh:547-558`). The lead
  is the rarest term, so its block spans (other's density over lead's density) of another term's
  blocks: more than one whenever the other is denser, more than a group of 32 when it is 32 times
  denser. `max_score` then returns the group's bound or the term's (`search.hh:351-358`), which
  skips few windows and rejects few candidates at `s + rest` (`search.hh:564-571`).
- **H5. Every candidate advances every other term, and a rare lead lands those advances in new
  blocks.** An advance outside the decoded block decodes 128 postings, and outside the decoded
  group 32 level-0 entries (`reader.hh:542-568, 881-891, 893-944`).
- **H6. Counted top-k scores every match.** A counted conjunction cannot take the bulk path
  (`search.hh:543`) and runs the generic loop, which skips scoring only when the whole-list bound
  cannot compete (`search.hh:1306-1311`, with `current_bound` defaulting to it at `:141`). Ranking
  while counting adds 671 µs over `COUNT` on the intersection class's mean for strata, 474 µs for
  Lucene, 365 µs for Lucene `-bp`, and 185 µs for Tantivy (n1 means), and two passes lose there
  (1.62×, "Within the classes").

### Unions

- **H7. Window bounds degenerate for dense clauses.** The windowed MaxScore bounds each clause over
  4,096 ordinals (`search.hh:1373-1377`). A block of a term with density ρ spans about 128/ρ
  ordinals, so above ρ = 1/32 a window covers more than one block and gets the group's bound; a
  group spans 4,096/ρ ordinals, so a window crosses a group boundary with probability about ρ and
  then gets the term's bound. A term in 10% of documents takes its group bound in about 90% of
  windows and its whole-term bound in the other 10% (derived from the layout, not measured). The
  per-window essential split and the per-document consult (`search.hh:1379-1389, 1414-1424`) work
  from those bounds.
- **H8. Counted unions of three or more clauses score every match.** When counting, every clause is
  essential and every match is scored and collected (`search.hh:1360, 1383-1425`). The record
  confirms it: strata's own `COUNT` plus `TOP_100` take 0.37× of its one-pass `TOP_100_COUNT` time
  on three-term unions and 0.26× on longer ones, where the class's mean trails Tantivy's by 6.1 ms.
  Two-term unions already count through `exact_count` and prune (two passes read 1.09× there), so
  the extra pass revision 0 blamed for them is not where the gap is.
- **H9. Per-query setup weighs on cheap unions.** The bulk path allocates and zero-fills 32 KiB of
  accumulators per clause per query (`search.hh:1353`), on top of H17's costs. The record does not
  point at it: at `TOP_100` and `TOP_1000` the union's cheapest and middle terciles read 1.40–1.61
  against the most expensive tercile's 1.32–1.38, at reference times of about 500 µs and more above
  the floor, where a few µs of setup is about 1%.
- **H10. Bounds are coarser than PISA's.** PISA keeps block-max data per 40 postings
  (`phase-3-ratchets.md:311-312`); strata per 128, and its groups per 4,096.

### intersection_union and negated

- **H11. Required-plus-optional has no bulk path and never drives from the optional side.** Each
  required document pays a virtual `next`, a bound of the optional side over the required block,
  `score`, then an optional advance and score (`search.hh:865-920`), and the optional bound falls
  back as in H4 (`search.hh:906`). Where the required side alone cannot reach θ, only documents
  holding both sides can compete, yet the loop still visits every required document.
- **H12. Exclusion has no bulk path.** Each candidate advances the excluded terms
  (`search.hh:964-974`); `TOP_1000` and `TOP_100_COUNT` run the generic loop.

### Phrases

- **H13. Verification dominates, at about 1.7× Tantivy's cost.** Phrase rows are flat across
  commands in every engine (n1: strata 2,355–2,576 µs, Tantivy 1,767–1,809 µs). The phrase and
  intersection classes are the same 300 term sets, and over its intersection `COUNT`, strata's
  phrase `COUNT` adds 1,566 µs and Tantivy's 920 µs (n1 means). Per candidate, strata decodes the
  rarest slot's positions (`reader.hh:1130-1156`), steps position-block headers from the postings
  block's first position block (`reader.hh:1187-1207`), reads counts or wdfs
  (`reader.hh:1117-1121`), and sets up `phrase_freq` (`search.hh:1144-1191`). Two-phase-critic
  (`+"the who" +uk`) pays the same verification for every document holding its three terms, and
  its counted rows show it (1.38–1.43×).
- **H14. The positions encoding costs decode time.** Positions restart at each document
  (`docs/design/phase-2.md:140-142`), so each document's first delta is an absolute position inside
  a PFOR block. P13's candidates (FOR, PFOR, and Elias-Fano per block; deltas against the document's
  length) are unmeasured (F13:749-755).

### Counting

- **H15. Skip entries take about a quarter of a counted block.** The one profile (`+time +for
  +kids`, `COUNT`) puts about 300 ns on each touched block, 23% of it in `decode_group`,
  `block_index_for`, and `skip_sized_impacts` (`query-gap-merged.md:24-28`; F13R:430-437); removing
  all of it leaves about 230 ns. The record's "roughly 90 ns for Lucene" has no recorded derivation,
  and Lucene 10.4's blocks hold 256 documents (`bench/phase0/query.json`, `details`), so the
  comparison must say per how many postings (calibration, third row). E1 re-derives it as Lucene's
  time for the query over the 128-posting blocks strata touches, labeled an estimate.
- **H16. Bit-filling and per-window overhead.** `fill` sets one bit per posting
  (`reader.hh:710-734`; 22% of that profile), the conjunction ANDs per window (10%), and every
  window recomputes `prefer_windows()` (`search.hh:485`) and clears and counts 64 words
  (`search.hh:1333-1338`). Accumulating a word in a register lost to mispredictions (1.092×,
  `query-gap-merged.md:17-19`).

### Every class

- **H17. Fixed per-query costs, near the floor.** strata pays per query: parsing into vectors of
  strings (`engine_query.hh:26-54`); a term table of strings (`searcher.hh:148-167`); for every term
  under 128 postings, a full decode to compute its frontier before the scorer decodes it again
  (`searcher.hh:140-143`; `reader.hh:345-347, 1075-1080`); a 256-entry length table per term,
  copied by value into its scorer (`search.hh:74-83, 315, 434`), five of them for a three-term
  phrase (`search.hh:999, 1021, 1249`); two copies of each term's pair vector
  (`engine_query.hh:66, 70`; `reader.hh:478`); a heap allocation per scorer; a reserve of 2k + 1
  hits (`search.hh:196`); and H9's accumulators. The record bounds them: 1 to 5 µs is at most 4% of
  the cheapest terciles' reference times, and in 24 of 35 rows the most expensive tercile reads
  worse than the cheapest, so they are no class's gap (QG-R14). They can still move the rows nearest
  the floor: `COUNT` term, the rare stratum, and the cheapest terciles. Allocations per query by
  class are owed since Phase 2 (C15), and no allocation-counting build is in the tree at `ff4320b`.
- **H18. Memory stalls on block jumps.** A leapfrog lands on a block that is not in cache, and
  hardware prefetchers follow sequential scans and miss jumps. Whether the gate VM's guest exposes
  hardware counters is unverified: E1 tries `perf stat` first and falls back to the macOS host's.
- **H19. Mispredicted branches.** The rejected LEB128 decoder showed skip-entry streams bound by
  mispredicted value lengths (`query-gap-merged.md:29-33`); `advance`, `collect`, and `skip_blocks`
  branch per posting or per block. H18's caveat applies.

### Before merging

- **H20. Per-segment costs.** An unmerged index repeats per segment what a merged one pays once: a
  dictionary walk per term (`searcher.hh:154-167`), a scorer tree per segment (`build`,
  `engine_query.hh:112-143`), a short list's pairs per segment (`searcher.hh:140-143`), and the bulk
  union's per-clause accumulators (`search.hh:1353`). Before P6, 16 segments put strata's own excess
  over its single-segment Phase 2 gate at 1.03× to 1.58× (`phase-3-ratchets.md:49-53`); P6 removed
  part of that (`docs/perf/p6-multisegment.md`), and the before-merging rows are unmeasured since
  (`phase-3-ratchets.md:356`).

### Levels and ceilings

Counters come from E1's counting build; shares from its profiles. A ceiling that needs a later
experiment keeps its hypothesis open until that experiment runs.

| H | Counter, and the level that confirms | Ceiling (refuted below 3%) | Rows |
|---|---|---|---|
| H1 | Blocks scored that the final θ_k would have skipped (E3's oracle mode): at least 20% of blocks scored | E4a's measured reduction, since the oracle seed removes exactly this | term:frequent, union `TOP_k` |
| H2 | Pairs evaluated per bound: a mean of at least 2 | Bound-evaluation share × (1 − 1 / mean pairs per bound) | term, union, intersection `TOP_k` |
| H3 | Postings scored per query; no level, the ceiling decides | Scoring share × (1 − 1/s), s the speedup of E12's kernel microbenchmark (2 until it runs, an estimate) | term, intersection `TOP_k` |
| H4 | Non-lead window bounds resolved at group or term level: at least 30% | E3's calibrated projection of M1 and M2, then E2's A/B | intersection, intersection_union `TOP_k` |
| H5 | Advances landing in a new block or group: at least 30% | Share of `skip_to_block_containing`, `decode_group`, and `load_next_block` under conjunction advances | intersection rows |
| H6 | Candidates scored over matches: at least 0.5 | Scoring share × (1 − candidates whose block bound competes / candidates scored) | intersection `TOP_100_COUNT` |
| H7 | Window bounds of clauses denser than 1/32 resolved at group or term level: at least 50% | As H4, on unions | union `TOP_k` |
| H8 | Confirmed by the record (two passes at 0.37× and 0.26× of one) | 1 − E9a's measured ratio | union `TOP_100_COUNT` |
| H9 | Bytes zero-filled per union query (n × 32 KiB) | Setup share in the cheapest union tercile | union, cheapest tercile |
| H10 | Postings scored under the best partition variant: at most 0.8 of today's | E3's calibrated projection of that variant | union, term `TOP_k` |
| H11 | Required visits in windows the required side cannot win alone, holding no optional match: at least 50% of required visits | That share × the required side's share of time | intersection_union `TOP_k` |
| H12 | Excluded advances per included candidate: at least 1 | Share of the exclusion's advances | negated `TOP_1000`, `TOP_100_COUNT` |
| H13 | Positions decoded and headers stepped per candidate; no level | Verification share × (1 − 920 / 1,566), the record's Tantivy-over-strata verification cost (an estimate) | phrase, two-phase-critic |
| H14 | Exceptions per positions block and widths (E7) | Position-decoding share × (1 − the best codec's decode time over today's, E7) | phrase |
| H15 | ns per touched block on counted rows: at least 200 | Skip-entry share × (1 − E6's kept layout's ns per lookup over today's) | `COUNT` intersection, union |
| H16 | Windows and blocks per query; no level | Window-overhead share, plus fill share × (1 − 1/s'), s' from E13's microbenchmark (2 until it runs, an estimate) | `COUNT`, `TOP_100_COUNT` intersection, negated |
| H17 | Allocations per query; the probe queries' in-process time | Probe time over the row's floor-subtracted reference time | term `COUNT`, term:rare, the cheapest terciles |
| H18 | Last-level misses per touched block: at least 1, where the guest exposes counters | E15's measured reduction | every class |
| H19 | Branch misses in a function with at least 5% of a row's time: at least 5% of its branches | E16's measured reduction | the classes those functions dominate |
| H20 | Per-segment work per query (term-table walks, scorer constructions, per-segment buffers) | The 16-segment index's in-process time over the merged one's, less 1, the part in per-segment components | the before-merging rows |

## Mechanisms

Every effect below is an estimate: none is measured, and pruning gains do not add (D6).

### Format-free

| Id | Mechanism | Source | Rows | Estimated effect | Cost |
|---|---|---|---|---|---|
| M1 | Range-max bounds: `max_score(from, upto)` returns the largest block bound among the blocks the range covers, with group bounds for whole groups inside it, so the whole-term bound applies only to a range that reaches the tail or covers every group | Block maxima (Ding & Suel, 2011) applied to windows | union, intersection, intersection_union, phrase `TOP_k` | 5–25% on union `TOP_k`, 5–15% on intersections | Small: `term_scorer::max_score`, `phrase_scorer::range_bound`; block bounds are already cached per group (`search.hh:385-394`) |
| M2 | Block-max AND: a candidate is checked against its lead score plus the bound of the block holding it in every other term, read from skip data without decoding the block | Ding & Suel, 2011 | intersection, intersection_union, two-phase-critic | 5–20% on intersection `TOP_k` | Small; cheaper once M16 lands |
| M3 | Bound-ordered traversal: a term's groups, or a union's docid ranges, in descending bound order, stopping when the next bound cannot beat θ | P3's anytime half (Mackenzie et al., 2022) | term (frequent), union `TOP_k` | At most E4a's oracle seed, its ceiling | Medium: forward-only iterators need a backward seek or a fresh iterator per group or range; docid-ordered segments on this path lose the equal-bound skip (below) |
| M4 | Counted top-k: one pass counts every match by windows and scores only the candidates whose bound competes | New | `TOP_100_COUNT` intersection, negated, intersection_union, two-phase-critic; unions only where it beats M24 | 20–40% on those rows | Medium |
| M5 | Bulk required-plus-optional, driving from the optional side in windows where the required side alone cannot reach θ; bulk exclusion | Ding & Suel's AND, and a new driver rule | intersection_union, negated | 20–40% on intersection_union `TOP_k` | Medium |
| M6 | Vectorized block scoring: a decoded block scored with SIMD and compared against θ, survivors collected; the conjunction's lead likewise | P2's vectorization; Lucene documents vectorized block-max MaxScore and AND | term, intersection, `TOP_1000` most | 10–30% where scoring dominates | Medium; scores must stay bit-identical (D4, Risks) |
| M7 | Array intersection of decoded FOR blocks for counts, through `docset`'s SIMD merge and galloping; bitset AND and P7's probes kept for bitset blocks; the choice per block pair | P2, P9 | `COUNT` and `TOP_100_COUNT` intersection, negated, two-clause union counts | 10–25% | Medium; the kernels exist in `docset::detail` (`docset.hh:187-236` at the pinned `0859e43`), and exposing them is an additive `docset` change (D8) |
| M8 | Fixed-cost cuts: a trivial bound for terms without skip blocks (every score is below the term's weight factor under both presets), one length table per query shared by its terms, spans instead of copied pair vectors, per-thread accumulators and collector buffers kept zeroed, scorers from a per-query arena; per segment, P6-4's items (H20) | P6-4; Phase 6's allocation row (PLAN.md:164) | term `COUNT`, term:rare, the cheapest terciles; the before-merging rows | 1–5 µs a query (E1) | Small. The trivial bound loosens the union's bound order and MaxScore split (`search.hh:704-715, 721-736, 1364`), so it is measured on unions, or the pairs are computed when a threshold first exists, as P6a does (`p6a-conjunction-bulk-topk.md:9-11`) |
| M9 | Bound arithmetic: `by_norm` on the pair's code directly, the same double for all 256 codes, since `norm_k[c]` depends only on `decode_length(c)` (`search.hh:81`) and `decode_length(encode_length(decode_length(c)))` equals `decode_length(c)`; division-free skip tests with a rounding margin | New | term, union | Part of H2's share; in E5's base (QG-R1) | Small |
| M10 | Phrase path: per-candidate setup out of the loop; a postings block's position headers stepped once when several candidates share the block; counts from the wdf block once | New | phrase, two-phase-critic | 10–20% | Medium |
| M11 | Prefetch the next block in scans, and a jump's target block as soon as its skip entry resolves | P6 | every class | 0–5% | Small |
| M12 | In-code branch and layout hints from measured profiles: `[[likely]]`, `[[unlikely]]`, cold paths split out, hot fields of `postings_iterator` together | The owner's direction (D9) | every class | 0–5% | Small per hint |
| M13 | Fixed-point clause sums: one accumulator in the union window instead of one per clause | P6's open proposal (`p6-multisegment.md:261-294`) | union, intersection_union | Up to the 4–5% the clause-order sum costs (`p6-multisegment.md:252-254`) | Changes scores at the grid's resolution: under the review's ruling 3 as QG-R26 amends it (D4, E17) |
| M14 | Phrase prefilters | P8 | phrase | Unmeasured | Its reserved kind 18 and bit 8 (F13:116): after the freeze |
| M15 | Next-word entries for frequent pairs | P5 (Williams et al., 2004) | phrase over stopwords, two-phase-critic | Unmeasured | As terms under a host prefix, no format hook (F13:115); its bytes count in the disk row (Q3 (a)) |
| M24 | Two-pass counting for unions with no exact count: count a `fresh()` copy by windows with `count()`, then collect the top k with counting off, so the bulk path prunes | P9's plan choice; the record (QG-R14) | union `TOP_100_COUNT` | strata's own time on unions of three terms to about 0.37×, of more to about 0.26× (the record's per-query geometric means); about 0.70× over the class, two-term unions unchanged (an estimate) | Small: a rule in `top_k` and in `search`'s per-segment loop |

### Format 13

| Id | Mechanism | Source | Rows | Estimated effect | Bytes (D15) |
|---|---|---|---|---|---|
| M16 | Advance entries (F10, F13:554-627): F10's A1 to A4, fixed width per block, offsets relative to the group's base within 16 bits, level 1 keeping format 12's wide path (F13:576-584), one entry per 128 or per 256 postings; validation per block decoded (F13:606-619) | F10, P6 | `COUNT`; every advance and window bound | Up to H15's 23% on counted intersections (F13R:430-437); the top-k share from E1 | A1 0 to +13.1 MB, likely about +8.7 MB; A2 and A4 −4.4 to +13.1 MB; A3 below A2; F10's flags on top, +0.55 MB at a bit a block or +4.37 MB at a byte (estimates until `poststats`, QG-R21) |
| M17 | Score-ordered frontiers: every stored frontier argmax-first under the segment's recorded preset at its own average, the rest in `competitive()`'s order; a reader bounds the list by its first pair (below) | New | term, union, intersection `TOP_k` | H2's share | None per block; a preset record of 26 bytes per segment (F13-R21; F7 at `b2f7600`) |
| M18 | One argmax pair per block (out of format 13, below) | Tantivy's documented single pair per block | | | Would save 2 bytes per dropped pair: 4.4 to 17.5 MB at 1.5 to 3 pairs a block |
| M19 | Partitions (F12, F13:660-671): P0, blocks as partitions, no bytes; P1, variable partitions; finer fixed partitions; bounds as pairs, or Q2 (b)'s one byte | P3 (Mallia et al., 2017) | union, term `TOP_k` | E3 decides, calibrated | At least +8.7 MB for fixed partitions of 64, +26 MB for 32; about none for P1 at an average of 128 |
| M20 | Threshold seeds (F13:673-696), only in segments without deletes (F13:689-694) | P21 | union, term `TOP_k` | Up to E4a's ceiling, less what M3 captures | At most 9 pairs of 2 to 4 bytes per term with skip blocks: 1.8–3.6 MB per 100,000 such terms (their count is unrecorded) |
| M21 | A positions codec field (F14, F13:749-755) | P13 | phrase | E7 decides | Inside the positions row (0.980× keyed `-bp`) and the whole index's 3.2 MB |
| M22 | Elias-Fano and run containers (F11's E2 and E3): a probe into a dense block without decoding it whole; the kernels in `bitpack` and `docset` (D8), whose Roaring layout has no run containers (`docset.hh:855-858` at `0859e43`) | P4 (Ottaviano & Venturini, 2014; Chambi et al., 2016) | intersections with rare leads | E6's probe microbenchmark | At most 0: chosen per block by size |
| M23 | Ordinal deltas stored as gap − 1, checked by a 64-bit sum against the block's entry (below) | New | every decode (`check_increasing` was 4% of H15's profile) | At most about 4% on counted rows | At most 0: fewer bits where a block's widest gap is a power of two (unmeasured) |

### M1 and M2: bounds at block granularity

M1 keeps `max_score(from, upto)`'s contract, an upper bound on every score in the range from skip
data alone (`scorer.hh:37-45`), and changes what it computes. Today's smallest-covering-entry rule
reaches for the term's whole-list bound as soon as a range crosses a group boundary; M1 takes the
maximum of the block bounds the range covers, using group bounds only for groups entirely inside
it. A 4,096-ordinal window over a term in 10% of documents covers about three blocks (H7), so M1
replaces a 32-block bound with a three-block one in most windows. The conjunction window (H4), the
required-plus-optional window (H11), and the phrase's lead bound (`search.hh:1221-1233`) all go
through it.

M2 is Ding and Suel's block-max AND: the window still comes from the lead, but each candidate is
checked against the sum of the bounds of the blocks that hold it, one per term, blocks that
`advance` would reach anyway. Both keep results exact by construction, and E2 checks them by
`dump` identity.

### M3: bound-ordered traversal

For a single term, compute every group's bound (today's `skip_blocks` computes nearly all of them
anyway, one at a time, `search.hh:403-430`), visit groups in descending bound order, score the
competitive blocks of each, and stop at the first group whose bound cannot beat θ. For a union, the
same over docid ranges aligned across the clauses (64Ki ordinals to start, a preset), each range's
bound being the sum of its clauses' M1 range bounds, with today's windowed MaxScore inside a range.
Hits arrive out of ordinal order, so the collector compares docids on ties and hands scorers the
score just below the k-th, the rule reordered segments already use (`search.hh:288-298, 1285-1301`;
`reordering.md:209-253`).

**Its costs** (QG-R18). Iterators only move forward (`reader.hh:542-568`; `:882`, "targets only
move forward"), so groups or ranges in bound order need a backward seek or a fresh iterator per
group or range, each re-decoding a group's level 0. A docid-ordered segment on this path collects
through `begin()` and forfeits the equal-bound skip: at most 0.7% on the gate queries, 1.9% on
`bm25` terms, and 7.4% on `bm25_xapian` terms (`reordering.md:242-253`). E10's A/B pays both. The
oracle seed bounds what M3 can gain: M3 reaches the final threshold's neighborhood after a few
groups, and a seed has it from the first posting.

### M4, M5, and M24: counted top-k and the required-plus-optional driver

M24 is a plan choice. `top_k` and `search`'s per-segment loop already take a count from
`exact_count()` and then prune (`search.hh:1273-1278`; `searcher.hh:202-210`). For a union with no
exact count (three or more clauses), M24 counts a `fresh()` copy with `count()`, which fills window
bits and never scores (`search.hh:1330-1340`; the disjunction's `fill`, `:805-815`), then collects
the top k with counting off. A union with a clause that has no `fresh()` (a phrase,
`search.hh:147`) keeps one pass, and deleted documents stay excluded, since `live_scorer::fresh`
wraps the copy (`searcher.hh:60-63`). A union too small to prune pays its second pass for nothing;
if the A/B shows a loss there, the rule gains a cost floor taken from its per-query results.

M4 merges the counting window and the bulk top-k into one pass. Per window, every clause fills its
match bits (P7's probes and the bitset AND as today), the bits are counted, and only the candidates
whose block bounds can beat θ are scored. Conjunctions score their candidates through M2's check;
unions score only their essential clauses' documents, consulting the others while a document can
still compete. On unions M4 must beat M24; on the other counted classes it is the path, since two
passes lose there (1.62× on intersections, 1.46× on negated).

M5 gives required-plus-optional a bulk path with two drivers. In a window where the required side's
bound plus the optional side's can beat θ but the required side's alone cannot, only documents
holding both can compete, so the optional side leads and the required side confirms, as in a
conjunction. Elsewhere the required side leads, rejecting candidates whose score plus the optional
side's M1 bound cannot compete before the optional side moves. Exclusion's bulk path scores the
included side's block, filters by θ, and checks exclusions only for survivors.

### M17: one pair bounds a list

**The scores.** Both presets score a posting with wdf t at length ℓ as w · g(t, ℓ; A), where
g(t, ℓ; A) = t / (t + K(ℓ, A)) and K(ℓ, A) = k1 · ((1 − b) + b · max(ℓ / A, m)). Here w is the
term's factor (its idf with the boost, and for Xapian the k3 and k1 + 1 factors), A is the average
length as the preset clamps it (max(1, avg) for `bm25`, `search.hh:79`; avg, or 1 when it is 0, for
`bm25_xapian`, `:95`), and m is 0 for `bm25` and `min_normlen` for `bm25_xapian`
(`search.hh:60-111`). A pair (t, ℓ) bounds the postings it dominates because g rises with t and
falls with ℓ, and a pair's ℓ is a decoded length code, never above a document's length
(`format.hh:256-257`).

**The bound.** Let A be the segment's average, A' the snapshot's, and c = min(1, A / A'). Then
K(ℓ, A') ≥ c · K(ℓ, A) for every ℓ: if A' ≤ A, c is 1 and every part of K grows or stays; if
A' > A, max(c · ℓ / A, m) ≥ c · max(ℓ / A, m) because m ≥ c · m, and (1 − b) ≥ c · (1 − b). So
g(t, ℓ; A') ≤ t / (t + c · K(ℓ, A)) for every pair, and the right side orders pairs as g at A does,
since both order by K(ℓ, A) / t and scaling every K by c keeps that order. With the argmax of g at
A first, every posting the list covers scores at most

    w · t₁ / (t₁ + c · K(ℓ₁, A)) · (1 + 2⁻⁴⁰)

In one merged segment A' = A and c is 1, and the bound is today's whole-frontier bound times the
margin. On synthetic 128-posting `bm25` blocks with A'/A from 1/4 to 4, it is within 0.13% of
today's bound at the median, where revision 0's form, w · g₁ · max(1, A' / A), is looser by about
A'/A itself when A' > A; neither falls below a block's true maximum in 120,000 random blocks of both
presets (the review's `m17_tighter.py`, `m17_loosen.py`). The correction is load-bearing: the first
pair evaluated at A' without it understates 6,865 of 200,000 random blocks (`m17_bound.py`).

**Preconditions.** 0 ≤ b ≤ 1 (for (1 − b) ≥ c · (1 − b)), k1 ≥ 0, m ≥ 0, and w ≥ 0. `weight::bm25`
does not check a boost's sign (`search.hh:76-78`), so a query with a negative boost, or a preset
whose parameters break these, evaluates every pair as today.

**The margin.** The writer picks the first pair by computed g, and the reader's bound and the
scorer's scores are computed in doubles, each a chain of about ten correctly rounded operations at
most. Relative to exact arithmetic, the writer's pick may sit up to 20 units of 2⁻⁵³ below the true
argmax, the reader's bound up to 10 units below its exact value, and a computed score up to 10 units
above its exact one: under 41 units in all with the final product's rounding, about 2⁻⁴⁷·⁷. The
factor 1 + 2⁻⁴⁰ covers that about 200 times. Today's bound carries no margin, since it evaluates
each pair with the scorer's own expression at A'.

**The order.** The writer stores every frontier (block, group, term) with the argmax first and the
rest in `competitive()`'s order (`format.hh:491-509`), which stays monotone in wdf and length, so
the rest can be delta-coded if the budget needs it (D15). Only the first position matters: a skip
test that the first pair fails skips the block, and one it passes competes, so the order of the rest
is never read for a skip. Ties in g resolve by the higher wdf, unique within a frontier, so the
order is deterministic. The writer knows the segment's average before it writes any frontier:
frontiers come from the lengths of documents already added (`writer.hh:454-455, 741-742`; the total
at `writer.hh:570-572`).

**The record.** A reader may trust the first pair only where the segment records the order it
holds: the preset's scheme and its k1, b, and `min_normlen` (F13-R21, F13R:989-999). Format 13 now
keeps it at the end of `inverted`'s statistics, which lead `term_index`: a preset count, then per
preset its scheme code and k1, b, and `min_normlen` as binary64, 26 bytes for one preset
(`format-13.md:406-415` at `b2f7600`), and M17's order follows the first. Under Q1 (c) each index
names its preset, defaulting to `bm25`: an index option, recorded in the manifest and set by a
replicated operation, so replicas' flushes and merges order alike (F13:889-890). The reader uses
the first pair only when the query's scheme and parameters equal the record bit for bit; any other
query evaluates every pair as today. A segment written before the option changed keeps its record
until a merge rewrites it.

**What it covers.** Stored frontiers only. Lists under 128 postings compute theirs at query time in
wdf order (`reader.hh:345-347, 1075-1080`; `searcher.hh:140-143`) and are evaluated pair by pair,
or bounded by M8's rule.

**The invariant and its checks** (QG-R2, QG-R3). Every stored frontier dominates every posting in
its range, and in a segment that records an order, its first pair is the argmax at the segment's
own average under the recorded preset. `check()` verifies both; today it validates no pair
(`check.hh:16-41`). P17's copy path copies a block whose ordinals stay live and rewrites only its
base (`layers.md:400, 413-420`; `reclamation.md:112-114`). The block's pairs depend only on its
wdfs and its documents' lengths, so they stay valid, but the first pair must be re-selected at the
merged segment's average: a pass over a few pairs, with no postings decoded. Group and term
frontiers are rebuilt by the merge either way. Layers' R8 standard for copy paths, with decoded
equality against the rebuild path and `check()` among its checks (`layers.md:27, 413-422`), then
catches a missed re-selection through both.

### M18: out of format 13

M18 stores one argmax pair per block for each fast preset. It leaves format 13 on this evidence
(QG-R4, the review's ruling 2):

- It drops an accepted rule: stored pairs must dominate the dropped ones
  (`phase-2-design.md:51-54`), which the Phase 2 gate kept for any future cap
  (`phase-2-gate.md:92`). An argmax pair dominates nothing; it bounds the others only under one
  preset, with M17's correction.
- Every other weighting, Xapiand's default in an index ordered by `bm25` and any later preset,
  would fall back to 32-block group frontiers, a loss no row here measures.
- Its bytes rest on an unmeasured frontier length. At 2 bytes per compact pair
  (`format.hh:326-343`), it saves 4.4 to 17.5 MB at 1.5 to 3 pairs a block, and nothing at one.
- P17 would have to recompute the pair from the block's wdfs and norms, most of the decode P17
  exists to avoid (QG-R2).

It returns only with all five of QG-R4's conditions: E1's block-frontier histogram; its bytes
against the whole-frontier alternatives (M17's delta-coded rest, F11's E5); a `SCHEME=bm25_xapian`
A/B of the group-only fallback per row; P17's cost; and Phase 2's D3 amended by name in this design.
This design does not pursue them before the freeze, since M17 gives the one-read bound with whole
frontiers kept.

### M23: a check that cannot wrap

M23 stores a FOR block's ordinal deltas as gap − 1, so every step is at least 1 and a block's
ordinals increase by construction; the decode adds 1 per value (an additive `bitpack` mode, D8).
The check that replaces `check_increasing` must not be a 32-bit sum. `decode_delta` prefix-sums in
32-bit lanes (`bitpack.hh:538-551` at `abf8972`), so a crafted block whose gaps sum to
(last − base) + 2³² ends on the entry's last yet wraps in the middle. Today `check_increasing`
rejects that block at load (`reader.hh:919-920, 1015-1024`). Without it, `fill` would set
`bits[(d − base) >> 6]` for an out-of-order d (`reader.hh:710-719`) past a 64-word window
(`search.hh:1332-1337`), and `count_below` and every advance assume order (`reader.hh:998-1013`).

**The check**, at every block decode: the entry's last exceeds the block's base (the previous
entry's last, or −1 before a term's first block, which today's reader passes as the largest
ordinal, `reader.hh:900`) and is below the document count; and the 64-bit sum S of the block's n
stored values satisfies S + n = last − base. S is exact (128 values below 2³² sum below 2³⁹), and
with every step at least 1 the partial sums rise strictly from the base to the last, so no lane
wraps and every ordinal lies in (base, last].

A block whose header declares values of at most 25 bits needs no separate sum. The reader takes
the width from the header, never from the values, or the form would need the scan it saves
(QG-R25): a FOR block's width (`header & 0x3f`, `bitpack.hh:506` at `abf8972`), an exception
block's width plus its exceptions' high bits (at most 32 together, `:521-522`), or a constant
block's value (`:499-504`). Its n gaps then sum to at most 128 · 2²⁵ = 2³², so its exact final
ordinal lies in (base, base + 2³²], whose one member congruent to the lane's final value modulo 2³²
is the entry's last, which lies above the base and below the document count: the 32-bit lane's
final value equal to the entry's last proves the 64-bit equality. That case costs only the
comparison `load_next_block` already makes (`reader.hh:919`), which is where M23's saving over
`check_increasing` would come from; every other block takes the 64-bit sum. E6 times both forms; a
unit test and `fuzz_segment`'s corpus carry a block whose gaps wrap by 2³², and one whose header
declares 26 bits.

### What the query path needs from format 13

1. A target's block located without parsing: fixed-width lasts searched in place (M16, F10's A1 to
   A4).
2. A block's bound from one read: M17's first pair at the start of the block's impact entry, under
   the segment's recorded preset.
3. M1's range bounds walk consecutive blocks' bounds, so a group's impact entries stay contiguous
   (F10's first ask, F13:601-603).
4. Every byte inside one budget (D15).

## The byte budget

This is D15's budget, in the units format 13's joint selection spends (F13:698-747). The constraint
is format 13's (F13:714-717): postings at most 833.7 MB, keyed `-bp`'s `.doc`
(`phase-3-ratchets.md:195`), against format 12's 845.6 MB reordered (`:204`), so format 13's
postings changes must net at least −11.9 MB; and the whole index at most 2,542.5 MB, 3.2 MB above
format 12's 2,539.3 MB (`:203`), read together with F8's dictionary change. Totals are estimates until `poststats` projects them exactly
over the reordered 5M merge (F22, F13:923-927).

| Unit | Count | Source |
|---|---|---|
| Full blocks | 4.37 million | `phase-2.md:132`: ⌊df / 128⌋ summed over terms, which reordering does not change |
| Groups | about 136,600 full ones, plus one partial group per term with skip blocks | 4.37 million / 32 |
| Terms with skip blocks (df ≥ 128) | not recorded | `poststats` |
| Pairs per block frontier | not recorded; Phase 2 assumed one or two (`phase-2.md:132-135`) | E1's and `poststats`' histograms |

| Item | Bytes per unit | Total at 5M | Status |
|---|---|---|---|
| F10 A1 (last u32, offset u16), over today's level-0 last and length fields | 6 against two LEB128 values of 3 to 6 bytes together: a last delta of 2 to 4 (a full block's last lies at least 128 past the previous block's; 4 where 128 postings span more than 2²¹ ordinals, terms under about 305 documents at 5M, and in a term's first block, whose delta is its last ordinal, `writer.hh:786`) and a length of 1 or 2 (the block's bytes shifted with the bitset flag, `writer.hh:787`): 0 to +3 per block, likely about +2, most blocks belonging to dense terms with a 2-byte delta and a 2-byte length (QG-R21) | 0 to +13.1 MB, likely about +8.7 MB | estimate; the flags below come on top |
| F10 A2 or A4 (last minus the group's base as u16 when the group's span fits, else u32; offset u16) | 4 where a group spans under 2¹⁶ ordinals, against today's 3 to 5 there (each delta below 2¹⁶ takes 2 or 3 bytes): −1 to +1; 6 elsewhere, against 3 to 6: 0 to +3 | −4.4 to +13.1 MB | estimate; the flags below come on top |
| F10 A3 (bit-packed per group) | below A2 | unmeasured | `poststats` |
| F10 at 256 postings per entry | half the entries | half of the above | `poststats` |
| F10 flags, one bit per block (FOR and bitset only) | 1/8 per block | +0.55 MB | exact given the count |
| F10 flags, one byte per block (more codes kept) | 1 per block | +4.37 MB | exact given the count |
| M17's order | none per block; 26 per segment for one preset | about 0 | exact |
| M17's rest delta-coded | below today's 2 per pair past the first | an unmeasured saving | `poststats` |
| F11's E1 to E5 (F13:642-651), M23 | per F11 | unmeasured savings | `poststats` |
| M19, fixed partitions of 64 | at least +2 per added partition (a bound byte and a last byte) | at least +8.7 MB (4.37 million added) | estimate |
| M19, fixed partitions of 32 | at least +2 per added partition | at least +26 MB (13.1 million added) | estimate |
| M19, F12's P1 at an average of 128 | a last per partition, in flagged terms only | about 0 net | estimate |
| M19, finer partitions on long lists only | at least +2 per added partition | per variant | `poststats` |
| M20, seeds | at most 9 pairs of 2 to 4 bytes per term with skip blocks, plus a count per k | 1.8–3.6 MB per 100,000 terms | estimate; the term count is unknown |
| M21, a positions codec field | per positions block | unmeasured: the positions row has 31.1 MB of room (1,525.0 against 1,556.1 MB, `:205`), the whole index 3.2 MB | E7 |
| M18 (out) | −2 per dropped pair | −4.4 to −17.5 MB at 1.5 to 3 pairs a block | estimate |

With F10's flags, A1 is +0.55 to +13.7 MB at a bit a block and +4.4 to +17.5 MB at a byte, which
F10 needs if M22's codec codes are kept (F13:568-570); A2 and A4 are −3.8 to +13.7 MB, or 0 to
+17.5 MB. Nothing is decided on these ranges: `poststats` projects each width candidate's bytes
exactly before any code (F13:703-713).

**The rule.** Before E3 simulates partition variants, `poststats`' projection (F13:703-713) fills
the unmeasured savings. E3 then simulates only variants whose bytes fit beside A2's, or the cheapest
layout E6 keeps, and the measured savings, without M18. A variant that adds 8.7 MB or more fits only
beside savings of that size elsewhere, so fixed partitions of 64 or 32 enter only if `poststats`
measures such savings; variable partitions averaging 128 postings or more, or finer partitions on
long lists only, fit without them. Every format item's table in `docs/perf/query-gap-format13.md`
states its bytes against this budget.

## What format 13 must record

Format 13's revision 1 carries M16, M17, M18, and M23 as candidates (F11's table, F13:642-651; the
joint selection, F13:698-747) and leaves their experiments to this workstream (F13:1114-1115). Per
candidate, what its text must state if the candidate is kept, and what decides it:

| Candidate | Format 13 must record | Decided by |
|---|---|---|
| M16, advance entries | F10 (F13:554-627): the kept width candidate (A1 to A4) and granularity (128 or 256 postings), the flags field's width, level 1's wide path; validation per block decoded, with M23's check if M23 is kept | E6 over E1's traces; `poststats`' bytes; the joint selection's objective (F13:718-720) |
| M17, frontier order | F7 (F13:365-380): the order record at the end of `inverted`'s statistics, which lead `term_index`: a preset count, then per preset a scheme (`bm25`, `bm25_xapian`) and its k1, b, and `min_normlen` as doubles, 26 bytes for one preset, M17 ordering by the first (F13-R21, as F7 now states it at `b2f7600`). F11's M17 row (F13:649): argmax-first at the segment's own clamped average under the recorded preset, the rest in `competitive()`'s order, ties by wdf; the bound w · t₁ / (t₁ + c · K(ℓ₁, A)) · (1 + 2⁻⁴⁰), c = min(1, A / A'), used only under the recorded scheme and parameters, with 0 ≤ b ≤ 1, k1 ≥ 0, m ≥ 0, w ≥ 0, on stored frontiers only. The joint selection's M17 row (F13:744): bytes "a per-segment record". F20 (F13:844-874): the order preset as an index option in the manifest, set by a replicated operation. F21 (F13:876-890): no digest record, since the order is derived; merge output a function of inputs and the preset (T8). F22 (F13:892-936): a fixture with an ordered segment, and `check()` verifying domination and argmax-first. The copy-path rule for P17, with layers' L6 row (`layers.md:400`) | E5, on E2's base with M9 |
| M18, argmax pairs | Removed from F11 (F13:650), from the joint selection (F13:745), and from F10's second ask (F13:601-604) | QG-R4 |
| M19, partitions | F12 (F13:660-671): P0 unless a variant inside D15's budget clears 3% by E3's calibrated range; a kept variant bounds by pairs, or under Q2 (b) by one byte per partition holding g₁ rounded up, read as w / (1 + c · (1/g₁ − 1)) under the same order record and used only to skip, with ARCHITECTURE's rule restated by its reason | E3, D15 |
| M20, seeds | F13 (F13:673-696): its reach, segments without deletes only (F13:689-694); its bytes in the joint selection | E4b, on top of M3 when E4a pays (E10); D15 |
| M21, positions codec field | F14 (F13:749-755): the field only if E7's keep line holds | E7 |
| M22, Elias-Fano and runs | F11's E2 and E3 codec codes (F13:645-646), with their kernels in `bitpack` and `docset` (D8) | E6's probes; F11's size rows |
| M23, gap − 1 | F11's M23 row (F13:651) and F10's validation (F13:606-619): the 64-bit sum, S + n = last − base with the base −1 before a term's first block, the entry's last above its base and below the document count, and the 25-bit lane comparison as its equivalent for a block whose header declares at most 25 bits (a FOR width, an exception block's width plus its high bits, or a constant's value), the width taken from the header and never from the values (QG-R25); `bitpack`'s delta decode gains its +1 mode; F22: `fuzz_segment`'s corpus gains a block whose gaps wrap by 2³² and one whose header declares 26 bits | E6 |

## Decisions

- **D1. Scope.** The carried after-merging rows on one node under the gate's `bm25` preset: the
  quick tier's classes (term, intersection, union, intersection_union, negated, phrase,
  two-phase-critic, and global) at `TOP_10`, `TOP_100`, `TOP_1000`, `TOP_100_COUNT`, and `COUNT`,
  each by its interval's upper bound (PLAN.md:105-110), with what closes each in "The rows to
  close". The before-merging rows join when the Phase 3 gate measures them (owed,
  `phase-3-ratchets.md:356`), carried or not by the owner's decision (Q4). `bm25_xapian`'s rows
  (Xapiand's default, a Phase 7 seam row, `query-gap-merged.md:34-38`) are no target and are a
  no-loss guard for every weighting and bound item (D14). Phase 4's and Phase 5's classes and P10's
  shared thresholds are out.
- **D2. E0 is the one calibration.** The per-class table above is a projection across three runs
  at three commits. E0 replaces it before any mechanism is ranked: every engine, PISA and Xapian
  included, on the sample S, five interleaved trials (D17), naming each row's fastest engine and
  every engine's ratio to it, which every later run scores against. It is the re-recording F1
  item 4 projects from (F13:138-145). E0 also makes the `dump` references (D4), settles PISA's
  semantics by counts (D13), and records every raw file's path and SHA-256 (C4). It is no gate row:
  the gate rows are the lead's confirmations (T5).
- **D3. Attribute before optimizing.** E1 runs before any mechanism is measured. Its record is also
  F1 item 4's attribution: for a class that misses at the freeze, the part of the needed reduction
  it assigns to code outside the format, each part named with its mechanism and ceiling (T1).
- **D4. Results never change.** Every mechanism keeps `verify` at zero mismatches on the four
  indexes; `strata_engine dump` byte-identical (docids and `%a` scores at k = 10, 100, and 1,000
  under both schemes, and counts; `strata_engine.cc:467-493`), over the gate queries and the
  stratified terms, against the references made at E0's commit: one per query file for the
  reordered merged, docid-ordered merged, and 16-segment indexes, and the `STRATA_DELETE_EVERY`
  index's own; and `parity` at 1, 4, and 16 segments and reordered, counted and pruned, at 419 of
  419 (`phase-3-ratchets.md:280-284`; PLAN.md:385-387). The docid-ordered index already prints the
  reordered one's bytes over the gate queries (`phase-3-ratchets.md:210-214`); E0 checks the
  16-segment one once, and a difference there is a determinism finding. The references are main's
  and the items run on the layers base (D16), so no item's `dump` identity is read on a base until
  the base check (QG-R24) shows that the base prints the references' bytes. Bounds may change;
  scores, ties, and counts may not. M13 is the one candidate that changes scores, and under the
  review's ruling 3 (as QG-R26 amends it) it re-pins `dump` once, at its commit.
- **D5. Format items are decided by calibrated ceilings before they are built.** E3 projects the
  work of partitions and seeds in the real scorers, and its time projections are calibrated against
  E2's and E4a's measured A/Bs, which give each class a range [κ_lo, κ_hi] of measured over
  projected reduction. A format item enters format 13's candidate set when that range times its
  projection clears 3% at the lower end on a row it targets; it leaves format 13 with its decoder
  and hooks (F13:129-131, 731-732) when the upper end is below 3% on every row it targets, or when
  its evidence is missing at the freeze; in between, it needs a real A/B before the freeze. E3
  decides nothing for a class whose range leaves [0.5, 2]. E5 tests the pair order in format-12
  bytes (any order is valid format 12), E6 and E7 microbenchmark layouts and codecs, and E4 times
  seeds.
- **D6. Format items are measured on the strongest format-free base.** Top-k innovations do not add
  (Mackenzie & Moffat, 2020), so the pair order, partitions, and seeds are measured after E2's kept
  items, M9 among them, and after E9's on the counted rows. Seeds are worth their bytes only by what
  they add on top of bound order when bound order pays (E4a, E10).
- **D7. Bounds stay pairs, made cheap.** Block, group, and term bounds remain (wdf, length) pairs,
  as ARCHITECTURE requires (ARCHITECTURE.md:151-152), and M17's correction keeps one pair a valid
  bound under any snapshot's statistics. Q2 is decided within the budget: a stored partition bound
  only if a variant fits D15 and E3 projects it at 3% by its calibrated range. It is then g₁, the
  first pair's t / (t + K) at the segment's average under the recorded preset, rounded up, never a
  score;
  the reader bounds by w / (1 + c · (1/g₁ − 1)), QG-R1's form, which rises with g₁, so rounding up
  stays valid; it skips and never scores; pairs stay at group and term level; and ARCHITECTURE's
  rule is restated by its reason, bounds valid under any snapshot's statistics. Quantized values
  count toward the stretch only as bounds, since the stretch is exact (PLAN.md:143-148); P20's
  quantized scores would change results (`frozen-index.md:29-32`), so P20 counts toward it only with
  exact scoring.
- **D8. Kernels and tools live in the libraries that own them.** SIMD intersection and galloping
  come from `docset` (`intersect_merge`, `intersect_gallop`), decoding from `bitpack` (M23's +1
  mode), and M22's Elias-Fano and run containers land there too; allocation counts extend
  `allocators` (QG-R17), and counters are `kronuz::metrics::Counter`. Each change is additive, in
  its own repository with its CI, and updates the `cpp-libraries` catalog's entry; none is a second
  copy in strata (calibration, first row).
- **D9. No profile-guided optimization.** Branch and layout hints go in the code, each from a
  measured profile and each A/B'd like any other item (M12).
- **D10. No caching across queries and no parallelism inside one.** The benchmark repeats queries,
  and both baselines serve single-threaded (`p6-multisegment.md:165-168`).
- **D11. Who measures what.** The performance engineer runs the in-process A/Bs
  (`bench/quick/ab.sh`, `ab.py`), the harness A/Bs against the fastest engine (D14), counters,
  profiles, and microbenchmarks, each a single-threaded run under a light hold (D17). They are
  evidence for keeping an item, never gate rows (PLAN.md:16-20). The calibration (E0) and every
  confirmation, which is a gate row (T5, T7), are the lead's, on main.
- **D12. Indexes and platforms.** Every A/B runs on S over the reordered merged 5M index and the
  16-segment index (`p6-multisegment.md:176-186`), both built once and untimed (their scale is Q5);
  the docid-ordered merged index joins for order-dependent items (M3, M23). A kept item repeats its
  in-process A/B on S on the FreeBSD VM and on the host's macOS (x86_64), each a light hold of
  minutes (D17), and stays only if no set or class row's upper bound exceeds 1.03 there
  (PLAN.md:447-448). arm64 runs when its host exists (PLAN.md:374-379); until then the
  SIMD items (M6, M7, M22) are provisional, and their `docs/perf/` entries say so.
- **D13. PISA is a report row, at `TOP_10`.** Its `TOP_100` and `TOP_1000` rows of union,
  intersection_union, and global are not exact (`phase-3-ratchets.md:313-318`). Its semantics on
  intersection, union, intersection_union, and negated are settled by counts, the four alike
  (QG-R25): PISA answers real counts at `COUNT`, so a class in which every PISA count equals
  strata's is settled. `gate.py` counts mismatches per engine and command over every class
  (`gate.py:112-120`, printed without `--md` at `:220-228`), which settles the four at once only
  when PISA's `COUNT` mismatches are zero; otherwise they need a split by class, which `gate.py`
  does not print yet ("First steps", item 2). E0 applies it to n2's raw files and to its
  calibration's `COUNT` rows, where PISA answers S. Until a class is settled, its PISA ratios are
  estimates and no decision rests on them.
  PISA's term `TOP_k` rows carry a per-query harness cost and are no pruning comparison.
- **D14. The keep rule.** An item's `docs/perf/` entry names its target rows (classes or strata,
  and commands) before its A/Bs run. Two A/Bs measure it, both on S (D17):
  - The in-process A/B, `bench/quick/ab.sh` with S's files as `QUERIES` and `TERMS`, about two
    minutes, scored by `bench/quick/ab.py` (`32d598a`). Per command, it reports the geometric mean
    over a row's queries of B's best time over A's, with its 95% bootstrap interval over queries,
    in two tables: per query set, as it always has, and per tag over both sets. A tag without a
    colon is a class, as `gate.py` scores it (`gate.py:121-126`); a tag with one is a stratum
    (`union:num_tokens_3`, `term:frequent`), a row of its own beside its class, where `gate.py`
    drops it (`gate.py:123-125`; QG-R22).
  - The harness A/B, Track P's landing measurement (PLAN.md:397-403 at main `21336b4`): strata
    before and after beside the target rows' fastest engine, interleaved, five trials, on the
    target classes' part of S, scored for B over A by `bench/ab.py` and for each side against the
    fastest by `gate.py --baseline` with the calibration (D17).

  An item stays when all of these hold:
  1. **A win:** on at least one target row of the reordered merged index, a class or a stratum named
     before the A/Bs, the in-process A/B's upper bound is below 1.0 and its geometric mean at most
     0.97 (BENCHMARKS.md:15-19), and the harness A/B's B over A meets the same there.
  2. **No loss:** on every set and class row of the reordered merged and 16-segment indexes, the
     upper bound is at most 1.03. Stratum rows are reported in the item's entry, each one above 1.03
     named beside its class's row, and do not fail the item on their own: a stratum's interval is
     as wide as its few queries make it, so a guard over every stratum would fail items by chance.
     Every item that changes a bound, the traversal, or which documents are scored (M1 to M6, M8's
     trivial bound, M9, M13, M17, M19, M20, and M24) meets this under `SCHEME=bm25_xapian` too
     (`ab.sh:6-7`): `bm25_xapian`'s pairs bound more loosely (length codes against exact lengths)
     and each costs more (`search.hh:105-111`), so a win under `bm25` can lose there (QG-R23, whose
     list this extends by M8 and M20, which change bounds and pruning for the same reason).
  3. **Reruns:** a row that fails item 2, or a win whose upper bound lies between 0.97 and 1.0,
     reruns in-process on S′ with six rounds (about five minutes), and the rerun's result stands in
     either direction.
  4. **The cumulative guard:** at every third kept item and before the freeze, the queue's tip
     against its base commit, in-process on S′, every set and class row's upper bound at most 1.03,
     under both schemes; for writer-side items (M16, M17, M19 to M23), indexing and merge time
     likewise, at about 100,000 documents (PLAN.md:106-110 at main), the merge's reordering threads
     under a strict hold of minutes. A failing row is traced to the kept items whose own A/Bs lost
     most on it, which are fixed or removed until the guard passes.
  5. D4's checks, T6's other axes, T9, and D12's platforms.

  An item that loses is recorded with its numbers and removed (PLAN.md:383-387).
- **D15. One byte budget.** Every format item's bytes are charged against one budget, the postings
  row's −11.9 MB and the whole index's 3.2 MB, with its units and their sources ("The byte budget"),
  before E3 simulates a variant or an item enters format 13's selection.
- **D16. Ratios against the baselines are measured on the base, not projected.** The experiments'
  base is the layers tip, which changes no format-12 byte but whose time is open: step 2's `COUNT`
  row on the stratified terms reads 1.156× from one round, and step 6's L15 A/B against the Phase 3
  commit has not run (layers' ledger at `1b6a148`; its re-check's C5). So an item's ratio against
  the baselines comes from its harness A/B (D14): the base's own binary, before and after, beside
  each target row's fastest engine, with the other engines placed by their calibrated ratios
  (D17). It measures the binary that lands, so no factor for layers enters it (revision 1's
  product of E0's, layers', and the item's ratios goes), and it is evidence, never a gate row
  (D11): the gate row is the lead's confirmation on main once layers and the item have landed.
  The base's results are held to main's too: layers' own checks compare no query result with
  main's (`layers.md:27`; L15), so the base check (QG-R24) compares the base's `dump` with E0's
  references before E9a's A/B and at every move of the base.
- **D17. Targeted runs** (PLAN.md:106-119 at main `21336b4`; the lead's direction of 2026-10-17).
  No query row takes a night. Every size below is an estimate from n1's recorded times and spreads
  (the method is item 8), which E0's own log replaces.
  1. **The samples.** Two, drawn from the gate queries and the stratified terms by class (a query's
     first tag without a colon) and, within a class, by its stratum tag: `num_tokens_2`, `_3`, and
     `_>3` for intersection, union, phrase, intersection_union, and negated, every one of whose
     queries carries one; `rare`, `medium`, and `frequent` for the stratified terms. Each stratum
     gives its class's draw a share in proportion to its size, at least one query, as a prefix of a
     seeded permutation (seed 7), so the larger sample contains the smaller; a class no larger than
     the draw is taken whole. **S**, to iterate: 40 per class, 220 queries (40 terms, 13 from each
     stratum and the gate set's one term; 40 each of intersection, union, and phrase; all 40
     intersection_union, all 19 negated, and the one two-phase-critic). **S′**, to confirm: about
     100 per class, 461 queries.
  2. **Why they represent each class.** The strata are where a class's ratio moves in n1's raw
     files: union `TOP_100_COUNT` reads 1.10, 1.70, and 2.09 by token count, and terms at
     `TOP_100_COUNT` 0.96, 1.04, and 0.70 by stratum. A proportional draw keeps every stratum's
     weight, so the plain geometric mean over a class's sampled queries estimates the class's, and
     within a stratum a seeded draw is unbiased. What the draw costs is its 95% sampling half-width
     on a class's geometric mean: 2.9% at the median row on S (9.1% at the 90th percentile; 13.2% at
     worst, on `COUNT` union, whose two-term queries count through `exact_count` and longer ones do
     not), and 1.4% on S′ (4.9%; 7.0%). S steers the work; S′ decides a row.
  3. **The calibration** is E0: every engine on S once, naming each row's fastest engine, which is
     the row's reference and drift anchor, and every other engine's ratio to it. A new engine
     version, corpus, index, or sample means a new calibration (PLAN.md:111-115 there). It is not
     E3's calibration of projections (D5).
  4. **Every later timed run** times strata against the fastest engine of the rows it measures,
     interleaved, five trials, on S or its target classes' part, and places the other engines by
     their calibrated ratios, through the cached baseline (`baseline.py`, `gate.py --baseline`)
     with each row's own anchor. Today's cached baseline has one anchor engine, Tantivy, and a
     fixed 3% band (BENCHMARKS.md:195-204); per-row anchors are a first step. The drift test
     compares medians: when a row's anchor's five-trial median leaves its calibrated median by more
     than the larger of 3% and the calibration's own five-trial range over its median, that row of
     the run counts for nothing until E0's calibration reruns. Single trials are not compared,
     since one falls outside five calibration trials' range about one time in three by chance.
  5. **The confirmation** is the gate row (T5, T7): once a row passes on S, the lead times strata
     against that row's fastest engine on S′, five interleaved trials, on main. A row that fails
     it is fixed and iterated on S again (PLAN.md:116-119 there).
  6. **Holds.** The harness serves one query at a time (D10) and `strata_engine bench` runs on one
     thread, so every timed run here is single-threaded and takes a light hold
     (`QUIET_LEVEL=light`). Only T6's merge rows, with reordering threads, take a strict hold, for
     minutes.
  7. **Settings.** Five runs per query (`NUM_ITER=5`, best of five) and a 5 s warm-up per engine and
     command (`WARMUP_TIME=5`), against the record's 10 and 20 s (`night_p1.sh:47`). At 20 s the
     warm-ups take about three quarters of a strata trial over S (100 s of about 133). E0 checks the
     5 s warm-up against 20 s once, and every later run keeps E0's settings, so an anchor's drift
     compares like with like.
  8. **Sizes.** Each engine's recorded time for a query's ten runs in n1, halved for five runs,
     summed over the sample and the five commands, plus the warm-ups; and the sampling half-width
     exp(1.96 · √(Σ_h (N_h/N)² · (s_h²/n_h) · (N_h − n_h)/N_h)) − 1 over a class's strata h, s_h
     being the standard deviation of strata's per-query log ratio against the class's reference,
     each query's ratio the median of its five trials:

     | Run | Engines | One trial | Five trials |
     |---|---|---:|---:|
     | E0, the calibration on S | every engine | about 345 s, Xapian's queries 128 s of it | about 29 min, and about 5 for the warm-up check |
     | Every row of S against the fastest | strata, Lucene `-bp`, Tantivy | about 120 s | about 10 min |
     | One target row: union `TOP_100_COUNT` on S | strata before and after, Tantivy | about 20 s | under 2 min |
     | In-process A/B on S, six rounds of three runs | strata, both sides | | about 2 min |
     | In-process A/B on S′, six rounds of three runs | strata, both sides | | about 5 min |
     | The confirmation: every row of S′ against the fastest | strata, Lucene `-bp`, Tantivy | about 175 s | about 14 min |

  9. **The index.** The existing 5M indexes, built once and untimed, so no run builds one; Q5 asks
     the owner whether the query rows stay at that scale.

## Before the freeze, and after

| Format-13 item | Decides | Evidence | Without evidence at the freeze |
|---|---|---|---|
| M16 advance entries | F10's width candidate and granularity, the flags' width, validation | E1 (shares, traces), E6, `poststats` | Chosen by F10's rule and the joint selection alone |
| M17 pair order | In or out, with F7's record and Q1 (c)'s preset | E5, on E2's base with M9 | Out: format 12's order and no record |
| M18 argmax pairs | Out (QG-R4) | | Out |
| M19 partitions | P0, or a variant inside the budget, its bound as pairs or Q2 (b)'s byte | E3 (calibrated), D15 | Out: F12's P0, no bytes (F13:666-668) |
| M20 seeds | In or out | E4b, on top of M3 when E4a pays; D15 | Out |
| M21 positions codec field | In or out | E7 | Out: F14's default |
| M22, M23 | In or out; M23's 64-bit check | E6, F11's size rows | Out |

Every other mechanism (M1–M15, M24) is format-free and lands on its own A/B, before or after the
freeze. M1, M2, and M9 go first because D6 measures the format items on top of them.

## Experiment queue

The order (QG-R14; the lead's decision in the ledger): E0, E1, the base check, E9a; then the freeze
set, which has format 13's freeze as its deadline (E3, E6, E7, E2, E5, E4, and E10 when E4 says
so); then the format-free items, E9b, E11, E10, E12, E14, E13, E15, E16, E8, and E17. Each records
its table in `docs/perf/` with its raw files' paths and SHA-256s, names its target rows before its
A/B, keeps by D14, and passes D4's checks wherever it changes code. It builds on the base the lead
names ("For the lead", item 1). Every timed step is a single-threaded run of minutes on S under a
light hold, against each row's fastest engine where it times the baselines (D17); none takes a
night.

- **E0. The calibration (the lead).** About 35 minutes in all, under a light hold (D17).
  - The samples: S and S′ drawn as D17 states, by the sampler of "First steps", item 3, their
    files' SHA-256s recorded.
  - The dry run, first: at 20,000 documents, each engine indexes them and answers S at every
    command, one trial, a 1 s warm-up and one run per query, as `night_p1.sh`'s `DRY=1` shrinks its
    steps. It passes when no unit failed (`INDEX_FAILED`, `UNIT_FAILED`), every engine has a time
    and a count for every query of S (PISA's phrases excepted, as UNSUPPORTED), and no engine's
    counts are all zero. Anything else is a harness bug, fixed before E0 measures (PLAN.md:106-110
    at main `21336b4`).
  - The indexes: each engine's existing 5M index, strata's reordered merged one as
    `engines/strata-merged`, reused and never rebuilt in the run, each recorded with its files'
    SHA-256s.
  - The warm-up check, before the calibration: strata, Lucene `-bp`, and Tantivy (the record's
    reference engines) once on S at `WARMUP_TIME=20`. The 5 s warm-up stands if, for each of the
    three, the geometric mean over the rows of its five-trial median at 5 s over its time at 20 s is
    at most 1.03 (BENCHMARKS' 3%); otherwise the calibration runs at 20 s, and so does every later
    run (D17, item 7).
  - The calibration: every engine (strata, Lucene, Lucene `-bp`, Tantivy, Xapian, PISA) on S, five
    trials with the engine order rotated (`bench/query_trials.sh` with `SETS` naming S's two
    files, `TRIALS=5`, `NUM_ITER=5`, `WARMUP_TIME=5`), about 29 minutes; scored by `gate.py --md
    --target 1.0 --strata strata-merged` and distilled by `baseline.py` into the calibration file,
    with per-row anchors (D17, item 4).
  - Recorded per row (a class or a stratum, at each command): strata's floor-subtracted ratio
    against the row's fastest engine, with its interval and width (the needed median, "What the
    targets ask"); which engine is fastest; every engine's ratio to it, Lucene `-bp`'s and PISA's
    among them; the anchor's five-trial median and range; the class by the reference's cost
    tercile; PISA's count mismatches by class (D13); and every raw file's path and SHA-256 (C4).
  - The `dump` references: E0's `strata_engine dump` on each of the four indexes T2 names (the
    reordered merged; the docid-ordered merged, `STRATA_REORDER=0 strata_engine merge`; the
    16-segment; and the `STRATA_DELETE_EVERY` index), over the gate queries and the stratified
    terms, each output's SHA-256 recorded beside each index's segment files' SHA-256s and the two
    query files', so the base check can show it reads the same bytes.
  - E0's splits (each row's reference engine, the cost terciles, the strata rows, and PISA's
    mismatches by class) come from the raw files, so they need not hold up E0's run; `gate.py`
    does not print them yet and gains them first ("First steps", item 2).
  - Output: the calibration file beside the raw results, its SHA-256 in
    `docs/gates/phase-3-ratchets.md`'s calibration section, which holds the table.
  - Rows: every row; a calibration, kept by definition, and no gate row (D2).
- **E1. Attribution.**
  - Tooling, under code review:
    - A `STRATA_COUNTERS` build option, off by default and never in an A/B binary, counting per
      query with `kronuz::metrics::Counter`: blocks decoded by kind (FOR, bitset, tail); groups
      decoded; advances (in block, new block, new group); `max_score` resolutions (block, group,
      term) by caller and clause density; pairs evaluated per bound; windows (total, skipped);
      candidates (scored, rejected by a bound, collected, and whose block bound competes); postings
      scored; required documents visited in windows the required side cannot win alone, and those
      holding no optional match (H11); excluded advances per included candidate (H12); positions
      decoded and headers stepped. `strata_engine bench` prints them as extra columns.
    - Allocation counts from `Kronuz/allocators`, extended additively with per-thread counts of
      allocations and of cumulative bytes allocated beside its live-byte counters
      (`allocators.h:115-116`), both tracking modes tested and its CI green. The counting build
      links it with `ALLOCATORS_TRACKED_MEM=ON` in the tool only, since tracking overrides the
      global `operator new` for the whole program (its README). This delivers C15's owed rows and
      PLAN's allocation rows (PLAN.md:162-164), at 1 and 16 segments (the P6 review's R6).
    - `poststats`' frontier-length histograms per level (F22, F13:923-927), main-side until format
      13 lands.
  - Procedure: counters over S and over both full query sets, every command, on the reordered
    merged and 16-segment indexes. Counts are deterministic and untimed, so they need only a host
    where `quiet check` exits 0 or 2, at `nice -n 19`. Single-threaded, under a light hold (D17):
    `perf record -g` over `strata_engine bench IDX COMMAND 20 < CLASS.txt` per class and command,
    `CLASS.txt` holding S's queries of the class (hardware events if the guest has them, else
    `cpu-clock`), minutes in all; the three probe queries of H17 (an absent term, a df-1 term, a
    union of two df-1 terms) per command; H15's re-derivation from E0's Lucene times on S; the
    access traces E6 replays.
  - Output: `docs/perf/query-gap-attribution.md`, T1's record.
  - Keep or remove: not applicable; it orders the rest.
- **The base check (QG-R24), before E9a's A/B and at every move of the base.**
  - Who and when: the performance engineer, once the lead names the base ("For the lead", item 1),
    and again each time the lead moves it (a later layers tip, or main once layers lands). It
    times nothing, so it needs only a host where `quiet check` exits 0 or 2, at `nice -n 19`.
  - Procedure: the base's `strata_engine`, built at the named commit, on each of E0's four index
    directories, unchanged since E0 (every segment file's SHA-256 equal to E0's record):
    `strata_engine dump DIR < queries.txt` and `< terms-stratified.txt`, the files E0 used (equal
    by SHA-256), each output through `sha256sum`: eight outputs against E0's eight. Both binaries
    read the same segment files, which the base reads as format 12 just as main does (D16), so
    any difference is the query path's.
  - Output: `docs/perf/query-gap-base.md`: the base commit and E0's, and per index and query file
    both SHA-256s and whether they are equal.
  - On a difference: `cmp` names the first differing line, and the differing lines are counted by
    scheme and k, which each line carries beside its query (`strata_engine.cc:486-488`), and by the
    query's class from its tags. The list goes through the lead to layers' code reviewer as
    a finding against layers. No item's D4 check, and so no keep line, is read on that base until
    the reviewer explains the difference; only then are the references re-made on the base by E0's
    procedure, the old and new SHA-256s recorded side by side with the explanation cited. Meanwhile
    the lead may name main as the base (D16).
- **E9. Two-pass counting (M24), then counted top-k (M4).**
  - E9a, first after E0, E1, and the base check: M24's rule, as "M4, M5, and M24" states it. Before
    its A/B (C5): the per-class `ab.py` with its fixture test (`32d598a`), M24 and these tests under
    code review, and the base check passed on the base. Tests: on every gate query and the
    stratified terms, the two-pass count equals the one-pass count, at one segment and 16, with and
    without deletes; `verify`; `dump` identity (D4). Its in-process A/B runs on S under both
    schemes (D14 item 2, QG-R23), about two minutes each; its harness A/B times strata before and
    after beside the row's fastest engine (Tantivy in n1; E0 names it) on S's unions at
    `TOP_100_COUNT`, five interleaved trials, under two minutes (D17). Target rows: union
    `TOP_100_COUNT` and its strata `union:num_tokens_3` and
    `union:num_tokens_>3`, which `ab.py` now reports; the keep decision can stand on the class row
    alone. Expected, an estimate from the record: about 0.37× and 0.26× of strata's own time on
    those strata, about 0.70× on the class.
  - E9b, first among the format-free items after the freeze set: M4. Tests: counted top-k equal to
    an uncounted top-k plus `count()` on every query, both schemes. Its A/Bs as E9a's, on its own
    target rows. Target rows: `TOP_100_COUNT` intersection, negated, intersection_union, and
    two-phase-critic; union `TOP_100_COUNT` against E9a's base.
- **E3. Work under substituted bounds, a mode of the real scorers.**
  - Tooling, in the counting build only, under code review: a bound source the term scorer consults
    for `block_bound`, `group_bound`, `max_score`, and `skip_blocks` (`search.hh:351-358, 385-430`),
    and through them every parent's windows and splits. Sources: `stored`, today's skip data; or
    exact partition maxima under the query's weight, computed once per term from its decoded
    postings, at 32, 40, 64, 128, and 256 postings per partition and for variable partitions
    averaging 64 and 128 (Mallia et al., 2017), each returning for a range the maximum over the
    partitions it overlaps (at 128 that is M1's rule). Seeds enter through E4's flag on `top_k`'s
    floor: none, the oracle θ_k from `dump`, and F13's dominance-pair seeds computed from the
    decoded postings. The scorers, windows, and traversal are strata's own, and only where a bound
    comes from changes, so E3 is no second traversal (calibration, first row).
  - Procedure, offline and untimed (no hold, at `nice -n 19`): the counters per query and k
    under each source and seed on the reordered merged index, over S, the queries its calibration's
    A/Bs time, and over both full query sets. A time projection per class: each
    counter's change times its unit cost, the profile's time in that component over its count from
    E1, an estimate.
  - Calibration, before E3 decides anything: E3 projects M1 (the exact source at 128), M2 (E2's
    code, toggled), and the oracle seed, and E2's and E4a's A/Bs measure them. Per class and
    command, measured over projected reduction across those points gives [κ_lo, κ_hi] (D5).
  - Bytes per variant from `poststats`, under whole frontiers, M17's delta-coded rest, and Q2 (b)'s
    byte; a variant that cannot fit (D15) is not simulated.
  - Output: `docs/perf/query-gap-format13.md`: postings scored and blocks touched per class and
    variant against today's, the calibrated projections, and bytes. H1's and H10's counters come
    from its oracle and partition modes.
  - Keep or remove: D5 for M19.
  - Rows: none directly.
- **E6. Advance entries, probes, and M23's check, microbenchmarked.**
  - Procedure: a microbenchmark under `bench/quick/` over real skip data (the 50 terms E1's counters
    touch most), replaying E1's traces (sequential scans, leapfrogs, window bounds): format 12's
    `decode_group` and search; F10's A1, A2, A3, and A4 (F13:586-599) at 128 and 256 postings per
    entry; probes per candidate into FOR (decode and search), bitset (rank), and Elias-Fano (select)
    blocks; `check_increasing` against M23's two forms with gap − 1 decoding. ns per lookup, probe,
    and block check; bytes per entry. Single-threaded, minutes, under a light hold (D17). M23's
    wrapping-block test runs with it.
  - Keep or remove: the layout with the lowest projected ns per touched block inside D15's budget;
    M23 if one of its forms beats `check_increasing` beyond the microbenchmark's interval; M22 if its
    probes beat FOR's decode and search on the blocks E1's traces touch. The tables go to format 13's
    candidate report.
  - Rows: decides M16, M22, M23.
- **E7. Positions, offline.**
  - Procedure: from E1's phrase profiles, the share of position decoding and header stepping; over
    the 5M positions, exceptions per block and widths under today's PFOR, FOR, Elias-Fano, and deltas
    against the document's length (P13); a decode microbenchmark per codec, single-threaded, under
    a light hold (D17).
  - Keep or remove: M21 stays only if a codec projects at least 3% on a phrase row with the
    positions row still at or below 1.0× keyed `-bp` (0.980× today, `phase-3-ratchets.md:205`) and
    its field inside D15's budget.
  - Rows: decides M21.
- **E2. M1, M2, and M9.**
  - Procedure: each its own A/B, in that order. Unit tests in `search_test`: a range bound is at
    least every score in the range and at most today's bound, over synthetic lists and the stratified
    terms; a candidate's block-max AND bound is at least its score; M9's `by_norm` on codes equals
    today's for all 256 codes, exhaustively. D4's checks;
    `bench/quick/ab.sh OUT A_BIN IDX B_BIN IDX 6 3` with S's files as `QUERIES` and `TERMS` on both
    indexes, and again with `SCHEME=bm25_xapian` for each of M1, M2, and M9 (D14 item 2; QG-R23,
    before E2's A/B, C6); each item's harness A/B on its target rows (D14); E1's resolution and pair
    counts before and after. E2's measured reductions for M1 and M2 are two of E3's calibration
    points.
  - Target rows: union, intersection, and intersection_union `TOP_10`, `TOP_100`, `TOP_1000`, and
    phrase `TOP_k` (M1 and M2); term and union `TOP_k` (M9).
- **E5. Score-ordered frontiers (M17) in format-12 bytes.**
  - Base: E2's kept items, M9 among them (QG-R1).
  - Procedure: an experiment writer stores every frontier (block, group, term) argmax-first under
    `bm25` at the segment's own clamped average, the rest in `competitive()`'s order, ties by wdf.
    Two indexes: the 16-segment index merged with it into one segment (A' = A); and a multi-segment
    index the experiment writer builds from the corpus, with segments of different averages, plus
    one with `STRATA_DELETE_EVERY`. Each must match the unordered build's `strata_engine sections`
    sizes and pass `check` (extended, behind the experiment flag, with M17's invariant), `verify`,
    `dump` identity, and `parity`. An experiment reader bounds a stored list by M17's bound under
    `bm25` with its default parameters only, a build flag standing in for F7's record, since format
    12 records no order; `bm25_xapian` queries evaluate every pair. A property test over random
    blocks, both presets, and A'/A from 1/4 to 4 checks the bound against each block's true maximum
    and fails for the mutants that drop the correction (the first pair evaluated at A', or c set to
    1). Each binary serves its own index, as L15 does, in both of D14's A/Bs on S. The indexes'
    builds are untimed and run at `nice -n 19` with at most four reordering threads when `quiet
    check` allows builds. Counters: pairs evaluated per query.
  - Keep or remove: D14, with `bm25_xapian` rows in its item 2. If kept, M17 enters format 13's
    selection with "What format 13 must record"'s text; otherwise it is out.
  - Target rows: term, union, intersection `TOP_k`.
- **E4. Seeds, timed.**
  - E4a, the oracle: an experiment flag reads θ_k per query and k from a `dump` file and seeds the
    collector with the double just below it (`top_k`'s floor, `search.hh:1279-1284`;
    `top_collector::seed`, `:201`). A is the flag off, B on, same binary and index, over S's term,
    union, and intersection_union queries (seeds do not apply to conjunctions or phrases,
    F13:689-694). It is H1's ceiling, M3's and M20's, and one of E3's calibration points.
  - E4b: F13's dominance-pair seeds, computed by E3's mode, the same way.
  - Keep or remove: E10 is built when E4a passes D14 on a term or union `TOP_k` row; otherwise H1
    is refuted, and M3 and M20 are out. M20 stays only if E4b passes D14 on a target row, its bytes
    fit D15, and, when E4a passes, it still passes on top of M3, which puts E10 before the freeze;
    if E10 cannot run by then, M20 is out (D5).
  - Target rows: term, union, intersection_union `TOP_k`.
- **E10. Bound-ordered traversal (M3),** when E4a passes: before the freeze if E4b passes too,
  otherwise in the format-free order. Tests: ties straddling the k-th with groups visited out of
  order, one segment and many (`multisegment_test`'s and `engine_query_test`'s R2 cases), on the
  reordered and docid-ordered indexes. Its A/B pays M3's costs and runs on the docid-ordered merged
  index too. Target rows: term and union `TOP_k`.
- **E11. Bulk required-plus-optional and exclusion (M5).** Target rows: intersection_union and
  negated `TOP_k` and `TOP_100_COUNT`.
- **E12. Vectorized block scoring (M6).** An added test: SIMD and scalar scores bit-identical on
  every posting of the stratified terms under both presets, with floating-point contraction off in
  scoring code. Provisional until arm64 runs (D12). Target rows: term and intersection `TOP_k`,
  `TOP_1000` most.
- **E14. The phrase path (M10).** Target rows: phrase, two-phase-critic.
- **E13. Array intersection for counts (M7) and window overhead (H16).** Provisional until arm64
  runs. Target rows: `COUNT` and `TOP_100_COUNT` intersection and negated; two-clause union counts.
- **E15. Prefetching (M11),** with last-level misses before and after where counters exist. Target
  rows: the classes E1 finds stalled (H18).
- **E16. Branch and layout hints (M12),** one function at a time from E1's branch profile. Target
  rows: the classes the hinted functions dominate.
- **E8. Fixed per-query and per-segment costs (M8).** Allocations per query by class before and
  after (fewer, and none per match); the three probe queries; M8's short-list bound measured on
  unions, or its pairs computed at the first threshold. Target rows: term `COUNT`, term:rare, and
  the cheapest terciles; on the 16-segment index, the classes H20 confirms.
- **E17. Fixed-point clause sums (M13),** under the review's ruling 3 as QG-R26 amends it: only if
  E1 shows the window path's per-clause buffers still cost at least 3% on the reordered index; the
  grid per query, scaled by the query's summed weight factors from the snapshot's statistics alone,
  so it is the same in every segment; bounds rounded up to the grid; `dump` re-pinned once, with
  `verify`, `parity`, and `engine_query_test`'s segment independence unchanged.
  - No cap on boosted clause sums (QG-R26 withdraws ruling 3's). Every clause scores below its
    weight factor, boost included: a term scores w · t / (t + K) with K > 0 under both presets
    (`search.hh:78-81, 101-110`; `bm25_xapian`'s K is at least 0.75 at its defaults), and a phrase
    likewise over its terms' summed factors (`:1021-1023, 1047-1058`). So a query's sum stays below
    the summed factors the grid is scaled by, and the grid rules out overflow by construction; a cap
    would be a limit Lucene does not have (`limits.md:1-8`; calibration, sixth row). The proposal's
    overflow argument for unboosted clauses (`p6-multisegment.md:275-276`) is then not needed.
  - The scale needs w ≥ 0 for every clause, as M17 does. `weight::bm25` admits a negative boost
    (`search.hh:76-78`); such a query keeps today's per-clause double sums.

After the freeze: M19's partitions on format 13 if E3 kept the hook; M14 in its reserved section;
and M15 as terms under a host prefix, its bytes in the disk row (Q3 (a)), with the phrase rows
stating whether Lucene's documented CommonGrams configuration joins the reference set, since each
baseline runs in its fastest supported configuration (BENCHMARKS.md:60-61).

## Gate rows

| Row | Target | Procedure |
|---|---|---|
| T1 Attribution | For every class and command above 1.0× by E0's upper bound: the reduction of strata's in-process time it needs ("What the targets ask"), split into parts, each a named component with its counters and profile share, assigned to the mechanism that addresses it with that mechanism's ceiling; the part format 13 can change stated apart, with E6's and E7's unit costs, and no part of a miss at the freeze assigned to the format (F13:138-145); H1–H20 each confirmed, refuted, or open by the verdict rule | E1's counting build and `perf record -g` over `strata_engine bench IDX COMMAND 20 < CLASS.txt`, `CLASS.txt` holding S's queries of each class, under a light hold (D17); recorded with raw files and their SHA-256s in `docs/perf/query-gap-attribution.md` |
| T2 Exactness | After every kept item: `verify` at zero mismatches on the reordered merged, docid-ordered merged, 16-segment, and `STRATA_DELETE_EVERY` indexes (962 + 200 queries, both schemes, k = 10, 100, 1,000); `dump` byte-identical against E0's references over both query files (D4), on a base that passed the base check (QG-R24); `parity` at 1, 4, and 16 segments and reordered, counted and pruned, 419 of 419 | `strata_engine verify DIR < queries`; `strata_engine dump DIR < queries.txt \| sha256sum`, and likewise for `terms-stratified.txt`; the base check's record in `docs/perf/query-gap-base.md`; `parity CORPUS_JSON 10000 RANK_JSONL [SEGMENTS]` and `PARITY_REORDER` (`tools/parity.cc:1-11`); ctest |
| T3 Item A/Bs | D14 per class: the win on a target class or stratum of the reordered merged index in both A/Bs, no set or class row above 1.03 on it or the 16-segment index, `bm25_xapian` rows for the items D14 item 2 lists, strata reported, reruns final, the cumulative guard; the FreeBSD VM and the host's macOS for kept items | In-process: `bench/quick/ab.sh OUT A_BIN IDX B_BIN IDX 6 3` with S's files as `QUERIES` and `TERMS` (about 2 minutes), on S′ for a rerun or the guard (about 5), `SCHEME=bm25_xapian` for the scheme rows, scored by `bench/quick/ab.py OUT` (per set and per tag, `32d598a`). Harness: `bench/ab.sh` on the target classes' part of S with the rows' fastest engine (its settings and third engine a first step), scored by `bench/ab.py` and `gate.py --baseline` with the calibration. Each a light hold (D17); the table in `docs/perf/<item>.md` |
| T4 Format-13 candidates | Before the freeze, each of M16, M17, and M19 to M23 has its candidate table, its bytes against D15's budget from `strata_engine sections` and `poststats`, its E3 projection with its calibrated range where E3 applies, and a keep or drop line; M18 is recorded as out | E3 to E7; `docs/perf/query-gap-format13.md`, folded into format 13's candidate report (F13:129-131, 698-747) |
| T5 The carried rows | On main, every class and command after merging, two-phase-critic included, at or below 1.0× its fastest baseline by the interval's upper bound, by Phase 4's gate (PLAN.md:111-124): each row confirmed on S′ once it passes on S, its anchor inside its calibrated band (D17), by the bound Q6 settles; the before-merging rows likewise on the 16-segment index, by the owner's carry decision (Q4) | The lead's confirmation: `bench/query_trials.sh` with `SETS` naming S′'s files, `ENGINES` strata and the rows' fastest engines, `TRIALS=5`, and E0's `NUM_ITER` and `WARMUP_TIME`, about 14 minutes under a light hold; scored by `gate.py --md --target 1.0 --strata strata-merged --baseline CALIBRATION` with per-row anchors |
| T6 Other axes | Per kept item: allocations per query by class, at 1 and 16 segments, not growing with matches and not above before; serving memory (`bench/serve_mem.py`) at or below before; indexing and merge time at most 1.03× before for writer-side items (M16, M17, M19 to M23), at about 100,000 documents and five interleaved trials (PLAN.md:106-110 at main `21336b4`), and the same in D14's cumulative guard; disk exact per section inside D15's budget, at 5M | The counting build with the `allocators` extension; `bench/serve_mem.py`; `bench/index_trials.sh` and a timed `strata_engine merge` at 100,000 documents, the merge's reordering threads under a strict hold of minutes; `strata_engine sections` |
| T7 Phase 6 | Every class and command at or below 0.7× by the upper bound | As T5, at Phase 6's gate |
| T8 Stretch | Reported at every gate: the geometric mean over the 35 class and command rows (seven classes, global excluded) of each row's floor-subtracted ratio against the fastest of Lucene, Lucene `-bp`, and Tantivy; and the `TOP_10` rows against PISA for the classes E0's count split settles (D13: intersection, union, intersection_union, and negated alike) | From the gate's confirmation (T5), each row against its fastest engine, and PISA placed by its calibrated ratio (D17): `gate.py --md --baseline CALIBRATION` |
| T9 Hardening | Per kept item: its tests pass under ASan and UBSan, and TSan where it adds threads; its added code meets TESTING.md's targets (scorers, collectors, and counting at least 95% of lines and 90% of branches; decoders, readers, and format 98% and 95%, every corruption check reached); its commit passes all eight CI jobs, with SIMD paths built on `linux-arm64-gcc` and `macos-arm64`; a decoder it changes (M16, M21, M22, M23) is fuzzed through format 13's T2, an hour per section selector at the freeze commit (F13:900-907, 994) | CI on the item's commit; `tools/coverage.sh`; `fuzz/fuzz_segment.cc` |

## Questions for the owner

Answered (the lead, under the owner's standing permission, 2026-10-17; the ledger):

- **Q1. The preset that orders frontiers:** (c), each index's configured preset, defaulting to
  `bm25`, recorded per segment, with M17 argmax-first (QG-R1). Option (d) cannot apply to M17,
  which has one first position, and M18 is out. Applied in M17 and "What format 13 must record".
- **Q2. A stored score bound:** (b), conditional and expected to close as (a): a stored partition
  bound only if a variant fits D15's budget, stored as g₁ rounded up and used only to skip. Applied
  in D7 and M19.
- **Q3. P5's bytes in the disk row:** (a), they count. Applied in M15.

Open:

1. **Q4. If the before-merging rows miss when the Phase 3 gate measures them, are they carried into
   this track with the after-merging rows?** PLAN lets the owner carry a row that misses
   (PLAN.md:111-116); these are owed (`phase-3-ratchets.md:356`) and unmeasured since P6.
   - Options: (a) carried under the same rule as the after-merging rows, so Phase 4's gate does not
     pass while one misses; (b) not carried, so the Phase 3 gate waits for them; (c) carried to
     Phase 6's gate.
   - Recommendation: (a). The mechanisms that close the after-merging rows close these too, and
     H20's per-segment costs are M8's items; holding Phase 3's gate for them would also hold the
     format work that closes the carried disk rows. Nothing waits on Q4 until the rows are measured.
2. **Q5. Which index do the query rows run on under PLAN's targeted runs?** PLAN now iterates
   indexing at about 100,000 documents and says full-corpus runs never gate a phase (PLAN.md:106-119
   at main `21336b4`), and names no scale for query rows, which have always run on the 5M merged
   indexes.
   - Options: (a) the existing 5M indexes, built once and untimed, with D17's small samples, the
     confirmation on S′ being the gate row; (b) the indexing rows' scales, iterating at 100,000
     documents and confirming at a larger one (`8c59423`'s text gave indexing rows 500,000;
     `21336b4`'s names none), every engine's indexes built at both; (c) (a) for iteration, and every
     gate query at 5M for the confirmation.
   - Recommendation: (a). `8c59423`'s text read "indexing rows at 500,000 documents today; query
     rows on a sampled query set" (PLAN.md:106-108 there): for queries the sample shrinks, not the
     corpus. And this design's mechanisms act on long lists: at 100,000 documents a term in 10% of
     the documents has 10,000 postings, about 78 blocks in 3 groups, so the group and whole-term
     fallbacks (H4, H7), two-pass counting (M24), and seeds would be timed where the gate rows do
     not live. The 5M indexes exist, no run builds one, and against the fastest engine a run
     there stays minutes (D17's sizes). (c) contradicts "full-corpus runs never gate a phase" if
     that covers the query sets. Nothing in the design changes under (b) but the indexes and E0's
     builds; E0 waits on Q5.
3. **Q6. Does a gate row's bound cover the sample's draw?** PLAN's passing rule takes the 95%
   bootstrap interval of the median over trials (PLAN.md:120-125 there), which measures timing
   noise. On a sample, a row also carries the draw's error, the same in every trial: up to 7.0% on
   S′ (D17, item 2).
   - Options: (a) the trial interval alone, with the row's sampling half-width reported beside it;
     (b) one bound over both, bootstrapping the trials and, within strata, the sample's queries, so
     a row passes for its class and not for its draw; (c) S′ grown until its worst half-width falls
     below a set width.
   - Recommendation: (b). It changes no run, only `gate.py`'s interval (a first step if chosen),
     and keeps "the interval's bound meets the target" true of the class; (a) can pass a row whose
     class misses by the draw's error, and (c) buys the same answer with longer runs. The first
     gate row waits on Q6; nothing before it does.

## For the lead

1. **The base.** The experiments touch `search.hh`, `searcher.hh`, and `reader.hh`, which
   `phase4/layers-refactor` rewrote (steps 5a and 6) and which format 13 stays out of until its
   rebase. Recommended, as in revision 0: the layers tip (`1b6a148`, or its successor once CI is
   green), A and B both on it, ratios measured there against the fastest (D16). Naming the base
   starts the base check (QG-R24), moving it reruns the check, and main is the fallback base while
   a difference waits on layers' code reviewer.
2. **E0** is the lead's one calibration (D2, D11, D17): the dry run, the warm-up check, and five
   trials of every engine on S, about 35 minutes under a light hold, and the rows F1 item 4
   projects from. It makes the `dump` references by its procedure (four indexes, both query files,
   every SHA-256 beside the segment files'), and its splits come from its raw files once `gate.py`
   prints them ("First steps", item 2). The Phase 3 gate's query rows become confirmations on S′
   (T5), not a night. E0 waits on Q5 for its indexes. The gate report gains Lucene `-bp`'s column,
   and calibration gains a row for a mean-time table cited without its reference engine (QG-R11;
   both still open on main).
3. **Done criteria.** K1 to K13 are confirmed (`cad1ddb`). K13 repeats K8's platforms and K11's T9
   (QG-R25); merging them is the lead's.
4. **Tooling under code review:** the per-class `ab.py` with its fixture test (`32d598a`, ctest
   `ab_scorer`); `gate.py`'s E0 splits; D17's sampler, per-row anchors, and harness A/B settings,
   before E0; E1's counters and the `allocators` change (in that repository, additive; Xapiand
   pins `2ebd873` and moves only when it bumps); M24 with E9a's tests, before E9a's A/B (C5); E3's
   mode; E6's microbenchmark.
5. **Holds and order.** The indexing gap first (the index's "Queued", item 0), then E0 (the index's
   "When the gate run releases the host", item 5), which no longer needs a night. Every timed step
   here is single-threaded and takes a light hold of minutes (D17), E0's being the longest at about
   35; only T6's merge rows take a strict hold, for minutes. E1's counters and E3's counts run
   whenever `quiet check` allows builds. E0 is long because it times every engine: Xapian's queries
   are about 128 s of each 345 s trial, so without Xapian E0 would take about 23 minutes. PLAN asks
   for one calibration run of every engine, so whether Xapian stays in it is the lead's call.
6. **Format 13 and layers.** "What format 13 must record" is for format 13's contributor. When M17
   is kept, layers' L6 P17 row and R8's copy-path standard gain the first-pair rule (QG-R2).
7. **Q4** goes to the owner when the before-merging rows are measured; **Q5** and **Q6** now, since
   E0 waits on Q5 and the first gate row on Q6.
8. **BENCHMARKS.** Its gate tier ("Every query, five trials", "One night", BENCHMARKS.md:189) and
   "Gate rows still run every engine" (:204) predate PLAN's targeted runs; this design follows
   PLAN, and aligning BENCHMARKS is the lead's.

## For the architect

Revision 2 folds in QG-R21 to QG-R26 and C5 to C7, which need no re-check unless the lead asks
(C7), and applies PLAN's targeted runs (D17). These are choices rather than folded text, for
whichever review next reads this design:

1. D14's rows: item 2 judges set and class rows; a stratum is reported, and can be a named target
   for item 1, but fails no item on its own.
2. D14 item 2's scheme rows extend QG-R23's list by M8's trivial bound and M20's seeds, for its own
   reason.
3. The base check reads E0's four indexes and both query files with the base's binary, so a
   difference isolates the query path; it reruns at every move of the base, with main as the
   fallback base.
4. D13's split: `gate.py` counts mismatches per command over every class (`gate.py:112-120`), so
   "the same `gate.py` output settles all four" (QG-R25) holds when PISA's `COUNT` mismatches are
   zero; otherwise a per-class split settles them, which `gate.py` gains first ("First steps",
   item 2).
5. D17's samples: drawn by the harness's own stratum tags in proportion, S at 40 and S′ at 100 per
   class, sized from n1's per-query spreads and times (D17, items 2 and 8), so each sample's class
   rows estimate the class's with the stated half-widths.
6. D17's drift test compares the anchor's five-trial median with its calibrated median, against the
   larger of 3% and the calibration's own range, since a single trial leaves five trials' range
   about one time in three by chance.
7. E0's warm-up check: the 5 s warm-up stands when each reference engine's geometric mean over the
   rows of its 5 s median over its 20 s time is at most 1.03.
8. Q6 is a methodology question as much as the owner's: whether a sampled row's bound must cover
   the draw as well as the trials.

The rulings stand as applied: M17 with QG-R1's form (M17), M18 out (QG-R4), M13 under ruling 3 as
QG-R26 amends it (E17), and M3 with QG-R18's costs (M3, E10).

## First steps

None of these waits on an open question. Each is pushed for CI to validate while the host holds
builds, and each goes under code review:

1. Done: the per-class `bench/quick/ab.py` (D14, QG-R22) with its fixture test, `32d598a`.
2. E0's splits in `bench/phase2/gate.py`, additive and behind a flag, so the gate rows print as
   today: each row's reference engine; count mismatches per engine, command, and class (D13);
   strata rows beside their class; and each class split in thirds by the reference engine's
   floor-subtracted time per query. A fixture test as `ab.py`'s.
3. D17's tooling, before E0, each additive with today's behavior the default and a fixture test:
   - `bench/sample_queries.py` draws S and S′ by stratum, in proportion, at least one query per
     stratum, as prefixes of one seeded permutation, so S′ contains S (today it samples by class
     alone, `sample_queries.py:25`, and so draws the stratified terms without their strata).
   - `bench/phase2/baseline.py` and `gate.py --baseline` take each row's anchor from the
     calibration, its fastest engine, with D17's drift test; one anchor and 3% stay the default.
   - `bench/ab.sh` takes `SETS`, `COMMANDS`, `NUM_ITER`, `WARMUP_TIME`, and a third engine, the
     target rows' fastest; `bench/ab.py` reports strata rows beside their classes, which it drops
     today as `gate.py` does (`bench/ab.py:30`).
   - If Q6 takes (b): `gate.py`'s interval bootstraps the sample's queries within strata as well as
     the trials.
4. E1's tooling: the counters option, the `allocators` change in its repository, the counter
   columns, the probe queries, and `poststats`' histograms.
5. M24's rule with E9a's tests.
6. E3's bound-source mode and E4's seed flag, a few lines over `top_k`'s existing floor.
7. M1, M2, and M9 with E2's unit tests.
8. E5's experiment writer and reader, its property test, and `check()`'s frontier verification,
   behind experiment flags.

## Risks

- **The starting table is a projection.** Three runs, three commits, a drift-flagged night. E0
  replaces it; until then every reduction above is an estimate.
- **Gains do not add.** Each item is measured on top of the ones kept before it (D6), so the queue's
  order changes what each one shows, and D14's cumulative guard catches losses that add up.
- **The freeze's deadline.** E0 to E7, and E10 when E4 calls for it, take minutes each on the gate
  VM under light holds (D17), so the deadline's risk is building E3's mode and E5's writer and
  reviewing them, not VM time. A format item without its evidence at the freeze
  leaves format 13 (D5); a later win then costs format 14 and its N − 1 reader (F13:200-203).
- **E3's calibration may fail.** A class whose measured-over-projected range leaves [0.5, 2] gets no
  E3 verdict, and its candidates need real A/Bs, which cost VM time before the freeze.
- **Writer-side work while indexing trails.** M16, M17, and M19 to M23 add work at flush and merge
  while one-thread indexing runs at about 0.46× Tantivy's throughput (the index's "Queued", item 0;
  not yet a gate row), so T6's 1.03× bound applies to each and D14's guard to their sum.
- **M24's small unions.** A union too small to prune pays a second pass for nothing; the A/B shows
  whether the rule needs a cost floor.
- **Rows set by noise or by one query.** `COUNT` term's upper bound is floor-bound noise
  (1.00× [0.99–1.41]), and two-phase-critic is one query, so one mechanism can swing it either way.
- **Bit identity under vectorization.** A SIMD path whose floating-point contraction or operation
  order differs from the scalar one changes scores. E12's test and T2's `dump` identity catch it;
  scoring code compiles with contraction off.
- **Profiles in the VM.** Hardware counters under QEMU with HVF are unverified; without them E1
  samples on `cpu-clock` and takes branch and cache counts on the macOS host.
- **Presets outside the order.** Under M17, a query under another preset or other parameters stays
  exact but evaluates every pair, as today.
- **Damage with resealed checksums.** A frontier corrupted and resealed so that its first pair
  understates can make a skip drop a hit, as a corrupted frontier can today. That is
  `fuzz_segment`'s "fence or answer" rule for resealed inputs (F13:614-619); `check()`'s new
  frontier verification finds it offline.
- **Rebase churn.** Query-path code conflicts with the layers branch until it lands ("For the lead",
  item 1).
- **A sample misstates its class.** S's draw errs by 2.9% at the median row and 13.2% at worst
  (`COUNT` union), S′'s by 1.4% and 7.0% (D17, estimates). S steers the work and S′ decides a row,
  under the bound Q6 settles. An anchor that drifts voids a run's rows until E0 recalibrates, about
  half an hour.
- **The base's results.** If layers' query path prints other bytes than main's, no item is read on
  that base until layers' code reviewer explains the difference (the base check), which can delay
  E9a; main is the fallback base meanwhile.
- **A figure without a source.** H15's "roughly 90 ns for Lucene" has no recorded derivation; no
  decision here rests on it until E1 re-derives it.

## References

- Broder, Carmel, Herscovici, Soffer, Zien. Efficient query evaluation using a two-level retrieval
  process. CIKM 2003. (WAND.)
- Chambi, Lemire, Kaser, Godin. Better bitmap performance with Roaring bitmaps. Software: Practice
  and Experience, 2016.
- Dhulipala, Kabiljo, Karrer, Ottaviano, Pupyrev, Shalita. Compressing graphs and indexes with
  recursive graph bisection. KDD 2016.
- Ding, Suel. Faster top-k document retrieval using block-max indexes. SIGIR 2011. (Block-max WAND
  and block-max AND.)
- Lemire, Boytsov. Decoding billions of integers per second through vectorization. Software:
  Practice and Experience, 2015.
- Lemire, Boytsov, Kurz. SIMD compression and the intersection of sorted integers. Software:
  Practice and Experience, 2016.
- Mackenzie, Moffat. Examining the additivity of top-k query processing innovations. CIKM 2020.
- Mackenzie, Petri, Moffat. Anytime ranking on document-ordered indexes. ACM TOIS, 2022.
- Mallia, Ottaviano, Porciani, Tonellotto, Venturini. Faster BlockMax WAND with variable-sized
  blocks. SIGIR 2017.
- Mallia, Siedlaczek, Mackenzie, Suel. PISA: performant indexes and search for academia. OSIRRC at
  SIGIR 2019.
- Mallia, Siedlaczek, Suel. An experimental study of index compression and DAAT query processing
  methods. ECIR 2019.
- Ottaviano, Venturini. Partitioned Elias-Fano indexes. SIGIR 2014.
- Petri, Moffat, Mackenzie, Culpepper, Beck. Accelerated query processing via similarity score
  prediction. SIGIR 2019. (Threshold estimation.)
- Turtle, Flood. Query evaluation: strategies and optimizations. Information Processing and
  Management, 1995. (MaxScore.)
- Williams, Zobel, Bahle. Fast phrase querying with combined indexes. ACM TOIS, 2004.
- The search-benchmark-game harness's engine descriptions for Lucene 10.4.0 and Tantivy 0.26, as
  recorded in `bench/phase0/query.json` (`details`): block sizes, skip levels, impact layouts, and
  the top-k algorithms each implements.
