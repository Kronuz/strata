#!/usr/bin/env python3
"""durability-modes revision 7, D8 and D18: does a whole comparison find every document it needs?

The architect's switch_model.py from the full design review of revision 6 (sha256 9808c1e2...),
ported with two additions: --expunge-held-only (DMS-24 item 2: the leader expunges only the
segments it held at the command, so a merge in flight there keeps its pre-command deletes), and
the comparison's computation at p stated and checked (DM-R42: the follower's live set at p is its
live set at a, less the documents written in (p, a], plus those live at p and dead by a).

A small exhaustive model of the two whole reinstalls that compare the leader's image with the
follower's own (D18, "A mode switch and D8's catch-up reinstall"), asking the question DM-R37's
rule rests on: for an honest leader and an honest follower, is every document the comparison needs
on the follower, in a segment it holds, its buffer, or its retained log? The comparison needs:
  - every dead ordinal of the leader's image at its position p (DM-R33: its records must equal the
    follower's copy of that document);
  - every document live at p and dead by the follower's applied position a >= p, since the follower
    has applied past p and the keyed sum of its live records at p needs their records too.
A document is its docid; its records are a function of its write, so "has it" means present.

Both replicas apply one log of Put(k) and Del(k) (a Put of a live key replaces it). Between
operations a replica may flush (a refresh: the buffer's live documents become a segment), start a
merge of any non-empty set of its unfrozen segments (one at a time; the output keeps the documents
live at the start), or end it (documents deleted meanwhile are carried as dead ordinals). Segments
are identified by their documents (docids are unique), so a rewrite with nothing dropped is the same
state. The follower's log base is its newest refresh at or below its checkpoint ceiling, the worst
case for it.

Scenarios:
  switch   document mode until the command after operation s. The follower merges none of the
           segments it holds at s (frozen), and its ceiling stays at s. The leader's first Image, at
           p >= s, holds no ordinal of a document deleted or replaced before the command (it
           expunged them). --inflight-open: the output of a merge in flight on the follower at s is
           not frozen (the literal reading of "the segments it then holds"); by default it is.
           --inclusive: the ceiling "at the command" admits a generation refreshed after the
           command, at its index (the follower then checkpoints what it flushed from its buffer);
           by default the ceiling is the follower's last refresh before the command.
  catchup  the follower installs the leader's image at c (a merge may be in flight on the leader),
           then applies the log itself, merging only its own later flushes, its ceiling at c; the
           leader's later Image at p > c is unconstrained (no expunge).
Mutants: --merge-held (nothing frozen), --no-expunge (switch: the leader's Image may hold
pre-command deletes), --expunge-held-only (switch: only the segments the leader held at the
command are expunged; the output of a merge in flight there may hold pre-command deletes),
--no-ceiling (the follower's base follows its newest refresh).

Usage: python3 -I switch_model.py --scenario switch --ops 5 --keys 2 [options]
Exit 0 when every needed document is present in every case, 1 with the first counterexample.
"""

from __future__ import annotations

import argparse
import itertools
import sys
import time
from typing import NamedTuple

Op = tuple[str, int]  # ("put" | "del", key)
Docs = frozenset[int]


class Seg(NamedTuple):
    docs: Docs
    frozen: bool
    straddle: bool = (
        False  # the output of a merge in flight at the command (the leader's tag)
    )


class R(NamedTuple):
    """One replica's state at an applied position (the position is kept outside)."""

    segs: frozenset[Seg]
    buffer: Docs
    merge: (
        tuple[frozenset[Docs], Docs, bool, bool] | None
    )  # (inputs, live at start, output frozen, output straddles the command)
    base: int  # the log base: the newest refresh at or below the ceiling


class Doc(NamedTuple):
    written: int
    dead: int | None  # the operation that deleted or replaced it


def replay(ops: tuple[Op, ...]) -> tuple[dict[int, Doc], list[list[int]]]:
    live: dict[int, int] = {}
    docs: dict[int, Doc] = {}
    wrote: list[list[int]] = [[]]
    nxt = 1
    for n, (kind, k) in enumerate(ops, 1):
        w: list[int] = []
        if k in live:
            old = live.pop(k)
            docs[old] = docs[old]._replace(dead=n)
        if kind == "put":
            docs[nxt] = Doc(n, None)
            live[k] = nxt
            w.append(nxt)
            nxt += 1
        wrote.append(w)
    return docs, wrote


def dead_by(d: Doc, n: int) -> bool:
    return d.dead is not None and d.dead <= n


