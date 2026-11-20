"""Model of journal's seal_turn (turn_driver.h:451-508 at journal cc95ba6) with D7's change:
a sealer is called again with the room left until it returns nothing. Units are record costs.

Checks two claims of rocksdb-parity.md D7 (rows 1-2, the design review's model), then the
revised D7 (rows 3-5):
  1. fairness: with one group writing faster than a turn drains, does another group's record
     wait a bounded number of turns? (literal: the call that returns nothing under less than
     the whole room sets resume_at to that same group, line 492)
  2. the ack=none window: with each group's unsealed bytes bounded at one turn's room, is the
     oldest unsealed byte sealed within one turn whatever the number of groups?
  3. the window with the bound per log: one admission counter for every group of the log,
     at one turn's room; writers past it wait in arrival order (FIFO), unadmitted
  4. the same with one bulk group and one ack=sync group: the sync record's wait from its
     arrival (admission included) and from its admission
  5. the fair resume rule with a group bound past the room (N5), bound per log off
Run: python3 -I d7_model.py
"""

from __future__ import annotations

import sys
from collections import deque


def run(groups: int, turns: int, rate: list[float], cap: float, whole: float, half: float, fair: bool, rec: list[float]) -> tuple[list[int], list[int]]:
    """rate[g]: cost units group g produces per turn; cap: per-group unsealed bound (D7: one turn's
    room); rec[g]: one record's cost. Returns per group the max turns a record waited unsealed and
    the number of records sealed."""
    backlog: list[deque[int]] = [deque() for _ in range(groups)]  # each record's birth turn
    credit = [0.0] * groups
    worst = [0] * groups
    sealed = [0] * groups
    next_seal = 0
    for t in range(turns):
        # producers fill during the previous barrier, up to the cap (apply waits past it)
        for g in range(groups):
            credit[g] += rate[g]
            while credit[g] >= rec[g] and (len(backlog[g]) + 1) * rec[g] <= cap:
                backlog[g].append(t)
                credit[g] -= rec[g]
            credit[g] = min(credit[g], rec[g])  # a blocked writer does not bank work
        order = list(range(next_seal, groups)) + list(range(0, next_seal))
        used, first, resume_at = 0.0, True, None
        for g in order:
            limit = whole if first else half
            room = limit - used
            if room < min(rec):
                resume_at = g if resume_at is None else resume_at
                break
            took = False
            while True:  # D7: call again with the room left until it returns nothing
                room = (whole if first else half) - used
                if backlog[g] and rec[g] <= room:
                    born = backlog[g].popleft()
                    worst[g] = max(worst[g], t - born)
                    sealed[g] += 1
                    used += rec[g]
                    first = False
                    took = True
                    continue
                # returns nothing
                if backlog[g] and resume_at is None:
                    # literal code: nothing under less than the whole room -> resume here
                    resume_at = ((g + 1) % groups) if (fair and took) else g
                break
        if resume_at is not None:
            next_seal = resume_at
    return worst, sealed


def run_log(groups: int, turns: int, rate: list[float], room: float, fair: bool, rec: list[float]) -> tuple[list[int], list[int], list[int]]:
    """The revised D7: the log's unsealed cost (every group's) is bounded at `room`, one turn's
    room after its first record. A writer whose record would pass it waits, unadmitted, in arrival
    order. Returns per group the max turns from admission to seal, the max turns from arrival to
    seal, and the records sealed."""
    backlog: list[deque[tuple[int, int]]] = [deque() for _ in range(groups)]  # (arrival, admission)
    credit = [0.0] * groups
    waiting: deque[tuple[int, int]] = deque()  # (group, arrival turn), FIFO
    pending = [False] * groups  # a group's writer waits for admission (one blocked join each)
    unsealed = 0.0
    window = [0] * groups
    wait = [0] * groups
    sealed = [0] * groups
    next_seal = 0
    for t in range(turns):
        # (credit accrues per turn; a writer arrives once per record its credit pays for)
        for g in range(groups):
            credit[g] = min(credit[g] + rate[g], max(rate[g], rec[g]))
        arrivals = True
        while arrivals:
            arrivals = False
            for g in range(groups):
                if not pending[g] and credit[g] >= rec[g]:
                    credit[g] -= rec[g]
                    pending[g] = True
                    waiting.append((g, t))
                    arrivals = True
            # admit in arrival order while the log has room; the head blocks the rest
            while waiting and unsealed + rec[waiting[0][0]] <= room:
                g, born = waiting.popleft()
                pending[g] = False
                backlog[g].append((born, t))
                unsealed += rec[g]
                arrivals = True
        order = list(range(next_seal, groups)) + list(range(0, next_seal))
        used, first, resume_at = 0.0, True, None
        for g in order:
            took = False
            while True:  # D7: called again with the room left until it returns nothing
                limit = room * 2 if first else room  # the first record may use the whole segment
                left = limit - used
                if backlog[g] and rec[g] <= left:
                    born, admitted = backlog[g].popleft()
                    window[g] = max(window[g], t - admitted)
                    wait[g] = max(wait[g], t - born)
                    sealed[g] += 1
                    used += rec[g]
                    unsealed -= rec[g]
                    first = False
                    took = True
                    continue
                if backlog[g] and resume_at is None:
                    resume_at = ((g + 1) % groups) if (fair and took) else g
                break
        if resume_at is not None:
            next_seal = resume_at
    return window, wait, sealed


