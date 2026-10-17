# strata review: Track D1, the scale contract and the deterministic simulator (preliminary)

- Date: 2026-10-17.
- Reviewed: `docs/design/simulator.md` at `e305adb` (D1–D12, T1–T13, Q1–Q6, S1–S5). S1 at
  `b70f2b4`: `sim/include/simulation/{random,clock,engine}.hh` and
  `sim/test/{random,engine}_test.cc`. S3 at `45a15ca` and S2 at `dc640bb` (`checker.hh`,
  `network.hh`, and their tests), which landed during the review; the ledger at `57a55fb`;
  the include fix at `7d0b3fa` and its CI run 39060501864.
- Checked against:
  - PLAN: the process rule, Goals 3 and 5, Every gate (the scale statement), Track C (C5),
    Track D (D1, the fidelity tiers, the gate per item), and Phase 3R's gate.
  - DISTRIBUTION: the scale contract (cluster and per-node rows), Layers, Open decision 2.
    REPLICATION: Versioned writes. `docs/design/phase-3.md`: Keys and versions.
  - `docs/design/track-c.md` (D1, D5, D11, D12, D13, D15–D18, the timing profile, T2, T3,
    T6, T7, T9), `placement-strategies.md`, `limits.md`, `docs/perf/commit-barriers.md`.
  - The seams at the commits the design names: `consensus` `fc66c0e`, `journal` `3dccf50`,
    `completion` `5251cf9`, `reactor` `e19afd9`, `task` `bbb2e14`; `metrics` `c975765`;
    `cluster`; strata's `faultfs.hh` and `executor.hh`.
  - My Track C and reordering reviews, for the house style. `docs/reviews/calibration.md`
    does not exist yet.
  - From papers and documentation only: Ongaro's dissertation (§3.6, §3.8), SWIM, Lifeguard,
    memberlist's documented defaults, Pittel (1987) on rumor spreading, Wing and Gong, Lowe,
    Horn and Kroening, Jepsen's documentation (independent keys, indeterminate operations),
    FoundationDB (SIGMOD 2021), TigerBeetle's VOPR documentation, the AAPCS64 and Apple
    arm64 ABIs.
- Checked by:
  - The prescribed command on the shared worktree, configured at 11:47:23, just after
    `45a15ca`: Release, Apple clang 17.0.0, Ninja, nice 10, `-j4`; 153 of 153 steps in
    6 min 30 s, no warnings; `ctest -R 'sim|engine|random'`: 4 of 4 passed in 14.8 s. The
    contributor kept committing during that build, so the snapshot builds below are the
    evidence for each commit.
  - `sim/` alone from `git archive` snapshots of `b70f2b4` and `57a55fb`, each built three
    ways: Apple clang 17 RelWithDebInfo `-Wall -Wextra -Werror`; Homebrew LLVM 23.1.2 Debug
    with `-fsanitize=address,undefined -fno-sanitize-recover=all`; GCC 16.2 (libstdc++)
    Release (`-O3`) `-Wall -Wextra -Werror`. `b70f2b4`: 2 of 2 pass under all three.
    `57a55fb`: 4 of 4 pass under Apple clang, and the other two fail to compile (SIM-R18,
    fixed at `7d0b3fa`).
  - CI at `7d0b3fa` (run 39060501864): the four simulation tests pass on linux-x86_64-gcc,
    linux-x86_64-clang, linux-arm64-gcc, and linux-tsan. linux-asan-ubsan reaches its
    1-hour limit before them. macOS x86_64 and arm64 stop at
    `include/strata/bisection.hh:541` (`std::jthread`), and FreeBSD at its VM step: main's
    break, not the branch's.
  - Probes in `.scratch/review-sim/probes/`, each with its log: `probe_determinism.cc`,
    `probe_faultfs.cc`, `probe_checker_bruteforce.cc`, and `contract_numbers.py` (every
    recomputed number below). `probe_generation_wrap.cc` is written; its run did not finish.
  - Not run: the last line of this review.

## Verdict

**APPROVE WITH CONDITIONS: the shape is right and S1's determinism reproduces; seven
findings change what later steps build on, and each pauses only the steps it touches.**

What holds, and reproduces:

- One thread runs events in `(time, sequence)` order, so ties keep scheduling order
  (`engine.hh:134`; `engine_test.cc:75-87`, order "aBbxcd").
- S1's determinism (`probe_determinism`): seed 1 gives `0xa17f79e528ea2e4e` in three
  separate processes and under GCC 16 with libstdc++; 200 seeds give 200 distinct hashes;
  a one-event perturbation shows first in its own 65,536-event checkpoint. CI at `7d0b3fa`
  holds both pinned hashes on Linux x86_64 (gcc, clang) and Linux arm64 (gcc).
- Lemire's draw is exactly uniform (the exhaustive 16-bit test, `random_test.cc:45-53`).
  The quantile and clock arithmetic is integer, monotone, and minimal in its inverse
  (`random.hh:104-126`, `clock.hh:43-52`).
- S3's search is sound and complete on small histories. On 330,000 random single-key
  histories of up to 8 operations, with unknown operations and cuts, it agrees with an
  independent brute force in every case (`probe_checker_bruteforce`: 288,792 linearizable,
  41,208 violations, 187,681 unknown operations, 211,629 histories with more than one
  segment, 0 mismatches). Past its bounds it says inconclusive, never pass
  (`checker.hh:146, 225`).
