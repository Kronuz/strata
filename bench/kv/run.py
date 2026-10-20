#!/usr/bin/env python3
"""The key-value quick tier: strata's core against RocksDB (docs/perf/core-vs-rocksdb.md).

  run.py calibrate OUT          RocksDB's variants per value size, one thread, one trial each
  run.py rows OUT SET [TRIALS]  interleaved trials of a row set (see SETS), results appended to OUT/raw.jsonl
  run.py aa OUT                 A/A: each engine against itself on a few rows
  run.py report OUT             the tables

Environment: KV_STRATA, KV_ROCKSDB (driver binaries), KV_DIR (data directory, default
OUT/data), KV_VARIANT_100 and KV_VARIANT_1024 (RocksDB's calibrated variants).
Every run's phase times are in its JSON line; the session's own phases go to OUT/phases.jsonl.
"""

from __future__ import annotations

import json
import math
import os
import random
import statistics
import subprocess
import sys
import time
from pathlib import Path
from typing import Any

DB_BENCH = ["fillseq", "fillrandom", "overwrite", "readrandom", "readmissing", "readseq", "seekrandom"]
YCSB = ["ycsb_a", "ycsb_b", "ycsb_c", "ycsb_d", "ycsb_e", "ycsb_f"]
WRITES = {"fillseq", "fillrandom", "overwrite", "ycsb_a", "ycsb_b", "ycsb_d", "ycsb_e", "ycsb_f"}
RECORDS = 100_000
FITS_MB = 256


def row_args(workload: str, value: int, memory_mb: int, threads: int, durability: str, batch: int) -> list[str]:
    ops = {"seekrandom": 10_000, "readseq": 1}.get(workload, 100_000)
    if durability == "sync" and batch == 1:
        ops = min(ops, 100_000)
    args = ["--workload", workload, "--records", str(RECORDS), "--ops", str(ops), "--value-size", str(value),
            "--threads", str(threads), "--durability", durability, "--batch", str(batch), "--memory-mb", str(memory_mb)]
    # Every run phase stops at 20 s (15 s durable); a capped row's rate is over the operations it finished.
    args += ["--max-seconds", "15" if durability == "sync" else "20"]
    return args


