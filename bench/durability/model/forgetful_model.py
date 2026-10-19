#!/usr/bin/env python3
"""Explicit-state model of durability-modes D2, D5, D6 and D7 (revision 1).

Extends the architect's DM-R1 probe (strata-wt/review-durability/.scratch/review-durability/
forgetful_model.py) and keeps its semantics: atomic steps; partitions and process crashes
unbounded (any node may take no part in any step); at most FMAX voters forgetful at once, at
most MAXOS OS crashes, MAXP client writes, terms up to MAXT. With --compat it reproduces that
probe's state space exactly (its modes none, heard and timeout).

Revision 1 adds, unless --compat:
  - failed elections: a candidate's requests raise the terms they reach without a win, as in
    Raft (the probe emitted won elections only, so a term rose only by a win);
  - each voter's commit_durable, learned from appends and clamped to the verified prefix,
    persisted at its barrier and restored after an OS crash; the checks that no follower
    replaces an entry at or below it ("floor"), nor one at or below commit_memory unless D6
    allows it ("fence": above commit_durable and older than the leader's newest Recovery);
  - a leader is not forgetful (it is its term's authority);
  - the modes below.

Modes for a forgetful voter:
  none         grants nothing and never campaigns until it re-learns (D5 alone)
  heard        revision 0's D6 balanced, its every-voter-heard condition only
  timeout      revision 0's D6 balanced as written: every voter heard, or recovery_timeout
  fplus        DM-R1's fix as the owner's Q3 rules it: pre-vote probes carry `forgetful`, and
               D6's conditions apply once f + 1 distinct forgetful voters were heard since the
               restart, itself included (every voter heard, or the timeout)
  fplus_heard  the same, every voter heard, no timeout
  round        revision 1's D6: fplus's count only starts a recovery election, and any voter
               that heard f + 1 forgetful voters may start one (itself counted when forgetful),
               so a voter that is not forgetful but holds the longest log can lead; the election
               asks each voter at its new term (one step each, so others interleave) and leads
               only when every answer at that term is a grant (a voter grants only a log at least
               as up to date as its own), f + 1 of them flagged forgetful, from every voter or,
               once its timeout passed, a majority; a forgetful voter grants nothing else
  round_heard  the same, every voter's answer required
Violations: "memory" (a new leader lacks a write acknowledged at memory: inside the model
only), "quorum" (one acknowledged at quorum), "floor", and "fence" (inside the model only).
A counterexample is real; a pass is evidence bounded by the depth and the scope.
"""
import argparse
import sys
import time
from collections import deque

FOLLOWER, CANDIDATE, LEADER, RCAND = 0, 1, 2, 3
NOOP, REC = 0, 1  # entry values; a client write k is 2 + k
(TERM, VOTED, LOG, SYNCED, FORG, ROLE, COMMIT, SEEN, SMAX, TFIRED, MATCH, SMATCH,
 SEENF, CDUR, PCDUR, RND) = range(16)
FIELDS = {name: i for i, name in enumerate(
    "TERM VOTED LOG SYNCED FORG ROLE COMMIT SEEN SMAX TFIRED MATCH SMATCH SEENF CDUR PCDUR RND".split())}
MODES = ("none", "heard", "timeout", "fplus", "fplus_heard", "round", "round_heard")


class Config:
    def __init__(self, mode, n, fmax, maxos, maxp, maxt, compat=False):
        assert mode in MODES and n % 2 == 1
        assert not compat or mode in ("none", "heard", "timeout")
        self.mode, self.N, self.FMAX, self.MAXOS, self.MAXP, self.MAXT = mode, n, fmax, maxos, maxp, maxt
        self.F = (n - 1) // 2
        self.MAJ = self.F + 1
        self.ALL = (1 << n) - 1
        self.compat = compat
        self.flags = mode in ("fplus", "fplus_heard", "round", "round_heard")
        self.timeout = mode in ("timeout", "fplus", "round")
        self.rounds = mode in ("round", "round_heard")

    def __str__(self):
        return (f"mode={self.mode} N={self.N} FMAX={self.FMAX} MAXOS={self.MAXOS} MAXP={self.MAXP} "
                f"MAXT={self.MAXT}{' compat' if self.compat else ''}")


