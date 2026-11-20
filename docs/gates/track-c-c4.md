# Track C, C4 gate report (rows queued as jobs)

C4 (pipelined replication and flow control: D15, D16, D18, and the hello's limits) as built in
`consensus`. This report holds C4's gate rows, run at `consensus` main `consensus:e1f249d` (the
landing of `consensus-rpc-lane`). A row gets its figures and job id from its jobs, never from a
recollection; a row with no job id is not buildable yet and says why.

Track C's reconciliation (strata `docs/design/track-c.md`, "Reconciliation (2026-11-18)")
maps A5 (C4) and the owed part of A1 (the hello's limits) onto these quests:

| Quest | Delivers | Status |
|---|---|---|
| `consensus-hello-limits` | The node hello carries the ten `Limits` fields and a wire version range; the host reports `PeerLimits` for each incarnation `member_of` places on that node; a host refuses a frame or window below its `rpc_bytes`. | Dropped, folded into `consensus-rpc-lane` (replan 2026-11-20), which landed it at `consensus:e1f249d` (`docs/mechanisms/hello.md`, `test/transport/hello_test.cc`) |
| `consensus-flow-bench` | T7 on the host transport: `docs/perf/flow-control.md` with each row's job id. | Landed (`consensus:afea684`, measured at `consensus:e7a453b`); rerun here at `consensus:e1f249d` |
| `group-install` | The snapshot transfer under group credit, and D18's catch-up retry from a newer checkpoint. | Dropped, folded into `group-heal` (replan 2026-11-20), which is planned and has no branch. T7e and D18 have nothing to run |

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

- Each row is a campaign job tagged to quest `track-c-gates`, at `consensus:e1f249d`, built with
  Ninja from `consensus` main's own pins (its host-transport tests need no `journal` pin).
- Timing rows run as `exclusive` jobs on the Linux gate VM (document-gate's, port 18782) with
  `netem` for added round trip and real storage at the follower; a row that does not time
  anything runs `light`.
- Each row reports its commit, its platform, the job id, the command, the raw figures, and
  its target. A row that misses its target is reported as a miss.
- Rows are never merged across platforms: a figure from macOS or FreeBSD is labeled as such.

## Rows

| Row | Target | Procedure | Result | Status | Job | Waits for |
|---|---|---|---|---|---|---|
| **T7a** One group's throughput at 0 ms added round trip, `pipeline = 4` against `pipeline = 1` | Pipelined at least as fast as `pipeline = 1`; within 20% of the follower's barrier-bound rate | `consensus-flow-bench` A1: `flow_bench`, one group, three voters, follower on real storage, exclusive | Queued. Prior run at `consensus:e7a453b` (`261118-114928-9bef7d`, for reference; not this row's result): 0.02 ms measured, 256 KiB frames: 54.4 MB/s pipelined, 48.6 MB/s at `pipeline = 1` (1.12 times) | Queued | `261120-123441-f9ff24` (A1, with T7b and T7c) | the Linux VM chain behind C2's reruns (`261120-122833-d9a63d`) |
| **T7b** The same at 1 ms | Pipelined faster than `pipeline = 1` | as T7a, `netem` delay 1 ms | Queued. Prior run at `consensus:e7a453b` (`261118-114928-9bef7d`, for reference; not this row's result): 2.8 ms measured: 55.5 against 32.8 MB/s (1.69 times) | Queued | `261120-123441-f9ff24` | the Linux VM chain behind C2's reruns (`261120-122833-d9a63d`) |
| **T7c** The same at 10 ms | Pipelined at least 4 times `pipeline = 1`, and within 20% of the follower's barrier-bound rate | as T7a, `netem` delay 10 ms | Queued. Prior run at `consensus:e7a453b` (`261118-114928-9bef7d`, for reference; not this row's result): 12.2 ms measured: 54.8 against 14.8 MB/s (3.70 times, below the 4 times target; 4.53 times with `fdatasync`, one run) | Queued | `261120-123441-f9ff24` | the Linux VM chain behind C2's reruns (`261120-122833-d9a63d`) |
| **T7d** 100 groups on one connection, one follower's barrier slowed 10 times | Every other group's commit p99 within 1.5 times the unslowed run; the slow group's unsent bytes within the transport's stated bound | `consensus-flow-bench` A2, exclusive, an unslowed run beside the slowed run | Queued. Prior run (`261118-114946-13cf10`): the other groups' apply p99 at node 2 35.7 to 36.7 ms slowed against 37.3 to 52.0 ms unslowed at 2.8 ms; group 0's undelivered bytes at most 768 KiB | Queued | `261120-123443-3f67e6` (A2) | the Linux VM chain behind C2's reruns (`261120-122833-d9a63d`) |
| **R19** The longest control-frame gap behind Bulk traffic | Within the bound the transport states (one data frame, one 64 KiB feed and the kernel's buffers); far under `election_min` (1 s) | `consensus-flow-bench` A3, with the host-transport `upload` case as its correctness companion | Queued. Its correctness companion, the host-transport `upload` case, runs in the suite rows | Queued | `261120-123446-52c072` (A3); suite rows below | the Linux VM chain behind C2's reruns (`261120-122833-d9a63d`) |
| **T7e** Snapshot throughput at 10 ms added round trip | At least 50 MB/s | chunks on the bulk lane under group credit | Not runnable: `group-install` was folded into `group-heal`, which is planned and has no branch; no code moves a snapshot's chunks on the bulk lane under group credit | Not built | none | `group-heal` |
| **D18** A transfer's catch-up retry from a newer checkpoint | The second attempt sends only the files the receiver lacks | `group-install` A2 and A8; its jobs are cited here | Not runnable, as T7e | Not built | none | `group-heal` |
| **H1** A leader holds back an entry above a peer's hello-reported command limit | The entry is held back; a mutant that ignores the reported limits fails the test | `consensus-hello-limits` A1: a job, and a `--fails-without` job on the plain test at the mutant | Queued. `consensus_transport_hello_leader` passed at `consensus:e1f249d` in `261120-085349-81eac6` (the quest's own job). The control is the mutant `hello-limits-ignored` (`consensus:53b223a`, built by `test/transport/rpc_mutants.py` over `e1f249d`); its job passes when the test fails | Queued | `261120-123421-4452cc` (test), `261120-123429-dee769` (control) | nothing; macOS light jobs |
| **H2** A hello whose limits fail `validate_limits` | Refused | `consensus-hello-limits` A2 | Queued. `consensus_transport_hello_codec` and `consensus_transport_node_hello` (zero or inverted limits refused, each malformed field) | Queued | `261120-123421-4452cc` | nothing; macOS light job |
| **H3** A peer outside the version range | Refused with a named error | `consensus-hello-limits` A3 | Queued. `consensus_transport_hello_version` (a host speaking only wire version 2 never connects to one speaking 1; `WireVersionRefused`) | Queued | `261120-123421-4452cc` | nothing; macOS light job |
| **H4** A host refuses a `maximum_frame` or group window below what its `rpc_bytes` needs | Refused at open | `consensus-hello-limits` tests | Queued. `consensus_transport_hello_version` (a 1 MiB `maximum_frame` for a 2 MiB `rpc_bytes` is refused at construction); a group window below the frame is not refused, by design (`docs/mechanisms/hello.md`) | Queued | `261120-123421-4452cc` | nothing; macOS light job |
| **T8** One CPU-hour of fuzzing on the hello's decoder | No crash, no timeout, no leak | `consensus-hello-limits` A4: libFuzzer with ASan and UBSan, at least one CPU-hour of executions at its commit, recorded with the executed-unit count | Queued. The hello's decoder is case 9 of `fuzz_decoders` (`FUZZ_DECODER=9`, `DTHL0002`): libFuzzer with ASan and UBSan, four runs of 1,000 s from distinct seeds on the checked-in corpus, CPU time from `time -p`. The quest's own deferred check `261120-085507-4feb50` failed at `e1f249d` with "No tests were found!!!" (`-R rpc_stream_fuzz` names no test): a wrong name, not a crash | Queued | `261120-123408-3ac775` (build), `261120-123410-8a605b`, `261120-123412-167bb5`, `261120-123414-ecad78`, `261120-123416-4346ee` | nothing; macOS light jobs |
| **Suite** The consensus suite and the mutants at the landing commits | All pass, every mutant killed | `ctest` and `tools/mutants.sh` at `consensus` main after both quests land, on macOS, Linux and FreeBSD | Queued: macOS `261120-123419-22485b` (ctest, which runs `tools/mutants.sh` as `consensus_mutants`), Linux VM `261120-123449-c0320e`, FreeBSD VM `261120-123452-94f5e9` | Queued | as in the Result column | the Linux VM chain behind C2's reruns (`261120-122833-d9a63d`) |
| **CI** The platform CI at that commit | Green on every platform | the landing quests' own CI runs | `consensus` main at `e1f249d`, CI run `44778137504`: `freebsd-x86_64`, `linux-asan-ubsan`, `linux-tsan`, `macos-x86_64`, `macos-arm64`, `linux-arm64-gcc`, `linux-x86_64-clang`, `linux-x86_64-gcc` success; the `headers` matrix skipped. The same commit on `quest/xapiand/consensus-rpc-lane`: run `44773283822`, success | Pass | `github.com/Kronuz/consensus/actions/runs/44778137504` | nothing |

## Rows that stay with other quests

- T4's 10 GiB install at 10 ms with 1,000 groups on the leader's node is `group-install`'s
  (A4's shape and its 1 GiB file sets) and `group-core`'s; C4 cites its figure for T7e only.
- D16's connection-wide window and R19's `TCP_NOTSENT_LOWAT` bound are unbuilt. A row for either
  belongs to a quest that builds it; this report records the transport's stated bound.

## Review

The architect's review is `docs/reviews/track-c-c4.md` (A3 of `track-c-gates`), written
after the rows above have results.
