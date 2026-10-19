#!/usr/bin/env python3
"""Explicit-state model of durability-modes D2, D5, D6 and D7 (revisions 1 to 3).

Extends the architect's DM-R1 probe (strata-wt/review-durability/.scratch/review-durability/
forgetful_model.py) and keeps its semantics: atomic steps; partitions and process crashes
unbounded (any node may take no part in any step); at most FMAX nodes forgetful at once, at
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

Revision 2 adds, each off unless asked for, so every revision-1 configuration keeps its state
space exactly:
  --restart text|persist  same-boot process restarts, also a clean close and reboot (at most
                          --maxr in a run): the page cache keeps every written entry, recovery
                          rewrites and syncs it, and the volatile state resets (role, commit_memory
                          to commit_durable, counts, timer, round). `text` clears forgetfulness, as
                          revision 1's D3 read (DM-R19's bug); `persist` keeps it until the voter
                          re-learns, revision 2's durable mark (D5).
  --torn                  a restart may also tear the unsynced tail: it keeps any prefix from the
                          synced one, rewrites and syncs it, and becomes forgetful (D3, D5).
  --retry                 a stuck recovery candidate starts another round at its term + 1 (D6).
  --base, --script        membership in the log (Track C's D1, D3, D4; DM-R20): the founding
                          voters, then configuration changes in order (`add:X` promotes node X,
                          which receives appends as a learner until then; `remove:Y`). A node's
                          membership in force is the newest configuration entry in its synced
                          prefix; a leader admits the next change only with an entry of its term
                          at or below its commit_durable and no configuration entry above it (D7),
                          and an added node's written and synced progress at least the commit
                          cursors; elections, commits, counts, and rounds use the membership in
                          force at the node that counts (votes from its voters, a majority of
                          them, f = (n - 1) // 2 of n); the leader counts itself only while a voter.
  --count f1|major        the recovery decision's flagged answers: f + 1 (revision 1), or a
                          majority of the candidate's membership, n // 2 + 1 (revision 2: f + 1
                          for odd n, f + 2 for even n).

Revision 3 (DM-R28) adds D4's vote barrier, on by default since it is the design's behavior:
  --barrier candidate     a candidate syncs its term and vote before any request leaves, normal
                          elections and recovery rounds alike, a barrier that covers every entry it
                          has written (D3), so it counts under the newest configuration entry in its
                          whole log and campaigns only as a voter of that membership. Revision 2's
                          model counted under its synced prefix's membership, which let `grow6`'s
                          W win under a membership the design does not admit.
  --barrier all           also every other term change and vote grant: a voter that moves to a new
                          term or grants syncs before it replies (D4), and its barrier covers its
                          whole written log, a follower's first append of a new term included.
  --barrier none          revision 2's model, which reproduces revision 2's counts.

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
               as up to date as its own), enough of them flagged forgetful (--count), from every
               voter or, once its timeout passed, a majority; a forgetful voter grants nothing else
  round_heard  the same, every voter's answer required
Violations: "memory" (a new leader lacks a write acknowledged at memory: inside the model
only), "quorum" (one acknowledged at quorum), "floor", and "fence" (inside the model only).
A counterexample is real; a pass is evidence bounded by the depth and the scope.
"""
import argparse
import sys
import time

FOLLOWER, CANDIDATE, LEADER, RCAND = 0, 1, 2, 3
NOOP, REC = 0, 1  # entry values; a client write k is 2 + k; configuration change k is CFG + k
CFG = 100
(TERM, VOTED, LOG, SYNCED, FORG, ROLE, COMMIT, SEEN, SMAX, TFIRED, MATCH, SMATCH,
 SEENF, CDUR, PCDUR, RND) = range(16)
FIELDS = {name: i for i, name in enumerate(
    "TERM VOTED LOG SYNCED FORG ROLE COMMIT SEEN SMAX TFIRED MATCH SMATCH SEENF CDUR PCDUR RND".split())}
MODES = ("none", "heard", "timeout", "fplus", "fplus_heard", "round", "round_heard")
# Replays only: a forced schedule written for one barrier mode may name a sync or a replication that
# another mode's barriers already made a no-op (--barrier all); with NOOPS those steps are emitted as
# transitions to the same state, so the schedule replays unchanged. Searches never set it.
NOOPS = False


class Config:
    def __init__(self, mode, n, fmax, maxos, maxp, maxt, compat=False, restart=None, maxr=0, torn=False,
                 retry=False, base=None, script=(), count="f1", barrier="candidate"):
        base = frozenset(range(n)) if base is None else frozenset(base)
        assert mode in MODES and (script or len(base) % 2 == 1) and base <= frozenset(range(n))
        assert not compat or (mode in ("none", "heard", "timeout") and not script and restart is None)
        assert barrier in ("none", "candidate", "all")
        self.barrier = "none" if compat else barrier
        assert not script or mode in ("round", "round_heard"), "membership changes need a round mode"
        self.mode, self.N, self.FMAX, self.MAXOS, self.MAXP, self.MAXT = mode, n, fmax, maxos, maxp, maxt
        self.compat = compat
        self.flags = mode in ("fplus", "fplus_heard", "round", "round_heard")
        self.timeout = mode in ("timeout", "fplus", "round")
        self.rounds = mode in ("round", "round_heard")
        self.restart, self.maxr, self.torn, self.retry, self.count = restart, maxr, torn, retry, count
        self.script = tuple(script)
        self.M = [base]
        for op, x in self.script:
            self.M.append(self.M[-1] | {x} if op == "add" else self.M[-1] - {x})
        # The fixed-membership figures, used by the modes that predate membership changes.
        self.F = (len(base) - 1) // 2
        self.MAJ = self.F + 1
        self.ALL = sum(1 << x for x in base)

    def __str__(self):
        extra = ""
        if self.restart:
            extra += f" restart={self.restart} maxr={self.maxr}" + (" torn" if self.torn else "")
        if self.retry:
            extra += " retry"
        if self.script:
            extra += f" base={sorted(self.M[0])} script={','.join(f'{op}:{x}' for op, x in self.script)}"
        if self.rounds and (self.script or self.count != "f1"):
            extra += f" count={self.count}"
        if self.barrier != "none":
            extra += f" barrier={self.barrier}"
        return (f"mode={self.mode} N={self.N} FMAX={self.FMAX} MAXOS={self.MAXOS} MAXP={self.MAXP} "
                f"MAXT={self.MAXT}{' compat' if self.compat else ''}{extra}")


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


