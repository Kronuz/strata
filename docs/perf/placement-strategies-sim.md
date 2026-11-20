# Placement strategies in simulation: locality against hash, count, affinity and partitioning

Question: how well can strata keep related indexes on the same nodes, as SPAR does ("The Little
Engine(s) That Could", Pujol et al., SIGCOMM 2010), and how does that compare with the other
strategies of `docs/design/placement-strategies.md`? The simulator is `sim/placement/` (pure
Python, standard library only); every number below comes from a `campaign` job at strata
`7d31634b` on the development Mac, light level. The runs are deterministic (seed 1), so the
numbers are exact for that seed, not timings.

Status: results pending; the sweep jobs are queued (journal lists them).

## What is simulated

Each placement **unit** is a path subtree (synthetic workload) or a user (social graphs). A
**request** is routed by an anchor unit and also reads its members: a social user reads every
friend, as in SPAR. Every strategy keeps three durable copies per unit (SPAR with K = 2), so the
comparison is like for like.

| Strategy | Voters | Read copies | Coordinator |
|---|---|---|---|
| hash | 3 random nodes per unit (rendezvous), no domains | none | any copy |
| count | Xapiand today: hashed start, next 2 nodes on the name ring | none | any copy |
| affinity | declared `affinity` and `follow` groups share one copyset | none | any copy |
| partition | METIS-style k-way partition of the co-access graph, part j on copyset j | none | any copy |
| partition+local | as partition | SPAR's post-processing: every leader holds all its members | any copy |
| spar | SPAR's online algorithm as published, K = 2 | master and slaves; slaves are asynchronous | master only |
| adaptive | affinity | the design's adaptive locality: budget, peer bound, hysteresis, hubs | any copy |
| two-level | partition | adaptive locality on the partition | any copy |

Copysets are nodes j, j + N/3 and j + 2N/3, each moved forward to a new rack, so a node is in
about three copysets and voters alone give it about six peers. Racks are contiguous: N/10 of
them (at least 3), so name-adjacent nodes share a rack, as in Xapiand's ring.

Adaptive locality completes requests at the anchor's leader, best rate per missing byte first.
A copy costs its size plus its write rate over a day. Each node may hold `beta` times the mean
voter bytes per node in read copies (beta = 1 unless stated). A new copy must keep both ends
within 64 peers and pulls from a feeder with fewer than 16 children. A unit whose read copies
reach half of the other nodes is promoted to `all` and fed through one shared relay tree.
Hysteresis divides an incumbent copy's cost by 1 + h and multiplies a new one's.

Metrics:

- **copies/unit**: durable plus read copies per unit; **bytes x**: copy bytes over logical bytes.
- **local %**: requests, weighted by rate, that one node answers; **fan-out**: nodes per
  request, mean and p99.
- **moved %/1% churn**: bytes copied when 1% of the nodes (at least one) are replaced, as a
  percentage of the logical data, per 1% of nodes; **x min** divides it by the bytes the
  leaving nodes held, the least any strategy can move.
- **peers**: distinct replication peers per node (voters pairwise, a read copy with its feeder,
  SPAR's master with each slave); the design's bound is 64.
- **spread %**: units whose durable copies span three racks.
- **load max/mean**: coordinator load of the busiest node over the mean.

## Workloads

| Workload | Units | Edges | Source and license |
|---|---|---|---|
| Facebook New Orleans links | 63,731 users | 817,090 | SPAR's own dataset (Viswanath et al., WOSN 2009), `socialnetworks.mpi-sws.org`; no license stated, citation requested. Timestamped links replay in time order. |
| Slashdot Zoo 2009 | 82,168 users | 504,230 undirected | SNAP `soc-Slashdot0902` (Leskovec et al., 2009); no license stated, citation requested |
| Synthetic many-indexes | pending | pending | 100,000 tenants, each a subtree of 1 to 200 small indexes (lognormal, median 64 KiB); 3% split off a write-hot nested root, half of them declared `follow`; tenants in organizations of 1 to 40, 30% of organizations declare `affinity`; 8 hub indexes of 10 to 200 MiB. Requests: 60% the tenant alone, 20% with a hub (Zipf), 12% with up to 4 organization peers, 8% with the nested root. |

## Results

Pending: Facebook, Slashdot and synthetic at 10, 100 and 1,000 nodes; budget and drift sweeps.

## The root index: read copies everywhere, or the nearest member plus a cache

The root (the index of indexes that replaces `.xapiand/indexes`) has 3 to 5 voters. Two ways to
serve its lookups:

1. **All:** a read copy on every node (`copies: all`), fed by a relay tree of fan-out 16, so
   every lookup is local.
2. **Cache:** a per-node cache; a miss asks the nearest root member (a follower read, or a read
   index for read-your-writes).

`sim/placement/root_index.py` models both (job 261120-113051-f5d8a1). Assumptions, all
parameters of the model: an entry is 256 bytes plus a 64-byte header; a new index is looked up
by q = 5 nodes (its copies and coordinators); a warm cache misses m = 0.1 times a second per
node (capacity and refresh); one-way hop 0.25 ms, in-zone round trip 0.5 ms, a relay batches 2
ms; 5 µs of CPU per message at each end; $0.04 per core-hour.

Per index creation, the root's own commit costs V - 1 appends either way. *All* then delivers
N - V more messages; *cache* costs 2q fetches. Per second, *all* sends `r (N - 1)` and *cache*
`r (V - 1 + 2q) + 2 m N`, so the cache sends fewer messages once `N (r - 2m) > r (V + 2q)`.

V = 3 (V = 5 in parentheses):

| N | creates/s | msgs/create all | msgs/create cache | msgs/s all | msgs/s cache | CPU all, cores | CPU cache, cores | new index visible everywhere, all |
|---|---|---|---|---|---|---|---|---|
| 10 | 1 | 9 | 12 (14) | 9 | 14 | 0.0001 | 0.0001 | 2.75 ms |
| 100 | 1 | 99 | 12 (14) | 99 | 32 | 0.0010 | 0.0003 | 5.00 ms |
| 1,000 | 1 | 999 | 12 (14) | 999 | 212 | 0.010 | 0.002 | 7.25 ms (5.00) |
| 10,000 | 1 | 9,999 | 12 (14) | 9,999 | 2,012 | 0.10 | 0.02 | 7.25 ms |
| 10 | 100 | 9 | 12 (14) | 900 | 1,202 | 0.009 | 0.012 | 2.75 ms |
| 100 | 100 | 99 | 12 (14) | 9,900 | 1,220 | 0.099 | 0.012 | 5.00 ms |
| 1,000 | 100 | 999 | 12 (14) | 99,900 | 1,400 | 1.0 | 0.014 | 7.25 ms (5.00) |
| 10,000 | 100 | 9,999 | 12 (14) | 999,900 | 3,200 | 10.0 | 0.03 | 7.25 ms |

Where the cache starts to win on messages: **17 nodes at 1 creation/s and 14 at 100/s** (V = 5:
19 and 16). At 10 nodes *all* is cheaper (9 messages per creation against 12). If warm caches
miss as often as once a second per node, the cache never wins on messages at 1 creation/s, but
both cost under 0.1 core at 10,000 nodes there; at 100/s the crossover stays at 14.

The magnitudes are small until the cluster is large and busy: at 10,000 nodes and 100
creations/s, *all* costs 1.0 million messages a second, 10 cores ($3,500 a year), against 0.03
cores for the cache: 312x fewer messages. At 1 creation/s the same cluster spends 0.1 core on
*all*.

Other axes decide more than messages:

- **Lookup latency.** *All* answers every lookup locally. The cache adds one in-zone round trip
  (0.5 ms) on a miss, which at m = 0.1 per second is about 0.01% of lookups for a node doing
  1,000 a second; a new index costs its first q lookups one round trip each.
- **Staleness.** *All* makes a new index visible everywhere 2.75 to 7.25 ms after its commit;
  a lookup inside that window misses, so *all* still needs the cache's miss path. Cache entries
  can be stale for moved or deleted indexes until used: the target refuses by epoch and the
  lookup retries once (one round trip). Misses read at the closed revision (about 50 ms behind)
  or through a read index (current, one more round trip).
- **Storage.** *All* stores the whole root on every node: 256 MB per node at 1 million
  indexes, 25.6 GB at 100 million. With millions of indexes per host the root outgrows any
  node long before 10,000 nodes; the cache holds each node's working set.
- **Availability.** With *all*, lookups survive the loss of the root's quorum; with the cache,
  only cached entries do.

Recommendation: serve the root through the nearest member and a per-node cache. *All* needs the
same miss path anyway, wins on messages only below about 15 nodes, and cannot hold a root of
hundreds of millions of indexes on every node. `copies: all` stays available as an ordinary
declared placement for a small cluster or a small root; it needs no special case.

## Scripted placement: the scoring seam

`docs/design/placement-strategies.md` parks scripted placement: a Lua scorer that filters and
scores candidate nodes, with the engine enforcing every invariant. `sim/placement/scored.py` is
that seam in the simulator: a strategy is one function `score(unit, node, view)` returning a
score or `None` (filtered out), and the engine walks candidates best first, accepting a node
only if it keeps distinct nodes, a new rack while racks remain, the peer bound and capacity;
when the scorer's candidates cannot, the engine fills from the least-loaded nodes that can and
counts it. Hash (`rendezvous`) and a co-access scorer (`locality`) are written as scorers, and
the tests check that the engine overrides a scorer that prefers one rack. A script is dry-run
by passing it here. Scoring every node per unit costs O(units x nodes), so the seam is for
dry runs at 10 to 100 nodes, not the 1,000-node sweeps.
