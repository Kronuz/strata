# Placement strategies: per-index replication and locality

Status: design, for Track D3. The owner's original Xapiand vision: each index (a path such
as `/a/b`) chooses how widely it is replicated, whether across the whole cluster, across a
third of it, or by another rule. Indexes can be associated so they are available together
in the same places, and copies move as access patterns change. The inspiration is SPAR
(Pujol et al., "The Little Engine(s) That Could: Scaling Online Social Networks",
SIGCOMM 2010).

## What Xapiand has today

`src/index_resolver_lru.cc`, own code:

- Per index, `_settings.number_of_shards` (fixed at creation) and
  `_settings.number_of_replicas` (changeable live). Command-line defaults are 5 and 1.
- `calculate_shards`: `jump_consistent_hash(path, total_nodes)` chooses a start
  position in the node list, sorted by name, and shard `s` goes to position
  `start - s`.
- `settle_replicas`: a shard's replicas are the next nodes after its primary in that
  sorted list. A count larger than the cluster is capped at the cluster size. When the
  count shrinks, the vector's tail is dropped.
- The resulting shard map is saved in the index's settings, so existing indexes keep
  their placement. `update_primary` moves the primary when a node changes.

So the only strategy is "N copies on name-adjacent nodes". It has three gaps:

1. It has no failure domains: name-adjacent nodes can share a rack.
2. There is no "everywhere" or fractional rule. The cap at cluster size makes `all`
   work only by accident, and only at creation time.
3. A node whose name sorts early shifts the positions of every later node, so new
   indexes created after a join land differently from the same index created before.
   Saved maps keep this from moving existing data.

Name adjacency does have one good property, kept below: each node's peers are bounded by
its neighbors in the ring, as copysets require.

## What SPAR does

SPAR treats each user as a vertex and each friendship as an edge.

- **Local semantics.** For each user's master, a master or replica of every neighbor is
  kept on the same server, so a read is answered locally.
- **Redundancy.** It also keeps K replicas.
- **Objective.** Minimize the total number of replicas, subject to an equal number of
  masters per server. This problem, MIN_REPLICA, is NP-hard.

The online heuristic reacts to six events: adding or removing a node, an edge, or a server.

- **On a new edge,** it compares three options: the status quo, moving u's master to
  v's server, and moving v's master to u's server.
- **It then picks** the option with the fewest replicas that keeps load balanced.
- **Locality replicas double as redundancy replicas,** so local semantics cost little
  beyond K.

## Problems in the paper

These matter for a search engine:

1. **Asynchronous redundancy is not durability.** The Replication Manager propagates
   writes to replicas with eventual consistency. A master that fails after
   acknowledging a write loses it, even with K replicas. The paper explicitly leaves
   movement consistency to "semantic reconciliation based on versioning". strata keeps
   quorum voters for durability and treats locality copies as a separate kind.
2. **Locality replicas serve no reads.** Reads go only to masters, so the replicas add
   cost without read capacity, and load balance counts only masters.
3. **Unit costs.** Every replica counts as 1. Indexes differ in size by orders of
   magnitude, and a copy of a write-hot index costs replication traffic for as long as
   it exists. The objective must weigh bytes and write rate, not replica count.
4. **A centralized partition manager.** The prototype handles 52 edge events per
   second, and no design is given for partitioning it. strata has no global placement
   leader (DISTRIBUTION.md).
5. **No failure domains.** Neither its K replicas nor its promotion choice on failure
   consider racks or zones.
6. **Peers are unbounded.** Locality replicas add distinct peers per node without
   limit, but strata bounds a node's peers at 64 (Track D3, `track-c.md` D12), since
   connections and keepalives scale with peers.
7. **Hubs.** Footnote 1 assumes the follow limits of social networks bound hubs. An
   index that is queried together with everything has no such bound. Under SPAR it
   simply grows toward full replication, a state that should be declared as "all",
   not accumulated one edge at a time.
8. **Minor issues.** Constraint 3 (exactly equal masters per server) is infeasible
   unless the count divides evenly, and Lemma 1's reduction is omitted. Edges never
   decay, there is no hysteresis besides the load ratio, and moves have no rate limit.

The idea holds: copies kept for redundancy can be placed where they also give locality,
and doing so is far cheaper than random placement. The mechanism needs replacing.

## Newer work it builds on

- **Two-level placement.** Social Hash (Facebook, NSDI 2017) separates two problems:
  1. Affinity: items go to many small buckets by graph partitioning, slowly.
  2. Load: buckets go to servers, quickly and independently.

  Akkio (OSDI 2018) moves small units ("µ-shards") toward where they are accessed.
- **Load-aware moves with a churn budget.** Slicer (Google, OSDI 2016) does this.
- **Constraint-based placement.** Shard Manager (Facebook, SOSP 2021) uses spread,
  affinity, and capacity constraints, solved by local search.
- **Declarative replication.** CockroachDB zone configurations give replicas per table
  and per index:
  - voters versus non-voting replicas, which serve follower reads;
  - constraints and lease preferences;
  - `GLOBAL` tables.

  Elasticsearch's `auto_expand_replicas: 0-all` and its allocation awareness are the
  simple form of the same idea.
- **Copysets** (Cidon et al., 2013) bound the distinct replica sets, which lowers the
  chance that correlated failures lose data. This is strata's peer bound.

## Design

### Two kinds of copies