def events(r: R, n: int, docs: dict[int, Doc], ceiling: int | None, maxsegs: int):
    if r.buffer:
        keep = frozenset(d for d in r.buffer if not dead_by(docs[d], n))
        segs = r.segs | ({Seg(keep, False)} if keep else set())
        if len(segs) <= maxsegs:
            base = n if (ceiling is None or n <= ceiling) else r.base
            yield r._replace(segs=segs, buffer=frozenset(), base=base)
    if r.merge is None:
        avail = sorted(
            (s for s in r.segs if not s.frozen), key=lambda s: sorted(s.docs)
        )
        for size in range(1, len(avail) + 1):
            for chosen in itertools.combinations(avail, size):
                ins = frozenset(s.docs for s in chosen)
                snap = frozenset(
                    d for s in chosen for d in s.docs if not dead_by(docs[d], n)
                )
                yield r._replace(merge=(ins, snap, False, False))
    else:
        ins, snap, out_frozen, straddle = r.merge
        rest = frozenset(s for s in r.segs if s.docs not in ins)
        out = {Seg(snap, out_frozen, straddle)} if snap else set()
        yield r._replace(segs=rest | out, merge=None)


def closure(layer: set[R], n: int, docs, ceiling, maxsegs) -> set[R]:
    seen = set(layer)
    todo = list(layer)
    while todo:
        r = todo.pop()
        for t in events(r, n, docs, ceiling, maxsegs):
            if t not in seen:
                seen.add(t)
                todo.append(t)
    return seen


def reach(
    docs,
    wrote,
    start: R,
    n0: int,
    upto: int,
    ceiling,
    maxsegs,
    freeze_at=None,
    inflight_open=False,
    inclusive=False,
    tag_only=False,
):
    """Every state reachable at each position n0..upto. The command sits after operation freeze_at:
    events at that position run before it, then the segments held (and, unless inflight_open, the
    output of a merge in flight) are frozen, then events run after it. A refresh after the command
    is at the command's own index: it moves the log base only when the ceiling is `inclusive`.
    tag_only (the leader): nothing is frozen; a merge in flight at the command is tagged."""
    out: dict[int, set[R]] = {}
    layer = {start}
    for n in range(n0, upto + 1):
        if n > n0:
            layer = {r._replace(buffer=r.buffer | frozenset(wrote[n])) for r in layer}
        layer = closure(layer, n, docs, ceiling, maxsegs)
        if tag_only and freeze_at is not None and n == freeze_at:
            layer = {
                r._replace(merge=(r.merge[0], r.merge[1], False, True))
                if r.merge
                else r
                for r in layer
            }
        elif freeze_at is not None and n == freeze_at:
            nl = set()
            for r in layer:
                segs = frozenset(Seg(x.docs, True) for x in r.segs)
                m = r.merge
                if m is not None and not inflight_open:
                    m = (m[0], m[1], True, m[3])
                nl.add(r._replace(segs=segs, merge=m))
            after = ceiling if inclusive or ceiling is None else n - 1
            layer = closure(nl, n, docs, after, maxsegs)
        out[n] = layer
    return out


def has(f: R, docs: dict[int, Doc]) -> Docs:
    held = {d for s in f.segs for d in s.docs} | set(f.buffer)
    logged = {d for d, doc in docs.items() if doc.written > f.base}
    return frozenset(held | logged)


def at_p(f: R, docs: dict[int, Doc], p: int, a: int) -> set[int]:
    """DM-R42's computation: the follower's live set at p from its state at a, as D18 states it
    (its live documents at a, less those written in (p, a], plus those live at p and dead by a,
    each found in a segment, its buffer, or its log). Returns what differs from the truth."""
    held = {d for s in f.segs for d in s.docs} | set(f.buffer)
    live_a = {d for d in held if not dead_by(docs[d], a)}
    written_after = {d for d, doc in docs.items() if p < doc.written <= a}
    back = {
        d
        for d, doc in docs.items()
        if doc.written <= p and not dead_by(doc, p) and dead_by(doc, a)
    } & has(f, docs)
    computed = (live_a - written_after) | back
    truth = {d for d, doc in docs.items() if doc.written <= p and not dead_by(doc, p)}
    return computed ^ truth


def show(segs) -> list[list[int]]:
    return sorted(sorted(s.docs) for s in segs)


def report(scn, ops, where, p, a, d, docs, need_dead, f, cases, start) -> int:
    why = (
        "a dead ordinal of the leader's image"
        if d in need_dead
        else "live at p, dead by a"
    )
    print(
        f"COUNTEREXAMPLE ({scn}): ops {list(ops)}, {where}, Image at p={p}, follower at a={a}"
    )
    print(
        f"  docid {d} ({why}; written by op {docs[d].written}, killed by op {docs[d].dead}) is not on the follower"
    )
    if d in need_dead:
        print(f"  the leader's image: segments {show(need_dead[d].segs)}")
    print(
        f"  the follower: segments {show(f.segs)}, buffer {sorted(f.buffer)}, log base {f.base}"
    )
    print(f"# {cases:,} cases, {time.time() - start:.1f} s")
    return 1


def needed(images, docs, p):
    need_dead: dict[int, R] = {}
    for r in images:
        for seg in r.segs:
            for d in seg.docs:
                if dead_by(docs[d], p):
                    need_dead.setdefault(d, r)
    return need_dead