def two_x_mb(value: int) -> int:
    return max(1, (RECORDS * (16 + value)) // 2 >> 20)


def sets() -> dict[str, list[dict[str, Any]]]:
    s: dict[str, list[dict[str, Any]]] = {}
    fits = [dict(w=w, value=100, mb=FITS_MB, threads=1, dur="none", batch=1) for w in DB_BENCH]
    fits += [dict(w=w, value=1024, mb=FITS_MB, threads=1, dur="none", batch=1) for w in YCSB]
    s["fits1"] = fits
    s["twox1"] = [dict(r, mb=two_x_mb(r["value"])) for r in fits]
    s["sync1"] = [dict(w=w, value=100, mb=FITS_MB, threads=1, dur="sync", batch=100) for w in ["fillrandom", "overwrite"]]
    s["sync1"] += [dict(w=w, value=1024, mb=FITS_MB, threads=1, dur="sync", batch=1) for w in ["ycsb_a", "ycsb_f"]]
    s["sync1"] += [dict(w="fillrandom", value=100, mb=FITS_MB, threads=1, dur="sync", batch=1)]
    s["wal1"] = [dict(w=w, value=100, mb=FITS_MB, threads=1, dur="wal", batch=1) for w in ["fillrandom", "overwrite"]]
    s["fits4"] = [dict(r, threads=4) for r in fits if r["w"] != "readseq"]
    s["many"] = [dict(w="readrandom", value=100, mb=FITS_MB, threads=1, dur="none", batch=1, indexes=1000)]
    return s


def env_bin(name: str) -> str:
    v = os.environ.get(name)
    if not v:
        sys.exit(f"{name} is not set")
    return v


def run_one(engine: str, row: dict[str, Any], out: Path, trial: int, variant: str | None = None) -> dict[str, Any]:
    binary = env_bin("KV_STRATA" if engine == "strata" else "KV_ROCKSDB")
    data = Path(os.environ.get("KV_DIR", str(out / "data"))) / engine
    args = [binary, "--dir", str(data)] + row_args(row["w"], row["value"], row["mb"], row["threads"], row["dur"], row["batch"])
    if row.get("indexes", 1) > 1:
        args += ["--indexes", str(row["indexes"])]
    if engine == "rocksdb":
        v = variant or os.environ.get(f"KV_VARIANT_{row['value']}", "lvl-4k-lz4")
        args += ["--variant", v]
    t0 = time.time()
    p = subprocess.run(args, capture_output=True, text=True)
    if p.returncode != 0:
        sys.stderr.write(p.stderr)
        raise SystemExit(f"{engine} failed: {' '.join(args)}")
    r = json.loads(p.stdout.strip().splitlines()[-1])
    r["trial"] = trial
    r["wall"] = time.time() - t0
    r["row"] = row
    if p.stderr.strip():
        r["stderr"] = p.stderr.strip()[-500:]
    return r


def append(out: Path, name: str, r: dict[str, Any]) -> None:
    with open(out / name, "a") as f:
        f.write(json.dumps(r) + "\n")


def phase(out: Path, name: str, seconds: float, **kw: Any) -> None:
    append(out, "phases.jsonl", dict(phase=name, seconds=round(seconds, 3), at=time.strftime("%H:%M:%S"), **kw))


def cmd_calibrate(out: Path) -> None:
    variants = ["lvl-4k-lz4", "lvl-16k-lz4", "lvl-4k-none", "lvl-4k-zstd", "lvl-4k-lz4-hash", "uni-4k-lz4"]
    t0 = time.time()
    for value, rows in [(100, ["fillrandom", "readrandom", "ycsb_a"]), (1024, ["fillrandom", "readrandom", "ycsb_a"])]:
        for w in rows:
            row = dict(w=w, value=value, mb=FITS_MB, threads=1, dur="none", batch=1)
            for v in variants:
                r = run_one("rocksdb", row, out, 0, v)
                append(out, "calibration.jsonl", r)
                print(f"calibrate {value} {w} {v}: {run_phase(r)['ops_per_sec']:.0f} ops/s", flush=True)
            r = run_one("strata", row, out, 0)
            append(out, "calibration.jsonl", r)
            print(f"calibrate {value} {w} strata: {run_phase(r)['ops_per_sec']:.0f} ops/s", flush=True)
    phase(out, "calibrate", time.time() - t0)
    best = pick_variants(out)
    print(json.dumps(best))


def pick_variants(out: Path) -> dict[int, str]:
    rs = [json.loads(x) for x in open(out / "calibration.jsonl")]
    best: dict[int, str] = {}
    for value in (100, 1024):
        score: dict[str, list[float]] = {}
        for r in rs:
            if r["engine"] == "rocksdb" and r["value_size"] == value:
                score.setdefault(r["variant"], []).append(run_phase(r)["ops_per_sec"])
        best[value] = max(score, key=lambda v: geomean(score[v]))
    return best


def geomean(xs: list[float]) -> float:
    return math.exp(sum(math.log(x) for x in xs) / len(xs))


def run_phase(r: dict[str, Any]) -> dict[str, Any]:
    return next(p for p in r["phases"] if p["name"] == "run")


def cmd_rows(out: Path, name: str, trials: int) -> None:
    # A rerun of a set is recorded under its own name: "fits1@2" reruns fits1.
    rows = sets()[name.split("@")[0]]
    t0 = time.time()
    for row in rows:
        t1 = time.time()
        for trial in range(trials):
            order = ["strata", "rocksdb"] if trial % 2 == 0 else ["rocksdb", "strata"]
            for engine in order:
                r = run_one(engine, row, out, trial)
                r["set"] = name
                append(out, "raw.jsonl", r)
        print(f"{name} {row['w']} v{row['value']} t{row['threads']} {row['dur']}: {time.time() - t1:.1f}s", flush=True)
    phase(out, f"rows:{name}", time.time() - t0, trials=trials, rows=len(rows))


def cmd_aa(out: Path, trials: int, label: str = "aa") -> None:
    rows = [r for r in sets()["fits1"] if r["w"] in ("fillrandom", "readrandom", "ycsb_a", "ycsb_e")]
    t0 = time.time()
    for row in rows:
        for engine in ("strata", "rocksdb"):
            for trial in range(2 * trials):
                r = run_one(engine, row, out, trial)
                r["set"] = label
                r["side"] = "a" if trial % 2 == 0 else "b"
                append(out, "raw.jsonl", r)
    phase(out, label, time.time() - t0, trials=trials)


def boot_ratio(a: list[float], b: list[float], n: int = 4000) -> tuple[float, float, float]:
    rng = random.Random(7)
    med = statistics.median(a) / statistics.median(b)
    rs = []
    for _ in range(n):
        ra = [rng.choice(a) for _ in a]
        rb = [rng.choice(b) for _ in b]
        rs.append(statistics.median(ra) / statistics.median(rb))
    rs.sort()
    return med, rs[int(0.025 * n)], rs[int(0.975 * n) - 1]


def key_of(r: dict[str, Any]) -> tuple[Any, ...]:
    row = r["row"]
    return (r.get("set"), row["w"], row["value"], row["mb"], row["threads"], row["dur"], row["batch"], row.get("indexes", 1))


def summarize(rs: list[dict[str, Any]]) -> dict[str, float]:
    runs = [run_phase(r) for r in rs]
    med = statistics.median

    def phase_of(r: dict[str, Any], n: str) -> dict[str, Any] | None:
        return next((p for p in r["phases"] if p["name"] == n), None)

    out = dict(
        ops=med([p["ops_per_sec"] for p in runs]),
        p50=med([p["p50_us"] for p in runs]),
        p99=med([p["p99_us"] for p in runs]),
        p999=med([p["p999_us"] for p in runs]),
        cpu=med([p["cpu_us_per_op"] for p in runs]),
        peak=med([r["peak_rss"] for r in rs]),
        end=med([r["end_rss"] for r in rs]),
        disk=med([r["disk"] for r in rs]),
        files=med([r["files"] for r in rs]),
        fds=med([r["fds"] for r in rs]),
        wamp=med([r["written"] / r["logical"] if r["logical"] else 0 for r in rs]),
        samp=med([r["disk"] / (r["records"] * (16 + r["value_size"])) for r in rs]),
        total=med([r["total_seconds"] for r in rs]),
        n=len(rs),
    )
    for n in ("open", "load", "load_settle", "reopen", "run", "settle"):
        xs = [p["seconds"] for p in (phase_of(r, n) for r in rs) if p]
        if xs:
            out["t_" + n] = med(xs)
    ro = [phase_of(r, "reopen") for r in rs]
    if all(ro):
        out["reopen_rss"] = med([p["rss"] for p in ro if p])
    return out


def rate(r: dict[str, Any]) -> float:
    """Operations per second of the run; records per second for a full scan."""
    p = run_phase(r)
    if r["workload"] == "readseq":
        return p["touched"] / r["value_size"] / p["seconds"]
    return p["ops_per_sec"]


def durable_rate(r: dict[str, Any]) -> float:
    """The run's operations over the run and the settle that makes them durable."""
    p = run_phase(r)
    st = next((x for x in r["phases"] if x["name"] == "settle"), None)
    ops = p["touched"] / r["value_size"] if r["workload"] == "readseq" else p["ops"]
    return ops / (p["seconds"] + (st["seconds"] if st else 0))


def word(ratio: float, higher_better: bool, noun: tuple[str, str], ci: tuple[float, float, float] | None = None) -> str:
    if ci and ci[1] <= 1.0 <= ci[2] or abs(ratio - 1) < 0.10:
        return f"tie ({ratio:.2f})"
    better = ratio > 1 if higher_better else ratio < 1
    f = ratio if ratio > 1 else 1 / ratio
    return f"{f:.2f}× {noun[0] if better else noun[1]}"


def pct(ratio: float, noun: str) -> str:
    if abs(ratio - 1) < 0.10:
        return f"tie ({ratio:.2f})"
    return f"{abs(ratio - 1):.0%} {'more' if ratio > 1 else 'less'} {noun}" if ratio < 2 else f"{ratio:.1f}× the {noun}"


def direction(ratio: float, lo: float, hi: float, faster_is_higher: bool, noun: tuple[str, str]) -> str:
    """strata's direction: ratio is strata over RocksDB."""
    if lo <= 1.0 <= hi or abs(ratio - 1) < 0.03:
        return "tie"
    better = ratio > 1 if faster_is_higher else ratio < 1
    f = ratio if ratio > 1 else 1 / ratio
    word = noun[0] if better else noun[1]
    return f"{f:.2f}× {word}"


def cmd_report(out: Path) -> None:
    rs = [json.loads(x) for x in open(out / "raw.jsonl")]
    groups: dict[tuple[Any, ...], dict[str, list[dict[str, Any]]]] = {}
    for r in rs:
        if str(r.get("set", "")).startswith("aa"):
            continue
        groups.setdefault(key_of(r), {}).setdefault(r["engine"], []).append(r)
    print("| set | workload | value | M MiB | thr | dur | strata ops/s | RocksDB ops/s | strata/RocksDB [95% CI] | direction | p50 µs s/r | p99 µs s/r | p99.9 µs s/r | CPU µs/op s/r | peak RSS MiB s/r | disk MiB s/r | write amp s/r |")
    print("|" + "---|" * 17)
    for k, g in groups.items():
        if "strata" not in g or "rocksdb" not in g:
            continue
        s, r = summarize(g["strata"]), summarize(g["rocksdb"])
        ratio, lo, hi = boot_ratio([run_phase(x)["ops_per_sec"] for x in g["strata"]], [run_phase(x)["ops_per_sec"] for x in g["rocksdb"]])
        d = direction(ratio, lo, hi, True, ("faster", "slower"))
        mib = 1 << 20
        print(f"| {k[0]} | {k[1]} | {k[2]} | {k[3]} | {k[4]} | {k[5]}{'/' + str(k[6]) if k[5] == 'sync' else ''} | {s['ops']:,.0f} | {r['ops']:,.0f} | "
              f"{ratio:.3f} [{lo:.3f}, {hi:.3f}] | {d} | {s['p50']:.2f}/{r['p50']:.2f} | {s['p99']:.2f}/{r['p99']:.2f} | {s['p999']:.1f}/{r['p999']:.1f} | "
              f"{s['cpu']:.2f}/{r['cpu']:.2f} | {s['peak'] / mib:.0f}/{r['peak'] / mib:.0f} | {s['disk'] / mib:.1f}/{r['disk'] / mib:.1f} | {s['wamp']:.2f}/{r['wamp']:.2f} |")
    print()
    print("Phase times (median seconds per run, strata/RocksDB):")
    print()
    print("| set | workload | thr | open | load | load settle | reopen | run | settle | total |")
    print("|---|---|---|---|---|---|---|---|---|---|")
    for k, g in groups.items():
        if "strata" not in g or "rocksdb" not in g:
            continue
        s, r = summarize(g["strata"]), summarize(g["rocksdb"])

        def pair(n: str) -> str:
            return f"{s.get('t_' + n, 0):.3f}/{r.get('t_' + n, 0):.3f}" if ("t_" + n) in s else "-"
        print(f"| {k[0]} | {k[1]} | {k[4]} | {pair('open')} | {pair('load')} | {pair('load_settle')} | {pair('reopen')} | {pair('run')} | {pair('settle')} | {s['total']:.2f}/{r['total']:.2f} |")
    print()
    print("Directions per axis (strata against RocksDB; a ratio inside 10% is a tie at this tier):")
    print()
    print("| set | workload | thr | dur | throughput | to durable | p99 | CPU/op | peak RSS | disk | bytes written |")
    print("|---|---|---|---|---|---|---|---|---|---|---|")
    for k, g in groups.items():
        if "strata" not in g or "rocksdb" not in g:
            continue
        s, r = summarize(g["strata"]), summarize(g["rocksdb"])
        thr = boot_ratio([rate(x) for x in g["strata"]], [rate(x) for x in g["rocksdb"]])
        dur = boot_ratio([durable_rate(x) for x in g["strata"]], [durable_rate(x) for x in g["rocksdb"]])
        cells = [word(thr[0], True, ("faster", "slower"), thr), word(dur[0], True, ("faster", "slower"), dur),
                 word(s["p99"] / r["p99"], False, ("lower", "higher")), word(s["cpu"] / r["cpu"], False, ("less CPU", "more CPU")),
                 pct(s["peak"] / r["peak"], "memory"), pct(s["disk"] / r["disk"], "disk"), pct(s["wamp"] / r["wamp"] if r["wamp"] else 0, "written")]
        print(f"| {k[0]} | {k[1]} | {k[4]} | {k[5]}{'/' + str(k[6]) if k[5] == 'sync' else ''} | " + " | ".join(cells) + " |")
    aa = [r for r in rs if str(r.get("set", "")).startswith("aa")]
    if aa:
        print()
        print("A/A (same engine, alternate trials as sides):")
        print()
        print("| set | engine | workload | ratio b/a [95% CI] | spread (max/min - 1) |")
        print("|---|---|---|---|---|")
        by: dict[tuple[str, str], dict[str, list[float]]] = {}
        for r in aa:
            by.setdefault((r["set"], r["engine"], r["row"]["w"]), {}).setdefault(r["side"], []).append(run_phase(r)["ops_per_sec"])
        for (st, e, w), sides in by.items():
            ratio, lo, hi = boot_ratio(sides["b"], sides["a"])
            allv = sides["a"] + sides["b"]
            print(f"| {st} | {e} | {w} | {ratio:.3f} [{lo:.3f}, {hi:.3f}] | {max(allv) / min(allv) - 1:.1%} |")


def main() -> None:
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    cmd, out = sys.argv[1], Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    if cmd == "calibrate":
        cmd_calibrate(out)
    elif cmd == "rows":
        cmd_rows(out, sys.argv[3], int(sys.argv[4]) if len(sys.argv) > 4 else 5)
    elif cmd == "aa":
        cmd_aa(out, int(sys.argv[3]) if len(sys.argv) > 3 else 3, sys.argv[4] if len(sys.argv) > 4 else "aa")
    elif cmd == "report":
        cmd_report(out)
    elif cmd == "one":
        engine, w = sys.argv[3], sys.argv[4]
        row = dict(w=w, value=int(sys.argv[5]) if len(sys.argv) > 5 else 100, mb=FITS_MB, threads=1, dur="none", batch=1)
        print(json.dumps(run_one(engine, row, out, 0), indent=1))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
