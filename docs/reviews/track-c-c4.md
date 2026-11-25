# track-c-gates review: C4 gate (architect)

Commit: strata:f46af14b179a5a500e239106feadd192928a1dbc (the report as of strata:605f1dd5fc6d263ea21e86888828962bf5b08770), judging rows at consensus:8801b99025ba4f094e2566e7284d76b5b119a8b0 and consensus:e1f249d8d9ff64410f87042ba90f4c218d9ec336
Verdict: APPROVE WITH CONDITIONS
Date: 2026-11-21

Conditions 1 and 2 are text fixes to `docs/gates/track-c-c4.md` (findings F1 and F2, both
blocking). The report lands once they are in; they need no rerun, and the lead checks them
against the logs cited here. Conditions 3 to 5 must hold before C4 is called passed, and C4 is
not passed at `8801b99`: T7c misses, T7e and D18 are not built, and no Driver host can turn on
D15.

Scope: `docs/gates/track-c-c4.md`, read against each job's raw log and `spec.json`. Code read at
consensus `8801b99`:

- `bench/flow_bench.cc`: `gap()` (710-814), the `Node` harness (360-380), the header (24-40).
- `include/consensus/transport/host_transport.h`: options 226-229, `send` 349-366,
  `data_ready` 603-604, `enqueue` 686-707, `feed` 741-783.
- `include/consensus/transport/connection.h:31`, `types.h:61, 119-127`, and
  `worker.h:150, 178-179, 689`.
- `docs/perf/flow-control.md:31-32`.

Code read in strata: `include/strata/group/*` on `quest/xapiand/group-core`. No GPL sources were
read.

## Evidence I relied on

| Job | Row | Result in the log |
|---|---|---|
| 261120-152013-0bef2c | T7a to T7c, 8801b99 | Every median and range in the report matches the log (for example 10 ms: 15.3, 16.0 and 16.2 against 50.3, 51.3 and 50.8 MB/s, so 3.18 times). The barrier lines print `fdatasync ... 711.7 MB/s` and `modeled 1000 us ... 639.5 MB/s` (log lines 4-5) |
| 261120-152015-3b7ebd | T7d, 8801b99 | 4 KiB arm, node 2's apply p99 35.9 against 35.7 ms (1 ms) and 49.6 against 50.1 ms (10 ms); at most 786,432 bytes undelivered (1,048,576 with `fdatasync`) |
| 261120-152017-dd170c, 261120-200724-273aa8, 261120-123446-52c072 | R19 at 8801b99 (two runs) and e1f249d | Tabulated by my script (below); the log's tree hashes match `8801b99`'s `bench` and `include` |
| 261120-151941-ce0b7a, 261120-200605-743b1d, 261120-200607-b7499c | Mutants on macOS, Linux and FreeBSD | `tools/mutants.sh exit 0 after 2616 s / 1900 s / 1764 s, 16 verdicts`, each with at least one suite `yes` (the script exits 1 when a mutant survives, `tools/mutants.sh:74-76`). Per-mutant times 81-428 s, 100-175 s and 94-140 s, with the longest named as in the report |
| 261120-200606-fa2caa | FreeBSD suite | `100% tests passed, 0 tests failed out of 91`, 162.21 s, commit 8801b99 |
| 261120-151939-4e57e4 | macOS suite | `100% tests passed out of 91` |
| CI run 44829606479 | Linux suite, every platform | Head SHA `8801b99`, conclusion success, every job green; the headers job skipped (`gh run view`) |
| 261120-151930-aa8e3e, -151933-c7a05f, -151935-1303bf, -151937-91e7a8, 261120-200804-fd5a96 | T8 at 8801b99 | Decoder 9 (`decode_node_hello`, round-tripped, `fuzz_decoders.cc:119-122`), seeds 1-5. Executed units 43,626,468 + 17,719,274 + 25,924,755 + 17,622,051 + 44,555,784 = 149,448,332. `time -p` user+sys 924.80 + 611.00 + 807.73 + 615.03 + 991.54 = 3,950.10 s, **65.8 CPU-minutes**. Every run exits 0 with no artifact; `-fsanitize=fuzzer,address,undefined` (`CMakeLists.txt:146-147`); 119 edges and 135 features |
| 261120-151953-846669, 261120-151943-7cef64 | H1 to H4 | Hello tests pass at 8801b99. At `consensus:f066c51`, which is 8801b99 plus a one-line mutant in `host_transport.h`, `consensus_transport_hello_leader` fails, and the job passes because it does |

Question 4 is settled: the mutant (16 of 16 on all three platforms), FreeBSD suite (91 of 91) and
T8 (65.8 CPU-minutes) figures are what the jobs show.

## What I checked myself

Scripts are under `.scratch/xapiand/review-track-c-gates-architect/` in the Xapiand checkout.
`r19_table.py LOG...` prints each gap row's delay max against the stated bound, and its longest
gap against the bound plus the same setting's idle gap. No machine probe was needed for C4: the
open questions are settled by the logs and the code, or need exclusive timing (Condition 3).