def members(cfg, n):
    """The membership in force: the newest configuration entry in the synced prefix (Track C D1)."""
    if not cfg.script:
        return cfg.M[0]
    for t, v in reversed(n[LOG][:n[SYNCED]]):
        if v >= CFG:
            return cfg.M[v - CFG]
    return cfg.M[0]


def fof(m):
    return (len(m) - 1) // 2


def maj(m):
    return len(m) // 2 + 1


def flagged_count(n, m):
    return sum(1 for x in m if n[SEENF] >> x & 1)


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


def synced_all(cfg, n):
    """D4's barrier: the hard state persists, and the core's barrier covers every written entry."""
    return setf(n, SYNCED=len(n[LOG]), PCDUR=n[CDUR])


def at_candidate(cfg, n):
    """The candidate after its campaign's barrier (DM-R28), or as it stands without one."""
    return synced_all(cfg, n) if cfg.barrier != "none" else n


def at_voter(cfg, n):
    """A voter after it persists a new term or a grant: synced only under --barrier all."""
    return synced_all(cfg, n) if cfg.barrier == "all" else n


def writes(log, upto):
    return {(i + 1, log[i][0], log[i][1]) for i in range(upto) if 2 <= log[i][1] < CFG}


def holds(log, acked):
    return all(i <= len(log) and log[i - 1] == (t, v) for (i, t, v) in acked)


def show(v):
    return "noop" if v == NOOP else "rec" if v == REC else f"cfg{v - CFG}" if v >= CFG else f"v{v - 2}"


