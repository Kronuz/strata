#!/usr/bin/env python3
"""format-14-kv's micro-prototypes: run them interleaved over rounds, then summarize.

  run.py blocks OUT ROUNDS     block layouts and codecs (single-threaded: a light hold)
  run.py cache OUT ROUNDS      the decoded-block cache at 1..16 threads (a strict hold)
  run.py buffer OUT ROUNDS     the buffer's sorted view (single-threaded: a light hold)
  run.py bufmem OUT ROUNDS     the current document-layout buffer's bytes per record
  run.py report OUT            medians per configuration, as markdown tables

Binaries come from $KV_PROTO (default build-rel). OUT is a directory; each run appends one JSON
line to OUT/<kind>.jsonl, with the round and the configuration's wall time.
"""

from __future__ import annotations

import json
import os
import statistics
import subprocess
import sys
import time
from typing import Any

Config = tuple[str, list[str]]

BIN = os.environ.get("KV_PROTO", "build-rel")


def blocks_configs() -> list[Config]:
    out: list[Config] = []
    for value, records in ((100, 1_000_000), (1024, 200_000)):
        base = ["--records", str(records), "--value", str(value), "--ms", "700"]
        out.append(("frame64", base + ["--layout", "frame64", "--codec", "zstd3"]))
        for block in (4096, 8192, 16384):
            for codec in ("none", "lz4", "zstd1", "zstd3"):
                out.append(
                    (
                        "inline",
                        base
                        + [
                            "--layout",
                            "inline",
                            "--codec",
                            codec,
                            "--block",
                            str(block),
                        ],
                    )
                )
            for codec in ("none", "lz4"):
                out.append(
                    (
                        "split",
                        base
                        + [
                            "--layout",
                            "split",
                            "--codec",
                            codec,
                            "--block",
                            str(block),
                        ],
                    )
                )
    return [(f"{BIN}/kv_proto_blocks", args) for _, args in out]


def cache_configs() -> list[Config]:
    out: list[Config] = []
    # hot: every block fits and is prefilled (hits only); zipf: a third of the universe fits.
    modes = (
        ("hot", ["--universe", "32768", "--capacity", "65536", "--prefill", "1"]),
        ("zipf", ["--universe", "200000", "--capacity", "65536"]),
    )
    for impl in ("clock", "lru"):
        for _, mode in modes:
            for threads in (1, 4, 8, 16):
                out.append(
                    (
                        f"{BIN}/kv_proto_cache",
                        ["--impl", impl, "--threads", str(threads), "--ms", "1500"]
                        + mode,
                    )
                )
    return out


def buffer_configs() -> list[Config]:
    out: list[Config] = []
    for impl in ("skiplist", "btree", "sortrun", "sortrun2"):
        for value in (100, 1024):
            records = 1_000_000 if value == 100 else 200_000
            out.append(
                (
                    f"{BIN}/kv_proto_buffer",
                    ["--impl", impl, "--records", str(records), "--value", str(value)],
                )
            )
    return out


def bufmem_configs() -> list[Config]:
    return [
        (f"{BIN}/kv_proto_bufmem", ["--dir", ".scratch/bufmem", "--value", str(v)])
        for v in (100, 1024)
    ]


KINDS = {
    "blocks": blocks_configs,
    "cache": cache_configs,
    "buffer": buffer_configs,
    "bufmem": bufmem_configs,
}


def run(kind: str, out: str, rounds: int) -> None:
    os.makedirs(out, exist_ok=True)
    configs = KINDS[kind]()
    with open(os.path.join(out, f"{kind}.jsonl"), "a") as f:
        for r in range(rounds):
            # Interleaved: every configuration once per round, alternating direction.
            order = configs if r % 2 == 0 else list(reversed(configs))
            for exe, args in order:
                t0 = time.time()
                res = subprocess.run(
                    [exe] + args, capture_output=True, text=True, check=True
                )
                for line in res.stdout.splitlines():
                    if line.startswith("{"):
                        row = json.loads(line)
                        row["round"] = r
                        row["wall_s"] = round(time.time() - t0, 2)
                        f.write(json.dumps(row) + "\n")
                        f.flush()
                        print(
                            kind, r, " ".join(args), row.get("wall_s"), file=sys.stderr
                        )


