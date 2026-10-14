# strata distribution

strata is distributed-first. Every component is designed for clusters of hundreds of
thousands of nodes, and states what it does at that scale before it is built. This
document holds the scale contract, the layers, what carries forward from Xapiand and
what does not, and the open decisions. [REPLICATION.md](REPLICATION.md) covers one
replication group; this document covers the cluster of groups. [PLAN.md](PLAN.md)'s
Track D builds it.

## Scale contract

| Dimension | Target |
|---|---|
| Nodes per cluster | ≥ 100,000 |
| Shards (replication groups) per cluster | ≥ 10^7 |
| Groups per node | ≥ 1,000, with fsyncs per second per node within budget (Track C2) |
| Indexes (namespaces) per cluster | ≥ 10^6 |
| Shards one query fans out to | ≥ 10^4, with partial results under a deadline |
| Membership change detected and disseminated | O(log n) gossip rounds; no node-wide broadcast |
| Placement decisions | Made by the group owning the index's range, never by one global leader |
| Failure domains | Node, rack, zone, region; replicas never share a domain they are spread over |
| Cluster-state memory per node | Stated as a function of cluster size; membership state bounded per peer, placement state only for the ranges a node serves |

Per node, the rows the core is judged against:

| Dimension | Target |
|---|---|
| Indexes (shards) per node | ≥ 1,000, most of them idle |
| Memory per idle index | Stated, and bounded by its open segments' pinned structures |
| Open files and mappings per node | Within Linux's default `vm.max_map_count` (65,530) at the index target |
| Commit barriers per node per second | Stated; group commit across indexes |
| Flush, merge, and reclamation work per node | Budgeted across indexes, never per index unbounded |

Every component's design states its cost in these terms: messages per node per
second, state per node, and round trips per operation, each as a function of cluster
size. Anything linear in the number of nodes on a hot path is a defect.

## Layers

```
 clients          HTTP API, query DSL, bulk ingest
 coordinators     routing, fan-out trees, merging, global statistics   (stateless)
 ─────────────────────────────────────────────────────────────────────────────────
 root authority   small strict-consensus group: placement-group map only
 placement groups each owns a range of indexes: shard map, replica placement
 shard groups     strata::group per shard: consensus + journal + core
 ─────────────────────────────────────────────────────────────────────────────────
 membership       SWIM-style gossip: liveness and suspicion, never decisions
 transport        Detent's peer transport (TCP, TLS before production)
```

- **Membership** answers who is alive. It never decides placement or leadership.
  SWIM's probing and piggybacked dissemination keep per-node load constant as the
  cluster grows.
- **Authority** is hierarchical. The root group maps index ranges to placement
  groups and changes rarely. Placement groups split when they grow, like any other
  range. No single leader sees every shard.
- **Shard groups** replicate each shard with quorum durability (REPLICATION.md).
  Heartbeats coalesce per node pair, and idle groups go quiescent (Track C2). 10^3
  groups per node cost about what one does at rest on two conditions: the groups are
  quiescent, and placement bounds the distinct peers per node (at most 64, Track D3),
  since node-pair keepalives and connections scale with those peers
  (`docs/design/track-c.md`, D12).
- **Coordinators** hold no durable state. Any node can coordinate, and dedicated
  coordinator tiers can be added for fan-out.

Where each layer's code lives:

| Layer | Home |
|---|---|
| Transport | `consensus`'s peer transport (the Detent stack) |
| Membership | A membership library of its own, decided in "Open decisions" |
| Root authority and placement groups | A strata target beside `strata::group`, `strata::placement`, on `consensus` |
| Shard groups | `strata::group` |
| Coordinators, query fan-out, merging | A strata target, `strata::coordinator`; Xapiand hosts it |
| HTTP API, schema, routing | Xapiand |

The core (`strata` itself) has no network, consensus, schema, or geometry code.

## Query

- A shard returns a mergeable result: statistics, top-k with sort keys, aggregation
  partials, and estimate triples. Merging is associative, so fan-out forms a tree:
  each tier merges its children and forwards one result.
- Global statistics for exact scoring are cached per index with bounded staleness.
  An exact mode gathers them first, at one extra round trip.
- Threshold sharing (PLAN P10) sends the running global k-th score down the tree, so
  shards prune against the global threshold rather than their local one.
- Each request carries a deadline. Shards that miss it are reported, and the result
  is marked partial. Requests to slow replicas are hedged.

## Storage tiers

- **Hot:** segments on local NVMe, served by the reactor (ARCHITECTURE.md, "I/O").
- **Cold:** immutable segments on object storage, opened by stateless searchers
  through `residency` with a verified block cache (Quickwit's model). An index can
  move between tiers without reindexing.

## From Xapiand

Xapiand's design is a starting point, kept where it holds at the target scale.

| Xapiand | Decision | Reason |
|---|---|---|
| An index per path, sharded and replicated | Kept, as namespaces mapped to shard groups (Track D4) | It is the user-facing model |
| UDP multicast discovery | Replaced by SWIM-style gossip over the peer transport (D2) | Multicast is unavailable or limited across racks, zones, and clouds, and its traffic grows with the cluster |
| UDP Raft (`cluster::Raft`), volatile | Replaced by `consensus` from the Detent stack, durable, with membership changes (Track C) | Membership and placement need a durable, strict authority |
| Per-shard `ELECT_PRIMARY` | Replaced by group leadership | One source of truth per shard |
| Asynchronous pull replication after commit | Replaced by quorum-acknowledged journals | No acknowledged write may be lost |
| Remote protocol (GPL) | Replaced by the shard RPC | License, and mergeable partial results |
| Haystack-style volumes in the node | Open (below) | |

## Open decisions

1. **The blob store.** Xapiand keeps whole documents in Haystack-style append-only
   volumes in each node. At the target scale, the options are:
   - Keep it in the node, beside the shard: one binary, and a document fetch never
     leaves the shard's node. But blob compaction and capacity are tied to search
     nodes, and large blobs compete with segment I/O.
   - Make it a separate service with its own groups and placement: storage scales
     independently of search, and the engine stores only the fields results need.
     But a document fetch costs one more hop, and there is another service to run.

   Recommendation: a separate service built from the same group machinery, with
   strata's stored section holding what search results return. Track D7 measures
   both on the fetch paths Xapiand's API uses before the decision is made.
2. **Membership library and transport.** The Detent stack has no gossip library, and
   its peer transport carries Raft over TLS on persistent connections with control and
   bulk lanes. SWIM probes random peers every period, which at 100,000 nodes needs a
   bounded connection cache on that transport or a datagram path beside it. The
   decision names the library and its transport and states connections and messages
   per node per second at 100,000 nodes. Any change to `consensus`'s transport or to
   `cluster` goes through the Detent work first. Track D2 follows it.
3. **Coordinator placement.** Coordination on every node, or dedicated tiers above a
   cluster size, decided by D5's fan-out measurements.

## Dependencies

Membership, transport, and consensus come from the stack built for Detent
(`completion`, `journal`, `consensus`, and its peer transport). Changes to them are
coordinated with the Detent work first (PLAN, Track C).
