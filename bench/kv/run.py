#!/usr/bin/env python3
"""The key-value quick tier: strata's core against RocksDB (docs/perf/core-vs-rocksdb.md).

  run.py calibrate OUT [SIZES]  RocksDB's variants per value size (default 100,1024; the ladder is 400,4096,65536), one thread, one trial each
  run.py rows OUT SET [TRIALS]  interleaved trials of a row set (see SETS), results appended to OUT/raw.jsonl
  run.py aa OUT                 A/A: each engine against itself on a few rows
  run.py dry OUT SET [RECORDS]  every row of SET once per engine on RECORDS (default 2000) records, with the checks a tiny run can make
  run.py report OUT             the tables

Environment: KV_STRATA, KV_ROCKSDB (driver binaries), KV_DIR (data directory, default
OUT/data), KV_VARIANT_<value size> (RocksDB's calibrated variant: 100, 400, 1024, 4096, 65536).

The coverage sets (docs/perf/benchmark-coverage.md, "Add this week"), each a few minutes at 3 to 5 trials:
  twox4, fits8   fits4's rows at half the memory; fits1's rows on 8 threads
  delete1        deleterandom of half the records, then readall over the result (every key once; the
                 driver checks the misses equal the deletes); disk is after the settle. Fits and 2x
  big1           10x memory: 1M records at 12 MiB (1 KiB values: 200k records at 24 MiB)
  multiget1      multireadrandom, batch 10: strata loops gets, RocksDB calls MultiGet. `ops` counts keys,
                 the latency percentiles are per batch
  bulk1          bulkload: fillrandom with automatic merges off (RocksDB: its bulkload preset, vector
                 memtable, no WAL), then a timed forced merge (`compact` phase), then settle. Fits and 2x
  rww1           readwhilewriting, inline: each thread alternates readrandom's gets and writes of a
                 token bucket at 2 MB/s (benchmark.sh's rate). The reads and the write keys and values are
                 the same sequence for both engines; how many writes fit in the run follows the
                 reads' speed, so the engines finish with different write counts (`writes` in the run phase)
  sizes1         the value-size ladder: 400 B, 4 KiB, 64 KiB, records scaled to a file of about 11.6 MB
                 (64 KiB: 2000 records); needs KV_VARIANT_400, _4096, _65536 from `calibrate`
Differences that cannot be removed: a multiget is a loop of gets in strata (no batched get until M9);
bulkload's forced merge is force_merge(1) in strata and CompactRange in RocksDB; RocksDB's threads share
one DB while strata's own one index each (fits4, twox4, fits8).
Every run's phase times are in its JSON line; the session's own phases go to OUT/phases.jsonl.
"""

from __future__ import annotations

import json
import math
import os
import random
import re
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


