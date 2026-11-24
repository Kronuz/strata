#!/usr/bin/env python3
"""F13 of strata docs/design/rocksdb-parity.md: sizing turns on D1's segment ladder, with journal's
arithmetic at 1222e3b: record_cost, turn_bound and turn_room (turn_log.h:106-121), seal_turn's
whole and half rooms (turn_driver.h:550-575) and encode's fresh segment (turn_log.h:475-482),
which throws "turn exceeds a segment" when a turn does not fit one: the driver fails and every
index on the log fences.

A log on the ladder: rungs from the smallest segment whose half room admits one framed largest
chunk, doubling to segment_bytes (64 MiB, 32 KiB blocks). K writers of chunk-sized records (1 MiB,
kv's command_bytes) join through D7's per-log counter in arrival order; one turn per barrier, which
seals the admitted records, the first against the whole room and later ones against half.
Variants:
  fixed     every room from options.segment_bytes (journal today); segments from the ladder,
            which grows only past retain_segments live segments (D1 item 1 as written)
  follows   every room (whole, half, whole_room, D7's counter) from the segment the next turn
            opens, the prepared spare; a turn that starts with a join waiting prepares the spare
            one rung up, used from the next turn
  smallest  every room from the smallest rung
Then a step down when a checkpoint empties the log while records are admitted:
  naive     the spare drops to the smallest rung at once
  guarded   the spare never drops below the rung whose half room holds the admitted bytes, and
            only after a turn with no join waiting
Run: python3 -I d1_ladder_model.py
"""

from __future__ import annotations

import sys
from collections import deque

MiB = 1 << 20
BLOCK = 32 << 10
HEADER = 25
BLOCK_COST = 126
TOP = 64 * MiB
CHUNK = 1 * MiB
RETAIN = 4
BARRIER_S = 0.021


def record_cost(payload: int) -> int:
    return payload + HEADER + 40


def turn_bound(cost: int, b: int = BLOCK) -> int:
    fixed = cost + record_cost(4) + BLOCK_COST + b // 2
    return fixed + (fixed * BLOCK_COST + (b - BLOCK_COST) - 1) // (b - BLOCK_COST)


def turn_room(limit: int, b: int = BLOCK) -> int:
    scaled = limit // b * (b - BLOCK_COST) + limit % b * (b - BLOCK_COST) // b
    fixed = record_cost(4) + BLOCK_COST + b // 2
    return scaled - fixed if scaled > fixed else 0


