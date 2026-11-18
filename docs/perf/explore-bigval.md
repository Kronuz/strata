> Status: SUPERSEDED (2026-10-21) the reclaim backlog it found is fixed on main by the background reclaim (docs/perf/reclaim-bg.md, docs/perf/reclamation.md).

# 64 KiB values, YCSB A: disk 622 MB against 95 MB (hunt-list row)

Row: sizes1 ycsb_a v65536 (2,000 records, 20,000 ops, M=256 MiB), strata main d1bbc42, Release.

## Cause

Not blocks, framing or merge selection. After settle the index holds 4-8 live segments (live
section bytes about 75 MB, live_docs 2000, like RocksDB's 72 MB live), but 44 obsolete
segment files sit in the reclaim queue, undrained. `commit()` reclaims only `reclaim_slice`
(64 MiB) per call, and the row's run plus settle makes few commits, so merged-away
segments (about 600 MB of overwritten versions) are still on disk when disk is read.
`KV_DRAIN` probe (reclaim() after settle): 609 MB -> 84 MB, 44 pending -> 0.

## Experiment 1 (win): reclaim_slice 4096 MiB (`KV_RECLAIM_SLICE_MB`, kv_strata.cc)

5 interleaved rounds, A base / B slice / A2 base (strict hold). get digest identical in all
15 runs (a46c3543d251394d, 9980 hits).

| arm | disk after settle | run ops/s | run p99.9 us | total s | peak RSS MiB |
|---|---|---|---|---|---|
| A (5) | 661-792 MB | 8294-8826 | 30.5-35.4k | 3.34-3.63 | 769-847 |
| A2 (5) | 681-787 MB | 7897-8722 | 30.5-34.9k | 3.14-3.70 | 760-883 |
| B (5) | 79-88 MB | 7893-8553 | 31.1-36.4k | 3.60-3.91 | 792-869 |

Disk is 8-9x smaller (RocksDB 100 MB). A/A spread: disk +-8%, ops/s +-5%. B costs about
2-5% run throughput and +0.3 s total (the truncate/unlink work moves into commits); not
separable from A/A noise on throughput, consistent on total time.

## Not fixed

Peak RSS (about 800 MB against RocksDB 560 MB) is unchanged by draining: it is not the
queued files. Not investigated (budget_peak 234 MB of 256, so mapped reads or retained
buffers are the suspects).

## Fix to finish

Make the reclaim backlog drain in proportion to what merges produce (e.g. slice = max(64 MiB,
backlog / k), or reclaim on merge publish, or drain when reclaim_pending is over a bound),
keeping the step truncation so one call never stalls. Also have the bench settle() call
reclaim() so "disk after settle" means settled. No format bytes.
