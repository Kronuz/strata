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
| sync (batch 1, 10, 100) | `sync1`: fillrandom, overwrite, YCSB A and F | `a93f2b59` (code `cc8ba3aa`) | throughput tie on every row (0.98-1.11×); one barrier per batch, as RocksDB | `docs/perf/standalone-turn-log.md`; 261118-202755-3d88cf, 261118-202758-1425d5, 261118-202802-fb1582 |
| wal (`ack=none`) | `wal1`: fillseq, fillrandom, overwrite | `cc8ba3aa` | 1.68-1.94× faster, p99.99 113-179× higher; window bounded at one barrier plus one write (D107); not equal durability (D104) | same note; 261117-134809-8409da |
| sync, group commit | K=1, 4, 8 writers, fillrandom batch 1 | `cc8ba3aa` | tie at K=1 and K=8, 1.32-1.39× slower at K=4 (turn-log-parity A6) | same note; 261117-134842-7d024d |

### VM rows (A6, VM-virtio, D114)

qemu with HVF on this Mac, virtio disk with `cache=none` on APFS: the barriers measure qemu's, not a
device's, so these rows check the platform, not the gate's ratios. Ubuntu 24.04 (Linux 6.8, 4 vCPU,
6 GiB) and FreeBSD 14.4 (4 vCPU, 3 GiB), at `db443563` (code `cc8ba3aa`), one trial each.

| Platform | Dry run (every set, 2,000 records, digests equal) | sync rows (`syncaa`) | wal1 | Jobs |
|---|---|---|---|---|
| Linux x86_64 | passed | fillrandom batch 100 1.22×, batch 1 2.00×, YCSB A 1.99× faster | 1.05-1.15× slower, p99 1.23-1.44× higher | build 261117-200358-f31d88, dry 261117-200358-e66125, rows 261117-200358-1336ac |
| FreeBSD x86_64 | passed | batch 100 1.19× faster; batch 1 and YCSB A tie (0.92-0.94×) | 1.39-1.45× slower, p99 1.65-1.80× higher | build 261117-200402-b01248, dry 261117-200402-6b0f87, rows 261117-200402-ed7288 |

strata's peak memory runs from 29% less to 67% more than RocksDB's, and its disk 1.3-7.8× RocksDB's.
FreeBSD has no per-process write counter, so its bytes-written column reads zero and is unmeasured.
wal1, 1.68-1.94× faster on the Mac, is slower in both VMs, where strata spends 1.3-1.7× RocksDB's
CPU per write; why is not measured.

RocksDB runs pinned at 11.11 on its fastest calibrated configuration per value size, under the
fairness audit (`docs/perf/rocksdb-fairness-audit.md`): `lvl-16k-lz4` at 100 B and `lvl-4k-none`
at 1 KiB (A5). `run.py calibrate` scores each variant by the geometric mean of its median ops/s on
fillrandom, readrandom and YCSB A over five interleaved trials, and fails when the pin trails the
fastest by more than 3%. At 100 B the pin is fastest (261118-175418-fa1d4d; next `lvl-4k-lz4-hash`
at 0.967). At 1 KiB `lvl-16k-lz4` scored 0.970 of `lvl-4k-none` (261118-175419-c46cd4: fillrandom
343k against 301k ops/s), so 1 KiB re-pinned, and a second calibration agrees
(261118-202804-4d7ad8, `lvl-16k-lz4` at 0.983). The 1 KiB rows above ran on the new pin; the A/A
and the VM rows' 1 KiB row ran on `lvl-16k-lz4`. The A/A on the sync rows
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
- **Platforms:** macOS x86_64 on APFS carries the gate's rows; Linux and FreeBSD x86_64 run in
  qemu VMs (sync and wal1 rows, one trial each, VM-virtio).
- **Not measured:** 10× memory, more than 8 writers, millions of indexes on one host, arm64, and
  Linux or FreeBSD on a device rather than a VM.

## What the gate still owes

1. **A3:** the 10× memory rows on every workload, and the `none` tier rerun at the gate's commit
   (the `none` rows above are main `75d1e7e`'s).
2. **A4:** strata at or under 1.0× RocksDB's time on every row and axis. Open misses: group commit
   at K=4 (1.32-1.39×), the `wal` p99.99 (113-179×), peak memory (2.5-3.4× on 1 KiB rows), disk
   (3.3× on batched fills) and bytes written (36-92% more) on the durable rows, scans, the 2×
   tail, and wal1 in both VMs (1.05-1.45× slower).
3. **A6:** arm64 rows, before Phase 3R's next gate (D105); CI's linux-arm64 job builds and runs
   the tests. Linux and FreeBSD are VM rows only (above).
4. **A8:** this report, completed when the rows above are in.

## Checks (A7)

At `db443563`: one CPU-hour or more of fuzzing (3 workers × 25 minutes, ASan and UBSan) on each
decoder: `stored` (format 14) 261117-181138-259eee, `manifest_log` 261117-181138-24cc48,
`manifest_log_file` 261117-181138-0354ba, `command` 261117-181139-098eed. Readers reject unknown
versions, and the fixtures decode: 261117-181139-9949e7. Coverage: 261117-181139-6e2a77. ASan,
UBSan and TSan run on CI's Linux jobs.
