# strata review: the layers refactor's code, steps 0 to 7b (C8)

- Date: 2026-10-18.
- Reviewed: `phase4/layers-refactor` at `d508158`, 31 commits over `origin/main` `32d1d52`
  (`git log origin/main..d508158`), in a detached worktree (`strata-wt/review-layers`).
  `origin/phase4/layers-refactor` and the lead's worktree are both at `d508158`; nothing is
  unpushed.
- Read first: `docs/status/layers-refactor.md` (steps 0 to 7b, deviations, R17 and R18's
  inventory), `docs/design/layers.md` revision 4, `docs/reviews/layers-design.md` (C5 to
  C8, R12 to R19), `AGENTS.md`, `docs/reviews/calibration.md`. Then the diffs of
  `include/strata/{reader,writer,merge,index,section_io,search,searcher,reorder,check}.hh`,
  `fuzz/fuzz_segment.cc`, `tools/`, and `test/`, each set against main's file.
- Calibration applied: CI checked on every platform for the reviewed commit; `HEAD`
  compared with `origin`; every new mechanism searched for an existing one (LC1, LC4).
- Built and run locally (macOS x86_64, under the host's light holds: `nice -n 19`, at most
  four jobs). Release builds of main (`git archive` of `32d1d52`) and the tip with the same
  compiler and pins; an ASan+UBSan build of the tip with Homebrew LLVM 23 (Debug,
  `-fno-sanitize-recover=undefined`, libc++ hardening `EXTENSIVE`). The probes and their
  outputs are in the worktree's untracked `.scratch/`: `bytes20k.sh`, `run_probes.sh`,
  `compare.py`, `asan_extra.sh`, and
  `probe/{verdicts,results,merge_charge,fixtures_check,move_probe,move_all}.cc`.
  `verdicts`, `results`, and `merge_charge` are compiled against both trees,
  `fixtures_check` against main, and the move probes against the tip (`move_probe` also
  against a mutant copy of its `include/`).
- CI: run 39109639725 at `d508158`, all eight jobs green (linux gcc, clang, arm64 gcc,
  ASan+UBSan, TSan; macOS x86_64 and arm64; FreeBSD).

## Verdict

**APPROVE WITH CONDITIONS.** The refactor changes no bytes and no results, and I found no
memory-safety defect. Nothing blocks. Four things must change before it lands:

1. The step-2 duplicate-kind "behavior change" never happened (LC1).
2. The merge path holds 48 bytes more per output document than main's merge, against
   L5 (LC2).
3. The R3 move test cannot catch the bug it exists for (LC3).
4. Dead code goes with the oracle at landing (LC4).

LC5 to LC10 are text, naming, inventory, and duplication items.

## What holds, with its evidence

- **Segments are byte-identical on the 20k corpus** (`bytes20k.sh`). I indexed the 20,000
  documents with main's and the tip's `strata_engine` in five settings:
  - a 1 MB buffer with auto-merge (4 segments);
  - deletes of every 7th document (2,858 deleted);
  - auto-merge off (20 segments);
  - four flush threads;
  - a 24 MB `STRATA_BUDGET_MB` with deletes of every 5th document.

  Each index was then force-merged twice, once reordered and once in docid order. In all
  15 states, every file (segments, manifests, live docs, metadata) is byte-identical
  between main and the tip. A second main run gave the same hashes, so the comparison is
  deterministic. The two binaries differ (md5 `c5709af5` against `8db8d88c`).
- **Results are identical** (`results.cc`). On all 15 index states these match exactly
  between main and the tip:
  - `digest()` and the `check()` report;
  - 1,500 queries in BM25 and BM25-Xapian: the stratified terms, single words by df band,
    `+a +b`, `a b`, `+a -b`, two- and three-word phrases, and three-to-six-word ORs;
  - each query's top 10 (docid and hex-exact score) and its counts by both paths.
- **The fixtures pin main's current bytes** (`fixtures_check.cc`, built against main). It
  recomputes the six segment fixtures and `digest.txt` with main's own writer, merge, and
  index, and all seven equal the checked-in files. The tip passes `fixtures_test` on every
  CI job.
- **The writer façade reproduces main's bytes outside the fixtures.** The six
  `fuzz_segment` bases (wide skip offsets, keyed, permuted) have equal xxh64 from both
  writers (`verdicts bases`). The merge probe's outputs are equal in docid order and when
  permuted with a record spill.
