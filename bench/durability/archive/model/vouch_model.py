#!/usr/bin/env python3
"""durability-modes D17's `Vouch` and D18's whole images (revision 7; DM-R34, DM-R40, DM-R41, DMS-15).

An explicit-state model of one group's `Image` entries and the members that install them, to check
which images count as grounded and when a member answers `yes`. Revision 7 widens revision 5's model
as the architect's `vouch_model_r6.py` (full design review of revision 6) does: every member may enter
D8's catch-up, and each member's image carries the planted entries it holds, so a whole comparison
compares content instead of asking whether the leader's image is tainted.

The leader is compromised: each entry it appends is `honest`, `plant` (its new segment carries
content the log never held), or `clean` (a later delete and merge remove every earlier plant, as in
the security reviewer's vouch_position.py). Members:
  - F1, F2: segment-mode followers holding image 0, grounded (or, with --switch, as a switch to
    `verify: install` leaves them).
  - N1, N2: absent until they join with a whole image, from the leader (unvouched: D18's
    `install_unvouched`) or from a grounded member that is not the leader.
Actions: the leader appends; a segment-mode member installs the next entry (checks 1 to 5 refuse a
planted one, and the member applies the batches itself); a member enters D8's catch-up (applying the
log itself on top of its installed image) and later reinstalls the leader's latest image whole,
comparing it with its own (its plants, less those a `clean` in the caught-up range removed); a member
that is not grounded asks another to vouch for its join entry or its current entry, and a `yes`
grounds it, a `no` sends it back to a whole fetch.

Properties: P1, no grounded segment-mode member holds a planted entry; P2, no member answers `yes`
for an entry whose image at the leader holds a planted entry. Under --switch grounded, entry 0's
plant (--plant0, one a sampled rebuild missed before the switch) is the stated residual and is
excluded from both, and P3 holds instead: no `yes` for an entry at or before the switch.

--catchup (how a passing catch-up reinstall grounds):
  base   revision 7 (DM-R40): only a member whose installed image was grounded when its catch-up
         began; any other installs the leader's image and stays unvouched
  any    revision 6 as written: every passing reinstall grounds (the mutant)
--answer (when a member answers `yes`):
  entry             revision 5 and 7: only for an entry checked itself since its image was grounded
                    (one interval per member); not grounded answers `not yet`; below it `skipped`
  position          revision 4: for an entry at or below the position its install checks passed
  entry_ungrounded  DMS-15's fix alone: for an entry it checked itself, grounded or not
  position_grounded DM-R34's fix alone: grounded members answer by position
--switch (DM-R41: F1 and F2's images were installed under `verify: sample`, then the group
switched to `install` at entry 0):
  none        F1 and F2 grounded by construction (document mode, or install checks throughout)
  grounded    option (a), revision 7: each follower's image counts as grounded at the switch
              (`grounded_by_switch`), its interval of checked entries starting after it
  unvouched   option (b): they stay unvouched until a `yes`

Usage: python3 -I vouch_model.py [--catchup base] [--answer entry] [--switch none] [--plant0]
                                 [--maxe 3] [--depth 13] [--cap 1500000]
"""

from __future__ import annotations

import argparse
import sys
import time
from collections import deque
from typing import NamedTuple

KINDS = ("honest", "plant", "clean")
NAMES = ("F1", "F2", "N1", "N2")


class M(NamedTuple):
    mode: str  # absent, seg, catch, doc
    image: int  # the entry its installed image is at (-1 when absent)
    grounded: bool
    cfrom: int  # the interval of entries checked since grounding: [cfrom, cthrough]
    cthrough: int
    passed: int  # revision 4's position through which its install checks passed
    checked: frozenset[int]  # entries it checked itself, grounded or not (the controls)
    failed: int  # the entry its checks failed at, else -1
    join: int  # the entry of its whole install, else -1
    plants: frozenset[int]  # planted entries its own image holds


