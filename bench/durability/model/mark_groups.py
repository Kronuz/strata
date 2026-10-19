#!/usr/bin/env python3
"""DM-R24 rule 2 with more than one group (DM-R29). The architect's probe from the re-check of
revision 3 (strata-wt/review-durability/.scratch/review-durability/r3/mark_groups.py), ported
unchanged but for --switch-marking (segment switches between mark turns) and its header line.

DM-R24 rule 2 with more than one group: the first-turn marks of several forgetful groups,
spread over the epoch's first turns, with reclamation's copies riding in those turns.

The designer's bench/durability/model/mark_model.py at b56cb46 models one group, so every
first turn holds that group's mark and nothing else. D3 rule 2 (durability-modes.md:566-576)
writes the marks "in the epoch's first turns" (plural) and lets "reclamation's copies ... ride
in the same turns ... since a copy keeps its old sequence and survives or tears with the marks
beside it". That holds for a copy of a group whose mark is in the same turn or an earlier one.
A copy of a group whose mark is in a later turn survives without it, and rule (c) then reads
the copy's epoch, which is V's: the group recovers not forgetful.

This model keeps mark_model.py's state, recovery, barrier, reclamation, open-record copies,
checks, and the oldest-copy reading of rule (c), per group, and adds:
  --groups K        groups on the core (default 2), each with v0 synced and one write v1;
  --mpt N           marks per first turn (default 1; 0: every mark in one turn);
  --ride R          what reclamation may copy in a mark turn: none, own (only groups whose mark
                    is in this turn or an earlier one), or any (D3's text as written);
  --close-marking   also allow a clean close between mark turns (D3 does not forbid it);
  --switch-marking  also allow a segment switch between mark turns (marks that fill a segment).
Variants: firstturn (revision 3) or orig (copies keep their first epoch; no marks written).
Usage: python3 -I mark_groups.py [--depth N] [--groups K] [--mpt N] [--ride R] [--variant V]
"""
import argparse
import sys
import time
from collections import namedtuple

Rec = namedtuple("Rec", "grp seq kind entries mark born")  # grp -1: an open copy (kind 'O')
Turn = namedtuple("Turn", "no recs S synced torn seg")
Epoch = namedtuple("Epoch", "no boot V open_written open_synced open_alive open_seg turns clean")
St = namedtuple("St", "boot epochs forg holds acked seq nextturn nextval pending_open phase curseg switches pending")

G = 2
MAXVAL = 2
MPT = 1
RIDE = "any"
VARIANT = "firstturn"
SWITCHES = 2
CLOSE_MARKING = False
SWITCH_MARKING = False
LIMIT = 8_000_000
MARK_ALWAYS = False
COPY_READING = "oldest"  # which of two readable copies' turn rule (c) reads: mark_model.py's oldest, or newest


def kept_turns(ep):
    out = []
    for t in ep.turns:
        if t.torn:
            break
        out.append(t)
    return out


def live_records(st, g):
    """Group g's records as recovery reads them: one copy per sequence (the oldest readable,
    mark_model.py's reading), superseded by the newest checkpoint record."""
    seen = {}
    for ep in st.epochs:
        for t in kept_turns(ep):
            for r in t.recs:
                if r.grp == g and (r.seq not in seen or COPY_READING == "newest"):
                    seen[r.seq] = (r, ep.no, t.no)  # --copy-reading newest: the latest copy's turn
    ck = [s for s, (r, _, _) in seen.items() if r.kind == "C"]
    if ck:
        base = max(ck)
        seen = {s: v for s, v in seen.items() if s >= base}
    return seen


def holds_of(seen):
    h = set()
    for s in sorted(seen):
        r = seen[s][0]
        h = set(r.entries) if r.kind == "C" else h | set(r.entries)
    return frozenset(h)


def open_fields(ep, new_boot):
    if ep.open_alive and ep.open_written and (ep.open_synced or not new_boot):
        return ep.boot, ep.V
    for t in reversed(kept_turns(ep)):
        for r in t.recs:
            if r.kind == "O" and r.entries[0] == ep.no:
                return r.entries[1], r.entries[2]
    return None


def tset(tup, i, v):
    lst = list(tup)
    lst[i] = v
    return tuple(lst)