def lastpos(log):
    return (log[-1][0], len(log)) if log else (0, 0)


def setf(node, **kw):
    n = list(node)
    for k, v in kw.items():
        n[FIELDS[k]] = v
    return tuple(n)


def fresh():
    return (0, -1, (), 0, False, FOLLOWER, 0, 0, (0, 0), False, None, None, 0, 0, 0, None)


def bits(mask):
    return [i for i in range(mask.bit_length()) if mask >> i & 1]


def grant_ok(cfg, r, cand_pos):
    """Whether voter r's state lets it grant a normal (pre-)vote to a log at cand_pos."""
    if not r[FORG]:
        return True
    mode = cfg.mode
    if mode in ("none", "round", "round_heard"):
        return False
    heard = r[SEEN] == cfg.ALL
    if mode == "heard":
        allowed = heard
    elif mode == "timeout":
        allowed = heard or r[TFIRED]
    else:
        count = bin(r[SEENF]).count("1") >= cfg.F + 1
        allowed = count and (heard or (mode == "fplus" and r[TFIRED]))
    return allowed and cand_pos >= r[SMAX]


def campaign_ok(cfg, n):
    return not n[FORG] or grant_ok(cfg, n, lastpos(n[LOG]))


def writes(log, upto):
    return {(i + 1, log[i][0], log[i][1]) for i in range(upto) if log[i][1] >= 2}


def holds(log, acked):
    return all(i <= len(log) and log[i - 1] == (t, v) for (i, t, v) in acked)


def show(v):
    return "noop" if v == NOOP else "rec" if v == REC else f"v{v - 2}"


