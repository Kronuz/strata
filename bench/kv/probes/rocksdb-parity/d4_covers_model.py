#!/usr/bin/env python3
"""D4 of strata docs/design/rocksdb-parity.md: what a background flush's checkpoint may supersede,
and what a close hands the table. The design review's model (round 2, job 261120-124240-07f24d),
extended with the close rules, leave() as D4 designs it, and a reopen.

Exhaustive over event sequences of one logged group (states deduplicated):
  J  a transaction joins the open record (opening one if none) and applies at {s, i}
  C  chunk 1 of a two-chunk transaction joins the open record; the next J is its last chunk, and
     the transaction applies whole at that J's position (A3's shape). Only T or J follow a C: the
     writer holds the lease through the transaction.
  T  a turn seals the closed records, the open record and a staged checkpoint; its barrier
     returns, so they are durable
  F  freeze: X_f = the applied position (one frozen buffer at a time)
  I  install: the frozen segment's commit_files makes the head X_f; then the checkpoint is staged
     at the next sequence N, closing the open record first, with `covers` per the rule
  L  close: the newest checkpoint, durable and the group's last record, goes to the table as base
     if the close rule accepts it; records through base are superseded
  D  leave() as D4 designs it: every build installs, the active buffer freezes and installs, the
     turn seals, then L
  R  reopen after a close: the table's record is staged again at max(base, retired) + 1, which is
     base + 2 (journal's retired floor is the close's sequence + 1), covering base
A power cut may follow any prefix. Recovery: the newest durable checkpoint supersedes records
through its covers (and earlier checkpoints); strata replays the rest in sequence order,
skipping positions at or below the head; a two-chunk transaction applies only whole.
Must hold: every transaction whose last record is durable is in the reopened index.

Rules for covers (F8):
  note           covers = X_f.index (D4 item 3 at strata:e5721fd6)
  freeze-closes  F closes the open record, so X_f ends its record; covers = X_f.index
  prev-if-shared covers = X_f.index, or X_f.index - 1 when that record holds a later position
Close rules (F10):
  none  the table takes any last checkpoint
  seq   the checkpoint's covers is its sequence - 1 (the review's statement)
  live  the checkpoint is the group's only live record: no durable batch above its covers

Run: python3 -I d4_covers_model.py [depth]
"""

from __future__ import annotations

import sys
from dataclasses import dataclass, field, replace

RULES = ("note", "freeze-closes", "prev-if-shared")
CLOSES = ("none", "seq", "live")


@dataclass(frozen=True)
class Rec:
    seq: int
    kind: str  # 'B' batch, 'K' checkpoint
    items: tuple = ()  # batch: ((s, i, what), ...) what: 'tx', 'c1', 'commit'
    covers: int = 0
    at: tuple = (0, 0)  # checkpoint: X


@dataclass(frozen=True)
class St:
    next_seq: int = 1
    open_seq: int = 0
    open_items: tuple = ()
    closed: tuple = ()  # Recs closed, not yet sealed
    durable: tuple = ()  # this life's Recs durable, in sequence order
    applied: tuple = (0, 0)
    head: tuple = (0, 0)  # the index's durable manifest position
    frozen: tuple | None = None
    staged_cp: Rec | None = None
    pending_chunk: bool = False
    applied_txs: tuple = ()  # positions of applied transactions (the last chunk's for two-chunk)
    closed_group: Rec | None = None  # the table's record while the group is closed
    table_base: int = 0  # the table's base after a reopen: records through it are superseded
    events: str = field(default="", compare=False)


def durable_through(s: St) -> int:
    # Turns seal in sequence order, so a life's durable records are contiguous from its start;
    # the table's base is durable too.
    return max([r.seq for r in s.durable] + [s.table_base])


def drained(s: St) -> bool:
    """The group as a close finds it: nothing open, closed, staged or frozen, and its last durable
    record a checkpoint with nothing after it."""
    if s.closed_group is not None or s.open_seq or s.closed or s.staged_cp is not None or s.frozen is not None or not s.durable:
        return False
    last = s.durable[-1]
    return last.kind == "K" and last.seq + 1 == s.next_seq


def close_ok(close: str, s: St) -> bool:
    last = s.durable[-1]
    if close == "seq":
        return last.covers == last.seq - 1
    if close == "live":
        return not any(r.kind == "B" and r.seq > last.covers for r in s.durable)
    return True