def check(args) -> int:
    alphabet = [("put", k) for k in range(args.keys)] + [
        ("del", k) for k in range(args.keys)
    ]
    empty = R(frozenset(), frozenset(), None, 0)
    cases = 0
    start = time.time()
    for ops in itertools.product(alphabet, repeat=args.ops):
        if ops[0][0] == "del":
            continue  # a delete of nothing changes nothing
        docs, wrote = replay(ops)
        N = len(ops)
        lead = reach(docs, wrote, empty, 0, N, None, args.maxsegs)
        for s in range(N + 1):
            if args.scenario == "switch":
                ceiling = None if args.no_ceiling else s
                fz = None if args.merge_held else s
                foll = reach(
                    docs,
                    wrote,
                    empty,
                    0,
                    N,
                    ceiling,
                    args.maxsegs,
                    fz,
                    args.inflight_open,
                    args.inclusive,
                )
                pre = {d for d, doc in docs.items() if dead_by(doc, s)}
                lead_s = (
                    reach(
                        docs, wrote, empty, 0, N, None, args.maxsegs, s, tag_only=True
                    )
                    if args.expunge_held_only
                    else lead
                )
                for p in range(s, N + 1):
                    images = [
                        r for r in lead_s[p] if not r.buffer
                    ]  # the leader refreshes at p
                    if not args.no_expunge:
                        images = [
                            r
                            for r in images
                            if not any(
                                d in pre
                                for seg in r.segs
                                if not (args.expunge_held_only and seg.straddle)
                                for d in seg.docs
                            )
                        ]
                    need_dead = needed(images, docs, p)
                    for a in range(p, N + 1):
                        later = {
                            d
                            for d, doc in docs.items()
                            if doc.written <= p
                            and not dead_by(doc, p)
                            and dead_by(doc, a)
                        }
                        need = set(need_dead) | later
                        for f in foll[a]:
                            cases += 1
                            miss = (need - has(f, docs)) or at_p(f, docs, p, a)
                            if miss:
                                where = f"command after op {s}"
                                return report(
                                    "switch",
                                    ops,
                                    where,
                                    p,
                                    a,
                                    min(miss),
                                    docs,
                                    need_dead,
                                    f,
                                    cases,
                                    start,
                                )
            else:  # catchup: c plays s's part
                c = s
                bases = {
                    r for r in lead[c] if not r.buffer
                }  # the leader's image at c follows its refresh
                for b in bases:
                    # The installed segments are frozen from the install on (never merged by the
                    # follower), unless --merge-held.
                    installed = R(
                        frozenset(Seg(x.docs, not args.merge_held) for x in b.segs),
                        frozenset(),
                        None,
                        c,
                    )
                    ceiling = None if args.no_ceiling else c
                    foll = reach(docs, wrote, installed, c, N, ceiling, args.maxsegs)
                    lead_b = reach(docs, wrote, b, c, N, None, args.maxsegs)
                    for p in range(c + 1, N + 1):
                        images = [r for r in lead_b[p] if not r.buffer]
                        need_dead = needed(images, docs, p)
                        for a in range(p, N + 1):
                            later = {
                                d
                                for d, doc in docs.items()
                                if doc.written <= p
                                and not dead_by(doc, p)
                                and dead_by(doc, a)
                            }
                            need = set(need_dead) | later
                            for f in foll[a]:
                                cases += 1
                                miss = (need - has(f, docs)) or at_p(f, docs, p, a)
                                if miss:
                                    where = f"installed at c={c}"
                                    return report(
                                        "catchup",
                                        ops,
                                        where,
                                        p,
                                        a,
                                        min(miss),
                                        docs,
                                        need_dead,
                                        f,
                                        cases,
                                        start,
                                    )
    print(
        f"{args.scenario}: every needed document present; {cases:,} (log, command, p, a, follower state) cases, "
        f"{time.time() - start:.1f} s"
    )
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--scenario", choices=("switch", "catchup"), default="switch")
    ap.add_argument("--ops", type=int, default=5)
    ap.add_argument("--keys", type=int, default=2)
    ap.add_argument("--maxsegs", type=int, default=3)
    ap.add_argument("--merge-held", action="store_true")
    ap.add_argument("--no-expunge", action="store_true")
    ap.add_argument("--no-ceiling", action="store_true")
    ap.add_argument(
        "--expunge-held-only",
        action="store_true",
        help="the leader expunges only the segments it held at the command (DMS-24 item 2)",
    )
    ap.add_argument("--inflight-open", action="store_true")
    ap.add_argument(
        "--inclusive",
        action="store_true",
        help="the ceiling admits a refresh at the command's own index",
    )
    args = ap.parse_args()
    opts = [
        m
        for m in (
            "merge_held",
            "no_expunge",
            "no_ceiling",
            "expunge_held_only",
            "inflight_open",
            "inclusive",
        )
        if getattr(args, m)
    ]
    print(
        f"scenario {args.scenario}, {args.ops} operations over {args.keys} keys, at most {args.maxsegs} segments; "
        f"options: {opts or 'none'}"
    )
    return check(args)


if __name__ == "__main__":
    sys.exit(main())