def successors(cfg, state):
    nodes, amem, adur, nprop, nos = state
    N, MAJ, F = cfg.N, cfg.MAJ, cfg.F
    out = []

    def emit(label, ns, am=amem, ad=adur, np=nprop, no=nos, check=None):
        out.append((label, (tuple(ns), am, ad, np, no), check))

    for c in range(N):
        n = nodes[c]
        # A normal election: c campaigns at term + 1, reaching the voters in mask.
        if n[ROLE] != LEADER and n[TERM] < cfg.MAXT and campaign_ok(cfg, n) and not (cfg.rounds and n[FORG]):
            T = n[TERM] + 1
            others = [x for x in range(N) if x != c]
            for mask in range(1, 1 << len(others)):
                ns = list(nodes)
                cand = setf(n, TERM=T, VOTED=c, ROLE=CANDIDATE, MATCH=None, SMATCH=None, RND=None)
                cpos = lastpos(cand[LOG])
                votes = 1
                reached = [r for b, r in enumerate(others) if mask >> b & 1]
                for r in reached:
                    rn = ns[r]
                    if T > rn[TERM]:
                        rn = setf(rn, TERM=T, VOTED=-1, ROLE=FOLLOWER, MATCH=None, SMATCH=None, RND=None)
                    if rn[FORG]:
                        rn = setf(rn, SEEN=rn[SEEN] | (1 << c), SMAX=max(rn[SMAX], cpos))
                    if rn[TERM] == T and rn[VOTED] in (-1, c) and cpos >= lastpos(rn[LOG]) and grant_ok(cfg, rn, cpos):
                        rn = setf(rn, VOTED=c)
                        votes += 1
                    ns[r] = rn
                if votes >= MAJ:
                    ok_mem, ok_dur = holds(cand[LOG], amem), holds(cand[LOG], adur)
                    log = cand[LOG] + ((T, NOOP),)
                    m = [0] * N
                    m[c] = len(log)
                    cand = setf(cand, ROLE=LEADER, LOG=log, MATCH=tuple(m), SMATCH=tuple([0] * N))
                    if not cfg.compat:
                        cand = setf(cand, FORG=False, SEEN=0, SMAX=(0, 0), TFIRED=False, SEENF=0)
                    ns[c] = cand
                    emit(f"elect {c} term {T} votes from {reached}", ns, check=("elect", ok_mem, ok_dur))
                elif not cfg.compat:
                    ns[c] = cand
                    emit(f"fail-elect {c} term {T} reaching {reached}", ns)
        # Pre-vote probes: any non-leader's requests reach forgetful voters (D6's "seen"), and
        # under the flag modes say whether their sender is forgetful (kind 6).
        if cfg.mode != "none" and n[ROLE] != LEADER:
            pos = lastpos(n[LOG])
            for r in range(N):
                rn = nodes[r]
                if r == c or not (rn[FORG] or (cfg.rounds and n[FORG] and rn[ROLE] != LEADER)):
                    continue
                seen, smax = (rn[SEEN] | (1 << c), max(rn[SMAX], pos)) if rn[FORG] else (rn[SEEN], rn[SMAX])
                seenf = rn[SEENF] | ((1 << c) if cfg.flags and n[FORG] else 0)
                if (seen, smax, seenf) != (rn[SEEN], rn[SMAX], rn[SEENF]):
                    ns = list(nodes)
                    ns[r] = setf(rn, SEEN=seen, SMAX=smax, SEENF=seenf)
                    emit(f"probe {c}->{r}", ns)
        # recovery_timeout passes with no leader.
        if cfg.timeout and (n[FORG] or (cfg.rounds and bin(n[SEENF]).count("1") >= F + 1)) and not n[TFIRED]:
            ns = list(nodes)
            ns[c] = setf(n, TFIRED=True)
            emit(f"recovery_timeout {c}", ns)
        # A client write at the leader: written, not synced.
        if n[ROLE] == LEADER and nprop < cfg.MAXP:
            log = n[LOG] + ((n[TERM], 2 + nprop),)
            m = list(n[MATCH])
            m[c] = len(log)
            ns = list(nodes)
            ns[c] = setf(n, LOG=log, MATCH=tuple(m))
            emit(f"write v{nprop} at {c} term {n[TERM]}", ns, np=nprop + 1)
        # Replication: the follower takes the leader's log by Raft's conflict rule, acknowledges
        # the written and the synced index (D7), learns both cursors clamped to what it
        # verified, and re-learns if forgetful (D5).
        if n[ROLE] == LEADER:
            L = n[LOG]
            rterm = max((t for (t, v) in L if v == REC), default=0)
            for f in range(N):
                if f == c or nodes[f][TERM] > n[TERM]:
                    continue
                fn = nodes[f]
                i = 0
                while i < len(L) and i < len(fn[LOG]) and fn[LOG][i] == L[i]:
                    i += 1
                if i == len(L):
                    newlog, replaced_from = fn[LOG], None
                else:
                    newlog = L
                    replaced_from = i + 1 if i < len(fn[LOG]) else None
                check = None
                base = fn[COMMIT]
                if cfg.compat:
                    if replaced_from is not None and replaced_from <= fn[COMMIT]:
                        check = ("fence",)
                elif replaced_from is not None:
                    if replaced_from <= fn[CDUR]:
                        check = ("floor",)
                    elif replaced_from <= fn[COMMIT]:
                        top = min(fn[COMMIT], len(fn[LOG]))
                        if not all(fn[LOG][k][0] < rterm for k in range(replaced_from - 1, top)):
                            check = ("fence",)
                        base = replaced_from - 1  # D6: commit_memory falls to the divergence point
                synced = min(fn[SYNCED], i)
                commit = max(base, min(n[COMMIT], len(L)))
                cdur = fn[CDUR] if cfg.compat else max(fn[CDUR], min(n[CDUR], len(L)))
                fn2 = setf(fn, TERM=n[TERM], VOTED=fn[VOTED] if fn[TERM] == n[TERM] else -1, ROLE=FOLLOWER,
                           LOG=newlog, SYNCED=synced, COMMIT=commit, FORG=False, SEEN=0, SMAX=(0, 0),
                           TFIRED=False, MATCH=None, SMATCH=None, SEENF=0, CDUR=cdur, RND=None)
                m = list(n[MATCH])
                sm = list(n[SMATCH])
                m[f] = len(L)
                sm[f] = min(synced, len(L))
                ns = list(nodes)
                ns[f] = fn2
                ns[c] = setf(n, MATCH=tuple(m), SMATCH=tuple(sm))
                if ns[f] == nodes[f] and ns[c] == nodes[c]:
                    continue
                emit(f"replicate {c}->{f}" + (f" {check[0].upper()}" if check else ""), ns, check=check)
        # Commit: commit_memory over written matches, commit_durable over synced ones (D7).
        if n[ROLE] == LEADER:
            L = n[LOG]
            m = list(n[MATCH])
            m[c] = len(L)
            sm = list(n[SMATCH])
            sm[c] = n[SYNCED]
            newc = n[COMMIT]
            for k in range(len(L), n[COMMIT], -1):
                if L[k - 1][0] == n[TERM] and sum(1 for x in m if x >= k) >= MAJ:
                    newc = k
                    break
            dur = 0
            for k in range(len(L), 0, -1):
                if L[k - 1][0] == n[TERM] and sum(1 for x in sm if x >= k) >= MAJ:
                    dur = k
                    break
            am = amem | writes(L, newc)
            ad = adur | writes(L, dur)
            cdur = n[CDUR] if cfg.compat else max(n[CDUR], dur)
            if newc != n[COMMIT] or am != amem or ad != adur or cdur != n[CDUR]:
                ns = list(nodes)
                ns[c] = setf(n, COMMIT=newc, CDUR=cdur)
                emit(f"commit at {c}: memory {newc}, durable {dur}", ns, am=frozenset(am), ad=frozenset(ad))
        # A barrier (D3): the log, and the hard state's commit index (commit_durable, D7).
        pcdur = n[PCDUR] if cfg.compat else n[CDUR]
        if n[SYNCED] < len(n[LOG]) or pcdur != n[PCDUR]:
            ns = list(nodes)
            ns[c] = setf(n, SYNCED=len(n[LOG]), PCDUR=pcdur)
            emit(f"sync {c}", ns)
        # An OS crash or power loss: the unsynced tail goes; term, vote, and the synced hard
        # state's commit_durable stay (D4); commit_memory restarts from it.
        if nos < cfg.MAXOS:
            forgetful_after = sum(1 for k in range(N) if k != c and nodes[k][FORG]) + 1
            if forgetful_after <= cfg.FMAX:
                log = n[LOG][:n[SYNCED]]
                cdur = 0 if cfg.compat else min(n[PCDUR], len(log))
                ns = list(nodes)
                ns[c] = setf(n, LOG=log, FORG=True, ROLE=FOLLOWER, COMMIT=cdur, SEEN=1 << c, SMAX=lastpos(log),
                             TFIRED=False, MATCH=None, SMATCH=None, SEENF=(1 << c) if cfg.flags else 0,
                             CDUR=cdur, RND=None)
                emit(f"os-crash {c} (keeps {len(log)} synced entries)", ns, no=nos + 1)
        # Revision 1's recovery election (round modes).
        if cfg.rounds:
            if n[ROLE] in (FOLLOWER, CANDIDATE) and n[TERM] < cfg.MAXT and bin(n[SEENF]).count("1") >= F + 1:
                rnd = [None] * N
                rnd[c] = (n[FORG], True)
                ns = list(nodes)
                ns[c] = setf(n, TERM=n[TERM] + 1, VOTED=c, ROLE=RCAND, MATCH=None, SMATCH=None, RND=tuple(rnd))
                emit(f"round {c} term {n[TERM] + 1}", ns)
            if n[ROLE] == RCAND:
                U, cpos = n[TERM], lastpos(n[LOG])
                for r in range(N):
                    if r == c or n[RND][r] is not None:
                        continue
                    rn = nodes[r]
                    ns = list(nodes)
                    if rn[TERM] > U:
                        ns[c] = setf(n, TERM=rn[TERM], VOTED=-1, ROLE=FOLLOWER, RND=None)
                        emit(f"ask {c}->{r} refused at term {rn[TERM]}", ns)
                        continue
                    if U > rn[TERM]:
                        rn = setf(rn, TERM=U, VOTED=-1, ROLE=FOLLOWER, MATCH=None, SMATCH=None, RND=None)
                    if rn[FORG]:
                        rn = setf(rn, SEEN=rn[SEEN] | (1 << c), SMAX=max(rn[SMAX], cpos))
                    granted = rn[VOTED] in (-1, c) and cpos >= lastpos(rn[LOG])
                    if granted:
                        rn = setf(rn, VOTED=c)
                    rnd = list(n[RND])
                    rnd[r] = (rn[FORG], granted)
                    ns[r] = rn
                    ns[c] = setf(n, RND=tuple(rnd))
                    emit(f"ask {c}->{r}" + (" granted" if granted else "") + (" forgetful" if rn[FORG] else ""), ns)
                answers = [x for x in n[RND] if x is not None]
                grants = sum(1 for x in answers if x[1])
                forgetful = sum(1 for x in answers if x[0])
                heard = len(answers) == N
                allowed = heard if cfg.mode == "round_heard" else heard or n[TFIRED]
                # Every answer at its term a grant: a refusal (another vote at U, or a log ahead of
                # its own) ends the round.
                if grants == len(answers) and grants >= MAJ and forgetful >= F + 1 and allowed:
                    ok_mem, ok_dur = holds(n[LOG], amem), holds(n[LOG], adur)
                    log = n[LOG] + ((U, REC), (U, NOOP))
                    m = [0] * N
                    m[c] = len(log)
                    ns = list(nodes)
                    ns[c] = setf(n, ROLE=LEADER, LOG=log, MATCH=tuple(m), SMATCH=tuple([0] * N), FORG=False,
                                 SEEN=0, SMAX=(0, 0), TFIRED=False, SEENF=0, RND=None)
                    emit(f"decide {c} term {U} ({grants} grants, {forgetful} forgetful)", ns,
                         check=("elect", ok_mem, ok_dur))
    return out


