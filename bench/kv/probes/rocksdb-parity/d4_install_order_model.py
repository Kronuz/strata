#!/usr/bin/env python3
"""D4 of strata docs/design/rocksdb-parity.md: two frozen buffers in flight (F9). The design
review's model (round 2, job 261120-124622-b44851), extended with the log wait as a variable and
the states that expose writes past V.

Exhaustive over event sequences of one logged group, with up to two frozen buffers building at
once and the checkpoint rule fixed (the freeze closes the open record, F8):
  J  a transaction joins the open record and applies at {s, i}
  T  a turn seals the closed and open records and the staged checkpoints; they are durable
  F  freeze: the active buffer's transactions since the last freeze become a frozen buffer
  I  the oldest frozen buffer installs: commit_files (head := max(head, its X)), then its
     checkpoint, at the next sequence, covering its X's record
  O  the newest frozen buffer installs first (a second build finishing first); its checkpoint
     waits for the older one's, in freeze order
With the wait, an install waits until the log is durable through its X.index (F9a). A power cut
may follow any prefix. Recovery replays the records after the newest durable checkpoint's covers,
skipping positions at or below the head; the index holds the installed buffers' transactions.
A state exposes writes past V when an installed segment holds a position whose record is not
durable: views read segments whole, so they would see a crash-removable write.

Run: python3 -I d4_install_order_model.py [depth]
"""

from __future__ import annotations

import sys
from dataclasses import dataclass, replace


@dataclass(frozen=True)
class St:
    next_seq: int = 1
    open_seq: int = 0
    open_n: int = 0
    closed: tuple = ()  # (seq, n) closed, not sealed
    durable: tuple = ()  # (seq, kind, n_or_covers)
    applied: tuple = (0, 0)
    txs: tuple = ()  # applied positions
    frozen: tuple = ()  # (lo, hi] ranges in freeze order, not installed
    installed: tuple = ()  # ranges installed
    head: tuple = (0, 0)
    waiting_cp: tuple = ()  # X of installs whose checkpoint waits for an older one
    staged: tuple = ()  # checkpoints staged: (seq, covers)
    last_frozen: tuple = (0, 0)
    events: str = ""


def through(s: St) -> int:
    n = 0
    for q in sorted(r[0] for r in s.durable):
        if q != n + 1:
            break
        n = q
    return n


def close_open(s: St) -> St:
    if not s.open_seq:
        return s
    return replace(s, closed=s.closed + ((s.open_seq, s.open_n),), open_seq=0, open_n=0)


def stage_cps(s: St) -> St:
    # Checkpoints in freeze order: one per install whose older buffers all installed.
    staged, waiting, nxt = list(s.staged), list(s.waiting_cp), s.next_seq
    pending_lo = [lo for (lo, hi) in s.frozen]
    for x in sorted(waiting):
        if any(lo < x for lo in pending_lo):
            continue  # an older frozen buffer is not installed yet
        staged.append((nxt, x[0]))
        nxt += 1
        waiting.remove(x)
    return replace(s, staged=tuple(staged), waiting_cp=tuple(waiting), next_seq=nxt)


