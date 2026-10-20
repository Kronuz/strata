#!/usr/bin/env python3
"""DMS-25 (security re-check of revision 8, C47): the once-only `grounded_by_switch` consumed by a
switch the leader times while no follower is current in segment mode. Ported from the security
reviewer's switch_consumed.py (the model path now this directory's), cleaned, otherwise the same
schedules.

Drives vouch_model_cycle.py (--cycle --born sample --up-rule first --yes-rule checked) along a fixed
prefix, then searches every state reachable from it (bounded) for a grounded segment-mode member.
Under --switch-ground eligible (revision 8's rule, the mutant) a group whose first `up` arrives while
both followers are in catch-up (D8's release leaves the same shape: a leader that proposes no `Image`
until the share releases every follower, or floods them past D9's bound) has no grounded member in
any reachable state. Under --switch-ground deferred (the design's rule) the first follower to
reinstall whole after the `up` is grounded, in one step. The controls: the followers current at the
`up` (grounded at it, both rules) and one follower out (it recovers by rule 4 under eligible, by
its own comparison under deferred).

Usage: python3 -I switch_consumed.py [--switch-ground eligible|deferred] [--maxe 4] [--depth 10]
"""

from __future__ import annotations

import argparse
import importlib.util
import sys
from collections import deque
from pathlib import Path
from types import ModuleType
from typing import Any

MODEL = Path(__file__).resolve().parent / "vouch_model_cycle.py"


def load_model() -> ModuleType:
    spec = importlib.util.spec_from_file_location("vouch_model_cycle", MODEL)
    assert spec is not None and spec.loader is not None
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def drive(mod: Any, cfg: Any, state: Any, maxe: int, prefix: list[str]) -> Any:
    """Follows each labeled step of the prefix from the model's own successors."""
    for want in prefix:
        nxt = None
        for step in mod.successors(state, cfg, maxe):
            if step[0] == want:
                nxt = step[1]
                break
        if nxt is None:
            labels = sorted(s[0] for s in mod.successors(state, cfg, maxe))
            raise SystemExit(f"no step {want!r}; available: {labels}")
        v = mod.violation(cfg, nxt, None)
        if v:
            raise SystemExit(f"prefix violates {v} at {want!r}")
        state = nxt
    return state


def search(
    mod: Any, cfg: Any, start: Any, maxe: int, depth: int, cap: int, goal: Any
) -> tuple[int, bool, list[str] | None]:
    """Bounded BFS: states seen, whether the bound cut it, and the shortest path to a goal state."""
    parent: dict[Any, Any] = {mod.key(start): None}
    frontier: deque[tuple[Any, int]] = deque([(start, 0)])
    cut = False
    while frontier:
        s, d = frontier.popleft()
        if d >= depth:
            cut = True
            continue
        for step in mod.successors(s, cfg, maxe):
            label, t = step[0], step[1]
            if mod.key(t) in parent:
                continue
            parent[mod.key(t)] = (s, label)
            if goal(t):
                trace = [label]
                p = parent[mod.key(s)]
                while p is not None:
                    s2, lab = p
                    trace.append(lab)
                    p = parent[mod.key(s2)]
                trace.reverse()
                return len(parent), cut, trace
            if len(parent) > cap:
                return len(parent), True, None
            frontier.append((t, d + 1))
    return len(parent), cut, None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--maxe", type=int, default=4)
    ap.add_argument("--depth", type=int, default=10)
    ap.add_argument("--cap", type=int, default=1_500_000)
    ap.add_argument("--switch-ground", default="eligible")
    args = ap.parse_args()
    mod = load_model()
    cfg = mod.Cfg(
        "base",
        "entry",
        "none",
        False,
        True,
        "first",
        False,
        "checked",
        "sample",
        args.switch_ground,
    )
    f0 = mod.M("seg", 0, False, 1, 0, 0, frozenset(), -1, -1, frozenset())
    init = mod.S((), (f0, f0), (mod.ABSENT, mod.ABSENT))
    deferred = args.switch_ground.startswith("deferred")

    print(f"model: {MODEL.name}, --switch-ground {args.switch_ground}")
    print(
        "rules: born sample, first `up` grounds per follower, `yes` grounds after checked installs"
    )

    def grounded_seg(s: Any) -> bool:
        return any(m.mode == "seg" and m.grounded for m in s.fs + s.ns)

    # The attack: both followers out of segment mode (catch-up) when the leader writes `up`; under
    # eligible each then reinstalls whole, still unvouched; under deferred the reinstall grounds.
    attack = [
        "leader appends entry 1 (honest)",
        "F1 enters catch-up",
        "F2 enters catch-up",
        "leader appends entry 2 (up)",
    ]
    if not deferred:
        attack += [
            "F1 reinstalls entry 2 whole: equal, still unvouched",
            "F2 reinstalls entry 2 whole: equal, still unvouched",
        ]
    s = drive(mod, cfg, init, args.maxe, attack)
    n, cut, trace = search(mod, cfg, s, args.maxe, args.depth, args.cap, grounded_seg)
    print(
        f"\nattack prefix ({len(attack)} steps), then {n:,} states to depth {args.depth}"
        + (" (cut)" if cut else " (every reachable state)")
    )
    if trace:
        print("  a grounded member is reachable:")
        for k, lab in enumerate(trace, 1):
            print(f"    {k:2}. {lab}")
    else:
        print(
            "  no reachable state has a grounded segment-mode member: the switch is spent"
        )

    # The control: the followers current in segment mode at the `up`.
    control = [
        "leader appends entry 1 (honest)",
        "F1 installs entry 1 (honest)",
        "F2 installs entry 1 (honest)",
        "leader appends entry 2 (up)",
        "F1 installs entry 2 (up) (grounded by the switch)",
    ]
    s = drive(mod, cfg, init, args.maxe, control)
    g = [
        mod.NAMES[i]
        for i, m in enumerate(s.fs + s.ns)
        if m.mode == "seg" and m.grounded
    ]
    print(f"\ncontrol prefix ({len(control)} steps): grounded members {g}")

    # One follower out: F2 in catch-up at the `up`, F1 grounded at it.
    one = [
        "leader appends entry 1 (honest)",
        "F1 installs entry 1 (honest)",
        "F2 enters catch-up",
        "leader appends entry 2 (up)",
        "F1 installs entry 2 (up) (grounded by the switch)",
    ]
    if not deferred:
        one.append("F2 reinstalls entry 2 whole: equal, still unvouched")
    s = drive(mod, cfg, init, args.maxe, one)
    print(
        f"\none follower out ({len(one)} steps): F1 grounded, F2 not; shortest path to F2 grounded:"
    )
    _, _, trace = search(
        mod,
        cfg,
        s,
        args.maxe,
        4,
        args.cap,
        lambda t: t.fs[1].mode == "seg" and t.fs[1].grounded,
    )
    for k, lab in enumerate(trace or [], 1):
        print(f"    {k:2}. {lab}")
    if not trace:
        print("    none within 4 steps")
    return 0


if __name__ == "__main__":
    sys.exit(main())