| Copy | Consensus role | Purpose | Count |
|---|---|---|---|
| Voter | Voter or witness (Track C) | Durability and leadership | 3 or 5, spread over failure domains |
| Read copy | Learner, or a snapshot copy when frozen | Read capacity and locality | 0 to all nodes |

A strategy never changes the quorum. "Everywhere" means a voter set of three plus read
copies on every other node.

- **Reads.** Read copies serve reads at a stated freshness: follower reads behind a
  closed revision, or a read index for read-your-writes.
- **Distribution.** Leaders do not stream to thousands of learners. Read copies pull
  from a relay tree of other copies, which matches Xapiand's original pull-based
  replication, with fan-out bounded per relay.
- **Frozen indexes** (`frozen-index.md`) distribute one content-addressed snapshot and
  have no log at all, which makes them the natural case for "everywhere".

### Strategies

Each index declares a placement in `_settings`, and child paths inherit it:

```json
"_settings": {
  "number_of_shards": 3,
  "placement": {
    "voters": 3,
    "spread": "zone",
    "copies": "all" | 4 | {"fraction": 0.33},
    "constraints": ["+region=us-east", "-class=spot"],
    "affinity": "social",
    "follow": "/a/b"
  }
}
```

1. **Count** (Xapiand's `number_of_replicas`): a fixed number of read copies. It maps
   directly: `number_of_replicas: n` is `voters: min(n + 1, 3)` plus the remaining
   copies.
2. **Fraction:** ceil(f × eligible nodes). When membership changes, the target is
   recomputed with hysteresis, so one node joining does not move copies.
3. **All:** every eligible node, as Elasticsearch's `0-all` does.
4. **Constraints:** label selectors on the node's zone, rack, class, or any attribute it
   advertises. These limit where voters and copies may go.
5. **Follow:** co-partitioning. Shard i of this index is placed with shard i of the
   followed index. If the shard counts differ, the whole index is placed wherever the
   followed index has a copy. This suits a small dimension index read with a large one.
6. **Affinity group:** indexes with the same `affinity` are placed as one unit, with the
   same node sets per shard number, and move together.
7. **Adaptive locality:** SPAR's idea, corrected. Read copies are added or removed from
   observed co-access, under a budget (below).

Strategies 1–6 are declarative, and the placement group computes them directly.
Strategy 7 is an optimizer on top that only adds or removes read copies. It never moves
voters, so a bad decision costs efficiency, never durability.

### Authority

A placement group owns a range of indexes (DISTRIBUTION.md). An affinity group must be
decided in one place, so the placement range key starts with the affinity group's name,
then the path. Members of one group fall in one range, and one placement group decides
them together.

Adaptive edges between ranges are requests. The coordinator that observes a co-access
asks the target index's placement group for a read copy on a node. That group grants or
declines it under its own budget. No global partition manager exists.

### Adaptive locality

- **Graph.** The vertices are (index, shard) units. An edge's weight is the decayed rate
  at which two units appear in the same request: multi-index searches, routed requests,
  and joins.
- **Benefit.** A copy of unit u on node n is worth the co-access rate times the remote
  fan-out cost saved (latency and CPU).
- **Cost.** The copy costs u's bytes, plus u's write rate times its replication bytes,
  plus one new peer if n does not already peer with u's group.
- **Decisions.** Add a copy when benefit exceeds cost times (1 + h), and remove one when
  benefit falls below cost / (1 + h).
- **Budgets.** Each node has a budget of locality bytes (a fraction of its disk), and
  each placement group a move budget per minute, as in Slicer.
- **Hubs.** A unit whose copies pass a fraction of the cluster is promoted to the
  "fraction" or "all" strategy and stops being tracked edge by edge.

Two-level placement keeps this stable. Affinity groups and co-access choose buckets,
which change slowly. Load balancing assigns buckets to nodes, which changes quickly and
respects the copyset bound. Voters keep their failure-domain spread throughout.

### Moves

A move uses Track C's membership changes only:

1. Add a learner.
2. Let it catch up from a snapshot, then the log.
3. Promote it if it is to become a voter.
4. Remove the old copy.

The single-step rule (D3) and the `leaving` marker keep each step safe. Leadership moves
by transfer (D6), so a move toward locality can also move the leader to the node where
writes originate.

## Feasibility

- **Count, fraction, all, constraints, follow, affinity groups: high.** All are
  placement-group computations on top of learners, membership changes, and leadership
  transfer, which Track C provides. "All" at 100,000 nodes depends on relay trees and on
  quiescence; frozen indexes make it cheap.
- **Adaptive locality: moderate.** It needs co-access telemetry from coordinators, the
  two-level partitioner, and the budgets above. Its value depends on the workload having
  multi-index requests. Fan-out across one index's shards gains nothing from it, since
  every search touches every shard.

## Testing

- **D1's simulator:** placement at 100,000 nodes for every strategy, checking failure
  domains, the peer bound of 64, and the move budget, with membership churn.
- **SPAR's own comparison, reproduced on co-access graphs:** replication overhead and
  moves for adaptive locality against random and against hash placement. Costs are
  weighted by bytes and write rate rather than counted as units.
- **A real cluster:** multi-index query latency and bytes moved with and without
  locality copies.

## Where it fits

It belongs to Track D3, with the API surface in D4. It depends on:

- C1: learners, single-step changes, and transfer.
- C2: quiescent groups.
- D1's simulator.
- P20, for frozen indexes distributed everywhere.
