"""Revision 6: the architect's wake model (.scratch/arch-r5/wake_model.py) with CT-R40's four rules.

State: the slot's state and owed flag, one request pinner, K chore callers (pin, lease, an
optional nested pin by the lease holder, release, unpin), the lease holder, the wake, and one
optional too_many_open failure of the wake. The sweep demotes from {0, open} as before.
Rules (each a switch, so every mutant is one switch off):
  wait   a chore pin that meets `waking` waits for the wake's end (rule 1)
  held   ... unless its thread holds the index's lease (`held_here()`), which enters (rule 1)
  bo     a repost goes through the host's timer, never straight back to the queue (rule 2)
  scope  the wake ends chore_scope before its final compare-exchange (rule 3)
  tmo    a wake's too_many_open takes the backoff: request pins fail at once until a tick (rule 4)
  owedw  mutant switch: a chore pin on `owed` waits (CT-R33 broken)
Reports per run: terminal deadlocks; stuck (a waiter, request or chore, that no path releases:
deadlock or livelock); starvable (a cycle in which the request waits while chores release the
lease); immediate repost (busy loop); resumption inside the wake's scope; a wake posted per pin
under overload (no tick since the failure); R28 (the request's pin only at {open, flag clear}).
"""
from collections import deque
from itertools import product
import sys

sys.setrecursionlimit(100000)
WAITS = ("cwait", "nwait")


