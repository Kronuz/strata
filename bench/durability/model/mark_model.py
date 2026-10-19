#!/usr/bin/env python3
"""DM-R24: the bytes that carry forgetfulness across restarts, through every lifecycle event of
the records that hold them. One core's turn log (Track C D11 as durability-modes D3 and D4
revise it) and one group G in an `acks: written` configuration.

Built on the architect's probe from the re-check of revision 2 (strata-wt/review-durability/
.scratch/review-durability/r2/mark_mechanism.py), whose state, recovery, and checks it keeps,
and to which it adds what that probe left out: segments, reclamation of whole segments (which
can take the newest epoch's open record, `turn_log.h:376-388` at journal cb66f3d), the open
record's lifetime rules, a separate step between the open record's sync and the first turn, and
a precision check beside the safety one.

The bytes, as revision 3 states them (D3, D4):
  - unclean: the newest epoch's boot identity differs from this boot's, or cannot be read, and
    the newest epoch does not end with a clean-close record (D3, D5);
  - same boot: turns after the highest S are rewritten in place and synced; any torn turn after S
    is a verdict (D3);
  - V, in the new open record: this epoch when this recovery's verdict says so, else the newest
    epoch's V; an open record recovery cannot read counts as naming its own epoch (D3);
  - G recovers forgetful when the verdict says so, when its newest hard state carries the mark,
    or when its newest hard state was written in an epoch older than V; "newest" is the record
    with the highest per-group sequence, never the latest in journal order (D3, DM-R24 rule 1);
  - a recovery that leaves G forgetful writes G's hard state with the mark before anything else
    of the epoch, in the turns right after the open record (DM-R24 rule 2, `firstturn`);
  - the newest epoch's open record stays readable for the epoch's life: its fields (epoch, boot
    identity, V) are a live record of the journal, copied forward by reclamation's slices like a
    group's, and its segment leaves only once a copy in a later segment is synced (DM-R24 rule 3,
    `--open-life copy`).

Variants of the hard state's epoch (--variant):
  firstturn   revision 3's rule: rule (c) reads the epoch of the turn holding the surviving copy,
              and a recovery that leaves G forgetful writes its marked hard state in the epoch's
              first turn, before any other turn;
  literal     revision 2's rule as written: no first-turn mark (the architect's 6-step loss);
  orig        records carry the epoch they were first written in, through copy-forward;
  firstturn_jorder  firstturn with "newest" by journal order (a control: it loses).
The open record's lifetime (--open-life):
  copy        revision 3's rule above;
  none        revision 2's journal: the open record is no live record, so its segment can be
              reclaimed within its epoch, and recovery then reads the boot identity as unknown,
              which is unclean (D3): safe, but a process crash then reads as unclean;
  same        the reading D3 forbids: an unreadable boot identity taken as this boot's.

Checks after every recovery:
  safety      G is never left not forgetful while it lacks an entry it acknowledged (it would
              vote, and could elect a log without a `memory` write: the `restart3` shape);
  precision   after a process crash or a clean close (restart or reboot), G is forgetful exactly
              when it was before the stop (T7's claim; a long epoch must not read as unclean).

Ghost state: `acked` (entries G acknowledged at Written) and `holds` (entries in G's log).
Usage: python3 -I mark_model.py [--depth N] [--variant V] [--open-life L] [--switches K]
"""
import argparse
import sys
import time
from collections import namedtuple

# A record: per-group sequence (0 for an open copy), kind ('B' batch with hard state, 'C' group
# checkpoint with hard state, 'O' a copy of an open record's fields), the entries it adds (B) or
# holds (C) or, for 'O', (epoch, boot, V), the mark, and the epoch it was first written in.
Rec = namedtuple("Rec", "seq kind entries mark born")
# A turn: its number, records, S (the last turn a returned barrier covered when it was written),
# synced, torn, and its segment.
Turn = namedtuple("Turn", "no recs S synced torn seg")
# An epoch: number, open record (boot, V), its state (written, synced, alive: not reclaimed), its
# segment, turns, and whether a clean-close record ends it.
Epoch = namedtuple("Epoch", "no boot V open_written open_synced open_alive open_seg turns clean")
St = namedtuple("St", "boot epochs forg holds acked seq nextturn nextval pending_open phase curseg switches")