R19 rows past a bound, from the script (ms):

| Job | Setting | Row | Delay max / bound | Longest gap / bound + idle gap |
|---|---|---|---|---|
| dd170c (8801b99, run 1) | 0 ms | Control, 256 KiB | **59.0 / 15.1** (p99 40.0) | **34.4 / 27.2** |
| dd170c | 0 ms | Control, 2 MiB | **29.9 / 14.6** | 18.1 / 26.7 |
| 273aa8 (8801b99, run 2) | 0 ms | Control, 2 MiB | **24.4 / 12.8** | 13.1 / 24.9 |
| 52c072 (e1f249d) | 0 ms | Control, 2 MiB | **14.8 / 14.6** | 13.9 / 26.2 |
| 52c072 | 1 ms | Control, 2 MiB | **19.5 / 18.7** | 15.3 / 30.7 |
| 52c072 | 10 ms | Control, 256 KiB | 22.7 / 34.2 | **52.3 / 46.3** |

Every other row of the three runs is under both bounds. The worst figures are a delay max of
59.0 ms and a gap of 53.7 ms, against `election_min` 1,000 ms.

### Question 2: is the floor argument sound, and is any row a residual?

The floor is sound for the gap and does not apply to the delay. `gap()` timestamps each control
frame inside the loop's handler and sends one per 10 ms on an absolute schedule
(`flow_bench.cc:788-795`). So a gap is the cadence, plus the change in delay between two frames,
plus the sender's jitter, and the idle row measures cadence plus jitter (12.0-13.9 ms). Comparing
a loaded gap with the bound plus the idle gap is fair. But the gap is the weaker statistic: one
frame delayed by D shows a gap of only about 10 + D - d(previous). The delay max is the direct
measure of R19's wait, and the report says as much. The floor does not excuse a delay past the
bound.

Measured that way, the stated bound does not hold at 0 ms. R19's own case is past it in 1 of the
2 runs at 8801b99 (3.9 times the bound, with a p99 of 40 ms, so a stall over several frames, not
one late sample), and the report says 1 of 5 in `flow-control.md`'s A3. The 2 MiB Control row is
past it in every run at 0 ms. Nothing attributes the excursion: it could be scheduling (two
transports of two loops each, the feeding thread and the kernel's loopback on 4 vCPUs), or a
stall in the transport. Under the Reconciliation's decision 4 (strata:f2358007, "R19 (lead)"),
R19 stands on `election_min`, and every row is at least 17 times under it. So R19 passes its
operative clause. The stated bound is a residual (F3, Condition 3).

### Question 3: are the "leader dropped" frames the 8 MiB `data_queue` overflow?

Yes. `stats.dropped` is incremented in two places only. The first, `send`
(`host_transport.h:358-359`), is not reached: a 2 MiB frame is 2,097,177 bytes against a
`maximum_frame` of 4 MiB (`connection.h:31`), and `gap()` stores every group before it starts.
The second, `enqueue` (`:694-695`), drops a data frame when `queued + frame > data_queue` and a
frame is already queued. A frame moves from `queued` to `outstanding` when it is fed (`:777-778`).
The window admits a second frame only while `outstanding + frame <= window` (`:603-604`), and two
2 MiB frames pass it by 50 bytes. A group therefore holds one outstanding frame and three queued
ones; a fourth queued frame passes 8 MiB by 100 bytes. `top_up` stops on sent minus delivered,
not on credit, so a group whose delivered frame's credit is still in flight takes one more frame,
and the transport drops it.

So the drops are the transport's stated per-group cap, and the bench overfills it. Control frames
are never dropped: 300 of 300 arrive in every row. Detent's `consensus-transport-budgets` (A1:
two `rpc_bytes` frames fit the window and four fit the queue; A2: no drops in these rows) is the
right owner, and the attribution holds.

## Findings