class S(NamedTuple):
    log: tuple[str, ...]  # kinds of entries 1..n; entry 0 is the base image
    fs: tuple[M, ...]  # F1, F2
    ns: tuple[M, ...]  # N1, N2


ABSENT = M("absent", -1, False, 0, -1, -1, frozenset(), -1, -1, frozenset())


class Cfg(NamedTuple):
    catchup: str
    answer: str
    switch: str
    plant0: bool


def key(s: S) -> S:
    """F1 and F2 are symmetric, and N1 and N2: the visited set keys on sorted pairs."""
    return S(s.log, tuple(sorted(s.fs)), tuple(sorted(s.ns)))


def leader_plants(cfg: Cfg, log: tuple[str, ...], e: int) -> frozenset[int]:
    """Planted entries the leader's image at entry e holds (a `clean` removes every earlier one)."""
    live: set[int] = {0} if cfg.plant0 else set()
    for k, kind in enumerate(log[:e], 1):
        if kind == "plant":
            live.add(k)
        elif kind == "clean":
            live.clear()
    return frozenset(live)


def counted(cfg: Cfg, plants: frozenset[int]) -> frozenset[int]:
    """The plants a property counts: under option (a), entry 0's is the stated residual."""
    return plants - {0} if cfg.switch == "grounded" else plants


def answer(cfg: Cfg, m: M, e: int) -> str:
    if m.failed != -1 and m.failed <= e:
        return "no"
    if m.mode != "seg":
        return "not yet"
    if cfg.answer == "entry":
        if not m.grounded or e > m.cthrough:
            return "not yet"
        return "yes" if e >= m.cfrom else "skipped"
    if cfg.answer == "position":
        return "yes" if e <= m.passed else "not yet"
    if cfg.answer == "entry_ungrounded":
        if e in m.checked:
            return "yes"
        return "not yet" if e > m.passed else "skipped"
    if cfg.answer == "position_grounded":
        return "yes" if m.grounded and e <= m.passed else "not yet"
    raise ValueError(cfg.answer)


def install_next(log: tuple[str, ...], m: M) -> M:
    e = m.image + 1
    kind = log[e - 1]
    if kind == "plant":  # checks 1 to 5 against the log refuse planted content
        return m._replace(mode="doc", failed=e)
    m = m._replace(
        image=e,
        passed=e,
        checked=m.checked | {e},
        plants=frozenset() if kind == "clean" else m.plants,
    )
    if m.grounded:
        m = m._replace(cthrough=e)
    return m


def successors(s: S, cfg: Cfg, maxe: int):
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
        name = NAMES[i]
        if m.mode == "seg" and m.image < last:
            yield (
                f"{name} installs entry {m.image + 1}",
                rebuild(i, install_next(log, m)),
            )
        if m.mode == "seg":
            yield f"{name} enters catch-up", rebuild(i, m._replace(mode="catch"))
        if m.mode == "catch" and last > m.image:
            own = frozenset() if "clean" in log[m.image : last] else m.plants
            if own != leader_plants(cfg, log, last):
                yield (
                    f"{name} reinstalls entry {last} whole: refused",
                    rebuild(i, m._replace(mode="doc", failed=last)),
                )
            else:
                grounds = cfg.catchup == "any" or m.grounded
                nm = m._replace(
                    mode="seg",
                    image=last,
                    plants=own,
                    passed=last,
                    checked=m.checked | {last},
                )
                if grounds:
                    nm = nm._replace(grounded=True, cfrom=last, cthrough=last)
                verdict = "equal, grounded" if grounds else "equal, still unvouched"
                yield f"{name} reinstalls entry {last} whole: {verdict}", rebuild(i, nm)
        if m.mode == "absent":
            yield (
                f"{name} joins with the leader's image at {last}, unvouched",
                rebuild(
                    i,
                    M(
                        "seg",
                        last,
                        False,
                        last + 1,
                        last,
                        last,
                        frozenset(),
                        -1,
                        last,
                        leader_plants(cfg, log, last),
                    ),
                ),
            )
            for j, src in enumerate(members):
                if j != i and src.mode == "seg" and src.grounded:
                    y = src.image
                    yield (
                        f"{name} joins with {NAMES[j]}'s image at {y}",
                        rebuild(
                            i,
                            M(
                                "seg",
                                y,
                                True,
                                y + 1,
                                y,
                                y,
                                frozenset(),
                                -1,
                                y,
                                src.plants,
                            ),
                        ),
                    )
        if m.mode == "seg" and not m.grounded:
            for e in sorted({m.join, m.image} - {-1}):
                for j, src in enumerate(members):
                    if j == i or src.mode == "absent":
                        continue
                    a = answer(cfg, src, e)
                    label = f"{name} asks {NAMES[j]} to vouch for entry {e}: {a}"
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
                        yield label, rebuild(i, ABSENT)  # re-fetch: rejoin