- The starting point is accurate. The four harnesses' sizes and models match
  (`liveness_test.cc:186-193`), and `simulation_test.cc:549` checks only the necessary
  condition the design says it does. Every seam D7 cites exists at its commit: `Tick`
  (`types.h:176`, `core.h:433`), `Worker::run_one` (`worker.h:447`), `take_io_operation`
  (`:211`), the turn-driver constructor (`:111-117`), the pure codec (`codec.h:18, 117`),
  `TurnDriver` (`turn_driver.h:64-90`), and `random_identity` (`primitives.h:33`) with its
  four call sites.
- consensus's `Core` uses ordered containers and integers only (`core.h:1158-1190`; no
  floating point in its headers), and its sorts have no ties (`membership.h:72`,
  `core.h:1130`, `profile.h:15`), so tier A's authority groups can meet D2.
- The sans-I/O rule (D7), three tiers (D8), swarm testing, schedules as data, sometimes
  counters (D6), and a library of its own (D11) are the right choices. FoundationDB's and
  TigerBeetle's precedents are read as their papers and documentation describe them.

What blocks, and what it pauses:

- SIM-R1, D2's contract and the engine's labels: S4's adapters and D2's and D3's state
  machines.
- SIM-R2, D6's fault budget: the fault planner and its schedules.
- SIM-R3, D9's register is not strata's: S3's model, the history format, and every recorder
  (S4's clients, C5, the 30-node cluster).
- SIM-R4, the disk: tier C's journals and `Worker`s.
- SIM-R5, the histograms: S5's measurements and every reported bound.
- SIM-R6, D7's seams: tier C+ and the Q3 and Q4 changes.
- SIM-R7, the network's connections: S4.

## Rulings on D1–D12

| Decision | Ruling | Changes |
|---|---|---|
| D1 The scale contract | Sound with changes | The topology, numbers that do not follow from their reasons, DISTRIBUTION's per-node rows, the fan-out tail (SIM-R8 to SIM-R11) |
| D2 Determinism | Sound with changes | The banned list completed, the flags on every translation unit, labels required (SIM-R1) |
| D3 Time and clocks | Sound | A rate that changes mid-run needs a piecewise clock, if D6 adds drift (SIM-R19) |
| D4 Network | Sound with changes | In order or not at all, no oracle on send, clogs, link eviction (SIM-R7) |
| D5 Disks | Sound model, wrong vehicle | faultfs's crash model on a sparse, sector-granular, multi-directory store (SIM-R4) |
| D6 Faults and schedules | Wrong in its budget; the rest sound | Budget lost or damaged durable state only (SIM-R2) |
| D7 Production code inside | Sound rule; the table overstates | completion changes for Q4; strata's executor and flush; what Peers holds (SIM-R6) |
| D8 Fidelity tiers | Sound with changes | Tier A's store, tier B's elections, the shared view under mass failure, tier C's costs (SIM-R12) |
| D9 The checker | Sound search, wrong register, incomplete under unknown writes | strata's semantics and the history format (SIM-R3); dominance by reachability (SIM-R13) |
| D10 Shrinking | Sound with changes | The stability claim narrowed; budgets per tier (SIM-R14) |
| D11 Where the code lives | Sound | Q1, below |
| D12 Runs and reports | Sound with changes | Histograms with stated error; a versioned line format (SIM-R5) |

## Findings

### What the branch builds on

1. **SIM-R1. D2's banned list is incomplete for one hash on every platform, and the
   engine's labels default to zero** (`simulator.md:109-120`).
   - **Missing from the list.** Each of these differs between Linux, macOS, and FreeBSD,
     or between x86_64 and arm64, with no thread or clock involved:
     - the order of equivalent elements after `std::sort`, `std::partial_sort`,
       `std::nth_element`, and heap operations: the standard leaves it unspecified, and
       libstdc++ and libc++ implement different algorithms, so a `std::priority_queue`
       pops ties in different orders;
     - `std::shuffle` and `std::sample` (the algorithm is unspecified), and `std::hash`
       (implementation-defined; the two libraries hash strings differently), and with it
       the iteration order of every unordered container;
     - `long double`: 80-bit on x86_64, binary128 on Linux and FreeBSD arm64, 64-bit on
       macOS arm64;
     - arithmetic on plain `char`: unsigned on Linux and FreeBSD arm64 (AAPCS64), signed
       elsewhere (Apple's arm64 ABI);
     - the environment: faultfs decides whether `File::map` maps from
       `STRATA_FAULTFS_MAP` (`faultfs.hh:83`). `probe_faultfs`: null at `0`, a mapping at
       `1`, and mapped reads bypass media faults.
   - **Contraction.** `-ffp-contract=off` is set on the two test executables only
     (`sim/CMakeLists.txt:17`), not on the `simulation` interface target (`:6-8`). A
     simulator binary that includes consensus, journal, or strata headers compiles them
     with the compiler's default: contraction within expressions under clang, `fast` in
     GCC's GNU modes. On arm64 both fuse into FMA, which x86_64's baseline lacks.
   - **Labels.** `at(t, e, kind = 0, node = 0)` (`engine.hh:60`), and the hash folds only
     time, kind, and node (`engine.hh:93`). `probe_determinism blind`: one run executes A
     then B at t = 10, the other B then A; with default labels both hash to
     `0x04016a2ea0a3b382`, under Apple clang and GCC alike, and with distinct kinds they
     differ. "The logged hashes bisect to the first event that differs"
     (`simulator.md:114-117`) holds only where every scheduling site names its event or
     traces its payload. S2 does (`network.hh` labels each frame and traces each
     delivery); S1's defaults invite the opposite.
   - **Why now.** D2's and D3's state machines, S4's adapters, and the placement code are
     written against this contract. Existing production code already passes the longer
     list ("What holds"), so the change costs a paragraph and two lines of CMake.
   - **Fix.** Add the items above to D2's list. Make `-ffp-contract=off` an interface
     option of `simulation`, required of every target linked into a simulator binary.
     Remove the defaults for `kind` and `node`. Add a T1 test that swaps two same-time
     events of different kinds and finds the first differing checkpoint. Keep the pinned
     hashes under both standard libraries, as CI already does.

2. **SIM-R2. D6 budgets the wrong faults** (`simulator.md:73, 180-185`).
   - **Evidence.** The budget is "at most f of a group's 2f + 1 voters are down,
     partitioned away, or have damaged storage at once". Raft's safety holds under any
     number of crashes with durable state and any message loss or partition (dissertation
     §3.6, §3.8). What f bounds is lost or damaged durable state. T2's harness crashes and
     partitions with no such budget and loses each pending batch at random
     (`simulation_test.cc:23-31`). TigerBeetle's rule, which D6 cites, is about storage
     faults.
   - **Why it matters.** As written, the planner refuses majority crashes and majority
     partitions, where election-safety and leader-completeness bugs live, so T4 and T8
     would test less than T2 does today. The contract's own rates cross that line: at
     nominal churn about 35 nodes are restarting at any instant and about 3.6 groups have
     2 of 3 voters down; at stress, about 347 nodes and 360 groups. The rolling-upgrade rule
     adds about 477 groups per round (SIM-R9).
   - **Fix.**
     - Within the model: any crashes, power loss under honest barriers, partitions, clogs,
       clock skew, and gray failures, plus at most f voters per group whose durable state
       is lost or damaged. Such a voter counts until a new incarnation has caught up and
       the old one is removed (`track-c.md` D5).
     - Beyond the model: f + 1 such voters, or a barrier that lies.
     - Liveness bounds are measured after the heal, and during faults only for groups that
       keep a connected live quorum.
     - The planner checks only the durable-state budget, per group a fault touches (a
       node's disk: about 300 groups; a rack: 12,000).
     - D1's no-loss row says "with lost or damaged durable state" where it says "failed".

3. **SIM-R3. D9's register is not strata's, and S3 is built on it**
   (`simulator.md:279-283`; `checker.hh:52, 94-104, 176-193`).
   - **Evidence.** S3 advances the version on every successful put and erase, and reads
     expected version 0 as "absent and never written" (`checker.hh:52`). strata's rules
     (`phase-3.md:235-238`): a key whose copies are all deleted is absent, re-adding it
     takes version 1, expected version 0 requires absence, and a mismatch rejects with the
     current version. Two correct strata histories that S3 rejects:
     - put (version 1), erase, then put with expected 0: it succeeds in strata and must
       conflict in S3's model, whose version is 2;
     - put (version 1), erase, put: version 1 in strata and 3 in S3's model, so a get that
       reads version 1 is reported as a violation.
   - **The prune.** `normalize` drops a pending conditional write whose expected version
     is below the current one (`checker.hh:179`). That is sound only while versions never
     decrease. Under strata's rules a version recurs after a delete and re-add, so the
     prune discards writes that can still succeed. The subset dominance at
     `checker.hh:187` stays sound under any model.
   - **Unchecked.** A conflict's reported current version is never compared
     (`checker.hh:95, 100`), though strata returns it.
   - **The format.** D9 shares the history format with C5 and the 30-node cluster
     (`simulator.md:299-302`), and T13 fuzzes its parser, but D9 specifies neither the
     fields nor the time base. A real cluster's invocation and response times need one
     clock: one control host's monotonic clock, as Jepsen runs.
   - **Fix.** D9 states the model as strata's: absent at version 0; a put on an absent key
     makes version 1, on a present key the next version; an erase makes the key absent;
     expected 0 means absent; a conflict reports the current version, which the checker
     checks. D9 specifies the history file format: a versioned header, the fields, units,
     outcome codes, and the time base. S3 follows, and the expected-version prune goes, or
     applies only to keys whose history has no erase. T3's corpus adds re-adds, expected 0
     after an erase, and conflict versions, by hand and generated. Phase 3R's checker row
     (`PLAN.md:473`) needs exactly these histories.

4. **SIM-R4. faultfs cannot be a node's disk** (`simulator.md:154-159`; D8's tier C rows,
   `:259-260`).
   - **Evidence** (`probe_faultfs`):
     - One store per filesystem: a second store's `acquire_owner` fails with `EAGAIN`
       (`faultfs.hh:390`). Names are single components (`faultfs.hh:72`), so a node's
       groups cannot have directories of their own.
     - `File::sync` copies the whole file (`faultfs.hh:354`), and current and durable
       bytes are both kept. A 4 KiB write and a sync cost 0.067 ms on a 1 MiB file,
       1.05 ms on 4 MiB, 2.68 ms on 16 MiB, and 11.0 ms on 64 MiB.
     - The turn log preallocates 64 MiB segments of zeros and keeps a spare
       (`turn_log.h:48-50, 398-405`). At those defaults a node holds at least 2 × 64 MiB
       × 2 copies = 256 MiB, 75 GiB at 300 nodes against D8's 5.3 GB, and each turn's
       barrier costs about 11 ms of wall time against D8's 2 µs per event.
   - journal's `MemoryIO` has the right shape: durable bytes plus pending sectors, and
     directories by prefix over one power cut. But it rebuilds the whole file on every
     read, write, and sync (`memory_io.h:59-108`). Its namespace model also differs from
     faultfs's: it loses every unsynced namespace change, where faultfs keeps any ordered
     subset (`faultfs.hh:163-184`). "The same sector model" holds for data only.
   - **Fix.** D5 specifies the disk: faultfs's crash model (torn sectors, both sizes,
     ordered namespace subsets, failed syncs, EIO and ENOSPC, media faults) on a sparse
     store. Reads and writes cost O(bytes touched), syncs and crashes O(dirty sectors), and
     zero preallocation costs nothing. Directories have their own `sync_directory` and
     owner lock over one shared power cut, and nothing comes from the environment.
     faultfs's strata tests pass unchanged on it. D8's tier C rows state the
     `TurnLogOptions` they assume and are recomputed.

5. **SIM-R5. The `metrics` histograms cannot measure the bounds** (`simulator.md:353`).
   - **Evidence.** The library's only histogram has 14 bounds at powers of 4 from 1 µs to
     64 s (`metrics.hh:64-66`). Each target falls inside a 4× bucket: 5 s in (4, 16] s,
     75 ms in (64, 256] ms, 45 s in (16, 64] s. 300 s and 15 min fall in the overflow
     bucket. T9's ±10% at p50 and p99 cannot be read from such buckets.
   - **Fix.** A log-linear histogram: 2^k sub-buckets per power of two, an integer bucket
     index from the bit width and a shift, mergeable across seeds by addition. At 0.8%
     relative error over 1 ns to 2^64 ns it takes about 7,400 counters (58 KiB). It goes in
     `metrics`, extended rather than duplicated and coordinated with Detent, which links
     it, or in `simulation`. Each bound states its error, and D12's JSON line format is
     versioned with the histograms in it.

6. **SIM-R6. D7's seam table claims more than exists** (`simulator.md:210-227`).
   - **completion changes for Q4.** `IO` is completion's class (`completion/io.h:116`,
     namespace `kronuz::journal`), so Q4(a)'s `new_identity()` changes completion, which
     D7's row and its closing claim say nothing does (`simulator.md:218, 225-227`).
     `PublicationSteps` has no `IO` (`mutation.h:38-40`), so its caller passes the
     identity in. The other three sites reach an `IO`: `artifact.h:51` through
     `OwnerSession::io_lifetime`, `artifact.h:136`, and `journal.h:461`.
   - **strata lacks the seam.** `inline_executor::sleep_until` advances its own manual
     clock (`executor.hh:62`), and the merge's rate limit and the memory-budget wait call
     it in loops (`index.hh:929, 1096-1098`). In a cluster simulation, a node's background
     work would move its own clock ahead while nothing else runs, so its wait never sees a
     flush, a merge, or a budget release. The multi-run flush starts raw `std::thread`s
     (`index.hh:1718-1719`), which D2 bans. Tier C+ needs a stepping executor, where work
     suspends at `sleep_until` and `checkpoint` and resumes on an engine event (the intent
     of `executor.hh:43-45`), and the flush's runs posted to the executor.
   - **What Peers holds.** D15's flights run in `Core`, so the simulator does run the
     pipeline window. What does not run is Peers' control queue of 4 frames and its drop
     when full, the data queue (4 frames, 4 MiB), control-first ordering, and the
     reconnect backoff (`peers.h:33-36, 126-150, 331`). Q3's extraction also needs D12's
     node-pair keepalive detector, which the group-failover bound rests on
     (`simulator.md:83`; `track-c.md:647-650`).
   - **Ticks.** `Core` silently ignores a `Tick` whose election delay lies outside
     `[election_min, election_max]` (`core.h:434`). The adapter asserts the range.
   - **Text.** D7's completion row lists `IO`, `File`, and `DirectoryCursor` against
     `io.h:73, 101, 116`, which are `File`, `DirectoryCursor`, and `IO`.

