# Track C, C4 gate report (results at `e1f249d` and at main `8801b99`)

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
| **T7a** One group's throughput at 0 ms added round trip, `pipeline = 4` against `pipeline = 1` | Pipelined at least as fast as `pipeline = 1`; within 20% of the follower's barrier-bound rate | `consensus-flow-bench` A1: `flow_bench`, one group, three voters, follower on real storage, exclusive | Modeled 1 ms barrier, 256 KiB frames, median of three. At `e1f249d`: 55.2 MB/s pipelined (55.0-57.0) against 52.0 MB/s (43.9-55.4) at `pipeline = 1`, 1.06 times faster. At `8801b99`: 54.0 MB/s (53.3-54.1) against 46.5 MB/s (41.8-49.8), 1.16 times faster. The 20% clause is not computed: the bench prints no follower barrier-bound rate (open for the review) | First clause passes at both; 20% clause not computed | `261120-123441-f9ff24` (`e1f249d`); `261120-152013-0bef2c` (`8801b99`) | nothing |
| **T7b** The same at 1 ms | Pipelined faster than `pipeline = 1` | as T7a, `netem` delay 1 ms | At `e1f249d`: 55.3 MB/s (53.5-55.5) against 37.6 MB/s (36.1-38.8), 1.47 times faster. At `8801b99`: 54.9 MB/s (53.8-55.0) against 33.2 MB/s (33.1-33.6), 1.65 times faster | Pass at both | `261120-123441-f9ff24`; `261120-152013-0bef2c` | nothing |
| **T7c** The same at 10 ms | Pipelined at least 4 times `pipeline = 1`, and within 20% of the follower's barrier-bound rate | as T7a, `netem` delay 10 ms | At `e1f249d`: 54.9 MB/s (54.9-56.3) against 16.8 MB/s (16.7-16.9), **3.27 times, below the 4 times target**; with `fdatasync` (one run) 72.8 against 17.6 MB/s, 4.14 times. At `8801b99`: 50.8 MB/s (50.3-51.3) against 16.0 MB/s (15.3-16.2), **3.18 times**; with `fdatasync` (one run) 59.6 against 17.7 MB/s, **3.37 times**. Pipelined throughput is flat from 0 to 10 ms (54 to 51 MB/s at `8801b99`): the leader's storage holds it (`docs/perf/flow-control.md`, A1). For the review: the bench's `frame 0` arm at 10 ms runs faster at `pipeline = 1` than at 4 (79.7 against 68.1 MB/s at `8801b99`, 93.6 against 57.7 at `e1f249d`, one run each) | **Miss** at both commits (modeled barrier; and `fdatasync` at `8801b99`) | `261120-123441-f9ff24`; `261120-152013-0bef2c` | nothing |
| **T7d** 100 groups on one connection, one follower's barrier slowed 10 times | Every other group's commit p99 within 1.5 times the unslowed run; the slow group's unsent bytes within the transport's stated bound | `consensus-flow-bench` A2, exclusive, an unslowed run beside the slowed run | Node 2's apply p99 for the other 99 groups (the comparator `flow-control.md` uses), hot arms, three runs each. At `e1f249d`: 36.7-40.7 ms slowed against 39.7-60.1 ms unslowed at 1 ms, 50.4-56.3 against 49.6-58.3 ms at 10 ms; the 4 KiB arm (one run each) 63.6 against 46.7 ms (1.36 times) at 1 ms and **83.6 against 53.5 ms (1.56 times) at 10 ms, past 1.5 times** (its leader commit p99 63.9 against 32.9 ms). At `8801b99`: 35.5-36.7 against 36.0-45.4 ms at 1 ms, 49.3-51.2 against 50.6-50.8 ms at 10 ms; the 4 KiB arm 35.7 against 35.9 ms at 1 ms and 50.1 against 49.6 ms (1.01 times) at 10 ms (leader commit p99 29.8 against 29.0 ms). Group 0's undelivered bytes at most 768 KiB at both (1 MiB with `fdatasync`), under the stated 4 MiB window plus 8 MiB queue | Pass at `8801b99`, the 4 KiB arm included (one run each); at `e1f249d` the 10 ms 4 KiB single run missed | `261120-123443-3f67e6`; `261120-152015-3b7ebd` | nothing |
| **R19** The longest control-frame gap behind Bulk traffic | Within the bound the transport states (one data frame, one 64 KiB feed and the kernel's buffers); far under `election_min` (1 s) | `consensus-flow-bench` A3, with the host-transport `upload` case as its correctness companion | Control-frame delay max (send to arrival: the wait behind data plus the one-way delay) against the stated bound. At `e1f249d`, R19's case (256 KiB frames on the Control lane): 10.9 against 12.0 ms (0 ms), 13.3 against 16.9 (1 ms), 22.7 against 34.2 (10 ms), 37.3-44.4 against 89.7-102.8 ms at 1 Gb/s; the 2 MiB Control rows exceed it by 0.2 ms (0 ms) and 0.8 ms (1 ms), within the one-way delay the measure over-counts. At `8801b99`, two runs (first, repeat): R19's case at 0 ms **59.0 against 15.1 ms** (p99 40.0, p50 1.0) then 7.8 against 11.5 ms, so the first run's excursion did not repeat; 9.1 and 10.8 against 14.7 and 15.1 (1 ms); 21.8 and 22.1 against 34.4 and 34.7 (10 ms); 33.4-46.2 and 37.6-45.1 against 87.0-103.8 ms at 1 Gb/s. The 2 MiB Control row, not R19's case (R19 caps an append at 256 KiB), is past at 0 ms in both runs (29.9 against 14.6, 24.4 against 12.8 ms) and at or under it elsewhere; Bulk is under in every row (0 ms: 2.6 and 3.0 against 13.8 and 13.5 ms). Longest gap, against the bound plus the probe's idle floor (below, "R19's gap floor"): the idle rows read 12.0-12.7 ms in the repeat (12.0-13.9 ms in the first run); at 0 ms the repeat's loaded gaps are past the bare bound by 0.3-1.8 ms (13.1 against 12.8 ms, 2 MiB Control; 12.5 against 11.5, 256 KiB Control; 15.3 against 13.5, Bulk), as the first run's were, and 1.0, 0.4 and 3.2 ms above the 12.1 ms idle floor, under each bound. Every row of both runs is far under `election_min`: worst delay max 59.0 ms, worst gap 53.7 ms, against 1,000 ms. The leader's 0-2 drops on 2 MiB rows are the bench's surplus data frames (below), never a control frame: all 300 arrived in every row | Pass at `e1f249d`. At `8801b99`: R19's case under its bound in the repeat, **past at 0 ms in 1 of 2 runs** (as 1 of 5 in `flow-control.md`'s A3); every gap under the bound plus the idle floor; the 2 MiB Control row past at 0 ms in both runs | `261120-123446-52c072`; `261120-152017-dd170c`; repeat `261120-200724-273aa8` | nothing |
| **T7e** Snapshot throughput at 10 ms added round trip | At least 50 MB/s | chunks on the bulk lane under group credit | Not runnable: `group-install` was folded into `group-heal`, which is planned and has no branch; no code moves a snapshot's chunks on the bulk lane under group credit | Not built | none | `group-heal` |
| **D18** A transfer's catch-up retry from a newer checkpoint | The second attempt sends only the files the receiver lacks | `group-install` A2 and A8; its jobs are cited here | Not runnable, as T7e | Not built | none | `group-heal` |
| **H1** A leader holds back an entry above a peer's hello-reported command limit | The entry is held back; a mutant that ignores the reported limits fails the test | `consensus-hello-limits` A1: a job, and a job on the plain test at the mutant that passes when it fails | `consensus_transport_hello_leader` passed at `e1f249d` and at `8801b99`. At the mutant `hello-limits-ignored` it failed, so the control passed (ctest exit 8): `consensus:53b223a` over `e1f249d`, and `consensus:f066c51` over `8801b99` ("appends to 3 carrying it: 1 FAILED") | Pass at both | `261120-123421-4452cc`, `261120-123429-dee769`; `261120-151953-846669`, `261120-151943-7cef64` | nothing |
| **H2** A hello whose limits fail `validate_limits` | Refused | `consensus-hello-limits` A2 | `consensus_transport_hello_codec` and `consensus_transport_node_hello` passed at both commits (zero or inverted limits refused, each malformed field) | Pass at both | `261120-123421-4452cc`; `261120-151953-846669` | nothing |
| **H3** A peer outside the version range | Refused with a named error | `consensus-hello-limits` A3 | `consensus_transport_hello_version` passed at both commits (a host speaking only wire version 2 never connects to one speaking 1; `WireVersionRefused`) | Pass at both | `261120-123421-4452cc`; `261120-151953-846669` | nothing |
| **H4** A host refuses a `maximum_frame` or group window below what its `rpc_bytes` needs | Refused at open | `consensus-hello-limits` tests | `consensus_transport_hello_version` passed at both commits (a 1 MiB `maximum_frame` for a 2 MiB `rpc_bytes` is refused at construction); a group window below the frame is not refused, by design (`docs/mechanisms/hello.md`) | Pass at both | `261120-123421-4452cc`; `261120-151953-846669` | nothing |
| **T8** One CPU-hour of fuzzing on the hello's decoder | No crash, no timeout, no leak | `consensus-hello-limits` A4: libFuzzer with ASan and UBSan, at least one CPU-hour of executions at its commit, recorded with the executed-unit count | Clang (Homebrew LLVM), ASan and UBSan, `FUZZ_DECODER=9` on the checked-in corpus, runs of 1,000 s from seeds 1-4. At `e1f249d`: 34,169,910, 34,806,581, 48,083,799 and 51,084,971 executed units (168,145,261 in total); CPU time (user + sys, `time -p`) 973.8, 982.1, 990.5 and 994.8 s, 3,941 s in total (65.7 CPU-minutes). At `8801b99`: 43,626,468, 17,719,274, 25,924,755 and 17,622,051 units (104,892,548 in total); CPU 924.8, 611.0, 807.7 and 615.0 s, 2,958.6 s (49.3 CPU-minutes: the runs shared a loaded host), so a fifth run from seed 5 tops it up: 44,555,784 units, CPU 991.5 s; in total 149,448,332 units and 3,950.1 s (65.8 CPU-minutes). Every run exits 0 with no crash, timeout or leak artifact; coverage stops at 119 edges and 135 features in every run | Pass at both | `261120-123408-3ac775` (build), `261120-123410-8a605b`, `261120-123412-167bb5`, `261120-123414-ecad78`, `261120-123416-4346ee`; at `8801b99`: `261120-151928-c212a8` (build), `261120-151930-aa8e3e`, `261120-151933-c7a05f`, `261120-151935-1303bf`, `261120-151937-91e7a8`, `261120-200804-fd5a96` | nothing |
| **Suite** The consensus suite and the mutants at the landing commits | All pass, every mutant killed | `ctest -E consensus_mutants`, and `tools/mutants.sh BUILD 400 10000` outside ctest, as separate jobs on macOS, Linux and FreeBSD (Detent's ruling on msg-170, until `consensus-mutants-per-test` gives each mutant its own ctest) | At `e1f249d`, ctest with `consensus_mutants` inside: every other test passed on macOS (79 of 80) and Linux (79 of 80); `consensus_mutants` hit ctest's 1,800 s TIMEOUT with 14 of 16 verdicts printed (macOS) and 9 of 16 (Linux), each a kill, none at its own deadline. At `8801b99`: macOS ctest 91 of 91; macOS `tools/mutants.sh` 16 of 16 killed in 2,616 s, 81 to 428 s each (the longest "repair ignores the term at the base boundary"); Linux suite from consensus CI (run `44829606479`, `linux-x86_64-gcc`, `-clang` and `linux-arm64-gcc` green; the VM job `261120-152231-ab18c1` was cancelled for it); Linux `tools/mutants.sh` 16 of 16 killed in 1,900 s, 100 to 175 s each (the longest "votes and pre-votes without the log check"); FreeBSD ctest 91 of 91 (162 s); FreeBSD `tools/mutants.sh` 16 of 16 killed in 1,764 s, 94 to 140 s each (the longest "a leader grants pre-votes"). The VM jobs' first submits `261120-152233-5ce427`, `261120-152240-f54066`, `261120-152243-1ec7d9` were skipped behind a failed or cancelled job | Pass on macOS, Linux and FreeBSD at `8801b99` | `261120-123419-22485b`, `261120-123449-c0320e` (`e1f249d`, timeout); at `8801b99`: macOS `261120-151939-4e57e4` (suite), `261120-151941-ce0b7a` (mutants); Linux `ci:` run `44829606479` (suite), `261120-200605-743b1d` (mutants); FreeBSD `261120-200606-fa2caa` (suite), `261120-200607-b7499c` (mutants) | nothing |
| **CI** The platform CI at that commit | Green on every platform | the landing quests' own CI runs | `e1f249d`: run `44778137504`, all 8 platforms success. `8801b99`: run `44829606479`, success on all 8 platform jobs (Linux x86_64 gcc and clang, Linux arm64, macOS x86_64 and arm64, FreeBSD x86_64, ASan+UBSan, TSan); the headers job skipped | Pass at both | `github.com/Kronuz/consensus/actions/runs/44778137504`, `.../44829606479` | nothing |

## R19's gap floor and the leader's drops

Both from `bench/flow_bench.cc` and `include/consensus/transport/host_transport.h` at
`consensus:8801b99`, and the jobs `261120-152017-dd170c` and `261120-200724-273aa8`.

**The gap floor.** `gap` sends one control frame every 10 ms by construction: its loop sends a
heartbeat when `us_now() >= next`, advances `next += 10'000`, tops up the data and sleeps 500 us
(`flow_bench.cc:788-795`). The longest gap between arrivals is therefore at least the 10 ms
cadence, plus up to 0.5 ms of polling, the VM's oversleep of `sleep_for` and the spread of
delivery (the idle 0 ms row's delay max less its p50 is 1.0 ms). The idle `lane none` rows read
12.0-12.7 ms in every netem setting of the repeat, 12.0-13.9 ms in the first run, 11.6-13.1 ms
in `flow-control.md`'s A3. The stated bound is a wait, not a gap, and omits that floor, so a gap
is compared with the bound plus the same setting's idle gap. Delay max, the send-to-arrival
time, carries no cadence and is the direct measure of R19's wait; it over-counts only the
one-way delay (0.5 ms idle p50 at 0 ms).

**The leader's drops.** "leader dropped" is `stats(0).dropped + stats(1).dropped` of the
leader's transport. The repeat reads 0 on every idle and 256 KiB row and 0-2 on the 2 MiB rows
(the first run: 1-2 on all but its 0 ms Bulk row). Of `dropped`'s two sites, `send`'s
(`host_transport.h:358`) does not apply: `maximum_frame` is 4 MiB by default
(`connection.h:31`), the bench leaves it, and a 2 MiB frame is 2,097,177 bytes with its 25-byte
header (a frame over the cap would drop every frame, not 1 in about 1,400 at 937-987 MB/s); and
every group is stored before the first send and never forgotten. The drops are `enqueue`'s
(`host_transport.h:694-695`): a group's data frame is dropped when its queue would pass
`data_queue`, 8 MiB. With headers, three queued 2 MiB frames are 6,291,531 bytes and a fourth
passes 8 MiB by 100 bytes; the 4 MiB `window` likewise holds one 2 MiB frame, not two (two
pass it by 50 bytes). A group therefore holds at most four 2 MiB frames, one outstanding and
three queued. The bench's `top_up` posts one frame per group per pass until sent less delivered
reaches `4 × (window + frame)`, 24 MiB across the four groups, and counts a frame at its post, so
a pass can end near 32 MiB, four frames per group on average; a group behind the others then gets
a fifth, and it is dropped. At 256 KiB, 31 frames fit the queue and the bench never posts that many. This is the
transport's stated overflow (Raft resends), not a fault: the 2 MiB rows measure the control
wait with each group's queue at its cap and one frame in flight, which is the load they intend;
a dropped frame is never sent, so it adds no wait and does not count in the delivered rate.

## Rows that stay with other quests

- T4's 10 GiB install at 10 ms with 1,000 groups on the leader's node is `group-install`'s
  (A4's shape and its 1 GiB file sets) and `group-core`'s; C4 cites its figure for T7e only.
- D16's connection-wide window and R19's `TCP_NOTSENT_LOWAT` bound are unbuilt. A row for either
  belongs to a quest that builds it; this report records the transport's stated bound.

## Review

The architect's review is `docs/reviews/track-c-c4.md` (A3 of `track-c-gates`), written
after the rows above have results.