def successors(cfg, state):
    nodes, amem, adur, nprop, nos, nr = state
    N = cfg.N
    out = []

    def emit(label, ns, am=amem, ad=adur, np=nprop, no=nos, r=nr, check=None):
        out.append((label, (tuple(ns), am, ad, np, no, r), check))

    for c in range(N):
        n = nodes[c]
        mc = members(cfg, n)
        F, MAJ = fof(mc), maj(mc)
        voter = c in mc
        # A normal election: c campaigns at term + 1, reaching the voters in mask. With D4's
        # vote barrier (DM-R28) it counts under its whole log's membership, as a voter of it.
        nb = at_candidate(cfg, n)
        mcb = members(cfg, nb)
        if c in mcb and n[ROLE] != LEADER and n[TERM] < cfg.MAXT and campaign_ok(cfg, n) and not (cfg.rounds and n[FORG]):
            T = n[TERM] + 1
            MAJB = maj(mcb)
            others = [x for x in sorted(mcb) if x != c]
            for mask in range(1, 1 << len(others)):
                ns = list(nodes)
                cand = setf(nb, TERM=T, VOTED=c, ROLE=CANDIDATE, MATCH=None, SMATCH=None, RND=None)
                cpos = lastpos(cand[LOG])
                votes = 1
                reached = [r for b, r in enumerate(others) if mask >> b & 1]
                for r in reached:
                    rn = ns[r]
                    moved = T > rn[TERM]
                    if moved:
                        rn = setf(rn, TERM=T, VOTED=-1, ROLE=FOLLOWER, MATCH=None, SMATCH=None, RND=None)
                    if rn[FORG]:
                        rn = setf(rn, SEEN=rn[SEEN] | (1 << c), SMAX=max(rn[SMAX], cpos))
                    granted = rn[TERM] == T and rn[VOTED] in (-1, c) and cpos >= lastpos(rn[LOG]) and grant_ok(cfg, rn, cpos)
                    if granted:
                        rn = setf(rn, VOTED=c)
                        votes += 1
                    if moved or granted:
                        rn = at_voter(cfg, rn)
                    ns[r] = rn
                if votes >= MAJB:
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
        # Pre-vote probes: a voter's requests reach forgetful voters (D6's "seen"), and under the
        # flag modes say whether their sender is forgetful (kind 6).
        if cfg.mode != "none" and n[ROLE] != LEADER and voter:
            pos = lastpos(n[LOG])
            for r in sorted(mc):
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
        if cfg.timeout and (n[FORG] or (cfg.rounds and flagged_count(n, mc) >= F + 1)) and not n[TFIRED]:
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
        # The next configuration change (Track C D3), admitted on the durable cursor (D7).
        if cfg.script and n[ROLE] == LEADER:
            L = n[LOG]
            cfgs = [i for i in range(len(L)) if L[i][1] >= CFG]
            k = (L[cfgs[-1]][1] - CFG if cfgs else 0) + 1
            if k <= len(cfg.script) and all(i + 1 <= n[CDUR] for i in cfgs) and \
                    any(L[i][0] == n[TERM] for i in range(min(n[CDUR], len(L)))):
                op, x = cfg.script[k - 1]
                if op == "remove" or (n[MATCH][x] >= n[COMMIT] and n[SMATCH][x] >= n[CDUR]):
                    log = L + ((n[TERM], CFG + k),)
                    m = list(n[MATCH])
                    m[c] = len(log)
                    ns = list(nodes)
                    ns[c] = setf(n, LOG=log, MATCH=tuple(m))
                    emit(f"{op} {x} at {c} term {n[TERM]}", ns)
        # Replication: the follower takes the leader's log by Raft's conflict rule, acknowledges
        # the written and the synced index (D7), learns both cursors clamped to what it
        # verified, and re-learns if forgetful (D5). Learners and removed members receive too.
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
                pcdur = fn[PCDUR]
                if cfg.barrier == "all" and fn[TERM] < n[TERM]:
                    # The new term persists before the reply, and the barrier covers the batch.
                    synced, pcdur = len(newlog), cdur
                fn2 = setf(fn, TERM=n[TERM], VOTED=fn[VOTED] if fn[TERM] == n[TERM] else -1, ROLE=FOLLOWER,
                           LOG=newlog, SYNCED=synced, COMMIT=commit, FORG=False, SEEN=0, SMAX=(0, 0),
                           TFIRED=False, MATCH=None, SMATCH=None, SEENF=0, CDUR=cdur, RND=None, PCDUR=pcdur)
                m = list(n[MATCH])
                sm = list(n[SMATCH])
                m[f] = len(L)
                sm[f] = min(synced, len(L))
                ns = list(nodes)
                ns[f] = fn2
                ns[c] = setf(n, MATCH=tuple(m), SMATCH=tuple(sm))
                if ns[f] == nodes[f] and ns[c] == nodes[c] and not NOOPS:
                    continue
                emit(f"replicate {c}->{f}" + (f" {check[0].upper()}" if check else ""), ns, check=check)
        # Commit: commit_memory over written matches, commit_durable over synced ones (D7), each
        # over the voters of the leader's membership in force.
        if n[ROLE] == LEADER:
            L = n[LOG]
            m = list(n[MATCH])
            m[c] = len(L)
            sm = list(n[SMATCH])
            sm[c] = n[SYNCED]
            newc = n[COMMIT]
            for k in range(len(L), n[COMMIT], -1):
                if L[k - 1][0] == n[TERM] and sum(1 for x in mc if m[x] >= k) >= MAJ:
                    newc = k
                    break
            dur = 0
            for k in range(len(L), 0, -1):
                if L[k - 1][0] == n[TERM] and sum(1 for x in mc if sm[x] >= k) >= MAJ:
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
        if n[SYNCED] < len(n[LOG]) or pcdur != n[PCDUR] or NOOPS:
            ns = list(nodes)
            ns[c] = setf(n, SYNCED=len(n[LOG]), PCDUR=pcdur)
            emit(f"sync {c}", ns)
        # An OS crash or power loss: the unsynced tail goes; term, vote, and the synced hard
        # state's commit_durable stay (D4); commit_memory restarts from it.
        forgetful_after = sum(1 for k in range(N) if k != c and nodes[k][FORG]) + 1
        if nos < cfg.MAXOS:
            if forgetful_after <= cfg.FMAX:
                log = n[LOG][:n[SYNCED]]
                cdur = 0 if cfg.compat else min(n[PCDUR], len(log))
                ns = list(nodes)
                ns[c] = setf(n, LOG=log, FORG=True, ROLE=FOLLOWER, COMMIT=cdur, SEEN=1 << c, SMAX=lastpos(log),
                             TFIRED=False, MATCH=None, SMATCH=None, SEENF=(1 << c) if cfg.flags else 0,
                             CDUR=cdur, RND=None)
                emit(f"os-crash {c} (keeps {len(log)} synced entries)", ns, no=nos + 1)
        # A same-boot process restart, or a clean close and reboot (revision 2): the page cache
        # holds every written entry, which recovery rewrites and syncs; the forgetful mark
        # survives under `persist`, and a torn tail keeps a shorter prefix and makes it forgetful.
        if cfg.restart and nr < cfg.maxr:
            forg = n[FORG] if cfg.restart == "persist" else False
            cdur = n[CDUR]
            ns = list(nodes)
            ns[c] = setf(n, ROLE=FOLLOWER, SYNCED=len(n[LOG]), PCDUR=cdur, COMMIT=min(cdur, len(n[LOG])), FORG=forg,
                         SEEN=(1 << c) if forg else 0, SMAX=lastpos(n[LOG]) if forg else (0, 0), TFIRED=False,
                         MATCH=None, SMATCH=None, SEENF=(1 << c) if (forg and cfg.flags) else 0, RND=None)
            emit(f"process-restart {c}" + (" forgetful" if forg else ""), ns, r=nr + 1)
            if cfg.torn and (n[FORG] or forgetful_after <= cfg.FMAX):
                for keep in range(n[SYNCED], len(n[LOG])):
                    log = n[LOG][:keep]
                    cdur = min(n[CDUR], keep)
                    ns = list(nodes)
                    ns[c] = setf(n, LOG=log, ROLE=FOLLOWER, SYNCED=keep, PCDUR=cdur, COMMIT=cdur, FORG=True,
                                 SEEN=1 << c, SMAX=lastpos(log), TFIRED=False, MATCH=None, SMATCH=None,
                                 SEENF=(1 << c) if cfg.flags else 0, CDUR=cdur, RND=None)
                    emit(f"torn-restart {c} (keeps {keep} entries)", ns, r=nr + 1)
        # The recovery election (round modes).
        if cfg.rounds:
            nb = at_candidate(cfg, n)
            if voter and c in members(cfg, nb) and n[ROLE] in (FOLLOWER, CANDIDATE) and n[TERM] < cfg.MAXT and \
                    flagged_count(n, mc) >= F + 1:
                rnd = [None] * N
                rnd[c] = (n[FORG], True)
                ns = list(nodes)
                ns[c] = setf(nb, TERM=n[TERM] + 1, VOTED=c, ROLE=RCAND, MATCH=None, SMATCH=None, RND=tuple(rnd))
                emit(f"round {c} term {n[TERM] + 1}", ns)
            if n[ROLE] == RCAND:
                U, cpos = n[TERM], lastpos(n[LOG])
                if cfg.retry and voter and U < cfg.MAXT and flagged_count(n, mc) >= F + 1:
                    rnd = [None] * N
                    rnd[c] = (n[FORG], True)
                    ns = list(nodes)
                    ns[c] = setf(at_candidate(cfg, n), TERM=U + 1, VOTED=c, RND=tuple(rnd))
                    emit(f"round-again {c} term {U + 1}", ns)
                for r in sorted(mc):
                    if r == c or n[RND][r] is not None:
                        continue
                    rn = nodes[r]
                    ns = list(nodes)
                    if rn[TERM] > U:
                        ns[c] = setf(at_voter(cfg, n), TERM=rn[TERM], VOTED=-1, ROLE=FOLLOWER, RND=None)
                        emit(f"ask {c}->{r} refused at term {rn[TERM]}", ns)
                        continue
                    moved = U > rn[TERM]
                    if moved:
                        rn = setf(rn, TERM=U, VOTED=-1, ROLE=FOLLOWER, MATCH=None, SMATCH=None, RND=None)
                    if rn[FORG]:
                        rn = setf(rn, SEEN=rn[SEEN] | (1 << c), SMAX=max(rn[SMAX], cpos))
                    granted = rn[VOTED] in (-1, c) and cpos >= lastpos(rn[LOG])
                    if granted:
                        rn = setf(rn, VOTED=c)
                    if moved or granted:
                        rn = at_voter(cfg, rn)
                    rnd = list(n[RND])
                    rnd[r] = (rn[FORG], granted)
                    ns[r] = rn
                    ns[c] = setf(n, RND=tuple(rnd))
                    emit(f"ask {c}->{r}" + (" granted" if granted else "") + (" forgetful" if rn[FORG] else ""), ns)
                answers = [n[RND][x] for x in sorted(mc) if n[RND][x] is not None]
                grants = sum(1 for x in answers if x[1])
                forgetful = sum(1 for x in answers if x[0])
                heard = len(answers) == len(mc)
                allowed = heard if cfg.mode == "round_heard" else heard or n[TFIRED]
                need = F + 1 if cfg.count == "f1" else MAJ
                # Every answer at its term a grant: a refusal (another vote at U, or a log ahead of
                # its own) ends the round.
                if voter and grants == len(answers) and grants >= MAJ and forgetful >= need and allowed:
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
    return (tuple(fresh() for _ in range(cfg.N)), frozenset(), frozenset(), 0, 0, 0)


