#!/usr/bin/env python3
"""The write-buffer bake-off's runner (docs/perf/write-buffer-bakeoff.md): interleaved trials of the
three candidates and RocksDB through the same workload code, each trial running every arm of a row in a
rotated order, with A/A twins (an arm run under a second name) so every row carries its own spread.

  bakeoff.py t1 OUT TRIALS [ROWSET]    T1 (fill/overwrite, 1/2/4/8 writers) + T6's bytes per record
  bakeoff.py reads OUT TRIALS [ROWSET] T2/T3: get and scan latency beside writers
  bakeoff.py req OUT TRIALS            T11a (applier) and T11b (write group): request threads over many indexes
  bakeoff.py misc OUT TRIALS           T6 freeze sweep, T7 many, T4 single-writer shape
  bakeoff.py report OUT KIND           medians, spreads and ratios from OUT/KIND.jsonl

Binaries: $LB (the prototype), $LBR (RocksDB adapter), $LBM (today's buffer). OUT is a directory; every
process appends its JSON line(s) to OUT/KIND.jsonl with its trial, arm and row. Run pieces under
`quiet run bakeoff-N 20 ... -- bakeoff.py ...` (strict), at most 20 minutes each.
"""

from __future__ import annotations

import json
import os
import statistics
import subprocess
import sys
import time
from pathlib import Path
from typing import Any

LB = os.environ.get("LB", ".scratch/lb2/lb_plain")
LBR = os.environ.get("LBR", ".scratch/lb2/lb_rocksdb")
LBM = os.environ.get("LBM", "build-rel/kv_proto_lanebuf_main")
SCRATCH = os.environ.get("LB_SCRATCH", ".scratch/bakeoff")

Arm = tuple[str, list[str]]  # label, argv


def arms_t1(row: dict[str, Any]) -> list[Arm]:
    mode = row["workload"]
    common = ["--writers", str(row["writers"]), "--records", str(row["records"]), "--ops", str(row["records"]), "--value", str(row["value"]), "--workload", mode, "--segments", "2", "--seg-records", "100000"]
    lb = lambda impl: [LB, "fill", "--impl", impl, *common]
    rocks = [LBR, "fill", "--dir", f"{SCRATCH}/rdb", "--wal", "0", "--workload", mode, "--writers", str(row["writers"]), "--records", str(row["records"]), "--ops", str(row["records"]), "--value", str(row["value"])]
    out = [("C", lb("c")), ("F", lb("f")), ("A'", lb("a")), ("C2", lb("c")), ("F2", lb("f")), ("RocksDB", rocks)]
    if mode == "fillrandom" and row["value"] == 100:
        out.append(("main", [LBM, "fill", "--dir", f"{SCRATCH}/main", *common[:10]]))
    return out


def rows_t1(rowset: str) -> list[dict[str, Any]]:
    rows = []
    for value, records in ((100, 1_000_000), (1024, 200_000)):
        for workload in ("fillrandom", "overwrite"):
            for writers in (1, 2, 4, 8):
                if rowset == "primary" and not (value == 100 and workload == "fillrandom"):
                    continue
                if rowset == "rest" and (value == 100 and workload == "fillrandom"):
                    continue
                rows.append({"workload": workload, "value": value, "records": records, "writers": writers})
    return rows


def rotate(arms: list[Arm], k: int) -> list[Arm]:
    k %= len(arms)
    return arms[k:] + arms[:k]


def run_one(argv: list[str], timeout: float = 900) -> list[dict[str, Any]]:
    p = subprocess.run(argv, capture_output=True, text=True, timeout=timeout)
    if p.returncode != 0:
        sys.stderr.write(f"FAILED {' '.join(argv)}\n{p.stderr[-800:]}\n")
        return []
    out = []
    for line in p.stdout.splitlines():
        line = line.strip()
        if line.startswith("{"):
            try:
                out.append(json.loads(line))
            except json.JSONDecodeError:
                pass
    return out


def append(path: Path, rec: dict[str, Any]) -> None:
    with path.open("a") as f:
        f.write(json.dumps(rec) + "\n")


def cmd_t1(out: Path, trials: int, rowset: str) -> None:
    out.mkdir(parents=True, exist_ok=True)
    rows = rows_t1(rowset)
    for trial in range(trials):
        for row in rows:
            for label, argv in rotate(arms_t1(row), trial):
                for rec in run_one(argv):
                    append(out / "t1.jsonl", {"trial": trial, "arm": label, "row": row, "rec": rec})
        print(f"t1 trial {trial} done", flush=True)


def reads_rows(rowset: str) -> list[dict[str, Any]]:
    rows = []
    for kind in ("get", "scan"):
        for writers in (1, 2, 4, 8):
            for readers in (1, 4):
                rows.append({"kind": kind, "writers": writers, "readers": readers, "mbs": 0})
        rows.append({"kind": kind, "writers": 4, "readers": 8, "mbs": 0})
        for writers in (1, 4):
            rows.append({"kind": kind, "writers": writers, "readers": 4, "mbs": 2})
    if rowset != "all":
        rows = [r for r in rows if r["kind"] == rowset]
    return rows


def arms_reads(row: dict[str, Any]) -> list[Arm]:
    common = ["--kind", row["kind"], "--writers", str(row["writers"]), "--readers", str(row["readers"]), "--write-mb-s", str(row["mbs"]), "--seconds", "2", "--records", "200000", "--value", "100"]
    lb = lambda impl: [LB, "rww", "--impl", impl, *common]
    rocks = [LBR, "rww", "--dir", f"{SCRATCH}/rdb", "--wal", "0", *common]
    return [("C", lb("c")), ("A'", lb("a")), ("C2", lb("c")), ("RocksDB", rocks)]