def smallest_rung(chunk: int) -> int:
    s = BLOCK
    while turn_room(s // 2) < record_cost(chunk):
        s *= 2
    return s


S_MIN = smallest_rung(CHUNK)


def rung_for(cost: int) -> int:
    s = S_MIN
    while s < TOP and turn_room(s // 2) < cost:
        s *= 2
    return s


def simulate(variant: str, k: int, rate, turns: int, checkpoint_every: int = 0, step: str = "guarded") -> dict:
    """rate(t): tenths of a chunk each writer produces per turn, None for saturating writers (always
    one join waiting); writers start out of phase. Each turn seals what was admitted, writes, and during its barrier writers join and
    are admitted against the counter's room; after the barrier a checkpoint may empty the log and
    the ladder picks the next spare. Returns throws, waits and throughput."""
    cost = record_cost(CHUNK)
    seg, offset, spare = S_MIN, 0, S_MIN
    live_segments = 1
    waiting: deque = deque()
    admitted: deque = deque()
    unsealed = 0
    credit = [w * 10 // k for w in range(k)]  # tenths of a chunk
    pending = [False] * k
    out = {"throw": None, "adm_to_seal": 0, "arr_to_seal": 0, "arr_all": 0, "top_rung": S_MIN, "climb": 0, "sealed": [], "rung": []}

    def room_of() -> int:
        return TOP if variant == "fixed" else S_MIN if variant == "smallest" else spare

    def admit(t: int, r) -> None:
        nonlocal unsealed
        half = turn_room(room_of() // 2)
        progress = True
        while progress:
            progress = False
            for w in range(k):
                if not pending[w] and credit[w] >= 10:
                    credit[w] -= 10
                    pending[w] = True
                    waiting.append((w, t))
                    progress = True
            while waiting and unsealed + cost <= half:  # D7: the log's unsealed cost within one half room
                w, born = waiting.popleft()
                pending[w] = False
                admitted.append((born, t))
                unsealed += cost
                if r is None:
                    credit[w] = 10
                progress = True

    for t in range(turns):
        r = rate(t)
        admit(t - 1, r)  # joins left waiting in the last barrier take the room once it exists
        whole, half = turn_room(room_of()), turn_room(room_of() // 2)
        used, first, n = 0, True, 0
        while admitted:
            if cost > (whole if first else half) - used:
                break
            born, adm = admitted.popleft()
            out["adm_to_seal"] = max(out["adm_to_seal"], t - adm)
            out["arr_all"] = max(out["arr_all"], t - born)
            if t >= 20:
                out["arr_to_seal"] = max(out["arr_to_seal"], t - born)
            used, unsealed, first, n = used + cost, unsealed - cost, False, n + 1
        out["sealed"].append(n)
        if n:
            bt = turn_bound(used)
            if offset + bt <= seg:
                offset += bt
            else:
                nxt = spare
                if variant == "fixed":  # D1 item 1 as written: double past retain_segments live
                    nxt = min(2 * seg, TOP) if live_segments > RETAIN else seg
                if bt > nxt:
                    out["throw"] = t
                    break
                seg, offset, live_segments = nxt, bt, live_segments + 1
        for w in range(k):  # the barrier: writers produce and join
            credit[w] = 10 if r is None else credit[w] + r
        admit(t, r)
        demand = bool(waiting)
        emptied = checkpoint_every and t % checkpoint_every == checkpoint_every - 1
        if emptied:
            live_segments = 1
        if variant == "follows":
            if demand:
                spare = min(2 * spare, TOP)
            elif emptied and step == "naive":
                spare = S_MIN
            elif emptied and spare > S_MIN:
                spare = max(rung_for(unsealed), spare // 2)
        if (spare if variant == "follows" else seg) > out["top_rung"]:
            out["top_rung"], out["climb"] = spare if variant == "follows" else seg, t + 1
        out["rung"].append(spare if variant == "follows" else seg)
    return out


def mib_s(chunks_per_turn: float) -> float:
    return chunks_per_turn * CHUNK / BARRIER_S / MiB


def main() -> int:
    ok = True
    print(
        f"rungs: smallest {S_MIN // MiB} MiB for {CHUNK // MiB} MiB chunks at {BLOCK >> 10} KiB blocks, top {TOP // MiB} MiB; "
        f"half rooms {', '.join(f'{s // MiB}: {turn_room(s // 2) / record_cost(CHUNK):.2f}' for s in (4 * MiB, 8 * MiB, 16 * MiB, 32 * MiB, 64 * MiB))} chunks"
    )
    loads = (("K=1 at 0.6 chunks/turn (wal: 30 MB/s, 21 ms)", 1, lambda t: 6), ("K=8 at 0.6 each", 8, lambda t: 6), ("K=8 saturating", 8, lambda t: None))
    print("1. from an empty log, 300 turns: first throw, turns from admission to seal, from arrival to seal (after turn 20, and in all),")
    print("   the top rung and the turn it was reached, chunks sealed per turn over the last 100 and their rate at 21 ms barriers")
    for label, k, rate in loads:
        for variant in ("fixed", "follows", "smallest"):
            r = simulate(variant, k, rate, 300)
            tail = r["sealed"][-100:]
            per_turn = sum(tail) / len(tail) if tail and r["throw"] is None else 0.0
            print(
                f"   {label:46s} {variant:9s} throw at turn {r['throw']!s:5s} admission->seal {r['adm_to_seal']:2d} "
                f"arrival->seal {r['arr_to_seal']:2d} ({r['arr_all']:3d} in all) top rung {r['top_rung'] // MiB:2d} MiB at turn {r['climb']:2d}  "
                f"{per_turn:5.2f} chunks/turn ({mib_s(per_turn):5.0f} MiB/s)"
            )
            if variant == "follows":
                ok &= r["throw"] is None and r["adm_to_seal"] <= 1
            if variant == "fixed" and k == 8:
                ok &= r["throw"] is not None
    print("   (admission->seal 1: every admitted record seals in the turn after the barrier that admitted it)")
    print("2. step down: K=8 at 0.6 each for 150 turns, idle 100, then again for 150; a checkpoint empties the log every 10 turns")
    phased = lambda t: 6 if t < 150 or t >= 250 else 0
    for step in ("naive", "guarded"):
        r = simulate("follows", 8, phased, 400, checkpoint_every=10, step=step)
        rest = r["rung"][245] // MiB
        print(
            f"   {step:8s} throw {r['throw']!s:5s} admission->seal {r['adm_to_seal']:3d} arrival->seal {r['arr_to_seal']:3d} "
            f"rung at rest {rest} MiB, at load {r['rung'][140] // MiB} MiB"
        )
        if step == "guarded":
            ok &= r["throw"] is None and r["adm_to_seal"] <= 1 and r["rung"][245] == S_MIN
        else:
            ok &= r["adm_to_seal"] > 1
    print("all claims hold" if ok else "unexpected")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