def row_args(row: dict[str, Any]) -> list[str]:
    workload, value, durability, batch = row["w"], row["value"], row["dur"], row["batch"]
    records = row.get("records", RECORDS)
    ops = {"seekrandom": 10_000, "readseq": 1, "deleterandom": records // 2, "readwhilewriting": 1_000_000}.get(workload, 100_000)
    ops = row.get("ops", ops)
    args = ["--workload", workload, "--records", str(records), "--ops", str(ops), "--value-size", str(value),
            "--threads", str(row["threads"]), "--durability", durability, "--batch", str(batch), "--memory-mb", str(row["mb"])]
    if "after" in row:
        args += ["--after", row["after"]]
    if "write_mb_s" in row:
        args += ["--write-mb-s", str(row["write_mb_s"])]
    # Every run phase stops at 20 s (15 s durable); a capped row's rate is over the operations it finished.
    args += ["--max-seconds", "15" if durability == "sync" else "20"]
    return args


def two_x_mb(value: int, records: int = RECORDS) -> int:
    return max(1, (records * (16 + value)) // 2 >> 20)


# The value-size ladder: records that keep the file near fits1's 11.6 MB (64 KiB: 2000 records, 131 MB).
LADDER = {400: 11_600_000 // (16 + 400), 4096: 11_600_000 // (16 + 4096), 65536: 2000}


def sets() -> dict[str, list[dict[str, Any]]]:
    s: dict[str, list[dict[str, Any]]] = {}
    fits = [dict(w=w, value=100, mb=FITS_MB, threads=1, dur="none", batch=1) for w in DB_BENCH]
    fits += [dict(w=w, value=1024, mb=FITS_MB, threads=1, dur="none", batch=1) for w in YCSB]
    s["fits1"] = fits
    s["twox1"] = [dict(r, mb=two_x_mb(r["value"])) for r in fits]
    s["sync1"] = [dict(w=w, value=100, mb=FITS_MB, threads=1, dur="sync", batch=100) for w in ["fillrandom", "overwrite"]]
    s["sync1"] += [dict(w=w, value=1024, mb=FITS_MB, threads=1, dur="sync", batch=1) for w in ["ycsb_a", "ycsb_f"]]
    s["sync1"] += [dict(w="fillrandom", value=100, mb=FITS_MB, threads=1, dur="sync", batch=1)]
    s["sync1"] += [dict(w="fillrandom", value=100, mb=FITS_MB, threads=1, dur="sync", batch=10)]
    s["wal1"] = [dict(w=w, value=100, mb=FITS_MB, threads=1, dur="wal", batch=1) for w in ["fillseq", "fillrandom", "overwrite"]]
    s["fits4"] = [dict(r, threads=4) for r in fits if r["w"] != "readseq"]
    s["twox4"] = [dict(r, mb=two_x_mb(r["value"])) for r in s["fits4"]]
    s["fits8"] = [dict(r, threads=8) for r in fits if r["w"] != "readseq"]
    one = dict(value=100, mb=FITS_MB, threads=1, dur="none", batch=1)
    s["delete1"] = [dict(one, w="deleterandom", after="readall"), dict(one, w="deleterandom", after="readall", mb=two_x_mb(100))]
    s["big1"] = [dict(one, w=w, records=1_000_000, mb=12) for w in ["fillrandom", "readrandom", "seekrandom"]]
    s["big1"] += [dict(one, w="ycsb_a", value=1024, records=200_000, mb=24)]
    s["multiget1"] = [dict(one, w="multireadrandom", batch=10), dict(one, w="multireadrandom", batch=10, mb=two_x_mb(100))]
    s["bulk1"] = [dict(one, w="bulkload"), dict(one, w="bulkload", mb=two_x_mb(100))]
    s["rww1"] = [dict(one, w="readwhilewriting", write_mb_s=2), dict(one, w="readwhilewriting", write_mb_s=2, mb=two_x_mb(100))]
    s["sizes1"] = [dict(one, w=w, value=v, records=n, ops=min(100_000, 10 * n)) for v, n in LADDER.items() for w in ["fillrandom", "readrandom", "ycsb_a"]]
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
    args = [binary, "--dir", str(data)] + row_args(row)
    if row.get("indexes", 1) > 1:
        args += ["--indexes", str(row["indexes"])]
    if engine == "rocksdb":
        v = variant or os.environ.get(f"KV_VARIANT_{row['value']}", "lvl-4k-lz4")
        # Many column families settle in one atomic flush: one MANIFEST edit, not one per family.
        if row.get("indexes", 1) > 1 and "-atomic" not in v:
            v += "-atomic"
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


def cmd_calibrate(out: Path, sizes: list[int]) -> None:
    t0 = time.time()
    for value in sizes:
        # Large values add a block as big as a record; the smaller ladder files carry their own record count.
        variants = ["lvl-4k-lz4", "lvl-16k-lz4", "lvl-4k-none", "lvl-4k-zstd", "lvl-4k-lz4-hash", "uni-4k-lz4"] + (["lvl-64k-lz4"] if value >= 4096 else [])
        for w in ["fillrandom", "readrandom", "ycsb_a"]:
            row = dict(w=w, value=value, mb=FITS_MB, threads=1, dur="none", batch=1)
            if value in LADDER:
                row.update(records=LADDER[value], ops=min(100_000, 10 * LADDER[value]))
            for v in variants:
                r = run_one("rocksdb", row, out, 0, v)
                append(out, "calibration.jsonl", r)
                print(f"calibrate {value} {w} {v}: {run_phase(r)['ops_per_sec']:.0f} ops/s", flush=True)
            r = run_one("strata", row, out, 0)
            append(out, "calibration.jsonl", r)
            print(f"calibrate {value} {w} strata: {run_phase(r)['ops_per_sec']:.0f} ops/s", flush=True)
    phase(out, "calibrate", time.time() - t0)
    best = pick_variants(out, sizes)
    print(json.dumps(best))
    print(" ".join(f"KV_VARIANT_{v}={n}" for v, n in best.items()))


def pick_variants(out: Path, sizes: list[int]) -> dict[int, str]:
    rs = [json.loads(x) for x in open(out / "calibration.jsonl")]
    best: dict[int, str] = {}
    for value in sizes:
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


def cmd_dry(out: Path, name: str, records: int) -> None:
    """Every row of a set once per engine on a tiny table: what a tiny run can check, checked."""
    os.environ["KV_GET_DIGEST"] = "1"
    bad = 0
    for row in sets()[name]:
        tiny = dict(row, records=records, ops=min(row.get("ops", 100_000), 10 * records))
        if row["w"] == "deleterandom":
            tiny["ops"] = records // 2
        if row["w"] == "readwhilewriting":
            tiny["ops"] = 20 * records  # long enough for the token bucket to fire
        if row["w"] == "bulkload":
            tiny["after"] = "readall"  # read the compacted store back: every key, digest equal across engines
        digests = {}
        line = {}
        for engine in ("strata", "rocksdb"):
            r = run_one(engine, tiny, out, 0)
            run = run_phase(r)
            after = next((x for x in r["phases"] if x["name"] == "after"), None)
            compact = next((x for x in r["phases"] if x["name"] == "compact"), None)
            m = re.search(r"get digest ([0-9a-f]{16}) hits (\d+)", r.get("stderr", ""))
            digests[engine] = m.groups() if m else None
            line[engine] = (run["ops"], run["missed"], run["writes"], after and (after["ops"], after["missed"]), compact and round(compact["seconds"], 4), r["disk"])
            ok = run["ops"] > 0 and r["disk"] > 0
            if row["w"] == "multireadrandom":
                ok = ok and run["missed"] == 0 and run["touched"] == run["ops"] * row["value"]
            if row["w"] == "readwhilewriting":
                ok = ok and run["writes"] > 0 and run["logical"] == run["writes"] * (16 + row["value"])
            if row["w"] == "deleterandom":
                ok = ok and run["ops"] == records // 2 and after is not None and after["ops"] == records and after["missed"] == run["ops"]
            if row["w"] == "bulkload":
                ok = ok and compact is not None and run["ops"] == records and after is not None and after["ops"] == records and after["missed"] == 0
            if not ok:
                bad += 1
                print(f"FAIL {engine} {label(row)}: {line[engine]}")
        a, b = digests["strata"], digests["rocksdb"]
        capped = line["strata"][0] != line["rocksdb"][0]  # a durable row stopped at its time cap: different prefixes
        same = "n/a" if not a or not b or row["w"] == "readwhilewriting" or capped else "equal" if a == b else "DIFFER"
        if same == "DIFFER":
            bad += 1
        print(f"{name} {label(row)} v{row['value']}: (ops, missed, writes, after, compact s, disk) strata {line['strata']} rocksdb {line['rocksdb']}; get digest (value hash, hits) {a} {b}: {same}", flush=True)
    if bad:
        raise SystemExit(f"{bad} dry-run checks failed")


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
    return (r.get("set"), label(row), row["value"], row["mb"], row["threads"], row["dur"], row["batch"], row.get("indexes", 1))


def label(row: dict[str, Any]) -> str:
    """The workload cell: its name, then what sets the row apart from the set's default."""
    out = row["w"]
    if row["w"] == "multireadrandom":
        out += f" b{row['batch']}"
    if row.get("records", RECORDS) != RECORDS:
        out += f" {row['records']:,} recs"
    if "after" in row:
        out += f" +{row['after']}"
    return out


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
        p9999=med([p.get("p9999_us", 0) for p in runs]),
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
    for n in ("open", "load", "load_settle", "reopen", "run", "compact", "settle", "after"):
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
    after_run = sum(x["seconds"] for x in r["phases"] if x["name"] in ("compact", "settle"))  # bulkload's forced merge is part of getting durable
    ops = p["touched"] / r["value_size"] if r["workload"] == "readseq" else p["ops"]
    return ops / (p["seconds"] + after_run)


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


def num(v: float) -> str:
    return f"{v:,.0f}" if abs(v) >= 1000 else f"{v:.4g}"


def phase_named(r: dict[str, Any], name: str, field: str) -> float | None:
    p = next((x for x in r["phases"] if x["name"] == name), None)
    return p.get(field) if p else None


def extra_of(r: dict[str, Any]) -> bool:
    """A row with an after pass, a compact phase, or the writes of a read-while-writing run."""
    return "after" in r["row"] or r["workload"] in ("bulkload", "readwhilewriting")


def cmd_report(out: Path) -> None:
    rs = [json.loads(x) for x in open(out / "raw.jsonl")]
    groups: dict[tuple[Any, ...], dict[str, list[dict[str, Any]]]] = {}
    for r in rs:
        if str(r.get("set", "")).startswith("aa"):
            continue
        groups.setdefault(key_of(r), {}).setdefault(r["engine"], []).append(r)
    print("| set | workload | value | M MiB | thr | dur | strata ops/s | RocksDB ops/s | strata/RocksDB [95% CI] | direction | p50 µs s/r | p99 µs s/r | p99.9 µs s/r | p99.99 µs s/r | CPU µs/op s/r | peak RSS MiB s/r | disk MiB s/r | write amp s/r |")
    print("|" + "---|" * 18)
    for k, g in groups.items():
        if "strata" not in g or "rocksdb" not in g:
            continue
        s, r = summarize(g["strata"]), summarize(g["rocksdb"])
        ratio, lo, hi = boot_ratio([run_phase(x)["ops_per_sec"] for x in g["strata"]], [run_phase(x)["ops_per_sec"] for x in g["rocksdb"]])
        d = direction(ratio, lo, hi, True, ("faster", "slower"))
        mib = 1 << 20
        print(f"| {k[0]} | {k[1]} | {k[2]} | {k[3]} | {k[4]} | {k[5]}{'/' + str(k[6]) if k[5] == 'sync' else ''} | {s['ops']:,.0f} | {r['ops']:,.0f} | "
              f"{ratio:.3f} [{lo:.3f}, {hi:.3f}] | {d} | {s['p50']:.2f}/{r['p50']:.2f} | {s['p99']:.2f}/{r['p99']:.2f} | {s['p999']:.1f}/{r['p999']:.1f} | {s['p9999']:.1f}/{r['p9999']:.1f} | "
              f"{s['cpu']:.2f}/{r['cpu']:.2f} | {s['peak'] / mib:.0f}/{r['peak'] / mib:.0f} | {s['disk'] / mib:.1f}/{r['disk'] / mib:.1f} | {s['wamp']:.2f}/{r['wamp']:.2f} |")
    print()
    print("Phase times (median seconds per run, strata/RocksDB):")
    print()
    print("| set | workload | thr | open | load | load settle | reopen | run | compact | settle | after | total |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|")
    for k, g in groups.items():
        if "strata" not in g or "rocksdb" not in g:
            continue
        s, r = summarize(g["strata"]), summarize(g["rocksdb"])

        def pair(n: str) -> str:
            return f"{s.get('t_' + n, 0):.3f}/{r.get('t_' + n, 0):.3f}" if ("t_" + n) in s else "-"
        print(f"| {k[0]} | {k[1]} | {k[4]} | {pair('open')} | {pair('load')} | {pair('load_settle')} | {pair('reopen')} | {pair('run')} | {pair('compact')} | {pair('settle')} | {pair('after')} | {s['total']:.2f}/{r['total']:.2f} |")
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
    extras = [(k, g) for k, g in groups.items() if "strata" in g and "rocksdb" in g and any(extra_of(x) for x in g["strata"] + g["rocksdb"])]
    if extras:
        print()
        print("Extra phases and counts (median, strata/RocksDB): the pass after the run, the forced merge, the writes a read-while-writing run fit:")
        print()
        print("| set | workload | M MiB | after ops/s | after p99 µs | after misses | compact s | settle s | disk MiB | writes in run |")
        print("|---|---|---|---|---|---|---|---|---|---|")
        for k, g in extras:
            cells = []
            for pick in (
                lambda r: phase_named(r, "after", "ops_per_sec"), lambda r: phase_named(r, "after", "p99_us"), lambda r: phase_named(r, "after", "missed"),
                lambda r: phase_named(r, "compact", "seconds"), lambda r: phase_named(r, "settle", "seconds"), lambda r: r["disk"] / (1 << 20),
                lambda r: phase_named(r, "run", "writes"),
            ):
                v = [[pick(x) for x in g[e]] for e in ("strata", "rocksdb")]
                cells.append("/".join("-" if any(y is None for y in vs) else num(statistics.median(vs)) for vs in v))
            print(f"| {k[0]} | {k[1]} | {k[3]} | " + " | ".join(cells) + " |")
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
        cmd_calibrate(out, [int(x) for x in sys.argv[3].split(",")] if len(sys.argv) > 3 else [100, 1024])
    elif cmd == "dry":
        cmd_dry(out, sys.argv[3], int(sys.argv[4]) if len(sys.argv) > 4 else 2000)
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
