#!/usr/bin/env python3
"""Run placement strategies over co-access workloads and print one table per graph and N.

python3 sim/placement/run.py --smoke
python3 sim/placement/run.py --graph .scratch/.../facebook-links.txt.gz --nodes 10,100
python3 sim/placement/run.py --synthetic 1.0 --nodes 1000 --strategies hash,adaptive
python3 sim/placement/run.py --root-index
"""

from __future__ import annotations

import argparse
import json
import os
import random
import sys
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import root_index
from cluster import Cluster, Placement, bytes_on, evaluate
from strategies import (
    adaptive_place,
    affinity_place,
    churn_count,
    churn_hash,
    churn_slots,
    churn_spar,
    count_place,
    hash_place,
    local_post,
    partition_place,
    spar_place,
)
from workloads import Workload, from_edges, load_edge_list, synthetic

ALL = ["hash", "count", "affinity", "partition", "partition+local", "spar", "adaptive", "two-level"]
COLUMNS = [
    ("strategy", "strategy", "{}"),
    ("copies/unit", "copies_per_unit", "{:.2f}"),
    ("bytes x", "byte_overhead", "{:.2f}"),
    ("local %", "local_frac", "{:.1%}"),
    ("fan-out", "fanout_mean", "{:.2f}"),
    ("p99", "fanout_p99", "{:.0f}"),
    ("moved %/1% churn", "churn_pct", "{:.2f}"),
    ("x min", "churn_x_min", "{:.2f}"),
    ("peers max", "peers_max", "{:.0f}"),
    ("peers mean", "peers_mean", "{:.1f}"),
    ("spread %", "spread_frac", "{:.1%}"),
    ("load max/mean", "load_max_over_mean", "{:.2f}"),
    ("s", "seconds", "{:.1f}"),
]


def planted(groups: int, size: int, p_in: float, p_out: float, seed: int = 0) -> Workload:
    """A small planted-partition social graph for smoke runs."""
    rng = random.Random(seed)
    n = groups * size
    pairs = []
    for u in range(n):
        for v in range(u + 1, n):
            same = u // size == v // size
            if rng.random() < (p_in if same else p_out):
                pairs.append((u, v))
    rng.shuffle(pairs)
    return from_edges(f"planted-{groups}x{size}", pairs, seed)


def run_one(
    w: Workload, n: int, strategies: list[str], seed: int, sample: int, beta: float
) -> list[dict[str, object]]:
    c = Cluster.of(n)
    rng = random.Random(seed + n)
    k = max(1, round(0.01 * n))
    leave = set(rng.sample(range(n), k))
    total = sum(w.size)
    churn_scale = 100.0 * k / n
    rows: list[dict[str, object]] = []
    cache: dict[str, Placement] = {}

    def base(name: str) -> Placement:
        if name not in cache:
            cache[name] = affinity_place(w, c) if name == "affinity" else partition_place(w, c, seed)
        return cache[name]

    for name in strategies:
        t0 = time.perf_counter()
        spar_state = None
        if name == "hash":
            p = hash_place(w, c, seed)
        elif name == "count":
            p = count_place(w, c)
        elif name == "affinity":
            p = base("affinity")
        elif name == "partition":
            p = base("partition")
        elif name == "partition+local":
            p = local_post(w, base("partition"))
        elif name == "spar":
            p, spar_state = spar_place(w, c, k=2, seed=seed)
        elif name == "adaptive":
            p = adaptive_place(w, c, base("affinity"), beta=beta)
        elif name == "two-level":
            p = adaptive_place(w, c, base("partition"), beta=beta, name="two-level")
        else:
            raise SystemExit(f"unknown strategy {name}")
        build = time.perf_counter() - t0
        m: dict[str, object] = {"strategy": name, **evaluate(w, c, p, sample=sample, seed=seed)}
        minimum = bytes_on(w, p, leave)
        if name == "hash":
            moved = churn_hash(w, c, p, leave, seed)
        elif name == "count":
            moved = churn_count(w, c, p, leave)
        elif name == "spar":
            assert spar_state is not None
            moved = churn_spar(spar_state, leave)
            m["master_cov"] = p.extra["master_cov"]
            m["spar_moves"] = p.extra["moves"]
        else:
            moved = churn_slots(w, p, leave)
        m["churn_pct"] = 100.0 * moved / total / churn_scale
        m["churn_x_min"] = moved / minimum if minimum else 0.0
        if "hubs" in p.extra:
            m["hubs"] = p.extra["hubs"]
        m["seconds"] = build
        m.update({"graph": w.name, "nodes": n, "domains": c.domains, "beta": beta, "leave": k})
        rows.append(m)
    return rows


def drift(w: Workload, n: int, seed: int, beta: float, hs: list[float]) -> list[dict[str, object]]:
    """Hysteresis: redraw 10% of request rates and count the read-copy bytes that change."""
    c = Cluster.of(n)
    b = affinity_place(w, c)
    rng = random.Random(seed)
    rates = [r for _, _, r in w.templates]
    p0 = adaptive_place(w, c, b, beta=beta, rates=rates)
    drifted = [r * rng.lognormvariate(0.0, 1.0) if rng.random() < 0.1 else r for r in rates]
    total = sum(w.size)
    out = []
    for h in hs:
        p1 = adaptive_place(w, c, b, beta=beta, rates=drifted, incumbent=p0.reads, h=h)
        added = removed = 0.0
        for u in range(w.units):
            a, z = set(p0.reads[u]), set(p1.reads[u])
            added += w.size[u] * len(z - a)
            removed += w.size[u] * len(a - z)
        m = evaluate(w, c, p1, sample=20_000, seed=seed)
        out.append(
            {
                "graph": w.name,
                "nodes": n,
                "h": h,
                "added_pct": 100 * added / total,
                "removed_pct": 100 * removed / total,
                "local_frac": m["local_frac"],
                "fanout_mean": m["fanout_mean"],
            }
        )
    return out


