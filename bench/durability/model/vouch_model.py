#!/usr/bin/env python3
"""durability-modes D17's `Vouch` and D18's whole images (revision 5; DM-R34, DMS-15, DMS-17).

An explicit-state model of one group's `Image` entries and the members that install them, to check
the answering rule: a member answers `yes` for an entry only if it checked that entry itself from a
grounded image (one it built, compared whole against its own, fetched from a grounded member that
is not the leader, or had vouched), and a member whose own image is not grounded answers `not yet`.

The leader is compromised: each entry it appends is `honest`, `plant` (its new segment carries
content the log never held), or `clean` (a later delete and merge remove planted content, as in the
security reviewer's vouch_position.py). The image at entry e is tainted when some entry at or below
e planted content that no later `clean` removed. Members:
  - F1, F2: segment-mode followers holding the honest image 0, grounded.
  - N1, N2: absent until they join with a whole image, from the leader (not grounded: D18's
    `install_unvouched`) or from a grounded member that is not the leader.
Actions: the leader appends; a segment-mode member installs the next entry (checks 1 to 5 refuse a
planted one, and the member applies the batches itself); a follower enters D8's catch-up (applying
the log itself) and later reinstalls the leader's latest image whole against its own (refused when
tainted); a member that is not grounded asks another member to vouch for its join entry or its
current entry, and a `yes` grounds it, a `no` sends it to apply the batches itself.

Properties: P1, no grounded segment-mode member holds a tainted image; P2, no member answers `yes`
for a tainted entry.

Rules (--rule):
  entry             revision 5: yes only for an entry checked itself since its image was grounded
                    (one interval per member, reset by a whole compare or a grounding); not grounded
                    answers `not yet`; below the interval `skipped`
  position          revision 4 as written: yes for an entry at or below the position through which
                    its install checks passed, whether or not its own image was vouched
  entry_ungrounded  DMS-15's fix alone: yes for an entry it checked itself, grounded or not (DM-R34's
                    control)
  position_grounded DM-R34's fix alone: grounded members answer by position (DMS-15's control)

Usage: python3 -I vouch_model.py --rule entry --maxe 4 --depth 16 [--cap 3000000]
"""

from __future__ import annotations

import argparse
import sys
import time
from collections import deque
from typing import NamedTuple

KINDS = ("honest", "plant", "clean")


class M(NamedTuple):
    mode: str  # absent, seg, catch, doc
    image: int  # the entry its installed image is at (-1 when absent)
    grounded: bool
    cfrom: int  # revision 5's interval of entries checked since grounding: [cfrom, cthrough]
    cthrough: int
    passed: int  # revision 4's position through which its install checks passed
    checked: frozenset[int]  # entries it checked itself, grounded or not (the controls)
    failed: int  # the entry its checks failed at, else -1
    join: int  # the entry of its whole install, else -1


class S(NamedTuple):
    log: tuple[str, ...]  # kinds of entries 1..n; entry 0 is the honest base image
    fs: tuple[M, ...]  # F1, F2
    ns: tuple[M, ...]  # N1, N2


def key(s: S) -> S:
    """F1 and F2 are symmetric, and N1 and N2: the visited set keys on sorted pairs."""
    return S(s.log, tuple(sorted(s.fs)), tuple(sorted(s.ns)))


def tainted(log: tuple[str, ...], e: int) -> bool:
    t = False
    for k in log[:e]:
        if k == "plant":
            t = True
        elif k == "clean":
            t = False
    return t


def answer(rule: str, m: M, e: int) -> str:
    if m.failed != -1 and m.failed <= e:
        return "no"
    if m.mode != "seg":
        return "not yet"
    if rule == "entry":
        if not m.grounded or e > m.cthrough:
            return "not yet"
        return "yes" if e >= m.cfrom else "skipped"
    if rule == "position":
        return "yes" if e <= m.passed else "not yet"
    if rule == "entry_ungrounded":
        if e in m.checked:
            return "yes"
        return "not yet" if e > m.passed else "skipped"
    if rule == "position_grounded":
        return "yes" if m.grounded and e <= m.passed else "not yet"
    raise ValueError(rule)


def install_next(log: tuple[str, ...], m: M) -> M:
    e = m.image + 1
    if log[e - 1] == "plant":  # checks 1 to 5 against the log refuse planted content
        return m._replace(mode="doc", failed=e)
    m = m._replace(image=e, passed=e, checked=m.checked | {e})
    if m.grounded:
        m = m._replace(cthrough=e)
    return m


