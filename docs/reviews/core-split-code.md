# Code review: phase4/core-split (tip c5db03c)

Reviewer: code-reviewer, fresh context, 2026-10-20. Worktree: `strata-wt/review-core-split`
(detached at c5db03c). Branch is main d4fec9a plus 11 commits (82 files, +2614/-1754). One review,
one fix round.

## Verdict: APPROVE WITH CONDITIONS

No blocker. The split is real, behavior-preserving on everything I could byte-compare, and the new
core-only key-value path works. Three small fixes (CR1, CR2, CR3) and CI green at the landing tip (CR4)
are conditions; the rest are notes.

## What ran

| Check | Result |
|---|---|
| Release build (Apple clang, x86_64), `ctest -j8` | 42/42 (`.scratch/ctest-rel.log`) |
| ASan+UBSan, Homebrew clang 23, `-O1 -g`, asserts on, `-fno-sanitize-recover=undefined`, `ctest -j8` | 42/42 (`.scratch/ctest-asan.log`; macOS has no LSan, `detect_leaks=0`) |
| Output bytes, main vs branch, `strata_engine index` on a 100k-doc Zipf corpus, `STRATA_IDENTITY_SEED=42` | identical directories, threads 1 and 4, 3 rounds each |
| Same with `STRATA_BUFFER_MB=8 STRATA_DELETE_EVERY=7` (5 to 8 segments, flushes, auto-merges, 14286 deletes) | identical directories, threads 1 and 4 |
| 100k-doc build time, interleaved main/branch, 3 rounds, `quiet run` light | t1: 1.05/0.84/0.79 s main vs 1.04/0.82/0.80 s branch; t4: 0.62/0.62/0.61 vs 0.60/0.59/0.61. Unchanged within noise (`.scratch/timing.log`) |
| CI at c5db03c (run 39460730958) | 6 jobs green; freebsd-x86_64 red, linux-tsan cancelled (see CR4) |

## Findings

### CR1 (condition): the boundary check is weaker than the design states
`docs/design/core-split.md` "The boundary" says a core consumer cannot reach a search header and
links none of docset, bitpack, fst, analysis. Probes in `.scratch/boundary-probe/run.sh` (each
appends one line to a core header, compiles with core_boundary's exact flags, restores it):

1. `#include "strata/search/format.hh"` fails (the check works for this spelling).
2. `#include "../search/bisection.hh"`, `"../search/exact.hh"`, `"../search/ordered.hh"` from a
   core header **compile** (`rel_*`). The core root is a symlink (`CMakeLists.txt:45`), so the
   compiler resolves `..` physically and lands in `include/strata/search/`. The other 15 search
   headers fail only because they in turn include docset/bitpack/analysis; the wall is the library
   absence, not the path. `core_boundary_negative` tests only the absolute spelling.
3. Adding `docset`/`bitpack` to `strata_core`'s link line (`core_links_docset` probe: their include
   directories added to the flags, plus a relative search include) compiles, so no test notices
   core gaining a search-only library. The "links none of search's libraries" claim is held by the
   text of `CMakeLists.txt:49` alone.
4. `#include "fst.hh"` compiles in core: `termdict` links `fst::fst` publicly (termdict
   `CMakeLists.txt:13`), so `fst-src` is on core's include path. The design's list names fst as not
   linked; it is a transitive dependency of core.

Today nothing in `include/strata/core` includes a search header or a search library (grep clean), so
this is a gap in the guard, not a leak. Fix in this round: (a) a ctest that greps
`include/strata/core/*.hh` for any quoted include not starting `strata/core/` and not a listed core
dependency (catches `../`), (b) a configure-time check that `strata_core`'s transitive
`INTERFACE_LINK_LIBRARIES` contains none of docset, bitpack, analysis, unicode, snowball,
(c) correct the design sentence: fst arrives through termdict.

### CR2 (condition): `order_source<search_layout>` is a silent ODR trap
The reordering settings and order source are chosen by specializing `order_source<Layout>`
(`core/merge.hh:149`, specialization at `search/reorder.hh:244`). If a translation unit sees
`search_layout` and core's index without `search/reorder.hh`, `basic_index_options<search_layout>`
instantiates from the primary template: no diagnostic, no reordering, and a different class layout
than other TUs. Probe `.scratch/probes/odr_a.cc` + `odr_b.cc`: sizeof options is **224** without and
**256** with reorder.hh in the same program, and `order_source<search_layout>::exists` differs (0 vs
1). Every in-tree user includes `search/index.hh`, which brings reorder.hh, so nothing breaks today,
but the failure mode (a quiet no-reorder index, or a link-time-invisible ODR violation) is bad for a
shared layout. The other hooks fail loudly (an incomplete `inverter`, an undefined
`inverted_plugin::merge`). Fix: declare `template <> struct order_source<search_layout>;` in
`search/reader.hh` next to `search_layout` (`search/reader.hh:140`), so using it without the
definition is a compile error.