def markdown(rows: list[dict[str, object]]) -> str:
    head = "| " + " | ".join(c[0] for c in COLUMNS) + " |"
    sep = "|" + "|".join("---" for _ in COLUMNS) + "|"
    lines = [head, sep]
    for r in rows:
        cells = []
        for _, key, fmt in COLUMNS:
            v = r.get(key, "")
            cells.append(fmt.format(v) if v != "" else "")
        lines.append("| " + " | ".join(cells) + " |")
    return "\n".join(lines)


def root_table() -> str:
    nodes = [10, 100, 1000, 10_000]
    rates = [1.0, 100.0]
    out = []
    for p in (root_index.Params(voters=3), root_index.Params(voters=5)):
        out.append(f"\nroot index, V={p.voters}, q={p.readers_per_index}, m={p.miss_rate}/s per node")
        out.append(
            "| N | creates/s | msgs/create all | msgs/create cache | msgs/s all | msgs/s cache | "
            "cores all | cores cache | visible ms all | depth | member lookups/s cache |"
        )
        out.append("|" + "---|" * 11)
        for row in root_index.table(nodes, rates, p):
            out.append(
                "| {nodes} | {rate:g} | {all_msgs_per_create} | {cache_msgs_per_create} | {all_msgs_per_s:,.0f} | "
                "{cache_msgs_per_s:,.0f} | {all_cores:.4f} | {cache_cores:.4f} | {all_visible_ms:.2f} | "
                "{relay_depth} | {cache_member_in_per_s:,.1f} |".format(**row)
            )
        for r in rates:
            for m in (0.1, 1.0):
                q = root_index.Params(voters=p.voters, miss_rate=m)
                out.append(
                    f"crossover V={p.voters} rate={r:g}/s miss={m}/s/node: N >= {root_index.crossover(r, q)}"
                )
    return "\n".join(out)


def smoke() -> None:
    here = os.path.dirname(os.path.abspath(__file__))
    suite = unittest.defaultTestLoader.discover(here, pattern="test_*.py", top_level_dir=here)
    result = unittest.TextTestRunner(verbosity=1).run(suite)
    if not result.wasSuccessful():
        raise SystemExit(1)
    for w in (planted(8, 25, 0.3, 0.004), synthetic(0.002)):
        rows = run_one(w, 10, ALL, seed=1, sample=2000, beta=1.0)
        print(f"\n{w.name}: {w.units} units, {w.indexes} indexes, {len(w.edges)} edges, N=10")
        print(markdown(rows))
        for r in rows:
            for _, key, _ in COLUMNS:
                assert key in r, (r["strategy"], key)
        by = {r["strategy"]: r for r in rows}
        assert by["partition+local"]["local_frac"] == 1.0, by["partition+local"]
        assert by["spar"]["local_frac"] == 1.0, by["spar"]
        assert by["hash"]["read_copies_per_unit"] == 0.0
        print(json.dumps(drift(w, 10, 1, 1.0, [0.0, 0.25])))
    print(root_table())
    print("smoke ok")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--smoke", action="store_true", help="unit tests and small runs, in seconds")
    ap.add_argument("--graph", help="edge list (SNAP or WOSN, gzip ok)")
    ap.add_argument("--synthetic", type=float, help="many-indexes workload at this scale (1.0: 100k tenants)")
    ap.add_argument("--nodes", default="10,100,1000")
    ap.add_argument("--strategies", default=",".join(ALL))
    ap.add_argument("--beta", default="1", help="locality budgets, comma separated")
    ap.add_argument("--drift", default="", help="hysteresis values for the drift run, e.g. 0,0.25")
    ap.add_argument("--sample", type=int, default=20_000)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--root-index", action="store_true")
    a = ap.parse_args()
    if a.smoke:
        smoke()
        return
    if a.root_index:
        print(root_table())
        return
    if a.graph:
        w = load_edge_list(a.graph, seed=a.seed)
    elif a.synthetic:
        w = synthetic(a.synthetic, seed=a.seed)
    else:
        raise SystemExit("give --smoke, --graph, --synthetic or --root-index")
    print(
        f"{w.name}: {w.units} units, {w.indexes} indexes, {len(w.edges)} edges, {len(w.templates)} templates"
    )
    for n in [int(x) for x in a.nodes.split(",")]:
        for beta in [float(x) for x in a.beta.split(",")]:
            rows = run_one(w, n, a.strategies.split(","), a.seed, a.sample, beta)
            print(
                f"\n{w.name} N={n} domains={rows[0]['domains']} beta={beta:g} churn={rows[0]['leave']} node(s)"
            )
            print(markdown(rows))
            for r in rows:
                print("RESULT " + json.dumps(r))
        if a.drift:
            for r in drift(w, n, a.seed, float(a.beta.split(",")[0]), [float(x) for x in a.drift.split(",")]):
                print("DRIFT " + json.dumps(r))
        sys.stdout.flush()


if __name__ == "__main__":
    main()