MAXVAL = 2       # client entries v0, v1 (v0 acknowledged and synced at the start)
COPY = True      # --no-copy disables G's copy-forward
SWITCHES = 2     # segment switches a run may take (opens always start a segment)
LIMIT = 6_000_000
VARIANT = "firstturn"
OPEN_LIFE = "copy"


def kept_turns(ep):
    """The turns of an epoch that recovery keeps: the longest whole prefix of those present."""
    out = []
    for t in ep.turns:
        if t.torn:
            break
        out.append(t)
    return out


def live_records(st):
    """G's records as recovery reads them: one copy per sequence (the oldest readable copy's turn,
    the reading least favorable to finding a hole), superseded by the newest checkpoint record
    (track-c.md:518-530). Returns {seq: (rec, epoch_of_turn, turn_no)}."""
    seen = {}
    for ep in st.epochs:
        for t in kept_turns(ep):
            for r in t.recs:
                if r.kind != "O" and r.seq not in seen:
                    seen[r.seq] = (r, ep.no, t.no)
    ck = [s for s, (r, _, _) in seen.items() if r.kind == "C"]
    if ck:
        base = max(ck)
        seen = {s: v for s, v in seen.items() if s >= base}
    return seen


def holds_of(seen):
    h = set()
    for s in sorted(seen):
        r = seen[s][0]
        if r.kind == "C":
            h = set(r.entries)
        else:
            h |= set(r.entries)
    return frozenset(h)


def open_fields(ep, new_boot):
    """The epoch's open fields (boot, V) as recovery can read them, or None: the original while
    its segment is alive and it reads back (synced, or in this boot's page cache), else the
    newest whole copy in the epoch's kept turns."""
    if ep.open_alive and ep.open_written and (ep.open_synced or not new_boot):
        return ep.boot, ep.V
    for t in reversed(kept_turns(ep)):
        for r in t.recs:
            if r.kind == "O" and r.entries[0] == ep.no:
                return r.entries[1], r.entries[2]
    return None


def recover(st, new_boot):
    """Runs recovery after a stop; returns the new state (phase 'open') and a note."""
    boot = st.boot + 1 if new_boot else st.boot
    epochs = list(st.epochs)
    newest = None
    for ep in reversed(epochs):
        fields = open_fields(ep, new_boot)
        has_turn = any(not t.torn for t in ep.turns)
        if fields is not None or has_turn:
            newest = ep
            break
    assert newest is not None
    inferred = fields is None
    if inferred:
        open_boot = boot if OPEN_LIFE == "same" else None  # `same` is the reading D3 forbids
        prevV = newest.no
    else:
        open_boot, prevV = fields
    clean = newest.clean
    unclean = (open_boot != boot) and not clean
    verdict = unclean
    torn_after_S = False
    if not unclean and not new_boot:
        S = max([t.S for t in kept_turns(newest)], default=0)
        torn_after_S = any(t.torn for t in newest.turns if t.no > S)
        verdict = verdict or torn_after_S
    E = max(e.no for e in epochs) + 1
    V = E if verdict else prevV
    # Same boot: rewrite and sync every kept turn in place; a torn turn's bytes stay where they
    # are (track-c.md:510-516), so a recovery that stops before its open record is durable leaves
    # the next one the same evidence. Epochs after the newest readable one are ignored.
    new_epochs = []
    for ep in epochs:
        if ep.no > newest.no:
            continue
        k = len(kept_turns(ep))
        turns = tuple(t._replace(synced=True) if i < k else t for i, t in enumerate(ep.turns))
        new_epochs.append(ep._replace(turns=turns, open_synced=ep.open_written))
    st2 = st._replace(epochs=tuple(new_epochs))
    seen = live_records(st2)
    holds = holds_of(seen)
    hs = max(seen) if seen else None
    if VARIANT == "firstturn_jorder" and seen:
        hs = max(seen, key=lambda q: (seen[q][2], q))  # the latest turn holding one of G's records
    rec, at = (seen[hs][0], seen[hs][1]) if hs is not None else (None, 0)
    born = (rec.born if VARIANT == "orig" else at) if rec else 0
    forg = verdict or (rec is not None and rec.mark == 1) or (rec is not None and born < V)
    seq = (max(seen) + 1) if seen else 1
    note = (f"recover epoch {E} ({'reboot' if new_boot else 'same boot'}): open {'inferred' if inferred else 'read'} "
            f"unclean={unclean} torn_after_S={torn_after_S} V={V} newest hs seq={hs} mark={rec.mark if rec else None} "
            f"epoch-seen={born} -> forgetful={forg}")
    st3 = st2._replace(boot=boot, forg=forg, holds=holds, seq=seq, pending_open=(E, boot, V), phase="open")
    return st3, note


