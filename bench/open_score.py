#!/usr/bin/env python3
"""Scores an open-many A/B (docs/perf/small-indexes.md).

  open_score.py DIR

DIR holds {docs}-{variant}-{round}.txt, each `strata_engine open-many` output. Per size and
variant: the open p50 and mean per round, the anonymous bytes per idle open index, and the
first use p50; then, per size and for each variant against the next (and the first against
the last), the earlier one's time over the later one's per round (open p50, open mean, and
open plus first use p50, rounds paired), as the median with its 95% bootstrap interval: above
1 means the later variant is faster.
"""

from __future__ import annotations

import itertools
import os
import random
import re
import statistics
import sys

OPEN = re.compile(r"^open us: mean ([\d.]+) p50 ([\d.]+)")
HEAP = re.compile(r"^per index: rss \d+ B, anon (-?\d+) B")
FIRST = re.compile(r"^first use us: mean [\d.]+ p50 ([\d.]+)")


def load(path: str) -> dict[str, float]:
    row: dict[str, float] = {}
    with open(path) as f:
        for line in f:
            if m := OPEN.match(line):
                row["mean"], row["p50"] = float(m.group(1)), float(m.group(2))
            elif m := HEAP.match(line):
                row["anon"] = float(m.group(1))
            elif m := FIRST.match(line):
                row["first"] = float(m.group(1))
    return row


def interval(
    values: list[float], reps: int = 10000, seed: int = 2026
) -> tuple[float, float, float]:
    rng = random.Random(seed)
    meds = sorted(
        statistics.median(rng.choices(values, k=len(values))) for _ in range(reps)
    )
    return (
        statistics.median(values),
        meds[int(0.025 * reps)],
        meds[int(0.975 * reps) - 1],
    )


def main(argv: list[str]) -> None:
    root = argv[1]
    runs: dict[int, dict[str, dict[int, dict[str, float]]]] = {}
    for name in os.listdir(root):
        m = re.fullmatch(r"(\d+)-([a-z])-(\d+)\.txt", name)
        if m:
            row = load(os.path.join(root, name))
            if "p50" in row:
                docs, variant, trial = int(m.group(1)), m.group(2), int(m.group(3))
                runs.setdefault(docs, {}).setdefault(variant, {})[trial] = row
    print(
        "| Documents | Variant | Open p50 per round (µs) | Open mean per round (µs) | Anonymous per idle index | First use p50 |"
    )
    print("|---:|---|---|---|---:|---:|")
    for docs, variants in sorted(runs.items()):
        for v, trials in sorted(variants.items()):
            rows = [trials[t] for t in sorted(trials)]
            p50 = ", ".join(f"{r['p50']:.0f}" for r in rows)
            mean = ", ".join(f"{r['mean']:.0f}" for r in rows)
            anon = statistics.median(r.get("anon", 0) for r in rows)
            first = statistics.median(r.get("first", 0) for r in rows)
            print(f"| {docs} | {v} | {p50} | {mean} | {anon:.0f} B | {first:.0f} µs |")
    for docs, variants in sorted(runs.items()):
        order = sorted(variants)
        pairs = list(itertools.pairwise(order))
        if len(order) > 2:
            pairs.append((order[0], order[-1]))
        for x, y in pairs:
            common = sorted(set(variants[x]) & set(variants[y]))
            for stat in ("p50", "mean", "p50+first"):

                def value(row: dict[str, float], stat: str = stat) -> float:
                    if stat == "p50+first":
                        return row["p50"] + row.get("first", 0)
                    return row[stat]

                med, lo, hi = interval(
                    [value(variants[x][t]) / value(variants[y][t]) for t in common]
                )
                print(
                    f"{docs} documents, open {stat}: {x} over {y} {med:.3f}x [{lo:.3f}, {hi:.3f}] over {len(common)} rounds"
                )


if __name__ == "__main__":
    main(sys.argv)