7. **SIM-R7. S2's connections are not TCP's** (`simulator.md:138-142`;
   `network.hh:102-121, 143-154, 185`).
   - **A hole in the stream.** A frame is dropped at delivery when its link is
     unreachable at that instant (`network.hh:117`), and the connection survives. A later
     frame on the same lane that arrives after a heal is delivered, so one connection
     shows a hole. TCP never does: it retransmits in order, or the connection resets.
     Production code may rely on that (the snapshot channel's offsets, D16's credit
     windows).
   - **An oracle on send.** `send` returns false when the link is cut at send time
     (`network.hh:106`). A TCP sender learns of a partition only by timeout or reset.
   - **Clogs.** A clog delays only frames sent after it, since arrival takes
     `clogged_until` at send time (`network.hh:111, 154`); frames already in flight still
     arrive during the clog.
   - **Memory.** Link state is created on the first send or clog and never erased
     (`network.hh:107, 154`; there is no `links_.erase`). It grows with the distinct
     directed pairs ever used. The peer bound limits that for consensus traffic, but not
     for SWIM over a connection cache or for fan-out, against the design's "nothing it
     keeps per node grows with n" (`simulator.md:382-383`).
   - **Tests.** The randomized test pins a hash over partitions and heals but asserts no
     ordering property across them (`network_test.cc:53-72`); the FIFO test has no
     partitions (`:85-97`).
   - **Fix.** A frame lost in flight resets its connection: the epoch moves, later frames
     drop, and both ends are told. `send` reports nothing the sender cannot see. A clog
     holds frames in flight. Idle links (nothing in flight, no clog) are evicted. Each gets
     a test, and the randomized workload gains a property check.

