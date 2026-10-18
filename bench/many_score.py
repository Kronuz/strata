#!/usr/bin/env python3
"""Scores a many-index write run (docs/perf/small-indexes.md, M2).

  many_score.py DIR DOCS_TOTAL

DIR holds t{trial}-{engine}.time (GNU time -v) and .bytes (the indexes' bytes on disk) per
unit. Per engine: the wall times, their median, CPU over wall, CPU per document (user plus
system over DOCS_TOTAL), peak RSS, and bytes on disk; then each strata build's throughput over
each other engine's (and over a later-named strata build's), per trial, as the median with its
95% bootstrap interval.
"""

from __future__ import annotations

import os
import random
import re
import statistics
import sys


def seconds(text: str) -> float:
    total = 0.0
    for part in text.split(":"):
        total = total * 60 + float(part)
    return total


def load(path: str) -> dict[str, float]:
    row: dict[str, float] = {}
    with open(path) as f:
        for line in f:
            value = line.rsplit(" ", 1)[-1].strip()
            if "Elapsed (wall clock)" in line:
                row["wall"] = seconds(value)
            elif "User time" in line:
                row["user"] = float(value)
            elif "System time" in line:
                row["sys"] = float(value)
            elif "Maximum resident set size" in line:
                row["rss"] = float(value) * 1024
    bytes_path = path.removesuffix(".time") + ".bytes"
    if os.path.exists(bytes_path):
        with open(bytes_path) as f:
            row["bytes"] = float(f.read().strip() or 0)
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
    root, docs = argv[1], float(argv[2])
    runs: dict[str, dict[int, dict[str, float]]] = {}
    for name in sorted(os.listdir(root)):
        m = re.fullmatch(r"t(\d+)-(.+)\.time", name)
        if m:
            row = load(os.path.join(root, name))
            if "wall" in row:
                runs.setdefault(m.group(2), {})[int(m.group(1))] = row
    print(
        "| Engine | Wall (s, per trial) | Median | CPU / wall | CPU per document | Peak RSS | Bytes on disk |"
    )
    print("|---|---|---:|---:|---:|---:|---:|")
    for e, trials in sorted(runs.items()):
        walls = [trials[t]["wall"] for t in sorted(trials)]
        cpu = [
            trials[t].get("user", 0) + trials[t].get("sys", 0) for t in sorted(trials)
        ]
        rss = statistics.median(trials[t].get("rss", 0) for t in trials) / 2**20
        size = statistics.median(trials[t].get("bytes", 0) for t in trials) / 2**20
        print(
            f"| {e} | {', '.join(f'{w:.1f}' for w in walls)} | {statistics.median(walls):.1f} | "
            f"{100 * statistics.median(c / w for c, w in zip(cpu, walls)):.0f}% | {1e6 * statistics.median(cpu) / docs:.1f} us "
            f"| {rss:.0f} MB | {size:.1f} MB |"
        )
    for s in sorted(e for e in runs if e.startswith("strata")):
        for e in sorted(runs):
            if e == s or (e.startswith("strata") and e < s):
                continue
            common = sorted(set(runs[s]) & set(runs[e]))
            if common:
                med, lo, hi = interval(
                    [runs[e][t]["wall"] / runs[s][t]["wall"] for t in common]
                )
                print(
                    f"{s}'s throughput over {e}'s: {med:.3f}x [{lo:.3f}, {hi:.3f}] over {len(common)} trials"
                )


if __name__ == "__main__":
    main(sys.argv)
