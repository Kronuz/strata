# Benchmark coverage: what the upstream engines measure, what we run

Decides which benchmarks we add this week and feeds the explorers' hunt list. Written
2026-10-21 from the sources below; nothing was compiled or run. Run times are **estimates**
scaled from measured medians in `docs/perf/core-vs-rocksdb-0021/raw.jsonl.gz` (one run is
0.35-3.7 s at 100k records; a row is 5 trials x 2 engines), except where a measured session
time is cited.

Columns: **Run?** yes / partly / no. **Feature**: exists / Phase 4 / Phase 5 / turn log / not
planned. **Cost**: flag (run.py or driver option), workload (new driver workload), harness
(harness change), corpus (new corpus).

## Sources

Licenses allow porting definitions with notices (RocksDB: GPLv2 or Apache-2.0, port under Apache-2.0).

| Tag | Source | Revision read |
|---|---|---|
| dbb | `tools/db_bench_tool.cc`, https://github.com/facebook/rocksdb/blob/main/tools/db_bench_tool.cc | main `074134d` |
| bsh | `tools/benchmark.sh`, https://github.com/facebook/rocksdb/blob/main/tools/benchmark.sh | same |
| reg | `tools/regression_test.sh`, https://github.com/facebook/rocksdb/blob/main/tools/regression_test.sh | same |
| wperf | https://github.com/facebook/rocksdb/wiki/Performance-Benchmarks | fetched 2026-10-21 |
| wrmw | https://github.com/facebook/rocksdb/wiki/Read-Modify-Write-Benchmarks | same |
| wtools | https://github.com/facebook/rocksdb/wiki/Benchmarking-tools | same |
| lu | https://github.com/mikemccand/luceneutil (`tasks/*.tasks`, `src/python/nightlyBench.py`, `nrtPerf.py`, `constants.py`), Apache-2.0 | `f77ba1c` |
| lun | nightly charts, http://blog.mikemccandless.com/lucenebench/ (not fetched; chart list taken from `nightlyBench.py:1687-1810`) | |
| sbg | https://github.com/quickwit-oss/search-benchmark-game (`README.md`, `queries.txt`, `commands.txt`), MIT | `3347d5c` |
| tv | https://github.com/quickwit-oss/tantivy `benches/`, `columnar/benches/`, `sstable/benches/`, `Cargo.toml:193-245`, MIT | `6788926` |
| pisa | https://github.com/pisa-engine/pisa `tools/`, `benchmarks/`, Apache-2.0 | `b90f379` |

Ours: `docs/BENCHMARKS.md`, `docs/perf/core-vs-rocksdb-0021.md`, `bench/kv/run.py`,
`bench/kv/kvdriver.hh`, `bench/kv/kvgen.hh`, `bench/kv/kv_strata.cc`,
`bench/engines/rocksdb-11.11/kv_rocksdb.cc`, `docs/design/rocksdb-survey.md` section 4,
prospect `docs/gates/phase-3-ratchets.md`, prospect `bench/phase2/gate.py`.

## What the harness does today (facts the tables rely on)

1. Driver workloads: fillseq, fillrandom, overwrite, readrandom, readmissing, readseq,
   seekrandom (seek then 10 nexts), YCSB A-F (`kvgen.hh:185`, streams `kvgen.hh:241-264`).
   Op kinds: put, get, update, insert, scan, rmw (`kvgen.hh:175`). There is no delete,
   no reverse scan, no batched get, no rate-limited writer, no duration mode (only
   `--max-seconds` as a cap, `kvdriver.hh:232`).
2. Sets (`run.py:49-60`): `fits1` (13 rows, 100 B for db_bench rows, 1 KiB for YCSB, 256 MiB
   budget), `twox1` (budget = half the data, 5 MiB for 100 B), `sync1`, `wal1`, `fits4`
   (fits1 at 4 threads without readseq), `many` (1,000 indexes, readrandom). Data is
   `RECORDS = 100_000` (`run.py:30`), i.e. 11.6 MB at 100 B: "2x" is a 5 MiB budget on a
   small file, never a big file.
3. Threads are not symmetric: RocksDB's T threads share one DB (`kv_rocksdb.cc:2`), strata
   opens one index per thread (`kv_strata.cc:64`), and a strata scan reads only its own thread's
   shard (`kv_strata.cc:97`). `fits4` is a shared-nothing-vs-shared row; the fairness audit
   (`rocksdb-fairness-audit.md:162-166`) accepts it for put/get rows.
