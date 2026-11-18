# Track C, C4 gate report (skeleton, rows pending)

C4 (pipelined replication and flow control: D15, D16, D18, and the hello's limits) as built in
`consensus`. This report holds C4's gate rows. Every row is pending: its jobs run after the
quests it waits on land, at `consensus` main on that day. No row below has a number; a
row gets its figures and job id from its quest's jobs, never from a recollection.

Track C's reconciliation (strata `docs/design/track-c.md`, "Reconciliation (2026-11-18)")
maps A5 (C4) and the owed part of A1 (the hello's limits) onto these quests:

| Quest | Delivers | Status |
|---|---|---|
| `consensus-hello-limits` | The node hello carries the ten `Limits` fields and a wire version range; the host reports `PeerLimits` for each incarnation `member_of` places on that node; a host refuses a frame or window below its `rpc_bytes`. | Not landed |
| `consensus-flow-bench` | T7 on the host transport: `docs/perf/flow-control.md` with each row's job id. | Not landed |
| `group-install` | The snapshot transfer under group credit, and D18's catch-up retry from a newer checkpoint. | Not landed; T7's snapshot row waits for it |

## What is built (at the reconciliation, before this report's runs)

Landed before this report's rows and rerun by them; the rows below are the runs, not these
commits.

- D15 (`consensus:6af73ae`, `7e23145`, `24d5399`, `906383c`, `c752657`): the leader's per-peer
  flights, the follower's merged `Receive`, the sender queues, and the frame cap.
- D16: multiplexing and per-group credit (`detent:transport-groups`, `consensus:e4e394b`; identity,
  `consensus:361d5d2`), and TLS (`consensus:677c864`).
- D18 (`consensus:fcc18e6`): the snapshot window. Chunks do not yet ride the bulk lane under group
  credit (`group-install`).

Not built, and stated by `docs/mechanisms/host-transport.md`: R19's 64 KiB unsent-bytes bound
(`TCP_NOTSENT_LOWAT` is unset; the stated bound is one data frame, one 64 KiB feed and the
kernel's buffers), D16's connection-wide window, and D18's third-failure stop (no criterion).
The rows measure the bound the transport states and say so.

## Procedure, common to every row

- Each row is a campaign job tagged to quest `track-c-gates`, at `consensus` main's commit on the
  day it runs, built `RelWithDebInfo` with Ninja, with `--ref` pins for the other repositories it
  needs (`journal`, `completion`) at their mains, as C2's report does.
- Timing rows run as `exclusive` jobs on the Linux gate VM (document-gate's, port 18782) with
  `netem` for added round trip and real storage at the follower; a row that does not time
  anything runs `light`.
- Each row reports its commit, its platform, the job id, the command, the raw figures, and
  its target. A row that misses its target is reported as a miss.
- Rows are never merged across platforms: a figure from macOS or FreeBSD is labeled as such.

## Rows

| Row | Target | Procedure | Result | Status | Job | Waits for |
|---|---|---|---|---|---|---|
| **T7a** One group's throughput at 0 ms added round trip, `pipeline = 4` against `pipeline = 1` | Pipelined at least as fast as `pipeline = 1`; within 20% of the follower's barrier-bound rate | `consensus-flow-bench` A1: `flow_bench`, one group, three voters, follower on real storage, exclusive | | Pending | | `consensus-flow-bench` |
| **T7b** The same at 1 ms | Pipelined faster than `pipeline = 1` | as T7a, `netem` delay 1 ms | | Pending | | `consensus-flow-bench` |
| **T7c** The same at 10 ms | Pipelined at least 4 times `pipeline = 1`, and within 20% of the follower's barrier-bound rate | as T7a, `netem` delay 10 ms | | Pending | | `consensus-flow-bench` |
| **T7d** 100 groups on one connection, one follower's barrier slowed 10 times | Every other group's commit p99 within 1.5 times the unslowed run; the slow group's unsent bytes within the transport's stated bound | `consensus-flow-bench` A2, exclusive, an unslowed run beside the slowed run | | Pending | | `consensus-flow-bench` |
| **R19** The longest control-frame gap behind Bulk traffic | Within the bound the transport states (one data frame, one 64 KiB feed and the kernel's buffers); far under `election_min` (1 s) | `consensus-flow-bench` A3, with the host-transport `upload` case as its correctness companion | | Pending | | `consensus-flow-bench` |
| **T7e** Snapshot throughput at 10 ms added round trip | At least 50 MB/s | chunks on the bulk lane under group credit | | Pending | | `group-install`; also `consensus-flow-bench` |
| **D18** A transfer's catch-up retry from a newer checkpoint | The second attempt sends only the files the receiver lacks | `group-install` A2 and A8; its jobs are cited here | | Pending | | `group-install` |
| **H1** A leader holds back an entry above a peer's hello-reported command limit | The entry is held back; a mutant that ignores the reported limits fails the test | `consensus-hello-limits` A1: a job, and a `--fails-without` job on the plain test at the mutant | | Pending | | `consensus-hello-limits` |
| **H2** A hello whose limits fail `validate_limits` | Refused | `consensus-hello-limits` A2 | | Pending | | `consensus-hello-limits` |
| **H3** A peer outside the version range | Refused with a named error | `consensus-hello-limits` A3 | | Pending | | `consensus-hello-limits` |
| **H4** A host refuses a `maximum_frame` or group window below what its `rpc_bytes` needs | Refused at open | `consensus-hello-limits` tests | | Pending | | `consensus-hello-limits` |
| **T8** One CPU-hour of fuzzing on the hello's decoder | No crash, no timeout, no leak | `consensus-hello-limits` A4: libFuzzer with ASan and UBSan, at least one CPU-hour of executions at its commit, recorded with the executed-unit count | | Pending | | `consensus-hello-limits` |
| **Suite** The consensus suite and the mutants at the landing commits | All pass, every mutant killed | `ctest` and `tools/mutants.sh` at `consensus` main after both quests land, on macOS, Linux and FreeBSD | | Pending | | both quests |
| **CI** The platform CI at that commit | Green on every platform | the landing quests' own CI runs | | Pending | | both quests |

## Rows that stay with other quests

- T4's 10 GiB install at 10 ms with 1,000 groups on the leader's node is `group-install`'s
  (A4's shape and its 1 GiB file sets) and `group-core`'s; C4 cites its figure for T7e only.
- D16's connection-wide window and R19's `TCP_NOTSENT_LOWAT` bound are unbuilt. A row for either
  belongs to a quest that builds it; this report records the transport's stated bound.

## Review

The architect's review is `docs/reviews/track-c-c4.md` (A3 of `track-c-gates`), written
after the rows above have results.
