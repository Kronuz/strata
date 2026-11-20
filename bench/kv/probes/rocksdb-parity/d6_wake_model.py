#!/usr/bin/env python3
"""F12 of strata docs/design/rocksdb-parity.md: a wake (reopen) on a loop that does not own the
device's log, under D6's handoff and Detent's journal-idle-groups (journal:c3fb805, D4, D5).

One group g on loop L; the device log's turns run on the owner loop O. g starts closed: its table
entry (L's shard) is durable as `closed` at base b. Exhaustive over event sequences:
  W  L wakes g: the entry turns `open` in memory (the shard is dirty, no commit) and the table's
     record is staged again as g's checkpoint at max(b, retired) + 1 = b + 2 (idle-groups D4)
  J  a writer on L joins g: a batch record staged
  K  g's index checkpoints: a checkpoint record staged, covering every batch before it
  C  L commits its dirty shard (sync_files, rename, directory sync): the entry is durable `open`
  U  L clears g's gate under the driver mutex and wakes the owner (rule "gate" only)
  S  the owner's turn: seals g's staged records (unless gated), then its barrier makes them durable
     (rule "owner-commit": the owner first commits every loop's dirty shards under a table lock)
  P  L parks or closes g: nothing staged, its newest durable record a checkpoint covering every
     batch; the shard commits `closed` at that checkpoint's sequence, then the log forgets g
A power cut may follow any prefix. Recovery reads the table: a `closed` entry restores g from its
record and drops g's records above its base; an `open` entry replays them. A durable batch above
a `closed` base is an acknowledged write lost silently (idle-groups D5's lost alternative).
Rules:
  pull          the owner seals whatever g staged (D6's handoff as written)
  gate          "transition before records": a woken or created group is gated until its loop's
                shard commit is durable; the owner's turn skips a gated group's records
  owner-commit  the owner commits every loop's dirty shards in before_turn, under a table lock
Run: python3 -I d6_wake_model.py [depth]
"""

from __future__ import annotations

import sys
from dataclasses import dataclass, field, replace

RULES = ("pull", "gate", "owner-commit")


@dataclass(frozen=True)
class St:
    durable_entry: tuple = ("closed", 0)  # the table's durable entry: (state, base)
    mem_open: bool = False  # the entry in memory
    gated: bool = False
    staged: tuple = ()  # g's records staged, not sealed: (kind, seq)
    durable: tuple = ()  # g's records durable since its last wake: (kind, seq)
    base: int = 0
    next_seq: int = 1
    wakes: int = 0
    stalls: int = 0  # owner turns that paid a table commit before their write
    events: str = field(default="", compare=False)


def step(s: St, ev: str, rule: str, max_wakes: int) -> St | None:
    if ev == "W":
        if s.mem_open or s.wakes >= max_wakes:
            return None
        seq = s.base + 2  # max(base, retired) + 1, the retired floor being base + 1
        return replace(s, mem_open=True, gated=rule == "gate", staged=(("K", seq),), durable=(), next_seq=seq + 1, wakes=s.wakes + 1, events=s.events + ev)
    if ev == "J":
        if not s.mem_open:
            return None
        return replace(s, staged=s.staged + (("B", s.next_seq),), next_seq=s.next_seq + 1, events=s.events + ev)
    if ev == "K":
        newest = (s.durable + s.staged)[-1:] if (s.durable or s.staged) else ()
        if not s.mem_open or not newest or newest[0][0] != "B":
            return None
        return replace(s, staged=s.staged + (("K", s.next_seq),), next_seq=s.next_seq + 1, events=s.events + ev)
    if ev == "C":
        if not s.mem_open or s.durable_entry[0] == "open":
            return None
        return replace(s, durable_entry=("open", s.base), events=s.events + ev)
    if ev == "U":
        if rule != "gate" or not s.gated or s.durable_entry[0] != "open":
            return None
        return replace(s, gated=False, events=s.events + ev)
    if ev == "S":
        n, stall = s, 0
        if rule == "owner-commit" and s.mem_open and s.durable_entry[0] == "closed":
            n, stall = replace(n, durable_entry=("open", s.base)), 1
        if n.gated or not n.staged:
            if not stall:
                return None
            return replace(n, stalls=n.stalls + 1, events=s.events + ev)
        return replace(n, durable=n.durable + n.staged, staged=(), stalls=n.stalls + stall, events=s.events + ev)
    if ev == "P":
        if not s.mem_open or s.staged or s.gated or not s.durable or s.durable[-1][0] != "K":
            return None
        base = s.durable[-1][1]
        return replace(s, durable_entry=("closed", base), mem_open=False, durable=(), base=base, events=s.events + ev)
    return None


def lost(s: St) -> list:
    """g's acknowledged batches a power cut now loses silently."""
    if s.durable_entry[0] == "closed":
        return [q for (kind, q) in s.durable if kind == "B" and q > s.durable_entry[1]]
    return []


def explore(rule: str, depth: int, max_wakes: int) -> dict:
    out = {"states": 0, "bad": 0, "stalled_turns": 0, "example": ""}
    seen, frontier = set(), [St()]
    for _ in range(depth + 1):
        nxt = []
        for s in frontier:
            key = replace(s, events="")
            if key in seen:
                continue
            seen.add(key)
            out["states"] += 1
            if lost(s):
                out["bad"] += 1
                out["example"] = out["example"] or s.events
            for ev in "WJKCUSP":
                n = step(s, ev, rule, max_wakes)
                if n is not None:
                    if n.stalls > s.stalls:
                        out["stalled_turns"] += 1
                    nxt.append(n)
        frontier = nxt
    return out


def main() -> int:
    depth = int(sys.argv[1]) if len(sys.argv) > 1 else 12
    print(f"depth {depth}, up to 2 wakes: states, power-cut states losing an acknowledged batch under a closed entry, and owner turns that paid a table commit")
    runs = {}
    for rule in RULES:
        r = runs[rule] = explore(rule, depth, 2)
        print(f"rule={rule:13s} states={r['states']:7d} bad={r['bad']:6d} stalled_turns={r['stalled_turns']:6d} first bad: {r['example'] or '-'}")
    claims = {
        "pull: a woken group's batch is durable while the table says closed": runs["pull"]["bad"] > 0,
        "gate: no state loses an acknowledged batch": runs["gate"]["bad"] == 0,
        "gate: no owner turn pays a table commit": runs["gate"]["stalled_turns"] == 0,
        "owner-commit: no state loses, but owner turns pay table commits": runs["owner-commit"]["bad"] == 0 and runs["owner-commit"]["stalled_turns"] > 0,
    }
    for k, v in claims.items():
        print(f"claim: {k}: {'yes' if v else 'NO'}")
    ok = all(claims.values())
    print("all claims hold" if ok else "unexpected")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