def beyond_model(cfg):
    return cfg.FMAX > min(fof(m) for m in cfg.M)


def run(cfg, depth, limit, out=sys.stdout, start_state=None, start_depth=0):
    start = time.time()
    init = initial(cfg) if start_state is None else start_state
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
    origin = f" from step {start_depth} of a schedule" if start_state is not None else ""
    print(f"{cfg}{origin}: {len(parent):,} states, {'all reached' if complete else f'bounded at depth {d}'}"
          f" ({time.time() - start:.0f} s); states by depth {per_depth}", file=out)
    beyond = beyond_model(cfg)
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


def replay(cfg, labels, out=sys.stdout, quiet=False):
    """Follows a forced schedule; reports each step's violations and stops at an unknown label.
    Returns (violations, final state), or (None, the state where it stopped)."""
    s = initial(cfg)
    if not quiet:
        print(f"forced schedule under {cfg}:", file=out)
    hit, peak = [], 0
    for i, want in enumerate(labels, 1):
        global NOOPS
        NOOPS = True
        try:
            succ = {label: (t, check) for label, t, check in successors(cfg, s)}
        finally:
            NOOPS = False
        if want not in succ:
            near = sorted(l for l in succ if l.split(" ")[0] == want.split(" ")[0])[:8]
            if not quiet:
                print(f"  step {i}: '{want}' is not enabled here; enabled of its kind: {near}", file=out)
            return None, s
        s, check = succ[want]
        peak = max(peak, sum(1 for n in s[0] if n[FORG]))
        v = violations(check)
        hit += v
        if not quiet:
            print(f"  {i:2}. {want}" + (f"   <- {', '.join(v)}" if v else ""), file=out)
    if not quiet:
        print(f"  result: {', '.join(hit) if hit else 'no violation'}; at most {peak} voters forgetful at once",
              file=out)
    return hit, s


