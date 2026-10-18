#!/usr/bin/env python3
"""Scores a bench/index_ab.sh run.

  index_ab.py DIR

DIR holds t{trial}-{variant}.time (GNU time -v), .bytes, and .log per unit. Per variant:
the wall times, their median and spread, CPU over wall, peak RSS, bytes, and strata's
segments. Then each strata variant's throughput (a to d) over A's and over the anchor (the
fastest variant that is not strata), per trial, as the median with its 95% bootstrap
interval (index_gate.py's).
"""

from __future__ import annotations

import os
import re
import statistics
import sys

from index_gate import interval, load

STRATA = ("a", "b", "c", "d")  # strata variants; any other name is a baseline engine


def cpu_share(path: str) -> float:
    with open(path) as f:
        for line in f:
            if "Percent of CPU" in line:
                return float(line.rsplit(" ", 1)[1].strip().rstrip("%")) / 100
    return 0.0


def segments(path: str) -> str:
    with open(path) as f:
        found = re.findall(r"(\d+) segments", f.read())
    return found[-1] if found else "-"


def main(argv: list[str]) -> None:
    root = argv[1]
    runs: dict[str, dict[int, dict[str, float]]] = {}
    for name in sorted(os.listdir(root)):
        m = re.fullmatch(r"t(\d+)-(.+)\.time", name)
        if not m or not os.path.exists(os.path.join(root, name[:-5] + ".bytes")):
            continue
        row: dict[str, float] = dict(load(os.path.join(root, name)))
        row["cpu"] = cpu_share(os.path.join(root, name))
        with open(os.path.join(root, name[:-5] + ".bytes")) as f:
            row["bytes"] = float(f.read().split()[0])
        runs.setdefault(m.group(2), {})[int(m.group(1))] = row
    print(
        "| Variant | Wall (s, per trial) | Median | Spread | CPU / wall | Peak RSS | Bytes | Segments |"
    )
    print("|---|---|---:|---:|---:|---:|---:|---:|")
    for v, trials in runs.items():
        times = [trials[t]["time"] for t in sorted(trials)]
        med = statistics.median(times)
        spread = (max(times) - min(times)) / med
        cpu = statistics.median(r["cpu"] for r in trials.values())
        rss = max(r.get("rss", 0) for r in trials.values())
        first = min(trials)
        seg = segments(os.path.join(root, f"t{first}-{v}.log")) if v in STRATA else "-"
        print(
            f"| {v} | {', '.join(f'{x:.1f}' for x in times)} | {med:.1f} | {100 * spread:.1f}% | "
            f"{100 * cpu:.0f}% | {rss / 2**30:.2f} GiB | {trials[first]['bytes'] / 1e6:.1f} MB | {seg} |"
        )

    def ratio(of: str, over: str) -> str:
        """The throughput of `of` over that of `over`, per trial."""
        common = sorted(set(runs[of]) & set(runs[over]))
        if not common:
            return "no common trials"
        med, lo, hi = interval(
            [runs[over][t]["time"] / runs[of][t]["time"] for t in common]
        )
        return f"{med:.3f}x [{lo:.3f}, {hi:.3f}] over {len(common)} trials"

    print()
    mine = [v for v in STRATA if v in runs]
    for v in mine[1:]:
        if "a" in runs:
            print(f"{v.upper()}'s throughput over A's: {ratio(v, 'a')}")
    # The anchor: the fastest engine that is not a strata variant.
    anchors = sorted(
        (v for v in runs if v not in STRATA),
        key=lambda v: statistics.median(r["time"] for r in runs[v].values()),
    )
    if anchors:
        for v in mine:
            print(f"{v.upper()}'s throughput over {anchors[0]}: {ratio(v, anchors[0])}")


if __name__ == "__main__":
    main(sys.argv)
