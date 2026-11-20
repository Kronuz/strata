"""A METIS-style multilevel k-way partitioner in pure Python.

Heavy-edge matching coarsens the graph, a breadth-first fill seeds k balanced parts on the
coarsest level, and size-constrained greedy moves refine each level on the way back up. It
minimizes the weighted edge cut under a vertex-weight cap of ``(1 + eps) * total / k``.
"""

from __future__ import annotations

import random
from collections import deque

Adj = list[dict[int, float]]


def build_adjacency(n: int, weighted_edges: dict[tuple[int, int], float]) -> Adj:
    adj: Adj = [{} for _ in range(n)]
    for (u, v), wt in weighted_edges.items():
        if u == v:
            continue
        adj[u][v] = adj[u].get(v, 0.0) + wt
        adj[v][u] = adj[v].get(u, 0.0) + wt
    return adj


def _coarsen(
    adj: Adj, vw: list[float], limit: float, rng: random.Random
) -> tuple[Adj, list[float], list[int]]:
    n = len(adj)
    match = [-1] * n
    order = list(range(n))
    rng.shuffle(order)
    cmap = [-1] * n
    nc = 0
    for u in order:
        if match[u] >= 0:
            continue
        best, best_w = -1, 0.0
        for v, wt in adj[u].items():
            if match[v] < 0 and v != u and wt > best_w and vw[u] + vw[v] <= limit:
                best, best_w = v, wt
        if best >= 0:
            match[u], match[best] = best, u
            cmap[u] = cmap[best] = nc
        else:
            match[u] = u
            cmap[u] = nc
        nc += 1
    cadj: Adj = [{} for _ in range(nc)]
    cvw = [0.0] * nc
    for u in range(n):
        cu = cmap[u]
        cvw[cu] += vw[u]
        row = cadj[cu]
        for v, wt in adj[u].items():
            cv = cmap[v]
            if cv != cu:
                row[cv] = row.get(cv, 0.0) + wt
    return cadj, cvw, cmap


def _initial(adj: Adj, vw: list[float], k: int, target: float) -> list[int]:
    n = len(adj)
    part = [-1] * n
    p = 0
    load = 0.0
    order = sorted(range(n), key=lambda x: -sum(adj[x].values()))
    for seed in order:
        if part[seed] >= 0:
            continue
        queue = deque([seed])
        part[seed] = -2
        while queue:
            u = queue.popleft()
            if load + vw[u] > target and load > 0 and p < k - 1:
                p += 1
                load = 0.0
            part[u] = p
            load += vw[u]
            for v in sorted(adj[u], key=lambda x: -adj[u][x]):
                if part[v] == -1:
                    part[v] = -2
                    queue.append(v)
    return part


def _refine(
    adj: Adj, vw: list[float], part: list[int], k: int, cap: float, passes: int, rng: random.Random
) -> None:
    load = [0.0] * k
    for u, p in enumerate(part):
        load[p] += vw[u]
    order = list(range(len(adj)))
    for _ in range(passes):
        rng.shuffle(order)
        moved = 0
        for u in order:
            own = part[u]
            conn: dict[int, float] = {}
            for v, wt in adj[u].items():
                pv = part[v]
                conn[pv] = conn.get(pv, 0.0) + wt
            if not conn:
                continue
            here = conn.get(own, 0.0)
            best, gain = own, 0.0
            for q, c in conn.items():
                if q == own:
                    continue
                g = c - here
                if load[q] + vw[u] > cap:
                    continue
                if g > gain or (g == gain and g >= 0 and best != own and load[q] < load[best]):
                    best, gain = q, g
            # Zero-gain moves only to rebalance an overloaded part.
            if best != own and (gain > 0 or (gain == 0 and load[own] > cap)):
                part[u] = best
                load[own] -= vw[u]
                load[best] += vw[u]
                moved += 1
        if moved == 0:
            break
    # Balance: move the least-connected vertices out of overloaded parts.
    over = {p for p in range(k) if load[p] > cap}
    by_part: dict[int, list[int]] = {p: [] for p in over}
    for u, p in enumerate(part):
        if p in over:
            by_part[p].append(u)
    for p in sorted(over):
        members = by_part[p]
        members.sort(key=lambda u: sum(wt for v, wt in adj[u].items() if part[v] == p))
        for u in members:
            if load[p] <= cap:
                break
            q = min(range(k), key=load.__getitem__)
            if q == p or load[q] + vw[u] > cap:
                continue
            part[u] = q
            load[p] -= vw[u]
            load[q] += vw[u]


def partition(
    adj: Adj, vw: list[float], k: int, eps: float = 0.03, seed: int = 0, passes: int = 6
) -> list[int]:
    """Assign each vertex a part in ``range(k)``, minimizing the cut under the weight cap."""
    rng = random.Random(seed)
    n = len(adj)
    if k <= 1 or n == 0:
        return [0] * n
    total = sum(vw)
    cap = max((1 + eps) * total / k, max(vw))
    levels: list[tuple[Adj, list[float], list[int]]] = []
    cur_adj, cur_vw = adj, vw
    while len(cur_adj) > max(4 * k, 64):
        cadj, cvw, cmap = _coarsen(cur_adj, cur_vw, cap / 2, rng)
        if len(cadj) > 0.95 * len(cur_adj):
            break
        levels.append((cur_adj, cur_vw, cmap))
        cur_adj, cur_vw = cadj, cvw
    part = _initial(cur_adj, cur_vw, k, total / k)
    _refine(cur_adj, cur_vw, part, k, cap, passes, rng)
    for fine_adj, fine_vw, cmap in reversed(levels):
        part = [part[cmap[u]] for u in range(len(fine_adj))]
        _refine(fine_adj, fine_vw, part, k, cap, passes, rng)
    return part


def cut(adj: Adj, part: list[int]) -> float:
    return sum(wt for u in range(len(adj)) for v, wt in adj[u].items() if u < v and part[u] != part[v])
