"""Summarizes vecbench JSON lines: per-vector cost and the exact-search crossover."""
from __future__ import annotations
import json, sys
from collections import defaultdict

rows = [json.loads(l) for l in open(sys.argv[1]) if l.startswith("{")]
by = defaultdict(dict)
for r in rows:
    by[(r["enc"], r["d"], r["mode"])][r["n"]] = r
print("## median us per query (hot | cold), 1 thread, top-10")
encs = ["f32_scalar", "f32", "f16", "int8", "binary"]
for d in (384, 768, 1536):
    print(f"\nd={d}")
    print("| enc | " + " | ".join(f"n={n:,}" for n in (100, 1000, 10000, 100000, 1000000)) + " | ns/vec (largest cold) | GB/s |")
    for e in encs:
        hot, cold = by.get((e, d, "hot"), {}), by.get((e, d, "cold"), {})
        cells = []
        for n in (100, 1000, 10000, 100000, 1000000):
            h, c = hot.get(n), cold.get(n)
            if not h and not c: cells.append("—"); continue
            cells.append(f"{h['median_us']:.1f}" + (f" \\| {c['median_us']:.1f}" if c else ""))
        big = max(list(cold.values()) + list(hot.values()), key=lambda r: r["n"]) if (cold or hot) else None
        print(f"| {e} | " + " | ".join(cells) + (f" | {big['ns_per_vec']:.2f} | {big['gbps']:.1f} |" if big else " | | |"))
print("\n## filtered: ns per scored vector (filter10, filter1) at n=100,000")
for d in (384, 768, 1536):
    for e in encs[1:]:
        f10 = by.get((e, d, "filter10"), {}).get(100000); f1 = by.get((e, d, "filter1"), {}).get(100000)
        hot = by.get((e, d, "hot"), {}).get(100000)
        if f10 and hot:
            print(f"d={d} {e}: full {hot['ns_per_vec']:.2f}, 10% {f10['ns_per_vec']:.2f}, 1% {f1['ns_per_vec']:.2f} ns/vec")
print("\n## crossover: largest n x d per query budget (cold, else hot), 1 thread")
for e in encs[1:]:
    for d in (384, 768, 1536):
        src = by.get((e, d, "cold"), {}) or {}
        hot = by.get((e, d, "hot"), {})
        merged = {**hot, **src}
        pts = sorted(merged.items())
        # ns per vector at the largest measured n (bandwidth-bound regime)
        big = pts[-1][1]
        npv = big["ns_per_vec"]
        out = []
        for budget_ms in (1, 5, 10):
            n = budget_ms * 1e6 / npv
            out.append(f"{budget_ms} ms: n≈{n:,.0f} (n×d≈{n*d:.2e})")
        print(f"{e} d={d} ({npv:.2f} ns/vec): " + "; ".join(out))