### The scale contract (D1)

8. **SIM-R8. Two zones per region** (`simulator.md:39`).
   - "Each domain level ... has at least 5 members" holds for regions (5) and for zones
     across the cluster (10), not for zones within a region (2). Three voters spread over
     zones inside one region are then unsatisfiable. That is the usual deployment, and what
     a `+region=...` constraint produces (`placement-strategies.md:165`). So every
     three-voter group spans regions, and a quorum write pays a cross-region round trip:
     30 ms one way at p50 and 120 ms at p99 by D4's tables (`network.hh:67-69, 75`).
     T7's 75 ms p99 cannot hold for any index whose replicas span regions. Public clouds
     offer three or more zones per region.
   - **Fix.** 5 regions × 4 zones × 125 racks × 40 nodes keeps 100,000 nodes and 2,500
     racks; a zone outage is then 5,000 nodes. D1 states the default `spread`, and for T7
     where the index's replicas and its coordinator sit.

9. **SIM-R9. Numbers that do not follow from their reasons** (line numbers are
   `simulator.md`'s; the arithmetic is `contract_numbers.py`'s).
   - **Placement groups** (`:44`). Splitting at 32,768 leaves sizes in [16,384, 32,768]
     with a mean near 22,700, so about 440 groups, not 1,000 with a mean of 10^4. A
     thousand groups needs a threshold near 14,400, below the largest index. State 440, or
     let a group exceed the threshold by one index. The "about 300 placement groups at
     once" of `:84` holds either way.
   - **Shard bytes** (`:43`). Truncated at 64 GiB, the mean is 2.78 GiB if out-of-range
     draws are redrawn and 2.95 GiB if clamped, not 3.1 (the untruncated mean): 0.81 to
     0.86 TiB per node and 7.9 to 8.4 PiB per zone. Say which truncation.
   - **Index tiers** (`:42`). The top tier holds 0.44× the shards of the tier below it,
     not 1.6×.
   - **Rolling upgrade** (`:65`). One rack per zone at once takes 10 racks down in 10
     zones. A group's three voters sit in three zones, each on the restarting rack with
     probability 1/250, so about 477 groups lose their quorum in each of 250 rounds a day.
     Restarting one zone's racks at a time keeps every group spread over zones at one
     voter down. D1 gains the bound this needs: 0 groups without a quorum during an
     upgrade.
   - **Re-replication** (`:85`). 77 s assumes 64 targets with 4 incoming transfers each.
     Into one replacement node, the copyset-preserving choice, it takes 4,945 s; 15
     minutes needs at least 6 targets (824 s). The row states the spread D3 must reach
     within the peer bound.
   - **Zone-outage dissemination** (`:80`). 125 s is a floor at full efficiency for death
     updates alone. Suspicions spread too (2 × 10^4 updates: 250 s), and an epidemic sends
     each update many times: memberlist's documented retransmit multiplier of 4 gives
     4 · ⌈log10(n + 1)⌉ = 24 at 10^5. The membership-cost row caps traffic at 4 messages
     per node per second at rest (`:82`), which leaves piggybacking as the only channel.
     The zone row names the mechanism that beats 300 s: a burst mode during mass change
     (memberlist gossips on its own interval besides probing), or anti-entropy.
   - **False suspicion** (`:81`). "0 at 0%" holds only without gray nodes and clogs, and
     nominal runs have both (`:62`, D4).
   - **Authority load** (`:87`). A burst of 1,000 creations per second under one path
     prefix (time-named indexes) lands in one placement group, since each owns a
     contiguous range (`placement-strategies.md:151-152, 192-197`): 1,000 decisions per
     second against a bound of 10 entries. The row states decisions per entry (batching)
     or how a hot range splits.

