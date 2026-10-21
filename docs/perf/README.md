# strata performance records

One file per Track P item (PLAN.md) on the core: what it changes, the class it targets, and its
before/after table. The search records (P6, P6a, P7, P12, the BM25 presets, the query gap,
format 13's query items and landing, the inverter) moved with the search code to prospect's
[docs/perf](https://github.com/Kronuz/prospect/tree/main/docs/perf).

| Item | Change | Class | Result |
|---|---|---|---|
| [Flush memory](peak-memory.md) | Flush sizes its writer up front, borrows stored records from the buffer, streams every run to its file | peak memory, fillrandom 100k | peak RSS median 95.5 → 70.7 MiB (-25.9%, A/A 1.2%); segment bytes unchanged |
| [Reclamation](reclamation.md) | Whole-segment drop at commit, stepped file removal, merges at commit | disk under deletes, foreground stalls | FIFO without merges: 7.5× fewer bytes held; with merges, no change (write amplification 2.0); freeing costs 7–130 ms per GiB across platforms |
| [Commit barriers](commit-barriers.md) | A commit's data files made durable with one batched sync (`IO::sync_files`) | commit latency | commit phase median: macOS 187 → 106 ms, Linux ext4 24.8 → 16.2 ms; FreeBSD unchanged |
| [P15](p15-region-size.md) | Verified regions of 4 KiB instead of 64 KiB | serving memory | peak 677 → 574 MB (with lengths verified on first use; 1.025× Tantivy); queries within a few percent |
| [Search layout as KV](core-vs-rocksdb.md) | `search_layout` with keys and stored only, driven as a key-value store against RocksDB 11.11.1 (quick tier, macOS host); not the core comparison | db_bench rows, YCSB A–F, many indexes | blind writes 1.2–5.4× faster, absent keys 2.8–6.9× faster, disk 24–29% less; gets 12–32× slower, scans 45–220× slower, durable writes 4.7–5.2× slower; 1,000 indexes: 58% less memory, gets 2.95× slower |
| [KV fairness audit](rocksdb-fairness-audit.md) | What RocksDB did in each KV row (statistics, barrier counts, source); harness fixes | every KV row | sync YCSB 4.99–5.02× slower (was 4.74–4.93×); many-index settle about 3.97× slower (was 1.72×); 2× fill wins are macOS barriers (1.51–3.69× slower with cheap barriers); strata's fills 1.17–1.23× faster to durable without an empty commit |