4. strata index API today: `delete_key` exists (`index.hh:49`, handled `index.hh:2126,2157`),
   `force_merge(n)` exists (`index.hh:586`), forward `scan(from[, to][, n], f)`
   (`index.hh:1012-1031`), `get` (`index.hh:954-965`). Scan is owner-thread only and races
   with a writer on another thread; "get has no such state and is safe"
   (`index.hh:1003-1005`). No reverse scan, no batched get (`rocksdb-survey.md:177`, M9).
5. Search classes run (prospect `phase-3-ratchets.md`): term, intersection, union,
   intersection_union, phrase, negated, global, each as TOP_10, TOP_100, TOP_1000, COUNT,
   TOP_100_COUNT, over the search-benchmark-game corpus and queries (181 gate queries, 40
   stratified terms in the quick tier, `BENCHMARKS.md` Tiers). Indexing 1 thread keyed, disk
   after merge, serving memory: run. Multi-thread and replace-heavy indexing, write
   amplification, refresh latency: owed (`phase-3-ratchets.md:372`).

## Suite 1: RocksDB (strata core, key-value)

Upstream defaults. `bsh`: 20 B keys, 400 B values, 8 KiB blocks, zstd, 16 GiB cache,
`NUM_THREADS=64`, sync on for multi-thread writers, `seek_nexts=10`, `NUM_KEYS` default 8e9
(`bsh`, "const_params_base"). `wperf` run: AWS m5d.2xlarge (8 CPU, 32 GB, NVMe), 900M keys,
cache 6 GiB, `DURATION=5400`, `MB_WRITE_PER_SEC=2`, sequence bulkload, readrandom,
multireadrandom (`--multiread_batched`), fwdrange, revrange, overwrite, readwhilewriting,
fwdrangewhilewriting, revrangewhilewriting, each also with direct I/O. `reg`: load
`fillseq,compactall` of 1G keys, 100 B keys, 900 B values, 1 GiB cache, 16 threads, then
seekrandom_asyncio, multireadrandom_asyncio, readrandom, readwhilewriting, deleterandom,
seekrandom, seekrandomwhilewriting, multireadrandom (multiread batch 128, stride 12).
`dbb` default: `num` 1M, key 16 B, value 100 B (`dbb:318,380,440`).