def recover(st, new_boot):
    boot = st.boot + 1 if new_boot else st.boot
    epochs = list(st.epochs)
    newest, fields = None, None
    for ep in reversed(epochs):
        fields = open_fields(ep, new_boot)
        if fields is not None or any(not t.torn for t in ep.turns):
            newest = ep
            break
    assert newest is not None
    inferred = fields is None
    open_boot, prevV = (None, newest.no) if inferred else fields
    unclean = (open_boot != boot) and not newest.clean
    verdict = unclean
    torn_after_S = False
    if not unclean and not new_boot:
        S = max([t.S for t in kept_turns(newest)], default=0)
        torn_after_S = any(t.torn for t in newest.turns if t.no > S)
        verdict = verdict or torn_after_S
    E = max(e.no for e in epochs) + 1
    V = E if verdict else prevV
    new_epochs = []
    for ep in epochs:
        if ep.no > newest.no:
            continue
        k = len(kept_turns(ep))
        turns = tuple(t._replace(synced=True) if i < k else t for i, t in enumerate(ep.turns))
        new_epochs.append(ep._replace(turns=turns, open_synced=ep.open_written))
    st2 = st._replace(epochs=tuple(new_epochs))
    forg, holds, seq, pending, notes = [], [], [], [], []
    for g in range(G):
        seen = live_records(st2, g)
        hs = max(seen) if seen else None
        rec, at = (seen[hs][0], seen[hs][1]) if hs is not None else (None, 0)
        born = (rec.born if VARIANT == "orig" else at) if rec else 0
        f = verdict or (rec is not None and rec.mark == 1) or (rec is not None and born < V)
        forg.append(f)
        holds.append(holds_of(seen))
        seq.append((max(seen) + 1) if seen else 1)
        if VARIANT == "firstturn" and f and (MARK_ALWAYS or rec is None or rec.mark == 0):
            pending.append(g)  # D3 rule 2; mark_model.py marks every forgetful group (--mark-always)
        notes.append(f"g{g}: newest seq {hs} mark {rec.mark if rec else None} epoch {born} -> {'F' if f else 'not F'}")
    note = (f"recover epoch {E} ({'reboot' if new_boot else 'same boot'}): open {'inferred' if inferred else 'read'} "
            f"verdict={verdict} V={V}; " + "; ".join(notes))
    st3 = st2._replace(boot=boot, forg=tuple(forg), holds=tuple(holds), seq=tuple(seq), pending_open=(E, boot, V),
                       phase="open", pending=tuple(pending))
    return st3, note


def write_turn(st, recs):
    eps = list(st.epochs)
    cur = eps[-1]
    S = max([t.no for ep in eps for t in ep.turns if t.synced], default=0)
    t = Turn(st.nextturn, tuple(recs), S, False, False, st.curseg)
    eps[-1] = cur._replace(turns=cur.turns + (t,), clean=False)
    return st._replace(epochs=tuple(eps), nextturn=st.nextturn + 1)


def live_open(st):
    ep = st.epochs[-1]
    best = None
    for t in ep.turns:
        if t.synced and not t.torn:
            for r in t.recs:
                if r.kind == "O" and r.entries[0] == ep.no:
                    best = ("copy", t.no, t.seg)
    if best:
        return best
    return ("orig", ep.open_seg) if ep.open_alive else None


def barrier(st):
    eps = [ep._replace(turns=tuple(t if t.torn else t._replace(synced=True) for t in ep.turns)) for ep in st.epochs]
    st = st._replace(epochs=tuple(eps))
    newest_turn, ck = {}, {}
    for ep in eps:
        for t in ep.turns:
            for r in t.recs:
                if r.kind != "O":
                    newest_turn[(r.grp, r.seq)] = t.no
                    if r.kind == "C":
                        ck[r.grp] = max(ck.get(r.grp, 0), r.seq)
    lo = live_open(st)
    out = []
    for ep in eps:
        turns = []
        for t in ep.turns:
            keep = []
            for r in t.recs:
                if r.kind == "O":
                    if lo and lo[0] == "copy" and lo[1] == t.no:
                        keep.append(r)
                elif newest_turn[(r.grp, r.seq)] == t.no and r.seq >= ck.get(r.grp, 0):
                    keep.append(r)
            turns.append(t._replace(recs=tuple(keep)))
        out.append(ep._replace(turns=tuple(turns)))
    return reclaim(st._replace(epochs=tuple(out)))


