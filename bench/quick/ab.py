#!/usr/bin/env python3
"""Scores quick/ab.sh output: per command, the geometric mean over queries of B's best time
over A's (best over rounds), with a 95% bootstrap interval over queries, the per-round ratios
for spread, and result mismatches (counts must agree).

Two tables: one row per query set, then one per tag over both sets. A tag without a colon is a
class, as gate.py scores it; a tag with one is a stratum (union:num_tokens_3, term:rare), listed
after its class. Tags come from OUT/queries.txt and OUT/terms.txt, which ab.sh copies there, or
else from $QUERIES and $TERMS as ab.sh resolves them.

  ab.py OUT
  ab.py OUT --aa AA_OUT

With --aa, the run-level table (the lead's ruling of 2026-10-19, docs/perf/ab-run-level.md):
each round of ab.sh is a run; a row's value in a run is the median over its queries of B's
time over A's in that run; the row's ratio is the geometric mean of its runs' values, with a
95% t-interval over runs. AA_OUT is the same session's A/A (A against itself), scored the same
way, and its interval is the row's noise band. A row is a loss only when its lower bound
clears both 1.03 and the A/A band's upper bound, and a win only when its upper bound is below
both 1.0 and the band's lower bound with the ratio at most 0.97.
"""

from __future__ import annotations

import glob
import json
import math
import os
import random
import sys
from collections import defaultdict
from collections.abc import Hashable
from typing import TypedDict, TypeVar

COMMANDS = ["TOP_10", "TOP_100", "TOP_1000", "TOP_100_COUNT", "COUNT"]
SETS = ["queries", "terms"]
# Where ab.sh reads each set: its environment variable and default.
SOURCES = {
    "queries": ("QUERIES", "~/bench/search-benchmark-game/queries.txt"),
    "terms": ("TERMS", "~/bench/search-benchmark-game/terms-stratified.txt"),
}

K = TypeVar("K", bound=Hashable)
Best = dict[tuple[str, str, str], dict[str, float]]
Rounds = dict[tuple[str, str, str, str], dict[str, float]]
Results = dict[tuple[str, str, str], dict[str, str]]
Tags = dict[str, tuple[str, dict[str, list[str]]]]


class Row(TypedDict):
    command: str
    row: str
    n: int
    gm: float
    lo: float
    hi: float
    rounds: list[float]
    mismatches: int


def load(out: str) -> tuple[Best, Rounds, Results]:
    """Each side's best time per command, set, and query; every round's times; the results."""
    best: Best = defaultdict(dict)
    rounds: Rounds = {}
    results: Results = defaultdict(dict)
    for path in sorted(glob.glob(os.path.join(out, "*.tsv"))):
        r, side, cmd, qset = os.path.basename(path)[:-4].split("-", 3)
        times: dict[str, float] = {}
        with open(path) as f:
            for line in f:
                t, res, q = line.rstrip("\n").split("\t", 2)
                times[q] = float(t)
                b = best[(side, cmd, qset)]
                b[q] = min(b.get(q, math.inf), float(t))
                results[(side, cmd, qset)][q] = res
        rounds[(r, side, cmd, qset)] = times
    return best, rounds, results


def query_key(line: str) -> str | None:
    """The query as strata_engine bench prints it (strata_engine.cc:552-555): the JSON string,
    each backslash taking the next character as is."""
    line = line.rstrip("\n")
    at = line.find('"query": "')
    if at < 0:
        return None
    chars: list[str] = []
    i = at + 10
    while i < len(line) and line[i] != '"':
        if line[i] == "\\":
            i += 1
        chars.append(line[i] if i < len(line) else "\0")
        i += 1
    return "".join(chars)


def load_tags(out: str) -> Tags:
    """{set: (its file, {query: tags})} for each set whose query file exists."""
    tags: Tags = {}
    for qset in SETS:
        env, default = SOURCES[qset]
        path = os.path.join(out, qset + ".txt")
        if not os.path.exists(path):
            path = os.path.expanduser(os.environ.get(env, default))
        if not os.path.exists(path):
            continue
        by: dict[str, list[str]] = {}
        with open(path) as f:
            for line in f:
                key = query_key(line)
                if key is not None:
                    by[key] = json.loads(line).get("tags", [])
        tags[qset] = (path, by)
    return tags


