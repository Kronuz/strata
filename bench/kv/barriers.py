#!/usr/bin/env python3
"""M2 (standalone-turn-log): device flushes per operation on the durable rows (macOS).

  barriers.py OUT SET [ENGINES]   each row of SET (sync1, wal1) once per engine, under barriers.dylib

Counts every F_FULLFSYNC and fsync in the run phase (between its `phase` marks) and divides by
the run's operations. Environment as run.py's: KV_STRATA, KV_ROCKSDB, KV_VARIANT_<value size>.
ENGINES is a comma list (default strata,rocksdb); a binary named by KV_STRATA_MAIN runs as
`strata-main`, `main`'s driver for the before column.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import run


def dylib(out: Path) -> Path:
    lib = out / "barriers.dylib"
    if not lib.exists():
        subprocess.run(
            [
                "cc",
                "-O2",
                "-dynamiclib",
                str(Path(__file__).resolve().parent / "barriers.c"),
                "-o",
                str(lib),
            ],
            check=True,
        )
    return lib


def name_of(row: dict) -> str:
    return f"{run.label(row)} {row['dur']} b{row['batch']} v{row['value']}"


def one(engine: str, row: dict, out: Path, lib: Path) -> dict:
    binary = (
        os.environ["KV_STRATA_MAIN"]
        if engine == "strata-main"
        else run.env_bin("KV_STRATA" if engine == "strata" else "KV_ROCKSDB")
    )
    data = out / "data" / engine
    log = out / f"{engine}-{name_of(row)}.tsv".replace(" ", "_")
    args = [binary, "--dir", str(data)] + run.row_args(row)
    if engine == "rocksdb":
        args += [
            "--variant",
            os.environ.get(f"KV_VARIANT_{row['value']}", "lvl-4k-lz4"),
        ]
    env = dict(
        os.environ,
        BARRIERS_LOG=str(log),
        DYLD_INSERT_LIBRARIES=str(lib),
        KV_PHASE_MARKS="1",
    )
    p = subprocess.run(args, capture_output=True, text=True, env=env, check=False)
    if p.returncode:
        sys.stderr.write(p.stderr)
        raise SystemExit(f"{engine} failed")
    marks = [
        (ln.split()[1], int(ln.split()[2]))
        for ln in p.stderr.splitlines()
        if ln.startswith("phase ")
    ]
    starts = [t for name, t in marks if name == "run"]
    if not starts:
        raise SystemExit("no run phase mark")
    begin = starts[0]
    end = min([t for _, t in marks if t > begin], default=1 << 63)
    full = sync = 0
    for ln in log.read_text().splitlines():
        at, _, kind, _ = ln.split("\t", 3)
        if begin <= int(at) < end:
            full += kind == "F"
            sync += kind == "S"
    r = json.loads(p.stdout.strip().splitlines()[-1])
    ops = run.run_phase(r)["ops"]
    return {
        "engine": engine,
        "row": name_of(row),
        "ops": ops,
        "fullfsync": full,
        "fsync": sync,
        "per_op": (full + sync) / max(ops, 1),
    }


def main() -> None:
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    out, name = Path(sys.argv[1]), sys.argv[2]
    engines = (sys.argv[3] if len(sys.argv) > 3 else "strata,rocksdb").split(",")
    out.mkdir(parents=True, exist_ok=True)
    lib = dylib(out)
    with open(out / "barriers.jsonl", "a") as f:
        for row in run.sets()[name]:
            for engine in engines:
                r = one(engine, row, out, lib)
                f.write(json.dumps(r) + "\n")
                print(
                    f"{r['row']:<40} {engine:<12} ops {r['ops']:>8}  F_FULLFSYNC {r['fullfsync']:>7}  fsync {r['fsync']:>6}  per op {r['per_op']:.3f}"
                )


if __name__ == "__main__":
    main()
