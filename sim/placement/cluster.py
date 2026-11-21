"""Nodes, failure domains, placements and the metrics every strategy reports."""

from __future__ import annotations

import random
from dataclasses import dataclass, field

from workloads import Workload

PEER_BOUND = 64
RELAY_FANOUT = 16


@dataclass
class Cluster:
    """N nodes in contiguous failure domains (racks), named in ring order."""

    n: int
    domains: int

    @classmethod
    def of(cls, n: int) -> Cluster:
        return cls(n, max(3, n // 10))

    def domain(self, node: int) -> int:
        return node * self.domains // self.n

    def ring_set(self, start: int, size: int, spread: bool) -> list[int]:
        """Nodes from ``start`` forward on the ring; with ``spread``, each in a new domain."""
        out = [start % self.n]
        used = {self.domain(out[0])}
        node = start
        steps = 0
        while len(out) < min(size, self.n):
            node = (node + 1) % self.n
            steps += 1
            if node in out:
                continue
            d = self.domain(node)
            if spread and d in used and len(used) < self.domains and steps < 4 * self.n:
                continue
            out.append(node)
            used.add(d)
        return out


@dataclass
class Placement:
    """Per unit: voters (leader first) and read copies.

    ``coord_any``: a request may be coordinated by any copy of its anchor (read copies serve
    reads); SPAR sends it to the master only. ``durable_all``: every copy counts toward
    durability (SPAR's asynchronous slaves); otherwise only voters do. ``fanout``: read copies
    pull through a relay tree of this fan-out; 0 means the leader pushes to each copy (SPAR).
    """

    strategy: str
    voters: list[list[int]]
    reads: list[list[int]]
    coord_any: bool = True
    durable_all: bool = False
    fanout: int = RELAY_FANOUT
    extra: dict[str, object] = field(default_factory=dict)

    def copies(self, u: int) -> list[int]:
        return self.voters[u] + self.reads[u]


def peers(p: Placement, n: int) -> list[set[int]]:
    """Replication peers per node: voters peer pairwise; a read copy peers with its feeder."""
    out: list[set[int]] = [set() for _ in range(n)]
    feeder: dict[tuple[int, int], int] = p.extra.get("feeder", {})  # type: ignore[assignment]
    everywhere: set[int] = p.extra.get("all_units", set())  # type: ignore[assignment]
    if everywhere:
        # Units on every node share one relay tree over the node ids, fed by node 0.
        for x in range(1, n):
            parent = (x - 1) // RELAY_FANOUT
            out[x].add(parent)
            out[parent].add(x)
    for u in range(len(p.voters)):
        vs = p.voters[u]
        for i, a in enumerate(vs):
            for b in vs[i + 1 :]:
                if a != b:
                    out[a].add(b)
                    out[b].add(a)
        if u in everywhere:
            if vs[0] != 0:
                out[vs[0]].add(0)
                out[0].add(vs[0])
            continue
        chain = vs + p.reads[u]
        v = len(vs)
        for i in range(v, len(chain)):
            c = chain[i]
            parent = feeder.get((u, c), -1)
            if parent < 0:
                parent = chain[0] if p.fanout <= 0 else chain[(i - v) // p.fanout]
            if parent != c:
                out[c].add(parent)
                out[parent].add(c)
    return out


def evaluate(w: Workload, c: Cluster, p: Placement, sample: int = 20_000, seed: int = 0) -> dict[str, float]:
    """Replication overhead, locality, fan-out, peers, spread and load for one placement."""
    units = w.units
    total_bytes = sum(w.size)
    copies = [set(p.copies(u)) for u in range(units)]
    ncopies = sum(len(s) for s in copies)
    copy_bytes = sum(w.size[u] * len(copies[u]) for u in range(units))
    reads = sum(len(r) for r in p.reads)

    templates = w.templates
    if len(templates) > sample:
        templates = random.Random(seed).sample(templates, sample)
    rate_total = 0.0
    rate_local = 0.0
    fan_sum = 0.0
    fans: list[tuple[int, float]] = []
    load = [0.0] * c.n
    for anchor, members, rate in templates:
        cands = p.copies(anchor) if p.coord_any else p.voters[anchor][:1]
        # The copy holding most members coordinates; ties go to the least loaded copy.
        best = cands[0]
        best_have = -1
        for node in cands:
            have = 0
            for m in members:
                if node in copies[m]:
                    have += 1
            if have > best_have or (have == best_have and load[node] < load[best]):
                best, best_have = node, have
        missing = [m for m in members if best not in copies[m]]
        fan = 1
        while missing:
            count: dict[int, int] = {}
            for m in missing:
                for node in copies[m]:
                    count[node] = count.get(node, 0) + 1
            pick = max(count, key=count.__getitem__)
            missing = [m for m in missing if pick not in copies[m]]
            fan += 1
        rate_total += rate
        if fan == 1:
            rate_local += rate
        fan_sum += fan * rate
        fans.append((fan, rate))
        load[best] += rate
    fans.sort()
    acc = 0.0
    p99 = fans[-1][0] if fans else 1
    for f, r in fans:
        acc += r
        if acc >= 0.99 * rate_total:
            p99 = f
            break

    peer_sets = peers(p, c.n)
    peer_counts = [len(s) for s in peer_sets]
    need = min(3, c.domains)
    spread_ok = 0
    for u in range(units):
        durable = copies[u] if p.durable_all else set(p.voters[u])
        if len({c.domain(x) for x in durable}) >= need:
            spread_ok += 1
    mean_load = sum(load) / c.n if c.n else 0.0
    return {
        "units": units,
        "copies_per_unit": ncopies / units,
        "byte_overhead": copy_bytes / total_bytes,
        "read_copies_per_unit": reads / units,
        "local_frac": rate_local / rate_total if rate_total else 1.0,
        "fanout_mean": fan_sum / rate_total if rate_total else 1.0,
        "fanout_p99": float(p99),
        "peers_max": float(max(peer_counts)),
        "peers_mean": sum(peer_counts) / c.n,
        "over_peer_bound": sum(1 for x in peer_counts if x > PEER_BOUND) / c.n,
        "spread_frac": spread_ok / units,
        "load_max_over_mean": max(load) / mean_load if mean_load else 0.0,
    }


def bytes_on(w: Workload, p: Placement, nodes: set[int]) -> float:
    """Bytes of every copy, voter or read, held by ``nodes``."""
    total = 0.0
    for u in range(w.units):
        for x in p.voters[u]:
            if x in nodes:
                total += w.size[u]
        for x in p.reads[u]:
            if x in nodes:
                total += w.size[u]
    return total
