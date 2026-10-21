#!/usr/bin/env python3
"""A branch A/B of strata's key-value driver against itself at another commit: kv_strata built at
two commits (A, the base; B, the branch) on the quick tier's rows, run-level and interleaved.

  ab.py aa OUT [TRIALS]          A/A first: each binary against itself on a few rows
  ab.py rows OUT SET [TRIALS]    interleaved trials of SET (points, many, or any of run.py's sets)
  ab.py report OUT               B against A per row, and the A/A noise floor

Environment: KV_A, KV_B (the two kv_strata binaries), KV_DIR (data directory, default OUT/data).
Raw lines go to OUT/ab.jsonl with each run's phase times, as run.py's do; a branch A/B is
functional evidence, never a gate row (PLAN, "What waits").
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))
import run as kv

SCANS = {"readseq", "seekrandom", "ycsb_e"}


def ab_sets() -> dict[str, list[dict[str, Any]]]:
    s = kv.sets()
    s["points"] = [r for r in s["fits1"] if r["w"] not in SCANS]  # point gets and fills
    # The pressure flush: memory-bound writes (twox1) and the one fits row that must not move.
    s["pressure"] = [r for r in s["twox1"] if r["w"] in ("fillrandom", "overwrite", "fillseq") and r["value"] == 100]
    s["pressure"] += [r for r in s["fits1"] if r["w"] == "fillrandom" and r["value"] == 100]
    # Rows the pressure flush's explorer did not run: durable fills, many indexes, 1 KiB ycsb_a at 2x.
    s["pressure_more"] = [r for r in s["sync1"] if r["w"] == "fillrandom" and r["batch"] == 100] + [r for r in s["wal1"] if r["w"] == "fillrandom"]
    s["pressure_more"] += s["many"] + [r for r in s["twox1"] if r["w"] == "ycsb_a"]
    return s


def run_one(side: str, row: dict[str, Any], out: Path, trial: int) -> dict[str, Any]:
    binary = kv.env_bin("KV_A" if side == "a" else "KV_B")
    data = Path(os.environ.get("KV_DIR", str(out / "data"))) / side
    shutil.rmtree(data, ignore_errors=True)
    args = [binary, "--dir", str(data)] + kv.row_args(row["w"], row["value"], row["mb"], row["threads"], row["dur"], row["batch"])
    if row.get("indexes", 1) > 1:
        args += ["--indexes", str(row["indexes"])]
    t0 = time.time()
    p = subprocess.run(args, capture_output=True, text=True, check=False)
    if p.returncode != 0:
        sys.stderr.write(p.stderr)
        raise SystemExit(f"{side} failed: {' '.join(args)}")
    r = json.loads(p.stdout.strip().splitlines()[-1])
    r.update(side=side, trial=trial, wall=time.time() - t0, row=row)
    shutil.rmtree(data, ignore_errors=True)
    return r


def append(out: Path, r: dict[str, Any]) -> None:
    with open(out / "ab.jsonl", "a") as f:
        f.write(json.dumps(r) + "\n")


def cmd_rows(out: Path, name: str, trials: int) -> None:
    rows = ab_sets()[name]
    t0 = time.time()
    for row in rows:
        t1 = time.time()
        for trial in range(trials):
            for side in ["a", "b"] if trial % 2 == 0 else ["b", "a"]:
                r = run_one(side, row, out, trial)
                r["set"] = name
                append(out, r)
        print(f"{name} {row['w']} v{row['value']} x{row.get('indexes', 1)}: {time.time() - t1:.1f}s", flush=True)
    kv.phase(out, f"ab:{name}", time.time() - t0, trials=trials, rows=len(rows))


def cmd_aa(out: Path, trials: int) -> None:
    rows = [r for r in ab_sets()["points"] if r["w"] in ("fillrandom", "readrandom", "ycsb_c")]
    t0 = time.time()
    for row in rows:
        for side in ("a", "b"):
            for trial in range(2 * trials):
                r = run_one(side, row, out, trial)
                r["set"] = "aa"
                r["aa_half"] = trial % 2
                append(out, r)
        print(f"aa {row['w']} v{row['value']}: done", flush=True)
    kv.phase(out, "ab:aa", time.time() - t0, trials=trials)


def cmd_report(out: Path) -> None:
    with open(out / "ab.jsonl") as f:
        rs = [json.loads(x) for x in f]
    groups: dict[tuple[Any, ...], dict[str, list[dict[str, Any]]]] = {}
    for r in rs:
        if r.get("set") == "aa":
            continue
        groups.setdefault(kv.key_of(r), {}).setdefault(r["side"], []).append(r)
    mib = 1 << 20
    print(
        "| set | workload | value | indexes | A ops/s | B ops/s | B/A [95% CI] | direction | p50 µs A/B | p99 µs A/B | CPU µs/op A/B | peak RSS MiB A/B | disk MiB A/B | n |"
    )
    print("|" + "---|" * 14)
    for k, g in groups.items():
        if "a" not in g or "b" not in g:
            continue
        a, b = kv.summarize(g["a"]), kv.summarize(g["b"])
        ratio, lo, hi = kv.boot_ratio([kv.rate(x) for x in g["b"]], [kv.rate(x) for x in g["a"]])
        d = kv.direction(ratio, lo, hi, True, ("faster", "slower"))
        print(
            f"| {k[0]} | {k[1]} | {k[2]} | {k[7]} | {a['ops']:,.0f} | {b['ops']:,.0f} | {ratio:.3f} [{lo:.3f}, {hi:.3f}] | {d} | "
            f"{a['p50']:.2f}/{b['p50']:.2f} | {a['p99']:.2f}/{b['p99']:.2f} | {a['cpu']:.2f}/{b['cpu']:.2f} | "
            f"{a['peak'] / mib:.0f}/{b['peak'] / mib:.0f} | {a['disk'] / mib:.1f}/{b['disk'] / mib:.1f} | {a['n']}/{b['n']} |"
        )
    print()
    print("Phase times (median seconds per run, A/B):")
    print()
    print("| set | workload | load | load settle | reopen | run | settle | reopen RSS MiB |")
    print("|---|---|---|---|---|---|---|---|")
    for k, g in groups.items():
        if "a" not in g or "b" not in g:
            continue
        a, b = kv.summarize(g["a"]), kv.summarize(g["b"])

        def pair(n: str, a: dict[str, float] = a, b: dict[str, float] = b) -> str:
            return f"{a.get('t_' + n, 0):.3f}/{b.get('t_' + n, 0):.3f}" if ("t_" + n) in a else "-"

        rss = f"{a['reopen_rss'] / mib:.0f}/{b['reopen_rss'] / mib:.0f}" if "reopen_rss" in a and "reopen_rss" in b else "-"
        print(f"| {k[0]} | {k[1]} | {pair('load')} | {pair('load_settle')} | {pair('reopen')} | {pair('run')} | {pair('settle')} | {rss} |")
    aa = [r for r in rs if r.get("set") == "aa"]
    if aa:
        print()
        print("A/A (each binary against itself, alternate runs as halves):")
        print()
        print("| binary | workload | second/first half [95% CI] | spread (max/min - 1) |")
        print("|---|---|---|---|")
        by: dict[tuple[str, str], dict[int, list[float]]] = {}
        for r in aa:
            by.setdefault((r["side"], r["row"]["w"]), {}).setdefault(r["aa_half"], []).append(kv.rate(r))
        for (side, w), halves in by.items():
            ratio, lo, hi = kv.boot_ratio(halves[1], halves[0])
            allv = halves[0] + halves[1]
            print(f"| {side.upper()} | {w} | {ratio:.3f} [{lo:.3f}, {hi:.3f}] | {max(allv) / min(allv) - 1:.1%} |")


def main() -> None:
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    cmd, out = sys.argv[1], Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    if cmd == "aa":
        cmd_aa(out, int(sys.argv[3]) if len(sys.argv) > 3 else 3)
    elif cmd == "rows":
        cmd_rows(out, sys.argv[3], int(sys.argv[4]) if len(sys.argv) > 4 else 5)
    elif cmd == "report":
        cmd_report(out)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