def final_state(cfg, s, out=sys.stdout):
    nodes, amem, adur = s[0], s[1], s[2]
    print(f"  acknowledged at memory {sorted(amem)}, at quorum {sorted(adur)}", file=out)
    for i, n in enumerate(nodes):
        role = "FCLR"[n[ROLE]]
        held = all(i2 <= len(n[LOG]) and n[LOG][i2 - 1] == (t, v) for (i2, t, v) in amem)
        print(f"  node {i}: term {n[TERM]} role {role} forgetful {n[FORG]} members {sorted(members(cfg, n))} "
              f"commit_memory {n[COMMIT]} commit_durable {n[CDUR]} holds the memory writes {held} "
              f"log [{', '.join(f'{show(v)}@{t}' for t, v in n[LOG])}]", file=out)


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
    # Five voters, found by forgetful_check's swarm walks (kind first, seed 11): voter 4 counts
    # forgetful voters 0, 2 and itself over episodes that never overlap, grants, and an honest
    # follower must later replace an entry at or below its commit.
    "fplus5b": ["elect 2 term 1 votes from [0, 3]", "replicate 2->0", "replicate 2->4",
                "commit at 2: memory 1, durable 0", "os-crash 4 (keeps 0 synced entries)",
                "replicate 2->0", "recovery_timeout 4", "os-crash 0 (keeps 0 synced entries)",
                "recovery_timeout 0", "write v0 at 2 term 1", "probe 3->4", "probe 0->4", "probe 1->4",
                "probe 1->0", "replicate 2->0", "os-crash 2 (keeps 0 synced entries)", "probe 0->2",
                "elect 0 term 2 votes from [1, 3]", "probe 2->4", "replicate 0->2",
                "elect 3 term 3 votes from [0, 1, 4]", "probe 0->4", "elect 0 term 4 votes from [1, 2, 3, 4]",
                "elect 3 term 5 votes from [0, 1, 4]", "replicate 3->4", "replicate 3->0 FENCE"],
    # Beyond the model at three voters: 0 and 1 lose v0, 2 still holds it and is not forgetful.
    # 2 can win neither a normal election (forgetful voters grant only recovery requests) nor,
    # unless any voter that heard f + 1 forgetful voters may start one, a recovery election.
    "live3": ["elect 0 term 1 votes from [1, 2]", "write v0 at 0 term 1", "replicate 0->2",
              "replicate 0->1", "commit at 0: memory 2, durable 0",
              "os-crash 0 (keeps 0 synced entries)", "os-crash 1 (keeps 0 synced entries)",
              "probe 0->2", "probe 1->2", "round 2 term 2", "ask 2->0 granted forgetful",
              "ask 2->1 granted forgetful", "decide 2 term 2 (3 grants, 2 forgetful)"],
    # DM-R19 (the architect's restart3, --restart text): one OS crash, then a process restart in
    # the new boot before re-learning; three voters, at most one forgetful at once. Under
    # --restart persist the restart keeps 0 forgetful, and step 7 is not enabled.
    "restart3": ["elect 0 term 1 votes from [1]", "write v0 at 0 term 1", "replicate 0->1",
                 "commit at 0: memory 2, durable 0", "os-crash 0 (keeps 0 synced entries)",
                 "process-restart 0", "elect 0 term 2 votes from [2]"],
    "restart3p": ["elect 0 term 1 votes from [1]", "write v0 at 0 term 1", "replicate 0->1",
                  "commit at 0: memory 2, durable 0", "os-crash 0 (keeps 0 synced entries)",
                  "process-restart 0 forgetful", "elect 0 term 2 votes from [2]"],
    # The same through a torn same-boot tail instead of an OS crash (--restart text --torn):
    # 0 restarts with its tail torn (forgetful), restarts again cleanly (text: not forgetful),
    # and campaigns without v0.
    "torn3": ["elect 0 term 1 votes from [1]", "write v0 at 0 term 1", "replicate 0->1",
              "commit at 0: memory 2, durable 0", "torn-restart 0 (keeps 1 entries)",
              "process-restart 0", "elect 0 term 2 votes from [2]"],
}

