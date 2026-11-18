# Hunt list

Targets for explorers: each is one direct question with its evidence, starting files and the
check that must stay identical. An explorer takes one, proves a change or a cause, and stops; a
developer finishes wins. Results go to `exploration-log.md`. Add targets as benchmarks and reviews
find them; strike them when taken.

## Ready

| Target | Repo | Evidence | Start in | Must stay identical |
|---|---|---|---|---|
| Write buffer at ~290 B/record against 177 B designed | strata | `peak-memory.md` (explore/peak-memory) | `include/strata/core/` buffer and arena | key-order scan hash |
| Synchronous commit CPU 2.5–10× RocksDB's per op, beyond the barrier count | strata | `core-vs-rocksdb-f14.md` sync1 rows | commit path in `index.hh`, completion's sync | data after reopen |
| prospect phrases 1.10–1.21× slower than Tantivy (confirm on main first) | prospect | `format13-loss-attribution.md` loss 4 and its profile rows | positions reader, `phrase_freq` | `dump` over gate queries |
| prospect two-phase-critic COUNT, 9.1 ms | prospect | `format13-loss-attribution.md` profile table | the positional verifier | `dump` |
| `word_sum_`'s 1 KB per postings iterator on the many-indexes workload | prospect | review CR3 in `count-union-bits.md` | `reader.hh` | `dump`, and RSS with 1,000 open indexes |
| Multi-threaded rows (fits4) not yet run against RocksDB on format 14 | strata | `core-vs-rocksdb-f14.md` (fits4 omitted) | `bench/kv/run.py rows OUT fits4` | n/a: a measurement |
| termdict exact-size truncation sweep through cursor, seek and scan | termdict | review CR1 in `docs/reviews/seek-decode.md` | `test/test.cc` | existing tests; a test, not a speedup |
| prospect fuzz corpora still format-13 segments | prospect | the format 14 pin's report | `fuzz/` | replay tests |

## New from the coverage rows (2026-10-21, main f2d6107)

| Target | Repo | Evidence | Start in | Must stay identical |
|---|---|---|---|---|
| 10× RAM: concurrent flush and merge working memory peaks at 45 MiB against a 12 MiB budget (fuse-filter build, term-dictionary cut, segment write, merge output); by dirty footprint strata already beats RocksDB (40.5 vs 53.3 MiB readrandom), the RSS gap is clean pages and freed arenas (`docs/perf/rss-transient.md`) | strata | rss rows | stream the filter and term-dictionary builds; charge flush and merge draws to the budget | get digest |
| Linux: flushmem lowers `budget_peak` 40–44% but peak RSS is 4–7% higher, and a 1M-key merge's process RSS drops only 1–4% (glibc keeps freed arenas?) (`docs/perf/flushmem.md`, Linux section) | strata | flushmem rows on the Linux VM | allocator retention on glibc; `malloc_trim` cost there | get digest |
| ycsb_a v1024 at 10× RAM: disk 1.14× RocksDB's because reclaim merges fire only above 20% deleted index-wide (Lucene's default too); 10% gives 1.04× for 1.3× the bytes written, not adopted (`docs/perf/deadratio.md`). Open: a per-segment or dead-aware selection that costs fewer writes, judged under sustained load | strata | deadratio rows | merge policy selection | get digest |

## Parked (owner, 2026-10-26)

| Idea | Repo | Evidence | Why parked |
|---|---|---|---|
| Pair (bigram) postings for adjacent frequent terms, to reach Tantivy on phrase queries | prospect | `docs/perf/phrases-explore.md` on prospect `perf/phrases` (estimate only: +21% disk at df ≥ 1% covering 25% of gate phrases; +12% at df ≥ 3% covering 8%) | not by default: it trades disk, an axis strata must also win, for one query class, and neither Lucene nor Tantivy pays it by default; revisit only as a per-field opt-in |

## Taken

See `exploration-log.md`.

## Security (2026-10-27)

| Finding | Repo | Evidence | Plan |
|---|---|---|---|
| Vendored Xapian parser heap-buffer-overflow on `{"t_some":{"_near":"(barack OR michelle) AND obama"}}`, while unwinding from UnimplementedError (`TermGroup::as_group`, `yy_destructor`); reachable from any HTTP client | Xapiand | ASan, queryparser-ab T3 run | GPL code we do not read; queryparser replaces the parser at T4 (Q4), which removes it |