def score(
    command: str,
    label: str,
    qs: list[K],
    a: dict[K, float],
    b: dict[K, float],
    per_round: list[tuple[dict[K, float], dict[K, float]]],
    res_a: dict[K, str],
    res_b: dict[K, str],
    rng: random.Random,
) -> Row:
    """One row over the queries qs; a round counts when both sides timed every one of them."""
    logs = [math.log(max(b[q], 0.1) / max(a[q], 0.1)) for q in qs]
    boot = sorted(
        math.exp(sum(rng.choice(logs) for _ in logs) / len(logs)) for _ in range(1000)
    )
    spread = [
        math.exp(sum(math.log(max(rb[q], 0.1) / max(ra[q], 0.1)) for q in qs) / len(qs))
        for ra, rb in per_round
        if all(q in ra and q in rb for q in qs)
    ]
    return {
        "command": command,
        "row": label,
        "n": len(qs),
        "gm": math.exp(sum(logs) / len(logs)),
        "lo": boot[25],
        "hi": boot[974],
        "rounds": spread,
        "mismatches": sum(1 for q in qs if res_a[q] != res_b[q]),
    }


def round_ids(rounds: Rounds) -> list[str]:
    return sorted({k[0] for k in rounds}, key=int)


def set_rows(best: Best, rounds: Rounds, results: Results) -> list[Row]:
    """Per command and query set, as ab.py always scored them (one seed for every row)."""
    rng = random.Random(7)
    rows: list[Row] = []
    for cmd in COMMANDS:
        for qset in SETS:
            a, b = best.get(("a", cmd, qset)), best.get(("b", cmd, qset))
            if not a or not b:
                continue
            qs = sorted(set(a) & set(b))
            if not qs:
                continue
            per_round = [
                (
                    rounds.get((r, "a", cmd, qset), {}),
                    rounds.get((r, "b", cmd, qset), {}),
                )
                for r in round_ids(rounds)
            ]
            res_a, res_b = results[("a", cmd, qset)], results[("b", cmd, qset)]
            rows.append(score(cmd, qset, qs, a, b, per_round, res_a, res_b, rng))
    return rows


def class_rows(
    best: Best, rounds: Rounds, results: Results, tags: Tags
) -> tuple[list[Row], dict[str, int]]:
    """Per command and tag over both sets, each query keyed by (set, query); and per set, how
    many queries both sides timed that have no tags."""
    rows: list[Row] = []
    untagged: dict[str, set[str]] = defaultdict(set)
    ids = round_ids(rounds)
    for cmd in COMMANDS:
        a: dict[tuple[str, str], float] = {}
        b: dict[tuple[str, str], float] = {}
        res_a: dict[tuple[str, str], str] = {}
        res_b: dict[tuple[str, str], str] = {}
        per_round: list[
            tuple[dict[tuple[str, str], float], dict[tuple[str, str], float]]
        ]
        per_round = [({}, {}) for _ in ids]
        for qset in SETS:
            for side, times, res in (("a", a, res_a), ("b", b, res_b)):
                for q, t in best.get((side, cmd, qset), {}).items():
                    times[(qset, q)] = t
                    res[(qset, q)] = results[(side, cmd, qset)][q]
            for i, r in enumerate(ids):
                for side, merged in (("a", per_round[i][0]), ("b", per_round[i][1])):
                    for q, t in rounds.get((r, side, cmd, qset), {}).items():
                        merged[(qset, q)] = t
        by: dict[str, list[tuple[str, str]]] = defaultdict(list)
        for key in sorted(set(a) & set(b)):
            qset, q = key
            known = tags[qset][1].get(q) if qset in tags else None
            if not known:
                untagged[qset].add(q)
                continue
            for tag in dict.fromkeys(known):
                by[tag].append(key)
        for tag in sorted(by):
            # Seeded per row, so a row's interval does not depend on which others exist.
            rng = random.Random(f"{cmd}\t{tag}")
            rows.append(score(cmd, tag, by[tag], a, b, per_round, res_a, res_b, rng))
    return rows, {qset: len(qs) for qset, qs in untagged.items()}


def table(label: str, rows: list[Row]) -> None:
    print(f"| Command | {label} | n | B/A | 95% | Rounds | Mismatches |")
    print("|---|---|---:|---:|---|---|---:|")
    for r in rows:
        spread = " ".join(f"{x:.2f}" for x in r["rounds"])
        print(
            f"| `{r['command']}` | {r['row']} | {r['n']} | {r['gm']:.3f} "
            f"| {r['lo']:.3f}–{r['hi']:.3f} | {spread} | {r['mismatches']} |"
        )


# Two-sided 95% t quantiles by degrees of freedom (runs - 1); 1.96 beyond the table.
T975 = [
    12.706,
    4.303,
    3.182,
    2.776,
    2.571,
    2.447,
    2.365,
    2.306,
    2.262,
    2.228,
    2.201,
    2.179,
    2.160,
    2.145,
    2.131,
    2.120,
    2.110,
    2.101,
    2.093,
    2.086,
    2.080,
    2.074,
    2.069,
    2.064,
    2.060,
    2.056,
    2.052,
    2.048,
    2.045,
    2.042,
]