10. **SIM-R10. DISTRIBUTION's per-node rows have no bounds** (`DISTRIBUTION.md:24-32`).
    - Open files and mappings within `vm.max_map_count` (65,530), commit barriers per
      node per second, memory per idle index, and budgeted flush, merge, and reclamation
      work appear in none of D1's bounds. Tier C can count open files and barriers per
      node on the simulated disk directly. D1 maps every scale-contract row to a bound, or
      to where it is measured instead, as Every gate's scale statement needs
      (`PLAN.md:150-151`).

11. **SIM-R11. T7's result is set by an unstated tail** (`simulator.md:88, 399`).
    - The slowest of 16,384 shards sits near the quantile 1 − 1/16,384 (p99.994). The row
      states p50 2 ms and p99 20 ms per shard. Past p99 the value is whatever the table's
      last point says, linearly interpolated, and S2's default tables put p100 at 10× p50
      (`network.hh:67-69`). The row states the per-shard and per-hop tail to p99.99, the
      hedging trigger and its budget, and SIM-R8's placement.

### The tiers, the checker, and shrinking (D8–D10)

12. **SIM-R12. D8's tiers** (`simulator.md:234-275`).
    - **Tier A's store.** Authority groups run on T2's `Durable` model, whose image is
      every entry through the base (`simulation_test.cc:78`). A placement group's image
      grows with every decision, and building the contract by proposals replays 3 × 10^7
      placements. Tier A needs a store whose image is the placement map's checkpoint, and
      an initial state installed as a snapshot.
    - **Tier B's elections.** "The most up-to-date durable log in a reachable quorum"
      (`:242-243`) is one of Raft's outcomes. Raft elects any candidate whose log is at
      least as up to date as each of a majority's (§3.6), so a less current candidate can
      win and truncate others' uncommitted suffixes. D1's acknowledged-prefix row and
      configuration rollback depend on that case. Tier B draws among every candidate Raft
      admits.
    - **The shared view under mass failure** (`:262-271`). "The few updates it received
      out of order" fails in the zone outage: 10^4 deaths and their suspicions spread at
      once in random order, so each node holds an arbitrary subset of the window, up to
      10^9 extras across 10^5 nodes. A per-node bitset over [cursor, head) costs 1.25 KB
      per 10^4 updates, about 125 MB at 10^5 nodes. D8 states the representation and its
      peak.
    - **Tier C's costs** (`:259-260`) follow SIM-R4: 75 GiB at default segment sizes in
      today's faultfs, and 11 ms per barrier on a 64 MiB segment.