| Workload | Measures | Upstream setting | Run? | Feature | Cost |
|---|---|---|---|---|---|
| fillseq | Ordered ingest | bsh `fillseq_disable_wal` / `_enable_wal`: 1 thread, vector memtable; reg load | yes (fits1, twox1) | exists | none; add WAL-on row to `wal1` (flag) |
| fillrandom | Random ingest, write stall | dbb default 1M x 116 B | yes (fits1, twox1, sync1, wal1) | exists | none |
| filluniquerandom | Unique-key random load | bsh `filluniquerandom`, 1 thread | yes (our fillrandom is a permutation, `kvgen.hh:222-231`) | exists | none |
| fillsync | One fsync per write, N/100 ops | `wtools` | yes (sync1 fillrandom batch 1) | exists (turn log for group commit) | none |
| fillbatch | Write batches | `dbb` `batch_size` | partly (sync1 batch 100 only) | exists | flag: batch rows in `wal1` and `fits1` |
| overwrite | Random puts over existing keys, write amp | bsh `overwrite`, NUM_THREADS, sync; wperf 900M keys | yes (single thread; fits4 stale) | exists | flag |
| updaterandom | Read then write, same key | `wrmw`: 50M ops, 8 B values, disable_wal | partly (ycsb_f is the same shape, Zipfian not uniform) | exists | flag: uniform rmw variant, workload |
| mergerandom / readrandommergerandom / readwhilemerging | Merge operands (uint64 add) | `wrmw`; bsh `mergerandom`, `readwhilemerging` | no | not planned until K7 (`rocksdb-survey.md:212`) | n/a |
| readrandom | Point get hits | bsh NUM_THREADS; wperf; reg | yes | exists | none |
| readmissing | Point get misses (filter) | dbb | yes | exists | none |
| readseq | Full scan | dbb | yes (1 thread; fits4 excludes it) | exists | none |
| readreverse / revrange | Reverse full scan / seeks | bsh `revrange`, wperf, `dbb` `reverse_iterator` (`dbb:397`) | no | not planned (no reverse scan, `index.hh:1012-1031`) | n/a; owner question |
| seekrandom / fwdrange | Seek + 10 nexts | bsh `fwdrange`: `seek_nexts=10`, NUM_THREADS (`dbb:387`) | partly (1 thread; fits4 stale) | exists | flag: fits4 |
| multireadrandom | Batched gets | bsh `--batch_size=10`; reg batch 128, stride 12; wperf `--multiread_batched` | no | exists as loop of gets; batched API is M9 | workload |
| readwhilewriting | Reader p99 with one rate-limited writer | bsh: NUM_THREADS readers + 1 writer, `MB_WRITE_PER_SEC`; wperf 2 MB/s, 5400 s | no | partly: threaded needs a safe reader beside a writer (`index.hh:1003-1005` covers scan only; prospect notes a shared-ownership reader for Phase 3R) | workload (inline form) or harness (threads) |
| fwd/revrangewhilewriting (seekrandomwhilewriting) | Iterator p99 beside writer | bsh `fwdrangewhilewriting`; reg | no | not planned for reverse; forward blocked as above (scan races a writer) | harness |
| readrandomwriterandom | Mixed 90/10 | `dbb` `readwritepercent=90` (`dbb:1065`) | partly (ycsb_b is 95/5 Zipfian) | exists | flag: uniform mix variant |
| deleterandom | Random deletes, then reads over the result | reg step 5; `dbb:4854` | no | exists (`delete_key`) | workload |
| deleteseq / fillanddeleteuniquerandom | Ordered deletes; churn | `dbb` | no | exists | workload |
| range delete (`writes_per_range_tombstone`) | Range tombstones | `dbb:1101-1104` | no | Phase 4 (K8, delete by query) | n/a |
| bulkload | Load with no auto-compaction, vector memtable, no WAL, then `compact` | bsh `bulkload`; wperf first row | partly (fillrandom + settle merges) | exists (`auto_merge`, `force_merge`) | harness: bulk option + timed compact phase |
| compact / compactall | Full compaction time | reg load; bsh `universal_compaction` sweeps subcompactions 1,2,4,8,16 | no | exists (`force_merge`); parallel merge is M8 | harness |
| mixgraph | FB UDB-shaped mix: power-law key ranges, Pareto value sizes, get/put/seek ratios | `dbb:1865-1917` (`key_dist_a/b`, `value_k`, `value_sigma`, `iter_k`, `mix_*_ratio`, `sine_mix_rate`) | no | exists (get, put, scan) | workload (generator) |
| timeseries | One writer, many random readers by id | `wtools` | no | exists (needs shared reader) | workload |
| randomtransaction | Multi-key transactions with checks | bsh `randomtransaction`, 5 threads | no | not planned in core kv | n/a |
| ttl, backup/restore, ingestexternalfile, replay, verifychecksum | Feature paths | `dbb` names | no | not planned (ingest: bulk path, owner call) | n/a |
| Threads | Scaling | bsh 64, reg 16, wperf unstated | partly (4; stale on format 14; asymmetric sharing) | exists | flag: `fits4`, add 8 and 16 |
| Value sizes | 100 B dbb, 400 B bsh, 900 B reg, 4 KiB, 64 KiB (BlobDB) | `rocksdb-survey.md:299` | partly (100 B and 1 KiB) | exists | flag: `value` in rows; `KV_VARIANT_<n>` per size, calibrate loop (`run.py:109`) |
| Dataset vs memory | fits, 2x, 10x RAM | survey section 4; reg 1G keys (~100 GB) | partly (fits, 2x on 11.6 MB files; no 10x; no real >RAM file) | exists | flag: per-row `records` |
| Durability tiers | none, WAL, sync | survey "Equal durability" | yes (sync1 short, 0021 stopped after first sync row) | turn log for the sync loss | none |
| Many indexes | 1,000 column families vs indexes | ours | yes (`many`) | exists | none |
| YCSB A-F | Zipfian mixes | survey section 4 | yes (A-F, 1 KiB) | exists | none |
| async_io / direct I/O / readahead | Read path I/O | reg `*_asyncio`; wperf DIO | no | Phase 6 (residency backends, M9) | harness |
| Write amplification, stalls, p99.99 | Per-row extras wperf reports | wperf columns: W-Amp, stall %, p50-p99.99 | partly (written/logical and p99.9 reported, `run.py:201`) | exists | flag: add p99.99 to `json_phase` |

## Suite 2: Lucene (luceneutil), prospect search