- **Opening damaged input never changes outcome** (`verdicts.cc`, `compare.py`). The
  probe runs each tree's own `fuzz_segment` transform and `exercise()` over 9,616 inputs:
  - the checked-in corpus (1,090);
  - 4,360 mutants of it;
  - 4,000 random inputs;
  - 166 structural edits: each fuzz base with each section removed, each kind
    duplicated, and four unknown kinds appended.

  No input's accept-or-reject outcome changed, and no build threw a type outside the
  three the harness names. Every type change, 345 in all, goes from a leaked
  `std::invalid_argument` (`fst: …`) or `termdict::corrupt` to `corrupt_segment: term
  index`, at the term-dictionary open (`reader.hh:277-283`). In the corpus alone it is
  33, not the recorded 29 (LC1).
- **ASan+UBSan:** see the last section.
- **The presence mask (7b, R12, R16).** The pieces are in place:
  - `present_` is initialized after `core_` and before `readers_` (`reader.hh:795, 858-860`);
  - the full mask fits 32 entries, and `static_assert` at `831` pins that;
  - `holds<P>()` takes a discarded `if constexpr` branch for a plugin outside the layout
    (`816-822`);
  - `contains<P>()` is `static constexpr` (`810-812`), and `fixtures_test.cc:98-107, 139-141`
    instantiates the guarded template both ways.

  No reader stores a pointer into `basic_segment`. `move_all.cc` destroys a heap-held
  source after the move, then reads through every reader under ASan+UBSan, and every
  read is clean:
  - `term`, postings, positions, and `length`;
  - `keys`' `find`, `scan`, and `version`;
  - `stored`;
  - a scorer's `top_k`;
  - `verify_all`;
  - the core's `docid`.

  The tip's test cannot show this; LC3 is why.
- **`merge_options` (7a).** All 15 `merge_options` initializations outside the oracle
  name their fields, including the index's `merge_layout` call (`index.hh:1127-1128`).
  Their order matches the declaration order (`merge.hh:226-234`).
- **Lifetimes in the merge path.** `merge_options` holds a span and a `string_view`. Every
  caller builds it inside the call expression, from objects that outlive the call. The
  index passes `order`, a local, and `ctx.key_prefix`, a `std::string` copy
  (`index.hh:915`). Each `doc_fields::key` views a string in the `keys` maps, which live
  until `merge_layout` returns (`merge.hh:259, 330`). The plugin merges read readers
  through pointers into segments the job owns (`merge.hh:360`).
- **The moved code moved verbatim.** From `class postings_iterator` to the end of
  `reader.hh`, main and the tip differ only in the reader type the iterators hold
  (`const segment*` against `const inverted_plugin::reader*`), plus the new `keys`
  `find` and `scan`.
- **No test lost an assertion.** Every changed test file has at least main's count of
  `CHECK`, `assert`, and `static_assert` calls.
- **R18's six items are built:**
  - the concept, asserted per entry (`reader.hh:760-779`);
  - disjoint kinds and `docids_section` at most once (`780-781`);
  - the stub `document_layout` instantiation (`concepts_test.cc:25-44`);
  - the digest's key check through `keys`' scan (`index.hh:569-575`);
  - the index's merge over its own `Layout` (`index.hh:1127`);
  - the empty flush as a failure (`fixtures_test.cc:143-150`).
- **Detent is unaffected.** Its CMake files do not reference strata. The branch changes
  no `FetchContent` pin: `CMakeLists.txt` only adds two tests.

## Findings