13. **SIM-R13. D9's checker under unknown writes** (`simulator.md:288-295`).
    - An unknown write stays pending for the rest of its key's history. `search` records
      an end at every node after the last finite operation, including nodes reached by
      applying carried unknown writes (`checker.hh:149-153`), and `normalize` removes only
      subsets at the same state (`:187`). With k unconditional unknown puts of distinct
      values outstanding and a get that sees none of them, a cut carries 1 + k · 2^(k − 1)
      configurations: 449 at k = 7 and 1,025 at k = 8, past the 1,024 bound
      (`:68, 225`). The key is inconclusive from then on, though its history is
      linearizable. Every client timeout makes an unknown write, so long tier C histories
      reach this. The count is from the code; its probe did not run under the hold.
    - **The node budget** accumulates over a key's segments and configurations
      (`checker.hh:146, 219`), while its comment says per segment and configuration
      (`:67`). A long history of cheap segments goes inconclusive too.
    - **Fix.** A configuration reachable from another by applying some of its pending
      writes is dominated; equivalently, ends are recorded only when the last finite
      operation is linearized. The budget is per segment and configuration, as documented,
      with a separate total. Generators retire a key after a bounded number of unknown
      writes, as Jepsen's independent keys do. D9 states the inconclusive rate as a gate
      target (for example, at most 0.1% of keys, each reported).

14. **SIM-R14. D10's stability claim, and T11's budget** (`simulator.md:314-315, 403`).
    - "Removing a fault does not reshuffle" latencies and workload. Streams are per
      component, but a component's draws are sequential, and S2 draws every latency of a
      node from one stream (`network.hh:126, 168`). A removed fault that changes how many
      frames a node sends shifts every later latency of that node; only unrelated
      components keep their draws. Draws keyed by event (a latency keyed by link and frame
      ordinal) narrow the shift. D10 states the property it achieves.
    - T11 gives each mutant 2 CPU-hours. At tier B a simulated hour costs about 0.4 h by
      D8's estimate, so delta debugging over hundreds of schedule entries runs for days.
      T11 states a budget per tier, and D10 states how the population shrinks with
      symbolic targets preserved: a scenario carries its topology and its placement
      subset.

### Gate rows

15. **SIM-R15. Every row has a target and a procedure; these cannot be rerun or decided as
    written** (`simulator.md:393-405`).
    - **T1.** `sim_run --seeds 1-1000` names no tier, scale, or length; at tier B's
      contract, 1,000 seed-hours cost about 400 CPU-hours. State it: 1,000 seeds at a
      reduced scale, and the pinned hashes at full scale for seeds 1–10.
    - **T2.** It counts only T4's budget, which is tier B. Tier C's classes (power loss at
      each barrier kind: journal, segment, manifest; corruption per file class) and
      Raft-internal counters (a configuration entry in a replaced suffix) need T8's
      budget. Media faults at their stated rates (1 latent error per 10 TB read, 1 bit flip
      per 10^14 bytes) reach zero at tier C's read volume, so coverage needs injected
      faults with their own counters.
    - **T3.** The generated corpus mutates into stale reads only
      (`checker_test.cc:149-168`). T3 names stale reads, lost writes, reordered versions,
      and an unknown write seen then unseen, each by hand and by mutation. Add a
      brute-force cross-check, as `probe_checker_bruteforce` does, and SIM-R3's cases.
    - **T4.** No compute budget. At D8's 0.4 s per simulated second, nominal alone is 80
      CPU-hours, 27 hours on the gate VM at 3 seeds at once, before stress and the
      scenario files, on the host the benchmark gates hold. State the budget and the host.
    - **T5.** State the exponent's fit (least squares on log-log over the four n, on
      per-seed medians) and its noise bound.
    - **T6.** A node lost late in a one-hour run cannot finish re-replication inside it
      (15 minutes of hold plus 15 of bound), so its sample is censored; nominal churn gives
      about 92 losses over 200 seeds. Run each seed past its last loss by the hold plus the
      bound, or count only losses in the first 30 minutes.
    - **T8.** "10^7 events over 1,000 seeds" counts events, which are finer than Phase
      3R's steps (`PLAN.md:470-472`) and T2's. At D8's 6 × 10^5 events per simulated
      second, 10^4 events is 17 ms of simulated time, under one election timeout. State
      the scale (nodes, groups), the simulated time per seed, and a unit matched to Phase
      3R's.
    - **T9.** Calibrating tier B on tier C (`:240-241`) and testing it on the same
      scenarios is circular. Calibrate on one set, and test on held-out scenarios and
      seeds. Replace "within ±10% at p50 and p99" with a bootstrap interval of the ratio
      inside [0.9, 1.1]; with 20,000 samples, p99 rests on about 200.
    - **T10.** "Else D8 is revised" cannot fail. The targets that matter: tiers A and B at
      the contract within 4 GB per process (3 seeds on the 16 GB gate VM), tier C at 1,000
      nodes within 6 GB, and wall time per simulated second that fits T4's stated budget.
    - **T12.** Membership mutants are caught by T5's runs, not T4's or T8's. Each mutant
      names its budget, and each new oracle, the history checker included, has one.
    - **Missing.** The beyond-the-model self-check (`:186-188`) has no row: the beyond
      profile must report loss in a stated share of seeds. SIM-R9's upgrade bound needs a
      row too.