def write_turn(st, recs):
    """Appends one turn (written to the page cache, unsynced) to the current epoch's segment."""
    eps = list(st.epochs)
    cur = eps[-1]
    S = max([t.no for ep in eps for t in ep.turns if t.synced], default=0)
    t = Turn(st.nextturn, tuple(recs), S, False, False, st.curseg)
    eps[-1] = cur._replace(turns=cur.turns + (t,), clean=False)
    return st._replace(epochs=tuple(eps), nextturn=st.nextturn + 1)


def live_open(st):
    """Where the newest epoch's live open fields are (open-life copy): ('orig', seg) or
    ('copy', turn_no, seg) for the newest synced copy, else the original."""
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
    """Syncs every written turn; effects at the barrier (D3, DM-R6): a copy retires its original,
    a checkpoint record supersedes G's earlier records, an open copy retires the original and
    older copies, and a segment holding no live record leaves, oldest first, never the current
    one (turn_log.h:376-388)."""
    eps = [ep._replace(turns=tuple(t if t.torn else t._replace(synced=True) for t in ep.turns)) for ep in st.epochs]
    st = st._replace(epochs=tuple(eps))
    newest_turn, ck = {}, 0
    for ep in eps:
        for t in ep.turns:
            for r in t.recs:
                if r.kind != "O":
                    newest_turn[r.seq] = t.no
                    if r.kind == "C":
                        ck = max(ck, r.seq)
    lo = live_open(st) if OPEN_LIFE == "copy" else None
    out = []
    for ep in eps:
        turns = []
        for t in ep.turns:
            keep = []
            for r in t.recs:
                if r.kind == "O":
                    if lo and lo[0] == "copy" and lo[1] == t.no:
                        keep.append(r)
                elif newest_turn[r.seq] == t.no and r.seq >= ck:
                    keep.append(r)
            turns.append(t._replace(recs=tuple(keep)))
        out.append(ep._replace(turns=tuple(turns)))
    # The original's bytes stay readable until its segment is reclaimed (open_alive tracks the
    # bytes); once a synced copy exists, only the copy pins its segment (live_open).
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
    """Removes the oldest segments while they hold no live record and are not current."""
    lo = live_open(st) if OPEN_LIFE == "copy" else None
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
                continue  # every byte of an older epoch is gone
            eps.append(ep._replace(open_alive=alive, turns=turns))
        st = st._replace(epochs=tuple(eps))