def cmd_reads(out: Path, trials: int, rowset: str) -> None:
    out.mkdir(parents=True, exist_ok=True)
    rows = reads_rows(rowset)
    for trial in range(trials):
        for row in rows:
            for label, argv in rotate(arms_reads(row), trial):
                for rec in run_one(argv):
                    append(out / "reads.jsonl", {"trial": trial, "arm": label, "row": row, "rec": rec})
        print(f"reads trial {trial} done", flush=True)


def req_common(extra: list[str]) -> list[str]:
    return ["--indexes", "1000", "--min-records", os.environ.get("LB_MIN", "1000"), "--max-records", os.environ.get("LB_MAX", "10000"), "--value", "1024", "--seconds", os.environ.get("LB_SECS", "3"),
            "--rs", "1,4,8,16", "--dists", "uniform,zipf", "--mixes", "1w3r,3w1r", *extra]


def arms_req(path: str, turn: str) -> list[Arm]:
    if path == "applier":
        a = ["--path", "applier", "--appliers", os.environ.get("LB_APPLIERS", "2"), "--turn-us", turn]
        return [("C", [LB, "requests", "--impl", "c", *req_common(a)]), ("F", [LB, "requests", "--impl", "f", *req_common(a)]), ("F2", [LB, "requests", "--impl", "f", *req_common(a)]),
                ("RocksDB", [LBR, "requests", "--dir", f"{SCRATCH}/rdb", "--wal", "1", *req_common([])])]
    a = ["--path", "group"]
    return [("C", [LB, "requests", "--impl", "c", *req_common(a)]), ("F", [LB, "requests", "--impl", "f", *req_common(a)]), ("F2", [LB, "requests", "--impl", "f", *req_common(a)]),
            ("RocksDB", [LBR, "requests", "--dir", f"{SCRATCH}/rdb", "--wal", "0", *req_common([])])]


def cmd_req(out: Path, trials: int, which: str) -> None:
    out.mkdir(parents=True, exist_ok=True)
    paths = {"a0": ("applier", "0"), "a50": ("applier", os.environ.get("LB_TURN", "50")), "b": ("group", "0")}
    for trial in range(trials):
        for key in which.split(","):
            path, turn = paths[key]
            for label, argv in rotate(arms_req(path, turn), trial):
                for rec in run_one(argv, timeout=1500):
                    append(out / "req.jsonl", {"trial": trial, "arm": label, "path": key, "rec": rec})
        print(f"req trial {trial} done", flush=True)


def cmd_misc(out: Path, trials: int) -> None:
    out.mkdir(parents=True, exist_ok=True)
    for impl in ("c", "a", "f"):
        for rec in run_one([LB, "freeze", "--impl", impl, "--trials", str(trials)], timeout=1800):
            append(out / "freeze.jsonl", rec)
        for rec in run_one([LB, "many", "--impl", impl, "--count", "1000", "--trials", str(trials)]):
            append(out / "many.jsonl", rec)


# ---- report -------------------------------------------------------------------------------


def med(xs: list[float]) -> float:
    return statistics.median(xs) if xs else float("nan")


def spread(xs: list[float]) -> float:
    return (max(xs) - min(xs)) / med(xs) * 100 if len(xs) > 1 and med(xs) else float("nan")


def load(path: Path) -> list[dict[str, Any]]:
    return [json.loads(x) for x in path.read_text().splitlines() if x.strip()] if path.exists() else []


def row_key(r: dict[str, Any]) -> str:
    return " ".join(f"{k}={v}" for k, v in r["row"].items())


def report(out: Path, kind: str) -> None:
    recs = load(out / f"{kind}.jsonl")
    groups: dict[str, dict[str, list[dict[str, Any]]]] = {}
    for r in recs:
        key = row_key(r) if "row" in r else r.get("path", "")
        groups.setdefault(key, {}).setdefault(r["arm"], []).append(r)
    metrics = {
        "t1": [("ops/s", lambda r: r["rec"]["result"]["ops_per_sec"]), ("cpu ns/op", lambda r: r["rec"]["result"]["cpu_ns_per_op"]), ("rss B/new rec", lambda r: r["rec"]["result"]["rss_per_new_record"])],
        "reads": [("read p99 ns", lambda r: r["rec"]["result"]["p99_ns"]), ("read p99.9 ns", lambda r: r["rec"]["result"]["p999_ns"]), ("reads/s", lambda r: r["rec"]["result"]["read_ops_per_sec"]),
                  ("writes/s", lambda r: r["rec"]["result"]["write_ops_per_sec"])],
    }[kind]
    for key, arms in groups.items():
        print(f"\n## {key}")
        for name, f in metrics:
            line = []
            for arm in sorted(arms):
                xs = [f(r) for r in arms[arm]]
                line.append(f"{arm}: {med(xs):,.0f} (n={len(xs)}, spread {spread(xs):.1f}%)")
            print(f"- {name}: " + "; ".join(line))


def main() -> None:
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(2)
    cmd, out = sys.argv[1], Path(sys.argv[2])
    if cmd == "t1":
        cmd_t1(out, int(sys.argv[3]), sys.argv[4] if len(sys.argv) > 4 else "all")
    elif cmd == "reads":
        cmd_reads(out, int(sys.argv[3]), sys.argv[4] if len(sys.argv) > 4 else "all")
    elif cmd == "req":
        cmd_req(out, int(sys.argv[3]), sys.argv[4] if len(sys.argv) > 4 else "a0")
    elif cmd == "misc":
        cmd_misc(out, int(sys.argv[3]))
    elif cmd == "report":
        report(out, sys.argv[3])
    else:
        print(__doc__)
        sys.exit(2)


if __name__ == "__main__":
    main()
