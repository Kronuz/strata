#!/usr/bin/env python3
"""Writes the two sparse corpora as NDJSON for xapiand_terms.py (fields-and-keys, FK-R3).

Usage: sparse_corpora.py OUTDIR
  sparse-kw.ndjson   1,000 docs, each 20 of 2,000 one-word string fields (terms.py's sparse, seed 7)
  sparse-num.ndjson  1,000 docs, each 20 of 300 integer fields in 0..10^6 (seed 11); Xapiand
                     indexes each with six accuracy levels, so 300 paths become 2,100 engine fields
"""

import json
import random
import sys

out = sys.argv[1]
random.seed(7)
vocab = [
    "red",
    "green",
    "blue",
    "alpha",
    "beta",
    "gamma",
    "delta",
    "on",
    "off",
    "yes",
    "no",
]
with open(f"{out}/sparse-kw.ndjson", "w") as o:
    o.writelines(
        json.dumps(
            {
                f"attr_{random.randrange(2000):04d}": random.choice(vocab)
                for _ in range(20)
            }
        )
        + "\n"
        for _ in range(1000)
    )
random.seed(11)
with open(f"{out}/sparse-num.ndjson", "w") as o:
    o.writelines(
        json.dumps(
            {
                f"m_{random.randrange(300):03d}": random.randrange(10**6)
                for _ in range(20)
            }
        )
        + "\n"
        for _ in range(1000)
    )
