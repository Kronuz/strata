# Track D1: the scale contract and a deterministic cluster simulator

PLAN, Track D: D1 is "the scale contract and a deterministic simulator of at least 100,000
nodes and 10^7 shards (time, network, disks, and failures simulated), shared with Track C's
harness", designed with C5 under one owner. Track D's gate per item is the simulator's runs
at the scale contract (no acknowledged loss, no split placement, bounded convergence and
fan-out latency), a real cluster of at least 30 nodes, and the architect's review. Phase 3R's
gate runs 10^7 steps over 1,000 seeds with the linearizability checker. This document
specifies the contract, the simulator that checks it, and the checker C5 shares.

Library commits read for this design: `consensus` `fc66c0e`, `journal` `3dccf50`,
`completion` `5251cf9`, `reactor` `e19afd9`, `task` `bbb2e14`.

## Starting point

Four harnesses exist. Each is a test file with its own network, storage, and clock model:

| Harness | What it runs | Scale | Models |
|---|---|---|---|
| `consensus` `test/simulation_test.cc` (979 lines; T2, C5's first version) | `Core`s with a modeled durable store | 3–7 nodes, one group | Loss, reordering, duplication, partitions, crashes keeping or losing a pending batch, clock rates 0.5×–2× |
| `consensus` `test/liveness_test.cc` (446 lines; T3) | `Core`s in simulated milliseconds | 3–5 nodes, one group | Barrier latency with p99 = b, one-way delay in [r/4, r/2] |
| `consensus` `test/shared_cluster_test.cc` (251 lines) | `Worker`s on `journal`'s turn log over `MemoryIO` | 3 nodes, 3 groups | Drops, reordering, power cuts at a random I/O |
| strata `include/strata/faultfs.hh` (494 lines) | strata's index over an in-memory `IO` | one node | Torn 512-byte sectors, namespace durability, EIO and ENOSPC rules, media faults on reads |

`journal` `include/journal/memory_io.h` is a second in-memory filesystem with the same
sector model. No harness has a linearizability checker: `simulation_test.cc:549` checks that
a read's index covers every commit known at admission, which is a necessary condition, not
linearizability of client histories. None runs more than 7 nodes, and none shares a network,
disk, or clock model with another. D1 replaces the bespoke parts with one engine and keeps
every oracle.

## D1. The scale contract

### Topology and population

| Dimension | Contract | Why |
|---|---|---|
| Nodes | 100,000 | Goal 3 ("hundreds of thousands"); DISTRIBUTION's first row. Scaling runs add 10^4, 3×10^4, and 2×10^5 to fit each cost's exponent in n |
| Failure domains | 5 regions × 2 zones × 250 racks × 40 nodes | Each domain level DISTRIBUTION names is present and has at least 5 members, so `spread: zone` and `spread: region` with 3 voters are satisfiable after one loss at that level |
| Shard groups | 9,968,800 (≈ 10^7), 3 voters each | DISTRIBUTION's row; 300 voter replicas per node on average |
| Dense profile | 10^7 groups on 3×10^4 nodes, 1,000 replicas per node | DISTRIBUTION's per-node row (≥ 1,000 groups per node) |
| Indexes | 10^6: 900,000 of 1 shard, 90,000 of 16, 9,000 of 256, 900 of 4,096, 100 of 16,384 | 9,968,800 shards in total; each tier adds about 1.6× the shards of the one below, a heavy tail, and the top tier exercises the ≥ 10^4 fan-out row |
| Shard bytes | Lognormal, median 1 GiB, σ = 1.5, truncated to [1 MiB, 64 GiB]; mean ≈ 3.1 GiB | Moves and catch-up are byte-bound; about 0.9 TiB of replicas per node at 300 replicas |
| Placement groups | Split at 32,768 shards; about 1,000 at the contract | Mean 10^4 shards each; the largest index (16,384 shards) fits one group; the root holds about 1,000 range entries |
| Distinct peers per node | ≤ 64 | `track-c.md` D12's bound for D3 (copysets); 256 keepalive frames per second per node at rest |

The workload numbers are the simulator's defaults until D3 and D4 state measured ones; each
is a parameter.

### Churn and failures

Rates at 100,000 nodes. The nominal column is the planning rate; the stress column is 10×
it, and every safety row runs at both. Scripted scenarios add the correlated cases.

| Event | Nominal | Stress | Why |
|---|---|---|---|
| Node restart (crash or upgrade), back within 15 min | 1 per node per 30 days: 139 per hour | 1,389 per hour | A monthly rolling upgrade plus crashes. Ford et al. (OSDI 2010) find most node unavailability shorter than 15 minutes, which is why replacement waits |
| Permanent node loss (disk or host) | 4% per node-year: 11 per day | 110 per day | The upper end of the 2–4% annual disk replacement rates Schroeder and Gibson (FAST 2007) report; Pinheiro et al. (FAST 2007) report 1.7% to 8.6% by drive age |
| Rack power loss | 1 per 1,000 racks per day: 2.5 per day | 25 per day | Correlated failure, the case copysets bound (Cidon et al., USENIX ATC 2013) |
| Zone outage | Scripted: 1 zone (10,000 nodes) for 1 h | 2 zones in different regions | `spread: zone` must keep quorum; dissemination is bandwidth-bound (below) |
| Network partition | Scripted: a rack, a zone, a region, asymmetric and one-way | Random cuts at 10× | Partitions that leave each side with some voters |
| Gray failure | 1 node in 1,000 at any time with 10× disk latency or 5% loss | 1 in 100 | Huang et al. (HotOS 2017); Gunawi et al. (FAST 2018) |
| Media faults | 1 latent sector error per 10 TB read; 1 silent bit flip per 10^14 bytes read | 100× | Bairavasundaram et al. (SIGMETRICS 2007); REPLICATION's corruption defense |
| Index creation and deletion | 11.6 per second each (10^6 indexes living 1 day) | 116 per second, and bursts of 1,000 per second for 60 s | Xapiand's index per path makes indexes cheap and numerous |
| Rolling upgrade | The whole cluster in 24 h, at most one rack per zone at once | In 6 h | D8; leaders move by transfer (Track C, D6), nothing re-replicates |

### Bounds

Every row holds on every seed. A violation of a safety row is a failure whatever the cause.

| Bound | Target | Why |
|---|---|---|
| **No acknowledged loss** | 0 writes acknowledged with `ack=quorum` or `ack=all` lost: each stays visible to every later read until a later write replaces it, with at most f of a group's 2f + 1 voters failed at once | REPLICATION's guarantee. Runs beyond the model (D6) must show loss, which proves the oracle sees it |
| **No split placement** | 0 instants at which two placement groups both hold a range at epochs either considers current; 0 shards with two committed voter sets in one configuration epoch; 0 replicas serving a shard its placement group has no record of, after 60 s | DISTRIBUTION: one authority per range, never one global leader |
| **Acknowledged prefix on a quorum** | After every membership change, a quorum of the new voters holds the acknowledged prefix durably | Catches a placement that removes a voter before its replacement caught up |
| **Failure domains** | 0 groups with two voters in one domain they are spread over, except where no placement satisfies the constraint, each such case reported | DISTRIBUTION's failure-domain row |
| **Peer bound** | Distinct peers per node ≤ 64 at every instant; a move that would pass it waits | `track-c.md` D12 |
| **Failure detection** | A crashed node first suspected within 5 s at p99 | SWIM (Das et al., DSN 2002) with a 1 s period: a member goes unprobed for k periods with probability about e^−k, so k = 4.6 at p99 |
| **Single-failure dissemination** | 99.9% of live nodes know a node is dead within 45 s at p99 | 5 s to suspicion, a suspicion timeout that grows with log n (Lifeguard, Dadgar et al., 2017; 20 s at 10^5 with 4 periods per decade of n), and about 20 periods of infection-style dissemination (log2 10^5 = 16.6) |
| **Zone-outage dissemination** | 99.9% of live nodes know all 10,000 deaths within 300 s | Bandwidth-bound: about 40 updates of 32 bytes ride a 1,400-byte datagram, a node hears about 2 messages per second, so 10^4 updates need at least 125 s to reach a node by piggybacking alone. D2 may add full-state exchange to shorten it; 300 s stays under the 15-minute replacement hold either way |
| **False suspicion** | At most 10^−4 live nodes declared dead per node-hour at 1% datagram loss; 0 at 0% | Lifeguard's motivation: a false death moves replicas |
| **Membership cost** | At most 4 membership messages sent per node per second at rest; the exponent of messages per node in n at most 0.15 over 10^4…2×10^5 | DISTRIBUTION: nothing linear in n on a hot path. Log factors over a 20× range give an exponent near 0.09 |
| **Group failover** | After a zone outage, every group that kept a quorum commits a new entry within 5 s at p99, 10 s worst | `track-c.md` T3's rows: a quiescent group's leader node failing yields a leader within 4 s; 10^6 groups elect at once, about 30 per surviving node |
| **Placement decisions** | Replacements for a lost node's replicas committed within 10 s of its declared loss; a rack's (12,000 replicas) within 60 s | Decisions are small consensus entries in about 300 placement groups at once |
| **Re-replication** | A lost node's replicas back to full replication within 15 min at p99 after the hold | 0.9 TiB from ≤ 64 peers into ≤ 64 targets at `track-c.md` D13's 4 transfers per node and T7's 50 MB/s each: 12.8 GB/s, about 77 s, before contention |
| **Zone re-replication** | No data moves during the hold (1 h for a zone); afterwards moves stay within each node's 4 + 4 transfers and each placement group's move budget | 3×10^6 replicas, about 9 PiB, cannot move at once and should not |
| **Authority load** | The root commits at most 1 entry per second at nominal churn; a placement group at most 10 | The root changes only on placement-group splits and moves |
| **Fan-out latency** | A query to 16,384 shards completes in 75 ms at p99 under a 100 ms deadline, with at most 0.1% of shards missing in the p99 query; aggregators merge at most 64 children | Fan-in 64 gives depth 3 (64^2 = 4,096 < 16,384 ≤ 64^3). Without hedging, the slowest of 16,384 shards sits past every shard's p99.99 (Dean and Barroso, CACM 2013); hedging after the shard p95 bounds it. Per-shard times default to p50 2 ms, p99 20 ms until D5 measures strata's |
| **Cluster state per node** | Membership ≤ 32 bytes per cluster member (3.2 MB at 10^5); placement ≤ 1 KiB per hosted replica; routing caches bounded | DISTRIBUTION's memory row, stated as a function of n |

The membership, placement, and fan-out rows bind D2, D3, and D5; the simulator measures them
and the owning design may revise a target with its reason. The safety rows are fixed.

## D2. Determinism

One run is a pure function of its seed, its parameters, and its scenario file.

- **One thread, one queue.** Events run in order of `(time, sequence)`, where `sequence` is
  the order of scheduling. Equal times never depend on heap order or addresses.
- **Splittable random streams.** The seed feeds splitmix64 (Steele et al., OOPSLA 2014);
  every component draws from its own stream, derived by mixing its parent's key with a
  label (`node/17/disk`, `faults/partition`). Each stream is xoshiro256** (Blackman and
  Vigna, ACM TOMS 2021). A component that draws one more value shifts only its own stream,
  which keeps shrinking stable (D10).
- **Integer distributions.** Bounded integers use Lemire's method (ACM TOMACS 2019).
  Probabilities are parts per million. Latencies are drawn from quantile tables in integer
  nanoseconds with linear interpolation. `std::uniform_int_distribution` and the other
  standard distributions are banned: their outputs differ between libstdc++ and libc++.
- **Banned in code the simulator runs:** `std::random_device`, the `random` library's
  per-thread generators, `steady_clock` and `system_clock`, threads, iteration over
  unordered containers whose order reaches an output, ordering by pointer, and libm
  transcendental functions in decisions. Floating-point `+ − × ÷ √` are allowed: IEEE 754
  rounds them correctly, and the simulator builds with `-ffp-contract=off`.
- **The trace hash.** Every event folds its time, kind, node, and a digest of its payload
  into a 64-bit running hash, and every 65,536 events the hash is logged. Two runs of one
  seed must agree; when they do not, the logged hashes bisect to the first event that
  differs.
- **Across platforms.** The engine and tiers A and B give the same trace hash on Linux,
  macOS, and FreeBSD, x86_64 and arm64. Tier C with strata's scoring needs only the same
  hash per platform, since BM25 calls `log`.

## D3. Time and clocks

- Simulated time is `uint64` nanoseconds from 0. Nothing waits in real time.
- Each node has a clock `local(t) = offset + t × rate_ppm / 10^6`, computed in 128 bits and
  monotone. Rates range over 0.5×–2× (T2's range) for safety runs and ±200 ppm for latency
  runs. Clock steps model NTP corrections where code reads a wall clock (none should).
- Production code receives time only as an input: `consensus` `Tick{now, election_delay}`
  (`types.h:176`, handled at `core.h:433`), strata's `executor::now()` (`executor.hh:40`),
  `task::inline_scheduler::now()` (`task.hh:122`). Election delays come from the node's
  stream.

## D4. Network

- **Topology.** A node id packs region, zone, rack, and position. One-way latency comes from
  a quantile table per distance class: same rack (p50 50 µs), same zone (100 µs), other zone
  (500 µs), other region (30 ms), each with a p99 of 4× its p50 by default.
- **Connections.** The peer transport is TCP (DISTRIBUTION, "Layers"), so frames on one
  connection and lane arrive in order or not at all. A connection reset drops what is in
  flight, and the sender reconnects after a backoff. Reordering exists only across
  connections and lanes, as on real TCP. Bulk frames are paced by a per-link bandwidth
  (10 Gb/s default), so snapshot transfers take time proportional to bytes.
- **Datagrams.** If D2 chooses a datagram path for probes, datagrams are lost, duplicated,
  and reordered independently, per link class.
- **Faults.** A partition colors nodes and cuts links between colors; asymmetric and one-way
  cuts are link sets. Clogging stops delivery on a link for a time and then releases the
  backlog (FoundationDB's simulation, Zhou et al., SIGMOD 2021, injects the same). Gray
  links add loss or latency to one node's links.
- **Cost.** A partition is a byte per node (its color) plus a set of cut links. In-flight
  messages are the only per-message state.

## D5. Disks

- **One in-memory filesystem.** strata's `faultfs` already models what power loss keeps
  (torn 512-byte sectors, namespace changes durable only after a directory sync), EIO and
  ENOSPC rules, short writes, failed syncs that drop unsynced bytes (the Linux behavior
  Rebello et al., USENIX ATC 2020, document), and media faults on reads. It becomes the
  simulator's disk. `journal`'s `MemoryIO` duplicates its sector model and retires once
  `journal`'s tests can fetch the simulator (Q1); until then both stay.
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
- **Power loss and disk loss.** Power loss crashes the node and applies `faultfs`'s crash
  rules. Disk loss replaces the disk with an empty one; the node rejoins as a new
  incarnation (Track C, D5).
- **Beyond the model.** A profile where `fsync` returns before persisting (REPLICATION lists
  it outside the guarantee) runs only in the oracle self-check (D6).

## D6. Faults and schedules

- **Within the model.** Faults are budgeted so that the guarantee's premise holds: at most f
  of a group's 2f + 1 voters are down, partitioned away, or have damaged storage at once.
  TigerBeetle's simulator documentation describes the same rule for storage faults: inject
  them where the cluster can still recover, so that a failure is a bug and not expected
  loss. The planner checks each fault against every group the fault touches, which at full
  scale means the groups with a replica on the node, rack, or zone.
- **Beyond the model.** A separate profile breaks the premise on purpose (f + 1 voters lost,
  lying barriers). The loss oracle must report loss there; a run that reports none fails,
  since its oracle is blind.
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
  assertions do. A gate row names each counter's minimum.

## D7. Production code inside the simulator

The rule: decisions live in sans-I/O state machines that take events and time and return
actions; drivers (sockets, timers, threads, files) do no deciding. The simulator replaces the
drivers and runs the state machines unchanged. FoundationDB's simulation replaces its network
and file interfaces the same way (Zhou et al., SIGMOD 2021).

| Library | Seam | Status | Use |
|---|---|---|---|
| `consensus` `Core` | `step(event)` returns actions; time from `Tick`; election delays from the host (`core.h:433-435`) | Exists | Authority groups at every tier; shard groups at tier C |
| `consensus` `Worker` | `run_one(Tick)` (`worker.h:447`), `take_io_operation()` (`:211`), the turn-driver constructor (`:111-117`) | Exists | Shard groups at tier C |
| `consensus` transport codec | `encode`/`decode` are pure (`transport/codec.h:18`, `:117`) | Exists | Every simulated frame is encoded and decoded, so the wire format runs |
| `consensus` transport `Peers` | Asio executor, TLS, and `steady_clock` read inline (`transport/peers.h:171`, `:190`, `:380`; `session.h:123`) | No seam | The simulator delivers decoded messages to the Worker itself, so `Peers`' lane order, pipeline window, and credit do not run. Q3 |
| `journal` | `TurnDriver` (`turn_driver.h:64-90`); `Store` over `IO` | Exists | Tier C |
| `journal` identities | `detail::random_identity()` reads `std::random_device` (`primitives.h:33`), called for artifact and mutation identities and temporary names (`artifact.h:51`, `:136`; `mutation.h:40`; `journal.h:461`) | **Must be added** | Temporary names and stored identities differ between runs of one seed, so tier C's trace hash is not reproducible. Q4 |
| `completion` | `IO`, `File`, `DirectoryCursor` interfaces (`io.h:73`, `:101`, `:116`); `IOOperation` (`operation.h:63`) | Exists | The simulator's disk and completion queue implement them; no change to `completion` |
| `reactor` | `io_context` threads, `steady_clock` at accept (`reactor.h:287`, `:326`) | None needed | The simulator never runs `reactor`. It stays a driver, tested on real nodes |
| strata | `executor` (`now()`, `sleep_until`, `executor.hh:40-42`) and `inline_executor` (`:51`); `faultfs` | Exists | Tier C with strata's core |
| `task` | `inline_scheduler` (`task.hh:118-143`) | Exists | Coroutine hosts |
| `cluster` | `liveness.h:59` and `raft.h:144` read `steady_clock` and `std::random_device` | Not carried forward | DISTRIBUTION retires both |
| D2 membership, D3 placement, D5 coordinator | Not yet written | **Requirement** | Each is a sans-I/O state machine with time as input; D2's member view is a container type parameter whose merge is a join (commutative, associative, idempotent); no per-node state linear in n except that view (D8) |

The two additions to libraries Detent links (Q3, Q4) are additive, defaulted to today's
behavior, and coordinated with the Detent work first (PLAN, Track C). Nothing else in
`consensus`, `journal`, `completion`, or `reactor` changes.

## D8. Fidelity tiers

Tiers are cumulative. A scenario names its tier and runs unchanged at any tier that has the
components it touches.

- **Tier A, membership and authority at full scale.** D2's membership code on every node;
  the root and every placement group as real `consensus` `Core` groups with a modeled store
  (T2's `Durable` model), since there are only about 1,000 of them; D3's placement code as
  their state machine. Shard groups exist only as placement records. 10^5 nodes.
- **Tier B, shard groups as abstract state machines at full scale.** Tier A plus every
  shard group as a simulator-owned model: members and roles, leader, term, commit index, each
  member's durable and received index, quiescence, and timers. Elections, commit latency,
  and catch-up draw from distributions calibrated on tier C (T9). A crash drops each member's
  received-but-not-durable suffix; leader choice follows Raft's rule (the most up-to-date
  durable log in a reachable quorum). The model does not test Raft; it tests placement,
  membership changes, and failure handling against Raft's contract, at 10^7 groups. A batch
  is one event chain, not one per write. 10^5 nodes and 10^7 groups.
- **Tier C, full groups at a stated smaller scale.** Tier A plus every shard group as a real
  `Worker` on the node's `journal` turn log (D11 of `track-c.md`) over the simulator's disk,
  with a register state machine; tier C+ applies batches to strata's core through
  `strata::group` once Phase 3R builds it. 300 nodes × 100 replicas before D17's memory
  work, 1,000 nodes × 100 replicas after (estimates below).

Memory and time per tier. **All figures are estimates** from the structures' sizes; T10
replaces them with measurements and the hardware they ran on.

| Tier | Per node | Per group | Total at the contract | Events per simulated second | Wall time per simulated second |
|---|---|---|---|---|---|
| A | About 5 KiB: SWIM state 256 B, a piggyback queue capped at 512 entries (4 KiB), suspicion timers, in-flight datagrams; the member view shared (below) | Placement record about 48 B, held by each of 3 authority replicas | About 2 GB: 0.5 GB nodes, 1.4 GB placement maps (10^7 × 48 B × 3), authority cores | About 6 per node: 6×10^5 | About 0.2 s at 0.3 µs per event |
| B | As A | Plus about 128 B of abstract state: 1.3 GB | About 3.5 GB | Plus 8 per batch: 8×10^5 at 10^4 active groups × 10 batches/s; idle groups cost none | About 0.4 s; a zone outage adds about 2×10^7 events, 6 s |
| C, before D17 | 100 replicas × (133 KiB idle `Worker` heap, `track-c.md` "Starting point", + 8 KiB of checkpoint files) + a 4 MiB turn log: about 18 MiB | | 300 nodes: about 5.3 GB | About 60 per idle group before D12's quiescence (heartbeats every 100 ms): 6×10^5 at 10^4 groups | About 1.2 s at 2 µs per event (core step, codec) |
| C, after D17 and D12 | 100 × (8 KiB + 8 KiB) + 4 MiB: about 5.6 MiB | | 1,000 nodes: about 5.6 GB | Keepalives only at rest: 2P/k = 256 frames per node per second | About 0.3 s at rest |

The member view is the one structure that would be quadratic: a full view at every node is
10^5 × 10^5 entries, 160 GB at 16 bytes. Tier A therefore runs D2's code over a view
container that shares state across nodes: one table of every update ever made about each
member, each node holding a cursor into it and the few updates it received out of order.
A node's view of a member is the join of the table's entries up to its cursor and its own
extras, which is exact because the merge is a join (D7's requirement on D2). Production keeps
a dense array. The two containers implement one concept, and a test runs D2's suite over
both. Probe order needs no per-node list: a keyed Feistel permutation over member slots with
cycle-walking (Black and Rogaway, CT-RSA 2002) gives each node its own round-robin order in
O(1) state, which D2 may adopt in production too.

The tiers run on one host: tiers A and B in about 4 GB per process, so the Linux gate VM
(16 GB, 8 vCPUs) runs 3 seeds at once; tier C at 1,000 nodes in about 6 GB. Under the host
rules each process is one job.

## D9. The linearizability checker (shared with C5)

- **What it checks.** Client histories: each operation's invocation and response times in
  simulated nanoseconds, its arguments, and its result, or "unknown" when the client timed
  out. The model is a versioned register per document key: `put(key, value, expected
  version?)`, `delete(key, expected version?)`, `get(key)`. That covers Phase 3R's "write and
  get-by-id histories" and REPLICATION's versioned writes.
- **Algorithm.** Wing and Gong's search (JPDC 1993) with Lowe's memoization of (linearized
  set, model state) (CCPE 2017), split by key under P-compositionality (Horn and Kroening,
  FORTE 2015): a history is linearizable if and only if each key's sub-history is. Porcupine
  (its documentation) and Knossos (Jepsen's documentation) take the same approach.
- **Unknown outcomes** may take effect at any point after their invocation or never, as
  Jepsen's indeterminate operations do.
- **Long histories.** Each key's history splits at quiescent cuts (no operation pending), and
  the checker carries the set of model states reachable at each cut. A cut with more than
  1,024 reachable states, or a search past 10^7 nodes, reports "inconclusive" with the key,
  never "pass". Checking is NP-complete in general (Gibbons and Korach, SIAM J. Comput.
  1997), so generators keep at most 8 operations pending per key and pause each key's
  clients now and then.
- **Scale.** Tier C checks every key. Tier B has no values, so its oracles are the counters
  in D1's bounds; it checks linearizability on 10^4 sampled keys whose writes it tracks by
  version.
- **Shared.** The checker is an offline function over a history file, with no dependency on
  `consensus` or strata. C5's harness (`simulation_test.cc` and its successors) records client
  histories through it, and the 30-node real cluster records the same format, as Jepsen does
  for real systems, so one checker covers both.
- **Placement histories.** Root and placement-group operations (assign a range, split, move a
  replica) are checked by the no-split-placement oracle at every event, not by search.

## D10. Shrinking a failing seed

A failing seed reproduces exactly. Shrinking makes it small enough to read:

1. **Cut the length.** The failure is detected at event k; the run is cut to end at k.
2. **Shrink the fault schedule.** Delta debugging (Zeller and Hildebrandt, IEEE TSE 2002)
   over the schedule's entries: remove halves, then quarters, down to single entries,
   keeping each removal that still fails. Then move entries earlier and merge partitions.
   Latencies and workload come from their own streams (D2), so removing a fault does not
   reshuffle them.
3. **Shrink the workload.** Halve clients, keys, groups, and nodes while the failure holds;
   generators map smaller draws to simpler choices, the property Hypothesis's reducer relies
   on (MacIver and Donaldson, ECOOP 2020).
4. **Emit a scenario.** The result is a scenario file: seed, parameters, and the explicit
   schedule, versioned, under `test/scenarios/`, and replayed as a test. A scenario keeps
   reproducing after unrelated code changes add draws, which a bare seed does not.

Each shrinking attempt is one full run, so shrinking costs runs, not memory. T11 bounds it.

## D11. Where the code lives

Three homes were considered:

| Home | For | Against |
|---|---|---|
| strata | strata owns D1 | `consensus` and `journal` cannot fetch strata for their tests (C5): strata depends on them |
| `consensus` | C5's harness sits beside the code it tests | Network, disk, membership, and placement models are not consensus; `LIBRARY_BOUNDARIES.md` split `cluster` because it grew four libraries in one, and this would start the same in a library Detent links |
| **A new library, `Kronuz/simulation`** | One engine for every consumer; depends only on `completion`'s `io.h` | One more repository; adapters for `consensus` need a home |

**Recommendation: the new library** (Q1). The engine, network, disk (`faultfs`, moved), fault
planner, checker, and shrinker go there, depending only on `completion`. The `consensus`
adapter (a group host driving `Core` and `Worker` on the simulator) is an optional target in
`consensus` (`consensus::simulation`, off by default), so Detent's build is unchanged. strata
fetches both. No cycle: `simulation` → `completion`; `consensus` tests → `simulation`.

Until the owner decides, the code starts on this branch as `sim/`, a self-contained CMake
project in namespace `kronuz::simulation` with no strata include, so that `git filter-repo
--subdirectory-filter sim` turns it into the library with its history if Q1 goes that way.

## D12. Runs and reports

- `sim_run --tier a|b|c --profile nominal|stress|beyond --seeds FIRST-LAST --hours H
  [--scenario FILE]` runs seeds, one per process, and writes one JSON line per seed: trace
  hash, every bound's measured value, every sometimes counter, events, peak RSS, wall time,
  and the host.
- `sim_shrink --seed S [...]` shrinks a failure into a scenario file.
- `sim_check HISTORY` runs the linearizability checker on a history file.
- Metrics are fixed histograms (`metrics` library), never unbounded sample lists.

## Prior art

From published papers and documentation only; no code was read or copied.

- **FoundationDB** (Zhou et al., SIGMOD 2021): the whole database runs in one deterministic
  process against simulated network and disks, with fault injection points in its own code
  and clogged connections. D1 takes the single-process, interface-replacement model.
  FoundationDB writes its code against its own actor framework so all of it runs simulated;
  strata cannot run `reactor` that way, so D7 moves decisions out of drivers instead.
- **TigerBeetle's VOPR** (TigerBeetle documentation): a whole cluster in one process from a
  seed, with storage faults kept within what the cluster can recover from, and separate
  safety and liveness checking. D6 takes the fault budget and the heal-then-progress phase.
- **Jepsen, Knossos, Elle, Porcupine:** black-box testing of real clusters with generated
  faults and an offline checker. Knossos and Porcupine check linearizability (Porcupine with
  P-compositionality); Elle (Kingsbury and Alvaro, VLDB 2020) checks transactional isolation
  and is the reference if strata adds multi-document transactions. D9 takes the offline
  checker and indeterminate operations; the real-cluster gate runs Jepsen-style.
- **Antithesis** (its documentation): unmodified software in a deterministic hypervisor, with
  "always" and "sometimes" properties. D6's sometimes counters follow it; a hypervisor would
  also cover `reactor` and `Peers`, which D7 does not.
- **Membership:** SWIM (Das et al., DSN 2002), Lifeguard (Dadgar et al., 2017), HyParView
  (Leitão et al., DSN 2007, partial views), Rapid (Suresh et al., USENIX ATC 2018). They
  set D1's membership bounds and are D2's to choose among.

## Scale (of the simulator itself)

At the contract the simulator holds about 4 GB per tier-B process and runs about 2.5×
faster than real time at nominal load (estimates). Its cost per node is constant except for
the shared member view (O(updates)), and per group constant; nothing it keeps per node grows
with n. A 2×10^5-node run (T10) checks that the estimate scales linearly.

## Gate rows

Each row runs from `sim/` (or the library, after Q1) at the gate's commit. "Seeds" are
consecutive from 1 unless stated. Every row's JSON lines are kept under `docs/gates/`.

| Row | Target | Procedure |
|---|---|---|
| T1 Determinism | Two runs of each seed give the same trace hash; a pinned hash per tier for seeds 1–10 matches on Linux x86_64 and arm64, macOS x86_64 and arm64, and FreeBSD | `sim_run --seeds 1-1000` twice, comparing hashes; a CTest with the pinned hashes in CI on every platform |
| T2 Coverage | Every fault class, and every sometimes counter, reached at least 100 times over the T4 budget; each counter's count recorded | The counters in T4's JSON lines |
| T3 Checker | Accepts every history of a corpus of linearizable histories and rejects every one of a corpus of non-linearizable ones (stale reads, lost writes, reordered versions, an unknown write seen then unseen), each built by hand and by mutating a correct register; reports inconclusive past its bounds, never pass | `sim/test/checker_test.cc` |
| T4 Safety at full scale | Every safety bound of D1 holds over 200 seeds × 1 simulated hour each at nominal and at stress, tier B, 10^5 nodes and 10^7 groups, plus each scripted scenario (zone outage, rack loss during a rolling upgrade, region partition, two zones in two regions, a placement-group split during a node loss) over 50 seeds | `sim_run --tier b --profile nominal --seeds 1-200 --hours 1`, then `--profile stress`, then `--scenario` for each file in `sim/scenarios/` |
| T5 Membership | D1's detection, dissemination, false-suspicion, and cost rows at 10^5 nodes; the cost exponent fitted over n = 10^4, 3×10^4, 10^5, 2×10^5 | `sim_run --tier a --profile nominal --seeds 1-50 --hours 1 --nodes N` for each N |
| T6 Placement | D1's decision, re-replication, zone, and authority-load rows | T4's runs; the zone scenario |
| T7 Fan-out | D1's fan-out row over 10^4 queries to the 16,384-shard indexes, with 1 node in 1,000 gray | `sim_run --tier b --scenario fanout.scn --seeds 1-20` |
| T8 Full fidelity | Phase 3R's rows at tier C: at least 10^7 events over 1,000 seeds each for 3 and 5 voters, with crashes, power loss at every barrier, partitions, clock skew, membership changes, and corruption of every file class; the checker passes on every history; zero acknowledged loss; zero served corruption | `sim_run --tier c --voters 3 --seeds 1-1000`, then `--voters 5` |
| T9 Tier conformance | Tier B's calibrated quantities (election time, commit latency, catch-up time, unavailability per group after each scripted fault) within ±10% of tier C's at p50 and p99, over at least 1,000 groups × 20 seeds of the same scenarios at 300 nodes | `sim_run --tier b` and `--tier c` with `--nodes 300` and the same scenario files; `sim/tools/conform.py` compares |
| T10 Simulator cost | Peak RSS and wall time per simulated second for each tier at its stated scale, and at 2×10^5 nodes for tiers A and B; measured RSS within 2× the D8 estimate, else D8 is revised with the measurement | `/usr/bin/time -v` (Linux) or `-l` (macOS) around T4's and T8's runs |
| T11 Shrinking | Each seeded mutant's first failing seed shrinks to a scenario of at most 10 fault entries within 2 CPU-hours; the scenario fails with the mutant and passes without it | `sim_shrink` on T12's mutants |
| T12 Mutants | Each seeded fault detected within the T4 or T8 budget, with seeds to detection recorded: acknowledging before a quorum is durable; a deposed leader serving a read without a probe; one range assigned to two placement groups; removing a voter before its replacement caught up; two voters placed in one zone; a peer-bound check off; a member declared dead without suspicion; ignoring an fsync failure | `sim/tools/mutants.sh`, as `consensus` `tools/mutants.sh` does |
| T13 Every gate | ASan and UBSan over every simulator test (TSan too, though the engine is single-threaded); one CPU-hour of fuzzing for the scenario-file and history-file parsers; coverage at TESTING's targets | CI, `tools/coverage.sh` |

## Open questions for the owner

1. **Q1. Where the code lives.** Options: a new `Kronuz/simulation` library with an optional
   adapter target in `consensus`; `consensus`; strata. Recommendation: the new library
   (D11). Creating it is a public repository, so it waits for the owner; the code starts
   in `sim/` here either way.
2. **Q2. The member view (with D2).** Options: (a) full views with a join merge and a view
   container type, which the simulator shares (D8); (b) partial views (HyParView-style,
   O(log n) per node), which need no sharing but give placement groups no full view of
   candidate nodes. Recommendation: (a). It decides whether tier A at 10^5 nodes is about
   2 GB or not feasible on one host; D2 decides with it.
3. **Q3. `Peers` in the simulator.** Options: (a) extract `Peers`' send policy (control
   frames first, the pipeline window, credit) into a sans-I/O class in `consensus` that
   both `Peers` and the simulator drive, an additive change coordinated with Detent;
   (b) leave it untested in simulation and cover it with real-network tests only.
   Recommendation: (a), done with C4's D16, which replaces that policy anyway.
4. **Q4. The identity source in `journal`.** Options: (a) a virtual
   `IO::new_identity()` defaulting to `random_device`, overridden by the simulator's disk;
   (b) an identity generator passed to `Store` and `TurnDriver`. Recommendation: (a): one
   additive virtual, Detent's `PosixIO` unchanged. Until it lands, tier C runs are
   reproducible in outcome but their trace hashes are not, so T1 excludes tier C.
5. **Q5. Failure rates.** Options: the published rates in D1 with 10× stress, or the owner's
   fleet data if there is any. Recommendation: D1's rates; replace them with fleet data
   when it exists.
6. **Q6. What tier B's results prove.** Options: tier B's safety rows count for Track D's
   gates on placement and membership logic only, with Raft's safety from tier C and T2; or
   Track D's gates require tier C runs at their scale too. Recommendation: the first, with
   T9's conformance as its condition.

## First steps

S1 and S3 depend on no open question and no library change; they start on this branch now.
S2 and S4 follow. S5 waits for D2 and D3's first state machines, and tier C's journals wait
for Q4.

- **S1. The deterministic engine** (`sim/include/simulation/`):
  - `random.hh`: `splitmix64`, `xoshiro256ss`, `stream` (derived from a parent key and a
    label), `below(n)` (Lemire), `chance_ppm(p)`, `quantile_table` (integer nanoseconds,
    interpolated).
  - `clock.hh`: `node_clock{offset, rate_ppm}` with `local(t)`, monotone, 128-bit math.
  - `engine.hh`: `engine` with `now()`, `at(t, event)`, `after(d, event)`, `cancel(id)`,
    `run_until(t)`, `run_steps(n)`; a 4-ary heap of 24-byte `(time, sequence, slot)` entries,
    event bodies in a slab, cancellation by generation; the trace hash and its checkpoints.
  - Tests: two runs of one seed agree; a pinned hash for a fixed scenario; ties ordered by
    scheduling; cancellation; `below(n)` unbiased (a 16-bit instantiation checked
    exhaustively over every input for n up to 257, chi-square for the 64-bit one); streams independent of sibling
    draws; clock monotonicity at every rate.
- **S2. The network** (`network.hh`): topology ids, distance classes, latency tables, FIFO
  connections with resets, datagrams, partitions by color, cut links, clogging, bandwidth
  pacing. Tests: FIFO per connection, reordering only across connections, partitions
  symmetric and one-way, bytes over bandwidth.
- **S3. The checker** (`checker.hh`): history records, the versioned-register model, WGL with
  Lowe's cache, the per-key split, quiescent cuts, unknown outcomes, inconclusive bounds.
  Tests: T3's corpora.
- **S4. Tier C0: `consensus` `Core` groups on the engine** (`sim/consensus/`, fetching
  `consensus` at `fc66c0e`): T2's modeled store and oracles ported onto the engine and
  network, a register state machine, clients recording histories, the checker over them.
  Its acceptance: T2's oracles and `simulation_test.cc`'s mutants detected as `tools/mutants.sh`
  reports today, plus the checker passing.
- **S5. Tier B's abstract groups** at 10^5 nodes and 10^7 groups over a reference placement
  (a copyset ring with the peer bound, until D3's state machine exists): the memory
  measurement of T10 first, then T4's oracles.

## Risks

- **Abstraction gaps.** Tier B can miss what its model leaves out (a log-matching bug, a
  snapshot race). T9 checks its timing, not its completeness; Raft's safety stays with tier C
  and T2.
- **Untested drivers.** `reactor` and `Peers` run only on real networks (D7, Q3). The 30-node
  cluster gate and the transport's own tests cover them.
- **Estimates.** Every memory and time figure in D8 is an estimate until T10. If tier B
  exceeds 16 GB at the contract, its per-group state is the first thing to shrink.
- **Seed rot.** A seed's meaning changes whenever code draws differently; regression cases
  are scenario files (D10), not seeds.