Corpus: Wikipedia line docs, "medium" (~1 KB docs, 33,332,620 in the 20120502 file,
`constants.py:100`) and "big" (~4 KB, 6,726,515 docs, `constants.py:103`). Task files
`tasks/wikimedium.10M.tasks` (13,956 lines, 500 queries per category) and
`wikimedium.1M.tasks`. Nightly: 1 indexing thread, 2048 MB RAM buffer, 5 queries per
category, 50 repeats, 20 JVMs, intra-query concurrency 8 (`nightlyBench.py:91-110`).

| Workload (luceneutil category) | Measures | Upstream setting | Run? | Feature | Cost |
|---|---|---|---|---|---|
| Low/Med/HighTerm | Single term by frequency tier | wikimedium 500 each (HighTerm 84-93) | partly (term class from 40 stratified terms; no tier split) | exists | flag: split `terms-sample` by tier (harness) |
| AndHigh/Med/Low | Conjunction by tier | 500 each | partly (intersection class, no tier labels) | exists | harness: tier tags in `sample_queries.py` |
| OrHigh/Med/Low | Disjunction by tier | 500 each; 499 OrHighLow | partly (union class) | exists | same |
| Low/Med/HighPhrase | Exact phrase | 500/500/36 | yes (phrase class) | exists | none |
| Low/Med/HighSloppyPhrase | Phrase with slop | 500/500/36 | no | exists (NEAR, `PLAN.md:339`; query form to confirm) | harness: task file + oracle counts |
| Low/Med/HighSpanNear, Intervals Ordered | Span and interval queries | 500/500/36 | no | partly (NEAR); intervals not planned | harness |
| Wildcard, Prefix3 | Automata terms | 500 each | no | Phase 5 (`PLAN.md:632`) | harness |
| Fuzzy1, Fuzzy2, Respell | Edit-distance terms | 500 each | no | Phase 5 | harness |
| IntNRQ | Numeric range | 500 | no | Phase 4 (bkd, `PLAN.md:599`) | harness |
| HighTermMonthSort, DayOfYearSort, TitleBDVSort, TermDateTimeSort | Top-N sorted by doc value | 237 each | no | Phase 4 (doc values, sort keys) | harness |
| *DayTaxoFacets, Browse*Facets, TermDateFacets | Facet counts over a match set | 500 / 1 per category; 10M facets task file | no | Phase 4 (aggregation collectors) | harness |
| PKLookup | Primary key lookup | nightly chart (`nightlyBench.py:1786`) | partly (kv `readrandom` measures the key path; no search-index PK row) | exists | harness |
| countOnly.tasks, disjunctionIntensive/Realistic/Simple, crazyRandomMinShouldMatch (mpq), dismax, combinedfields | Count, disjunction shapes, min-should-match, dismax, BM25F | `tasks/*.tasks` | partly (COUNT yes; rest no) | min-should-match, dismax, BM25F not found in `PLAN.md`: owner question | harness |
| highlight / hilite tasks | Highlighting | `tasks/highlight.tasks` | no | not planned | n/a |
| Vector search (KNN) | ANN recall and QPS | `tasks/vector*.tasks`, `src/python/knnPerfTest.py` | no | not planned | n/a |
| Indexing GB/hour, medium docs | Throughput to committed index | 1 thread, 2 GB buffer (`nightlyBench.py:94`) | yes (1 thread, keyed; 1M-doc rows superseded, 5M rerun owed) | exists | none |
| Indexing GB/hour, big docs (4 KB) | Same, big docs | wikibig | no | exists | corpus |
| Indexing with vectors / quantized vectors | Vector build | nightly charts | no | not planned | n/a |
| Indexing with doc values (sorted/sparse) | Build with columns | nightly "fixed index"; sparse taxis | no | Phase 4 | corpus + harness |
| Index disk usage | Bytes after merge | nightly FixedIndexSize | yes (per section, `phase-3-ratchets.md:174`) | exists | none |
| NRT reopen | Reopen latency, QPS beside indexing | `nightlyBench.py:106-110`: 1,103 docs/s, 30 min, 4 search threads, 1 index thread, 1 reopen/s; `nrtPerf.py` modes add, update, ndv_update, bdv_update | no (refresh latency owed, `phase-3-ratchets.md:372`) | partly: refresh exists; readers beside a writer need Phase 3R's shared reader; doc-value updates Phase 4 | harness |
| Merge cost / GC / JIT charts | Merge time, pauses | nightly | partly (write amp owed) | exists | harness |
| Stored fields BEST_SPEED / BEST_COMPRESSION | Stored size, index time, retrieve time | geonames stored-fields benchmark (`nightlyBench.py:739-751`) | no | exists (`stored` plugin) | corpus (GeoNames is on our corpus list) |
| Geo (points, polygons, OSM) | Distance and polygon | `backTestGeo.py`, `IndexAndSearchOpenStreetMaps.java` | no | Phase 4 | corpus |
| Taxis (sparse numeric) | Range/aggregation on sparse doc values | `SearchTaxis.java`, `IndexTaxis.java` | no | Phase 4 | corpus |