class RunRow(TypedDict):
    command: str
    row: str
    n: int
    ratio: float
    lo: float
    hi: float
    runs: list[float]


def median(xs: list[float]) -> float:
    ys = sorted(xs)
    m = len(ys) // 2
    return ys[m] if len(ys) % 2 else (ys[m - 1] + ys[m]) / 2


def run_interval(values: list[float]) -> tuple[float, float, float]:
    """The geometric mean of per-run values and its 95% t-interval over runs."""
    logs = [math.log(v) for v in values]
    mean = sum(logs) / len(logs)
    if len(logs) < 2:
        return math.exp(mean), 0.0, math.inf
    sd = math.sqrt(sum((x - mean) ** 2 for x in logs) / (len(logs) - 1))
    t = T975[len(logs) - 2] if len(logs) - 2 < len(T975) else 1.96
    half = t * sd / math.sqrt(len(logs))
    return math.exp(mean), math.exp(mean - half), math.exp(mean + half)


def run_rows(rounds: Rounds, tags: Tags) -> list[RunRow]:
    """Per command and tag over both sets: each run's median over the tag's queries of B/A in
    that run, then the interval over runs."""
    rows: list[RunRow] = []
    ids = round_ids(rounds)
    for cmd in COMMANDS:
        values: dict[str, list[float]] = defaultdict(list)
        sizes: dict[str, int] = {}
        for r in ids:
            ratios: dict[str, list[float]] = defaultdict(list)
            for qset in SETS:
                ra, rb = (
                    rounds.get((r, "a", cmd, qset), {}),
                    rounds.get((r, "b", cmd, qset), {}),
                )
                for q in sorted(set(ra) & set(rb)):
                    known = tags[qset][1].get(q) if qset in tags else None
                    for tag in dict.fromkeys(known or []):
                        ratios[tag].append(max(rb[q], 0.1) / max(ra[q], 0.1))
            for tag, xs in ratios.items():
                values[tag].append(median(xs))
                sizes[tag] = max(sizes.get(tag, 0), len(xs))
        for tag in sorted(values):
            ratio, lo, hi = run_interval(values[tag])
            rows.append(
                {
                    "command": cmd,
                    "row": tag,
                    "n": sizes[tag],
                    "ratio": ratio,
                    "lo": lo,
                    "hi": hi,
                    "runs": values[tag],
                }
            )
    return rows


def verdict(row: RunRow, band: RunRow | None, guard: float = 1.03) -> str:
    """A loss clears 1.03 and the A/A band with its lower bound; a win clears 1.0 and the band
    with its upper bound, at a ratio of at most 0.97; otherwise neither."""
    aa_lo, aa_hi = (band["lo"], band["hi"]) if band else (1.0, 1.0)
    if row["lo"] > max(guard, aa_hi):
        return "loss"
    if row["hi"] < min(1.0, aa_lo) and row["ratio"] <= 0.97:
        return "win"
    return "neither"


def run_table(rows: list[RunRow], aa: list[RunRow]) -> None:
    bands = {(r["command"], r["row"]): r for r in aa}
    print(
        "| Command | Row | n | Runs | B/A | 95% over runs | A/A | A/A 95% | Verdict |"
    )
    print("|---|---|---:|---:|---:|---|---:|---|---|")
    for r in rows:
        b = bands.get((r["command"], r["row"]))
        aa_txt = f"{b['ratio']:.3f} | {b['lo']:.3f}–{b['hi']:.3f}" if b else "– | –"
        print(
            f"| `{r['command']}` | {r['row']} | {r['n']} | {len(r['runs'])} | {r['ratio']:.3f} "
            f"| {r['lo']:.3f}–{r['hi']:.3f} | {aa_txt} | {verdict(r, b)} |"
        )


def main(out: str) -> None:
    best, rounds, results = load(out)
    table("Set", set_rows(best, rounds, results))
    tags = load_tags(out)
    rows, untagged = class_rows(best, rounds, results, tags)
    print()
    table("Class", rows)
    for qset, n in sorted(untagged.items()):
        where = tags[qset][0] if qset in tags else "no query file found"
        print(
            f"\nQueries of the {qset} set without tags, in no class row: {n} ({where})."
        )


if __name__ == "__main__":
    if len(sys.argv) == 4 and sys.argv[2] == "--aa":
        aa_rounds = load(sys.argv[3])[1]
        run_table(
            run_rows(load(sys.argv[1])[1], load_tags(sys.argv[1])),
            run_rows(aa_rounds, load_tags(sys.argv[3])),
        )
    else:
        main(sys.argv[1])