def successors(st):
    """Yields (label, state, check), check is None or ('recover', stop kind, forgetful before)."""
    out = []
    if st.phase == "open":
        E, boot, V = st.pending_open
        seg = st.curseg + 1  # each open starts a new segment (track-c.md:470-474)
        ep = Epoch(E, boot, V, True, False, True, seg, (), False)
        out.append(("open-write", st._replace(epochs=st.epochs + (ep,), phase="opened", curseg=seg), None))
        out += stops(st, "before open-write: ")
        return out
    if st.phase == "opened":
        eps = list(st.epochs)
        eps[-1] = eps[-1]._replace(open_synced=True)
        nxt = "marking" if VARIANT in ("firstturn", "firstturn_jorder") and st.forg else "run"
        out.append(("open-sync", st._replace(epochs=tuple(eps), phase=nxt), None))
        out += stops(st, "before open-sync: ")
        return out
    if st.phase == "marking":
        # D4, DM-R24 rule 2: the marked hard state is the epoch's first turn; nothing else first.
        cur = st.epochs[-1].no
        s1 = write_turn(st, [Rec(st.seq, "B", (), 1, cur)])._replace(seq=st.seq + 1, phase="run")
        out.append(("first turn marks G", s1, None))
        out += stops(st, "before the first turn: ")
        return out
    cur = st.epochs[-1].no
    if not st.forg and st.nextval < MAXVAL:
        v = st.nextval
        s1 = write_turn(st, [Rec(st.seq, "B", (v,), 0, cur)])
        s1 = s1._replace(seq=st.seq + 1, holds=st.holds | {v}, acked=st.acked | {v}, nextval=v + 1)
        out.append((f"write v{v} (acked at Written)", s1, None))
    if st.forg:
        # Re-learning: what G lacks with the mark still set, then the batch that completes it
        # and clears the mark, after the re-learned entries (D4, D5).
        missing = tuple(sorted(st.acked - st.holds))
        s1 = write_turn(st, [Rec(st.seq, "B", missing, 1, cur)])
        out.append(("re-learn, part (mark set)", s1._replace(seq=st.seq + 1, holds=st.holds | set(missing)), None))
        if st.acked <= st.holds:
            s2 = write_turn(st, [Rec(st.seq, "B", (), 0, cur)])
            out.append(("re-learn completes (mark cleared)", s2._replace(seq=st.seq + 1, forg=False), None))
        s3 = write_turn(st, [Rec(st.seq, "B", (), 1, cur)])
        out.append(("hard state (mark set)", s3._replace(seq=st.seq + 1), None))
    if any(not t.synced for ep in st.epochs for t in ep.turns):
        out.append(("barrier", barrier(st), None))
    # Copy-forward: G's live records in a non-current segment rewritten into the head (any such
    # segment, a superset of the turn log's oldest-first slices).
    if COPY:
        seen = live_records(st)
        segs = sorted({t.seg for ep in st.epochs for t in kept_turns(ep) for r in t.recs
                       if r.kind != "O" and r.seq in seen and seen[r.seq][2] == t.no and t.seg != st.curseg})
        for sg in segs:
            recs = [seen[q][0] for q in sorted(seen) if any(t.no == seen[q][2] and t.seg == sg
                                                           for ep in st.epochs for t in ep.turns)]
            if recs:
                out.append((f"copy-forward G's seqs {[r.seq for r in recs]} from segment {sg}", write_turn(st, recs), None))
    # The open record's fields copied forward (open-life copy), when they live in a non-current segment.
    if OPEN_LIFE == "copy":
        lo = live_open(st)
        if lo and lo[-1] != st.curseg:
            ep = st.epochs[-1]
            s1 = write_turn(st, [Rec(0, "O", (ep.no, ep.boot, ep.V), 0, cur)])
            out.append((f"copy-forward the open record from segment {lo[-1]}", s1, None))
    # A segment switch: the next turn starts a new segment.
    if st.switches < SWITCHES:
        out.append((f"segment switch to {st.curseg + 1}", st._replace(curseg=st.curseg + 1, switches=st.switches + 1), None))
    # A forced checkpoint of G (its hard state captured with the mark as it stands).
    if all(t.synced for ep in st.epochs for t in ep.turns):
        s1 = write_turn(st, [Rec(st.seq, "C", tuple(sorted(st.holds)), int(st.forg), cur)])
        out.append(("checkpoint G", s1._replace(seq=st.seq + 1), None))
    out += stops(st)
    return out


def tear(st, keep_whole):
    """Marks the unsynced turns after the first keep_whole as torn, their records unreadable."""
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
    if st.phase == "run":
        s4 = barrier(st)
        eps = list(s4.epochs)
        eps[-1] = eps[-1]._replace(clean=True)
        s4 = s4._replace(epochs=tuple(eps))
        for nb in (False, True):
            s5, note = recover(s4, new_boot=nb)
            out.append((prefix + f"clean close, {'reboot' if nb else 'restart'}; " + note, s5, ("recover", "clean", before)))
    return out


