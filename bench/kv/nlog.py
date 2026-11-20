#!/usr/bin/env python3
"""M3's N-log and group-commit rows (standalone-turn-log Q1, D11): K writers, one index each, on L
turn logs of one host (KV_LOOPS), against RocksDB's K threads on one DB, fillrandom durable.

  nlog.py OUT [TRIALS] [BATCH]   K, L in (1,1) (4,1) (4,4) (8,1) (8,8); RocksDB at K = 1, 4, 8

L = 1 with K > 1 is group commit across indexes: one barrier per turn for every index on the
loop. Environment as run.py's: KV_STRATA, KV_ROCKSDB, KV_VARIANT_100.
"""

from __future__ import annotations

import json
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import run

CASES = [(1, 1), (4, 1), (4, 4), (8, 1), (8, 8)]


def main() -> None:
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    out = Path(sys.argv[1])
    trials = int(sys.argv[2]) if len(sys.argv) > 2 else 2
    batch = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    out.mkdir(parents=True, exist_ok=True)
    with open(out / "nlog.jsonl", "a") as f:
        for trial in range(trials):
            runs = [("strata", k, loops) for k, loops in CASES] + [
                ("rocksdb", k, 0) for k in (1, 4, 8)
            ]
            for engine, k, loops in runs if trial % 2 == 0 else reversed(runs):
                row = {
                    "w": "fillrandom",
                    "value": 100,
                    "mb": run.FITS_MB,
                    "threads": k,
                    "dur": "sync",
                    "batch": batch,
                }
                os.environ["KV_LOOPS"] = str(max(loops, 1))
                r = run.run_one(engine, row, out, trial)
                r["loops"] = loops
                f.write(json.dumps(r) + "\n")
                p = run.run_phase(r)
                print(
                    f"{engine:<8} K={k} L={loops} trial {trial}: {p['ops_per_sec']:.1f} ops/s  p50 {p['p50_us']:.0f} us"
                    f"  p99 {p['p99_us']:.0f} us  p99.9 {p['p999_us']:.0f} us",
                    flush=True,
                )


if __name__ == "__main__":
    main()