def main() -> int:
    ok = True
    print("1. fairness: group 0 bulk (rate 2 rooms/turn, records of 1/8 room), group 1 one small record per turn")
    for cap in (8.0, 16.0):
        for fair in (False, True):
            worst, sealed = run(2, 2000, rate=[16.0, 0.05], cap=cap, whole=8.0, half=8.0, fair=fair, rec=[1.0, 0.05])
            label = "resume at the drained group (literal)" if not fair else "resume after the drained group"
            print(f"   cap {cap / 8:.0f}x room  {label:40s} max wait turns {worst}  records sealed {sealed}")
    print("2. window: N groups, each bounded at one turn's room (cap = half), all saturating")
    for n in (1, 2, 4, 8, 32):
        worst, _ = run(n, 2000, rate=[100.0] * n, cap=8.0, whole=8.0, half=8.0, fair=True, rec=[1.0] * n)
        print(f"   N={n:3d}  max turns a record waits unsealed: {max(worst)}")
    print("   (D107's contract is one barrier plus one turn: a record waits at most 1 turn)")
    print("3. window, bound per log: N groups saturating, the log's unsealed cost at most one room")
    for n in (1, 2, 4, 8, 32, 256):
        window, wait, sealed = run_log(n, 2000, rate=[100.0] * n, room=8.0, fair=True, rec=[1.0] * n)
        fine = max(window) <= 1 and min(sealed) > 0
        ok &= fine
        print(f"   N={n:3d}  max turns admitted to sealed: {max(window)}  arrival to sealed: {max(wait)}  min records sealed per group: {min(sealed)}  {'ok' if fine else 'FAIL'}")
    print("4. one bulk group (2 rooms/turn, records of 1/8 room) and one ack=sync group, bound per log")
    for fair in (False, True):
        window, wait, sealed = run_log(2, 2000, rate=[16.0, 0.05], room=8.0, fair=fair, rec=[1.0, 0.05])
        fine = max(window) <= 1 and wait[1] <= 2 and sealed[1] > 0
        ok &= fine
        label = "resume after the drained group" if fair else "resume at the drained group (literal)"
        print(f"   {label:40s} admitted to sealed {window}  arrival to sealed {wait}  sealed {sealed}  {'ok' if fine else 'FAIL'}")
    print("   with 31 bulk groups and the sync group (32 writers):")
    window, wait, sealed = run_log(32, 2000, rate=[16.0] * 31 + [0.05], room=8.0, fair=True, rec=[1.0] * 31 + [0.05])
    fine = max(window) <= 1 and min(sealed) > 0
    ok &= fine
    print(f"   admitted to sealed max {max(window)}; sync record arrival to sealed {wait[31]} turns (FIFO behind 31 writers of 1/8 room); min sealed {min(sealed)}  {'ok' if fine else 'FAIL'}")
    print("5. N5: the fair resume rule, per-group bound 2x the room, bound per log off (rows of 1)")
    for fair in (False, True):
        worst, sealed = run(2, 2000, rate=[16.0, 0.05], cap=16.0, whole=8.0, half=8.0, fair=fair, rec=[1.0, 0.05])
        label = "resume after the drained group" if fair else "resume at the drained group (literal)"
        print(f"   {label:40s} max wait turns {worst}  records sealed {sealed}")
    print("all ok" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
