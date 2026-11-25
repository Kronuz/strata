#!/usr/bin/env python3
"""Search cost of sorted runs (rocksdb-parity msg-1): a corpus of text documents keyed in order and
its term, phrase and AND queries, then the per-side latency table of search-runs.sh's timings.

  search_runs.py corpus OUT DOCS   corpus.json (ids zero-padded, so the load is in key order),
                                   term.txt, phrase.txt and and.txt (the harness's JSON lines)
  search_runs.py report OUT        per command, set and side: p50 and p99 over queries of each
                                   query's median over rounds, their ratios to A, the geometric
                                   mean of per-query ratios to A with a 95% bootstrap interval
                                   (A2 against A is the A/A), and result mismatches against A
"""

from __future__ import annotations

import glob
import itertools
import math
import os
import random
import statistics
import sys
from collections import defaultdict

SETS = ("term", "phrase", "and")
QUERIES = 200


def corpus(out: str, docs: int) -> None:
    os.makedirs(out, exist_ok=True)
    r = random.Random(20261125)
    vocab = [f"w{i}" for i in range(30000)]
    weights = list(
        itertools.accumulate(1.0 / (i + 1) ** 1.05 for i in range(len(vocab)))
    )
    pairs: list[tuple[str, str]] = []
    with open(os.path.join(out, "corpus.json"), "w") as f:
        for n in range(docs):
            words = r.choices(vocab, cum_weights=weights, k=r.randint(20, 60))
            if r.random() < 4 * QUERIES / docs:
                i = r.randrange(len(words) - 1)
                pairs.append((words[i], words[i + 1]))
            f.write(f'{{"id": "{n:09d}", "text": "{" ".join(words)}"}}\n')

    def word() -> str:  # log-uniform rank: every frequency band alike
        return vocab[min(len(vocab) - 1, int(len(vocab) ** r.random()))]

    sets = {
        "term": [word() for _ in range(QUERIES)],
        "phrase": [f'"{a} {b}"' for a, b in r.sample(pairs, min(QUERIES, len(pairs)))],
        "and": [f"+{word()} +{word()}" for _ in range(QUERIES)],
    }
    for name, qs in sets.items():
        with open(os.path.join(out, f"{name}.txt"), "w") as f:
            f.writelines(
                '{"query": "' + q.replace('"', '\\"') + '", "tags": ["' + name + '"]}\n'
                for q in qs
            )


def pct(xs: list[float], p: float) -> float:
    xs = sorted(xs)
    return xs[min(len(xs) - 1, max(0, math.ceil(p / 100 * len(xs)) - 1))]


def report(out: str) -> None:
    # times[(side, cmd, set)][query] = per-round times; results likewise
    times: dict[tuple[str, str, str], dict[str, list[float]]] = defaultdict(
        lambda: defaultdict(list)
    )
    results: dict[tuple[str, str, str], dict[str, str]] = defaultdict(dict)
    for path in glob.glob(os.path.join(out, "t", "*.tsv")):
        _, side, cmd, qset = os.path.basename(path)[:-4].split("-", 3)
        with open(path) as f:
            for line in f:
                t, res, q = line.rstrip("\n").split("\t", 2)
                times[(side, cmd, qset)][q].append(float(t))
                results[(side, cmd, qset)][q] = res
    rng = random.Random(7)

    def ratio(a: dict[str, float], b: dict[str, float]) -> str:
        logs = [
            math.log(max(b[q], 0.1) / max(a[q], 0.1)) for q in sorted(set(a) & set(b))
        ]
        gm = math.exp(sum(logs) / len(logs))
        boot = sorted(
            math.exp(sum(rng.choice(logs) for _ in logs) / len(logs))
            for _ in range(1000)
        )
        return f"{gm:.3f} [{boot[25]:.3f}, {boot[974]:.3f}]"

    print(
        "| Command | Set | Side | n | p50 µs | p99 µs | p50 /A | p99 /A | per query /A [95%] | Mismatches |"
    )
    print("|---|---|---|---:|---:|---:|---:|---:|---|---:|")
    sides = sorted({k[0] for k in times}, key=lambda s: (s != "a", s))
    for cmd in ("TOP_10", "COUNT"):
        for qset in SETS:
            med = {
                s: {q: statistics.median(v) for q, v in times[(s, cmd, qset)].items()}
                for s in sides
                if times.get((s, cmd, qset))
            }
            if "a" not in med:
                continue
            base = med["a"]
            a50, a99 = pct(list(base.values()), 50), pct(list(base.values()), 99)
            for s, m in med.items():
                p50, p99 = pct(list(m.values()), 50), pct(list(m.values()), 99)
                bad = sum(
                    results[(s, cmd, qset)].get(q) != res
                    for q, res in results[("a", cmd, qset)].items()
                )
                rel = "" if s == "a" else ratio(base, m)
                print(
                    f"| {cmd} | {qset} | {s} | {len(m)} | {p50:.1f} | {p99:.1f}"
                    f" | {p50 / a50:.3f} | {p99 / a99:.3f} | {rel} | {bad} |"
                )


if __name__ == "__main__":
    if sys.argv[1] == "corpus":
        corpus(sys.argv[2], int(sys.argv[3]))
    else:
        report(sys.argv[2])