def segments_in(st):
    segs = set()
    for ep in st.epochs:
        if ep.open_alive:
            segs.add(ep.open_seg)
        for t in ep.turns:
            segs.add(t.seg)
    return segs


def reclaim(st):
    lo = live_open(st)
    while True:
        segs = segments_in(st)
        if not segs:
            return st
        oldest = min(segs)
        if oldest == st.curseg:
            return st
        live = any(r.kind != "O" for ep in st.epochs for t in ep.turns if t.seg == oldest for r in t.recs)
        if lo and lo[-1] == oldest:
            live = True
        if live:
            return st
        eps = []
        for ep in st.epochs:
            alive = ep.open_alive and ep.open_seg != oldest
            turns = tuple(t for t in ep.turns if t.seg != oldest)
            if not alive and not turns and ep.no != st.epochs[-1].no:
                continue
            eps.append(ep._replace(open_alive=alive, turns=turns))
        st = st._replace(epochs=tuple(eps))


def copy_sets(st, allowed):
    """Reclamation's slices: per non-current segment, the live records (of the allowed groups)
    whose newest kept copy is there."""
    out = []
    per = {g: live_records(st, g) for g in range(G)}
    segs = sorted({t.seg for ep in st.epochs for t in kept_turns(ep) for r in t.recs
                   if r.kind != "O" and r.seq in per[r.grp] and per[r.grp][r.seq][2] == t.no and t.seg != st.curseg})
    for sg in segs:
        recs = []
        for g in range(G):
            if g not in allowed:
                continue
            seen = per[g]
            for q in sorted(seen):
                if any(t.no == seen[q][2] and t.seg == sg for ep in st.epochs for t in ep.turns):
                    recs.append(seen[q][0])
        if recs:
            out.append((sg, recs))
    return out


