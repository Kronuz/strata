# The document profile gate (in progress)

The gate measures strata's lifecycle core, used as a key-value engine, against RocksDB 11.11 on
all four axes (PLAN, "The `document` profile gate (after Phase 4)"; `docs/design/rocksdb-survey.md`
K2). This report collects the rows measured so far, each with its commit, platform and jobs, and
lists what the gate still owes. Rows ran on the development Mac (Intel i9-9980HK, APFS) unless
marked.

## Prerequisites (A1, A2)

| Prerequisite | Where | Evidence |
|---|---|---|
| Standalone durability through the turn log (A1) | `cc8ba3aa`: `include/strata/core/turn_seal.hh`, `command.hh`, `index.hh` logged mode; `docs/design/standalone-turn-log.md` "Built" | build-test 261117-134744-7f0c17; crash, TSan and mutant jobs in the quest journal's Evidence |
| Format 13's ordered `keys` section | `76fa3313` ("Format 13 checkpoint"), an ancestor of `193e6e32` | quest `format13`, landed |
| Snapshot publication: shared reader views | `23e78bb6` (merge of `phase4/shared-reader-views`) | quest `shared-reader-views`, landed |
| Snapshot publication: write buffer reads | `2f56d64c` (merge of `phase4/write-buffer-reads`) | quest `write-buffer-reads`, landed |
| The manifest log as the checkpoint | `15d39c30` | quest `manifest-log`, landed |

## Rows measured

| Tier | Rows | Commit | Result | Source |
|---|---|---|---|---|
| none, fits and 2× | db_bench and YCSB A-F, 1 thread, 100 B and 1 KiB | main `75d1e7e` | wins 27 of 34; scans 1.12-1.21× slower; peak memory 1.5-4.3×; tail at 2× | `docs/perf/core-vs-rocksdb-f14.md` |
| sync (batch 1, 10, 100) | `sync1`: fillrandom, overwrite, YCSB A and F | `cc8ba3aa` | throughput tie on every row (0.93-1.01×); one barrier per batch, as RocksDB | `docs/perf/standalone-turn-log.md`; 261117-134818-6068ec, 261117-134824-199dc8, 261117-134834-f21397 |
| wal (`ack=none`) | `wal1`: fillseq, fillrandom, overwrite | `cc8ba3aa` | 1.68-1.94× faster, p99.99 113-179× higher; window bounded at one barrier plus one write (D107); not equal durability (D104) | same note; 261117-134809-8409da |
| sync, group commit | K=1, 4, 8 writers, fillrandom batch 1 | `cc8ba3aa` | tie at K=1 and K=8, 1.32-1.39× slower at K=4 (turn-log-parity A6) | same note; 261117-134842-7d024d |

RocksDB runs pinned at 11.11 on `lvl-16k-lz4`, its fastest calibrated configuration for both value
sizes, under the fairness audit (`docs/perf/rocksdb-fairness-audit.md`). The A/A on the sync rows
(261117-055320-5ca39d, 261117-055327-074989) spread 0-2.3% for strata and 0.2-8.0% for RocksDB.

## Scale

Each claim holds at the scale its rows measured, and no further:

- **Rows:** 100,000 records of 100 B or 1 KiB under a 256 MiB budget (fits), on one thread and one
  index. Sync rows are capped at 15 s, about 700-70,000 acknowledgements. Many indexes: 1,000
  indexes (main `75d1e7e`). Group commit: up to 8 writers on 1-8 logs.
- **Limits** (each from a mechanism): a logged transaction is at most 64 MiB, consensus's command
  ceiling (D3, Q5). Checkpoints are due every 64 MiB of log. A host runs 1-4,096 loops, one turn
  log each, with 64 MiB segments by default. Group ids are 64-bit and never reused. The benchmark
  host uses 1 MiB commands and 4 MiB segments (`bench/kv/kv_strata.hh`).
- **Not measured:** 10× memory, more than 8 writers, millions of indexes on one host, and any
  platform other than macOS x86_64 on APFS.

## What the gate still owes

1. **A3:** the 10× memory rows on every workload, and the `none` tier rerun at the gate's commit
   (the `none` rows above are main `75d1e7e`'s).
2. **A4:** strata at or under 1.0× RocksDB's time on every row and axis. Open misses: group commit
   at K=4 (1.32-1.39×), the `wal` p99.99 (113-179×), peak memory (2.5-3.4× on 1 KiB rows), disk
   (3.3× on batched fills) and bytes written (57-91% more) on the durable rows, scans, and the 2×
   tail.
3. **A6:** rows on Linux, FreeBSD and arm64. None have run, and how to get them is a decision
   (the quest journal's "Blocked on").
4. **A7:** queued at `12f59669`: one CPU-hour of fuzzing on each decoder (`stored`, format 14;
   `manifest_log` and `manifest_log_file`; `command`), the version-rejection and fixture tests,
   and coverage. ASan, UBSan and TSan run on CI's Linux jobs.
5. **A8:** this report, completed when the rows above are in.