def step(s: St, ev: str, rule: str, install_waits: bool, close: str) -> St | None:
    if s.closed_group is not None and ev != "R":
        return None
    if s.pending_chunk and ev not in ("T", "J"):
        return None
    if ev in ("J", "C"):
        if ev == "C" and s.pending_chunk:
            return None
        seq, items, nxt = s.open_seq, s.open_items, s.next_seq
        if not seq:
            seq, items, nxt = nxt, (), nxt + 1
        pos = (seq, len(items) + 1)
        if ev == "C":
            return replace(s, open_seq=seq, open_items=items + ((pos[0], pos[1], "c1"),), next_seq=nxt, pending_chunk=True, events=s.events + ev)
        what = "commit" if s.pending_chunk else "tx"
        return replace(
            s,
            open_seq=seq,
            open_items=items + ((pos[0], pos[1], what),),
            next_seq=nxt,
            applied=pos,
            applied_txs=s.applied_txs + (pos,),
            pending_chunk=False,
            events=s.events + ev,
        )
    if ev == "T":
        recs = list(s.closed)
        if s.open_seq:
            recs.append(Rec(s.open_seq, "B", s.open_items))
        if s.staged_cp is not None:
            recs.append(s.staged_cp)
        if not recs:
            return None
        recs.sort(key=lambda r: r.seq)
        return replace(
            s, closed=(), open_seq=0, open_items=(), staged_cp=None, durable=tuple(sorted(s.durable + tuple(recs), key=lambda r: r.seq)), events=s.events + ev
        )
    if ev == "F":
        if s.frozen is not None or s.applied <= s.head:
            return None
        n = replace(s, frozen=s.applied, events=s.events + ev)
        if rule == "freeze-closes" and s.open_seq:
            n = replace(n, closed=s.closed + (Rec(s.open_seq, "B", s.open_items),), open_seq=0, open_items=())
        return n
    if ev == "I":
        if s.frozen is None or s.staged_cp is not None:
            return None
        xf = s.frozen
        if install_waits and durable_through(s) < xf[0]:
            return None  # the install waits for the log through X_f.index (F9)
        closed = s.closed
        if s.open_seq:  # the checkpoint closes the open record first
            closed = closed + (Rec(s.open_seq, "B", s.open_items),)
        allrecs = {r.seq: r for r in s.durable + closed}
        cov = xf[0]
        if rule == "prev-if-shared":
            r = allrecs.get(xf[0])
            if r is not None and any((a, b) > xf for (a, b, _) in r.items):
                cov = xf[0] - 1
        cp = Rec(s.next_seq, "K", covers=cov, at=xf)
        return replace(s, closed=closed, open_seq=0, open_items=(), next_seq=s.next_seq + 1, head=xf, frozen=None, staged_cp=cp, events=s.events + ev)
    if ev == "L":
        if not drained(s) or not close_ok(close, s):
            return None
        return replace(s, closed_group=s.durable[-1], events=s.events + ev)
    if ev == "D":
        return leave(s, rule, install_waits, close)
    if ev == "R":
        if s.closed_group is None:
            return None
        base, at = s.closed_group.seq, s.closed_group.at
        cp = Rec(base + 2, "K", covers=base, at=at)
        return replace(s, closed_group=None, table_base=base, durable=(), staged_cp=cp, next_seq=base + 3, events=s.events + ev)
    return None


REFUSED = St(events="refused")


def leave(s: St, rule: str, install_waits: bool, close: str) -> St | None:
    """leave() under the lease (D4, F10): builds install in freeze order, the active buffer
    freezes and installs, the turn seals, then the close. REFUSED when the close rule refuses the
    drained group's checkpoint."""
    if s.closed_group is not None or s.pending_chunk or not (s.applied > s.head or s.frozen is not None or s.durable):
        return None
    t = s
    for _ in range(16):
        if t.frozen is not None:
            n = step(t, "I", rule, install_waits, close) or step(t, "T", rule, install_waits, close)
        elif t.staged_cp is not None or t.open_seq or t.closed:
            n = step(t, "T", rule, install_waits, close)
        elif t.applied > t.head:
            n = step(t, "F", rule, install_waits, close)
        else:
            break
        if n is None:
            return None
        t = n
    if not drained(t):
        return None
    if not close_ok(close, t):
        return REFUSED
    return replace(t, closed_group=t.durable[-1], events=s.events + "D")


def recover(s: St) -> tuple[str, list]:
    """Returns ('ok' | 'loud' | 'loss' | 'torn', details) for a power cut now."""
    must = []
    for r in s.durable:
        if r.kind == "B":
            must += [(a, b) for (a, b, w) in r.items if w in ("tx", "commit")]
    if s.closed_group is not None:
        cp = s.closed_group
        if s.head < cp.at:
            return "loud", ["head older than the table's checkpoint"]
        if cp.at < s.head:
            return "loud", ["head past the table's checkpoint"]
        lost = [p for p in must if p > s.head]
        return ("loss", lost) if lost else ("ok", [])
    if not s.durable and s.head == (0, 0):
        return "ok", []
    if s.head[0] > durable_through(s):
        return "loud", ["head names records the log lacks"]
    cps = [r for r in s.durable if r.kind == "K"]
    if cps:
        cp = cps[-1]
        kept = [r for r in s.durable if r.seq > cp.covers and (r.kind == "B" or r is cp)]
    else:
        kept = list(s.durable)
    got = {p for p in s.applied_txs if p <= s.head}
    chunk_seen = False
    for r in kept:
        if r.kind != "B":
            continue
        for a, b, w in r.items:
            p = (a, b)
            if w == "c1":
                chunk_seen = p > s.head
                continue
            if p <= s.head:
                continue
            if w == "commit" and not chunk_seen:
                return "torn", [p]
            got.add(p)
            chunk_seen = False
    lost = [p for p in must if p not in got]
    return ("loss", lost) if lost else ("ok", [])


