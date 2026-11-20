# Placement strategies in simulation: locality against hash, count, affinity and partitioning

Question: how well can strata keep related indexes on the same nodes, as SPAR does ("The Little
Engine(s) That Could", Pujol et al., SIGCOMM 2010), and how does that compare with the other
strategies of `docs/design/placement-strategies.md`? The simulator is `sim/placement/` (pure
Python, standard library only); every number below comes from a `campaign` job at strata
`7d31634b` on the development Mac, light level. The runs are deterministic (seed 1), so the
numbers are exact for that seed, not timings.

## Finding

1. **Two-level placement is the mechanism to build.** It puts voters on copysets chosen by an
   offline partition of the co-access graph and adds read copies under a budget. On the
   synthetic many-indexes workload at 1,000 nodes, it serves 100% of requests on one node with
   3.07 copies per unit (4.12x the logical bytes, hubs included), at most 28 peers per node,
   and churn moves only what the leaving nodes held. SPAR also reaches 100%, but needs 4.54
   copies (4.99x bytes), up to 999 peers per node, and moves 1.29x that minimum.
2. **Social graphs are the hard case, and the peer bound is what binds.** Full locality on
   Facebook at 1,000 nodes costs 14.3 copies per unit with partition+local and 14.6 with SPAR,
   at 855 to 909 peers per node. Under the 64-peer bound, two-level reaches 47.9% local at 3.74
   copies and cuts the mean fan-out from 22.4 to 8.9 nodes (Slashdot: 71.2% local, fan-out 10.9
   to 5.9).
3. **SPAR's balance rule is ambiguous, and both readings lose.** Read literally, masters pile
   up: on Facebook at 1,000 nodes the busiest node carries 41x the mean load, and replacing 1%
   of the nodes moves 93% of the data. Read strictly, it is balanced but needs as many copies as
   offline partitioning with SPAR's post-processing (14.56 against 14.30).
4. **Voters alone give little locality, and today's ring breaks rack spread.** Hash, count and
   affinity leave 0.1% of Facebook requests local at 1,000 nodes. Count (Xapiand's name ring)
   spreads no unit's voters over three racks. Hash reaches 385 to 549 peers per node.
5. **Serve the root index through the nearest member and a cache.** It sends fewer messages
   than a read copy on every node from 14 to 19 nodes on, and at 10,000 nodes and 100
   creations a second it costs 0.03 cores against 10 (own section below).

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
| spar | SPAR's online algorithm, K = 2; a master moves only to a partition with fewer masters | master and slaves; slaves are asynchronous | master only |
| spar-literal | as spar, with §4.2's exception read literally (see below) | as spar | master only |
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
| Synthetic many-indexes | 102,976 units from 956,026 indexes | 299,006 | 100,000 tenants, each a subtree of 1 to 200 small indexes (lognormal, median 64 KiB); 3% split off a write-hot nested root, half of them declared `follow`; tenants in organizations of 1 to 40, 30% of organizations declare `affinity`; 8 hub indexes of 10 to 200 MiB. Requests: 60% the tenant alone, 20% with a hub (Zipf), 12% with up to 4 organization peers, 8% with the nested root. |

## Results

Every table holds beta = 1. The x min column divides the bytes moved by the bytes the leaving
nodes held, the least any strategy can move.

Facebook, N = 10 (3 racks; churn replaces 1 node; jobs 261120-113016-34489c):

| strategy | copies/unit | bytes x | local % | fan-out | p99 | moved %/1% churn | x min | peers max | spread % | load max/mean |
|---|---|---|---|---|---|---|---|---|---|---|
| hash | 3.00 | 3.00 | 11.5 | 3.50 | 7 | 5.99 | 2.00 | 9 | 30 | 1.01 |
| count | 3.00 | 3.00 | 8.5 | 3.00 | 4 | 3.02 | 1.00 | 4 | 0 | 1.00 |
| affinity | 3.00 | 3.00 | 8.3 | 2.71 | 4 | 2.92 | 1.00 | 4 | 100 | 1.86 |
| partition | 3.00 | 3.00 | 37.4 | 1.83 | 3 | 4.12 | 1.00 | 4 | 100 | 1.51 |
| partition+local | 4.82 | 4.82 | 100.0 | 1.00 | 1 | 5.91 | 1.00 | 9 | 100 | 1.00 |
| spar | 4.67 | 4.67 | 100.0 | 1.00 | 1 | 9.71 | 2.06 | 9 | 52 | 1.02 |
| spar-literal | 3.54 | 3.54 | 100.0 | 1.00 | 1 | 11.32 | 2.98 | 9 | 43 | 2.38 |
| adaptive | 7.50 | 7.50 | 100.0 | 1.00 | 1 | 7.20 | 1.00 | 9 | 100 | 1.00 |
| two-level | 5.23 | 5.23 | 100.0 | 1.00 | 1 | 6.07 | 1.00 | 9 | 100 | 1.00 |

Facebook, N = 100 (10 racks; churn replaces 1 node; jobs 261120-113016-34489c):

| strategy | copies/unit | bytes x | local % | fan-out | p99 | moved %/1% churn | x min | peers max | spread % | load max/mean |
|---|---|---|---|---|---|---|---|---|---|---|
| hash | 3.00 | 3.00 | 1.2 | 11.44 | 40 | 5.99 | 2.08 | 99 | 74 | 1.02 |
| count | 3.00 | 3.00 | 0.6 | 11.89 | 36 | 2.86 | 1.00 | 4 | 0 | 1.02 |
| affinity | 3.00 | 3.00 | 0.6 | 11.89 | 36 | 3.00 | 1.00 | 4 | 100 | 1.02 |
| partition | 3.00 | 3.00 | 19.7 | 5.93 | 21 | 3.03 | 1.00 | 4 | 100 | 1.03 |
| partition+local | 9.18 | 9.18 | 100.0 | 1.00 | 1 | 8.15 | 1.00 | 99 | 100 | 1.06 |
| spar | 9.68 | 9.68 | 100.0 | 1.00 | 1 | 21.80 | 1.58 | 99 | 91 | 1.12 |
| spar-literal | 5.12 | 5.12 | 100.0 | 1.00 | 1 | 13.93 | 3.93 | 99 | 87 | 8.29 |
| adaptive | 6.01 | 6.01 | 59.3 | 5.59 | 26 | 6.01 | 1.00 | 67 | 100 | 1.06 |
| two-level | 6.01 | 6.01 | 84.0 | 1.84 | 10 | 6.06 | 1.00 | 69 | 100 | 1.15 |

Facebook, N = 1,000 (100 racks; churn replaces 10 nodes; jobs 261120-113018-d8cecc, 261120-113022-e0eb42, 261120-113025-27329d):

| strategy | copies/unit | bytes x | local % | fan-out | p99 | moved %/1% churn | x min | peers max | spread % | load max/mean |
|---|---|---|---|---|---|---|---|---|---|---|
| hash | 3.00 | 3.00 | 0.1 | 21.08 | 112 | 5.78 | 2.02 | 385 | 97 | 1.15 |
| count | 3.00 | 3.00 | 0.1 | 22.39 | 124 | 3.08 | 1.00 | 4 | 0 | 1.20 |
| affinity | 3.00 | 3.00 | 0.1 | 22.43 | 124 | 2.99 | 1.00 | 4 | 100 | 1.20 |
| partition | 3.00 | 3.00 | 17.0 | 11.57 | 66 | 3.01 | 1.00 | 4 | 100 | 1.50 |
| partition+local | 14.30 | 14.30 | 100.0 | 1.00 | 1 | 12.19 | 1.00 | 909 | 100 | 1.45 |
| spar | 14.56 | 14.56 | 100.0 | 1.00 | 1 | 16.30 | 1.18 | 855 | 98 | 1.70 |
| spar-literal | 7.43 | 7.43 | 100.0 | 1.00 | 1 | 92.76 | 2.66 | 986 | 99 | 41.00 |
| adaptive | 3.56 | 3.56 | 28.6 | 19.73 | 111 | 3.54 | 1.00 | 64 | 100 | 1.35 |
| two-level | 3.74 | 3.74 | 47.9 | 8.89 | 52 | 3.85 | 1.00 | 64 | 100 | 1.80 |

Slashdot, N = 10 (3 racks; churn replaces 1 node; jobs 261120-113028-7a1b5f):

| strategy | copies/unit | bytes x | local % | fan-out | p99 | moved %/1% churn | x min | peers max | spread % | load max/mean |
|---|---|---|---|---|---|---|---|---|---|---|
| hash | 3.00 | 3.00 | 29.1 | 2.41 | 7 | 5.99 | 2.00 | 9 | 30 | 1.00 |
| count | 3.00 | 3.00 | 20.8 | 2.32 | 4 | 3.02 | 1.00 | 4 | 0 | 1.00 |
| affinity | 3.00 | 3.00 | 20.5 | 2.21 | 4 | 2.93 | 1.00 | 4 | 100 | 1.10 |
| partition | 3.00 | 3.00 | 48.5 | 1.80 | 4 | 4.12 | 1.00 | 4 | 100 | 1.49 |
| partition+local | 4.73 | 4.73 | 100.0 | 1.00 | 1 | 5.78 | 1.00 | 9 | 100 | 1.00 |
| spar | 3.89 | 3.89 | 100.0 | 1.00 | 1 | 5.38 | 1.18 | 9 | 43 | 1.03 |
| spar-literal | 3.42 | 3.42 | 100.0 | 1.00 | 1 | 4.95 | 1.50 | 9 | 37 | 1.99 |
| adaptive | 5.87 | 5.87 | 100.0 | 1.00 | 1 | 5.54 | 1.00 | 9 | 100 | 1.00 |
| two-level | 5.04 | 5.04 | 100.0 | 1.00 | 1 | 5.91 | 1.00 | 9 | 100 | 1.00 |

Slashdot, N = 100 (10 racks; churn replaces 1 node; jobs 261120-113028-7a1b5f):

| strategy | copies/unit | bytes x | local % | fan-out | p99 | moved %/1% churn | x min | peers max | spread % | load max/mean |
|---|---|---|---|---|---|---|---|---|---|---|
| hash | 3.00 | 3.00 | 3.1 | 6.22 | 40 | 5.96 | 2.06 | 99 | 74 | 1.01 |
| count | 3.00 | 3.00 | 1.9 | 6.44 | 35 | 2.86 | 1.00 | 4 | 0 | 1.01 |
| affinity | 3.00 | 3.00 | 1.7 | 6.45 | 36 | 3.00 | 1.00 | 4 | 100 | 1.02 |
| partition | 3.00 | 3.00 | 41.4 | 4.96 | 34 | 3.09 | 1.00 | 4 | 100 | 1.02 |
| partition+local | 9.00 | 9.00 | 100.0 | 1.00 | 1 | 8.29 | 1.00 | 99 | 100 | 1.02 |
| spar | 7.58 | 7.58 | 100.0 | 1.00 | 1 | 11.68 | 0.81 | 99 | 84 | 1.17 |
| spar-literal | 4.74 | 4.74 | 100.0 | 1.00 | 1 | 6.74 | 1.73 | 99 | 78 | 10.85 |
| adaptive | 6.66 | 6.66 | 91.9 | 1.89 | 20 | 6.64 | 1.00 | 99 | 100 | 1.03 |
| two-level | 6.93 | 6.93 | 97.5 | 1.33 | 15 | 7.07 | 1.00 | 99 | 100 | 1.03 |

Slashdot, N = 1,000 (100 racks; churn replaces 10 nodes; jobs 261120-113031-f76ac7, 261120-113034-79ca70, 261120-113036-5cc371):

| strategy | copies/unit | bytes x | local % | fan-out | p99 | moved %/1% churn | x min | peers max | spread % | load max/mean |
|---|---|---|---|---|---|---|---|---|---|---|
| hash | 3.00 | 3.00 | 0.3 | 10.33 | 115 | 5.84 | 2.04 | 463 | 97 | 1.10 |
| count | 3.00 | 3.00 | 0.1 | 10.85 | 126 | 3.06 | 1.00 | 4 | 0 | 1.25 |
| affinity | 3.00 | 3.00 | 0.2 | 10.85 | 127 | 3.01 | 1.00 | 4 | 100 | 1.20 |
| partition | 3.00 | 3.00 | 37.6 | 8.66 | 114 | 2.95 | 1.00 | 4 | 100 | 1.20 |
| partition+local | 11.93 | 11.93 | 100.0 | 1.00 | 1 | 11.17 | 1.00 | 997 | 100 | 1.30 |
| spar | 10.88 | 10.88 | 100.0 | 1.00 | 1 | 14.07 | 1.20 | 986 | 96 | 1.70 |
| spar-literal | 6.58 | 6.58 | 100.0 | 1.00 | 1 | 8.77 | 1.56 | 999 | 96 | 46.80 |
| adaptive | 3.52 | 3.52 | 42.4 | 8.16 | 93 | 3.50 | 1.00 | 64 | 100 | 1.30 |
| two-level | 3.57 | 3.57 | 71.2 | 5.92 | 79 | 3.55 | 1.00 | 64 | 100 | 1.30 |

Synthetic, N = 10 (3 racks; churn replaces 1 node; jobs 261120-113039-a24e16):

| strategy | copies/unit | bytes x | local % | fan-out | p99 | moved %/1% churn | x min | peers max | spread % | load max/mean |
|---|---|---|---|---|---|---|---|---|---|---|
| hash | 3.00 | 3.00 | 85.5 | 1.16 | 3 | 5.96 | 2.01 | 9 | 30 | 1.01 |
| count | 3.00 | 3.00 | 79.1 | 1.24 | 3 | 3.07 | 1.00 | 4 | 0 | 1.01 |
| affinity | 3.00 | 3.00 | 81.3 | 1.21 | 3 | 2.91 | 1.00 | 4 | 100 | 1.01 |
| partition | 3.00 | 3.00 | 93.0 | 1.07 | 2 | 4.12 | 1.00 | 4 | 100 | 1.57 |
| partition+local | 3.04 | 3.05 | 100.0 | 1.00 | 1 | 4.14 | 1.00 | 9 | 100 | 1.41 |
| spar | 3.69 | 3.42 | 100.0 | 1.00 | 1 | 5.16 | 1.33 | 9 | 44 | 1.14 |
| spar-literal | 3.09 | 3.06 | 100.0 | 1.00 | 1 | 5.90 | 1.68 | 9 | 30 | 1.15 |
| adaptive | 4.21 | 3.88 | 100.0 | 1.00 | 1 | 3.62 | 1.00 | 9 | 100 | 1.01 |
| two-level | 3.04 | 3.05 | 100.0 | 1.00 | 1 | 4.14 | 1.00 | 9 | 100 | 1.41 |

Synthetic, N = 100 (10 racks; churn replaces 1 node; jobs 261120-113039-a24e16):

| strategy | copies/unit | bytes x | local % | fan-out | p99 | moved %/1% churn | x min | peers max | spread % | load max/mean |
|---|---|---|---|---|---|---|---|---|---|---|
| hash | 3.00 | 3.00 | 70.1 | 1.46 | 5 | 5.52 | 2.09 | 99 | 74 | 1.54 |
| count | 3.00 | 3.00 | 68.7 | 1.50 | 5 | 2.85 | 1.00 | 4 | 0 | 1.63 |
| affinity | 3.00 | 3.00 | 71.7 | 1.42 | 5 | 2.96 | 1.00 | 4 | 100 | 1.49 |
| partition | 3.00 | 3.00 | 81.3 | 1.19 | 2 | 2.95 | 1.00 | 4 | 100 | 2.12 |
| partition+local | 3.02 | 3.13 | 100.0 | 1.00 | 1 | 3.07 | 1.00 | 48 | 100 | 2.06 |
| spar | 4.36 | 3.91 | 100.0 | 1.00 | 1 | 4.03 | 1.17 | 99 | 82 | 1.79 |
| spar-literal | 3.12 | 3.18 | 100.0 | 1.00 | 1 | 5.21 | 2.12 | 99 | 69 | 1.70 |
| adaptive | 4.72 | 4.33 | 100.0 | 1.00 | 1 | 4.47 | 1.00 | 69 | 100 | 1.53 |
| two-level | 3.02 | 3.13 | 100.0 | 1.00 | 1 | 3.07 | 1.00 | 45 | 100 | 2.05 |

Synthetic, N = 1,000 (100 racks; churn replaces 10 nodes; jobs 261120-113041-eb2b90, 261120-113043-241386, 261120-113045-1109aa):

| strategy | copies/unit | bytes x | local % | fan-out | p99 | moved %/1% churn | x min | peers max | spread % | load max/mean |
|---|---|---|---|---|---|---|---|---|---|---|
| hash | 3.00 | 3.00 | 67.8 | 1.54 | 5 | 5.36 | 1.97 | 549 | 97 | 7.78 |
| count | 3.00 | 3.00 | 67.7 | 1.54 | 5 | 2.81 | 1.00 | 4 | 0 | 7.74 |
| affinity | 3.00 | 3.00 | 70.6 | 1.45 | 5 | 3.04 | 1.00 | 4 | 100 | 7.68 |
| partition | 3.00 | 3.00 | 79.1 | 1.21 | 2 | 2.87 | 1.00 | 4 | 100 | 7.78 |
| partition+local | 3.07 | 4.04 | 100.0 | 1.00 | 1 | 3.82 | 1.00 | 73 | 100 | 7.23 |
| spar | 4.54 | 4.99 | 100.0 | 1.00 | 1 | 5.85 | 1.29 | 999 | 94 | 9.17 |
| spar-literal | 3.22 | 4.21 | 100.0 | 1.00 | 1 | 7.73 | 1.83 | 999 | 90 | 10.73 |
| adaptive | 3.52 | 4.33 | 97.6 | 1.07 | 4 | 4.38 | 1.00 | 88 | 100 | 7.68 |
| two-level | 3.07 | 4.12 | 100.0 | 1.00 | 1 | 3.97 | 1.00 | 28 | 100 | 7.23 |

### What the tables show

- **Subtree units do most of the work on our workload.** Folding 956,026 indexes into 102,976
  units makes 60% of requests single-unit, so even hash serves 67.8% locally at 1,000 nodes.
  Declared affinity covers only 30% of organizations and adds 2.8 points (70.6%). Partition
  reaches 79.1% with no extra copies, and two-level reaches 100%.
- **Copysets cost nothing on the axes the design bounds.** Affinity and partition hold peers at
  4, spread every unit over three racks, and churn moves exactly what the leaving nodes held
  (1.00x). Hash moves 2.0x that minimum. Count spreads 0% of units over three racks, because
  name-adjacent nodes share a rack.
- **Adaptive copies need a partitioned base.** On the synthetic workload at 100 nodes, adaptive
  over affinity uses 4.72 copies where two-level uses 3.02, for the same 100% locality. On
  Facebook at 100 nodes, both use 6.01 copies, but adaptive reaches 59.3% local against
  two-level's 84.0%.
- **Load.** On the synthetic workload at 1,000 nodes, every strategy has a busiest coordinator
  at 7.2x to 10.7x the mean. That comes from single hot tenants (lognormal request rates, sigma
  1.5), not from placement: it is a read-capacity problem (`fraction` copies for hot units),
  not a locality one.

### Budget and hysteresis (jobs 261120-113047-03a766, 261120-113049-c6cae1; N = 100)

| Workload | strategy | beta 0.25 | beta 1 | beta 4 |
|---|---|---|---|---|
| Facebook | adaptive | 33.6% local, 3.75 copies | 59.3%, 6.01 | 93.4%, 16.58 |
| Facebook | two-level | 54.8%, 3.75 | 84.0%, 6.01 | 100%, 9.23 |
| Synthetic | adaptive | 99.8%, 4.46 | 100%, 4.72 | 100%, 4.72 |
| Synthetic | two-level | 100%, 3.02 | 100%, 3.02 | 100%, 3.02 |

On the synthetic workload the budget never binds. On Facebook, two-level at beta 4 equals
partition+local (9.23 against 9.18 copies): the budget is the knob between the two.

Hysteresis, measured after redrawing 10% of the request rates (beta 0.25):

- **Synthetic:** h = 0.25 cuts the read-copy bytes added from 1.00% to 0.67% of the data, 33%
  fewer, at an unchanged 99.8% locality.
- **Facebook:** h = 0.25 cuts them from 8.13% to 4.56%, 44% fewer, and locality moves from
  32.4% to 32.8%.

### Two defects the simulation found in the design's rules

- **Hub promotion by node fraction misfires in small clusters.** At 10 nodes, "read copies on
  half of the other nodes" means 4 copies, so 25,452 of 63,731 Facebook users are promoted to
  `all`, and adaptive pays 7.50 copies per unit. At 100 and 1,000 nodes on the synthetic
  workload, the rule promotes exactly the 8 hubs (7 under two-level at 1,000), which is its
  purpose. Promotion should also require a share of the request rate, or an absolute floor of
  copies.
- **One shared relay tree breaks the peer bound at its root.** The simulator feeds every
  promoted unit's tree from node 0, so node 0 peers with every hub leader: 99 peers on Slashdot
  at 100 nodes, 88 on the synthetic workload at 1,000. Feeding each tree at its leader's own
  position, or from a fixed per-rack relay, keeps that node within 64. The simulator leaves
  this as is; the peers max column shows it.

## What SPAR gets right, and where the design differs

SPAR's idea holds: copies kept for redundancy can be placed where they also give locality, and
that is far cheaper than placing them blindly. Its online heuristic beats offline partitioning
with post-processing when balance is loose: on Facebook at 100 nodes, spar-literal needs 5.12
copies per unit where partition+local needs 9.18. Its locality copies double as redundancy, so
at 10 nodes it needs 3.54 copies against 4.82.

The published mechanism fails on five counts:

1. **Balance.** §4.2 lets a master move to a fuller partition when the savings over the second
   best option exceed "the current ratio of load imbalance between partitions". Read that way
   (spar-literal), the master coefficient of variation is 0.47, 1.14 and 1.92 on Facebook at
   10, 100 and 1,000 nodes, against the paper's 0.0019. The busiest node then carries 41x the
   mean load, and replacing 1% of the nodes moves 92.8% of the data: the 10 leaving nodes
   held 35% of it (4.7x their share), and repairing them moved 2.66x that. Allowing moves only toward fewer masters (spar) restores balance
   (coefficient of variation at most 0.007), but then needs 4.67, 9.68 and 14.56 copies. That
   is no better than partition+local's 4.82, 9.18 and 14.30. The paper's results must lie
   between the two readings.
2. **Voters move for locality.** Durability follows the access pattern. SPAR's durable copies
   span three racks for only 52% of units at 10 nodes and 91% at 100. The design keeps voters
   on copysets (100%) and changes only read copies, so a bad locality decision costs
   efficiency, never durability.
3. **Unbounded peers.** SPAR reaches 855 to 999 peers per node at 1,000 nodes, against 64
   under the design's bound. That bound is what limits social-graph locality to 47.9% to 71.2%.
4. **Unit cost.** SPAR counts replicas, not bytes. On the synthetic workload at 1,000 nodes,
   SPAR stores 4.99x the logical bytes and two-level 4.12x. Most of the gap is copies of large
   hubs and write-hot nested roots, which the design's cost function avoids.
5. **Reads only at masters.** SPAR's copies add no read capacity. The design routes a request
   to any copy, which spreads load across equal copies.

The design keeps SPAR's objective (locality from copies) and replaces the mechanism. Voters
stay fixed, read copies are budgeted, the peer bound holds, and partitioning runs offline and
slowly while placement balances quickly. That is the two-level row, the best in every table
that respects the bounds.

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

## Recommendation for the placement after the first release

Build in this order:

1. **Copyset voters with rack spread, declarative strategies on top.** Use subtrees as units;
   `count`, `fraction` and `all` as read copies; `affinity` and `follow` as shared copysets.
   This fixes count's 0% rack spread and hash's 385 to 549 peers, and keeps churn at the
   minimum (1.00x). Feasibility is high: the design already rates it so.
2. **Two-level locality.** A periodic offline partition of the observed co-access graph picks
   each unit's copyset, and read copies are added under a byte budget with hysteresis (h =
   0.25). The partition costs 5 to 22 seconds in this Python code for 64,000 to 103,000 units
   into 1,000 parts, which is affordable as a background job. Fix hub promotion first (a share
   of the request rate, not only of nodes), and feed relay trees at their leaders.
3. **The root index through a cache** (previous section).

Not to build: SPAR's online master moves (balance or copies, never both, and unbounded peers);
adaptive read copies over hashed or declared voters alone (more copies for less locality than
two-level).

Not modeled: sharded units (every unit has one shard), co-access that changes beyond one 10%
redraw, the relay tree's bandwidth, and latency by topology. A real-cluster check of
multi-index query latency remains the design's third test.
