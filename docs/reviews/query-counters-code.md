# strata code review: query-counters (E1's counting build)

- Date: 2026-10-18. Tier: tooling (a code review only).
- Reviewed: `perf/query-counters` at `8ffede2` (`origin` at the same commit, nothing unpushed),
  `git log origin/main..perf/query-counters`: `55b2fe5` (the counters), `52a1cd4` (a bitset
  case), `a0fdda3` (the merge of main `d8d219c`), `8ffede2` (the ledger). Its base is main
  `d8d219c`; main is now `69a3f8c`, whose code is `734949e`'s, with layers and the inverter
  landed. Design:
  `docs/design/query-gap.md` E1 on `design/query-gap` at `e272ec2`; done criterion: that
  branch's `docs/status/query-gap.md` K1. Built and probed in the detached worktree
  `strata-wt/review-counters`.
- Read first: `docs/reviews/calibration.md`, whose checks were applied (CI on every platform for
  the reviewed commit; `HEAD` against `origin`; one implementation per mechanism; a claim is
  checked by running it), the branch's ledger, and E1 with the hypotheses' counter table (H1 to
  H20, `query-gap.md:497-523`).
- Clean room: no Xapian or Xapiand source read. `Kronuz/metrics` (MIT) read for `Counter`.

## Summary

Zero cost when off holds: with `STRATA_COUNTERS` off, every strata object of the tip is
byte-identical to its base's, the tool's included, with and without `-march=native`, and so is
every object of a trial merge with main but the test that does not compile. Counts are exact
where `counters_test` checks them (θ aside, CR1), and it passes under Release, ASan with UBSan,
and TSan; on the 100k index they are deterministic and sane. The counters are per thread: four
threads sharing one segment each count exactly what one thread does, with no TSan report.

Three things stand between this branch and E1's counts:

1. It does not build on main (CR3): `counters_test.cc` uses the API layers removed.
2. Three paths E1 measures count wrong or not at all (CR2), and θ is not the k-th score on the
   collector's path (CR1).
3. Ten of the 25 counters have no test, so a dropped count passes (CR4).

The rest is E1's fit (CR5) and smaller items (CR6 to CR8).

## What was run

The host: macOS 15.6 on an Intel i9-9980HK (x86_64), Apple clang 17.0.0 and Homebrew LLVM
23.1.2. Everything ran at `nice -n 19`, kept to four jobs under the light holds
`nostalls-core-r1` to `-r3`, and stopped through two strict holds (23:07 to 23:12, and
`scale-threads-mac2` from 23:31 to 23:37) by `.scratch/watch.sh`. Probes and scripts are in
`.scratch/` of the review worktree.

| What | Result |
|---|---|
| CI at `8ffede2` (run 39173532996) | All eight jobs green |
| Object identity, option off (`objid.sh`, `objcmp.sh`): every strata object of `d8d219c` and of `8ffede2`, Release, Apple clang, one source path and one build directory, every unit compiled from scratch (32 and 33, counted in the build logs) | 32 of 32 byte-identical (`counters_test`, on one side only, aside). `strata_engine.cc.o` is `11bc6dbe…deca` in both, the ledger's hash |
| The same with `-march=native`, the bench Makefile's flags (`bench/phase2/engine/Makefile:17`) | 32 of 32 byte-identical; `strata_engine.cc.o` `9fff94cf…2b65` in both |
| A trial merge with main `734949e` (`git merge-tree`: no textual conflict, tree `5113376`), built the same way with `-march=native` and compared with `734949e`'s objects | CR3: `counters_test.cc` does not compile. The other 34 objects are byte-identical to main's 34, so the hooks still cost nothing on layers' code; the merge's counting `strata_engine.cc` compiles (`merged_syntax.sh`) |
| `counters_test`: Release (Apple clang), ASan with UBSan and leak checks (LLVM, `-O1`), TSan (LLVM) | Passes in all three |
| `probe_threads.cc` under TSan: four threads, 50 rounds each of three queries (a full scan, a pruned `TOP_10`, a conjunction) on one shared segment, each round reset and read on its own thread | No report; every round's counts equal the single-threaded reference; the main thread's counters are its own |
| `probe_counters.cc`, Release and ASan with UBSan: θ by collection path; the rejected-by-bound identity on one segment and on two; advances on an exhausted iterator | CR1, CR2, CR5 |
| `probe_tail*.cc`: where a short term's second tail decode comes from | CR2 |
| The counting `strata_engine bench` on a copy of the 100k index (`idx-base-100k`, one segment, `seg-1.sx` SHA-256 `ebae6ebf…adbb`): all 962 phase-2 queries (`queries.txt`, `a8e8f2c1…54bd`) at `TOP_10`, `TOP_100`, `TOP_1000`, `TOP_100_COUNT`, and `COUNT`, one run each, and each query term's df; `check_invariants.py` over the outputs | Sane (below), with CR2's gaps. `the` at `TOP_10` gives the ledger's smoke figures exactly: 487 bitset blocks and a tail, 432 blocks and 7 groups skipped, 501 bound evaluations over 8,379 pairs, 6,311 postings scored, θ 0.170052 |

