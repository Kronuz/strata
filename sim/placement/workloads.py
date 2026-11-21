"""Co-access workloads: placement units, request templates and an edge arrival order."""

from __future__ import annotations

import gzip
import math
import random
from dataclasses import dataclass, field


@dataclass
class Workload:
    """Units with sizes, request templates over them, and co-access edges in arrival order.

    A template ``(anchor, members, rate)`` is a request routed by ``anchor`` that also reads
    ``members``. ``edges`` are the undirected co-access pairs, in the order SPAR's online
    algorithm sees them.
    """

    name: str
    size: list[float]
    write: list[float]
    group: list[int]
    templates: list[tuple[int, tuple[int, ...], float]]
    edges: list[tuple[int, int]]
    indexes: int = 0
    notes: dict[str, object] = field(default_factory=dict)

    @property
    def units(self) -> int:
        return len(self.size)


def _templates_from_adjacency(adj: list[set[int]]) -> list[tuple[int, tuple[int, ...], float]]:
    """SPAR's request: a user reads her own data and every neighbor's, at rate 1."""
    return [(u, tuple(sorted(nbrs)), 1.0) for u, nbrs in enumerate(adj) if nbrs]


def from_edges(name: str, pairs: list[tuple[int, int]], seed: int = 0) -> Workload:
    """A social workload from an ordered list of undirected edges; unit size 1, no writes."""
    ids: dict[int, int] = {}
    edges: list[tuple[int, int]] = []
    seen: set[tuple[int, int]] = set()
    for a, b in pairs:
        if a == b:
            continue
        u = ids.setdefault(a, len(ids))
        v = ids.setdefault(b, len(ids))
        key = (u, v) if u < v else (v, u)
        if key in seen:
            continue
        seen.add(key)
        edges.append((u, v))
    n = len(ids)
    adj: list[set[int]] = [set() for _ in range(n)]
    for u, v in edges:
        adj[u].add(v)
        adj[v].add(u)
    return Workload(
        name=name,
        size=[1.0] * n,
        write=[0.0] * n,
        group=[-1] * n,
        templates=_templates_from_adjacency(adj),
        edges=edges,
        indexes=n,
        notes={"seed": seed},
    )


def load_edge_list(path: str, seed: int = 0) -> Workload:
    """Read a SNAP or WOSN edge list (``a b [timestamp]``, ``#`` comments, gzip ok).

    Edges with a timestamp are replayed in time order after the untimestamped ones, which come
    first in a seeded random order, as SPAR's evaluation does.
    """
    opener = gzip.open if path.endswith(".gz") else open
    untimed: list[tuple[int, int]] = []
    timed: list[tuple[int, int, int]] = []
    with opener(path, "rt") as f:
        for line in f:
            if not line or line[0] == "#":
                continue
            parts = line.split()
            if len(parts) < 2:
                continue
            a, b = int(parts[0]), int(parts[1])
            if len(parts) >= 3 and parts[2].isdigit():
                timed.append((int(parts[2]), a, b))
            else:
                untimed.append((a, b))
    random.Random(seed).shuffle(untimed)
    timed.sort()
    pairs = untimed + [(a, b) for _, a, b in timed]
    base = path.rsplit("/", 1)[-1].split(".")[0]
    return from_edges(base, pairs, seed)


def synthetic(scale: float = 1.0, seed: int = 0) -> Workload:
    """A many-indexes workload shaped like strata's.

    Tenants own path subtrees (``/t/a/b``) of small indexes; each subtree folds into one
    placement unit. A few tenants split off a write-hot nested root (``/t/logs``). Tenants
    belong to organizations; some declare ``affinity`` for theirs, the rest are only observed.
    A handful of shared hub indexes are read with many tenants.
    """
    rng = random.Random(seed)
    tenants = max(20, int(100_000 * scale))
    hubs = 8
    size: list[float] = []
    write: list[float] = []
    group: list[int] = []
    indexes = 0
    kib = 1024.0

    # Tenant roots: a nested tree of small indexes, folded into one unit.
    nested_of: dict[int, int] = {}
    for _ in range(tenants):
        count = 1 + min(200, int(rng.expovariate(1 / 9)))
        indexes += count
        total = sum(rng.lognormvariate(math.log(64 * kib), 2.0) for _ in range(count))
        size.append(total)
        write.append(total * rng.lognormvariate(math.log(1e-5), 1.5))
        group.append(-1)
    # Nested roots: /t/logs declared as its own root, related to its parent by default.
    for t in range(tenants):
        if rng.random() < 0.03:
            nested_of[t] = len(size)
            indexes += 1
            big = rng.lognormvariate(math.log(64 * 1024 * kib), 1.0)
            size.append(big)
            write.append(big * rng.lognormvariate(math.log(1e-3), 1.0))
            group.append(-1)
    # Hubs: shared dimension indexes, small, read-mostly.
    hub_ids = []
    for h in range(hubs):
        hub_ids.append(len(size))
        indexes += 1
        size.append(rng.uniform(10, 200) * 1024 * kib)
        write.append(1.0 * kib)
        group.append(-1)

    # Organizations of 1..40 tenants; 30% declare affinity for theirs.
    orgs: list[list[int]] = []
    order = list(range(tenants))
    rng.shuffle(order)
    i = 0
    while i < tenants:
        k = min(tenants - i, max(1, int(rng.paretovariate(1.3))))
        k = min(k, 40)
        orgs.append(order[i : i + k])
        i += k
    org_of = [0] * tenants
    for o, members in enumerate(orgs):
        for t in members:
            org_of[t] = o
        if len(members) > 1 and rng.random() < 0.3:
            for t in members:
                group[t] = o
    # A declared nested root follows its parent (``follow``), so it joins the parent's group.
    for t, n in nested_of.items():
        if rng.random() < 0.5:
            group[n] = group[t] if group[t] >= 0 else tenants + t
            if group[t] < 0:
                group[t] = tenants + t

    templates: list[tuple[int, tuple[int, ...], float]] = []
    hub_weights = [1.0 / (h + 1) for h in range(hubs)]
    for t in range(tenants):
        r = rng.lognormvariate(0.0, 1.5)
        templates.append((t, (), 0.6 * r))
        h = rng.choices(hub_ids, hub_weights)[0]
        templates.append((t, (h,), 0.2 * r))
        mates = [m for m in orgs[org_of[t]] if m != t]
        if mates:
            pick = tuple(sorted(rng.sample(mates, min(4, len(mates)))))
            templates.append((t, pick, 0.12 * r))
        if t in nested_of:
            templates.append((t, (nested_of[t],), 0.08 * r))
    edge_set: set[tuple[int, int]] = set()
    for a, members, _ in templates:
        for m in members:
            edge_set.add((a, m) if a < m else (m, a))
    edges = sorted(edge_set)
    rng.shuffle(edges)
    return Workload(
        name=f"synthetic-{scale:g}",
        size=size,
        write=write,
        group=group,
        templates=templates,
        edges=edges,
        indexes=indexes,
        notes={"tenants": tenants, "nested_roots": len(nested_of), "hubs": hubs, "seed": seed},
    )