## Suite 3: Tantivy and search-benchmark-game

`sbg`: English Wikipedia, no stemming, queries from the AOL set filtered to at least two
terms with a phrase hit; query classes term, intersection, union, phrase; commands COUNT,
TOP_10, TOP_10_COUNT (`README.md`, `commands.txt`: 3,596 queries each); one thread, best of
10 runs. Ours is a superset (adds TOP_100, TOP_1000, TOP_100_COUNT, negated, intersection_union).
Engines in `engines/` include lucene 10.4.0 (`-bp`), tantivy, pisa 0.8.2, iresearch: already
our baseline set.

| Workload (tantivy `benches/`) | Measures | Upstream setting | Run? | Feature | Cost |
|---|---|---|---|---|---|
| sbg term / intersection / union / phrase x COUNT / TOP_10 / TOP_10_COUNT | Query latency | above | yes | exists | none |
| `and_or_queries.rs`, `intersection_bench.rs` | Boolean AND/OR microbenchmarks on synthetic postings | binggan | no | exists | harness (posting-list microbench) |
| `range_query.rs`, `range_queries.rs`, `bool_queries_with_range.rs` | Range, range + boolean | binggan, synthetic columns | no | Phase 4 | harness |
| `exists_query.rs`, `exists_json.rs` | Field-exists | synthetic | no | Phase 4 (optional sections) | harness |
| `agg_bench.rs` | Terms, histogram, date histogram, range, cardinality, composite, sub-aggregations, top hits, many segments | `agg_bench.rs:30-1042` (about 40 request shapes) | no | Phase 4 | harness |
| `regex_all_terms.rs` | Regex/automaton over all terms | synthetic | no | Phase 5 | harness |
| `index-bench.rs` | Indexing hdfs-logs JSON, GitHub events, wiki; with and without commit; dynamic JSON | `hdfs.json`, `gh.json`, `wiki.json` | partly (wiki yes) | JSON/log docs: Phase 4 (log profile) | corpus |
| `merge_segments.rs` | Segment merge time, sorted and unsorted | `merge_segments.rs:149-272` | no (write amp owed) | exists | harness (use `force_merge`) |
| `str_search_and_get.rs` | Search then fetch string fields, by distribution | `str_search_and_get.rs:31-310` | no | exists (stored) / Phase 4 (fast fields) | harness |
| `analyzer.rs` | Tokenizer throughput | `analyzer.rs:8-26` | no | exists | harness (microbench) |
| `columnar/benches`, `sstable/benches`, `stacker`, `bitpacker`, `common` | Column access/merge, term dictionary stream and ord-to-term, hash stacker, bitpacking | crate benches | partly (termdict has its own tests; no bench row) | exists / Phase 4 | harness |
| `query_parser_nested.rs` | Query parse | synthetic | no | Phase 5 (`queryparser`) | harness |

## Suite 4: PISA

| Workload | Measures | Upstream setting | Run? | Feature | Cost |
|---|---|---|---|---|---|
| `queries` (`tools/queries.cpp:347-443`) | Per-query latency for and, or, wand, block_max_wand, block_max_maxscore, maxscore, ranked_and, block_max_ranked_and, ranked_or, ranked_or_taat(_lazy); `--runs`, `--quantized`, `--safe`; reports mean, median, q90, q95, q99 (`queries.cpp:83-97,176-178`) | k = 10 default; thresholds optional | partly (sbg harness runs PISA at TOP_k and COUNT; we report best-of-10, no tails) | exists | harness: report q90/q95/q99 over runs |
| `thresholds`, `kth_threshold` | Priming with known k-th score | tools | no | exists (pruning floors) | harness |
| `evaluate_queries` | Effectiveness: TREC run, MRR/nDCG | tools | partly (`verify`/`parity` check equality with an oracle, not nDCG) | exists | harness |
| `index_perftest` (`benchmarks/`) | Posting decode and `next_geq` speed per codec | `benchmarks/index_perftest.cpp` | no | exists | harness (microbench) |
| `compress_inverted_index`, `create_wand_data` | Index size by codec, WAND metadata size | tools | partly (per-section disk) | exists | none |
| `selective_queries`, `profile_queries`, `shards` | Selectivity slices; per-query profile; shard sizes | tools | no | exists | harness |

