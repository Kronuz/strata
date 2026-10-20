# repo-split: search moves to Kronuz/prospect

- Task (the lead's, 2026-10-20): move strata's search side into its own repository,
  [Kronuz/prospect](https://github.com/Kronuz/prospect) (PLAN, "Names"), with history; strata
  keeps the core. Design: `docs/design/core-split.md` (the targets, hooks, and boundary tests).
- Branches: strata `phase4/repo-split` (worktree `strata-wt/repo-split`), prospect
  `phase4/repo-split` (checkout `~/code/Kronuz/prospect`; prospect's `main` is its root commit
  `752c418`, pushed only to make `main` the default branch), cpp-libraries
  `catalog/strata-prospect`.
- State: all three branches pushed; strata's 9 tests and prospect's 36 pass on macOS (45, as
  before the split); CI running on strata and prospect (see "CI").
- Land order: prospect `main` first (fast-forward to its `phase4/repo-split`), then this branch
  once prospect's CI is green. Land this branch with a merge commit, not a rebase: prospect pins
  strata at `1453626` (this branch's removal commit), which must stay reachable from `main`. Then
  the catalog branch.

## What moved where

| From strata | To prospect | Why |
|---|---|---|
| `include/strata/search/` | `include/prospect/`, namespace `prospect`, target `prospect::prospect` | the search target |
| the 28 search test programs, `test/skip_layout.hh`, `test/stepping_executor.hh`, `test/order_source_negative.cc`, the search fixtures (`*.sx`, `pre-d2/`, `digest.txt`, `seeds.txt`, `pre_freeze.txt`, and copies of the `.bin` files `fixtures_test` checks) | `test/` | they build search layouts; the lifecycle tests (index, lifecycle, reclaim, directory, media, merge, churn, background merges, and their `_mapped` runs) index through the search layout |
| `fuzz/fuzz_segment.cc`, `fuzz/fuzz_postings.cc` and their corpora; `fuzz/replay.cc` (copied) | `fuzz/` | search decoders |
| `tools/strata_engine.cc`, `tools/engine_query.hh` | `tools/` | the harness's engine: index, serve, verify, bench over the search layout |
| `tools/parity.cc` | `tools/` | oracle ranks: scoring |
| `tools/keybench.cc` | `tools/` | T5's key lookups read a search-layout index built by `strata_engine`; a search index opened as the document layout is refused |
| `tools/coverage.sh`, `tools/coverage_union.py` | `tools/` | coverage over the search tests, `parity`, and `verify`; `--headers /include/strata/core/` measures the fetched core |
| `tools/vm/` | `tools/vm/` (copied with history) | both repositories run the gate and FreeBSD VMs |
| `bench/` except `kv/` and `engines/rocksdb-11.11/` | `bench/` | query and indexing harnesses, the Lucene, Tantivy, and Xapian engines, soak |
| `NOTICE`, `LICENSES/` | same | the Lucene port is `prospect/search.hh`'s |
| design `phase-4.md`; gates `phase-2.md`, `phase-3-ratchets.md`; perf `bm25-presets`, `format13-*` (gate VM, landing, loss attribution and its patches, query items, seeds cost), `inverter`, `p6-multisegment`, `p6a-*`, `p7-*`, `p12-*`, `query-gap-merged`, `query-wins`; reviews `f13-query-code`, `inverter-*` (3), `p6-multisegment`, `phase-2-gate`, `phase-4-design`; status `f13-gate`, `f13-preflight`, `f13-query`, `inverter`, `inverter-followups`, `query-wins`; `testing/parity.md` | `docs/` | their subject is search |

strata keeps the core, its tests (`core_kv`, `files`, `faultfs`, `merge_policy`,
`fuzz_files_replay`, `core_boundary` and `core_includes` with their negatives), `bench/kv` and
RocksDB's driver, `tools/vm`, and the shared documents: PLAN, ARCHITECTURE (trimmed; the read
path, search sections, and text analysis went to prospect's), BENCHMARKS, TESTING, REPLICATION,
DISTRIBUTION, FEASIBILITY, `design/phase-2.md` and its review (the segment frame, cited by
`format.hh`), `design/reordering.md` and its review (the docids section and order source, cited by
the core), `perf/p15-region-size.md` (cited by `format.hh`), `design/format-13.md` with its reviews
and ledger (the format spans both), layers, core-split, the lead's `status/README.md`, and
`reviews/calibration.md`. `kv_strata_search` (the search layout as a key-value store) is dropped;
it needs prospect.

## History check

prospect was cut with `git filter-repo --paths-from-file` over every path the moved files ever
had (`git log --follow` per file, plus the pre-split flat headers and the
`include/strata/core/{format,reader,writer,index,merge,check}.hh` the split copied from, removed
again in `d804951`), then a pure rename commit (`c5111d9`). 455 of strata's 713 commits carry
over, all authored by Germán Méndez Bravo. `git log --follow include/prospect/reader.hh` reaches
88 commits back to 2026-10-11 ("Add the segment writer and reader"); `search.hh` 48.
`git blame -C -C include/prospect/reader.hh` attributes its lines to 62 commits back to
2026-10-11 (plain blame stops at the split's copy: 3 commits).

## Identity proof

`strata_engine index` over the 100,000-document Wikipedia sample (`corpus-100000.json`, sha256
`0a1d53444fbba789...`, the sample the worktrees share), `STRATA_IDENTITY_SEED=42`, built from
strata main `3f88a15` and from prospect `5fc4171` against strata `1453626`, Release, Apple clang
17, macOS x86_64. `diff -r` of the index directories:

| Run | Files | KiB | Result |
|---|---|---|---|
| 1 thread | 4 | 42,124 | identical |
| 4 threads | 7 | 45,272 | identical |
| 1 thread, then `merge` (one segment, docid order) | 5 | 42,128 | identical |
| 4 threads, then `merge` (reordered by bisection) | 9 | 86,300 | identical |

The comparison can fail: seed 43 differs from seed 42 in `manifest-2` and `seg-1.sx`. In CI,
`fixtures_test` (golden segment bytes) and `files` (golden manifest and metadata bytes) hold
identity on every platform.

## Tests

| Repository | Tests (macOS) | Notes |
|---|---|---|
| strata | 9 | the same on Linux |
| prospect | 36 | Linux adds `directory_tmpfs` (37) |
| Total | 45 | strata main before the split: 45 (46 on Linux) |

## CI

- strata `phase4/repo-split` at `3d3fd9b`: run 39481629332, in progress when recorded.
- prospect `phase4/repo-split` at `b22d7cc`: run 39481771140, in progress when recorded.
- The lead watches both to a result; docs-only commits (this ledger) start no run.

## In-flight branches that touch search

From `git branch -r`, each diffed against `main` from its merge base. Every one forked before
the core split (`298fc91`), so each first comes up to `3f88a15` in strata, through the split's
moves (flat `include/strata/X.hh` became `strata/core/X.hh` or `strata/search/X.hh`, and six files
were split at the plugin seam), before any part leaves.

| Branch | Ahead | Touches | Where it goes |
|---|---|---|---|
| `phase5/automata` | 14 | new `automata.hh`, `expand.hh`, their tests, `fuzz_automata` and corpus; design, review, ledger | prospect, whole; `design/limits.md`'s change stays in strata |
| `perf/query-exp` | 28 | new `counters.hh`; `reader.hh`, `search.hh`, `searcher.hh`; `bench/quick` (`qgbench.cc`, `ab*`), `test/fixtures/ab`, `strata_engine`, `engine_query.hh`; 26 perf docs | prospect, whole |
| `perf/query-counters` | 14 | `counters.hh`, `reader.hh`, `search.hh`, `searcher.hh`, `counters_test`, `strata_engine` | prospect, whole (`query-exp` holds the same files) |
| `design/query-gap` | 17 | `bench/quick/ab*`, `test/fixtures/ab`; design, review, ledger | prospect, whole |
| `perf/reorder-speed` | 9 | `bisection.hh`, `reorder.hh`, their tests, `strata_engine` | prospect; its `design/reordering.md` change stays in strata |
| `research/indexing` | 17 | `research/indexing/` (includes the search layout's `strata/index.hh`); research note | prospect |
| `phase3/bisection-pool` | 4 | `bisection.hh`, `reorder.hh` (search); `executor.hh`, `pool_executor.hh`, `index.hh` (core); tests, `strata_engine` | split: the executor and index changes to strata first, the rest to prospect |
| `perf/small-indexes` | 107 | `budget.hh`, `format.hh`, `index.hh` (core); `tuple_buffer.hh`, `writer.hh` (search); tests, `strata_engine`, the many-index harnesses with Lucene's and Tantivy's `Many` | split |
| `perf/indexing-gap` | 36 | `budget.hh`, `index.hh` (core); `tuple_buffer.hh`, `writer.hh`; tests, `strata_engine`, `bench/index_*` | split |
| `perf/inverter` | 78 | `budget.hh`, `index.hh`; `tuple_buffer.hh`, `writer.hh`; tests, `strata_engine`, CI | split; the inverter landed through `phase4/inverter-followups`, so check what is left |
| `perf/encode-once` | 70 | as `perf/inverter` | split; its last commit says "encode-once removed", so likely nothing to replay |
| `perf/index-size` | 55 | `index.hh`, `merge_policy.hh`, `merge_policy_test` (core); `strata_engine` | strata, except the `strata_engine` change |
| `perf/scale-threads` | 90 | `index.hh` (core); `index_test`, `strata_engine` | split: the core change to strata, the test and tool to prospect |
| `research/no-stalls` | 10 | `index.hh`; `research/no-stalls/` (the core's ingest against RocksDB) | strata; port its programs to `strata/core/index.hh` |
| `design/durability-modes` | 83 | docs; `bench/durability/`, whose tools include the pre-split `strata/index.hh` and `strata/format.hh` | strata; a tool that needs the search layout (`term_value_check.cc` reads term values) goes to prospect |
| `design/peer-transport`, `design/simulator` | 18, 21 | docs; `sim/` | strata; no search |

Fully on `main` (nothing to replay; delete when the lead decides): `perf/core-vs-rocksdb`,
`perf/f13-gate`, `perf/f13-preflight`, `perf/inverter-on-layers`, `phase4/core-split`,
`phase4/f13-query`, `phase4/f13m/*` (8), `phase4/format13`, `phase4/format13-on-main`,
`phase4/inverter-followups`, `phase4/layers-refactor`, `research/rocksdb-spec`.

### How to replay one onto prospect

1. In strata, bring the branch to `3f88a15` (`git rebase 3f88a15`, or merge it), resolving the
   core split's moves.
2. Core paths stay in strata: rebase the branch's core commits onto strata `main` after this
   branch lands (drop the search paths with `git rm` where a commit touched both). Land them
   first; prospect repins `GIT_TAG` to that commit.
3. Search paths go to prospect, with their commits:

   ```sh
   SEARCH="include/strata/search test fuzz tools bench docs \
     :!test/core_kv_test.cc :!test/files_test.cc :!test/faultfs_test.cc :!test/merge_policy_test.cc \
     :!test/fixtures/core_includes_bad :!fuzz/fuzz_files.cc :!fuzz/corpus/files :!tools/vm \
     :!bench/kv :!bench/engines/rocksdb-11.11"
   git -C ~/code/Kronuz/strata format-patch --stdout 3f88a15..BRANCH -- $SEARCH \
     | ~/code/Kronuz/prospect/tools/replay_strata_patch.py > replay.patch
   git -C ~/code/Kronuz/prospect switch -c BRANCH origin/main
   git -C ~/code/Kronuz/prospect am -3 replay.patch
   ```

   `replay_strata_patch.py` redoes the split's rewrite (paths, includes, the headers' namespace,
   `strata::` to `prospect::` for prospect's names, `PROSPECT_FIXTURES`/`PROSPECT_FUZZ`). Hunks on
   the sorted include blocks, and on `format::` in tests that say `using namespace prospect`, are
   fixed by hand. Drop the shared docs from the patch (`docs/PLAN.md`, `design/limits.md`,
   `design/reordering.md`, ...) and commit those to strata.
4. Build prospect against the core part (`-DFETCHCONTENT_SOURCE_DIR_STRATA=...`), run both
   suites, then push both branches.

## Done

- `1453626` strata: search removed; `strata::core` is the only target (`strata::search`,
  `strata::strata` and the linked core include root are gone); the search-only libraries are no
  longer fetched, and the link check still refuses them; `kv_strata_search` dropped;
  `core_boundary_negative` now looks for `prospect/format.hh`.
- `3d3fd9b` strata: README, AGENTS, ARCHITECTURE, PLAN's Names, BENCHMARKS, TESTING,
  core-split.md, the perf index, and the README diagram say where search went.
- prospect (`https://github.com/Kronuz/prospect`): `d804951` (core headers and shared docs
  left to strata), `c5111d9` (the rename), `5fc4171` (namespace and target, every caller
  migrated, no alias), `acd4ca2` (README, AGENTS, ARCHITECTURE, NOTICE), `b22d7cc` (the replay
  tool).
- cpp-libraries `5d7f459` on `catalog/strata-prospect`: strata and prospect entries, a "Pick by
  task" row each, and the layering line.

## Next

- The lead: land prospect `main`, then this branch (merge commit), then the catalog branch.
- The core's lifecycle is proven through the search layout: the crash, reclamation, directory,
  media, and background-merge suites moved to prospect with it. Port them to `document_layout`
  so strata proves its own lifecycle in its own CI (today `core_kv` alone, plus prospect's
  suite whenever it repins).
- Owner: rename `strata_engine`, the harness engine `engines/strata`, and the `STRATA_*` knobs
  once the gate harness and its records move to prospect (kept for now; prospect's AGENTS.md says
  why).
- Found, not introduced: `prospect/scorer.hh` (the `scorer` concept) and `prospect/search.hh`
  (the virtual `struct scorer`) cannot share a translation unit; they could not as
  `strata::scorer` either, and no unit includes both.

## Blocked on

- Nothing.