# DM-R20, the architect's `six` (r1/forgetful_members.py): six voters in a replacement window at
# rf = 5, node 2 leaving (--base 0,1,2,3,4,5 --script remove:2). X = 0 counts f + 1 = 3 forgetful
# answers at term 2 from episodes that never overlap: 2 (first episode), then 1, then 2 again;
# 1 re-learns in between from 5, elected at term 3 by 3, 4, 5 under the five-voter membership
# that removes 2. At most two voters are forgetful at any instant, f for both memberships.
SIX = [
    "elect 4 term 1 votes from [0, 1, 2]",
    "replicate 4->0", "replicate 4->1", "replicate 4->2", "replicate 4->3", "replicate 4->5",
    "sync 0", "sync 1", "sync 2", "sync 3", "sync 4", "sync 5",
    "replicate 4->0", "replicate 4->1", "replicate 4->2", "replicate 4->3", "replicate 4->5",
    "commit at 4: memory 1, durable 1",
    "write v0 at 4 term 1",
    "replicate 4->5", "replicate 4->0", "replicate 4->1",
    "commit at 4: memory 2, durable 1",
    "remove 2 at 4 term 1", "sync 4", "replicate 4->5", "sync 5",
    "os-crash 0 (keeps 1 synced entries)",
    "os-crash 2 (keeps 1 synced entries)", "probe 2->0", "replicate 4->2",
    "os-crash 1 (keeps 1 synced entries)", "probe 1->0",
    "round 0 term 2",
    "ask 0->1 granted forgetful",
    "ask 0->3 granted",
    "fail-elect 5 term 2 reaching [4]",
    "elect 5 term 3 votes from [3, 4]",
    "replicate 5->1",
    "os-crash 2 (keeps 1 synced entries)",
    "ask 0->2 granted forgetful",
    "recovery_timeout 0",
    "decide 0 term 2 (4 grants, 3 forgetful)",
]
# ... and the loss: 0 leads, 2 re-learns from it, 0 learns a higher term, wins a normal election
# with logs that end below term 2, and its appends replace v0 everywhere under recovery_term 2.
SIX_LOSS = SIX + [
    "replicate 0->2",
    "fail-elect 3 term 4 reaching [0]",
    "elect 0 term 5 votes from [2, 3, 4]",
    "replicate 0->4", "replicate 0->5", "replicate 0->1",
]
# The other half of the window (--base 0,1,2,3,4 --script add:5,remove:2): node 5 is promoted
# (six voters, f = 2) and the candidate X = 0 counts under the six-voter membership, while 3,
# which never synced the promotion, is elected under the five-voter founding membership between
# X's answers: 5's first episode, then 1's (1 re-learns from 3), then 5's second. At most two
# voters are forgetful at any instant, f for both memberships.
GROW6 = [
    "elect 4 term 1 votes from [0, 1]",
    "replicate 4->0", "replicate 4->1", "replicate 4->2", "replicate 4->3", "replicate 4->5",
    "sync 0", "sync 1", "sync 2", "sync 3", "sync 4", "sync 5",
    "replicate 4->0", "replicate 4->1", "replicate 4->2", "replicate 4->3", "replicate 4->5",
    "commit at 4: memory 1, durable 1",
    "add 5 at 4 term 1",
    "replicate 4->0", "replicate 4->1", "replicate 4->5",
    "sync 0", "sync 1", "sync 4", "sync 5",
    "write v0 at 4 term 1",
    "replicate 4->0", "replicate 4->1", "replicate 4->5",
    "commit at 4: memory 3, durable 2",
    "replicate 4->3",
    "os-crash 0 (keeps 2 synced entries)",
    "os-crash 5 (keeps 2 synced entries)", "probe 5->0", "replicate 4->5",
    "os-crash 1 (keeps 2 synced entries)", "probe 1->0",
    "round 0 term 2",
    "ask 0->1 granted forgetful",
    "ask 0->2 granted",
    "fail-elect 3 term 2 reaching [4]",
    "elect 3 term 3 votes from [2, 4]",
    "replicate 3->1",
    "os-crash 5 (keeps 2 synced entries)",
    "ask 0->5 granted forgetful",
    "recovery_timeout 0",
    "decide 0 term 2 (4 grants, 3 forgetful)",
]
# ... and the loss at the leader the window lets win: 0 leads at term 2, learns a higher term,
# and wins a normal election at term 5 under the six-voter membership with voters whose logs end
# below term 3; its appends replace v0 at 4 under recovery_term 2.
GROW6_LOSS = GROW6 + [
    "replicate 0->5",
    "fail-elect 2 term 4 reaching [0]",
    "elect 0 term 5 votes from [2, 4, 5]",
    "replicate 0->4",
]
# Track C's rf = 3 window (--voters 4 --base 0,1,2 --script add:3,remove:1 --fmax 1): node 3
# promoted (four voters, f = 1), v0 acknowledged at memory on three of them, and nothing forgetful
# yet. A prefix for bounded searches of the four-voter window, not a counterexample.
RF3WIN = [
    "elect 0 term 1 votes from [1]",
    "replicate 0->1", "replicate 0->2", "replicate 0->3",
    "sync 0", "sync 1", "sync 2", "sync 3",
    "replicate 0->1", "replicate 0->2", "replicate 0->3",
    "commit at 0: memory 1, durable 1",
    "add 3 at 0 term 1",
    "replicate 0->1", "replicate 0->2", "replicate 0->3",
    "sync 0", "sync 1", "sync 2", "sync 3",
    "write v0 at 0 term 1",
    "replicate 0->1", "replicate 0->2",
    "commit at 0: memory 3, durable 2",
]
# DM-R28 (the architect's re-check of revision 2): `grow6` above is the model's artifact. With D4's
# vote barrier, 3 syncs the promotion it holds at its campaign and counts six voters, so step 43
# ("elect 3 term 3 votes from [2, 4]") is not enabled; `six` replays unchanged. The growth failure
# is real one size up: `grow67` (the architect's, r2/grow67.py), rf = 5 to 7 (--voters 7 --base
# 0,1,2,3,4 --script add:5,add:6 --fmax 2), where X = 0 counts under the six-voter membership C6
# (f = 2) and W = 5 is elected under C7 (f = 3) between X's answers, the case C' = C plus a voter.
# FMAX 2 is the least f of the memberships in force (C5, C6, C7), so it is within the model.
_G67_ALL = [0, 1, 2, 3, 5, 6]
GROW67 = (
    ["elect 4 term 1 votes from [0, 1]"]
    + [f"replicate 4->{x}" for x in _G67_ALL] + [f"sync {x}" for x in range(7)] + [f"replicate 4->{x}" for x in _G67_ALL]
    + ["commit at 4: memory 1, durable 1", "add 5 at 4 term 1"]
    + [f"replicate 4->{x}" for x in _G67_ALL] + [f"sync {x}" for x in range(7)] + [f"replicate 4->{x}" for x in _G67_ALL]
    + ["commit at 4: memory 2, durable 2", "add 6 at 4 term 1",
       "replicate 4->5", "replicate 4->6", "sync 4", "sync 5", "sync 6",
       "write v0 at 4 term 1",
       "replicate 4->0", "replicate 4->1", "replicate 4->2", "replicate 4->5",
       "commit at 4: memory 4, durable 2",
       "os-crash 0 (keeps 2 synced entries)", "os-crash 2 (keeps 2 synced entries)", "probe 2->0",
       "replicate 4->2", "os-crash 1 (keeps 2 synced entries)", "probe 1->0",
       "round 0 term 2", "ask 0->1 granted forgetful", "ask 0->3 granted",
       "fail-elect 5 term 2 reaching [4]", "elect 5 term 3 votes from [3, 4, 6]",
       "replicate 5->1",
       "os-crash 2 (keeps 2 synced entries)", "ask 0->2 granted forgetful",
       "recovery_timeout 0", "decide 0 term 2 (4 grants, 3 forgetful)"]
)
# ... and the loss, built here: 0 leads at term 2 without v0 or C7, learns term 4, wins a normal
# election at term 5 under C6 with voters whose logs end below term 2's last index, and its
# appends replace v0 on every replica that held it (4 under recovery_term 2; 5 and 1 above their
# commit_memory).
GROW67_LOSS = GROW67 + [
    "replicate 0->2",
    "fail-elect 3 term 4 reaching [0]",
    "elect 0 term 5 votes from [2, 3, 4]",
    "replicate 0->4", "replicate 0->5", "replicate 0->1",
]
# The architect's prefixes for the other window shapes (r2/members_prefixes.py), each with v0
# acknowledged at memory in the even window: rf = 5 to 3 (--voters 5 --script remove:4,remove:3),
# and two replacements at rf = 3 (--voters 5 --base 0,1,2 --script add:3,remove:0,add:4,remove:1),
# whose second window has memberships two changes apart in force at once.
SHRINK31 = [
    "elect 0 term 1 votes from [1, 2]",
    "replicate 0->1", "replicate 0->2", "replicate 0->3", "replicate 0->4",
    "sync 0", "sync 1", "sync 2", "sync 3", "sync 4",
    "replicate 0->1", "replicate 0->2", "replicate 0->3", "replicate 0->4",
    "commit at 0: memory 1, durable 1",
    "remove 4 at 0 term 1",
    "replicate 0->1", "replicate 0->2", "replicate 0->3",
    "sync 0", "sync 1", "sync 2", "sync 3",
    "replicate 0->1", "replicate 0->2", "replicate 0->3",
    "commit at 0: memory 2, durable 2",
    "write v0 at 0 term 1",
    "replicate 0->1", "replicate 0->2",
    "commit at 0: memory 3, durable 2",
]
# The architect's shrink search's fence (r2/runs/cc-shrink31-*-fm1-d7.out), under both counts and
# with no forgetful voter: 4, removed, holds its removal unsynced and wins under the founding five.
# With the vote barrier its step 37 is not enabled.
SHRINK_FENCE = SHRINK31 + [
    "replicate 0->4", "remove 3 at 0 term 1", "replicate 0->1", "sync 0",
    "commit at 0: memory 4, durable 2", "elect 4 term 2 votes from [2, 3]", "replicate 4->0 FENCE",
]
REPLACE2 = [
    "elect 0 term 1 votes from [1]",
    "replicate 0->1", "replicate 0->2", "replicate 0->3", "replicate 0->4",
    "sync 0", "sync 1", "sync 2", "sync 3", "sync 4",
    "replicate 0->1", "replicate 0->2", "replicate 0->3", "replicate 0->4",
    "commit at 0: memory 1, durable 1",
    "add 3 at 0 term 1",
    "replicate 0->1", "replicate 0->2", "replicate 0->3",
    "sync 0", "sync 1", "sync 2", "sync 3",
    "replicate 0->1", "replicate 0->2", "replicate 0->3",
    "commit at 0: memory 2, durable 2",
    "remove 0 at 0 term 1",
    "replicate 0->1", "replicate 0->2", "replicate 0->3",
    "sync 0", "sync 1", "sync 2", "sync 3",
    "replicate 0->1", "replicate 0->2", "replicate 0->3",
    "commit at 0: memory 3, durable 3",
    "elect 1 term 2 votes from [2, 3]",
    "replicate 1->2", "replicate 1->3", "replicate 1->4",
    "sync 1", "sync 2", "sync 3", "sync 4",
    "replicate 1->2", "replicate 1->3", "replicate 1->4",
    "commit at 1: memory 4, durable 4",
    "write v0 at 1 term 2",
    "replicate 1->2", "replicate 1->3",
    "commit at 1: memory 5, durable 4",
    "replicate 1->4", "sync 4", "replicate 1->4",
    "add 4 at 1 term 2",
    "replicate 1->2", "replicate 1->4",
    "sync 1", "sync 2", "sync 4",
]
# DM-R26: `manual`'s f + 1 admission with every voter's answer required (round_heard, no timeout):
# X asks the voters that still hold v0 before W's election, and they refuse a log behind theirs,
# which ends the round (the architect's r2/six_heard.py; grow67's the same at its step 63).
SIX_HEARD = SIX[:36] + ["ask 0->4", "ask 0->5"]
GROW67_HEARD = GROW67[:63] + ["ask 0->4", "ask 0->5"]
# Revision 1's five-voter schedules relabeled for D4's vote barrier: 2 campaigned at step 9 with v0,
# so its barrier synced v0 and its OS crash keeps two entries. Under the candidate's barrier the
# relabeled step 15 is not enabled (results/r3/schedules.out): neither loses v0. The five-voter loss
# under every barrier is LEAD5 (below).
FPLUS5_VB = [l.replace("os-crash 2 (keeps 0 synced entries)", "os-crash 2 (keeps 2 synced entries)")
             for l in SCHEDULES["fplus5"]]