**The 100k run.** Over 4,810 query rows, by construction and against each term's document
frequency:

- `advances` = `advances_in_block` + `advances_new_block` in every row.
- `advances_new_group` ≤ `advances_new_block`, `windows_skipped` ≤ `windows`, `collected` ≤
  `candidates`, and `pairs_evaluated` ≥ `bound_evals` in every row.
- For a single term at every top-k command, `candidates` = `postings_scored`.
- FOR and bitset blocks decoded, and blocks OR'd, never exceed the full blocks of the query's
  terms, except at `TOP_100_COUNT` (170 rows), where a two-clause union's exact count walks a
  fresh conjunction of both terms before the collection walks them again (`search.hh:1485-1500`):
  real work, counted.
- Tails exceed the terms with one in 314 to 438 rows per command: CR2's item 4.
- `COUNT` reports 0 windows in all 962 rows, unions that OR 26,436 bitset blocks included: CR2's
  item 2.
- Deterministic: the first 40 rows at `TOP_10` and at `COUNT` equal the ledger's smoke files
  (`strata-wt/query-counters/.scratch/smoke-*.tsv`, built at `52a1cd4`) in every column but the
  time.

## Findings

### CR1. θ is the collector's last threshold, not the k-th score, and differs by path

- `top_collector::finish()` and `finish_docs()` record `min_` (`search.hh:230, 241`), which only
  `truncate()` raises: at 2k hits, or at the finish above k. A collector that took exactly k hits
  never truncated, so its θ is the floor, or 0 without one, though a k-th score exists. The heap
  path records the k-th score itself (`search.hh:1356`).
- Probe `probe_counters.cc`, one segment, a term with 10 postings, `TOP_10`: the collector path
  (`term_scorer`'s bulk collection) reports θ 0 and the heap path (the same scorer under a
  `live_scorer` with no deletes) 2.5706, the 10th score; the hits are equal. At `TOP_5` both
  report 4.0395.
- `counters_test.cc:87` pins that case: `theta == floor` for a top-1 whose one hit scores above
  the floor, so the final θ₁ is the hit's score and the test asserts the stale value.
- Why it matters: E1 prints θ per query and a mean per class (`strata_engine.cc:582-607`), H1
  is stated against the final θ_k, and E1 runs on the merged and the 16-segment indexes, which
  take different paths (`searcher.hh:185-232`). A column that changes with the path cannot be
  compared across them.
- Fix: in `finish()` and `finish_docs()`, after the sort, observe `hits[k - 1].score` when at
  least k hits remain, as the heap path does; `counters_test` expects the hot document's score
  and adds an exactly-k case on both paths. Before E1's counts.

### CR2. Events uncounted, or counted as something else, on paths E1 measures