1. **LC1. The step-2 duplicate-kind rejection is not a behavior change, and its check is
   dead code.**
   - Where: `reader.hh:113-120` (the comment and the `seen` loop). `format::decode_toc`
     already rejects a kind listed twice, with `corrupt("duplicate section")`
     (`format.hh:239-246`). That check is unchanged on this branch and has been on main
     since `5d1639d`, an ancestor of step 2's `97d0ae6`.
   - The claim is recorded in four places: `layers.md:81` (R17's row), `layers.md:649-652`
     (L7), `docs/status/layers-refactor.md:33`, and `97d0ae6`'s message. Each says main
     "read its first or last entry". Main never did.
   - The R17 count is stale too. At this tip, against current main, the corpus has 33
     type changes in 1,090 inputs (26 `invalid_argument`, 7 `termdict::corrupt`), not 29
     in 1,089.
   - None of the 9,616 inputs changed type at the region-table `catch`
     (`reader.hh:138-143`): `decode_toc` rejects overlapping and out-of-range sections
     first. Every change comes from the dictionary.
   - Failure scenario: the full review was asked to rule on a change that does not exist.
     Meanwhile the open path carries a second, untestable copy of the TOC's duplicate
     rule, which 7c's per-kind table work must keep in step with `decode_toc`: one rule
     implemented twice.
   - Probe: `verdicts struct`. All 68 duplicated-kind inputs (every kind of the six bases,
     listed twice) are rejected by main and by the tip with the same message,
     `corrupt_segment: duplicate section`, raised before `reader.hh:119` runs.
   - Fix: delete the loop and its comment. Correct L7 and the R17 row to say step 2
     changes exception types only (33 corpus inputs, all at the term index). Correct the
     ledger's deviation the same way.

2. **LC2. The plugin merge holds 48 bytes per output document that main's merge did not,
   against L5's "The refactor adds no charge" (`layers.md:467`).**
   - Where: `merge.hh:315-319` reserves and charges `refs`, `docids`, and `fields` for
     every output document. These are L6's `order` and `fields`, which the design requires
     to be materialized. `docids[i]` repeats `fields[i].docid`. Main's merge held only the
     writer's columns.
   - Probe: `merge_charge.cc`, built against both trees. It merges the same three keyed
     inputs, with deletes (54,285 live documents), into a `flush_charge` over a
     `byte_budget`. The output bytes are equal, but the charge is not:

     | Order | Bytes held at the end, main → tip | Peak, main → tip |
     |---|---|---|
     | Docid order | 23,344,211 → 25,425,603 | 23,868,597 → 25,777,858 (+8.0%) |
     | Permuted, records spilled | 19,375,995 → 21,457,387 | 21,113,115 → 21,806,570 (+3.3%) |

     Both rows hold +2,081,392 bytes. That is exactly 48 × 54,285 minus main's `docids_`
     capacity (65,536 × 8). The charge reflects real allocations, not only accounting.
   - Failure scenario: L15's forced merge of the 5M index holds about 170 MB more until
     publication. Under a D17 budget with background merges, `byte_budget::pressure()`
     (`budget.hh`) turns true earlier, so concurrent indexing flushes sooner and writes
     different segments, and the budget's peak moves. Inline merges are unaffected: the
     budgeted 20k run was byte-equal. No test pins a merge's charge: `merge_oracle_test`
     passes no `memory` to either merge (`merge_oracle_test.cc:131, 135`), and
     `index_test` checks only lower bounds (`index_test.cc:1199-1263`). Step 5's merge rows
     are unmeasured (`docs/perf/layers-refactor.md`, step 5), so nothing caught it.
   - Fix, for the architect and the lead: shrink the extra memory (at least drop the
     duplicate docid, or keep `refs` only and derive fields per plugin), or record the
     measured cost in the step-5 row and in L5 and L6 with the architect's acceptance.

3. **LC3. The R3 move test cannot see a reader left pointing into the moved-from
   segment.**
   - Where: `test/fixtures_test.cc:123-142`. `segment first(file); segment
     moved(std::move(first));` keeps `first` alive until the block ends, so a stale
     pointer reads valid memory. The test is meant to catch exactly that bug
     (`layers.md` L3, "Movability").
   - Probe: `move_probe.cc` against a mutant `reader.hh` whose stored reader keeps
     `&c.core` and reads `core_->doc_count()`:
     - in the test's shape, the source still alive, it passes under ASan;
     - with the source heap-held, moved from, and destroyed before the read, ASan reports
       heap-use-after-free in `core_segment::doc_count()` called from
       `stored_plugin::reader::stored`.
   - Unmutated, the tip is clean in both shapes. This matters at 7c, where readers gain
     `open_context::structures` and more state.
   - Fix: hold the source in a `std::unique_ptr`, move from it, reset it, then run the
     reads through all three readers.

4. **LC4. Dead code to delete at landing, beyond L6's oracle.**
   - `test/merge_oracle.hh`, `test/merge_oracle_test.cc`, and their target
     (`CMakeLists.txt:74-76`), as L6 and the ledger's Next 4 say.
   - `segment_writer::add_document` (`writer.hh:806-810`) and `flush_from` (`829-836`).
     Their only callers are `test/merge_oracle.hh:147, 174`. With the oracle gone, they
     are a second way into the writer for a merge, beside `merge_layout`, which a future
     caller could take around the plugin path.
   - `core_segment::verify(offset, length)` (`reader.hh:174`): no caller.
   - `merge_context::version` (`merge.hh:58`): `merge_layout` never sets it and no
     plugin's merge reads it.
   - LC1's loop.
   - One spill, two ways in: the merge hands `positions` to the sink
     (`section_io.hh:248, 281-284`; `merge.hh:102, 351`), and the flush passes it to the
     builder directly (`writer.hh:826`). Keep one.
   - Probe: `git grep` at `d508158` for each name; the callers are as listed.

5. **LC5. The API is not source-compatible through the aliases after step 6, contrary to
   L14 and the Phase 3R paragraph** (`layers.md:885-887, 907-910`). For the architect.
   - Step 6 (`6681f75`) removed `segment`'s forwarding members: `term`, `stored`, `docid`,
     `version`, `length`, `may_hold_key`, `sections`, `scan_terms`, `find_term`,
     `complete_impacts`, `dictionary`, and `docid_ordered`.
   - It also moved `term_scorer`, `phrase_scorer`, `postings_iterator`,
     `positions_reader`, `statistics`, and `weight` from `const segment&` to `const
     inverted_plugin::reader&` (`search.hh`; `reader.hh:871`).
   - `index`, `document`, `operation`, `transaction`, and `merge_source` are now aliases of
     templates, so a forward declaration such as `class index;` stops compiling.
   - So the change is not additive. Every in-repository consumer is updated and built by
     CI, and `bench/` has no C++ consumer outside the CMake targets.
   - Failure scenario: Phase 3R group code written "against the aliases" before landing
     does not compile after it.
   - Fix (text): L14 names step 6 as a second deliberate break beside 7a, and the Phase 3R
     paragraph says such code uses `core()` and `get<P>()`.

6. **LC6. Stale text.**
   - `reader.hh:66` says `validate_context::key_prefix` is "the manifest's, until format
     13". Revision 4's R13 keeps it the index's in every format (`layers.md:352`).
     Failure scenario: 7c's author removes it from the context, against R13. Fix: "the
     index's (R13)".
   - `docs/design/reordering.md:99` names `segment::stored_frame`, which is now
     `stored_plugin::reader::stored_frame`.

7. **LC7. Two types named `merge_context`.**
   - `basic_index`'s private background-merge environment (`index.hh:909-917`) and the
     plugin merge context (`merge.hh:57-74`) share a name. Inside `basic_index` the nested
     one hides `strata::merge_context`.
   - Failure scenario: index code that builds a plugin merge context by its unqualified
     name gets the wrong type, and the compiler's error points at the wrong struct.
   - Fix: rename the index's, for example to `merge_env`.

8. **LC8. Design claims not built and missing from the ledger's deviations**, extending
   R18's inventory:
   - Per-plugin `check` (`layers.md:193, 590`): `check.hh:20-42` names the three readers.
   - The builders' `bytes()` (L5's `section_builder`; "step 3 reads the sum of the
     builders' `bytes()`", `layers.md:466, 897`): no builder has one. The D17 charge is
     incremental through `memory`, and it equals main's: the budgeted 20k run was
     byte-equal.
   - Format 12's `P` digest records come from the core walking `inverted`'s terms
     (`index.hh:561-568`), not from a plugin hook. The ledger's deviation names only `D`.
   - `open_context::version` is the binary's `format::version` (`reader.hh:837`), not the
     segment's header version, which L7 specifies (`layers.md:582`). The two are equal in
     format 12 because the core refuses any other version (`reader.hh:110`). At 7c, an
     N − 1 decoder selected by it would never be chosen.
   - Fix: list each, with the step that closes it.

9. **LC9. The digest walks every key term twice; unmeasured.**
   - The first walk is `index.hh:561-568`, for the `P` records. The second is
     `index.hh:569-575`: `keys`' scan, which is `scan_dictionary` plus one `find_term`
     and one postings walk per key (`reader.hh:1486-1500`). Main checked keys inside its
     single walk.
   - The merge's key collection is not slower than main's: main called `term()`, which
     also computes tail impacts.
   - The digest is off the query and indexing paths, so this is low.
   - Fix: none required. Record it with L15's merge and digest rows.

10. **LC10. The deleted-bit test now has three copies.** Main had one,
    `merge_source::live` (now `merge.hh:32`). The branch adds `live_bits::live`
    (`reader.hh:84`) and `merge_input::live` (`merge.hh:53`), each the same expression
    over the same bit vector. Failure scenario: format 13's live-docs change (or a
    word-size change) updates one and misses the others, and merges and lookups disagree
    on which documents are live. Fix: one `live_bits` that the other two hold.