HEARD5_VB = [l.replace("os-crash 2 (keeps 0 synced entries)", "os-crash 2 (keeps 2 synced entries)")
             for l in SCHEDULES["heard5"]]
# DM-R30 (the architect's r3/lead5.py): the owner's rule at five voters under every barrier D4
# imposes. The leader that lets voter 2 re-learn v0 is the one that wrote it, which campaigned before
# v0 existed, so no campaign's barrier covers v0 there; its OS crash is the third forgetful episode
# voter 1 counts, while at most two voters are forgetful at once. HEARD5L replaces the timeout with
# probes from 3 and 4 (revision 0's every-voter-heard rule); LEAD5_LOSS carries the loss to node 2.
LEAD5 = [
    "elect 0 term 1 votes from [1, 2]", "write v0 at 0 term 1", "replicate 0->1", "replicate 0->2",
    "commit at 0: memory 2, durable 0", "os-crash 1 (keeps 0 synced entries)",
    "os-crash 2 (keeps 0 synced entries)", "probe 2->1", "replicate 0->2",
    "os-crash 0 (keeps 0 synced entries)", "probe 0->1", "recovery_timeout 1",
    "elect 1 term 2 votes from [3, 4]",
]
HEARD5L = LEAD5[:11] + ["probe 3->1", "probe 4->1", "elect 1 term 2 votes from [3, 4]"]
LEAD5_LOSS = LEAD5 + ["replicate 1->2 FENCE"]
SCHEDULES["lead5"] = LEAD5
SCHEDULES["heard5l"] = HEARD5L
SCHEDULES["lead5_loss"] = LEAD5_LOSS
SCHEDULES["six_heard"] = SIX_HEARD
SCHEDULES["grow67_heard"] = GROW67_HEARD
SCHEDULES["fplus5_vb"] = FPLUS5_VB
SCHEDULES["heard5_vb"] = HEARD5_VB
SCHEDULES["grow67"] = GROW67
SCHEDULES["grow67_loss"] = GROW67_LOSS
SCHEDULES["shrink31"] = SHRINK31
SCHEDULES["shrink_fence"] = SHRINK_FENCE
SCHEDULES["replace2"] = REPLACE2
SCHEDULES["rf3win"] = RF3WIN
SCHEDULES["grow6"] = GROW6
SCHEDULES["grow6_loss"] = GROW6_LOSS
SCHEDULES["six"] = SIX
SCHEDULES["six_loss"] = SIX_LOSS