1. **Rejections across segments.** `search()`'s generic loop drops a counted match that cannot
   compete without counting it (`searcher.hh:225`); `top_k`'s twin counts it (`search.hh:1317-1320`).
   Probe: `+a +b TOP_10_COUNT`, one segment, matches 13,333 = candidates 187 + rejected 13,146;
   the same documents in two segments, matches 13,334 but candidates 6,667 + rejected 0. Only
   the union counts its own (`search.hh:1451`), so E1's 16-segment runs undercount
   `rejected_by_bound` for every other counted query whose count is not known up front.
2. **`COUNT`'s windows.** `count()` fills 4,096-ordinal windows (`search.hh:1362-1371`), which
   `windows` never counts: 0 in all 962 `COUNT` rows at 100k. H16 names `COUNT` rows
   (`query-gap.md:519`).
3. **The exclusion's counting windows.** `exclusion_scorer::fill` advances the excluded side
   (`search.hh:972`) without counting it, and its closing `skip()` counts one `included` per
   window, not per candidate. H12 names `TOP_1000` and `TOP_100_COUNT`, so this is minor, but
   negated `COUNT` rows read as per-window figures.
4. **Tails decoded for bounds.** `segment::term()` computes a short term's competitive pairs by
   walking its postings (`reader.hh:346-348, 1085-1089`), and that walk is counted as a tail
   decode; the scorer's own walk is a second. Probe `probe_tail3.cc` on the 100k segment:
   `griffith` (df 97) at `TOP_10`, one tail after building the scorer and two after `top_k`.
   `tail_last()` decodes the tail's ordinals uncounted (`reader.hh:788-802`). M8's first item (a
   trivial bound for terms without skip blocks, `query-gap.md:540`) removes exactly the first
   walk, so E1 needs it apart.
- Fix: count 1 and 2 where they happen (2 as `windows` or its own counter, stated); count 3's
  advance and make `included` per candidate; count 4 as its own counter (tails walked for
  bounds), leaving `blocks_tail` the iterators'. With tests (CR4). Before E1's counts.

### CR3. The branch does not build on main (blocking)

- Main `734949e` holds layers, which removed `segment::term()` and changed `weight::bm25`'s
  first parameter. The branch merges into it without a textual conflict (`git merge-tree`, tree
  `5113376`), and the merge fails to compile `counters_test.cc`: three errors, at `:35` and
  `:90` (`seg.term`) and `:90` (`weight::bm25(seg, …)`). `.scratch/objid-merged-native-apple.build.log`.