| ID | Blocking | Kind | Finding | Evidence | Fix |
|---|---|---|---|---|---|
| F1 | yes | gap | The report does not say that no Driver host can select `Flow`. `Worker` builds every `Core` with `Flow{}`: one flight, and `frame_bytes = 0`, so appends go up to `rpc_bytes`, 2 MiB. `strata::group` sets no flow either. T7a to T7d measure `Core` driven directly by the bench, which `flow-control.md:31-32` states. Two consequences: "What is built" lists D15 and the frame cap as built, but no host on the drivers can reach them; and R19's dismissal of the 2 MiB Control row as "not R19's case (R19 caps an append at 256 KiB)" is false for every Driver host, where 2 MiB appends are the deployed case. | `worker.h:150, 178-179, 689`; `types.h:61, 123-127`; `flow_bench.cc:377`; no `Flow` or `frame_bytes` in `include/strata/group/*` (group-core) | Condition 1 |
| F2 | yes | defect | Two claims in R19's status do not follow from the numbers. "Every gap under the bound plus the idle floor" is false for the first run at 8801b99: dd170c's R19 case at 0 ms has a 34.4 ms gap against 27.2. "As the first run's were" (past the bare bound by 0.3-1.8 ms) is false for that row, which is 19.3 ms past. The e1f249d row "Pass" also hides a 10 ms 256 KiB Control gap of 52.3 against 46.3 ms. | `r19_table.py` on dd170c and 52c072 (table above) | Condition 2 |
| F3 | no | gap | The transport's stated bound does not hold at 0 ms. Delay max is 59.0 against 15.1 ms (R19's case, 1 of 2 runs) and 24.4-29.9 against 12.8-14.6 ms (2 MiB, every run), with no attribution. R19 passes on `election_min` by decision 4, but `docs/mechanisms/host-transport.md` states a bound that the measurements break. | Table above | Condition 3 |
| F4 | no | gap | Ruling on the item left open: T7's 20% clause can be computed, and it misses. The job prints the follower storage's own rate: modeled 1 ms barrier, 639.5 MB/s for 2 MiB barriers. Pipelined 54.0 MB/s at 0 ms is 8.4% of that, and 50.8 MB/s at 10 ms is 7.9%. The bench's leader persists one 256 KiB proposal at a time (`flow-control.md`, `one`), so the run measures the leader's proposal path, not the follower's barrier bound. T7c's 3.18 times has the same cause. | 0bef2c log lines 4-5, 12, 114 | T7a's and T7c's status read "20% clause: miss (8.4%, 7.9%), leader-bound" (part of Condition 2's edit). Condition 4 covers the rerun. |
| F5 | no | nit | The other item left open: at 10 ms the `frame 0` arm runs faster at `pipeline = 1` than at 4 (79.7 against 68.1 MB/s; p99 573 ms at 4). This fits F3's window arithmetic: the 4 MiB transport window holds one 2 MiB frame with its header, so pipeline 4's extra flights queue behind it. That is inferred, not measured. | 0bef2c lines 139, 144; `host_transport.h:226, 603-604` | Rerun the arm after `consensus-transport-budgets` lands. |
| F6 | no | nit | The report cites strata `docs/design/track-c.md`, "Reconciliation (2026-11-18)". That section is on `quest/xapiand/track-c` (strata:f2358007), not on strata main or this branch, so the link is dead for a reader of main. The C2 report and the quest file cite it the same way. | `git merge-base --is-ancestor f2358007 f46af14b`: not an ancestor | Cite `strata:f2358007`, or land the section. |

A finding that removes or narrows behavior is not involved. D15's reachability (Condition 5) is
the owner's choice of direction, and it is offered with options.

## Conditions

1. (F1, before the report lands) "What is built" says that `Worker` builds every `Core` with
   `Flow{}`, so a Driver host, `strata::group` among them, runs `pipeline = 1` with appends up to
   `rpc_bytes` (2 MiB), and that T7a to T7d measure `Core` with the bench's `Flow`. R19's cell
   drops "not R19's case" and treats the 2 MiB Control row as the deployed case.
2. (F2 and F4, before the report lands) R19's status states the first run's 0 ms R19 case as past
   both the bound (59.0 against 15.1 ms) and the bound plus floor (34.4 against 27.2 ms), and
   e1f249d's 10 ms 256 KiB gap as past the bound plus floor. T7a's and T7c's 20% clause reads as a
   leader-bound miss, with F4's figures.
3. (F3, before C4 is passed) The 0 ms excursion is attributed by a probe run as an `exclusive` job
   on the gate VM: per-thread scheduling and steal time, or a run with the loops pinned, beside a
   repeat of the A3 rows. Then either the transport is fixed, or `host-transport.md` states the
   measured envelope instead of a bound. The owner is consensus (Detent) or the lead. R19 meanwhile
   stands on `election_min`, as decision 4 rules.
4. (F4, before C4 is passed) T7c either reruns with a workload whose leader is not the bottleneck
   (proposals batched per persist), so the 4-times and 20% clauses measure the follower path, or
   the lead records the miss with this attribution and the owner accepts it. The lead's choice.
5. (F1, before C4 is passed; owner's call) D15 reaches the hosts that need it. Options:
   - (a) `Worker`/`Driver` options carry `Flow`, `strata::group` sets `Flow{4, 4 MiB, 256 KiB}`
     as D15 and R19 intend, and T7's 1 and 10 ms rows rerun through the Driver.
   - (b) D15 stays off for strata, recorded as the owner's decision, and T7 is read as Core-only.
   - (c) Keep `Flow{}` until `group-heal`, then (a).

   I recommend (a): at 10 ms, `pipeline = 1` at 256 KiB frames moves 16 MB/s.

   T7e and D18 remain owed to `group-heal`, as the report says.

## Not run

- No timing rerun. The R19 excursion's cause is open (Condition 3); settling it needs exclusive
  holds on the gate VM, which a review should not take.
- I did not rerun the suite, the mutants or the fuzzer; I read their logs. Sanitizers and the
  platform matrix are CI's (run 44829606479).
- I did not read `docs/mechanisms/host-transport.md` or `hello.md` in full, and I did not check
  T7d's e1f249d single-run miss beyond the report's own statement.
- I did not check Detent's builds at `8801b99`.