### CR3 (condition, small): core_kv_test does not do all it says, and `document_layout` is not in core
1. `test/core_kv_test.cc:4` and the design list a checkpoint and an install; the test takes
   `checkpoint()` and discards the generation (`:175`, `(void)gen` at `:184`) and never calls
   `install`. Probe `.scratch/probes/core_install_probe.cc` (core headers only, flags without any
   search include directory) shows both work on the document layout: install of the checkpoint
   gives the checkpoint's digest, drops the target's own content, `get` and `check()` agree, and a
   reopen keeps it. Fold that in; it is the part of the lifecycle that moves segment files
   wholesale.
2. The design table puts "the document layout" in core, but `document_layout` is defined nowhere
   under `include/` (`grep -rn document_layout include` finds nothing); `core_kv_test.cc:21` and
   `bench/kv/kv_strata.cc` each define their own `layout<keys_plugin, stored_plugin>`. Either add
   `using document_layout = layout<keys_plugin, stored_plugin>;` to `core/reader.hh` and use it in
   both, or reword the design. Preferred: the alias, so the core's one supported layout has one name.

Everything else the check list names is exercised and compared against a `std::map` model, 6 seeds
x 400 steps: put, conditional put (right and wrong version), batch rollback (second op fails, first
undone, checked through `get` afterwards), delete, `get` after every step, refresh, force_merge,
commit, close/reopen every fifth step, digest before and after commit and after a merge, `check()`
with `report.keys == model.size()`, the section set (`check_sections`, no search section). flush
threads 1 and 2 both run, and both take the whole-document buffer (`inverting()` is false for a
layout without an inverter).