def violations(check):
    if check is None:
        return []
    if check[0] == "elect":
        return [k for k, ok in (("memory", check[1]), ("quorum", check[2])) if not ok]
    return [check[0]]


def initial(cfg):
    return (tuple(fresh() for _ in range(cfg.N)), frozenset(), frozenset(), 0, 0)


def run(cfg, depth, limit, out=sys.stdout):
    start = time.time()
    init = initial(cfg)
    parent = {init: None}
    level = [init]
    found = {}
    per_depth = []
    d = 0
    while level and d < depth and len(parent) < limit:
        nxt = []
        for s in level:
            for label, t, check in successors(cfg, s):
                for k in violations(check):
                    if k not in found:
                        found[k] = (s, label, d + 1)
                if t not in parent:
                    parent[t] = (s, label)
                    nxt.append(t)
        d += 1
        per_depth.append(len(parent))
        level = nxt
    complete = not level
    print(f"{cfg}: {len(parent):,} states, {'all reached' if complete else f'bounded at depth {d}'}"
          f" ({time.time() - start:.0f} s); states by depth {per_depth}", file=out)
    beyond = cfg.FMAX > cfg.F
    allowed = {"memory", "fence"} if beyond else set()
    bad = [k for k in found if k not in allowed]
    if not bad:
        note = "; beyond the model: " + (", ".join(f"{k} found" for k in sorted(found)) if found else "nothing lost")
        print("  no violation of the target" + (note if beyond else ": no memory, quorum, floor, or fence"), file=out)
    for kind, (s, label, dd) in sorted(found.items()):
        trace = [label]
        while parent[s] is not None:
            s, lab = parent[s]
            trace.append(lab)
        trace.reverse()
        tag = "VIOLATION" if kind in bad else "expected beyond the model"
        print(f"  {tag} ({kind}), shortest trace, {len(trace)} steps:", file=out)
        for step in trace:
            print(f"    {step}", file=out)
    out.flush()
    return found, complete


