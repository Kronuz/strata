"""Placement strategies: hash, count, affinity, offline partition, SPAR and adaptive locality."""

from __future__ import annotations

import heapq
import random

from cluster import PEER_BOUND, RELAY_FANOUT, Cluster, Placement, bytes_on, peers
from partition import build_adjacency, partition
from spar import Spar
from workloads import Workload

VOTERS = 3


def _mix(x: int) -> int:
    """A 64-bit integer hash (splitmix64's finalizer), stable across runs."""
    x = (x + 0x9E3779B97F4A7C15) & 0xFFFFFFFFFFFFFFFF
    x = ((x ^ (x >> 30)) * 0xBF58476D1CE4E5B9) & 0xFFFFFFFFFFFFFFFF
    x = ((x ^ (x >> 27)) * 0x94D049BB133111EB) & 0xFFFFFFFFFFFFFFFF
    return x ^ (x >> 31)


# -- declarative strategies ------------------------------------------------------------------------


def hash_place(w: Workload, c: Cluster, seed: int = 0) -> Placement:
    """Hash or random: each unit's voters on distinct nodes chosen by its hash, no domains."""
    rng = random.Random(seed)
    v = min(VOTERS, c.n)
    return Placement(
        "hash", [rng.sample(range(c.n), v) for _ in range(w.units)], [[] for _ in range(w.units)]
    )


def count_place(w: Workload, c: Cluster, copies: int = 0) -> Placement:
    """Xapiand today: a hashed start on the name-sorted ring, then the next nodes, no domains."""
    voters, reads = [], []
    for u in range(w.units):
        ring = c.ring_set(_mix(u) % c.n, VOTERS + copies, spread=False)
        voters.append(ring[:VOTERS])
        reads.append(ring[VOTERS:])
    return Placement("count" if not copies else f"count+{copies}", voters, reads)


