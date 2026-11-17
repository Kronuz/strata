# The document profile gate (in progress)

The gate measures strata's lifecycle core, used as a key-value engine, against RocksDB 11.11 on
all four axes (PLAN, "The `document` profile gate (after Phase 4)"; `docs/design/rocksdb-survey.md`
K2). This report collects the rows measured so far, each with its commit, platform and jobs, and
lists what the gate still owes. Rows ran on the development Mac (Intel i9-9980HK, APFS) unless
marked.

## Prerequisites (A1, A2)

| Prerequisite | Where | Evidence |
|---|---|---|
| Standalone durability through the turn log (A1) | `193e6e32`: `include/strata/core/turn_seal.hh`, `command.hh`, `index.hh` logged mode; `docs/design/standalone-turn-log.md` "Built" | build-test 261117-055209-094ff8; crash, TSan and mutant jobs in the quest journal's Evidence |
| Format 13's ordered `keys` section | `76fa3313` ("Format 13 checkpoint"), an ancestor of `193e6e32` | quest `format13`, landed |
| Snapshot publication: shared reader views | `23e78bb6` (merge of `phase4/shared-reader-views`) | quest `shared-reader-views`, landed |
| Snapshot publication: write buffer reads | `2f56d64c` (merge of `phase4/write-buffer-reads`) | quest `write-buffer-reads`, landed |
| The manifest log as the checkpoint | `15d39c30` | quest `manifest-log`, landed |

## Rows measured

| Tier | Rows | Commit | Result | Source |
|---|---|---|---|---|
| none, fits and 2× | db_bench and YCSB A-F, 1 thread, 100 B and 1 KiB | main `75d1e7e` | wins 27 of 34; scans 1.12-1.21× slower; peak memory 1.5-4.3×; tail at 2× | `docs/perf/core-vs-rocksdb-f14.md` |
| sync (batch 1, 10, 100) | `sync1`: fillrandom, overwrite, YCSB A and F | `193e6e32` | throughput tie on every row (0.96-1.05×); one barrier per batch, as RocksDB | `docs/perf/standalone-turn-log.md`; 261117-055245-3aecbb, 261117-055253-80fcf5, 261117-055301-8578af |
| wal (`ack=none`) | `wal1`: fillseq, fillrandom, overwrite | `193e6e32` | 4.48-6.72× faster, not equal durability (D104) | same note; 261117-055310-7264db |
| sync, group commit | K=1, 4, 8 writers, fillrandom batch 1 | `7db7a919` | 1.2-1.45× slower at K=4-8 (turn-log-parity A6) | same note; 261117-034940-b2b9c3 |

RocksDB runs pinned at 11.11 on `lvl-16k-lz4`, its fastest calibrated configuration for both value
sizes, under the fairness audit (`docs/perf/rocksdb-fairness-audit.md`). The A/A on the sync rows
(261117-055320-5ca39d, 261117-055327-074989) spread 0-2.3% for strata and 0.2-8.0% for RocksDB.

## What the gate still owes

1. **A3:** the 10× memory rows on every workload, and the `none` tier rerun at the gate's commit
   (the `none` rows above are main `75d1e7e`'s).
2. **A4:** strata at or under 1.0× RocksDB's time on every row and axis. Open misses: group commit
   (1.2-1.45×), peak memory (2.5-3.4× on 1 KiB rows), disk (3.3× on batched fills) and bytes
   written (57-91% more) on the durable rows, scans, and the 2× tail.
3. **A6:** rows on the Linux VM, the FreeBSD VM and an arm64 host. None have run.
4. **A7:** ASan and UBSan at the gate's commit, one CPU-hour of fuzzing on the command codec
   (`fuzz/fuzz_command.cc`) and on format 14's decoders, coverage, and the scale statement. TSan
   ran on the turn log's cases only.
5. **A8:** this report, completed when the rows above are in.