def violation(cfg: Cfg, s: S, step) -> str | None:
    for m in list(s.fs) + list(s.ns):
        if m.mode == "seg" and m.grounded and counted(cfg, m.plants):
            return f"P1: a grounded member holds planted entries {sorted(m.plants)} at {m.image}"
    if step is not None and len(step) == 3:
        _, _, (_, e) = step
        if counted(cfg, leader_plants(cfg, s.log, e)):
            return f"P2: `yes` for entry {e}, whose image holds a plant"
        if cfg.switch == "grounded" and e <= 0:
            return f"P3: `yes` for entry {e}, at or before the switch"
    return None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--catchup", default="base", choices=("base", "any"))
    ap.add_argument(
        "--answer",
        default="entry",
        choices=("entry", "position", "entry_ungrounded", "position_grounded"),
    )
    ap.add_argument(
        "--switch", default="none", choices=("none", "grounded", "unvouched")
    )
    ap.add_argument(
        "--plant0", action="store_true", help="image 0 holds a plant sampling missed"
    )
    ap.add_argument("--maxe", type=int, default=3)
    ap.add_argument("--depth", type=int, default=13)
    ap.add_argument("--cap", type=int, default=1_500_000)
    args = ap.parse_args()
    cfg = Cfg(args.catchup, args.answer, args.switch, args.plant0)
    start = time.time()
    base = frozenset({0}) if args.plant0 else frozenset()
    if args.switch == "none":
        f0 = M("seg", 0, True, 0, 0, 0, frozenset({0}), -1, -1, base)
    elif args.switch == "grounded":
        f0 = M("seg", 0, True, 1, 0, 0, frozenset(), -1, -1, base)
    else:
        f0 = M("seg", 0, False, 1, 0, 0, frozenset(), -1, 0, base)
    init = S((), (f0, f0), (ABSENT, ABSENT))
    parent: dict[S, tuple[S, str] | None] = {key(init): None}
    frontier = deque([(init, 0)])
    found = None
    cut = False  # whether the depth bound left a state unexpanded
    grounded_seen = 0  # states with a grounded segment-mode member besides the start
    while frontier:
        s, d = frontier.popleft()
        if d >= args.depth:
            cut = True
            continue
        for step in successors(s, cfg, args.maxe):
            label, t = step[0], step[1]
            v = violation(cfg, t, step)
            if key(t) not in parent:
                parent[key(t)] = (s, label)
                if any(m.mode == "seg" and m.grounded for m in t.fs + t.ns):
                    grounded_seen += 1
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
        f"catch-up grounds {cfg.catchup}, answers {cfg.answer}, switch {cfg.switch}, plant0 "
        f"{'on' if cfg.plant0 else 'off'}, entries up to {args.maxe}, {reach}: {len(parent):,} "
        f"states, {time.time() - start:.1f} s"
    )
    print(f"states with a grounded segment-mode member: {grounded_seen:,}")
    if not found:
        print("P1, P2" + (", P3" if cfg.switch == "grounded" else "") + " hold")
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