## Add this week (ranked)

Rule: no new engine feature, exercises something we do not measure. Sizes are 100k records
unless stated. Times are estimates from the `0021` medians (fits1 sum of one strata + one
RocksDB trial: 26 s, so 5 trials about 2.2 min; twox1: 49 s, about 4 min) and exclude builds.

| # | Add | Exact change | Quick-tier time (estimate) |
|---|---|---|---|
| 1 | Multi-thread rows, fresh on format 14: fits4 and a new twox4 (covers fwdrange at threads; stale since `core-vs-rocksdb.md:126`, hunt-list row 17) | `run.py:59`: after `fits4` add `s["twox4"] = [dict(r, mb=two_x_mb(r["value"])) for r in s["fits4"]]`; also `s["fits8"]` with `threads=8`. Run `run.py rows OUT fits4 5` and `twox4`. Report the asymmetry (RocksDB shared DB, strata per-thread index) in every row | fits4 about 2 min, twox4 about 4 min (the old format took 330 s, `core-vs-rocksdb.md:275`) |
| 2 | `deleterandom` plus a `readrandom` over the result, disk after settle | `kvgen.hh`: add `op_kind::erase`, workload `deleterandom` (stream = a seeded permutation of loaded ids, `ops = records/2`, each key deleted once, like `fillrandom`'s `order_`); `kvdriver.hh:253-290`: `case op_kind::erase: e.erase(t, k)`, count logical bytes 0, `usage_exit` text; `kv_strata.cc`: `erase` builds a `delete_key` op as `put` builds `replace_key` (`kv_strata.cc:72-81`, enum `index.hh:49`); `kv_rocksdb.cc`: `db_->Delete` in the same batch path as put; `run.py:27`: add `deleterandom` to `DB_BENCH`, `WRITES`. A second row `deleterandom_then_read` = run phase 2 `readmissing` on the deleted half (set `ops` and a `--after readrandom` flag) | 1.5 min for fits1 + twox1 at 100 B |
| 3 | Real 10x-RAM file | `run.py`: add `records` to the row dict (default `RECORDS`), pass `--records` in `row_args` (`run.py:34-43`), include it in `key_of` (`run.py:185`); new set `big1`: `records=1_000_000`, `value=100`, `mb=12` (about 10x), workloads fillrandom, readrandom, seekrandom, ycsb_a (1 KiB: `records=200_000`, `mb=24`), 3 trials. Strata merge waits make the 2x row 15 s per settle (`core-vs-rocksdb.md:270-277`) so expect longer | about 10-15 min (scales the 2x row 10x; unverified) |
| 4 | `multireadrandom` | `kvdriver.hh`: `op_kind::multiget` with `--multiget N` (default 10, bsh; also 128, reg); engine hook `multiget(t, keys, sink)`; `kv_strata.cc` loops `get` (until M9's batched API); `kv_rocksdb.cc` calls `MultiGet` (`ReadOptions::async_io` off). `kvgen.hh`: `multireadrandom` = readrandom ids grouped by N; `run.py`: row with `batch=10` reuses the `--batch` flag | 1 min; report row until M9 |
| 5 | `bulkload` and a timed `compact` phase | `kvdriver.hh`: workload `bulkload` = fillrandom with `--bulk`, then a `compact` phase calling `e.compact()`; `kv_strata.cc`: `o.auto_merge = false` under `--bulk`, `compact()` = `refresh(); force_merge(1); commit()` (`index.hh:586`); `kv_rocksdb.cc`: `DisableAutoCompactions`, vector memtable, `disableWAL`, then `CompactRange`; `run.py`: `s["bulk1"]` fits and 2x. Also gives M8 (single-thread forced merge cost) a row | 1 min |
| 6 | `readwhilewriting`, inline form | `kvdriver.hh`: workload `readwhilewriting`; one thread alternates reads and writes at `--write-mb-s` (token bucket on logical bytes; bsh uses 2 MB/s), latency recorded for reads only, `ops` as reads. This keeps scan and writer on one owner thread, so it is valid under `index.hh:1003-1005`. RocksDB runs the same inline loop (equal semantics). The threaded form (N readers + writer) waits on the contract check: one probe of `get` beside `apply` on one index under TSan (not run) | 1 min inline; threaded form not estimated |
| 7 | Value-size ladder: 400 B (bsh), 4 KiB, 64 KiB | `run.py:109` `calibrate` loops `(100, 1024)`: take the values from a list; add `KV_VARIANT_400`, `_4096`, `_65536`; set `s["sizes1"]` = fillrandom, readrandom, ycsb_a at those values with `records = 11.6e6 // (16+value)` so the file stays 11.6 MB (10,000 records at 64 KiB would be 655 MB: use `records=2000`). Calibration first (`run.py calibrate`) | about 4 min plus 2 min calibration |
| 8 | Free rows | `wal1` add `fillseq`; `sync1` add `batch=10`; `json_phase` add `p9999_us` (`kvdriver.hh` percentile 0.9999) so the wperf latency columns match | under 1 min |
| 9 | `mixgraph` (if time) | `kvgen.hh`: port `dbb:1865-1917` parameters (key_dist_a/b, value_k, value_sigma, iter_k, iter_sigma, mix_get/put/seek) as a `mixgraph` stream; engines already have get, put, scan | 1 day to write; 2 min to run |

### Status: items 1 to 8 in the harness

Built on branch `bench/kv-coverage` (harness only; nothing run beyond tiny dry runs, `run.py dry OUT SET`, which checks counts, misses, a read-back digest equal across engines, and the delete/compact/write invariants). Existing rows are unchanged: fits1 `fillrandom` and `ycsb_e` give the same operation counts, logical bytes, disk bytes and scan digest before and after.
Run a set with `run.py rows OUT SET TRIALS`; `run.py calibrate OUT 400,4096,65536` first for item 7.

| # | `run.py` set | What was built | Differences between the engines (unavoidable) |
|---|---|---|---|
| 1 | `twox4`, `fits8` (next to `fits4`) | rows only | RocksDB's threads share one DB, strata's own one index each; a strata scan reads its own thread's shard |
| 2 | `delete1` (fits and 2x) | workload `deleterandom` (each of `records/2` loaded keys deleted once, random order), then `--after readall` (every loaded key read once; the driver exits non-zero unless the misses equal the deletes); disk is measured after the settle. A delete counts its key bytes as logical, so write amplification and the settle apply | none in the sequence; each engine settles by its own rules (RocksDB flushes and waits for its compactions, strata refreshes, merges by policy and commits), so disk after the settle is each engine's state at its settle, not a forced full merge |
| 3 | `big1` | per-row `records`: 1M records of 100 B at 12 MiB; 200k records of 1 KiB (`ycsb_a`) at 24 MiB; the report prints the record count in the workload cell | none |
| 4 | `multiget1` (fits and 2x) | workload `multireadrandom`, `batch` = keys per multiget (10); the keys are `readrandom`'s | strata loops `get` (copying each value, as a single get does); RocksDB calls `DB::MultiGet`. `ops` counts keys, p50 to p99.99 are per batch |
| 5 | `bulk1` (fits and 2x) | workload `bulkload` = `fillrandom`'s keys, automatic merges off, then a timed `compact` phase, then the settle | strata: `auto_merge` off, `compact()` = refresh, `force_merge(1)`, commit. RocksDB: `Options::PrepareForBulkLoad()`, vector memtable, no WAL, flush then `CompactRange` over everything; background jobs stay the row's 2 |
| 6 | `rww1` (fits and 2x) | workload `readwhilewriting`, inline: each thread issues `readrandom`'s gets and, between them, writes of a 2 MB/s token bucket (burst at most 100 ms); latency covers the reads, `ops` counts reads, `writes` in the run phase counts the writes | the read sequence and the write keys and values are the same; how many writes fit in a run follows the reads' speed, so the counts differ. The threaded form still waits for the shared reader |
| 7 | `sizes1` | 400 B, 4 KiB, 64 KiB: `fillrandom`, `readrandom`, `ycsb_a`; records `11.6e6 / (16 + value)` (27,884; 2,821) and 2000 at 64 KiB; `KV_VARIANT_<size>`; `calibrate` takes the sizes and adds a 64 KiB block variant from 4 KiB values up | none; these files are small, so the fills last milliseconds: raise `records` in `LADDER` for steadier numbers |
| 8 | `wal1` (+ `fillseq`), `sync1` (+ batch 10), all phases | `p9999_us` in every phase | none |

Order rationale: 1 is code-free and the only row family with a stale result; 2 and 5 hit
paths strata has (`delete_key`, `force_merge`) that nothing measures; 3 and 7 change the
regime, where the 0021 report shows the largest gaps (fills 6x at 2x); 4 and 6 are report rows
until M9 and the shared reader.

Not this week: reverse scans (`revrange`, `readreverse`): no API (`index.hh:1012-1031`), owner
decision whether it belongs in K3. Merge operators: K7. Transactions, backup, TTL: not planned.

## Gate rows for later phases

Each row is a task file or harness run plus a ratchet target against the faster of Lucene and
Tantivy (`PLAN.md:612,636`). "Hunt" means the explorers get it when the feature lands.

| Row | Source | Phase that builds it | Harness to prepare now |
|---|---|---|---|
| Numeric range (IntNRQ), range + term filter | lu IntNRQ; tv `range_query(ies).rs`, `bool_queries_with_range.rs` | Phase 4 (bkd, `PLAN.md:599`) | 500 numeric-range queries on a doc-values field of the Wikipedia corpus (date or size) |
| Sort by doc value (month, day-of-year, title bdv, date-time) | lu HighTerm*Sort, TermDateTimeSort | Phase 4 (sort keys) | `sort_field` already exists in the baselines' indexes (`phase-3-ratchets.md:192`); add sorted commands to `commands.txt` shape |
| Facets / aggregations (taxonomy, SSDV, term date facets; terms, histogram, range, cardinality, composite) | lu `*Facets`; tv `agg_bench.rs` | Phase 4 (aggregation collectors) | Xapiand-shaped agg set from `agg_bench.rs:100-1000` request shapes |
| Prefix (Prefix3), wildcard, fuzzy 1 and 2, respell, regex over terms | lu Wildcard, Prefix3, Fuzzy1/2, Respell; tv `regex_all_terms.rs` | Phase 5 (`automata`, `PLAN.md:632`) | 500-query files already exist in `tasks/wikimedium.1M.tasks`; map to oracle counts |
| Sloppy phrase, span near, intervals | lu Low/Med/High{SloppyPhrase,SpanNear,Intervals*} | Phase 2 NEAR exists (`PLAN.md:339`); intervals not planned | oracle counts from Xapian NEAR |
| Min-should-match, dismax, BM25F | lu `mpq.tasks`, `dismax.tasks`, `combinedfields.tasks` | not in `PLAN.md`: owner question | n/a |
| NRT reopen latency and QPS beside indexing | lu `nightlyBench.py:106-110`, `nrtPerf.py` | refresh exists; shared reader Phase 3R; doc-value updates Phase 4 | NRT driver: 1,103 docs/s, 1 reopen/s, 4 search threads, 30 min (report p50/p99 reopen) |
| Indexing with doc values | lu fixed/sparse index, taxis | Phase 4 | Wikipedia + date/size columns, one corpus |
| Multi-thread and replace-heavy indexing, write amplification | `BENCHMARKS.md` Ratchet rows | owed in Phase 3 (T3, T4) | exists (`index_trials.sh`, `write_amp.sh`) |
| Stored-field compression modes | lu stored-fields benchmark (GeoNames) | exists now | GeoNames corpus load |
| Geo distance and polygon | lu geo, OSM | Phase 4 (geo index) | GeoNames points and shapes (corpus list) |
| Tail latency over runs (q90/q95/q99, c=1 and saturation) | pisa `queries.cpp:176-178` | report row now; gate at Phase 6 | keep every run's time, not best-of-10 |
| Aggregation/term-list rows on kv (`deleterandom`, range delete) | rocksdb K8 | Phase 4 (delete by query) | rows 2 above are the base |
| Reverse iteration, merge operators | rocksdb `revrange`, `mergerandom` | not planned / K7 | n/a |
| Vector search, highlighting | lu | not planned | n/a |
| io_uring / direct I/O reads | rocksdb `*_asyncio`, DIO runs | Phase 6 | `run.py` variant flag for RocksDB's `async_io` and `use_direct_reads` |

## Open questions for the owner

1. Q1 Reverse scans (`revrange`, `readreverse`): add to K3, or declare out? Recommendation: out until a workload needs them; list the rows as "not planned".
2. Q2 Min-should-match, dismax, BM25F: in or out of Phase 5? Recommendation: min-should-match in (cheap in a block-max scorer), the rest out.
3. Q3 Threaded `readwhilewriting`: wait for Phase 3R's shared reader, or let the core kv index serve `get` beside `apply` now? Recommendation: a one-day probe (TSan, get beside apply), then decide.
