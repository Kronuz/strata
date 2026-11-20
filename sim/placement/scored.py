"""The scripted-placement seam: a strategy is one scoring function, the engine keeps invariants.

A scorer sees ``(unit, node, view)`` and returns a score, or ``None`` to filter the node out.
The engine walks candidates best score first and accepts a node only if it keeps every
invariant: distinct nodes, a new failure domain while domains remain, the peer bound and node
capacity. When the scorer's candidates cannot satisfy them, the engine fills the rest from the
least-loaded nodes that do, and counts it in ``extra["engine_filled"]``. This is where a parked
Lua scorer (placement-strategies.md, "Scripted placement") would be dry-run.
"""

from __future__ import annotations

from collections.abc import Callable
from dataclasses import dataclass

from cluster import PEER_BOUND, Cluster, Placement
from strategies import VOTERS, _mix
from workloads import Workload


@dataclass
class View:
    """What a scorer may read: the cluster, the workload and the placement so far."""

    cluster: Cluster
    workload: Workload
    voters: list[list[int]]  # empty for units not placed yet
    load: list[float]  # voter bytes per node
    peers: list[set[int]]


Scorer = Callable[[int, int, View], "float | None"]


def scored_place(
    w: Workload,
    c: Cluster,
    score: Scorer,
    capacity: float | None = None,
    peer_bound: int = PEER_BOUND,
    name: str = "scored",
) -> Placement:
    """Place units largest first: the scorer ranks nodes, the engine enforces invariants."""
    view = View(c, w, [[] for _ in range(w.units)], [0.0] * c.n, [set() for _ in range(c.n)])
    cap = capacity if capacity is not None else float("inf")
    v = min(VOTERS, c.n)
    filled = 0

    def fits(u: int, node: int, chosen: list[int], domains: set[int]) -> bool:
        if node in chosen or view.load[node] + w.size[u] > cap:
            return False
        if c.domain(node) in domains and len(domains) < min(c.domains, v):
            return False
        new = sum(1 for x in chosen if x not in view.peers[node])
        if len(view.peers[node]) + new > peer_bound:
            return False
        return all(node in view.peers[x] or len(view.peers[x]) < peer_bound for x in chosen)

    for u in sorted(range(w.units), key=lambda x: -w.size[x]):
        ranked = []
        for node in range(c.n):
            s = score(u, node, view)
            if s is not None:
                ranked.append((-s, node))
        ranked.sort()
        chosen: list[int] = []
        domains: set[int] = set()
        for _, node in ranked:
            if len(chosen) == v:
                break
            if fits(u, node, chosen, domains):
                chosen.append(node)
                domains.add(c.domain(node))
        if len(chosen) < v:
            for node in sorted(range(c.n), key=lambda x: (view.load[x], x)):
                if len(chosen) == v:
                    break
                if fits(u, node, chosen, domains):
                    chosen.append(node)
                    domains.add(c.domain(node))
                    filled += 1
        for i, a in enumerate(chosen):
            view.load[a] += w.size[u]
            for b in chosen[i + 1 :]:
                view.peers[a].add(b)
                view.peers[b].add(a)
        view.voters[u] = chosen
    p = Placement(name, view.voters, [[] for _ in range(w.units)])
    p.extra["engine_filled"] = filled
    return p


def rendezvous(u: int, node: int, view: View) -> float:
    """Hash placement as a scorer: highest random weight."""
    return float(_mix(u * 1_000_003 + node))


def locality(neighbors: list[dict[int, float]], alpha: float = 1.0) -> Scorer:
    """Score a node by co-access with units already led there, minus a load penalty."""

    def score(u: int, node: int, view: View) -> float:
        near = sum(wt for x, wt in neighbors[u].items() if view.voters[x][:1] == [node])
        mean = sum(view.load) / len(view.load) or 1.0
        return near - alpha * view.load[node] / mean

    return score