def successors(s: S, rule: str, maxe: int):
    log = s.log
    last = len(log)
    members = list(s.fs) + list(s.ns)
    if last < maxe:
        for k in KINDS:
            yield f"leader appends entry {last + 1} ({k})", S(log + (k,), s.fs, s.ns)

    def rebuild(i: int, nm: M) -> S:
        ms = list(members)
        ms[i] = nm
        return S(log, tuple(ms[:2]), tuple(ms[2:]))

    for i, m in enumerate(members):
        name = ("F1", "F2", "N1", "N2")[i]
        if m.mode == "seg" and m.image < last:
            yield (
                f"{name} installs entry {m.image + 1}",
                rebuild(i, install_next(log, m)),
            )
        if m.mode == "seg" and i < 2:
            yield f"{name} enters catch-up", rebuild(i, m._replace(mode="catch"))
        if m.mode == "catch" and last > m.image:
            if tainted(log, last):
                yield (
                    f"{name} reinstalls entry {last} whole: refused",
                    rebuild(i, m._replace(mode="doc", failed=last)),
                )
            else:
                yield (
                    f"{name} reinstalls entry {last} whole: equal",
                    rebuild(
                        i,
                        m._replace(
                            mode="seg",
                            image=last,
                            grounded=True,
                            cfrom=last,
                            cthrough=last,
                            passed=last,
                            checked=m.checked | {last},
                        ),
                    ),
                )
        if m.mode == "absent":
            yield (
                f"{name} joins with the leader's image at {last}, unvouched",
                rebuild(
                    i,
                    M("seg", last, False, last + 1, last, last, frozenset(), -1, last),
                ),
            )
            for j, src in enumerate(members):
                if j != i and src.mode == "seg" and src.grounded:
                    y = src.image
                    yield (
                        f"{name} joins with {('F1', 'F2', 'N1', 'N2')[j]}'s image at {y}",
                        rebuild(i, M("seg", y, True, y + 1, y, y, frozenset(), -1, y)),
                    )
        if m.mode == "seg" and not m.grounded:
            for e in sorted({m.join, m.image}):
                for j, src in enumerate(members):
                    if j == i or src.mode == "absent":
                        continue
                    a = answer(rule, src, e)
                    label = f"{name} asks {('F1', 'F2', 'N1', 'N2')[j]} to vouch for entry {e}: {a}"
                    if a == "yes":
                        yield (
                            label,
                            rebuild(
                                i,
                                m._replace(
                                    grounded=True, cfrom=m.image + 1, cthrough=m.image
                                ),
                            ),
                            (j, e),
                        )
                    elif a == "no":
                        yield (
                            label,
                            rebuild(
                                i,
                                M("absent", -1, False, 0, -1, -1, frozenset(), -1, -1),
                            ),
                        )  # re-fetch: rejoin


def violation(s: S, step) -> str | None:
    for m in list(s.fs) + list(s.ns):
        if m.mode == "seg" and m.grounded and tainted(s.log, m.image):
            return f"P1: a grounded member holds the tainted image at {m.image}"
    if step is not None and len(step) == 3:
        _, _, (_, e) = step
        if tainted(s.log, e):
            return f"P2: `yes` for the tainted entry {e}"
    return None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "--rule",
        default="entry",
        choices=("entry", "position", "entry_ungrounded", "position_grounded"),
    )
    ap.add_argument("--maxe", type=int, default=4)
    ap.add_argument("--depth", type=int, default=16)
    ap.add_argument("--cap", type=int, default=3_000_000)
    args = ap.parse_args()
    start = time.time()
    f0 = M("seg", 0, True, 0, 0, 0, frozenset({0}), -1, -1)
    n0 = M("absent", -1, False, 0, -1, -1, frozenset(), -1, -1)
    init = S((), (f0, f0), (n0, n0))
    parent: dict[S, tuple[S, str] | None] = {
        key(init): None
    }  # key -> (the parent's actual state, label)
    frontier = deque([(init, 0)])
    found = None
    cut = False  # whether the depth bound left a state unexpanded
    while frontier:
        s, d = frontier.popleft()
        if d >= args.depth:
            cut = True
            continue
        for step in successors(s, args.rule, args.maxe):
            label, t = step[0], step[1]
            v = violation(t, step)
            if key(t) not in parent:
                parent[key(t)] = (s, label)
                if len(parent) > args.cap:
                    print(f"stopped at the state cap {args.cap}")
                    return 2
                frontier.append((t, d + 1))
            if v:
                found = (v, s, label)
                break
        if found:
            break
    reach = "every reachable state" if not cut and not found else f"depth {args.depth}"
    print(
        f"rule {args.rule}, entries up to {args.maxe}, {reach}: {len(parent):,} states, {time.time() - start:.1f} s"
    )
    if not found:
        print("P1 and P2 hold")
        return 0
    v, s, label = found
    trace = [label]
    while parent[key(s)] is not None:
        s, lab = parent[key(s)]
        trace.append(lab)
    trace.reverse()
    print(f"violated: {v}, in {len(trace)} steps")
    for k, lab in enumerate(trace, 1):
        print(f"  {k:2}. {lab}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
