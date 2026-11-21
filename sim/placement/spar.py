"""SPAR's online partitioning and replication, as published (Pujol et al., SIGCOMM 2010, §4).

Every user has one master and slaves. Local semantics: the server of each master holds a copy
(master or slave) of every neighbor. Redundancy: at least K slaves, and locality slaves count
toward K. The total kept is therefore ``sum over users of max(K, |L_u|)``, where ``L_u`` is the
set of servers other than u's master that host a master of one of u's neighbors.
"""

from __future__ import annotations

import heapq
import math
import random

from cluster import Placement


class Spar:
    """SPAR state over ``m`` servers with redundancy ``k`` and per-user sizes."""

    def __init__(
        self,
        m: int,
        k: int,
        size: list[float],
        seed: int = 0,
        max_move_degree: int = 2000,
        literal: bool = False,
    ) -> None:
        self.m = m
        self.k = k
        self.size = size
        self.rng = random.Random(seed)
        self.max_move_degree = max_move_degree
        # §4.2 also allows a move to a fuller partition when the savings over the second-best
        # option exceed the two partitions' master ratio. Read literally, that lets masters pile
        # up (coefficient of variation 0.5-1.9 on Facebook); the default allows moves only to a
        # partition with fewer masters, which matches the paper's reported balance.
        self.literal = literal
        n = len(size)
        self.alive = [True] * m
        self.nalive = m
        self.master = [-1] * n
        self.slaves: list[set[int]] = [set() for _ in range(n)]
        self.cnt: list[dict[int, int]] = [{} for _ in range(n)]
        self.lcount = [0] * n
        self.adj: list[set[int]] = [set() for _ in range(n)]
        self.nmasters = [0] * m
        self.heap: list[tuple[int, int]] = [(0, s) for s in range(m)]
        self.users = 0
        self.moves = 0
        self.edges_seen = 0
        self.moved_bytes = 0.0
        self.online = True  # False: edges only add replicas (tests build a state this way)

    # -- bookkeeping ---------------------------------------------------------------------------

    def _set_masters(self, s: int, delta: int) -> None:
        self.nmasters[s] += delta
        heapq.heappush(self.heap, (self.nmasters[s], s))

    def _least_loaded(self, exclude: int = -1) -> int:
        while True:
            count, s = self.heap[0]
            if not self.alive[s] or count != self.nmasters[s]:
                heapq.heappop(self.heap)
                continue
            if s == exclude:
                # Rare: look past the excluded server without losing it.
                first = heapq.heappop(self.heap)
                t = self._least_loaded()
                heapq.heappush(self.heap, first)
                return t
            return s

    def _random_alive(self, avoid: set[int]) -> int:
        while True:
            s = self.rng.randrange(self.m)
            if self.alive[s] and s not in avoid:
                return s

    def _settle(self, w: int, touched: tuple[int, ...]) -> int:
        """Restore ``slaves[w] = L_w + enough others for K``; return copies created."""
        mw = self.master[w]
        sl = self.slaves[w]
        cw = self.cnt[w]
        added = 0
        sl.discard(mw)
        for x in touched:
            if x != mw and x in cw and x not in sl:
                sl.add(x)
                added += 1
        lw = len(cw) - (1 if mw in cw else 0)
        self.lcount[w] = lw
        target = max(self.k, lw)
        if len(sl) > target:
            extra = [x for x in touched if x in sl and x not in cw]
            extra += [x for x in sl if x not in cw and x not in touched]
            for x in extra[: len(sl) - target]:
                sl.discard(x)
        while len(sl) < target and len(sl) < self.nalive - 1:
            sl.add(self._random_alive(sl | {mw}))
            added += 1
        return added

    def add_user(self, u: int, server: int = -1) -> None:
        t = server if server >= 0 else self._least_loaded()
        self.master[u] = t
        self._set_masters(t, 1)
        self._settle(u, ())
        self.users += 1

    def replicas(self) -> int:
        return sum(len(s) for s in self.slaves if s is not None)

    # -- the three configurations of an edge event ---------------------------------------------

    def move_delta(self, u: int, t: int) -> int:
        """Change in total slaves if u's master moved to server t."""
        k = self.k
        s = self.master[u]
        cu = self.cnt[u]
        old = self.lcount[u]
        new = len(cu) - (1 if t in cu else 0)
        d = max(k, new) - max(k, old)
        cnt = self.cnt
        master = self.master
        lcount = self.lcount
        for w in self.adj[u]:
            cw = cnt[w]
            mw = master[w]
            lw = lcount[w]
            nl = lw
            if s != mw and cw.get(s, 0) == 1:
                nl -= 1
            if t != mw and t not in cw:
                nl += 1
            if nl != lw:
                d += max(k, nl) - max(k, lw)
        return d

    def apply_move(self, u: int, t: int) -> None:
        s = self.master[u]
        if t not in self.slaves[u]:
            self.moved_bytes += self.size[u]
        self.master[u] = t
        self._set_masters(s, -1)
        self._set_masters(t, 1)
        for w in self.adj[u]:
            cw = self.cnt[w]
            c = cw[s] - 1
            if c:
                cw[s] = c
            else:
                del cw[s]
            cw[t] = cw.get(t, 0) + 1
            self.moved_bytes += self.size[w] * self._settle(w, (s, t))
        # u's old master server keeps its data as a slave when neighbors there still need it.
        self.slaves[u].add(s)
        self._settle(u, (s,))
        self.moves += 1

    def add_edge(self, u: int, v: int) -> None:
        if self.master[u] < 0:
            self.add_user(u)
        if self.master[v] < 0:
            self.add_user(v)
        if v in self.adj[u]:
            return
        self.edges_seen += 1
        self.adj[u].add(v)
        self.adj[v].add(u)
        mu, mv = self.master[u], self.master[v]
        self.cnt[u][mv] = self.cnt[u].get(mv, 0) + 1
        self.cnt[v][mu] = self.cnt[v].get(mu, 0) + 1
        self._settle(u, (mv,))
        self._settle(v, (mu,))
        if mu == mv or not self.online:
            return
        options = [(0, 0, -1, -1)]
        if len(self.adj[u]) <= self.max_move_degree:
            options.append((self.move_delta(u, mv), 1, u, mv))
        if len(self.adj[v]) <= self.max_move_degree:
            options.append((self.move_delta(v, mu), 2, v, mu))
        options.sort()
        for rank, (delta, _, x, t) in enumerate(options):
            if x < 0:
                return
            s = self.master[x]
            second = options[rank + 1][0] if rank + 1 < len(options) else 0
            ratio = self.nmasters[t] / max(1, self.nmasters[s])
            if self.nmasters[t] < self.nmasters[s] or (self.literal and (second - delta) > ratio):
                self.apply_move(x, t)
                return

    # -- servers ----------------------------------------------------------------------------------

    def remove_server(self, x: int) -> float:
        """§4.2 server removal: promote the slave whose server holds most neighbors, water-filled."""
        before = self.moved_bytes
        self.alive[x] = False
        self.nalive -= 1
        cap = math.ceil(self.users / self.nalive)
        mine = [u for u in range(len(self.master)) if self.master[u] == x]
        mine.sort(key=lambda u: -len(self.adj[u]))
        for u in mine:
            best, best_score = -1, -1.0
            for s in self.slaves[u]:
                if not self.alive[s] or self.nmasters[s] >= cap:
                    continue
                here = sum(1 for w in self.adj[u] if self.master[w] == s or s in self.slaves[w])
                score = here / max(1, len(self.adj[u]))
                if score > best_score:
                    best, best_score = s, score
            t = best if best >= 0 else self._least_loaded()
            if t not in self.slaves[u]:
                self.moved_bytes += self.size[u]
            self.master[u] = t
            self.nmasters[x] -= 1
            self._set_masters(t, 1)
            self.slaves[u].discard(t)
            for w in self.adj[u]:
                cw = self.cnt[w]
                c = cw[x] - 1
                if c:
                    cw[x] = c
                else:
                    del cw[x]
                cw[t] = cw.get(t, 0) + 1
                self.moved_bytes += self.size[w] * self._settle(w, (t,))
        for w in range(len(self.master)):
            if x in self.slaves[w]:
                self.slaves[w].discard(x)
                self.moved_bytes += self.size[w] * self._settle(w, ())
        return self.moved_bytes - before

    def add_server(self, y: int) -> float:
        """§4.2 server addition, forced redistribution: each server sends N/(M^2+M) masters."""
        before = self.moved_bytes
        m = self.nalive
        self.alive[y] = True
        self.nalive += 1
        self.nmasters[y] = 0
        heapq.heappush(self.heap, (0, y))
        quota = self.users / (m * m + m)
        carry = 0.0
        by_server: dict[int, list[int]] = {}
        for u, s in enumerate(self.master):
            if s >= 0 and s != y:
                by_server.setdefault(s, []).append(u)
        # The fractional quota carries across servers, so N/(M+1) masters move in total.
        for _s, users in sorted(by_server.items()):
            carry += quota
            take = int(carry)
            carry -= take
            users.sort(key=lambda u: len(self.slaves[u]))
            for u in users[:take]:
                self.apply_move(u, y)
        return self.moved_bytes - before

    def placement(self) -> Placement:
        n = len(self.master)
        return Placement(
            strategy="spar",
            voters=[[self.master[u]] for u in range(n)],
            reads=[sorted(self.slaves[u]) for u in range(n)],
            coord_any=False,
            durable_all=True,
            fanout=0,
            extra={"master_cov": master_cov(self.nmasters, self.alive), "moves": self.moves},
        )


def master_cov(nmasters: list[int], alive: list[bool]) -> float:
    xs = [c for c, a in zip(nmasters, alive) if a]
    mean = sum(xs) / len(xs)
    var = sum((x - mean) ** 2 for x in xs) / len(xs)
    return math.sqrt(var) / mean if mean else 0.0