def exposed(s: St) -> bool:
    # A reader at V sees the installed segment whole: content past the log's durable records.
    return s.head[0] > durable_through(s)


def explore(rule: str, install_waits: bool, close: str, depth: int) -> dict:
    out = {"states": 0, "ok": 0, "loud": 0, "loss": 0, "torn": 0, "exposed": 0, "false_refusal": 0, "leave_refused": 0, "example": {}}
    seen = set()
    frontier = [St()]
    for _ in range(depth + 1):
        nxt = []
        for s in frontier:
            key = replace(s, events="")
            if key in seen:
                continue
            seen.add(key)
            out["states"] += 1
            verdict, detail = recover(s)
            out[verdict] += 1
            if verdict != "ok" and verdict not in out["example"]:
                out["example"][verdict] = (s.events, detail)
            if exposed(s):
                out["exposed"] += 1
                out["example"].setdefault("exposed", (s.events, [s.head]))
            if drained(s) and not close_ok(close, s) and close_ok("live", s):
                out["false_refusal"] += 1  # a close of a group whose checkpoint is its only live record
                out["example"].setdefault("false_refusal", (s.events, [s.durable[-1]]))
            for ev in "JCTFILDR":
                n = step(s, ev, rule, install_waits, close)
                if n is REFUSED:
                    out["leave_refused"] += 1
                    out["example"].setdefault("leave_refused", (s.events + "D", []))
                elif n is not None:
                    nxt.append(n)
        frontier = nxt
    return out


def main() -> int:
    depth = int(sys.argv[1]) if len(sys.argv) > 1 else 10
    print(
        f"depth {depth}: states, then power cuts that recover ok / loud (log_lost) / silent loss / torn transaction, "
        "states exposing writes past V, drained closes refused although the checkpoint is the only live record, "
        "and leave() calls the close rule refused"
    )
    runs = {}
    for install_waits in (True, False):
        for close in CLOSES:
            for rule in RULES:
                r = explore(rule, install_waits, close, depth)
                runs[(rule, install_waits, close)] = r
                print(
                    f"covers={rule:15s} install_waits_log={install_waits!s:5s} close={close:5s} "
                    f"states={r['states']:6d} ok={r['ok']:6d} loud={r['loud']:5d} loss={r['loss']:5d} torn={r['torn']:4d} "
                    f"exposed={r['exposed']:5d} false_refusal={r['false_refusal']:4d} leave_refused={r['leave_refused']:4d}"
                )
                for k, (ev, d) in sorted(r["example"].items()):
                    print(f"    first {k}: events {ev} -> {d}")

    def bad(r: dict) -> int:
        return r["loss"] + r["torn"] + r["loud"] + r["exposed"]

    design = runs[("freeze-closes", True, "live")]
    note = runs[("note", True, "live")]
    claims = {
        "F8: D4 as written loses acknowledged transactions": note["loss"] + note["torn"] > 0,
        "F8: the freeze closing the record loses none": bad(runs[("freeze-closes", True, "seq")]) == 0 and bad(design) == 0,
        "F8: covers one record back when shared loses none": bad(runs[("prev-if-shared", True, "live")]) == 0,
        "F9: an install before the log is durable exposes writes past V": runs[("freeze-closes", False, "live")]["exposed"] > 0,
        "F10: a close that takes a partial checkpoint loses transactions": runs[("freeze-closes", True, "none")]["loss"] > 0,
        "F10: covers = sequence - 1 refuses the close of a reopened group that wrote nothing": runs[("freeze-closes", True, "seq")]["false_refusal"] > 0,
        "F10: the only-live-record rule refuses no such close": design["false_refusal"] == 0,
        "the design (freeze closes, install waits, only live record) loses, tears, exposes and refuses nothing": bad(design) == 0
        and design["false_refusal"] == 0
        and design["leave_refused"] == 0,
    }
    for k, v in claims.items():
        print(f"claim: {k}: {'yes' if v else 'NO'}")
    if not all(claims.values()):
        print("unexpected")
        return 1
    print("all claims hold")
    return 0


if __name__ == "__main__":
    sys.exit(main())