### CR4 (condition): CI at the reviewed tip is not green
Run 39460730958 (c5db03c): macos-x86_64, macos-arm64, linux-x86_64-gcc, linux-x86_64-clang,
linux-arm64-gcc and linux-asan-ubsan green; **freebsd-x86_64 failed** and **linux-tsan was
cancelled**. The FreeBSD error is `bench/kv/kvdriver.hh:344: use of undeclared identifier
'CLOCK_MONOTONIC_RAW'`, a file this branch did not write (it came in with main's KV harness). The
branch's newer tip dc4750f (805331a plus a main merge) changes only `bench/kv/kvdriver.hh`
(+5/-1) against c5db03c, so everything I reviewed is in it. The previous run (39453003373, "core-split
done locally") was green on all 8 jobs including tsan, which covers the split itself. Condition:
land only when the run for the final tip is green on all 8 jobs, tsan included.

### Notes (non-blocking, follow-ups)

- **CN1, moves carried no code change** (item 2). The 22 files that moved whole (10 to core, 12 to search) differ from main
  only in include lines (checked line by line; `search/reorder.hh` gains `using settings =
  reorder::settings;`, `search/scorer.hh` drops `using docset::no_more_docs;`, which
  `search/reader.hh` now asserts equal to core's own with a `static_assert`). For the six split
  files, `.scratch/splitdiff.sh` takes the multiset of normalized lines of the old file against
  core+search of the new one and lists what is added or removed (`.scratch/sd_*.diff`, comments and
  includes filtered): every added line is a hook (`builder`, `configure`, `inverter`, `records`,
  `buffered`, `total_length`/`length`, `consistent`/`refuse`, `report`/`check`), a forward
  declaration, a namespace/brace, or the generic-writer plumbing. Bodies of `inverted_builder`
  (writer), `inverted_plugin::merge` (merge), the term/seed check (check) and the keys/stored
  builders are verbatim (diff of the class bodies shows only the added `configure`, `add(part)`,
  `finish(sink)`). I did not use `git diff --color-moved`; git cannot follow a 1100-line class that
  moved across a split, the line-multiset diff is the stronger check.
- **CN2, order of operations** (item 2). Differences, all unobservable in bytes or digest:
  1. The writer now writes the plugins' sections in layout order and the core's `docids` after
     all of them (`core/writer.hh:350-351`); main wrote `docids` between the inverted sections and
     keys. `docids` is a pinned section (`core/format.hh:178`), which `section_sink` holds and emits
     in rank order at `finish()` (`core/section_io.hh:370`), so the bytes cannot move; the 100k
     byte-compare above confirms it. The merge already wrote docids last on main
     (`core/merge.hh:287`, comment "after the plugins, off their peak (CR8)"), so there the
     "now" is not new; the flush path now matches it, which also moves the docids charge off the
     builders' peak.
  2. `begin()` in a search-layout segment writer no longer calls `inverted_.begin(length)` when
     the inverted structure is off (main did, charging a lengths column it never wrote). Saves
     memory for keys+stored-only search indexes; no bytes change.
  3. When a document violates two rules at once, which exception is thrown can differ: the
     stored-record checks (`refuse`) now run before the document-limit and docid-order checks.
  4. `check_segment` runs each plugin's pass in layout order (inverted, keys, stored) rather than
     one per-document loop then the term scan then the key scan; same reads, same counts, but a
     segment with two independent corruptions may report the other one first.
  5. The digest folds plugins in layout order; for `search_layout` that is inverted then stored,
     as before (keys has no `records`), so the digest value is unchanged; the buffered-document
     case follows the same order. A mutant that drops the `buffered` hook is caught by
     `index_test.cc:406` (`.scratch/mut/no_buffered_hook`); a mutant zeroing the manifest total
     length by `corrupt_index: manifest: totals` (`.scratch/mut/total_length_zero`).
- **CN3, `basic_index::get(key)`** (item 3). `core/index.hh:874-884` mirrors `lookup_key`
  (`:1628`) exactly: buffered copy through the version map first (a deleted or replaced buffered
  copy leaves the map, so a segment copy marked deleted is skipped), then segments newest first
  with the per-segment live bits. Tested with a buffered copy and a segment copy, replaces,
  deletes (buffered and in-segment), a failed batch's rollback, force_merge, commit and reopen in
  `core_kv_test`, and with the search layout's inverting buffer at `index_test.cc:186,189` (buffered
  and flushed). The search layout's delete/replace/merge/reopen path through `get` is not in the
  repo's tests; probe `.scratch/probes/search_get_probe.cc` (core_kv_test with `search_layout` and
  terms, flush threads 1 and 2) passes in Release and under ASan+UBSan. Mutants: `get` ignoring
  the segment's deleted bits fails `core_kv_test.cc:160`; reverse segment order survives, an
  equivalent mutant (only one live copy exists). `get` returns a copy of the record
  (`std::optional<std::string>`) and decodes the stored frame on every call; fine as an API, see CN5.
- **CN4, the four remaining search references** (item 5). Acceptable as listed; none blocks the
  repository split. Weights for the architect: (1) `core/format.hh:136-193` names search kinds,
  bits, owners and ranks by number: a new search section (a later phase) is then a core edit
  and a core release, which the repository split turns into a cross-repo change. The design's
  "one numbering space every plugin claims from (L1)" is the reason; an open registry would remove
  it, at a cost. A call for the architect, not a defect here. (2) termdict/fusefilter: real
  dependency of core; also drags in `fst` (CR1 item 4). (3) The manifest's `total_length` and
  `live_length` are on-disk fields (0 for the document layout); removing them from core later is
  a manifest format change. (4) `no_more_docs` is core's own and search asserts equality with
  docset's, which is the right way round.
- **CN5, bench/kv/kv_strata.cc**: the old driver's point get kept one decoded stored frame per
  segment id and reused it; `basic_index::get` allocates a frame per call, so point-read numbers
  from the new `kv_strata` are not comparable to the first quick-tier report's, which used
  `kv_strata_search`'s old path. State it when the harness is next run.
- **CN6, `NOTICE:4`** still names `include/strata/search.hh`; now `include/strata/search/search.hh`.
- **CN7, cross-layout open fails closed.** `.scratch/probes/cross_open.cc`: a search-layout index
  opened as the document layout throws "unsupported index: structure bit 0"; a document-layout
  index opened as the search layout opens and `check()`s. Neither misreads. A test of the first
  would pin it.
- **CN8, other branches.** The header moves conflict with every in-flight branch that includes
  `strata/*.hh` by old path (automata, f13-query, perf worktrees); merge order is the lead's call.

## Items from the prompt

1. Boundary: real for the shipped headers; guard weaker than stated, CR1.
2. No behavior change: yes (CN1, CN2), proven by byte-identical segments and manifests from main
   and the branch.
3. `get`: correct (CN3).
4. `core_kv_test`: exercises the list except install; checkpoint taken, not used (CR3).
5. Search references: acceptable (CN4).

## Probes and scripts kept (all under `.scratch/` in the review worktree)

`boundary-probe/run.sh` (+ its logs), `probes/{build.sh, mutant.sh, search_get_probe.cc,
cross_open.cc, odr_a.cc, odr_b.cc, core_install_probe.cc, gen_corpus.py, flags.txt,
flags_core.txt}` (flags files hold paths into `build-rel`, which I removed; reconfigure
`cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=Release` to reuse), `mut/*` (mutated copies of the
core and search include trees), `splitdiff.sh` + `sd_*.diff`, `timing.sh` + `timing.log`, and the
build/ctest logs.

## Not run

TSan locally (CI's linux-tsan job was cancelled at the reviewed tip; green at the previous commit),
Linux and FreeBSD builds (CI only), libFuzzer targets (only their replay tests ran), arm64 locally,
benchmarks beyond the 100k build comparison, the Xapian/Xapiand sources (not read). Verdict stands
for c5db03c; origin's dc4750f differs only in `bench/kv/kvdriver.hh`.
