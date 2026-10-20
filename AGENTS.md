# AGENTS.md — strata

The segment core of the Elysian stack: an MIT, async, replicated, LSM-style store, built phase
by phase under measured gates. Search is [prospect](https://github.com/Kronuz/prospect), a
layout on this core in its own repository. Read [docs/PLAN.md](docs/PLAN.md) for the phases and
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the design before changing anything.

## Repo map

```
include/strata/core/ strata::core (header-only): the lifecycle (index, segments, manifests,
                  merges, budgets, files), the keys and stored plugins, the document layout,
                  executors, faultfs. The only target; prospect links it.
test/             ctest suites: core_kv (the document layout as a key-value store), files,
                  faultfs, merge_policy, and the boundary checks (core_boundary,
                  core_includes, each with its negative).
fuzz/             libFuzzer target for the files layer and its corpus.
bench/            kv/ (the key-value driver against RocksDB, engines/rocksdb-11.11).
tools/            vm/ (the Linux gate VM and the FreeBSD VM).
cmake/            check_core_includes.cmake (core includes only core and its libraries).
docs/             PLAN, ARCHITECTURE, REPLICATION, DISTRIBUTION, TESTING, BENCHMARKS;
                  design/, gates/, reviews/, perf/, status/ (the lead's index in README.md).
docs/strata.d2    README diagram; render: d2 --layout elk --sketch --theme 302 docs/strata.d2 docs/strata.svg
```

prospect holds the search code, its tests, fuzz targets, `strata_engine`, `parity`, `keybench`,
the query and indexing harnesses, and the search records (designs, reviews, perf records, and
ledgers whose subject is search). Shared documents stay here: PLAN, BENCHMARKS, TESTING,
format-13.md, phase-2.md, reordering.md, layers.md, core-split.md, and
`docs/reviews/calibration.md`. `docs/status/repo-split.md` lists what moved and how to replay
in-flight search branches onto prospect.

## Rules

- **Clean room.** Never read Xapian's or Xapiand's GPL sources (including
  `sortable_serialise`). Xapian behavior is learned only from oracle vectors produced
  by a separate harness. Permissive implementations (Lucene, Tantivy, PISA) and any
  literature are open to read; code ported rather than reimplemented keeps its license
  notice.
- **The core names no search type.** A layout reaches the core through its plugins' hooks
  (`docs/design/core-split.md`); a change prospect needs in the core lands here first, and
  prospect repins to it.
- **One implementation per mechanism.** Compatibility with Lucene, Tantivy, Xapian, or
  RocksDB is a preset of one implementation, never a parallel code path.
- **Limits are load-bearing.** Every cap names the mechanism it protects, and is at
  least the largest of Lucene's, Tantivy's, and Xapian's.
- **Gates are measured.** A claim in a gate report carries its platform, commit, and
  trial count. Iterate on small samples (about 100,000 documents) against the fastest
  baseline; one larger run confirms a gate row (PLAN.md, "Targeted runs").
- **Stage explicit paths.** Never `git commit -a`; reviewers may be editing alongside.

## Verify a change

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build && ctest --test-dir build
```

A core change is not done until prospect's suite passes on it too: configure prospect with
`-DFETCHCONTENT_SOURCE_DIR_STRATA=<this checkout>`. Sanitizer and FreeBSD coverage come from
CI; `cpp-libraries/tools/preflight.sh` runs the same matrix locally.