### The open questions

- **Q1, where the code lives: the new library**, as recommended. The catalog has no
  deterministic simulator, and `Kronuz/random` (one `std::mt19937_64` per thread, seeded
  from `std::random_device`) is what D2 bans, so `random.hh` duplicates nothing. The
  consensus adapter is an optional target, off by default. `simulation` carries SIM-R1's
  flags as interface options and depends on `metrics` (SIM-R5) besides `completion`. Until
  the owner decides, `sim/` stays self-contained (`sim/CMakeLists.txt`), as the
  filter-repo plan needs.
- **Q2, the member view: (a)**, full views with a join merge. 3.2 MB per node at 10^5
  fits DISTRIBUTION's "bounded per peer" (`DISTRIBUTION.md:22`), and placement gets its
  candidates without a second directory. Conditions: SIM-R12's bitset window in the shared
  container, and D2 states the merge as a lattice (incarnation, then status), with
  Lifeguard's confirmer sets merged by union.
- **Q3, Peers in the simulator: (a)**, extracted with D16, sans-I/O from its first commit,
  including the control and data queues, the drop rule, and D12's keepalive detector
  (SIM-R6). Until then the adapter models the transport, and T9 and T12 state the gap.
- **Q4, journal's identities: (a) in substance.** One defaulted virtual on completion's
  `IO`: `random_device` in `PosixIO`, the node's stream in the simulator. `PublicationSteps`
  gets its identity from its caller (SIM-R6). The change is coordinated with Detent first
  (PLAN, Track C), and T1 covers tier C once it lands.
- **Q5, failure rates: D1's published rates with 10× stress**, plus two correlated cases
  they miss: a bad push that restarts 10% of the nodes within a minute, and a top-of-rack
  switch failure that partitions a rack without powering it off. Fleet data replaces them
  when it exists.
- **Q6, what tier B proves: the first option**, with conditions: T9 on held-out scenarios
  (SIM-R15), tier B's elections over all of Raft's choices (SIM-R12), tier B's own mutants
  caught, and every tier-B failure replayed at tier C at reduced scale before it counts
  for or against a gate.

### Done criteria

16. **SIM-R16. The ledger's criteria are incomplete** (`docs/status/simulator.md:9-21` at
    `57a55fb`).
    - They carry no IDs and cover S1–S3 only. The house rule is that the lead sets
      numbered criteria (K1, ...) and the contributor ticks them.
    - Missing:
      - D2: the pinned hashes on macOS arm64 and FreeBSD (both stop at main's
        `std::jthread` break, `include/strata/bisection.hh:541`), the perturbation test,
        and SIM-R1's flags on every simulator target;
      - S2: in order or not at all across partitions and resets, and idle-link eviction
        (SIM-R7);
      - S3: strata's semantics (SIM-R3), the brute-force cross-check as a test, a bounded
        inconclusive rate on unknown-heavy histories (SIM-R13), and the history format with
        its parser fuzzed (T13);
      - S4: T2's oracles ported; consensus's 13 mutants (`tools/mutants.sh:22-37`) each
        caught by some suite; and the ported harness catching at least the seven that the
        randomized simulation alone caught at 400 seeds (`track-c-design.md:1757-1761`),
        with seeds to detection recorded;
      - S5: T10's measurement at the contract, with its host;
      - sanitizers in CI: linux-asan-ubsan reaches its 1-hour limit before the simulation
        tests (run 39060501864), so they have never run under ASan and UBSan in CI;
      - a libstdc++ build with `-Werror` before each push (SIM-R18 was compiled under
        libc++ only).

### The code on the branch