def copyset(c: Cluster, j: int) -> list[int]:
    """Copyset j: nodes j, j + N/3 and j + 2N/3, each moved forward to a new failure domain.

    Every node is in about three copysets, so voters alone give it about six peers.
    """
    out: list[int] = []
    used: set[int] = set()
    for i in range(min(VOTERS, c.n)):
        x = (j + i * c.n // VOTERS) % c.n
        for _ in range(c.n):
            if x not in out and (c.domain(x) not in used or len(used) >= c.domains):
                break
            x = (x + 1) % c.n
        out.append(x)
        used.add(c.domain(x))
    return out


def _groups(w: Workload) -> list[list[int]]:
    by: dict[int, list[int]] = {}
    out: list[list[int]] = []
    for u, g in enumerate(w.group):
        if g < 0:
            out.append([u])
        else:
            by.setdefault(g, []).append(u)
    out.extend(by.values())
    return out


def affinity_place(w: Workload, c: Cluster) -> Placement:
    """Declared affinity and follow: a group's units share one copyset, chosen by two hashes.

    Groups go largest first to the less loaded of two hashed copysets (two-choice balancing).
    """
    sets = [copyset(c, j) for j in range(c.n)]
    load = [0.0] * c.n
    voters: list[list[int]] = [[] for _ in range(w.units)]
    groups = _groups(w)
    groups.sort(key=lambda g: -sum(w.size[u] for u in g))
    for g in groups:
        b = sum(w.size[u] for u in g)
        key = g[0] * 2 + 1
        a1, a2 = _mix(key) % c.n, _mix(key ^ 0x5BD1E995) % c.n
        j = min((a1, a2), key=lambda x: max(load[y] for y in sets[x]))
        for y in sets[j]:
            load[y] += b
        for u in g:
            voters[u] = list(sets[j])
    return Placement("affinity", voters, [[] for _ in range(w.units)])


def coaccess_graph(w: Workload) -> dict[tuple[int, int], float]:
    edges: dict[tuple[int, int], float] = {}
    for a, members, rate in w.templates:
        for m in members:
            key = (a, m) if a < m else (m, a)
            edges[key] = edges.get(key, 0.0) + rate
    # Declared groups are co-placed by definition: tie them with heavy edges.
    for g in _groups(w):
        for u in g[1:]:
            key = (g[0], u) if g[0] < u else (u, g[0])
            edges[key] = edges.get(key, 0.0) + 1e6
    return edges


def partition_place(w: Workload, c: Cluster, seed: int = 0) -> Placement:
    """Offline ideal: a k-way partition of the co-access graph, part j on copyset j."""
    adj = build_adjacency(w.units, coaccess_graph(w))
    part = partition(adj, w.size, c.n, seed=seed)
    sets = [copyset(c, j) for j in range(c.n)]
    p = Placement("partition", [list(sets[part[u]]) for u in range(w.units)], [[] for _ in range(w.units)])
    p.extra["part"] = part
    return p


def local_post(w: Workload, base: Placement) -> Placement:
    """SPAR's post-processing of an offline partition: read copies give every leader local reads."""
    reads = [list(r) for r in base.reads]
    have = [set(base.voters[u]) | set(base.reads[u]) for u in range(w.units)]
    for a, members, _ in w.templates:
        leader = base.voters[a][0]
        for m in members:
            if leader not in have[m]:
                have[m].add(leader)
                reads[m].append(leader)
    return Placement(base.strategy + "+local", [list(v) for v in base.voters], reads)


def spar_place(w: Workload, c: Cluster, k: int = 2, seed: int = 0) -> tuple[Placement, Spar]:
    s = Spar(c.n, k, w.size, seed=seed)
    for u, v in w.edges:
        s.add_edge(u, v)
    for u in range(w.units):
        if s.master[u] < 0:
            s.add_user(u)
    return s.placement(), s


# -- adaptive locality -------------------------------------------------------------------------


def adaptive_place(
    w: Workload,
    c: Cluster,
    base: Placement,
    beta: float = 1.0,
    horizon: float = 86_400.0,
    hub_frac: float = 0.5,
    h: float = 0.0,
    incumbent: list[list[int]] | None = None,
    rates: list[float] | None = None,
    name: str = "adaptive",
) -> Placement:
    """The design's corrected SPAR: read copies only, under budgets, with hysteresis and hubs.

    Voters stay where ``base`` put them. A request is worth its rate; completing it at its
    anchor's leader costs the bytes of the missing copies, each costed at its size plus its
    write rate over ``horizon`` seconds. Requests are completed best value per cost first while
    the leader's locality budget (``beta`` times the mean voter bytes per node) and the peer
    bound allow. With ``incumbent`` copies, hysteresis discounts keeping a copy by ``1 + h`` and
    charges adding one ``1 + h``. Units whose read copies reach ``hub_frac`` of the other nodes
    are promoted to ``all`` and leave the edge-by-edge bookkeeping.
    """
    rates = rates if rates is not None else [r for _, _, r in w.templates]
    hubs: set[int] = set()
    reads, feeder = _greedy(w, c, base, beta, horizon, h, incumbent, rates, hubs)
    for u in range(w.units):
        if reads[u] and len(reads[u]) >= hub_frac * (c.n - len(base.voters[u])):
            hubs.add(u)
    if hubs:
        reads, feeder = _greedy(w, c, base, beta, horizon, h, incumbent, rates, hubs)
        for u in hubs:
            vs = set(base.voters[u])
            reads[u] = [x for x in range(c.n) if x not in vs]
    p = Placement(name, [list(v) for v in base.voters], reads)
    p.extra["feeder"] = feeder
    p.extra["all_units"] = hubs
    p.extra["hubs"] = len(hubs)
    return p


def _greedy(
    w: Workload,
    c: Cluster,
    base: Placement,
    beta: float,
    horizon: float,
    h: float,
    incumbent: list[list[int]] | None,
    rates: list[float],
    hubs: set[int],
) -> tuple[list[list[int]], dict[tuple[int, int], int]]:
    units = w.units
    have = [set(base.voters[u]) for u in range(units)]
    reads: list[list[int]] = [[] for _ in range(units)]
    feeder: dict[tuple[int, int], int] = {}
    children: dict[tuple[int, int], int] = {}
    inc = [set(x) for x in incumbent] if incumbent is not None else None
    voter_bytes = sum(w.size[u] * len(base.voters[u]) for u in range(units))
    budget = beta * voter_bytes / c.n
    used = [0.0] * c.n
    peer = peers(base, c.n)

    def cost(m: int, node: int) -> float:
        k = w.size[m] + w.write[m] * horizon
        if inc is None:
            return k
        return k / (1 + h) if node in inc[m] else k * (1 + h)

    def missing(t: int) -> list[int]:
        a, members, _ = w.templates[t]
        leader = base.voters[a][0]
        return [m for m in members if m not in hubs and leader not in have[m]]

    heap: list[tuple[float, int]] = []
    for t, (a, members, _) in enumerate(w.templates):
        if not members or rates[t] <= 0:
            continue
        miss = missing(t)
        if miss:
            leader = base.voters[a][0]
            heap.append((-rates[t] / sum(cost(m, leader) for m in miss), t))
    heapq.heapify(heap)
    while heap:
        _, t = heapq.heappop(heap)
        a = w.templates[t][0]
        leader = base.voters[a][0]
        miss = missing(t)
        if not miss:
            continue
        need = sum(w.size[m] for m in miss)
        if used[leader] + need > budget:
            continue
        plan: list[tuple[int, int]] = []
        new_peers: set[int] = set()
        ok = True
        for m in miss:
            src = next((x for x in have[m] if x in peer[leader]), -1)
            if src < 0:
                cands = [
                    x for x in have[m] if len(peer[x]) < PEER_BOUND and children.get((m, x), 0) < RELAY_FANOUT
                ]
                if not cands or len(peer[leader]) + len(new_peers) >= PEER_BOUND:
                    ok = False
                    break
                src = min(cands, key=lambda x: len(peer[x]))
                new_peers.add(src)
            plan.append((m, src))
        if not ok:
            continue
        for m, src in plan:
            have[m].add(leader)
            reads[m].append(leader)
            feeder[(m, leader)] = src
            children[(m, src)] = children.get((m, src), 0) + 1
            peer[leader].add(src)
            peer[src].add(leader)
            used[leader] += w.size[m]
    return reads, feeder


# -- churn -------------------------------------------------------------------------------------


def churn_slots(w: Workload, p: Placement, leave: set[int]) -> float:
    """Copyset strategies replace a node in its slots: every copy it held is copied once."""
    return bytes_on(w, p, leave)


def churn_count(w: Workload, c: Cluster, p: Placement, leave: set[int]) -> float:
    """Xapiand's saved shard maps: lost copies go to the next ring node; joiners get nothing."""
    return bytes_on(w, p, leave)


def churn_hash(w: Workload, c: Cluster, p: Placement, leave: set[int], seed: int = 0) -> float:
    """Rendezvous hashing: lost copies go to the next-ranked node, and each joiner wins its
    share of every unit (``copies / N``), so it moves the repair plus the joiners' share."""
    rng = random.Random(seed)
    moved = bytes_on(w, p, leave)
    joins = len(leave)
    for u in range(w.units):
        k = len(p.voters[u]) + len(p.reads[u])
        for _ in range(joins):
            if rng.random() < k / c.n:
                moved += w.size[u]
    return moved


def churn_spar(s: Spar, leave: set[int]) -> float:
    """SPAR's server removal, then forced redistribution onto the same number of new servers."""
    moved = 0.0
    for x in sorted(leave):
        moved += s.remove_server(x)
    for x in sorted(leave):
        moved += s.add_server(x)
    return moved