def initial():
    # Epoch 1, boot 1, segment 1: G's founding batch with v0, synced; v0 acknowledged.
    t1 = Turn(1, (Rec(1, "B", (0,), 0, 1),), 0, True, False, 1)
    ep = Epoch(1, 1, 0, True, True, True, 1, (t1,), False)
    return St(boot=1, epochs=(ep,), forg=False, holds=frozenset({0}), acked=frozenset({0}), seq=2, nextturn=2,
              nextval=1, pending_open=None, phase="run", curseg=1, switches=0)


def violation(check, t):
    _, kind, before = check
    if not t.acked <= t.holds and not t.forg:
        return "safety"
    if kind in ("process", "clean") and t.forg != before:
        return "precision"
    return None


def search(depth, key64=False):
    """Breadth-first to `depth`. With key64 the visited set keeps 64-bit hashes of the states and
    interned labels instead of the states, omitting a state with probability about n^2 / 2^65."""
    start = time.time()
    init = initial()
    key = hash if key64 else (lambda st: st)
    labels, label_id = [], {}
    parent = {key(init): None}
    level = [init]
    found = {}
    states = 1
    d = 0
    for d in range(1, depth + 1):
        if states > LIMIT:
            print(f"  stopped at the state limit ({LIMIT:,}) before depth {d}")
            d -= 1
            break
        nxt = []
        for s in level:
            ks = key(s)
            for label, t, check in successors(s):
                if check:
                    v = violation(check, t)
                    if v and v not in found:
                        found[v] = (ks, label, t)
                kt = key(t)
                if kt not in parent:
                    li = label_id.setdefault(label, len(labels))
                    if li == len(labels):
                        labels.append(label)
                    parent[kt] = (ks, li)
                    nxt.append(t)
                    states += 1
        level = nxt
        if "safety" in found:
            break
    print(f"variant={VARIANT} open-life={OPEN_LIFE} copy-forward={'on' if COPY else 'off'} switches={SWITCHES} "
          f"entries={MAXVAL} depth={d}: {states:,} states ({time.time() - start:.0f} s)" + ("; 64-bit keys" if key64 else ""))
    for kind in ("safety", "precision"):
        if kind not in found:
            print(f"  {kind}: holds")
            continue
        ks, label, t = found[kind]
        trace = [label]
        while parent[ks] is not None:
            ks, li = parent[ks]
            trace.append(labels[li])
        trace.reverse()
        what = (f"G recovered not forgetful while it lacks {sorted(t.acked - t.holds)}" if kind == "safety"
                else f"G recovered forgetful={t.forg} after a stop that loses nothing")
        print(f"  {kind}: VIOLATION, {what}; {len(trace)} steps:")
        for i, step in enumerate(trace, 1):
            print(f"    {i:2}. {step}")
    sys.stdout.flush()
    return found


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--depth", type=int, default=11)
    p.add_argument("--variant", default="firstturn", choices=("firstturn", "literal", "orig", "firstturn_jorder"))
    p.add_argument("--open-life", default="copy", choices=("copy", "none", "same"))
    p.add_argument("--no-copy", action="store_true", help="disable G's copy-forward")
    p.add_argument("--switches", type=int, default=2)
    p.add_argument("--maxval", type=int, default=2)
    p.add_argument("--limit", type=int, default=6_000_000)
    p.add_argument("--key64", action="store_true", help="keep 64-bit state hashes, not the states")
    a = p.parse_args()
    global COPY, MAXVAL, LIMIT, SWITCHES, VARIANT, OPEN_LIFE
    COPY, MAXVAL, LIMIT, SWITCHES, VARIANT, OPEN_LIFE = not a.no_copy, a.maxval, a.limit, a.switches, a.variant, a.open_life
    print(f"# {time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())} {' '.join(sys.argv[1:])}")
    search(a.depth, a.key64)


if __name__ == "__main__":
    main()
