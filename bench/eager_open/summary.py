#!/usr/bin/env python3
"""Summarizes ab.sh row logs (docs/perf/eager-open.md): per side and arm, the median over runs and
the [min, max] range of each meter, as a Markdown table.

usage: summary.py LOG...
"""

from __future__ import annotations

import re
import statistics
import sys
from collections import defaultdict

HEAD = re.compile(r"^== (\S+) eager_open open \S+/(\S+) (\d+) (\S+) query")
METERS = re.compile(
    r"^(after open|after query 1)\s+rss\s+(-?\d+) dirty\s+(-?\d+) anon\s+-?\d+ file\s+(-?\d+) vsize\s+(-?\d+) B/index \| fds ([\d.]+) maps ([\d.]+)"
)
PCT = re.compile(
    r"^(open|first query|second query)\s+p50\s+([\d.]+) us p99\s+([\d.]+) us"
)
CPU = re.compile(r"^open cpu [\d.]+ s \(([\d.]+) us/index\)")
HASH = re.compile(r"^query hash (\S+)")


def parse(
    paths: list[str],
) -> tuple[dict[tuple[str, str, str, str], dict[str, list[float]]], set[str]]:
    runs: dict[tuple[str, str, str, str], dict[str, list[float]]] = defaultdict(
        lambda: defaultdict(list)
    )
    hashes: set[str] = set()
    key: tuple[str, str, str, str] | None = None
    for path in paths:
        with open(path) as f:
            for line in f:
                if m := HEAD.match(line):
                    key = (m.group(2), m.group(3), m.group(4), m.group(1))
                    continue
                if key is None:
                    continue
                r = runs[key]
                if m := METERS.match(line):
                    tag = "open" if m.group(1) == "after open" else "q1"
                    for name, v in zip(
                        ("rss", "dirty", "file", "vsize", "fds", "maps"), m.groups()[1:]
                    ):
                        r[f"{tag}.{name}"].append(float(v))
                elif m := PCT.match(line):
                    tag = {"open": "open", "first query": "q1", "second query": "q2"}[
                        m.group(1)
                    ]
                    r[f"{tag}.p50"].append(float(m.group(2)))
                    r[f"{tag}.p99"].append(float(m.group(3)))
                elif m := CPU.match(line):
                    r["open.cpu"].append(float(m.group(1)))
                elif m := HASH.match(line):
                    hashes.add(f"{key[0]} x{key[1]}: {m.group(1)}")
    return runs, hashes


def cell(v: list[float], scale: float = 1.0, digits: int = 0) -> str:
    if not v:
        return "-"
    s = [x / scale for x in v]
    med = statistics.median(s)
    return f"{med:,.{digits}f} [{min(s):,.{digits}f}-{max(s):,.{digits}f}]"


def main(paths: list[str]) -> None:
    runs, hashes = parse(paths)
    cols = [
        ("open p50 µs", "open.p50", 1, 0),
        ("open p99 µs", "open.p99", 1, 0),
        ("open CPU µs", "open.cpu", 1, 0),
        ("dirty KB", "open.dirty", 1024, 1),
        ("resident KB", "open.rss", 1024, 1),
        ("file-backed KB", "open.file", 1024, 1),
        ("mapped KB", "open.vsize", 1024, 0),
        ("fds", "open.fds", 1, 2),
        ("maps", "open.maps", 1, 2),
        ("1st query p50 µs", "q1.p50", 1, 0),
        ("1st query p99 µs", "q1.p99", 1, 0),
        ("dirty after query KB", "q1.dirty", 1024, 1),
    ]
    print(
        "| fixture | count | arm | side | runs | "
        + " | ".join(c[0] for c in cols)
        + " |"
    )
    print("|" + "---|" * (5 + len(cols)))
    for (fixture, count, arm, side), r in sorted(
        runs.items(), key=lambda kv: (kv[0][0], int(kv[0][1]), kv[0][2], kv[0][3])
    ):
        n = len(r["open.p50"])
        print(
            f"| {fixture} | {count} | {arm} | {side} | {n} | "
            + " | ".join(cell(r[k], s, d) for _, k, s, d in cols)
            + " |"
        )
    for h in sorted(hashes):
        print(f"hash {h}")


if __name__ == "__main__":
    main(sys.argv[1:])
