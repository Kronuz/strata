"""A prospect churn op stream (upserts, deletes, commits, COUNT queries) for
test/prospect_integration.sh's identity check. `longkey` puts a 2000-byte id first (CR1 on the
inverting layout)."""

from __future__ import annotations

import random
import sys


def main() -> None:
    mode = sys.argv[1] if len(sys.argv) > 1 else "plain"
    rng = random.Random(7)
    words = [f"w{i}" for i in range(400)]
    out: list[str] = []
    if mode == "longkey":
        out.append("P\t" + "x" * 2000 + "\tw1 w2 w3")
        out.append("C")
        out.append("Q\tCOUNT\tw1")
    else:
        for i in range(30000):
            r = rng.random()
            doc = rng.randrange(6000)
            if r < 0.85:
                n = rng.randrange(1, 60)
                out.append(
                    f"P\t{doc}\t" + " ".join(rng.choice(words) for _ in range(n))
                )
            else:
                out.append(f"D\t{doc}")
            if i % 2500 == 2499:
                out.append("C")
                for q in ("w1", "+w2 +w3", "w4 w5 -w6", '"w7 w8"', "w9 w10 w11"):
                    out.append(f"Q\tCOUNT\t{q}")
                    out.append(f"Q\tTOP_10_COUNT\t{q}")
    sys.stdout.write("\n".join(out) + "\n")


main()
