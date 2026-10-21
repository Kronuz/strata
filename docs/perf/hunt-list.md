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
| prospect phrases 1.10–1.21× slower than Tantivy | prospect | `format13-loss-attribution.md` loss 4 and its profile rows | positions reader, `phrase_freq` | `dump` over gate queries |
| prospect two-phase-critic COUNT, 9.1 ms | prospect | `format13-loss-attribution.md` profile table | the positional verifier | `dump` |
| `word_sum_`'s 1 KB per postings iterator on the many-indexes workload | prospect | review CR3 in `count-union-bits.md` | `reader.hh` | `dump`, and RSS with 1,000 open indexes |
| Multi-threaded rows (fits4) not yet run against RocksDB on format 14 | strata | `core-vs-rocksdb-f14.md` (fits4 omitted) | `bench/kv/run.py rows OUT fits4` | n/a: a measurement |
| termdict exact-size truncation sweep through cursor, seek and scan | termdict | review CR1 in `docs/reviews/seek-decode.md` | `test/test.cc` | existing tests; a test, not a speedup |
| prospect fuzz corpora still format-13 segments | prospect | the format 14 pin's report | `fuzz/` | replay tests |
| A stored section of 0–3 bytes passes `check()` (SR2) | strata | `docs/reviews/f14-step1-decoder.md` | `reader.hh:431,502` | a fix with a test |
| A merge over a damaged source fences the index instead of quarantining it (SR4) | strata | same review | `index.hh:1453-1459` | a fix with a test |

## Taken

See `exploration-log.md`.
