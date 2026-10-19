#!/usr/bin/env python3
"""Median per window across rounds of research_ingest / research_rocks outputs.

    summarize.py [--every N] [--cols a,b,c] FILE...

Files with the same name up to '-rK.tsv' form one configuration; the table prints, for
every N-th window, the median over rounds of each chosen column (service p99/p999 and
query p99 in ms, segments or L0 files, merged or compacted MB, stall ms)."""

from __future__ import annotations

import re
import statistics
import sys
from collections import defaultdict

DEFAULT_COLS = ["done", "s_p99", "s_p999", "w_p99", "q_p50", "q_p99", "segs", "l0", "files", "pending_mb", "excess", "merge_mb", "compact_mb", "stall_ms", "rate_cap", "width"]
MS = {"s_p50", "s_p99", "s_p999", "s_max", "w_p50", "w_p99", "w_p999", "w_max", "q_p50", "q_p99", "q_p999", "q_max"}


def load(path: str) -> tuple[list[str], list[dict[str, float]], list[str]]:
    header: list[str] = []
    rows: list[dict[str, float]] = []
    notes: list[str] = []
    with open(path) as f:
        for line in f:
            line = line.rstrip("\n")
            if line.startswith("#"):
                notes.append(line)
                continue
            if line.startswith("win\t"):
                header = line.split("\t")
                continue
            parts = line.split("\t")
            if len(parts) != len(header):
                continue
            rows.append({h: float(v) for h, v in zip(header, parts)})
    return header, rows, notes


def main(argv: list[str]) -> int:
    every = 1
    cols = DEFAULT_COLS
    files: list[str] = []
    i = 0
    while i < len(argv):
        if argv[i] == "--every":
            every = int(argv[i + 1])
            i += 2
        elif argv[i] == "--cols":
            cols = argv[i + 1].split(",")
            i += 2
        else:
            files.append(argv[i])
            i += 1
    groups: dict[str, list[str]] = defaultdict(list)
    for p in files:
        groups[re.sub(r"-r\d+\.tsv$", "", p)].append(p)
    for name, paths in sorted(groups.items()):
        runs = [load(p) for p in sorted(paths)]
        header = runs[0][0]
        present = [c for c in cols if c in header]
        print(f"\n## {name} ({len(runs)} rounds)\n")
        print("| win | " + " | ".join(f"{c} (ms)" if c in MS else c for c in present) + " |")
        print("|---|" + "---:|" * len(present))
        nwin = min(len(r[1]) for r in runs)
        for w in range(0, nwin, every):
            cells = []
            for c in present:
                vals = [r[1][w][c] for r in runs]
                m = statistics.median(vals)
                if c in MS:
                    cells.append(f"{m / 1000:.1f}")
                elif c in ("done",):
                    # documents admitted in this window: the median of the per-window deltas
                    deltas = [r[1][w][c] - (r[1][w - 1][c] if w else 0) for r in runs]
                    cells.append(f"{statistics.median(deltas):.0f}")
                else:
                    cells.append(f"{m:.0f}" if abs(m) >= 10 else f"{m:.1f}")
            print(f"| {int(runs[0][1][w]['win'])} | " + " | ".join(cells) + " |")
        for r in runs:
            for n in r[2]:
                if n.startswith("# end") or n.startswith("# gap") or n.startswith("# pause") or n.startswith("# checkpoints"):
                    print(n)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