17. **SIM-R17. S1's 32-bit generation can fire the wrong event**
    (`engine.hh:46, 66, 91, 133`).
    - A canceled event's heap entry stays until its time, and its slot returns to the
      LIFO free list. After 2^32 reuses of that slot the generation wraps, the stale entry
      matches the slot's new event, and that event runs at the stale time. It is reachable
      where few events are pending (tier C0's small clusters) and a canceled timer sits far
      ahead. `probe_generation_wrap.cc` builds the case: a canceled event at 1,000 ns,
      2^32 − 1 reuses at time 0, then an event at 5,000 ns. Its run did not finish.
    - **Fix.** Each body records the sequence it was scheduled with, and `step` compares
      the entry's 64-bit sequence instead of a generation. The entry stays 24 bytes, and
      nothing wraps.

18. **SIM-R18. Closed at `7d0b3fa`: S2 did not compile under libstdc++ or LLVM 23.** At
    `57a55fb`, `network.hh:111` (`std::max` over an initializer list) failed under GCC 16,
    and `network.hh:150` (`std::fill`) under Homebrew LLVM 23, for want of `<algorithm>`,
    which Apple's libc++ includes transitively. `7d0b3fa` adds the include, and CI's Linux
    jobs pass.

19. **SIM-R19. Smaller notes, none blocking.**
    - `bounded_draw` computes the threshold's 64-bit modulo on every draw
      (`random.hh:59`). Lemire's method computes it only when the low word falls below n,
      which saves a division per latency draw.
    - `stream::child` is a pure function of the parent's key and the label
      (`random.hh:72-73`), so two components given one label share a stream. A debug
      check that labels are unique per parent prevents correlated draws.
    - The checkpoint is taken before the event's handler runs (`engine.hh:96`), so a
      payload traced at event 65,536 · k lands in the next window
      (`probe_determinism bisect 65536` reports window 1). The bisection procedure says so.
    - D3: a clock rate that changes mid-run needs a piecewise form, since an unsigned
      offset cannot absorb a faster rate. T2's rates are fixed per node
      (`simulation_test.cc:133`), so S4 does not need it.

## Conditions

1. **C1.** T1's pinned hashes pass on macOS arm64 and FreeBSD once main's `std::jthread`
   break is fixed. Until then the cross-platform claim covers Linux x86_64, Linux arm64,
   and macOS x86_64.
2. **C2.** The simulation tests run under ASan and UBSan in CI within the job's limit.
3. **C3.** Q3's and Q4's library changes land coordinated with Detent, with Detent's build
   green at the pin move (PLAN, Track C).
4. **C4.** Every gate run keeps its JSON lines, commit, and host under `docs/gates/`, with
   SIM-R5's histograms.
5. **C5.** A revision check covers SIM-R1 to SIM-R16 in the design before the fault planner
   is written and before the first run of T4 to T9.

## Which work starts when

| Work | Starts | Waits for |
|---|---|---|
| SIM-R1's labels, SIM-R7's connections, SIM-R17's sequence check, SIM-R13's dominance | Now | — |
| S3's register and the history format | Now | SIM-R3 in D9, in the same commit |
| S4: `Core` groups on the engine, T2's oracles, clients and histories | After SIM-R1, SIM-R3, and SIM-R7 | SIM-R2 before faults beyond T2's |
| The fault planner and the scenario files | After SIM-R2 | — |
| S5: tier B and T10's measurement | After SIM-R5 and SIM-R12's tier A store and view | D2's and D3's state machines for production code |
| Tier C on `Worker`s and turn logs | After SIM-R4 | Q4; Q3 for the transport |
| Tier C+ with strata's core | After SIM-R6's stepping executor | Phase 3R |
| T4 to T9 | After the revision check (C5) | The host's holds |

## Verdict and required changes, in one line each

**APPROVE WITH CONDITIONS.** A revision check of SIM-R1 to SIM-R16 comes before the fault
planner and the first gate run.

1. SIM-R1: D2's banned list completed (ties, hashes, shuffles, `long double`, `char`, the
   environment); `-ffp-contract=off` on every simulator target; labels required.
2. SIM-R2: D6 budgets lost or damaged durable state only; crashes and partitions are
   unbounded for safety.
3. SIM-R3: D9's register is strata's (version 1 after a re-add, expected 0 means absent,
   conflicts report the version); the history format specified; the version prune removed.
4. SIM-R4: the disk is faultfs's crash model on a sparse, sector-granular, multi-directory
   store.
5. SIM-R5: log-linear histograms with stated error; a versioned report format.
6. SIM-R6: D7's table corrected: completion changes for Q4, strata needs a stepping
   executor and an executor-run flush, and Q3 includes the keepalive detector.
7. SIM-R7: connections in order or not at all; no partition oracle on send; clogs hold
   frames in flight; idle links evicted.
8. SIM-R8: four (or three) zones per region; T7's placement stated.
9. SIM-R9: the placement-group count, shard mean, tier ratios, rolling upgrade,
   re-replication spread, dissemination mechanism, false-suspicion scope, and authority
   bursts corrected.
10. SIM-R10: DISTRIBUTION's per-node rows mapped to bounds.
11. SIM-R11: T7's tail and hedging stated.
12. SIM-R12: tier A's store, tier B's elections, the view's window, and tier C's costs.
13. SIM-R13: dominance by reachability; the budget per segment; an inconclusive-rate
    target.
14. SIM-R14: D10's stability claim narrowed; T11 per tier.
15. SIM-R15: T1, T2, T3, T4, T5, T6, T8, T9, T10, and T12 made decidable; rows for the
    beyond-the-model check and for upgrades.
16. SIM-R16: numbered criteria set by the lead, with the missing ones.
17. SIM-R17: compare sequences, not 32-bit generations.
18. SIM-R18: closed at `7d0b3fa`.
19. SIM-R19: notes.

Not run: benchmarks of any tier; the gate VM, the FreeBSD VM, and every remote host;
`probe_generation_wrap`'s 2^32-cycle run, interrupted by a pause and then stopped by the
strict hold from 13:17; probes for the checker's unknown-write growth and node budget and
for the network's holes across a heal, so SIM-R7 and SIM-R13 rest on the code; the GCC 16
and LLVM ASan+UBSan builds at `7d0b3fa`, stopped by the same hold; TSan locally; fuzzing;
coverage; macOS arm64 and FreeBSD, where CI stops at main's build break; Detent's suite;
consensus's `tools/mutants.sh`; tiers A, B, and C, which have no code yet; the 30-node
cluster.