def replay(cfg, labels, out=sys.stdout):
    """Follows a forced schedule; reports each step's violations and stops at an unknown label."""
    s = initial(cfg)
    print(f"forced schedule under {cfg}:", file=out)
    hit = []
    for i, want in enumerate(labels, 1):
        succ = {label: (t, check) for label, t, check in successors(cfg, s)}
        if want not in succ:
            near = sorted(l for l in succ if l.split(" ")[0] == want.split(" ")[0])[:8]
            print(f"  step {i}: '{want}' is not enabled here; enabled of its kind: {near}", file=out)
            return None
        s, check = succ[want]
        v = violations(check)
        hit += v
        print(f"  {i:2}. {want}" + (f"   <- {', '.join(v)}" if v else ""), file=out)
    print(f"  result: {', '.join(hit) if hit else 'no violation'}", file=out)
    return hit


SCHEDULES = {
    # DM-R1's first trace (model-d10.out), at three voters.
    "dmr1": ["elect 0 term 1 votes from [1]", "write v0 at 0 term 1", "replicate 0->1",
             "commit at 0: memory 2, durable 0", "os-crash 0 (keeps 0 synced entries)",
             "recovery_timeout 0", "elect 0 term 2 votes from [2]"],
    # Five voters, at most two forgetful at once: a voter that heard f + 1 forgetful voters in
    # two episodes that never overlap (0 re-learns before 2 forgets) campaigns without v0,
    # which 0 holds again.
    "fplus5": ["elect 0 term 1 votes from [1, 2]", "write v0 at 0 term 1", "replicate 0->1",
               "replicate 0->2", "commit at 0: memory 2, durable 0",
               "os-crash 1 (keeps 0 synced entries)", "os-crash 0 (keeps 0 synced entries)",
               "probe 0->1", "elect 2 term 2 votes from [3, 4]", "replicate 2->0",
               "os-crash 2 (keeps 0 synced entries)", "probe 2->1", "recovery_timeout 1",
               "fail-elect 3 term 3 reaching [1]", "elect 1 term 4 votes from [3, 4]"],
    # The same without the timeout: every voter heard, but 0 was heard while it had lost v0,
    # and it re-learned v0 afterwards.
    "heard5": ["elect 0 term 1 votes from [1, 2]", "write v0 at 0 term 1", "replicate 0->1",
               "replicate 0->2", "commit at 0: memory 2, durable 0",
               "os-crash 1 (keeps 0 synced entries)", "os-crash 0 (keeps 0 synced entries)",
               "probe 0->1", "elect 2 term 2 votes from [3, 4]", "replicate 2->0",
               "os-crash 2 (keeps 0 synced entries)", "probe 2->1", "probe 4->1",
               "fail-elect 3 term 3 reaching [1]", "elect 1 term 4 votes from [3, 4]"],
    # Beyond the model at three voters: 0 and 1 lose v0, 2 still holds it and is not forgetful.
    # 2 can win neither a normal election (forgetful voters grant only recovery requests) nor,
    # unless any voter that heard f + 1 forgetful voters may start one, a recovery election.
    "live3": ["elect 0 term 1 votes from [1, 2]", "write v0 at 0 term 1", "replicate 0->2",
              "replicate 0->1", "commit at 0: memory 2, durable 0",
              "os-crash 0 (keeps 0 synced entries)", "os-crash 1 (keeps 0 synced entries)",
              "probe 0->2", "probe 1->2", "round 2 term 2", "ask 2->0 granted forgetful",
              "ask 2->1 granted forgetful", "decide 2 term 2 (3 grants, 2 forgetful)"],
}


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("mode", choices=MODES)
    p.add_argument("--voters", type=int, default=3)
    p.add_argument("--fmax", type=int, help="forgetful voters at once (default f: inside the model)")
    p.add_argument("--maxos", type=int, default=2)
    p.add_argument("--maxp", type=int, default=1)
    p.add_argument("--maxt", type=int, default=2)
    p.add_argument("--depth", type=int, default=10)
    p.add_argument("--limit", type=int, default=20_000_000)
    p.add_argument("--compat", action="store_true", help="the architect's probe, exactly")
    p.add_argument("--schedule", choices=sorted(SCHEDULES), help="replay a forced schedule instead")
    a = p.parse_args()
    fmax = a.fmax if a.fmax is not None else (a.voters - 1) // 2
    cfg = Config(a.mode, a.voters, fmax, a.maxos, a.maxp, a.maxt, a.compat)
    print(f"# {time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())} {' '.join(sys.argv[1:])}")
    if a.schedule:
        replay(cfg, SCHEDULES[a.schedule])
    else:
        run(cfg, a.depth, a.limit)


if __name__ == "__main__":
    main()