def median_rows(path: str, key_fields: tuple[str, ...]) -> list[dict[str, Any]]:
    with open(path) as f:
        rows = [json.loads(line) for line in f]
    groups: dict[tuple[Any, ...], list[dict[str, Any]]] = {}
    for row in rows:
        groups.setdefault(tuple(row.get(k) for k in key_fields), []).append(row)
    out: list[dict[str, Any]] = []
    for rs in groups.values():
        med: dict[str, Any] = {}
        for field in rs[0]:
            vals = [r[field] for r in rs if isinstance(r.get(field), (int, float))]
            med[field] = statistics.median(vals) if vals else rs[0][field]
        med["n"] = len(rs)
        if rs and isinstance(rs[0].get("hit_ns"), (int, float)):
            vals = [r["hit_ns"] for r in rs]
            med["hit_spread"] = (
                (max(vals) - min(vals)) / statistics.median(vals)
                if statistics.median(vals)
                else 0
            )
        out.append(med)
    return out


def report(out: str) -> None:
    p = os.path.join(out, "blocks.jsonl")
    if os.path.exists(p):
        rows = median_rows(p, ("layout", "codec", "block", "value"))
        rows.sort(key=lambda r: (r["value"], r["layout"], r["block"], r["codec"]))
        print("## blocks (medians)\n")
        print(
            "| value | layout | block | codec | bytes/logical | index B/rec | key B/rec | build ns/rec | key ns | hit ns | miss ns | seek10 ns | scan ns/rec | n | hit spread |"
        )
        print(
            "|---|---|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|"
        )
        for r in rows:
            rec = r["records"]
            print(
                f"| {r['value']} | {r['layout']} | {r['block']} | {r['codec']} | {(r['bytes'] + r['key_bytes']) / r['logical']:.3f} | "
                f"{r['index_bytes'] / rec:.2f} | {r['key_bytes'] / rec:.2f} | {r['build_ns']:.0f} | {r['key_ns']:.0f} | {r['hit_ns']:.0f} | "
                f"{r['miss_ns']:.0f} | {r['seek10_ns']:.0f} | {r['scan_ns']:.1f} | {r['n']} | {r.get('hit_spread', 0):.2f} |"
            )
        print()
    p = os.path.join(out, "cache.jsonl")
    if os.path.exists(p):
        rows = median_rows(p, ("impl", "prefill", "threads"))
        rows.sort(key=lambda r: (r["prefill"], r["impl"], r["threads"]))
        print("## cache (medians)\n")
        print(
            "| mode | impl | threads | Mops/s total | ns/op per thread | hit rate | n |"
        )
        print("|---|---|---:|---:|---:|---:|---:|")
        for r in rows:
            mode = "hot" if r["prefill"] else "zipf"
            print(
                f"| {mode} | {r['impl']} | {r['threads']} | {r['mops']:.2f} | {r['ns_per_op']:.0f} | {r['hit_rate']:.3f} | {r['n']} |"
            )
        print()
    p = os.path.join(out, "buffer.jsonl")
    if os.path.exists(p):
        rows = median_rows(p, ("impl", "value"))
        rows.sort(key=lambda r: (r["value"], r["impl"]))
        print("## buffer (medians)\n")
        print(
            "| impl | value | B/record (accounted) | B/record (RSS) | insert ns | get ns | seek10 ns | ycsb-e ns/op | sorted-view ns/rec | n |"
        )
        print("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
        for r in rows:
            print(
                f"| {r['impl']} | {r['value']} | {r['bytes_per_record']:.1f} | {r['rss_per_record']:.1f} | {r['insert_ns']:.0f} | {r['get_ns']:.0f} | "
                f"{r['seek10_ns']:.0f} | {r['ycsb_e_ns']:.0f} | {r['sort_ns']:.1f} | {r['n']} |"
            )
        print()
    p = os.path.join(out, "bufmem.jsonl")
    if os.path.exists(p):
        rows = median_rows(p, ("value",))
        print("## current buffer (medians)\n")
        print("| value | RSS B/record | n |")
        print("|---:|---:|---:|")
        for r in rows:
            print(f"| {r['value']} | {r['rss_per_record']:.1f} | {r['n']} |")


if __name__ == "__main__":
    if sys.argv[1] == "report":
        report(sys.argv[2])
    else:
        run(sys.argv[1], sys.argv[2], int(sys.argv[3]))