def step(s: St, ev: str, out_of_order: bool, waits: bool) -> St | None:
    if ev == "J":
        seq, n, nxt = s.open_seq, s.open_n, s.next_seq
        if not seq:
            seq, n, nxt = nxt, 0, nxt + 1
        pos = (seq, n + 1)
        return replace(s, open_seq=seq, open_n=n + 1, next_seq=nxt, applied=pos, txs=s.txs + (pos,), events=s.events + ev)
    if ev == "T":
        recs = [(q, "B", n) for (q, n) in s.closed]
        if s.open_seq:
            recs.append((s.open_seq, "B", s.open_n))
        recs += [(q, "K", c) for (q, c) in s.staged]
        if not recs:
            return None
        return replace(s, closed=(), open_seq=0, open_n=0, staged=(), durable=tuple(sorted(s.durable + tuple(recs))), events=s.events + ev)
    if ev == "F":
        if len(s.frozen) >= 2 or s.applied <= s.last_frozen:
            return None
        n = close_open(s)  # the freeze closes the open record (F8)
        return replace(n, frozen=s.frozen + ((s.last_frozen, s.applied),), last_frozen=s.applied, events=s.events + ev)
    if ev in ("I", "O"):
        if not s.frozen or (ev == "O" and (not out_of_order or len(s.frozen) < 2)):
            return None
        k = 0 if ev == "I" else len(s.frozen) - 1
        lo, hi = s.frozen[k]
        if waits and through(s) < hi[0]:
            return None  # F9a: the install waits for the log through its X.index
        n = close_open(s)
        n = replace(
            n,
            frozen=s.frozen[:k] + s.frozen[k + 1 :],
            installed=s.installed + ((lo, hi),),
            head=max(s.head, hi),
            waiting_cp=s.waiting_cp + (hi,),
            events=s.events + ev,
        )
        return stage_cps(n)
    return None


def recover(s: St) -> str:
    must = set()
    for q, kind, n in s.durable:
        if kind == "B":
            must |= {(q, i) for i in range(1, n + 1)}
    if s.head[0] > through(s):
        return "loud"  # the head names records the log lacks: log_lost
    cps = [(q, c) for (q, kind, c) in s.durable if kind == "K"]
    covers = cps[-1][1] if cps else 0
    got = {p for p in s.txs if any(lo < p <= hi for (lo, hi) in s.installed)}
    for q, kind, n in s.durable:
        if kind == "B" and q > covers:
            got |= {(q, i) for i in range(1, n + 1) if (q, i) > s.head}
    return "loss" if must - got else "ok"


def exposed(s: St) -> bool:
    return any(p[0] > through(s) for p in s.txs if any(lo < p <= hi for (lo, hi) in s.installed))


def explore(out_of_order: bool, waits: bool, depth: int) -> dict:
    out = {"states": 0, "ok": 0, "loud": 0, "loss": 0, "exposed": 0, "example": {}}
    seen, frontier = set(), [St()]
    for _ in range(depth + 1):
        nxt = []
        for s in frontier:
            key = replace(s, events="")
            if key in seen:
                continue
            seen.add(key)
            out["states"] += 1
            v = recover(s)
            out[v] += 1
            if v != "ok":
                out["example"].setdefault(v, s.events)
            if exposed(s):
                out["exposed"] += 1
                out["example"].setdefault("exposed", s.events)
            for ev in "JTFIO":
                n = step(s, ev, out_of_order, waits)
                if n is not None:
                    nxt.append(n)
        frontier = nxt
    return out


def main() -> int:
    depth = int(sys.argv[1]) if len(sys.argv) > 1 else 10
    print(f"depth {depth}: states, power cuts recovering ok / loud (log_lost) / silent loss, and states exposing writes past V")
    runs = {}
    for name, ooo, waits in (
        ("design: freeze order, install waits for the log", False, True),
        ("a newer build installs first, waits", True, True),
        ("freeze order, no log wait", False, False),
    ):
        r = runs[name] = explore(ooo, waits, depth)
        firsts = ", ".join(f"{k} {v}" for k, v in sorted(r["example"].items())) or "-"
        print(f"{name:48s} states={r['states']:7d} ok={r['ok']:7d} loud={r['loud']:6d} loss={r['loss']:6d} exposed={r['exposed']:6d} first: {firsts}")
    design, newer, nowait = runs.values()
    claims = {
        "F9: the design loses nothing, reports no log_lost and exposes nothing": design["loss"] + design["loud"] + design["exposed"] == 0,
        "F9b: a newer build installing first loses acknowledged transactions": newer["loss"] > 0,
        "F9a: an install before the log wait exposes writes past V and leaves log_lost states": nowait["exposed"] > 0 and nowait["loud"] > 0,
    }
    for k, v in claims.items():
        print(f"claim: {k}: {'yes' if v else 'NO'}")
    ok = all(claims.values())
    print("all claims hold" if ok else "unexpected")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