def successors(st):
    out = []
    if st.phase == "open":
        E, boot, V = st.pending_open
        seg = st.curseg + 1
        ep = Epoch(E, boot, V, True, False, True, seg, (), False)
        out.append(("open-write", st._replace(epochs=st.epochs + (ep,), phase="opened", curseg=seg), None))
        out += stops(st, "before open-write: ")
        return out
    if st.phase == "opened":
        eps = list(st.epochs)
        eps[-1] = eps[-1]._replace(open_synced=True)
        nxt = "marking" if st.pending else "run"
        out.append(("open-sync", st._replace(epochs=tuple(eps), phase=nxt), None))
        out += stops(st, "before open-sync: ")
        return out
    cur = st.epochs[-1].no
    if st.phase == "marking":
        k = len(st.pending) if MPT == 0 else MPT
        now, rest = st.pending[:k], st.pending[k:]
        seq = st.seq
        marks = []
        for g in now:
            marks.append(Rec(g, seq[g], "B", (), 1, cur))
            seq = tset(seq, g, seq[g] + 1)
        nxt = "marking" if rest else "run"
        label = f"mark turn for g{list(now)}"
        out.append((label, write_turn(st, marks)._replace(seq=seq, pending=rest, phase=nxt), None))
        if RIDE != "none":
            allowed = set(range(G)) if RIDE == "any" else set(range(G)) - set(rest)
            for sg, recs in copy_sets(st, allowed):
                s1 = write_turn(st, marks + recs)._replace(seq=seq, pending=rest, phase=nxt)
                out.append((f"{label} + copies of {[(r.grp, r.seq) for r in recs]} from segment {sg}", s1, None))
        if any(not t.synced and not t.torn for ep in st.epochs for t in ep.turns):
            out.append(("barrier", barrier(st), None))  # between mark turns
        if SWITCH_MARKING and st.switches < SWITCHES:
            out.append((f"segment switch to {st.curseg + 1} (marking)", st._replace(curseg=st.curseg + 1, switches=st.switches + 1), None))
        out += stops(st)
        return out
    for g in range(G):
        if not st.forg[g] and st.nextval[g] < MAXVAL:
            v = st.nextval[g]
            s1 = write_turn(st, [Rec(g, st.seq[g], "B", (v,), 0, cur)])
            s1 = s1._replace(seq=tset(st.seq, g, st.seq[g] + 1), holds=tset(st.holds, g, st.holds[g] | {v}),
                             acked=tset(st.acked, g, st.acked[g] | {v}), nextval=tset(st.nextval, g, v + 1))
            out.append((f"g{g} writes v{v} (acked at Written)", s1, None))
        if st.forg[g]:
            # mark_model.py's three steps: what g lacks with the mark set, the batch that completes
            # re-learning and clears it (after the entries), and a hard state with the mark set.
            missing = tuple(sorted(st.acked[g] - st.holds[g]))
            s1 = write_turn(st, [Rec(g, st.seq[g], "B", missing, 1, cur)])
            out.append((f"g{g} re-learns {list(missing)} (mark set)",
                        s1._replace(seq=tset(st.seq, g, st.seq[g] + 1), holds=tset(st.holds, g, st.holds[g] | set(missing))), None))
            if st.acked[g] <= st.holds[g]:
                s2 = write_turn(st, [Rec(g, st.seq[g], "B", (), 0, cur)])
                out.append((f"g{g} re-learning completes (mark cleared)",
                            s2._replace(seq=tset(st.seq, g, st.seq[g] + 1), forg=tset(st.forg, g, False)), None))
            s3 = write_turn(st, [Rec(g, st.seq[g], "B", (), 1, cur)])
            out.append((f"g{g} hard state (mark set)", s3._replace(seq=tset(st.seq, g, st.seq[g] + 1)), None))
    if any(not t.synced for ep in st.epochs for t in ep.turns):
        out.append(("barrier", barrier(st), None))
    for sg, recs in copy_sets(st, set(range(G))):
        out.append((f"copy-forward {[(r.grp, r.seq) for r in recs]} from segment {sg}", write_turn(st, recs), None))
    lo = live_open(st)
    if lo and lo[-1] != st.curseg:
        ep = st.epochs[-1]
        out.append((f"copy-forward the open record from segment {lo[-1]}",
                    write_turn(st, [Rec(-1, 0, "O", (ep.no, ep.boot, ep.V), 0, cur)]), None))
    if st.switches < SWITCHES:
        out.append((f"segment switch to {st.curseg + 1}", st._replace(curseg=st.curseg + 1, switches=st.switches + 1), None))
    if all(t.synced for ep in st.epochs for t in ep.turns):
        for g in range(G):
            s1 = write_turn(st, [Rec(g, st.seq[g], "C", tuple(sorted(st.holds[g])), int(st.forg[g]), cur)])
            out.append((f"checkpoint g{g}", s1._replace(seq=tset(st.seq, g, st.seq[g] + 1)), None))
    out += stops(st)
    return out


def tear(st, keep_whole):
    eps, idx = [], 0
    for ep in st.epochs:
        turns = []
        for t in ep.turns:
            if not t.synced:
                if idx >= keep_whole:
                    t = t._replace(torn=True, recs=())
                idx += 1
            turns.append(t)
        eps.append(ep._replace(turns=tuple(turns)))
    return st._replace(epochs=tuple(eps))


def stops(st, prefix=""):
    out = []
    unsynced = sum(1 for ep in st.epochs for t in ep.turns if not t.synced)
    before = st.forg
    s1, note = recover(st, new_boot=False)
    out.append((prefix + "process crash; " + note, s1, ("recover", "process", before)))
    if unsynced:
        s2, note = recover(tear(st, 0), new_boot=False)
        out.append((prefix + "failed writeback, restart; " + note, s2, ("recover", "torn", before)))
    for keep in range(unsynced + 1):
        s3, note = recover(tear(st, keep), new_boot=True)
        out.append((prefix + f"power loss (keeps {keep} unsynced turns); " + note, s3, ("recover", "power", before)))
    if st.phase == "run" or (st.phase == "marking" and CLOSE_MARKING):
        s4 = barrier(st)
        eps = list(s4.epochs)
        eps[-1] = eps[-1]._replace(clean=True)
        s4 = s4._replace(epochs=tuple(eps))
        for nb in (False, True):
            s5, note = recover(s4, new_boot=nb)
            out.append((prefix + f"clean close, {'reboot' if nb else 'restart'}; " + note, s5, ("recover", "clean", before)))
    return out


