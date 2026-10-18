# AGENTS.md — strata

An MIT, async, replicated search engine for Xapiand, built phase by phase under
measured gates. Read [docs/PLAN.md](docs/PLAN.md) for the phases and
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the design before changing anything.

## Repo map

```
include/strata/   The engine (header-only): writer, reader, segment format, index,
                  merge and merge policy, search and scorers, executors, faultfs.
test/             ctest suites; *_mapped variants rerun under faultfs crash points.
fuzz/             libFuzzer targets and their corpora.
bench/            Gate benchmarks: engines/ (Lucene, Tantivy, Xapian harnesses),
                  phase0/, phase2/ (gate.py), quick/ A/Bs, soak/, overnight.sh.
tools/            strata_engine.cc (benchmark engine), parity.cc, coverage, vm/ (gate VM).
docs/             PLAN, ARCHITECTURE, REPLICATION, DISTRIBUTION, TESTING, BENCHMARKS;
                  design/ (per-phase designs), gates/ (measured rows), reviews/, perf/.
docs/strata.d2    README diagram; render: d2 --layout elk --sketch --theme 302 docs/strata.d2 docs/strata.svg
```

## Rules

- **Clean room.** Never read Xapian's or Xapiand's GPL sources (including
  `sortable_serialise`). Xapian behavior is learned only from oracle vectors produced
  by a separate harness. Permissive implementations (Lucene, Tantivy, PISA) and any
  literature are open to read; code ported rather than reimplemented keeps its license
  notice.
- **One implementation per mechanism.** Compatibility with Lucene, Tantivy, or Xapian is
  a preset of one implementation, never a parallel code path.
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

Sanitizer and FreeBSD coverage come from CI; `cpp-libraries/tools/preflight.sh` runs the
same matrix locally.
