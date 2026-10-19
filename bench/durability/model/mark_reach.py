#!/usr/bin/env python3
"""A reachability count for mark_model.py's design runs (DM-R24): how often the search reaches a
state whose newest epoch lost its original open record to reclamation while a copy holds its
fields, so the precision check (a process crash reads as the same boot) is exercised there.
Usage: python3 -I mark_reach.py [DEPTH]"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mark_model as m  # noqa: E402

m.VARIANT, m.OPEN_LIFE, m.COPY, m.SWITCHES, m.MAXVAL = "firstturn", "copy", True, 2, 2
depth = int(sys.argv[1]) if len(sys.argv) > 1 else 8
init = m.initial()
seen, level, reached, crashes = {init}, [init], 0, 0
for d in range(1, depth + 1):
    nxt = []
    for s in level:
        for label, t, check in m.successors(s):
            ep = t.epochs[-1]
            if not ep.open_alive and any(r.kind == "O" for tt in ep.turns for r in tt.recs):
                reached += 1
            if check and check[1] == "process":
                ep0 = s.epochs[-1]
                if not ep0.open_alive and any(r.kind == "O" for tt in ep0.turns for r in tt.recs):
                    crashes += 1
            if t not in seen:
                seen.add(t)
                nxt.append(t)
    level = nxt
    print(f"depth {d}: {len(seen):,} states; transitions into a state whose newest epoch's original open "
          f"record is reclaimed and a copy holds its fields: {reached:,}; process crashes from such a state: {crashes:,}")
