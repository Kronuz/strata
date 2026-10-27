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
| 10× RAM: peak RSS 146–246 MiB against RocksDB's 70–147 (1.6–2.1× larger; was 353–645); the 12 MiB budget covers only memtable and flush buffers, `budget_peak` 45 MiB; RSS is mostly freed malloc arenas kept resident (~128 MiB on readrandom) and mapped segments (`docs/perf/remeasure-2026-10-26.md`) | strata | remeasure rows | return transient read and merge buffers; draw them from the budget | get digest |
| ycsb_a v1024 at 10× RAM: disk 1.12× RocksDB's, ~12% dead documents in live segments below the merge trigger (queue and merge debt 0) | strata | remeasure rows | the merge dead-ratio trigger | get digest |

## Parked (owner, 2026-10-26)

| Idea | Repo | Evidence | Why parked |
|---|---|---|---|
| Pair (bigram) postings for adjacent frequent terms, to reach Tantivy on phrase queries | prospect | `docs/perf/phrases-explore.md` on prospect `perf/phrases` (estimate only: +21% disk at df ≥ 1% covering 25% of gate phrases; +12% at df ≥ 3% covering 8%) | not by default: it trades disk, an axis strata must also win, for one query class, and neither Lucene nor Tantivy pays it by default; revisit only as a per-field opt-in |

## Taken

See `exploration-log.md`.