## R17 and R18, the starting inventory

- **R17: confirmed, with corrections.** L7's rule holds: no input's outcome changed, and
  every type change ends at `corrupt_segment` (above). Step 2 changes one behavior, not
  two (LC1). The branch also makes one change that is not listed, and it is a fix: main's
  merge dereferenced an empty `optional` when a key term that `scan_dictionary` listed
  was not found again (main's `merge.hh`: `auto info = …->term(term); postings_iterator
  it(…, *info)`), which is undefined behavior. The tip throws `corrupt_segment: term index`
  (`reader.hh:1490`). Only a damaged dictionary whose checksums still pass reaches it.
  Outside the open path, step 5 changes one more behavior: the merge's memory (LC2). It
  changes no bytes in my runs, but under a budget it moves when concurrent flushes start.
- **R18: built,** as listed above. Add LC8's three items to the ledger's deviations.

## Blocking items

None.

## Conditions

Before landing:

1. LC1: delete the dead loop; correct L7, the R17 row, and the ledger (one behavior
   change; 33 corpus inputs).
2. LC2: shrink the merge's extra memory, or record the probe's numbers in
   `docs/perf/layers-refactor.md` and in L5 and L6 with the architect's acceptance.
3. LC3: make the move test destroy its source before reading.
4. LC4: delete the oracle, its target, `add_document`, `flush_from`,
   `core_segment::verify`, and `merge_context::version`.
5. LC5 and LC8: correct the design text and the ledger. LC6, LC7, and LC10 can ride with
   condition 1.
6. C5 (rows open; its holds `layers-c5-count` and `layers-c5-rss` were running during
   this review), C6 (the lead's per-commit run), C7, and N4 stand as the architect set
   them. On C7: main's `ci.yml` now caps the ASan job at 120 minutes instead of splitting
   it, and the job took 36 min 38 s at `d508158`. Whether that meets C7 is the
   architect's call. C8's other half is the architect's full design review.

## ASan and UBSan

I ran the 15 tests in the tip's ASan+UBSan build (`ctest -R`, log in
`.scratch/ctest-asan.log`):

| Test | Result |
|---|---|
| concepts | passed |
| segment | passed, 1,877 s |
| docids | passed |
| reorder | passed |
| fixtures | passed |
| merge | passed |
| merge_oracle | passed |
| multisegment | passed, 710 s |
| index | passed, 469 s |
| background_merge | passed |
| cost | passed |
| search | passed, 649 s |
| index_mapped | passed, 700 s |
| background_merge_mapped | passed |
| fuzz_segment_replay | passed, 1,369 s |

All 15 passed (ctest: 100%, 3,494 s), and none reported an ASan or UBSan error.

Also under ASan+UBSan:

- **The 20k corpus through the ASan build of `strata_engine`** (`asan_extra.sh`), with
  deletes every 7th and with a 24 MB budget and deletes every 5th. Each was indexed and
  force-merged both ways. Every run exited 0 with no report. All six states are
  byte-equal to the Release outputs, which come from a second compiler (Homebrew clang 23
  Debug against Apple clang Release).
- **`verdicts` over the corpus, the structural edits, and the mutants** (5,616 inputs): no
  report, and verdicts identical to the Release tip's.
- **`move_all`** (every reader after the source is freed): clean.

The strict holds `scale-merge` (16:03 to 16:18) and `reorder-speed` (from 16:20) arrived
during these runs. I paused mine with `SIGSTOP` and resumed them once `quiet wait --light`
returned.

Not run:

- a local full `ctest` in Release (CI's eight jobs ran it at `d508158`);
- TSan (CI's TSan job is green at `d508158`);
- the libFuzzer targets (only corpus replay and my mutation and random replays);
- the 4,000 random inputs under ASan (they ran in Release only);
- C6's per-commit builds (the lead's);
- any benchmark or timing;
- Linux or FreeBSD locally, and the gate VM;
- Xapian and Xapiand GPL sources (none read).
