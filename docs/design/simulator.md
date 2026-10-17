# Track D1: the scale contract and a deterministic cluster simulator

PLAN, Track D: D1 is "the scale contract and a deterministic simulator of at least 100,000
nodes and 10^7 shards (time, network, disks, and failures simulated), shared with Track C's
harness", designed with C5 under one owner. Track D's gate per item is the simulator's runs
at the scale contract (no acknowledged loss, no split placement, bounded convergence and
fan-out latency), a real cluster of at least 30 nodes, and the architect's review. Phase 3R's
gate runs 10^7 steps over 1,000 seeds with the linearizability checker. This document
specifies the contract, the simulator that checks it, and the checker C5 shares.

Library commits read for this design: `consensus` `fc66c0e`, `journal` `3dccf50`,
`completion` `5251cf9`, `reactor` `e19afd9`, `task` `bbb2e14`, `metrics` `c975765`.

Revision 1. It folds in the architect's preliminary review,
`docs/reviews/simulator-design.md` at `5ce1c89` (APPROVE WITH CONDITIONS, SIM-R1 to
SIM-R19). Each change cites its finding; the table below lists them.

## Changes from revision 0

| Finding | Where | Change |
|---|---|---|
| SIM-R1 | D2; `sim/CMakeLists.txt`; `engine.hh` | The banned list gains tie order after sorts and heaps, `std::shuffle` and `std::sample`, `std::hash` and unordered iteration, `long double`, plain `char` arithmetic, and the environment. `-ffp-contract=off` is an interface option of `simulation`. `at` and `after` require a kind and a node. A perturbation test finds the first differing checkpoint |
| SIM-R2 | D6; D1's no-loss row | The fault budget bounds lost or damaged durable state only (f per group); crashes, power loss under honest barriers, partitions, clogs, skew, and gray failures are unbounded for safety |
| SIM-R3 | D9; `checker.hh`; new `history.hh` | The register is strata's: absent at version 0, a re-add takes version 1, expected 0 means absent, a conflict reports the current version and the checker checks it. The history file format is specified. The expected-version prune applies only to keys with no erase |
| SIM-R4 | D5; D8's tier C rows | The disk is faultfs's crash model on a sparse, sector-granular, multi-directory store with costs in bytes touched and dirty sectors; tier C states its `TurnLogOptions` and is recomputed |
| SIM-R5 | D12; Q7 | Log-linear histograms with a stated error (0.79%), mergeable across seeds; the JSON line format is versioned |
| SIM-R6 | D7 | The seam table as it is: Q4 changes `completion`'s `IO`; strata needs a stepping executor and an executor-run flush; what `Peers` holds that the simulator does not run, including D12's keepalive detector; the adapter asserts `Tick`'s range |
| SIM-R7 | D4; `network.hh` | A frame lost in flight resets its connection and both ends are told; `send` returns nothing; a clog holds frames already in flight; idle links are evicted |
| SIM-R8 | D1; `network.hh` | 5 regions × 4 zones × 125 racks × 40 nodes; shard groups spread over zones inside a region by default, which T7 states |
| SIM-R9 | D1 | Placement groups about 440; shard mean 2.78 GiB (redrawn); tier ratio 0.44 at the top; rolling upgrade by zone with a zero-loss-of-quorum bound; re-replication over at least 16 targets; a dissemination mechanism named; false suspicion's scope; authority bursts batched |
| SIM-R10 | D1, "Per-node rows" | Each of DISTRIBUTION's per-node rows maps to a bound or to where it is measured |
| SIM-R11 | D1's fan-out row; D4 | Per-shard and per-hop tails stated to p99.99; hedging's trigger and budget |
| SIM-R12 | D8 | Tier A's store is the placement map's checkpoint; tier B's elections draw among every candidate Raft admits; the shared view's window is a bitset, with its peak; tier C's costs recomputed |
| SIM-R13 | D9; `checker.hh` | Dominance by reachability (ends recorded only where the last finite operation is linearized); the node budget per segment and configuration with a separate total; generators retire keys; an inconclusive-rate target |
| SIM-R14 | D10; T11 | The stability claim narrowed to what per-component streams give; shrinking budgets per tier; scenarios carry their topology and placement subset |
| SIM-R15 | Gate rows | T1, T2, T3, T4, T5, T6, T8, T9, T10, T12 decidable and rerunnable; T14 (beyond the model) and T15 (rolling upgrade) added |
| SIM-R16 | "Done criteria" | Numbered criteria K1 to K24 covering S1 to S9 |
| SIM-R17 | `engine.hh` | Cancellation compares each event's 64-bit sequence; no 32-bit generation wraps |
| SIM-R18 | `network.hh` | Closed at `7d0b3fa` (the `<algorithm>` include) |
| SIM-R19 | D2, D3; `random.hh` | Lemire's threshold computed only when the low word falls below n; child labels unique per parent; the checkpoint precedes its event's handler; a rate change mid-run needs a piecewise clock |

## Starting point

Four harnesses exist. Each is a test file with its own network, storage, and clock model:

| Harness | What it runs | Scale | Models |
|---|---|---|---|
| `consensus` `test/simulation_test.cc` (979 lines; T2, C5's first version) | `Core`s with a modeled durable store | 3–7 nodes, one group | Loss, reordering, duplication, partitions, crashes keeping or losing a pending batch, clock rates 0.5×–2× |
| `consensus` `test/liveness_test.cc` (446 lines; T3) | `Core`s in simulated milliseconds | 3–5 nodes, one group | Barrier latency with p99 = b, one-way delay in [r/4, r/2] |
| `consensus` `test/shared_cluster_test.cc` (251 lines) | `Worker`s on `journal`'s turn log over `MemoryIO` | 3 nodes, 3 groups | Drops, reordering, power cuts at a random I/O |
| strata `include/strata/faultfs.hh` (494 lines) | strata's index over an in-memory `IO` | one node | Torn 512-byte sectors, namespace durability, EIO and ENOSPC rules, media faults on reads |

`journal` `include/journal/memory_io.h` is a second in-memory filesystem with the same
sector model for data. Its namespace model differs (SIM-R4): it loses every unsynced
namespace change, where faultfs keeps any ordered subset (`faultfs.hh:163-184`). No harness
has a linearizability checker: `simulation_test.cc:549` checks that a read's index covers
every commit known at admission, which is a necessary condition, not linearizability of
client histories. None runs more than 7 nodes, and none shares a network, disk, or clock
model with another. D1 replaces the bespoke parts with one engine and keeps every oracle.

## D1. The scale contract

### Topology and population

| Dimension | Contract | Why |
|---|---|---|
| Nodes | 100,000 | Goal 3 ("hundreds of thousands"); DISTRIBUTION's first row. Scaling runs add 10^4, 3×10^4, and 2×10^5 to fit each cost's exponent in n |
| Failure domains | 5 regions × 4 zones × 125 racks × 40 nodes: 20 zones of 5,000 nodes, 2,500 racks | Each level DISTRIBUTION names has at least 4 members inside its parent, so three voters spread over zones inside one region stay satisfiable after one zone's loss, as public clouds' three-or-more zones per region allow. Two zones per region made every three-voter group span regions (SIM-R8) |
| Default spread | Shard groups: `spread: zone` inside one region; the root and placement groups: `spread: region` | A shard quorum write then pays a same-region round trip (D4: 0.5 ms one way at p50 between zones), not a cross-region one (30 ms at p50, 120 ms at p99). Authority groups must survive a region's loss and commit at most 10 entries per second, so they afford it. D3 may revise either with its reason |
| Shard groups | 9,968,800 (≈ 10^7), 3 voters each | DISTRIBUTION's row; 299 voter replicas per node on average |
| Dense profile | 10^7 groups on 3×10^4 nodes, 1,000 replicas per node | DISTRIBUTION's per-node row (≥ 1,000 groups per node) |
| Indexes | 10^6: 900,000 of 1 shard, 90,000 of 16, 9,000 of 256, 900 of 4,096, 100 of 16,384 | 9,968,800 shards: 0.90, 1.44, 2.30, 3.69, and 1.64 million per tier. The first four tiers each hold 1.6× the shards of the one below and the top tier 0.44× (SIM-R9): a heavy tail whose top tier exercises the ≥ 10^4 fan-out row |
| Shard bytes | Lognormal, median 1 GiB, σ = 1.5; a draw outside [1 MiB, 64 GiB] is redrawn (0.28% of draws); mean 2.78 GiB | Moves and catch-up are byte-bound: 0.81 TiB (891 GB) of replicas per node at 299 replicas; 1.5×10^6 replicas and 3.96 PiB per zone. The untruncated mean is 3.08 GiB; clamping instead of redrawing gives 2.95 (SIM-R9) |
| Placement groups | Split at 32,768 shards: sizes settle in [16,384, 32,768] with a mean near 22,700, so about 440 groups at the contract | The largest index (16,384 shards) fits one group; the root holds about 440 range entries. Revision 0's "about 1,000 with a mean of 10^4" needed a threshold near 14,400, below the largest index (SIM-R9) |
| Distinct peers per node | ≤ 64 | `track-c.md` D12's bound for D3 (copysets); 256 keepalive frames per second per node at rest |

The workload numbers are the simulator's defaults until D3 and D4 state measured ones; each
is a parameter. Each row's reason carries its arithmetic.

### Churn and failures

Rates at 100,000 nodes. The nominal column is the planning rate; the stress column is 10×
it, and every safety row runs at both. Scripted scenarios add the correlated cases.

| Event | Nominal | Stress | Why |
|---|---|---|---|
| Node restart (crash or upgrade), back within 15 min | 1 per node per 30 days: 139 per hour, about 35 down at once | 1,389 per hour, about 347 down at once | A monthly rolling upgrade plus crashes. Ford et al. (OSDI 2010) find most node unavailability shorter than 15 minutes, which is why replacement waits. At these rates about 3.6 groups (nominal) and 360 (stress) have 2 of 3 voters down at once, which D6 does not budget away (SIM-R2) |
| Permanent node loss (disk or host) | 4% per node-year: 11 per day | 110 per day | The upper end of the 2–4% annual disk replacement rates Schroeder and Gibson (FAST 2007) report; Pinheiro et al. (FAST 2007) report 1.7% to 8.6% by drive age |
| Rack power loss | 1 per 1,000 racks per day: 2.5 per day | 25 per day | Correlated failure, the case copysets bound (Cidon et al., USENIX ATC 2013) |
| Top-of-rack switch failure | Scripted: a rack partitioned, powered on | 1 per 1,000 racks per day | The review's Q5 case: partition without power loss |
| Bad push | Scripted: 10% of nodes restart within 1 min | Twice per run | The review's Q5 case: a correlated restart wave |
| Zone outage | Scripted: 1 zone (5,000 nodes) for 1 h | 2 zones in different regions | `spread: zone` must keep quorum; dissemination is bandwidth-bound (below) |
| Network partition | Scripted: a rack, a zone, a region, asymmetric and one-way | Random cuts at 10× | Partitions that leave each side with some voters |
| Gray failure | 1 node in 1,000 at any time with 10× disk latency or 5% loss | 1 in 100 | Huang et al. (HotOS 2017); Gunawi et al. (FAST 2018) |
| Media faults | 1 latent sector error per 10 TB read; 1 silent bit flip per 10^14 bytes read | 100× | Bairavasundaram et al. (SIGMETRICS 2007); REPLICATION's corruption defense. At tier C's read volume these reach zero, so T2's coverage profile injects them at its own rate (SIM-R15) |
| Index creation and deletion | 11.6 per second each (10^6 indexes living 1 day) | 116 per second, and bursts of 1,000 per second for 60 s under one path prefix | Xapiand's index per path makes indexes cheap and numerous; time-named indexes put a burst in one placement group's range |
| Rolling upgrade | The whole cluster in 24 h, one zone at a time, 25 of its racks (1,000 nodes) at once: 100 rounds of 14.4 min | In 6 h, one whole zone at once: 20 rounds of 18 min | Restarting inside one zone keeps every zone-spread group at one voter down (SIM-R9). Revision 0's one rack per zone across 10 zones put about 477 groups without a quorum in each round. Leaders move by transfer (Track C, D6); nothing re-replicates |

### Bounds

Every row holds on every seed. A violation of a safety row is a failure whatever the cause.

| Bound | Target | Why |
|---|---|---|
| **No acknowledged loss** | 0 writes acknowledged with `ack=quorum` or `ack=all` lost: each stays visible to every later read until a later write replaces it, with at most f of a group's 2f + 1 voters' durable state lost or damaged at once (D6); crashes, power loss under honest barriers, and partitions are not bounded | REPLICATION's guarantee; Raft's safety holds under any number of crashes with durable state and any message loss (Ongaro, §3.6, §3.8). Runs beyond the model (D6, T14) must show loss, which proves the oracle sees it (SIM-R2) |
| **No split placement** | 0 instants at which two placement groups both hold a range at epochs either considers current; 0 shards with two committed voter sets in one configuration epoch; 0 replicas serving a shard its placement group has no record of, after 60 s | DISTRIBUTION: one authority per range, never one global leader |
| **Acknowledged prefix on a quorum** | After every membership change, a quorum of the new voters holds the acknowledged prefix durably | Catches a placement that removes a voter before its replacement caught up |
| **Failure domains** | 0 groups with two voters in one domain they are spread over, except where no placement satisfies the constraint, each such case reported | DISTRIBUTION's failure-domain row |
| **Peer bound** | Distinct peers per node ≤ 64 at every instant; a move that would pass it waits | `track-c.md` D12 |
| **Upgrade availability** | 0 groups without a quorum caused by a rolling upgrade: a rack restarts only when every group with a voter on it has its other voters up, and waits otherwise | SIM-R9. With concurrent nominal churn, an unchecked restart of 1,000 nodes would leave about 210 groups at once with one voter upgrading and one crashed |
| **Failure detection** | A crashed node first suspected within 5 s at p99 | SWIM (Das et al., DSN 2002) with a 1 s period: a member goes unprobed for k periods with probability about e^−k, so k = 4.6 at p99 |
| **Single-failure dissemination** | 99.9% of live nodes know a node is dead within 45 s at p99 | 5 s to suspicion, a suspicion timeout that grows with log n (Lifeguard, Dadgar et al., 2017; 20 s at 10^5 with 4 periods per decade of n), and about 20 periods of infection-style dissemination (log2 10^5 = 16.6) |
| **Zone-outage dissemination** | 99.9% of live nodes know all 5,000 deaths within 300 s | 5,000 deaths and their 5,000 suspicions are 10^4 updates. Piggybacking alone, at 40 updates of 32 bytes per 1,400-byte datagram and about 2 messages heard per second, needs 125 s at perfect efficiency, and an epidemic sends each update many times (memberlist documents a retransmit multiplier of 4, 24 sends per update at 10^5). So D2 must add one of two mechanisms (SIM-R9): a burst mode, where a node holding more than one datagram of pending updates also gossips them to 3 random peers every 200 ms within 64 KB/s, about 600 updates received per second and about 70 s with 4× redundancy; or push-pull anti-entropy with one random peer every 10 s, about 17 rounds (170 s). 300 s stays under the 15-minute replacement hold either way |
| **False suspicion** | At most 10^−4 live nodes declared dead per node-hour at 1% datagram loss, with the nominal gray nodes and clogs; 0 at 0% loss without gray nodes or clogs | Lifeguard's motivation: a false death moves replicas. Nominal runs have gray nodes and clogs (D4), so "0" holds only in a profile without them (SIM-R9) |
| **Membership cost** | At rest, at most 4 membership messages sent per node per second; during a burst (above), at most 64 KB/s per node; the exponent of messages per node in n at most 0.15 over 10^4…2×10^5 (T5's fit) | DISTRIBUTION: nothing linear in n on a hot path. Log factors over a 20× range give an exponent near 0.09 |
| **Group failover** | After a zone outage, every group that kept a connected live quorum commits a new entry within 5 s at p99, 10 s worst, measured after the outage begins | `track-c.md` T3's rows: a quiescent group's leader node failing yields a leader within 4 s. About 5×10^5 groups (1/20 of them) lose their leader at once, about 5 per surviving node |
| **Placement decisions** | Replacements for a lost node's replicas committed within 10 s of its declared loss; a rack's (12,000 replicas) within 60 s | Decisions are small consensus entries in about 300 placement groups at once |
| **Re-replication** | A lost node's replicas back to full replication within 15 min at p99 after the hold, with its replicas spread over at least 16 distinct targets | 891 GB into 16 targets at `track-c.md` D13's 4 incoming transfers each and T7's 50 MB/s: 3.2 GB/s, 278 s before contention. Six targets (743 s) is the floor that meets 15 minutes with no margin; one replacement node, the copyset-preserving choice, takes about 4,900 s. D3 must reach the spread within the peer bound of 64 (SIM-R9) |
| **Zone re-replication** | No data moves during the hold (1 h for a zone); afterwards moves stay within each node's 4 + 4 transfers and each placement group's move budget | 1.5×10^6 replicas, about 4 PiB, cannot move at once and should not |
| **Authority load** | The root commits at most 1 entry per second at nominal churn; a placement group at most 10 entries per second, each batching up to 256 decisions over a 100 ms window; a range whose decisions stay above 1,000 per second for 60 s splits | A creation burst under one path prefix lands in one placement group, since each owns a contiguous range (`placement-strategies.md:151-152, 192-197`): 1,000 decisions per second are 10 entries of 100 (SIM-R9). The root changes only on placement-group splits and moves |
| **Fan-out latency** | A query to 16,384 shards completes in 75 ms at p99 under a 100 ms deadline, with at most 0.1% of shards missing in the p99 query; aggregators merge at most 64 children; the index's replicas sit in one region (the default spread) and its coordinator in that region | Fan-in 64 gives depth 3 (64^2 = 4,096 < 16,384 ≤ 64^3). The slowest of 16,384 shards sits near the quantile 1 − 1/16,384 (p99.994), so the row states the tail (SIM-R11): per-shard service time p50 2 ms, p95 8 ms, p99 20 ms, p99.9 40 ms, p99.99 80 ms, at most 200 ms, until D5 measures strata's; per hop D4's tables to p99.99. Hedging: a request outstanding past the shard's p95 (8 ms) goes to a second replica, at most 5% extra requests per query (Dean and Barroso, CACM 2013) |
| **Cluster state per node** | Membership ≤ 32 bytes per cluster member (3.2 MB at 10^5); placement ≤ 1 KiB per hosted replica; routing caches bounded | DISTRIBUTION's memory row, stated as a function of n |

The membership, placement, and fan-out rows bind D2, D3, and D5; the simulator measures them
and the owning design may revise a target with its reason. The safety rows are fixed.

### Per-node rows (SIM-R10)

DISTRIBUTION's per-node rows, each mapped to a bound or to where it is measured, as Every
gate's scale statement needs (`PLAN.md:150-151`):

| DISTRIBUTION row | Bound | Measured by |
|---|---|---|
| Indexes (shards) per node ≥ 1,000, most idle | The dense profile: 10^7 groups on 3×10^4 nodes; every safety row holds | T4 (dense, tier B); tier C at 1,000 replicas per node on 5 nodes (T8's dense variant) |
| Memory per idle index | At most `track-c.md` D17's per idle `Worker` (16 KiB after D17); the simulator reports heap per idle group | T10 at tier C |
| Open files and mappings per node | Peak per node below 65,530 (`vm.max_map_count`) at 1,000 indexes: at most 64 per index. The simulated disk counts open files and live mappings per node at every instant | T8's dense variant (journal and `Worker`s); tier C+ for strata's segments |
| Commit barriers per node per second | Within Track C2's budget; one barrier per core per turn (`track-c.md` D11), so the rate per node does not grow with groups per node. The simulated disk counts file and directory barriers per node | T8, T9 |
| Flush, merge, and reclamation work per node | Bytes flushed, merged, and reclaimed per node per second within the budget the core states; never one unbounded task per index | Tier C+ only: it needs SIM-R6's stepping executor (D7), so it waits for Phase 3R |

## D2. Determinism

One run is a pure function of its seed, its parameters, and its scenario file.

- **One thread, one queue.** Events run in order of `(time, sequence)`, where `sequence` is
  the order of scheduling. Equal times never depend on heap order or addresses. An event's
  identity is its 64-bit sequence, so cancellation cannot match a reused slot (SIM-R17).
- **Splittable random streams.** The seed feeds splitmix64 (Steele et al., OOPSLA 2014);
  every component draws from its own stream, derived by mixing its parent's key with a
  label (`node/17/disk`, `faults/partition`). Each stream is xoshiro256** (Blackman and
  Vigna, ACM TOMS 2021). A child is a pure function of its parent's key and its label, so
  labels are unique per parent: two components given one label would draw the same values
  (SIM-R19). A component that draws one more value shifts only its own stream (D10 states
  what that gives shrinking).
- **Integer distributions.** Bounded integers use Lemire's method (ACM TOMACS 2019), whose
  threshold's modulo runs only when the product's low word falls below n (SIM-R19).
  Probabilities are parts per million. Latencies are drawn from quantile tables in integer
  nanoseconds with linear interpolation. `std::uniform_int_distribution` and the other
  standard distributions are banned: their outputs differ between libstdc++ and libc++.
- **Banned in code the simulator runs** (SIM-R1 completes the list). Each differs between
  Linux, macOS, and FreeBSD, or between x86_64 and arm64, or between runs:
  - `std::random_device`, the `random` library's per-thread generators, `steady_clock` and
    `system_clock`, and threads;
  - the order of equivalent elements after `std::sort`, `std::partial_sort`,
    `std::nth_element`, and the heap operations, so `std::priority_queue` ties too: the
    standard leaves it unspecified and the two libraries implement different algorithms.
    A comparator that reaches an output is a total order (break ties by a key), or the
    sort is `std::stable_sort`;
  - `std::shuffle` and `std::sample` (the algorithm is unspecified); the simulator's own
    Fisher–Yates over `stream::below` replaces them;
  - `std::hash` (implementation-defined; the libraries hash strings differently), and with
    it the iteration order of every unordered container whose order reaches an output.
    Lookup in an unordered container is fine;
  - `long double` (80-bit on x86_64, binary128 on Linux and FreeBSD arm64, 64-bit on macOS
    arm64);
  - arithmetic on plain `char` (unsigned on Linux and FreeBSD arm64 under AAPCS64, signed
    elsewhere, including Apple's arm64): bytes are `unsigned char` or `std::uint8_t`;
  - the environment: nothing decides from `getenv`. faultfs's `STRATA_FAULTFS_MAP`
    (`faultfs.hh:83`) becomes a constructor option of the simulated disk (D5);
  - ordering by pointer, and libm transcendental functions in decisions.
- **Floating point.** `+ − × ÷ √` are allowed: IEEE 754 rounds them correctly once
  contraction is off. `-ffp-contract=off` is an interface compile option of the `simulation`
  target, so every target that links it compiles with it; a production library linked into
  a simulator binary (consensus, journal, strata) is built with the option too, which the
  adapter's CMake sets on the fetched targets (SIM-R1). Without it clang contracts within
  expressions and GCC's GNU modes contract freely, and arm64 fuses into FMA where x86_64's
  baseline does not.
- **The trace hash.** Every event folds its time, kind, and node into a 64-bit running hash,
  and handlers fold digests of what they changed. `at` and `after` take the kind and node
  with no defaults (SIM-R1): two runs that order same-time events differently must hash
  differently, which unlabeled events would not. Every 65,536 events the hash is logged.
  Two runs of one seed must agree; when they do not, the logged hashes bisect to the
  first window that differs. A checkpoint is taken after its event's labels fold and
  before its handler runs, so a payload traced by event 65,536 · k lands in window k + 1
  (SIM-R19). T1's perturbation test swaps two same-time events of different kinds and finds
  the first differing checkpoint.
- **Across platforms.** The engine and tiers A and B give the same trace hash on Linux,
  macOS, and FreeBSD, x86_64 and arm64, under libstdc++ and libc++. Tier C with strata's
  scoring needs only the same hash per platform, since BM25 calls `log`.

## D3. Time and clocks

- Simulated time is `uint64` nanoseconds from 0. Nothing waits in real time.
- Each node has a clock `local(t) = offset + t × rate_ppm / 10^6`, computed in 128 bits and
  monotone. Rates range over 0.5×–2× (T2's range) for safety runs and ±200 ppm for latency
  runs, fixed per node per run. A rate that changes mid-run (drift) needs a piecewise clock,
  one segment per change, since an unsigned offset cannot absorb a faster rate; D6 adds it
  only with a drift fault (SIM-R19). Clock steps model NTP corrections where code reads a
  wall clock (none should).
- Production code receives time only as an input: `consensus` `Tick{now, election_delay}`
  (`types.h:176`, handled at `core.h:433`), strata's `executor::now()` (`executor.hh:40`),
  `task::inline_scheduler::now()` (`task.hh:122`). Election delays come from the node's
  stream.

## D4. Network

- **Topology.** A node id packs region, zone, rack, and position (D1's 5 × 4 × 125 × 40).
  One-way latency comes from a quantile table per distance class: same rack (p50 50 µs),
  same zone (100 µs), other zone (500 µs), other region (30 ms). Each default table is
  half its p50 at the floor, 4× at p99, 6× at p99.9, 8× at p99.99, and 10× at the top
  (SIM-R11).
- **Connections** (SIM-R7). The peer transport is TCP (DISTRIBUTION, "Layers"). A
  connection joins two nodes, with four lanes each way; frames on one lane arrive in send
  order or not at all, and reordering exists only across lanes and connections. Bulk frames
  are paced by a per-link bandwidth (10 Gb/s default), so snapshot transfers take time
  proportional to bytes.
  - **In order or not at all.** A frame whose link is unreachable when it arrives is lost,
    and its loss resets the connection: every frame in flight either way is dropped, and
    both ends are told at that instant. A later frame on the lane cannot arrive after a heal
    and leave a hole, which production code may rely on (the snapshot channel's offsets,
    D16's credit windows). A reset the fault planner injects does the same.
  - **No oracle on send.** `send` returns nothing. A sender learns of a partition only as a
    reset, as TCP's timeout or RST tells it; the simulator tells it at once, and a host that
    models a retransmission timeout delays the notice itself.
  - **Clogs.** A clog holds every frame on the link until it clears, including frames
    already in flight, and then releases them in order (FoundationDB's simulation, Zhou et
    al., SIGMOD 2021, injects the same).
- **Datagrams.** If D2 chooses a datagram path for probes, datagrams are lost, duplicated,
  and reordered independently, per link class. Their send returns nothing either; a clog
  holds them, and a partition at arrival drops them.
- **Faults.** A partition colors nodes and cuts links between colors; asymmetric and one-way
  cuts are link sets. Gray nodes add loss or latency to one node's links.
- **Cost.** A partition is a byte per node (its color) plus the set of cut links. A link
  record exists only while it has frames in flight or a clog, and is evicted when idle, so
  the network's state is O(nodes + frames in flight + active clogs + cuts), never O(pairs
  ever used) (SIM-R7, against "nothing it keeps per node grows with n").

## D5. Disks

- **One in-memory disk** (SIM-R4). faultfs's crash model, on a store built for many nodes:
  - **The model, from faultfs:** torn sectors at both sizes (512 bytes and 4 KiB), namespace
    changes durable only after a directory sync with any ordered subset kept at power loss
    (`faultfs.hh:163-184`), EIO and ENOSPC rules, short writes, failed syncs that drop
    unsynced bytes (the Linux behavior Rebello et al., USENIX ATC 2020, document), and media
    faults on reads (latent sector errors and silent bit flips), each drawn from the node's
    `disk` stream.
  - **The store:** sparse and sector-granular. A file is a map of written sectors; holes
    read as zeros; a sector keeps one copy when clean and two (current and durable) only
    while dirty. Reads and writes cost O(bytes touched); a sync and a power cut cost
    O(dirty sectors and pending namespace changes); preallocation and truncation extend or
    shrink a size and cost O(1) plus the sectors they free. faultfs's `File::sync` copies
    the whole file (`faultfs.hh:354`): 11 ms for a 4 KiB write and a sync on a 64 MiB file,
    which a turn log's preallocated segments would pay at every barrier.
  - **Directories:** hierarchical paths, each directory with its own `sync_directory` and
    owner lock, so a node's groups and stores have directories of their own; one power cut
    applies to the whole disk. faultfs allows one store per filesystem and single-component
    names (`faultfs.hh:72, 390`).
  - **No environment:** whether `File::map` maps is a constructor option, and mapped reads
    see media faults as reads do (faultfs's mapped reads bypass them).
  - **Interfaces:** `completion`'s `IO`, `File`, and `DirectoryCursor` (`io.h:116, 73,
    101`) for `journal` and `Worker`s, and strata's faultfs interface. strata's faultfs
    tests pass unchanged on it; faultfs then becomes a thin layer over it once Q1 decides
    the home, and `journal`'s `MemoryIO` retires when `journal`'s tests can fetch it.
  - **Counters:** open files, live mappings, file barriers, and directory barriers per node
    at every instant, for D1's per-node rows.
- **Latency.** Each operation completes at a time drawn from the node's disk profile:
  writes, reads, file barriers, and directory barriers, each a quantile table. Profiles come
  from measurements: `docs/perf/commit-barriers.md` (ext4 9.5 ms, UFS 13.4 ms, APFS arm64
  12 ms, APFS x86_64 46 ms for 9 files and a directory barrier) and T6's journal barriers
  when measured. Gray disks multiply a profile by 10.
- **Completion.** `completion` operations are host-driven: an `IOOperation` is `submitted()`
  and later `complete(...)`d (`operation.h:63-71`), and `consensus::Worker` hands its
  operation to the host through `take_io_operation()` (`worker.h:211`). The simulator's
  completion queue keeps the `submit`/`poll` shape of the native queues
  (`bsd_completion.h:92`, `:156`), runs the primitive against the disk at its completion
  time, and completes the original. `journal::TurnDriver` is driven the same way: the host
  writes `write().bytes` and calls `completed()` or `failed()` (`turn_driver.h:64-90`).
- **Power loss and disk loss.** Power loss crashes the node and applies the crash rules.
  Disk loss replaces the disk with an empty one; the node rejoins as a new incarnation
  (Track C, D5).
- **Beyond the model.** A profile where `fsync` returns before persisting (REPLICATION lists
  it outside the guarantee) runs only in the oracle self-check (D6, T14).

## D6. Faults and schedules

- **Within the model** (SIM-R2). Raft's safety holds under any number of crashes with
  durable state and any message loss or partition (Ongaro, §3.6, §3.8). What f bounds is
  durable state lost or damaged. So within the model:
  - any crashes, power loss under honest barriers, partitions, clogs, clock skew, and gray
    failures, with no budget: majority crashes and majority partitions are where
    election-safety and leader-completeness bugs live, and T2's harness already crashes
    and partitions without one (`simulation_test.cc:23-31`);
  - at most f of each group's 2f + 1 voters whose durable state is lost (disk loss) or
    damaged (corruption, a lost unsynced suffix past what the barrier promised). Such a
    voter counts against f until a new incarnation has caught up and the old one is
    removed (`track-c.md` D5). TigerBeetle's simulator documentation applies its rule to
    storage faults the same way.
- **The planner** checks only that budget, per group a fault touches: a node's disk touches
  about 300 groups, a rack's 12,000. It needs no check for crashes or partitions.
- **Beyond the model.** A separate profile breaks the premise on purpose: f + 1 voters'
  durable state lost or damaged, or a barrier that lies. The loss oracle must report loss
  there (T14); a run that reports none fails, since its oracle is blind.
- **Liveness under faults.** Liveness bounds are measured after the heal, and during faults
  only for groups that keep a connected live quorum.
- **Swarm.** Each seed enables a random subset of fault classes and workload operations
  (Groce et al., ISSTA 2012, "Swarm Testing"): a seed without partitions reaches deep crash
  interleavings that constant partitions would interrupt.
- **Safety, then liveness.** A run injects faults for its fault phase, then heals everything
  and requires progress within the liveness bounds: every group with a quorum elects and
  commits, membership converges, placement reaches its targets.
- **Schedules are data.** The planner draws the fault schedule from its stream before the
  run as a list of `(time, fault, target)` entries, where a target may be symbolic ("the
  leader of group g", resolved when it fires). Shrinking edits this list (D10).
- **Sometimes counters.** Every rare event the gate needs reached (a configuration entry in a
  replaced suffix, a crash between promotion and removal, a move during a leader change, a
  placement-group split during a node loss) has a counter, as Antithesis's "sometimes"
  assertions do. A gate row names each counter's minimum (T2).

## D7. Production code inside the simulator

The rule: decisions live in sans-I/O state machines that take events and time and return
actions; drivers (sockets, timers, threads, files) do no deciding. The simulator replaces the
drivers and runs the state machines unchanged. FoundationDB's simulation replaces its network
and file interfaces the same way (Zhou et al., SIGMOD 2021).

The table as it is at the commits read (SIM-R6):

| Library | Seam | Status | Use |
|---|---|---|---|
| `consensus` `Core` | `step(event)` returns actions; time from `Tick`; election delays from the host (`core.h:433-435`) | Exists. `Core` ignores a `Tick` whose election delay lies outside `[election_min, election_max]` (`core.h:434`), so the adapter asserts the range | Authority groups at every tier; shard groups at tier C |
| `consensus` `Worker` | `run_one(Tick)` (`worker.h:447`), `take_io_operation()` (`:211`), the turn-driver constructor (`:111-117`) | Exists | Shard groups at tier C |
| `consensus` transport codec | `encode`/`decode` are pure (`transport/codec.h:18`, `:117`) | Exists | Every simulated frame is encoded and decoded, so the wire format runs |
| `consensus` transport `Peers` | Asio executor, TLS, and `steady_clock` read inline (`transport/peers.h:171`, `:190`, `:380`; `session.h:123`) | No seam. D15's flights run in `Core`, so the pipeline window runs. What does not: the control queue of 4 frames and its drop when full, the data queue (4 frames, 4 MiB), control-first ordering, the reconnect backoff (`peers.h:33-36, 126-150, 331`), and D12's node-pair keepalive detector, which the group-failover bound rests on (`track-c.md:647-650`) | The adapter models the transport until Q3 lands; T9 and T12 state the gap |
| `journal` identities | `detail::random_identity()` reads `std::random_device` (`primitives.h:33`), called at `artifact.h:51` (through `OwnerSession::io_lifetime`), `artifact.h:136`, `journal.h:461`, and `mutation.h:40` | **Must be added** (Q4). Three call sites reach an `IO`; `PublicationSteps` has none (`mutation.h:38-40`), so its caller passes the identity in | Temporary names and stored identities differ between runs of one seed until it lands, so tier C's trace hash is not reproducible |
| `completion` | `File`, `DirectoryCursor`, `IO` (`io.h:73`, `:101`, `:116`; `IO` is in namespace `kronuz::journal`); `IOOperation` (`operation.h:63`) | Exists; **Q4 changes it**: one defaulted virtual `IO::new_identity()`, `random_device` in `PosixIO`, the node's stream on the simulated disk | The simulator's disk and completion queue implement them |
| `reactor` | `io_context` threads, `steady_clock` at accept (`reactor.h:287`, `:326`) | None needed | The simulator never runs `reactor`. It stays a driver, tested on real nodes |
| strata | `executor` (`now()`, `sleep_until`, `executor.hh:40-42`); `inline_executor` (`:51`) | **Must be added for tier C+.** `inline_executor::sleep_until` advances its own manual clock (`executor.hh:62`), and the merge's rate limit and the memory-budget wait call it in loops (`index.hh:929, 1096-1098`): in a cluster, a node's background work would move its own clock while nothing else runs, and its waits would never see a flush, a merge, or a budget release. The multi-run flush starts raw `std::thread`s (`index.hh:1718-1719`), which D2 bans. Tier C+ needs a stepping executor, where work suspends at `sleep_until` and `checkpoint` and resumes on an engine event (the intent of `executor.hh:43-45`), and the flush's runs posted to the executor. Both are strata's own changes | Tier C+ with strata's core |
| `task` | `inline_scheduler` (`task.hh:118-143`) | Exists | Coroutine hosts |
| `cluster` | `liveness.h:59` and `raft.h:144` read `steady_clock` and `std::random_device` | Not carried forward | DISTRIBUTION retires both |
| D2 membership, D3 placement, D5 coordinator | Not yet written | **Requirement** | Each is a sans-I/O state machine with time as input; D2's member view is a container type parameter whose merge is a join, stated as a lattice (incarnation, then status), with Lifeguard's confirmer sets merged by union (Q2's conditions); no per-node state linear in n except that view (D8) |

The library changes the simulator needs are Q3 (`consensus`'s `Peers` policy), Q4
(`completion`'s `IO` and `journal`'s call sites), and Q7 (`metrics`' histogram). Each is
additive, defaults to today's behavior, and lands coordinated with the Detent work first,
with Detent's build green at the pin move (PLAN, Track C; condition C3). They are proposals
in this design; nothing here edits those libraries. strata's stepping executor is strata's
own change.

## D8. Fidelity tiers

Tiers are cumulative. A scenario names its tier and runs unchanged at any tier that has the
components it touches.

- **Tier A, membership and authority at full scale.** D2's membership code on every node;
  the root and every placement group as real `consensus` `Core` groups, since there are
  only about 440 of them; D3's placement code as their state machine. Shard groups exist
  only as placement records. 10^5 nodes. Their store (SIM-R12): T2's `Durable` model keeps
  every entry through the base (`simulation_test.cc:78`), and a placement group's image
  would grow with every decision. Tier A's store keeps the placement map's checkpoint as
  its image, truncates the log at each checkpoint, and installs the contract's initial state
  as a snapshot at time 0 instead of replaying 3×10^7 placements as proposals.
- **Tier B, shard groups as abstract state machines at full scale.** Tier A plus every
  shard group as a simulator-owned model: members and roles, leader, term, commit index, each
  member's durable and received index and the term of its last entry, quiescence, and
  timers. Elections, commit latency, and catch-up draw from distributions calibrated on
  tier C (T9). A crash drops each member's received-but-not-durable suffix. **Elections**
  (SIM-R12) follow Raft's vote rule, not its best case: a candidate wins if a reachable
  majority each hold a log no more up to date than its own (§3.6), and tier B draws the
  winner among every such candidate from the group's stream. A less current winner
  truncates the uncommitted suffixes that diverge from its log, the case D1's
  acknowledged-prefix row and configuration rollback depend on. The model does not test
  Raft; it tests placement, membership changes, and failure handling against Raft's
  contract, at 10^7 groups. A batch is one event chain, not one per write. 10^5 nodes and
  10^7 groups.
- **Tier C, full groups at a stated smaller scale.** Tier A plus every shard group as a real
  `Worker` on the node's `journal` turn log (D11 of `track-c.md`) over the simulated disk
  (D5), with a register state machine; tier C+ applies batches to strata's core through
  `strata::group` once Phase 3R builds it and strata has its stepping executor (D7). 300
  nodes × 100 replicas before D17's memory work, 1,000 nodes × 100 replicas after.
  Tier C's turn log runs with `TurnLogOptions{segment_bytes = 1 MiB, block_bytes = 32 KiB,
  spare_segments = 1, retain_segments = 4, forward_bytes = 256 KiB}` (SIM-R4): at most 5
  segments of written sectors, 5 MiB per node. Production's 64 MiB segments cost nothing
  preallocated on the sparse disk but up to 320 MiB once written; tier C rotates segments
  64× more often, which exercises moving records forward, and a scenario that needs
  production's sizes runs at fewer nodes.

Memory and time per tier. **All figures are estimates** from the structures' sizes; T10
replaces them with measurements and the hardware they ran on.

| Tier | Per node | Per group | Total at the contract | Events per simulated second | Wall time per simulated second |
|---|---|---|---|---|---|
| A | About 5 KiB: SWIM state 256 B, a piggyback queue capped at 512 entries (4 KiB), suspicion timers, in-flight datagrams; the member view shared (below), with up to 1.25 KB of window bitset at peak | Placement record about 48 B, held by each of 3 authority replicas | About 2.1 GB: 0.5 GB nodes, 1.4 GB placement maps (10^7 × 48 B × 3), authority cores, 0.125 GB of view windows at peak | About 6 per node: 6×10^5 | About 0.2 s at 0.3 µs per event |
| B | As A | Plus about 128 B of abstract state: 1.3 GB | About 3.5 GB | Plus 8 per batch: 8×10^5 at 10^4 active groups × 10 batches/s; idle groups cost none | About 0.4 s; a zone outage adds about 10^7 events, 3 s |
| C, before D17 | 100 replicas × (133 KiB idle `Worker` heap, `track-c.md` "Starting point", + 8 KiB of checkpoint files) + at most 5 MiB of turn log: at most 18.8 MiB | | 300 nodes: at most 5.5 GiB | About 60 per idle group before D12's quiescence (heartbeats every 100 ms): 6×10^5 at 10^4 groups | About 1.2 s at 2 µs per event (core step, codec); a barrier costs O(dirty sectors), microseconds |
| C, after D17 and D12 | 100 × (8 KiB + 8 KiB) + at most 5 MiB: at most 6.6 MiB | | 1,000 nodes: at most 6.4 GiB | Keepalives only at rest: 2P/k = 256 frames per node per second | About 0.3 s at rest |

Revision 0 estimated tier C on faultfs, whose whole-file copies and dual images made a node
at default segment sizes at least 256 MiB, 75 GiB at 300 nodes, and each barrier about 11 ms
of wall time (SIM-R4, SIM-R12).

The member view is the one structure that would be quadratic: a full view at every node is
10^5 × 10^5 entries, 160 GB at 16 bytes. Tier A therefore runs D2's code over a view
container that shares state across nodes: one table of every update ever made about each
member, in order, and per node a cursor into it plus a bitset over `[cursor, head)` of the
updates it received out of order; the cursor advances past the bitset's leading ones. A
node's view of a member is the join of the table's entries below its cursor and those its
bitset marks, which is exact because the merge is a join (D7's requirement on D2). The
window is not "a few updates" under mass failure (SIM-R12): in the zone outage, 10^4
updates (5,000 deaths and their suspicions) spread at once in random order, so each node
holds an arbitrary subset of a 10^4-update window, 1.25 KB per node and 125 MB at 10^5
nodes at the peak, which tier A's total includes. Production keeps a dense array. The two
containers implement one concept, and a test runs D2's suite over both. Probe order needs no
per-node list: a keyed Feistel permutation over member slots with cycle-walking (Black and
Rogaway, CT-RSA 2002) gives each node its own round-robin order in O(1) state, which D2 may
adopt in production too.

The tiers run on one host: tiers A and B in about 4 GB per process, so the Linux gate VM
(16 GB, 8 vCPUs) runs 3 seeds at once; tier C in at most 6.4 GiB. Under the host rules each
process is one job.

## D9. The linearizability checker (shared with C5)

- **What it checks.** Client histories: each operation's invocation and response times, its
  arguments, and its result, or "unknown" when the client timed out. The model is strata's
  versioned register per document key (SIM-R3; `phase-3.md:235-238`, as `index.hh`'s
  apply implements it):
  - a key is absent at version 0, or present at a version v ≥ 1 with a value;
  - `put(value, expected?)` on an absent key makes version 1, on a present key version
    v + 1; an unconditional put is strata's `replace_key`, and expected 0 is its `add`;
  - `erase(expected?)` makes a present key absent; on an absent key it is accepted and
    deletes nothing, which the history records as `missing`;
  - `get` returns the value and version, or `missing`;
  - expected 0 requires absence; any other expected version must equal the current one; a
    mismatch rejects with the current version (0 when absent), and the checker compares the
    reported version with the model's.

  That covers Phase 3R's "write and get-by-id histories" and REPLICATION's versioned writes.
  Revision 0's register advanced the version on every erase and read expected 0 as "never
  written", so it rejected correct strata histories (a re-add at version 1; expected 0
  after an erase).
- **Algorithm.** Wing and Gong's search (JPDC 1993) with Lowe's memoization of (linearized
  set, model state) (CCPE 2017), split by key under P-compositionality (Horn and Kroening,
  FORTE 2015): a history is linearizable if and only if each key's sub-history is. Porcupine
  (its documentation) and Knossos (Jepsen's documentation) take the same approach.
- **Unknown outcomes** may take effect at any point after their invocation or never, as
  Jepsen's indeterminate operations do. A get without a response constrains nothing and is
  dropped.
- **Long histories.** Each key's history splits at quiescent cuts (no finite operation
  pending), and the checker carries the set of configurations (model state and the unknown
  operations not yet applied) reachable at each cut. Three rules keep the set small without
  losing a future:
  - **Ends only where the last finite operation is linearized** (SIM-R13). A configuration
    reached from another by applying more carried unknown operations has no future the
    first lacks, since the next segment can apply them first; so a segment's search records
    a configuration only at the node that linearizes its last finite operation, and does
    not expand past it. Revision 0 recorded every node after it, which with k unknown puts
    of distinct values outstanding gave 1 + k · 2^(k − 1) configurations (1,025 at k = 8).
  - **Subset dominance** at the same state: a configuration whose pending set is contained
    in another's is dropped, since a pending operation may also never take effect.
  - **The expected-version prune** (a pending conditional operation whose expected version
    is below the current one can only conflict) applies only to keys with no erase in their
    history, where versions never decrease (SIM-R3).

  The bounds: more than 1,024 configurations at a cut, a search past 10^7 nodes in one
  segment from one configuration, or past 10^9 nodes in one key's history, report
  "inconclusive" with the key and the reason, never "pass" (SIM-R13: revision 0 summed
  the per-segment budget over the whole key). Checking is NP-complete in general (Gibbons
  and Korach, SIAM J. Comput. 1997), so generators keep at most 8 operations pending per
  key, pause each key's clients now and then, and retire a key after 4 unknown writes,
  moving its clients to a fresh key, as Jepsen's independent keys do. The target: at most
  0.1% of keys inconclusive per run, each reported (T3, T8).
- **Scale.** Tier C checks every key. Tier B has no values, so its oracles are the counters
  in D1's bounds; it checks linearizability on 10^4 sampled keys whose writes it tracks by
  version.
- **Shared.** The checker is an offline function over a history file, with no dependency on
  `consensus` or strata. C5's harness (`simulation_test.cc` and its successors) records client
  histories in the format below, and the 30-node real cluster records the same format, as
  Jepsen does for real systems, so one checker covers both.
- **Placement histories.** Root and placement-group operations (assign a range, split, move a
  replica) are checked by the no-split-placement oracle at every event, not by search.

### The history file, version 1 (SIM-R3)

A text file, one record per line, `\n`-terminated, at most 256 bytes per line.

```
strata-history 1 simulated
# client key op invoked returned outcome value expected version
7 42 put 1000 2500 ok 9001 - 1
8 42 get 1200 1900 ok 9001 - 1
9 42 erase 3000 - unknown - 1 0
```

- **Header.** The first line is `strata-history`, the format version, and the time base.
  A reader rejects any other version.
- **Time base.** `simulated`: simulated nanoseconds from 0. `monotonic`: nanoseconds of one
  host's monotonic clock; on a real cluster every client runs on one control host, which
  takes both timestamps, as Jepsen runs, so invocation and response share a clock.
- **Fields,** separated by one space; numbers are unsigned 64-bit decimal without sign or
  leading zeros; `-` marks an absent field:
  - `client`: the recording process. A client's operations do not overlap (each is invoked
    at or after the previous one's response), and an unknown is a client's last: after a
    timeout it continues under a new id;
  - `key`: the document key's 64-bit id, which the recorder assigns;
  - `op`: `put`, `erase`, or `get`;
  - `invoked`, `returned`: times in the base; `returned` is `-` exactly when the outcome is
    `unknown`;
  - `outcome`: `ok`, `conflict`, `missing`, or `unknown`;
  - `value`: a put's written value and an `ok` get's read value; `-` for erases and for gets
    that are `missing` or `unknown`. Generators write values unique per key;
  - `expected`: a put's or erase's expected version, `-` when unconditional and for gets;
  - `version`: an `ok` put's written version, an `ok` get's read version, a conflict's
    reported current version, and 0 otherwise.
- **Comments** start with `#` at the beginning of a line. Records may appear in any order.
- **Errors** name the line. T13 fuzzes the reader.

## D10. Shrinking a failing seed

A failing seed reproduces exactly. Shrinking makes it small enough to read:

1. **Cut the length.** The failure is detected at event k; the run is cut to end at k.
2. **Shrink the fault schedule.** Delta debugging (Zeller and Hildebrandt, IEEE TSE 2002)
   over the schedule's entries: remove halves, then quarters, down to single entries,
   keeping each removal that still fails. Then move entries earlier and merge partitions.
3. **Shrink the workload.** Halve clients, keys, groups, and nodes while the failure holds;
   generators map smaller draws to simpler choices, the property Hypothesis's reducer relies
   on (MacIver and Donaldson, ECOOP 2020). Shrinking the population keeps symbolic targets
   meaningful because a scenario carries its topology and the placement subset the failure
   touches (SIM-R14).
4. **Emit a scenario.** The result is a scenario file: seed, parameters, topology, placement
   subset, and the explicit schedule, versioned, under `test/scenarios/`, and replayed as a
   test. A scenario keeps reproducing after unrelated code changes add draws, which a bare
   seed does not.

**What stays stable** (SIM-R14). Streams are per component, so removing a fault leaves the
draws of every component it did not touch unchanged. A component's own draws are sequential:
the network draws every latency of a node from that node's stream (`network.hh`), so a
removed fault that changes how many frames a node sends shifts every later latency of that
node. Revision 0 claimed more ("removing a fault does not reshuffle" latencies). If shrinking
shows that shift matters, S4 keys latency draws by event (link and frame ordinal) to narrow
it to the link.

Each shrinking attempt is one full run, so shrinking costs runs, not memory. T11 bounds it
per tier.

## D11. Where the code lives

Three homes were considered:

| Home | For | Against |
|---|---|---|
| strata | strata owns D1 | `consensus` and `journal` cannot fetch strata for their tests (C5): strata depends on them |
| `consensus` | C5's harness sits beside the code it tests | Network, disk, membership, and placement models are not consensus; `LIBRARY_BOUNDARIES.md` split `cluster` because it grew four libraries in one, and this would start the same in a library Detent links |
| **A new library, `Kronuz/simulation`** | One engine for every consumer; depends only on `completion`'s `io.h` and `metrics` (Q7) | One more repository; adapters for `consensus` need a home |

**Recommendation: the new library** (Q1). The engine, network, disk (faultfs's model,
moved), fault planner, checker, and shrinker go there, depending only on `completion` and
`metrics`; `simulation` carries SIM-R1's flags as interface options. The `consensus` adapter
(a group host driving `Core` and `Worker` on the simulator) is an optional target in
`consensus` (`consensus::simulation`, off by default), so Detent's build is unchanged. strata
fetches both. No cycle: `simulation` → `completion`, `metrics`; `consensus` tests →
`simulation`. The catalog has no deterministic simulator, and `Kronuz/random` (one
`std::mt19937_64` per thread, seeded from `std::random_device`) is what D2 bans, so
`random.hh` duplicates nothing.

Until the owner decides, the code starts on this branch as `sim/`, a self-contained CMake
project in namespace `kronuz::simulation` with no strata include, so that `git filter-repo
--subdirectory-filter sim` turns it into the library with its history if Q1 goes that way.

## D12. Runs and reports

- `sim_run --tier a|b|c --profile nominal|stress|dense|beyond|coverage --seeds FIRST-LAST
  (--hours H | --minutes M | --steps N) [--nodes N] [--groups G] [--voters V]
  [--scenario FILE]` runs seeds, one per process, and writes one JSON line per seed.
- `sim_shrink --seed S [...]` shrinks a failure into a scenario file.
- `sim_check HISTORY` runs the linearizability checker on a history file (D9's format) and
  exits 0 when linearizable, 1 on a violation, 2 when inconclusive, and 3 when the file is
  malformed.
- **The report line** (SIM-R5) is versioned: `{"format": "strata-sim-run", "version": 1,
  ...}` with the commit, the host, the parameters, the trace hash and its checkpoints'
  count, every bound's measured value, every sometimes counter, events, peak RSS, wall
  time, and the histograms. A reader rejects other versions.
- **Histograms** (SIM-R5). `metrics`' only histogram has 14 bounds at powers of 4 from 1 µs
  to 64 s (`metrics.hh:64-66`): 5 s falls in (4, 16] s, 300 s and 15 min in the overflow,
  and T9's ±10% cannot be read from such buckets. The simulator records every latency and
  duration in a log-linear histogram: values below 128 ns exact, and above, 128 sub-buckets
  per power of two, whose index is an integer function of the value's bit width and a
  shift. Over 1 ns to 2^64 ns that is 7,424 counters (58 KiB), with relative error at most
  2^−7 (0.79%). Histograms merge across seeds by adding counters, so T9's bootstrap
  resamples seeds. A quantile reports its bucket's upper edge, so a bound is never
  understated, and every reported bound states its error. The line carries a histogram as
  its nonzero `[index, count]` pairs. Where the histogram lives is Q7.
- Metrics are fixed histograms, never unbounded sample lists.

## Prior art

From published papers and documentation only; no code was read or copied.

- **FoundationDB** (Zhou et al., SIGMOD 2021): the whole database runs in one deterministic
  process against simulated network and disks, with fault injection points in its own code
  and clogged connections. D1 takes the single-process, interface-replacement model.
  FoundationDB writes its code against its own actor framework so all of it runs simulated;
  strata cannot run `reactor` that way, so D7 moves decisions out of drivers instead.
- **TigerBeetle's VOPR** (TigerBeetle documentation): a whole cluster in one process from a
  seed, with storage faults kept within what the cluster can recover from, and separate
  safety and liveness checking. D6 takes the storage-fault budget and the heal-then-progress
  phase.
- **Jepsen, Knossos, Elle, Porcupine:** black-box testing of real clusters with generated
  faults and an offline checker. Knossos and Porcupine check linearizability (Porcupine with
  P-compositionality); Elle (Kingsbury and Alvaro, VLDB 2020) checks transactional isolation
  and is the reference if strata adds multi-document transactions. D9 takes the offline
  checker, indeterminate operations, independent keys, and one control host's clock; the
  real-cluster gate runs Jepsen-style.
- **Antithesis** (its documentation): unmodified software in a deterministic hypervisor, with
  "always" and "sometimes" properties. D6's sometimes counters follow it; a hypervisor would
  also cover `reactor` and `Peers`, which D7 does not.
- **Membership:** SWIM (Das et al., DSN 2002), Lifeguard (Dadgar et al., 2017), HyParView
  (Leitão et al., DSN 2007, partial views), Rapid (Suresh et al., USENIX ATC 2018),
  memberlist's documentation (its retransmit multiplier and gossip interval). They set D1's
  membership bounds and are D2's to choose among.
- **Histograms:** HdrHistogram's documentation describes the log-linear bucketing D12 uses.

## Scale (of the simulator itself)

At the contract the simulator holds about 4 GB per tier-B process and runs about 2.5×
faster than real time at nominal load (estimates). Its cost per node is constant except for
the shared member view (O(updates), plus each node's window at peak), and per group
constant; the network keeps state only for frames in flight and active clogs (D4). Nothing
it keeps per node grows with n. A 2×10^5-node run (T10) checks that the estimate scales
linearly.

## Gate rows

Each row runs from `sim/` (or the library, after Q1) at the gate's commit. "Seeds" are
consecutive from 1 unless stated. Every row's JSON lines, commit, and host are kept under
`docs/gates/` with their histograms (condition C4). Budgets are CPU-hours at D8's estimates;
T10 replaces the estimates before T4 runs, and a row whose measured cost exceeds its budget
by more than 1.5× is restated before it runs. The gate VM is the Linux gate host (16 GB, 8
vCPUs), three simulator processes at once, scheduled by the lead outside benchmark holds.

| Row | Target | Procedure |
|---|---|---|
| T1 Determinism | Two runs of each seed give the same trace hash and checkpoints. Pinned hashes match on Linux x86_64 (gcc and clang), Linux arm64, macOS x86_64 and arm64, and FreeBSD, under libstdc++ and libc++. The perturbation test finds the first differing checkpoint. Tier C is excluded until Q4 lands | (a) Every push: the engine, network, and checker tests' pinned hashes in CI on every platform. (b) At a gate: `sim_run --tier a --nodes 1000 --groups 10000 --seeds 1-1000 --minutes 10`, then `--tier b`, each twice, comparing the JSON lines' hashes (about 1 CPU-hour). (c) At a gate: seeds 1–10 at the contract for 1 simulated minute, tiers A and B, against `sim/test/pinned.json`, on the gate VM, the FreeBSD VM, and the macOS arm64 and x86_64 hosts (about 20 minutes per host) |
| T2 Coverage | Every fault class and every sometimes counter reached at least 100 times within the budget of the row that runs it; each count recorded. Tier B's classes (crash, restart, disk loss, rack power loss, top-of-rack failure, bad push, partitions, clogs, gray nodes, skew) count over T4. Tier C's (power loss at each barrier kind: journal, segment, manifest; corruption per file class; failed sync; EIO; ENOSPC; latent sector errors and bit flips injected at 1 per 10^8 bytes read, counted apart from the nominal rates; a configuration entry in a replaced suffix; a crash between promotion and removal) count over T8 with `--profile coverage` | The counters in T4's and T8's JSON lines; `sim/tools/coverage.py` lists every count under 100 |
| T3 Checker | Accepts every history of a corpus of linearizable histories and rejects every one of a corpus of non-linearizable ones. Each class by hand and by mutating a correct history: stale reads, lost writes, reordered versions, an unknown write seen then unseen, a wrong conflict version, a re-add at version 1, expected 0 after an erase. Agrees with an independent brute force on at least 50,000 random histories of up to 7 operations, with 0 mismatches. A history with 10 unknown puts carried across cuts is linearizable, not inconclusive. Reports inconclusive past its bounds, never pass. The history reader round-trips and rejects malformed lines and other versions | `sim/test/checker_test.cc` and `sim/test/history_test.cc` (CTest `simulation_checker`, `simulation_history`) in CI on every platform |
| T4 Safety at full scale | Every safety bound of D1 holds over 200 seeds × 1 simulated hour at nominal and 100 at stress, tier B, 10^5 nodes and 10^7 groups; over 20 seeds of the dense profile; and over 50 seeds of each scripted scenario (zone outage, rack loss during a rolling upgrade, region partition, two zones in two regions, a placement-group split during a node loss, a bad push, a top-of-rack failure) | `sim_run --tier b --profile nominal --seeds 1-200 --hours 1`; `--profile stress --seeds 1-100 --hours 1`; `--profile dense --seeds 1-20 --hours 1`; `--scenario FILE --seeds 1-50 --hours 1` for each file in `sim/scenarios/`. Budget: nominal 80 CPU-hours at 0.4 s per simulated second; stress about 120 (3× the events); dense about 8; scenarios about 140; about 350 in all, about 120 hours on the gate VM |
| T5 Membership | D1's detection, dissemination, false-suspicion, and cost rows at 10^5 nodes. The cost exponent: the least-squares slope of log(messages per node per second) on log n over n = 10^4, 3×10^4, 10^5, 2×10^5, each point the median over 50 seeds; the slope at most 0.15 and the upper end of its 95% bootstrap interval (10,000 resamples of seeds) at most 0.2 | `sim_run --tier a --profile nominal --seeds 1-50 --hours 1 --nodes N` for each N; `sim/tools/exponent.py`. Budget about 35 CPU-hours |
| T6 Placement | D1's decision, re-replication, zone, and authority-load rows. Each run is a 1-hour fault phase and 30 minutes of heal. A loss counts for the re-replication row only in the fault phase's first 30 minutes, so its 15-minute hold and 15-minute bound end inside the run; the row's p99 is over at least 200 counted losses | T4's stress seeds (about 230 counted losses) and its nominal seeds; the zone-outage scenario |
| T7 Fan-out | D1's fan-out row over 10^4 queries to the 16,384-shard indexes, with 1 node in 1,000 gray, the index's replicas in one region and its coordinator there, D1's per-shard table and hedging (at most 5% extra requests) | `sim_run --tier b --scenario sim/scenarios/fanout.scn --seeds 1-20` |
| T8 Full fidelity | Phase 3R's rows at tier C. A step is one input delivered to a production state machine (a message, a tick, a completion, a client request, or a fault), T2's unit and Phase 3R's (`PLAN.md:470-472`). 3 voters on 5 nodes × 8 groups and 5 voters on 7 nodes × 8 groups; 10^5 steps per seed (about 1 simulated minute), 1,000 seeds each: 10^8 steps per voter count, 10× Phase 3R's floor. Crashes, power loss at every barrier, partitions, clock skew, membership changes, and corruption of every file class. The checker passes on every history, with at most 0.1% of keys inconclusive, each reported; zero acknowledged loss; zero served corruption. A dense variant: 1,000 replicas per node on 5 nodes, 100 seeds | `sim_run --tier c --voters 3 --nodes 5 --groups 8 --steps 100000 --seeds 1-1000`, then `--voters 5 --nodes 7`; `--profile dense --nodes 5 --groups 1667 --seeds 1-100`. Budget about 4 CPU-hours |
| T9 Tier conformance | Tier B's calibrated quantities (election time, commit latency, catch-up time, unavailability per group after each scripted fault) match tier C's: for each quantity at p50 and p99, the 95% bootstrap interval of the ratio tier B / tier C (10,000 resamples of seeds) lies inside [0.9, 1.1], each over at least 20,000 samples. Calibration and test are disjoint: tier B is calibrated on `sim/scenarios/calibrate/` with seeds 1–20 and tested on `sim/scenarios/conform/` with seeds 101–120, at 300 nodes. Every tier-B failure in T4 is replayed at tier C at reduced scale before it counts for or against a gate | `sim_run --tier b` and `--tier c` with `--nodes 300` and the same files; `sim/tools/conform.py` compares |
| T10 Simulator cost | Tiers A and B at the contract within 4 GiB peak RSS per process; tier C at 300 nodes before D17 within 6 GiB, and at 1,000 nodes after D17 within 7 GiB; tier B within 0.6 s of wall time per simulated second at nominal (1.5× the estimate T4's budget assumes); at 2×10^5 nodes, tiers A and B within 2.1× their 10^5-node RSS. A missed target stops T4 or T8 until that tier is reworked | `/usr/bin/time -v` (Linux) or `-l` (macOS) around 3 seeds of each tier's T4 and T8 runs; host recorded |
| T11 Shrinking | Each seeded mutant's first failing seed shrinks to a scenario of at most 10 fault entries within its tier's budget: tier C at up to 10 nodes 2 CPU-hours; tier C at 300 nodes 8; tier B 24, after first reducing the population to the scenario's topology and placement subset. The scenario fails with the mutant and passes without it | `sim_shrink` on T12's mutants |
| T12 Mutants | Each seeded fault is detected within the budget of the row that names it, with seeds to detection recorded. T8: acknowledging before a quorum is durable; a deposed leader serving a read without a probe; ignoring an fsync failure. T4: one range assigned to two placement groups; removing a voter before its replacement caught up; two voters placed in one zone; a peer-bound check off; a rack upgraded while one of its groups has another voter down. T5: a member declared dead without suspicion; a suspicion timeout that does not grow with n. T3: S3's checker mutants (real-time order off, get versions unchecked, conflict versions unchecked, dominance reversed, the prune applied to keys with erases). T9: tier B's own (leader choice limited to the most current log; a crash keeping the received suffix), caught by the tier C replay. Every new oracle has a mutant | `sim/tools/mutants.sh`, as `consensus` `tools/mutants.sh` does |
| T13 Every gate | ASan and UBSan over every simulator test in CI within the job's time limit (condition C2), and TSan; one CPU-hour of fuzzing for the scenario-file and history-file readers, zero findings, with a hang timeout; coverage at TESTING's targets | CI, `tools/coverage.sh` |
| T14 Beyond the model | The beyond profile reports loss: destroying f + 1 voters' durable state in a group holding acknowledged writes reports loss in 50 of 50 seeds (tiers B and C); a lying barrier followed by power loss reports loss in at least 45 of 50 seeds at tier C, since a lie can go unobserved when no acknowledged write was in its window | `sim_run --tier b --profile beyond --seeds 1-50 --hours 1`; `sim_run --tier c --profile beyond --voters 3 --nodes 5 --groups 8 --steps 100000 --seeds 1-50` |
| T15 Rolling upgrade | D1's upgrade-availability row: 0 groups without a quorum caused by the upgrade, over 5 seeds × 24 h at nominal (500 rounds) and 10 seeds × 6 h at stress (200 rounds), tier B, with nominal churn running | `sim_run --tier b --scenario sim/scenarios/upgrade.scn --seeds 1-5 --hours 24`; `--scenario upgrade-stress.scn --seeds 1-10 --hours 6`. Budget about 72 CPU-hours |

## Open questions for the owner

Each stays the owner's; the reviewer's recommendation (`simulator-design.md`, "The open
questions") is recorded beside the design's.

1. **Q1. Where the code lives.** Options: a new `Kronuz/simulation` library with an optional
   adapter target in `consensus`; `consensus`; strata. Recommendation: the new library
   (D11). Creating it is a public repository, so it waits for the owner; the code starts
   in `sim/` here either way.
   - Review: the new library. The consensus adapter is an optional target, off by default;
     `simulation` carries SIM-R1's flags as interface options and depends on `metrics`
     besides `completion`; until the owner decides, `sim/` stays self-contained.
2. **Q2. The member view (with D2).** Options: (a) full views with a join merge and a view
   container type, which the simulator shares (D8); (b) partial views (HyParView-style,
   O(log n) per node), which need no sharing but give placement groups no full view of
   candidate nodes. Recommendation: (a). It decides whether tier A at 10^5 nodes is about
   2 GB or not feasible on one host; D2 decides with it.
   - Review: (a), 3.2 MB per node at 10^5 fits DISTRIBUTION's "bounded per peer", with
     conditions: SIM-R12's bitset window in the shared container (D8 now states it), and D2
     states the merge as a lattice (incarnation, then status), with Lifeguard's confirmer
     sets merged by union (D7's requirement row now states it).
3. **Q3. `Peers` in the simulator.** Options: (a) extract `Peers`' send policy into a
   sans-I/O class in `consensus` that both `Peers` and the simulator drive, an additive
   change coordinated with Detent; (b) leave it untested in simulation and cover it with
   real-network tests only. Recommendation: (a), done with C4's D16, which replaces that
   policy anyway, and including what D7 lists (the control and data queues, the drop rule,
   control-first ordering, the reconnect backoff, and D12's keepalive detector).
   - Review: (a), extracted with D16, sans-I/O from its first commit, including the control
     and data queues, the drop rule, and D12's keepalive detector. Until then the adapter
     models the transport, and T9 and T12 state the gap.
4. **Q4. The identity source in `journal`.** Options: (a) a virtual `IO::new_identity()` on
   `completion`'s `IO`, defaulting to `random_device`, overridden by the simulator's disk;
   (b) an identity generator passed to `Store` and `TurnDriver`. Recommendation: (a): one
   additive virtual, Detent's `PosixIO` unchanged, and `PublicationSteps` given its identity
   by its caller (D7). It changes `completion` as well as `journal` (SIM-R6). Until it
   lands, tier C runs are reproducible in outcome but their trace hashes are not, so T1
   excludes tier C.
   - Review: (a) in substance: one defaulted virtual on `completion`'s `IO`, `random_device`
     in `PosixIO`, the node's stream in the simulator; `PublicationSteps` gets its identity
     from its caller; coordinated with Detent first, and T1 covers tier C once it lands.
5. **Q5. Failure rates.** Options: the published rates in D1 with 10× stress, or the owner's
   fleet data if there is any. Recommendation: D1's rates; replace them with fleet data
   when it exists.
   - Review: D1's rates with 10× stress, plus two correlated cases they miss: a bad push
     that restarts 10% of the nodes within a minute, and a top-of-rack switch failure that
     partitions a rack without powering it off. D1's churn table now carries both as
     scripted cases, pending the owner's decision.
6. **Q6. What tier B's results prove.** Options: tier B's safety rows count for Track D's
   gates on placement and membership logic only, with Raft's safety from tier C and T2; or
   Track D's gates require tier C runs at their scale too. Recommendation: the first, with
   T9's conformance as its condition.
   - Review: the first, with conditions: T9 on held-out scenarios (now T9's procedure),
     tier B's elections over all of Raft's choices (D8), tier B's own mutants caught (T12),
     and every tier-B failure replayed at tier C at reduced scale before it counts for or
     against a gate (T9).
7. **Q7. Where the log-linear histogram lives** (SIM-R5). Options: (a) extend `metrics`,
   which Detent links, with a log-linear histogram beside today's fixed one, an additive
   change coordinated with Detent; (b) a histogram in `simulation`. Recommendation: (a): one
   implementation per mechanism, and production latency metrics gain the same resolution.
   S5's measurements wait for it.
   - Review: either, `metrics` extended rather than duplicated and coordinated with Detent,
     or `simulation`.

## First steps

S1 to S3 are built on this branch, and revision 1's fixes to them (SIM-R1, SIM-R3, SIM-R7,
SIM-R8, SIM-R11, SIM-R13, SIM-R17, SIM-R19) land with this revision. S4 and the fault planner
start after the architect's re-check of this revision (condition C5).

- **S1. The deterministic engine** (`sim/include/simulation/`):
  - `random.hh`: `splitmix64`, `xoshiro256ss`, `stream` (derived from a parent key and a
    label), `below(n)` (Lemire), `chance_ppm(p)`, `quantile_table` (integer nanoseconds,
    interpolated).
  - `clock.hh`: `node_clock{offset, rate_ppm}` with `local(t)`, monotone, 128-bit math.
  - `engine.hh`: `engine` with `now()`, `at(t, event, kind, node)`, `after(d, event, kind,
    node)`, `cancel(id)`, `run_until(t)`, `run_steps(n)`, `drain()`; a 4-ary heap of
    24-byte `(time, sequence, slot)` entries, event bodies in a slab, cancellation by
    sequence; the trace hash and its checkpoints.
  - Tests: two runs of one seed agree; a pinned hash for a fixed scenario; ties ordered by
    scheduling; cancellation, also across 2^32 reuses of a slot (a slow test, off by
    default); the perturbation test; labels required; `below(n)` unbiased (a 16-bit
    instantiation checked exhaustively over every input for n up to 257, chi-square for
    the 64-bit one); streams independent of sibling draws; clock monotonicity at every rate.
- **S2. The network** (`network.hh`): topology ids, distance classes, latency tables,
  connections in order or not at all with resets told to both ends, datagrams, partitions by
  color, cut links, clogs that hold frames in flight, bandwidth pacing, idle-link eviction.
  Tests: FIFO per connection and lane, reordering only across lanes and connections,
  partitions symmetric and one-way, no hole across a heal, clogs over frames in flight,
  eviction, bytes over bandwidth, and a randomized workload with an in-order-or-not-at-all
  property check and a pinned hash.
- **S3. The checker** (`checker.hh`, `history.hh`, `sim/tools/sim_check.cc`): strata's
  register, WGL with Lowe's cache, the per-key split, quiescent cuts, unknown outcomes,
  dominance by reachability, inconclusive bounds, and the history file's reader and writer.
  Tests: T3's corpora and brute-force cross-check; the reader's round trip and rejections.
- **S4. Tier C0: `consensus` `Core` groups on the engine** (`sim/consensus/`, fetching
  `consensus` at `fc66c0e`): T2's modeled store and oracles ported onto the engine and
  network, a register state machine, clients recording histories in D9's format, and the
  checker over them; the adapter asserts `Tick`'s range.
- **S5. Tier B's abstract groups** at 10^5 nodes and 10^7 groups over a reference placement
  (a copyset ring with the peer bound, until D3's state machine exists), with tier A's
  checkpoint store and the shared view's window: the memory measurement of T10 first, then
  T4's oracles. Its histograms wait for Q7.
- **S6. The disk** (D5): faultfs's model on the sparse store, with its counters; strata's
  faultfs tests run on it.
- **S7. The fault planner and scenario files** (D6, D10): the durable-state budget, swarm
  subsets, schedules as data, sometimes counters, and the scenario reader.
- **S8. Tier C on `Worker`s and turn logs** over S6's disk, after Q4 (identities) and with
  the modeled transport until Q3.
- **S9. Tier C+ with strata's core**, after strata's stepping executor and Phase 3R.

## Done criteria

Numbered for the ledger (`docs/status/simulator.md`), which ticks each with its evidence
(SIM-R16). The lead may add or renumber them.

| ID | Step | Criterion |
|---|---|---|
| K1 | All | Every simulator test passes in CI on Linux x86_64 (gcc, clang), Linux arm64 (gcc), macOS x86_64 and arm64, and FreeBSD, with `-Wall -Wextra -Werror`, at each pushed commit; the gcc job is the libstdc++ `-Werror` build |
| K2 | All | Every simulator test passes under ASan and UBSan locally and in CI's linux-asan-ubsan job within its time limit (C2), and under TSan in CI |
| K3 | S1 | The engine's pinned seed-1 trace hash holds on every K1 platform (T1, C1) |
| K4 | S1 | Labels are required (a compile-time check), and the perturbation test finds the first differing checkpoint (SIM-R1) |
| K5 | S1 | `-ffp-contract=off` is an interface option of `simulation` (SIM-R1) |
| K6 | S1 | Cancellation compares 64-bit sequences; the slot-wrap test (2^32 reuses) passes, and fails on revision 0's engine (SIM-R17) |
| K7 | S2 | In order or not at all: a frame lost in flight resets its connection, later frames on it drop, both ends are told; no hole across a heal; the randomized workload's property check passes (SIM-R7) |
| K8 | S2 | `send` returns nothing; a clog holds frames already in flight; idle links are evicted, with no link records after a drain (SIM-R7) |
| K9 | S2 | The network's pinned seed-1 trace hash holds on every K1 platform (T1) |
| K10 | S3 | strata's register: a re-add at version 1, expected 0 after an erase, and conflict versions checked; the prune only on keys without erases (SIM-R3) |
| K11 | S3 | T3's corpora by hand and by mutation for every class, and the brute-force cross-check on at least 50,000 histories with 0 mismatches |
| K12 | S3 | Dominance by reachability: 10 carried unknown puts are linearizable; the node budget is per segment and configuration, with a total (SIM-R13) |
| K13 | S3 | The history file, version 1: writer and reader round-trip, the reader rejects other versions and malformed records naming the line, and `sim_check` runs on a file |
| K14 | S3 | The history reader is fuzzed for one CPU-hour with zero findings (T13), at the gate |
| K15 | S4 | T2's oracles ported, and each of `consensus`'s 13 mutants (`tools/mutants.sh:22-37`) caught by some suite |
| K16 | S4 | The ported harness catches at least the seven mutants the randomized simulation alone caught at 400 seeds (`track-c-design.md:1757-1761`), with seeds to detection recorded |
| K17 | S4 | Clients record histories in D9's format; the checker passes on every history, with at most 0.1% of keys inconclusive, each reported |
| K18 | S4 | The adapter asserts each `Tick`'s election delay lies in `[election_min, election_max]` (SIM-R6) |
| K19 | S5 | Tier A's store keeps the placement map's checkpoint and installs the initial state as a snapshot; the shared view keeps a bitset window (SIM-R12) |
| K20 | S5 | T10's measurement at the contract with its host recorded: tiers A and B within 4 GiB |
| K21 | S5 | Histograms with stated error in the versioned report line (SIM-R5), once Q7 decides their home |
| K22 | S6 | The disk's crash model passes strata's faultfs tests unchanged; reads and writes cost O(bytes touched) and syncs O(dirty sectors) (SIM-R4) |
| K23 | S7 | The planner budgets only lost or damaged durable state, f per group (SIM-R2), and the beyond profile reports loss (T14) |
| K24 | Revision | The architect's re-check covers SIM-R1 to SIM-R16 in this revision (C5) |

## Risks

- **Abstraction gaps.** Tier B can miss what its model leaves out (a log-matching bug, a
  snapshot race). T9 checks its timing, not its completeness; Raft's safety stays with tier C
  and T2.
- **Untested drivers.** `reactor` and `Peers` run only on real networks (D7, Q3). The 30-node
  cluster gate and the transport's own tests cover them.
- **Estimates.** Every memory and time figure in D8 is an estimate until T10. If tier B
  exceeds 16 GB at the contract, its per-group state is the first thing to shrink.
- **Gate compute.** T4 alone is about 350 CPU-hours, about 120 hours on the gate VM, which
  the benchmark gates also hold. T10's measurement restates the budgets before T4 runs.
- **Seed rot.** A seed's meaning changes whenever code draws differently; regression cases
  are scenario files (D10), not seeds.