def parse_script(text):
    out = []
    for item in filter(None, (text or "").split(",")):
        op, _, x = item.partition(":")
        assert op in ("add", "remove"), item
        out.append((op, int(x)))
    return out


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("mode", choices=MODES)
    p.add_argument("--voters", type=int, default=3, help="nodes (the founding voters unless --base)")
    p.add_argument("--fmax", type=int, help="forgetful nodes at once (default f: inside the model)")
    p.add_argument("--maxos", type=int, default=2)
    p.add_argument("--maxp", type=int, default=1)
    p.add_argument("--maxt", type=int, default=2)
    p.add_argument("--depth", type=int, default=10)
    p.add_argument("--limit", type=int, default=20_000_000)
    p.add_argument("--compat", action="store_true", help="the architect's probe, exactly")
    p.add_argument("--restart", choices=("text", "persist"))
    p.add_argument("--maxr", type=int, default=1)
    p.add_argument("--torn", action="store_true")
    p.add_argument("--retry", action="store_true")
    p.add_argument("--base", help="founding voters, comma-separated (default: every node)")
    p.add_argument("--script", help="configuration changes in order: add:X,remove:Y")
    p.add_argument("--count", choices=("f1", "major"), default="f1")
    p.add_argument("--barrier", choices=("none", "candidate", "all"), default="candidate",
                   help="D4's vote barrier (DM-R28): candidate (default), all, or none (revision 2's model)")
    p.add_argument("--schedule", choices=sorted(SCHEDULES), help="replay a forced schedule instead")
    p.add_argument("--from-step", type=int, help="search exhaustively from this step of --schedule")
    p.add_argument("--final", action="store_true", help="print the final state after --schedule")
    p.add_argument("--write-schedules", metavar="DIR", help="write every forced schedule as DIR/NAME.txt and exit")
    a = p.parse_args()
    if a.write_schedules:
        for name, labels in sorted(SCHEDULES.items()):
            with open(f"{a.write_schedules}/{name}.txt", "w") as f:
                f.write("\n".join(labels) + "\n")
        return
    base = [int(x) for x in a.base.split(",")] if a.base else None
    nbase = len(base) if base else a.voters
    fmax = a.fmax if a.fmax is not None else (nbase - 1) // 2
    cfg = Config(a.mode, a.voters, fmax, a.maxos, a.maxp, a.maxt, a.compat, restart=a.restart,
                 maxr=a.maxr if a.restart else 0, torn=a.torn, retry=a.retry, base=base,
                 script=parse_script(a.script), count=a.count, barrier=a.barrier)
    print(f"# {time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())} {' '.join(sys.argv[1:])}")
    if a.schedule and a.from_step is not None:
        _, s = replay(cfg, SCHEDULES[a.schedule][:a.from_step], quiet=True)
        run(cfg, a.depth, a.limit, start_state=s, start_depth=a.from_step)
    elif a.schedule:
        hit, s = replay(cfg, SCHEDULES[a.schedule])
        if a.final and hit is not None:
            final_state(cfg, s)
    else:
        run(cfg, a.depth, a.limit)


if __name__ == "__main__":
    main()