def run(k, mode, req, wait, held, bo, scope, tmo, owedw=False, overload=True):
    # (st, flag, r, ch, lease, wake, fail); fail: 0 none, 1 failed and no tick since, 2 ticked
    start = ("owed", True, "idle" if req else "absent", ("idle",) * k, None, "none", 0)
    seen, q, edges = {start}, deque([start]), {}
    viol = {"R28": 0, "spin": False, "resume_in_scope": False, "wake_per_pin": False}

    def pins(r, ch):
        return (r in ("wait", "got")) + sum(c in ("pinned", "leased", "nested", "nwait", "cwait") for c in ch)

    while q:
        s = q.popleft()
        st, flag, r, ch, lease, wake, fail = s
        out = []

        def mk(st_=st, flag_=flag, r_=r, ch_=ch, lease_=lease, wake_=wake, fail_=fail):
            return (st_, flag_, r_, ch_, lease_, wake_, fail_)

        # request pin
        if r == "idle":
            if st == "owed":
                if tmo and fail == 1:
                    out.append(("rfast", s))  # fails at once, posts nothing
                else:
                    if fail == 1:
                        viol["wake_per_pin"] = True
                    out.append(("rpin", mk(st_="waking", r_="wait", wake_="queued")))
            elif st == "open":
                out.append(("rpin", mk(r_="got")))
            else:
                out.append(("rpin", mk(r_="wait")))
        if fail == 1:
            out.append(("tick", mk(fail_=2)))
        # the wake
        if wake == "queued":
            if lease is None:
                out.append(("wake", mk(lease_="wake", wake_="running")))
            elif bo:
                out.append(("repost", mk(wake_="delayed")))
            else:
                viol["spin"] = True
                out.append(("repost", s))
        if wake == "delayed":
            out.append(("tick", mk(wake_="queued")))
        if wake == "running" and scope:
            out.append(("scope_end", mk(lease_=None, wake_="ended")))
        if wake == "ended" or (wake == "running" and not scope):
            if lease == "wake":
                viol["resume_in_scope"] = True
            ch2 = tuple({"cwait": "pinned", "nwait": "nested"}.get(c, c) for c in ch)
            out.append(("wake_end", mk("open", False, "got" if r == "wait" else r, ch2,
                                       None if lease == "wake" else lease, "none")))
        if wake == "running" and overload and fail == 0:  # too_many_open: {p - w, owed}, waiters failed
            ch2 = tuple({"cwait": "pinned", "nwait": "nested"}.get(c, c) for c in ch)  # chore waiters get theirs at owed
            out.append(("wake_fail", mk("owed", True, "idle" if r == "wait" else r, ch2, None, "none", 1)))
        # chore callers
        for i, c in enumerate(ch):
            def put(v, lease_=lease):
                l = list(ch); l[i] = v
                return mk(ch_=tuple(l), lease_=lease_)
            if c == "idle":
                if st == "open" or (st == "owed" and not owedw) or (st == "waking" and not wait):
                    out.append(("cpin", put("pinned")))
                else:
                    out.append(("cpin", put("cwait")))
            elif c == "pinned" and lease is None:
                out.append(("clease", put("leased", i)))
            elif c == "leased":
                out.append(("crelease", put("idle" if mode == "stream" else "done", None)))
                nested_waits = (st == "waking" and wait and not held) or st == "demoting"
                out.append(("npin", put("nwait" if nested_waits else "nested")))
            elif c == "nested":
                out.append(("nunpin", put("leased")))
        # the sweep
        if st == "open" and pins(r, ch) == 0 and lease is None:
            out.append(("sweep", mk(st_="demoting", lease_="sweep")))
        if st == "demoting" and lease == "sweep":
            ch2 = tuple("pinned" if c == "cwait" else c for c in ch)
            out.append(("demote_end", mk("owed", True, r, ch2, None)))
        for lab, t in out:
            if t[2] == "got" and r != "got" and (t[0] != "open" or t[1]):
                viol["R28"] += 1
        edges[s] = out
        for _, t in out:
            if t not in seen:
                seen.add(t); q.append(t)

    final = lambda s: s[2] in ("got", "absent") and all(c == "done" for c in s[3])
    deadlocks = [s for s in seen if not edges[s] and not final(s)]
    # stuck: some waiter that no path releases
    rev = {s: [] for s in seen}
    for s, out in edges.items():
        for _, t in out:
            rev[t].append(s)

    def can_reach(good):
        ok = {s for s in seen if good(s)}
        dq = deque(ok)
        while dq:
            t = dq.popleft()
            for s in rev[t]:
                if s not in ok:
                    ok.add(s); dq.append(s)
        return ok
    stuck = 0
    parties = [(lambda s: s[2] == "wait", lambda s: s[2] != "wait")]
    for i in range(k):
        parties.append((lambda s, i=i: s[3][i] in WAITS, lambda s, i=i: s[3][i] not in WAITS))
    for waiting, freed in parties:
        ok = can_reach(freed)
        stuck += sum(1 for s in seen if waiting(s) and s not in ok)
    # starvation: an SCC of request-waiting states that contains a lease release
    nodes = {s for s in seen if s[2] == "wait"}
    index, low, onst, stack, sccs, n = {}, {}, set(), [], [], [0]

    def strong(v):
        index[v] = low[v] = n[0]; n[0] += 1
        stack.append(v); onst.add(v)
        for _, w in edges[v]:
            if w not in nodes:
                continue
            if w not in index:
                strong(w); low[v] = min(low[v], low[w])
            elif w in onst:
                low[v] = min(low[v], index[w])
        if low[v] == index[v]:
            comp = set()
            while True:
                w = stack.pop(); onst.discard(w); comp.add(w)
                if w == v:
                    break
            sccs.append(comp)
    for v in nodes:
        if v not in index:
            strong(v)
    starve = any(lab == "crelease" and t in comp for comp in sccs for s in comp for lab, t in edges[s])
    return len(seen), len(deadlocks), stuck, starve, viol


R6 = dict(wait=True, held=True, bo=True, scope=True, tmo=True)
VARIANTS = [
    ("R5 (revision 5)", dict(wait=False, held=True, bo=False, scope=False, tmo=False)),
    ("R6 (all four rules)", R6),
    ("mutant: chore pin enters waking", {**R6, "wait": False}),
    ("mutant: no held_here exception", {**R6, "held": False}),
    ("mutant: immediate repost", {**R6, "bo": False}),
    ("mutant: final CAS inside chore_scope", {**R6, "scope": False}),
    ("mutant: too_many_open without backoff", {**R6, "tmo": False}),
    ("mutant: chore pin waits on owed", {**R6, "owedw": True}),
]

if __name__ == "__main__":
    for name, v in VARIANTS:
        for k, mode, req in product((1, 2), ("finite", "stream"), (True, False)):
            n, dl, stuck, st, viol = run(k, mode, req, **v)
            print(f"{name:40} k={k} {mode:6} request={'yes' if req else 'no '}: {n:5} states, "
                  f"deadlocks {dl}, stuck {stuck}, starvable {st}, immediate repost {viol['spin']}, "
                  f"resume in scope {viol['resume_in_scope']}, wake per pin {viol['wake_per_pin']}, "
                  f"R28 {viol['R28']}")
