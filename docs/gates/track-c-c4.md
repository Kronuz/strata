# Track C, C4 gate report (results at `e1f249d`, rerun at main `8801b99` queued)

C4 (pipelined replication and flow control: D15, D16, D18, and the hello's limits) as built in
`consensus`. This report holds C4's gate rows, run at `consensus:e1f249d` (the landing of
`consensus-rpc-lane`) and rerun at `consensus` main `consensus:8801b99`. From `e1f249d` to `8801b99`
`consensus` adds only the multicast seeds (`seeds.h`, `seeds_codec.h`, their tests and fuzz corpus);
no file these rows build or measure changed. A row gets its figures and job id from its jobs, never from a
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

- Each row is a campaign job tagged to quest `track-c-gates`, at `consensus:e1f249d` and `8801b99`, built with
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
| **T7a** One group's throughput at 0 ms added round trip, `pipeline = 4` against `pipeline = 1` | Pipelined at least as fast as `pipeline = 1`; within 20% of the follower's barrier-bound rate | `consensus-flow-bench` A1: `flow_bench`, one group, three voters, follower on real storage, exclusive | At `e1f249d`, modeled 1 ms barrier, 256 KiB frames, median of three: 55.2 MB/s pipelined (55.0-57.0) against 52.0 MB/s (43.9-55.4) at `pipeline = 1`, 1.06 times faster. The 20% clause is not computed: the bench prints no follower barrier-bound rate (open for the review) | First clause passes at `e1f249d`; rerun at `8801b99` queued | `261120-123441-f9ff24` (`e1f249d`); `261120-152013-0bef2c` (`8801b99`) | nothing |
| **T7b** The same at 1 ms | Pipelined faster than `pipeline = 1` | as T7a, `netem` delay 1 ms | At `e1f249d`: 55.3 MB/s (53.5-55.5) against 37.6 MB/s (36.1-38.8), 1.47 times faster | Pass at `e1f249d`; rerun queued | `261120-123441-f9ff24`; `261120-152013-0bef2c` | nothing |
| **T7c** The same at 10 ms | Pipelined at least 4 times `pipeline = 1`, and within 20% of the follower's barrier-bound rate | as T7a, `netem` delay 10 ms | At `e1f249d`: 54.9 MB/s (54.9-56.3) against 16.8 MB/s (16.7-16.9), **3.27 times, below the 4 times target**; with `fdatasync` (one run) 72.8 against 17.6 MB/s, 4.14 times. Pipelined throughput is flat at 55 MB/s from 0 to 10 ms: the leader's storage holds it (`docs/perf/flow-control.md`, A1) | **Miss** at `e1f249d` (modeled barrier); rerun queued | `261120-123441-f9ff24`; `261120-152013-0bef2c` | nothing |
| **T7d** 100 groups on one connection, one follower's barrier slowed 10 times | Every other group's commit p99 within 1.5 times the unslowed run; the slow group's unsent bytes within the transport's stated bound | `consensus-flow-bench` A2, exclusive, an unslowed run beside the slowed run | At `e1f249d`, node 2's apply p99 for the other 99 groups (the comparator `flow-control.md` uses), hot arms, three runs each: 36.7-40.7 ms slowed against 39.7-60.1 ms unslowed at 1 ms, 50.4-56.3 against 49.6-58.3 ms at 10 ms. Group 0's undelivered bytes at most 768 KiB (1 MiB with `fdatasync`), under the stated 4 MiB window plus 8 MiB queue. The 4 KiB arm (one run each): 63.6 against 46.7 ms (1.36 times) at 1 ms, and **83.6 against 53.5 ms (1.56 times) at 10 ms, past 1.5 times** (its leader commit p99 63.9 against 32.9 ms) | Hot arms pass; the 10 ms 4 KiB single run misses; rerun queued | `261120-123443-3f67e6`; `261120-152015-3b7ebd` | nothing |
| **R19** The longest control-frame gap behind Bulk traffic | Within the bound the transport states (one data frame, one 64 KiB feed and the kernel's buffers); far under `election_min` (1 s) | `consensus-flow-bench` A3, with the host-transport `upload` case as its correctness companion | At `e1f249d`, control-frame delay max against the stated bound. R19's case (256 KiB frames on the Control lane): 10.9 against 12.0 ms (0 ms), 13.3 against 16.9 (1 ms), 22.7 against 34.2 (10 ms), 37.3-44.4 against 89.7-102.8 ms at 1 Gb/s. The 2 MiB Control rows exceed it by 0.2 ms (14.8 against 14.6, 0 ms) and 0.8 ms (19.5 against 18.7, 1 ms), within the one-way link delay the measure over-counts. Bulk, 2 MiB: 4.0 to 38.0 ms. Every row is under 58 ms, far under 1 s | Pass at `e1f249d` (R19's 256 KiB case); rerun queued | `261120-123446-52c072`; `261120-152017-dd170c` | nothing |
| **T7e** Snapshot throughput at 10 ms added round trip | At least 50 MB/s | chunks on the bulk lane under group credit | Not runnable: `group-install` was folded into `group-heal`, which is planned and has no branch; no code moves a snapshot's chunks on the bulk lane under group credit | Not built | none | `group-heal` |
| **D18** A transfer's catch-up retry from a newer checkpoint | The second attempt sends only the files the receiver lacks | `group-install` A2 and A8; its jobs are cited here | Not runnable, as T7e | Not built | none | `group-heal` |
| **H1** A leader holds back an entry above a peer's hello-reported command limit | The entry is held back; a mutant that ignores the reported limits fails the test | `consensus-hello-limits` A1: a job, and a job on the plain test at the mutant that passes when it fails | At `e1f249d`: `consensus_transport_hello_leader` passed; at the mutant `hello-limits-ignored` (`consensus:53b223a`) it failed, so the control passed (ctest exit 8). At `8801b99` the mutant is `consensus:f066c51` (`test/transport/rpc_mutants.py 8801b99 hello-limits-ignored`) | Pass at `e1f249d`; rerun queued | `261120-123421-4452cc`, `261120-123429-dee769`; `261120-151953-846669`, `261120-151943-7cef64` | nothing |
| **H2** A hello whose limits fail `validate_limits` | Refused | `consensus-hello-limits` A2 | At `e1f249d`: `consensus_transport_hello_codec` and `consensus_transport_node_hello` passed (zero or inverted limits refused, each malformed field) | Pass at `e1f249d`; rerun queued | `261120-123421-4452cc`; `261120-151953-846669` | nothing |
| **H3** A peer outside the version range | Refused with a named error | `consensus-hello-limits` A3 | At `e1f249d`: `consensus_transport_hello_version` passed (a host speaking only wire version 2 never connects to one speaking 1; `WireVersionRefused`) | Pass at `e1f249d`; rerun queued | `261120-123421-4452cc`; `261120-151953-846669` | nothing |
| **H4** A host refuses a `maximum_frame` or group window below what its `rpc_bytes` needs | Refused at open | `consensus-hello-limits` tests | At `e1f249d`: `consensus_transport_hello_version` passed (a 1 MiB `maximum_frame` for a 2 MiB `rpc_bytes` is refused at construction); a group window below the frame is not refused, by design (`docs/mechanisms/hello.md`) | Pass at `e1f249d`; rerun queued | `261120-123421-4452cc`; `261120-151953-846669` | nothing |
| **T8** One CPU-hour of fuzzing on the hello's decoder | No crash, no timeout, no leak | `consensus-hello-limits` A4: libFuzzer with ASan and UBSan, at least one CPU-hour of executions at its commit, recorded with the executed-unit count | At `e1f249d`, Clang (Homebrew LLVM), ASan and UBSan, `FUZZ_DECODER=9` on the checked-in corpus, four runs of 1,000 s from seeds 1-4: 34,169,910, 34,806,581, 48,083,799 and 51,084,971 executed units (168,145,261 in total), all exit 0 with no crash, timeout or leak artifact; CPU time (user + sys, `time -p`) 973.8, 982.1, 990.5 and 994.8 s, 3,941 s in total (65.7 CPU-minutes). Coverage stops at 119 edges and 135 features in every run | Pass at `e1f249d`; rerun queued | `261120-123408-3ac775` (build), `261120-123410-8a605b`, `261120-123412-167bb5`, `261120-123414-ecad78`, `261120-123416-4346ee`; at `8801b99`: `261120-151928-c212a8` (build), `261120-151930-aa8e3e`, `261120-151933-c7a05f`, `261120-151935-1303bf`, `261120-151937-91e7a8` | nothing |
| **Suite** The consensus suite and the mutants at the landing commits | All pass, every mutant killed | `ctest -E consensus_mutants`, and `tools/mutants.sh BUILD 400 10000` outside ctest, as separate jobs on macOS, Linux and FreeBSD (Detent's ruling on msg-170, until `consensus-mutants-per-test` gives each mutant its own ctest) | At `e1f249d`, ctest with `consensus_mutants` inside: every other test passed on macOS (79 of 80) and Linux (79 of 80); `consensus_mutants` hit ctest's 1,800 s TIMEOUT with 14 of 16 verdicts printed (macOS) and 9 of 16 (Linux), each a kill, none at its own deadline: the TIMEOUT is too short for 16 mutants on a loaded host, not a hang. FreeBSD was skipped behind the Linux job | Rerun at `8801b99` queued | `261120-123419-22485b`, `261120-123449-c0320e` (`e1f249d`, timeout); at `8801b99`: macOS `261120-151939-4e57e4` (suite), `261120-151941-ce0b7a` (mutants); Linux `261120-152231-ab18c1`, `261120-152233-5ce427`; FreeBSD `261120-152240-f54066`, `261120-152243-1ec7d9` | Linux suite behind the flow-bench and C2 VM jobs |
| **CI** The platform CI at that commit | Green on every platform | the landing quests' own CI runs | `e1f249d`: run `44778137504`, all 8 platforms success. `8801b99`: run `44829606479`, queued | Pass at `e1f249d`; pending at `8801b99` | `github.com/Kronuz/consensus/actions/runs/44778137504`, `.../44829606479` | nothing |

## Rows that stay with other quests

- T4's 10 GiB install at 10 ms with 1,000 groups on the leader's node is `group-install`'s
  (A4's shape and its 1 GiB file sets) and `group-core`'s; C4 cites its figure for T7e only.
- D16's connection-wide window and R19's `TCP_NOTSENT_LOWAT` bound are unbuilt. A row for either
  belongs to a quest that builds it; this report records the transport's stated bound.

## Review

The architect's review is `docs/reviews/track-c-c4.md` (A3 of `track-c-gates`), written
after the rows above have results.
