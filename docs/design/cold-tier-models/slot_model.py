"""Architect probe, revision 4: explicit-state models of D4/D5's slot word.

Model A (the wake's hand-off, CT-R29): one pinner on a warm defer_io slot, an adversarial sweep
that demotes whenever the word is {0, open} with the flag clear, and the wake chore. Variants:
  A1  revision 4 as written read one way: a pinner that meets the owed flag gives back its count,
      moves the slot to waking and waits; the wake stores {0, open} and resumes the waiter, which
      pins again.
  A2  the wake's store hands the pin to each registered waiter ({n, open}).
Reports whether the pinner can be starved (a cycle that never hands it the pin).

Model B (in-place reopen, CT-R30): a host keeps {slot, gen} and the index pointer it got with the
pin; the sweep closes the slot and, pinners having come, reopens in place. Variants:
  B1  the reopen keeps gen (revision 4: only a miss and an erase bump it).
  B2  the reopen bumps gen.
Reports whether a re-pin by handle succeeds while the host's cached index is the closed one.
"""

from collections import deque


def model_a(handoff: bool) -> tuple[int, bool]:
    # state: (pins, st, flag, pc) ; st in open/waking/demoting ; pc: pinner's program counter
    # pc: 'try' -> CAS pin ; 'check' -> load flag ; 'wait' -> parked ; 'got' -> holds pin (done)
    start = (0, "open", True, "try")
    seen, q, edges = {start}, deque([start]), {}
    while q:
        s = q.popleft()
        pins, st, flag, pc = s
        nxt = []
        # pinner
        if pc == "try" and st == "open":
            nxt.append((pins + 1, st, flag, "check"))
        if pc == "try" and st != "open":
            nxt.append((pins, st, flag, "wait"))
        if pc == "check":
            if not flag:
                nxt.append((pins, st, flag, "got"))
            elif st == "open":  # wins the move to waking; gives its count back and waits
                nxt.append((pins - 1, "waking", flag, "wait"))
            else:
                nxt.append((pins - 1, st, flag, "wait"))
        # the wake chore: promote, then store open and resume the waiter
        if st == "waking":
            if handoff and pc == "wait":
                nxt.append((pins + 1, "open", False, "got"))
            else:
                nxt.append((pins, "open", False, "try" if pc == "wait" else pc))
        # the sweep: demote from {0, open} on a hot slot (flag clear), then store open and resume
        if pins == 0 and st == "open" and not flag:
            nxt.append((0, "demoting", flag, pc))
        if st == "demoting":
            nxt.append((pins, "open", True, "try" if pc == "wait" else pc))
        edges[s] = nxt
        for t in nxt:
            if t not in seen:
                seen.add(t)
                q.append(t)
    # starvation: a cycle among states where the pinner never reaches 'got'
    non_got = {s for s in seen if s[3] != "got"}
    # find a cycle within non_got by DFS
    color = {}

    def dfs(u) -> bool:
        color[u] = 1
        for v in edges.get(u, []):
            if v not in non_got:
                continue
            if color.get(v) == 1:
                return True
            if v not in color and dfs(v):
                return True
        color[u] = 2
        return False

    cycle = any(dfs(s) for s in non_got if s not in color)
    return len(seen), cycle


def model_b(bump_on_reopen: bool) -> bool:
    gen, instance = 1, 1
    handle = (gen, instance)  # host keeps {slot, gen} and the index pointer from its last pin
    # sweep: open -> closing -> close(); pinners came -> opening (reopen in place) -> open
    instance += 1
    if bump_on_reopen:
        gen += 1
    # host re-pins by handle: compare-exchange succeeds while gen matches and state is open
    repin_ok = handle[0] == gen
    stale_pointer_used = repin_ok and handle[1] != instance
    return stale_pointer_used


if __name__ == "__main__":
    for name, h in (("A1 revision 4, no hand-off", False), ("A2 wake hands the pin to its waiters", True)):
        n, cyc = model_a(h)
        print(f"{name}: {n} states, pinner starvable (cycle without the pin): {cyc}")
    for name, b in (("B1 reopen keeps gen", False), ("B2 reopen bumps gen", True)):
        print(f"{name}: re-pin by handle succeeds onto a closed index's cached pointer: {model_b(b)}")
