#!/usr/bin/env python3
"""Summarizes ab.sh row logs (docs/perf/eager-open.md): per side and arm, the median over runs and
the [min, max] range of each meter, as a Markdown table.

usage: summary.py [--within BASE_ARM:PROTO_ARM] LOG...
       summary.py --identity LOG

--within: exits 1 unless, per fixture, every query hash agrees and proto's PROTO_ARM median of
open p50, open CPU, dirty and resident is at most the top of the A/A band, the range of base's and
aa's BASE_ARM runs. --identity: exits 1 unless every good fixture gives one hash and no refusal on
every side and arm, and on the corrupt one (d1000s4c) proto refuses every index, the eager arms at
open and lazy at its first read, with corrupt_segment.
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
IDENTITY_HEAD = re.compile(r"^== (\S+) eager_open identity \S+/(\S+) (\d+) (\S+)")
IDENTITY = re.compile(
    r"^identity (\d+) indexes: hash (\S+), refused at open (\d+), at query (\d+)"
)
WHAT = re.compile(r"^  (\d+) x (open|query): (.*)")
RULE = [
    ("open p50 µs", "open.p50"),
    ("open CPU µs", "open.cpu"),
    ("dirty B", "open.dirty"),
    ("resident B", "open.rss"),
]


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


def within(
    runs: dict[tuple[str, str, str, str], dict[str, list[float]]],
    hashes: set[str],
    base_arm: str,
    proto_arm: str,
) -> bool:
    ok = True
    by_fixture: dict[str, set[str]] = defaultdict(set)
    for h in hashes:
        fixture, value = h.split(": ")
        by_fixture[fixture].add(value)
    for fixture, values in sorted(by_fixture.items()):
        if len(values) != 1:
            print(f"FAIL {fixture}: query hashes differ: {sorted(values)}")
            ok = False
    fixtures = sorted({(k[0], k[1]) for k in runs})
    for fixture, count in fixtures:
        base = runs.get((fixture, count, base_arm, "base"), {})
        aa = runs.get((fixture, count, base_arm, "aa"), {})
        proto = runs.get((fixture, count, proto_arm, "proto"), {})
        if not base or not aa or not proto:
            print(f"FAIL {fixture} x{count}: missing base, aa or proto runs")
            ok = False
            continue
        for label, k in RULE:
            band = base[k] + aa[k]
            med = statistics.median(proto[k])
            verdict = "ok" if med <= max(band) else "FAIL"
            ok = ok and verdict == "ok"
            print(
                f"{verdict} {fixture} x{count} {label}: proto {proto_arm} {med:,.1f} against "
                f"base/aa {base_arm} [{min(band):,.1f}-{max(band):,.1f}] "
                f"(base {statistics.median(base[k]):,.1f}, aa {statistics.median(aa[k]):,.1f})"
            )
    return ok


def identity(path: str) -> bool:
    rows: dict[tuple[str, str, str], tuple[str, int, int, list[str]]] = {}
    key: tuple[str, str, str] | None = None
    with open(path) as f:
        for line in f:
            if m := IDENTITY_HEAD.match(line):
                key = (m.group(2), m.group(1), m.group(4))
            elif key and (m := IDENTITY.match(line)):
                rows[key] = (m.group(2), int(m.group(3)), int(m.group(4)), [])
            elif key and key in rows and (m := WHAT.match(line)):
                rows[key][3].append(f"{m.group(2)}: {m.group(3)}")
    ok = bool(rows)
    for fixture in sorted({k[0] for k in rows}):
        mine = {k: v for k, v in rows.items() if k[0] == fixture}
        for (_, side, arm), (h, at_open, at_query, whats) in sorted(mine.items()):
            print(
                f"{fixture} {side} {arm}: hash {h}, refused at open {at_open}, at query {at_query} {whats}"
            )
        if fixture != "d1000s4c":
            if len({v[0] for v in mine.values()}) != 1 or any(
                v[1] or v[2] for v in mine.values()
            ):
                print(f"FAIL {fixture}: hashes differ or an index was refused")
                ok = False
            continue
        for (_, side, arm), (h, at_open, at_query, whats) in mine.items():
            if side != "proto":
                continue
            want_open = arm != "lazy"
            refused = at_open if want_open else at_query
            other = at_query if want_open else at_open
            named = all("corrupt segment" in w for w in whats)
            if refused == 0 or other or not named:
                print(
                    f"FAIL {fixture} proto {arm}: refused at open {at_open}, at query {at_query}"
                )
                ok = False
    print("identity: " + ("ok" if ok else "FAIL"))
    return ok


def main(paths: list[str]) -> None:
    if paths[:1] == ["--identity"]:
        sys.exit(0 if identity(paths[1]) else 1)
    rule: tuple[str, str] | None = None
    if paths[:1] == ["--within"]:
        base_arm, proto_arm = paths[1].split(":")
        rule = (base_arm, proto_arm)
        paths = paths[2:]
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
    if rule:
        sys.exit(0 if within(runs, hashes, *rule) else 1)


if __name__ == "__main__":
    main(sys.argv[1:])