def initial():
    t1 = Turn(1, tuple(Rec(g, 1, "B", (0,), 0, 1) for g in range(G)), 0, True, False, 1)
    ep = Epoch(1, 1, 0, True, True, True, 1, (t1,), False)
    z = tuple(frozenset({0}) for _ in range(G))
    return St(boot=1, epochs=(ep,), forg=(False,) * G, holds=z, acked=z, seq=(2,) * G, nextturn=2,
              nextval=(1,) * G, pending_open=None, phase="run", curseg=1, switches=0, pending=())


def violation(check, t):
    _, kind, before = check
    for g in range(G):
        if not t.acked[g] <= t.holds[g] and not t.forg[g]:
            return "safety"
    if kind in ("process", "clean") and t.forg != before:
        return "precision"
    return None


def search(depth):
    start = time.time()
    init = initial()
    parent = {init: None}
    level = [init]
    found = {}
    d = 0
    for d in range(1, depth + 1):
        if len(parent) > LIMIT:
            print(f"  stopped at the state limit ({LIMIT:,}) before depth {d}")
            d -= 1
            break
        nxt = []
        for s in level:
            for label, t, check in successors(s):
                if check:
                    v = violation(check, t)
                    if v and v not in found:
                        found[v] = (s, label, t)
                if t not in parent:
                    parent[t] = (s, label)
                    nxt.append(t)
        level = nxt
        if "safety" in found:
            break
    print(f"variant={VARIANT} groups={G} marks-per-turn={MPT or 'all'} ride={RIDE} close-marking={CLOSE_MARKING} "
          f"copy-reading={COPY_READING} switch-marking={SWITCH_MARKING} "
          f"switches={SWITCHES} entries={MAXVAL} depth={d}: {len(parent):,} states ({time.time() - start:.0f} s)")
    for kind in ("safety", "precision"):
        if kind not in found:
            print(f"  {kind}: holds")
            continue
        s, label, t = found[kind]
        trace = [label]
        while parent[s] is not None:
            s, lab = parent[s]
            trace.append(lab)
        trace.reverse()
        bad = [g for g in range(G) if not t.acked[g] <= t.holds[g] and not t.forg[g]]
        what = (f"group(s) {bad} recovered not forgetful while lacking acknowledged entries" if kind == "safety"
                else "a group's forgetfulness changed across a stop that loses nothing")
        print(f"  {kind}: VIOLATION, {what}; {len(trace)} steps:")
        for i, step in enumerate(trace, 1):
            print(f"    {i:2}. {step}")
    sys.stdout.flush()
    return found


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--depth", type=int, default=9)
    p.add_argument("--groups", type=int, default=2)
    p.add_argument("--mpt", type=int, default=1)
    p.add_argument("--ride", default="any", choices=("none", "own", "any"))
    p.add_argument("--variant", default="firstturn", choices=("firstturn", "orig"))
    p.add_argument("--switches", type=int, default=2)
    p.add_argument("--maxval", type=int, default=2)
    p.add_argument("--close-marking", action="store_true")
    p.add_argument("--switch-marking", action="store_true")
    p.add_argument("--limit", type=int, default=8_000_000)
    p.add_argument("--mark-always", action="store_true", help="mark every forgetful group, as mark_model.py does")
    p.add_argument("--copy-reading", default="oldest", choices=("oldest", "newest"))
    a = p.parse_args()
    global G, MPT, RIDE, VARIANT, SWITCHES, MAXVAL, CLOSE_MARKING, LIMIT, MARK_ALWAYS, COPY_READING, SWITCH_MARKING
    G, MPT, RIDE, VARIANT, SWITCHES, MAXVAL, CLOSE_MARKING, LIMIT, MARK_ALWAYS, COPY_READING, SWITCH_MARKING = (
        a.groups, a.mpt, a.ride, a.variant, a.switches, a.maxval, a.close_marking, a.limit, a.mark_always, a.copy_reading,
        a.switch_marking)
    print(f"# {time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())} {' '.join(sys.argv[1:])}")
    search(a.depth)


if __name__ == "__main__":
    main()