- K1 asks for the tooling "on the experiments' base", which the lead names (`query-gap.md`, "For
  the lead", item 1): now main.
- The rest of the merge holds up: its 34 other objects are byte-identical to main's with the
  option off, and its counting `strata_engine.cc` compiles. The port is the test's alone.
- Fix: merge main (or rebase), port `counters_test.cc` to layers' API
  (`seg.get<inverted_plugin>().term(…)` and `weight::bm25` over that reader), and redo the
  object identity at the new tip against main's objects.

### CR4. The tests leave ten counters unchecked, and the counting tool unbuilt in CI

- `counters_test.cc` asserts 16 of the 25 counters and θ. Never asserted: `blocks_ored`,
  `max_score_block`, `max_score_group`, `max_score_term`, `windows`, `windows_skipped`,
  `rejected_by_bound`, `excluded_advances`, `included`; `skipped_terms` only as 0
  (`counters_test.cc:75`). Removing any of those counts passes the suite, and CR2's first three
  items are in that set. Every case is one segment and a single term; no union, conjunction,
  exclusion, `count()`, or multi-segment search runs counted.
- CI configures `STRATA_COUNTERS` off everywhere (`.github/workflows/ci.yml`), so
  `strata_engine`'s counting block (`strata_engine.cc:557-607`) compiles on no CI job and can
  break on gcc, FreeBSD, or arm64 unseen; the headers' counting code compiles only inside
  `counters_test`.
- Fix: cases for a union top-k (windows, skipped windows, the three `max_score` levels), a
  counted conjunction on one segment and on two (candidates + rejected = matches), a negated
  query, a `COUNT` over bitset blocks (`blocks_ored`, count windows), and a term ended by its
  bound (`skipped_terms` > 0); and a target that always builds `strata_engine.cc` with the option
  on (`strata_engine_counting`, not installed), so every CI job compiles it.

### CR5. E1's fit: what is counted, and three definitions to settle (for the lead and the architect)

E1's tooling list (`query-gap.md:1101-1118`) against the branch:

| E1 asks | Here |
|---|---|
| Blocks decoded by kind; groups decoded | Yes (CR2 item 4 for tails) |
| Advances in block, new block, new group | Yes; the definitions below |
| `max_score` resolutions by level, caller, and clause density | Level only: H4 and H7 need caller and density |
| Pairs evaluated per bound | Mixed (below) |
| Windows, total and skipped | Top-k only (CR2 item 2) |
| Candidates scored, rejected by a bound, collected, and whose block bound competes | The first three (CR1, CR2 item 1); H6's fourth is missing |
| Postings scored | Yes, the phrase scorer's excepted (ledger, Next) |
| H11's required visits; positions decoded and headers stepped (H13) | Missing (ledger, Next) |
| Excluded advances per included candidate (H12) | Yes, top-k paths (CR2 item 3) |
| Allocation counts through `Kronuz/allocators`; `poststats`' histograms | Not built (ledger, Next) |

Definitions to settle before E1 reads them:

1. `bound_evals` adds a memoized bound over all of a list's pairs (`search.hh:388-389`) to a block
   test that stops at the first pair that competes (`:435-436`), so `pairs_evaluated /
   bound_evals`, H2's level, is biased low by however many block tests there are. Two counters,
   or H2 restated over the first kind.
2. `advances_new_group` counts an unstarted iterator and a move into the tail as a new group
   (`reader.hh:554`), and an advance on an exhausted iterator counts as a move into a new block
   and a new group: probe, five such calls, five of each. H5's level reads these counters.
3. θ's definition (CR1).

K1 is not met; the ledger does not claim it.

### CR6. "Never in an A/B binary" has no mechanism on the timed path

- The option is off by default and `bench` warns on stderr, but `serve`, which the harness times,
  is silent in a counting build (`strata_engine.cc:208`), and the bench Makefile configures its
  build directory once (`bench/phase2/engine/Makefile:17`), so a directory once configured on
  stays on. The quick A/B's scorer happens to stop on a counting build's header line
  (`bench/quick/ab.py:28`), by accident rather than by rule.
- Fix: a counting build refuses `serve`, or prints its state at startup in both builds and the
  harness checks it.

### CR7. The ledger's identity evidence names the wrong platform and predates the merge

- The ledger records the identity on "Release, Apple clang, macOS arm64"; its objects
  (`strata-wt/query-counters/.scratch/base.o`, `off.o`, SHA-256 `11bc6dbe…deca`) are x86_64
  Mach-O, built at 14:34, before the merge `a0fdda3` (15:44). This review re-verifies it at the
  tip (above), with the same hash for the plain build.
- A trap for the next proof: `git archive` gives files their commit's time, so a reused build
  directory keeps older objects and two commits compare equal without a rebuild; this review's
  first comparison did exactly that and was redone from scratch (`objid.sh` now touches the
  sources and deletes strata's objects first).

### CR8. Smaller items

1. `counters.hh` includes `metrics.hh`, which strata gets only through `completion`'s pin
   (`9d7e724`, its `CMakeLists.txt:7-8`), as the design notes. Declaring `metrics` in strata's own
   `CMakeLists.txt` at that pin keeps the dependency strata's when `completion` changes its own.
2. `STRATA_COUNTERS` changes inline functions' bodies, so a program that defines it in some
   translation units and not others breaks the one-definition rule. True only of consumers
   today (strata is header-only and the tool is one unit); one line in `counters.hh` says so.
3. The new includes sit inside the system include block: `search.hh:30` between `<algorithm>`
   and `<array>`, and `strata_engine.cc:31-33`.

## Criteria

| Criterion | Met | Evidence |
|---|---|---|
| query-gap K1: E1's tooling reviewed and on the experiments' base | No | CR3 (not on main), CR5 (counters, allocations, `poststats` owed); the ledger's Next says so |
| Zero cost when off | Yes, at the tip | Every object identical to `d8d219c`'s, with and without `-march=native` |
| Counts exact on `counters_test` | Yes, for what it asserts | Passes in three builds; CR1 (θ's expectation), CR4 (what it asserts) |
| Counts sane at 100k | Yes, with CR2 | `check_invariants.py`; the smoke row reproduced |
| No data races | Yes | `probe_threads.cc` under TSan; `counters_test` under TSan |

## Verdict

**REJECT: zero cost when off holds and the counts are exact, deterministic, and per thread, but
the branch does not build on main (CR3), and two paths E1 measures count wrong (CR1, CR2).**

Blocking items:

1. CR3: merge main, port `counters_test.cc` to layers' API, and redo the object identity there,
   against main's objects.

Conditions, each with when it is due:

1. CR1 and CR2, in the same rework, since its re-check follows; before E1's counts in any case.
2. CR4's cases with them, and the always-built counting target, before E1's counts.
3. CR5's three definitions, the architect's ruling, before E1 states its verdicts (H2, H5, H1).
4. CR6 before E1 runs on a host with the harness's build directory.
5. CR7 and CR8 at the developer's convenience.

Design questions (for the architect):

1. θ: the k-th score at the end, or the collector's last threshold (CR1)?
2. Do `count()`'s windows count as windows for H16 (CR2 item 2), and do the tails walked for
   bounds belong to M8's attribution rather than `blocks_tail` (item 4)?
3. H2 over memoized bounds alone, or over block tests too (CR5 item 1)? H5's new group: is an
   unstarted iterator's first advance one (item 2)?

Not run: the counting tool on gcc, Linux, FreeBSD, or arm64 (no CI job builds it, CR4), and
the off build there (CI's eight jobs at `8ffede2` cover it, without an identity check); E1's own
runs (the reordered merged and 16-segment 5M indexes, all commands); any timing, since a counting
build is never timed; allocation counts and `poststats`, which do not exist yet; and the trial
merge's tests, since it does not compile.

## Re-check

- Date: 2026-10-19. Tip `b7823cd` (code as of `bdb072e`; the tip adds only the ledger), detached worktree
  `strata-wt/recheck-counters`. Fixes: `0aa306a` (CR3), `86d08c3` (CR1), `b1ce4f6` (CR2),
  `4489607` (CR4), `3deb9a0` (CI job), `bdb072e` (CR6 to CR8). Probes of `review-counters/.scratch`
  ported to layers' API (`.scratch/probes/` of this worktree) and rerun; no timing, no
  benchmarks. Release, Apple clang, x86_64, counting build; `counters_test` passes.

| Finding | Verdict | Evidence |
|---|---|---|
| CR1 | CLOSED | `search.hh:230` and `:241` observe `hits[k - 1].score` after the sort when exactly k remain; the heap path was already right (`:1362`). `probe_counters`: a 10-posting term at `TOP_10` and `TOP_5`, collector path and `live_scorer` heap path, θ 2.5706025401258588 and 4.039518277340636 on both (was 0 vs 2.5706); a floor with one hit gives the hit's score. Fix reverted (`git apply -R` of the `search.hh` hunk), `counters_test` fails at `:107` and `:123` (θ = 0, want 1), passes again restored |
| CR2.1 | CLOSED | `searcher.hh:226` counts the generic loop's rejection. `probe_counters`, `+a +b TOP_10_COUNT`: one segment matches 13,333 = 187 + 13,146; two segments 13,334 = 6,667 + 6,667 (was 6,667 + 0). `counters_test.cc:131-148` |
| CR2.2 | CLOSED | `search.hh:1373`: `count_windows`, its own counter (the review allowed "or its own counter, stated"). 100k `COUNT`, 40 queries: 1 to 25 windows per query (e.g. `plus size clothing` 25, `+vicenza +italy` 6), `windows` stays a top-k figure. `TOP_100_COUNT` shows both (`griffith observatory`: 23 and 1). H16 must read `count_windows`. `counters_test.cc:149-160` |
| CR2.3 | CLOSED | `search.hh:966-981` counts the fill's excluded advance and `included` by popcount, less the document the skip already counted; `:995-998` per candidate. 100k, four negated queries at `TOP_1000`, `TOP_100_COUNT`, `COUNT`: `included` equals the candidates (`TOP_1000`) or the result (`plus size -clothing`, `COUNT`: 1,762 = 1,762). Term minus term at `COUNT` reads 0 and 0 because it answers by `exact_count` (`:1477`) without walking the exclusion: correct, not a gap. `counters_test.cc:161-175` |
| CR2.4 | CLOSED | `reader.hh:1609-1611` moves the bound walk into `bound_tails` (only short terms reach it, `:446`). `probe_tail3`, 100k: `griffith` after the build 0 tails (was 1), after `top_k` 1 (was 2); the bench shows `blocks_tail` = the long terms' and `bound_tails` = the short ones'. Left as the ledger says: `tail_last()` is uncounted. `counters_test.cc:176-185` |
| CR3 | CLOSED | `counters_test.cc:35-40` uses `seg.get<inverted_plugin>()` and `weight::bm25(inv, …)`. Builds and passes at the tip. Object identity, Apple clang x86_64 Release, one source path, every unit compiled from scratch (`objid.sh`, `objcmp.sh`): the 34 strata objects of `bdb072e`'s code equal origin/main `69a3f8c`'s, 34 of 34 (`counters_test` the tip's 35th); `strata_engine.cc.o` `67ee80f5edb7` on both. Main is now `be64292`, whose inverter follow-ups postdate the merge, so the comparison is against the merged main, as the ledger states |
| CR4 | CLOSED | `counters_test.cc:186-233` adds the union, conjunction, term-ended, and negated cases. Mutation, mine, not the ledger's: for each of the 27 counters, every `STRATA_COUNT(name, …)` site disabled (`.scratch/mutate.sh`), `counters_test` rebuilt: 27 of 27 fail. CI job `linux-x86_64-gcc-counting` (`ci.yml:25-43`): builds `strata_engine` and `counters_test` with the option on, runs the test and a two-query bench smoke; run 39215775159 at `bdb072e`, all nine jobs green |
| CR5 | Not ruled | The architect's. θ is settled (CR1). Unchanged and still as probed: an advance on an exhausted iterator counts as a new block and a new group (5 calls, 5 and 5); `bound_evals` still mixes memoized and block-test evaluations |
| CR6 | CLOSED | `strata_engine.cc:746-751`: `serve` and `churn` exit 2 with a message in a counting build (run: both rc 2) |
| CR7 | CLOSED | The ledger names x86_64 and the post-merge identity (above reproduces it) |
| CR8 | CLOSED | `CMakeLists.txt:29-30, 35` declares `metrics` at `completion`'s own pin (`c975765`, `completion` `CMakeLists.txt:7`); `counters.hh:3-4` states the one-definition rule; `search.hh:27-29` and `strata_engine.cc:49-51` put the includes in the project and system blocks |

No new findings. Outside the scope, one line for the lead: in a union's counting mode, windows
skipped by their summed bounds count their documents without scoring them (`american funds`,
`TOP_100_COUNT`: 12,059 matches, 7,537 candidates, 4 rejected), so `candidates + rejected_by_bound =
matches` holds for conjunctions on one or many segments, not for unions; E1 should not assume it.

Verdict: CR1 to CR4 and CR6 to CR8 are closed; CR5 is the architect's. The code review's
blocking item and conditions are met.

Not run: gcc, Linux, FreeBSD, arm64 locally (CI's nine jobs at `bdb072e` cover them; the identity
check ran on Apple clang x86_64 only, and without `-march=native`); ASan and TSan locally
(CI's cover `counters_test` at `bdb072e`); `probe_threads` under TSan; E1's own runs; any timing.
