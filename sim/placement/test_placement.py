"""Each strategy on a small graph with its placement checked by hand."""

from __future__ import annotations

import math
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import root_index
from cluster import Cluster, Placement, bytes_on, evaluate, peers
from partition import build_adjacency, cut, partition
from scored import locality, rendezvous, scored_place
from spar import Spar
from strategies import (
    _mix,
    adaptive_place,
    affinity_place,
    churn_hash,
    copyset,
    count_place,
    hash_place,
    local_post,
)
from workloads import Workload, from_edges

# Two triangles joined by a bridge: 0-1-2 and 3-4-5, bridge 2-3.
BRIDGE = [(0, 1), (0, 2), (1, 2), (2, 3), (3, 4), (3, 5), (4, 5)]


def manual(units: int, templates: list[tuple[int, tuple[int, ...], float]], size: float = 1.0) -> Workload:
    return Workload("manual", [size] * units, [0.0] * units, [-1] * units, templates, [])


class ClusterTest(unittest.TestCase):
    def test_domains_and_copysets(self) -> None:
        c = Cluster.of(6)
        self.assertEqual([c.domain(x) for x in range(6)], [0, 0, 1, 1, 2, 2])
        self.assertEqual(copyset(c, 0), [0, 2, 4])
        self.assertEqual(copyset(c, 1), [1, 3, 5])
        self.assertEqual(copyset(c, 5), [5, 1, 3])
        self.assertEqual(copyset(Cluster.of(10), 0), [0, 4, 7])  # 3 shares node 0's rack

    def test_evaluate_by_hand(self) -> None:
        c = Cluster.of(6)
        w = manual(3, [(0, (1, 2), 1.0), (2, (0,), 1.0)])
        p = Placement("t", [[0, 2, 4], [1, 3, 5], [0, 3, 5]], [[], [], []])
        m = evaluate(w, c, p)
        # Request 1: node 0 holds units 0 and 2, unit 1 needs a second node. Request 2: local.
        self.assertEqual(m["local_frac"], 0.5)
        self.assertEqual(m["fanout_mean"], 1.5)
        self.assertEqual(m["spread_frac"], 1.0)
        self.assertEqual(m["copies_per_unit"], 3.0)
        self.assertEqual(peers(p, 6)[0], {2, 3, 4, 5})
        self.assertEqual(bytes_on(w, p, {0}), 2.0)


class HashCountTest(unittest.TestCase):
    def test_hash(self) -> None:
        w = from_edges("bridge", BRIDGE)
        p = hash_place(w, Cluster.of(6), seed=3)
        for u in range(6):
            self.assertEqual(len(set(p.voters[u])), 3)
            self.assertEqual(p.reads[u], [])
        self.assertEqual(p.voters, hash_place(w, Cluster.of(6), seed=3).voters)

    def test_count_is_ring_adjacent(self) -> None:
        w = from_edges("bridge", BRIDGE)
        c = Cluster.of(6)
        p = count_place(w, c, copies=2)
        for u in range(6):
            s = _mix(u) % 6
            self.assertEqual(p.voters[u], [s, (s + 1) % 6, (s + 2) % 6])
            self.assertEqual(p.reads[u], [(s + 3) % 6, (s + 4) % 6])
        # Name-adjacent voters share a rack: [0, 1, 2] spans domains {0, 1} only.
        self.assertLess(evaluate(w, c, count_place(w, c))["spread_frac"], 1.0)

    def test_hash_churn_moves_repair_plus_share(self) -> None:
        w = manual(100, [])
        c = Cluster.of(10)
        p = hash_place(w, c)
        moved = churn_hash(w, c, p, {0}, seed=1)
        self.assertGreater(moved, bytes_on(w, p, {0}))


class AffinityTest(unittest.TestCase):
    def test_groups_share_a_copyset(self) -> None:
        w = from_edges("bridge", BRIDGE)
        w.group = [7, 7, 7, 9, 9, 9]
        c = Cluster.of(6)
        p = affinity_place(w, c)
        self.assertEqual(p.voters[0], p.voters[1])
        self.assertEqual(p.voters[1], p.voters[2])
        self.assertEqual(p.voters[3], p.voters[5])
        self.assertIn(p.voters[0], [copyset(c, j) for j in range(6)])


class PartitionTest(unittest.TestCase):
    def test_bridge_splits_at_the_bridge(self) -> None:
        adj = build_adjacency(6, {e: 1.0 for e in BRIDGE})
        part = partition(adj, [1.0] * 6, 2)
        self.assertEqual(len({part[0], part[1], part[2]}), 1)
        self.assertEqual(len({part[3], part[4], part[5]}), 1)
        self.assertNotEqual(part[0], part[3])
        self.assertEqual(cut(adj, part), 1.0)

    def test_balanced_and_better_than_random(self) -> None:
        import random

        rng = random.Random(0)
        edges = {}
        for u in range(400):
            for v in range(u + 1, 400):
                if rng.random() < (0.2 if u // 50 == v // 50 else 0.002):
                    edges[(u, v)] = 1.0
        adj = build_adjacency(400, edges)
        part = partition(adj, [1.0] * 400, 8)
        loads = [part.count(p) for p in range(8)]
        self.assertLessEqual(max(loads), math.ceil(1.03 * 50))
        rand = [rng.randrange(8) for _ in range(400)]
        self.assertLess(cut(adj, part), 0.3 * cut(adj, rand))

    def test_local_post_adds_bridge_copies(self) -> None:
        w = from_edges("bridge", BRIDGE)
        base = Placement("partition", [[0, 2, 4]] * 3 + [[1, 3, 5]] * 3, [[] for _ in range(6)])
        p = local_post(w, base)
        # Leader 0 (units 0-2) needs unit 3; leader 1 (units 3-5) needs unit 2. Nothing else.
        self.assertEqual(p.reads, [[], [], [1], [0], [], []])
        self.assertEqual(evaluate(w, Cluster.of(6), p)["local_frac"], 1.0)


class SparTest(unittest.TestCase):
    def figure3(self) -> Spar:
        """The paper's Figure 3: users 1-6 are 0-5; M1, M2, M3 are servers 0, 1, 2; K = 0."""
        s = Spar(3, 0, [1.0] * 6)
        s.online = False
        for u, server in [(0, 0), (1, 0), (2, 0), (3, 0), (4, 1), (5, 2)]:
            s.add_user(u, server)
        for u, v in [(0, 1), (0, 2), (0, 3), (0, 4), (4, 5)]:
            s.add_edge(u, v)
        return s

    def test_figure3_state(self) -> None:
        s = self.figure3()
        self.assertEqual(s.replicas(), 4)
        self.assertEqual(s.slaves[0], {1})
        self.assertEqual(s.slaves[4], {0, 2})
        self.assertEqual(s.slaves[5], {1})

    def test_figure3_deltas(self) -> None:
        s = self.figure3()
        s.add_edge(0, 5)  # status quo: replicas of 1 on M3 and of 6 on M1
        self.assertEqual(s.replicas(), 6)
        self.assertEqual(s.move_delta(0, 2), 1)  # 1 to M3: 2', 3', 4' and 1' on M1, minus 5' on M1
        self.assertEqual(s.move_delta(5, 0), -3)  # 6 to M1: the paper's total of 3

    def test_figure3_balance_keeps_status_quo(self) -> None:
        s = self.figure3()
        s.online = True
        s.add_edge(0, 5)
        # Moving 6 to M1 saves 3 but M1 has 4 masters against M3's 1: the paper keeps status quo.
        self.assertEqual(s.master, [0, 0, 0, 0, 1, 2])
        self.assertEqual(s.replicas(), 6)
        self.assertEqual(s.slaves[0], {1, 2})
        self.assertEqual(s.slaves[5], {0, 1})

    def test_literal_rule_moves_to_fuller_partition(self) -> None:
        # M1 holds 0, 1; M2 holds 2, 3, 4; M3 holds 5, 6. Edge 0-5 arrives: moving 5 to M1
        # saves 3 replicas, moving 0 to M3 saves 1, and M1 and M3 hold 2 masters each.
        for literal, master5 in ((False, 2), (True, 0)):
            t = Spar(3, 0, [1.0] * 7, literal=literal)
            t.online = False
            for u, server in [(0, 0), (1, 0), (2, 1), (3, 1), (4, 1), (5, 2), (6, 2)]:
                t.add_user(u, server)
            for u, v in [(0, 1), (0, 4), (4, 5)]:
                t.add_edge(u, v)
            t.online = True
            t.add_edge(0, 5)
            self.assertEqual(t.master[5], master5)
            self.assertEqual(t.master[0], 0)

    def test_move_when_balance_allows(self) -> None:
        s = Spar(2, 0, [1.0] * 4)
        for u, server in [(0, 0), (1, 1), (2, 0), (3, 0)]:
            s.add_user(u, server)
        s.add_edge(0, 1)
        self.assertEqual(s.master[0], 1)
        self.assertEqual(s.replicas(), 0)

    def test_redundancy_floor(self) -> None:
        s = Spar(5, 2, [1.0] * 2)
        s.add_edge(0, 1)
        for u in (0, 1):
            self.assertGreaterEqual(len(s.slaves[u]), 2)
            self.assertNotIn(s.master[u], s.slaves[u])

    def test_add_server_takes_its_share(self) -> None:
        s = Spar(5, 0, [1.0] * 40)
        s.alive[4] = False
        s.nalive = 4
        for u in range(40):
            s.add_user(u)
        s.add_server(4)  # forced redistribution: N/(M^2+M) from each of M=4 servers, 8 in all
        self.assertEqual(s.nmasters, [8, 8, 8, 8, 8])

    def test_remove_server_restores_semantics(self) -> None:
        w = from_edges("bridge", BRIDGE)
        s = Spar(4, 1, w.size)
        for u, v in w.edges:
            s.add_edge(u, v)
        x = s.master[0]
        s.remove_server(x)
        for u in range(6):
            self.assertNotEqual(s.master[u], x)
            self.assertNotIn(x, s.slaves[u])
            for v in s.adj[u]:
                self.assertTrue(s.master[v] == s.master[u] or s.master[u] in s.slaves[v])


class AdaptiveTest(unittest.TestCase):
    def base(self, voters: list[list[int]]) -> Placement:
        return Placement("affinity", voters, [[] for _ in voters])

    def test_copy_at_leader_within_budget(self) -> None:
        c = Cluster.of(6)
        w = manual(4, [(0, (1,), 1.0)])
        b = self.base([[0, 2, 4], [1, 3, 5], [5, 0, 2], [5, 0, 2]])
        p = adaptive_place(w, c, b, beta=1.0)
        self.assertEqual(p.reads, [[], [0], [], []])
        self.assertEqual(p.extra["feeder"], {(1, 0): 5})  # node 0 already peers with 5
        self.assertEqual(p.voters, b.voters)  # voters never move
        self.assertEqual(adaptive_place(w, c, b, beta=0.25).reads, [[], [], [], []])

    def test_hub_promoted_to_all(self) -> None:
        c = Cluster.of(6)
        w = manual(7, [(a, (0,), 1.0) for a in range(1, 7)])
        b = self.base([[0, 2, 4]] + [copyset(c, j) for j in range(6)])
        p = adaptive_place(w, c, b, beta=10.0)
        self.assertEqual(p.extra["hubs"], 1)
        self.assertEqual(sorted(p.voters[0] + p.reads[0]), list(range(6)))

    def test_hysteresis_keeps_incumbent(self) -> None:
        c = Cluster.of(6)
        w = manual(3, [(0, (1,), 1.0), (0, (2,), 1.1)])
        b = self.base([[0, 2, 4], [1, 3, 5], [1, 3, 5]])
        p0 = adaptive_place(w, c, b, beta=0.7)
        self.assertEqual(p0.reads, [[], [], [0]])
        drifted = [1.05, 1.0]
        self.assertEqual(
            adaptive_place(w, c, b, beta=0.7, rates=drifted, incumbent=p0.reads, h=0.0).reads, [[], [0], []]
        )
        self.assertEqual(
            adaptive_place(w, c, b, beta=0.7, rates=drifted, incumbent=p0.reads, h=0.25).reads, [[], [], [0]]
        )


class ScoredTest(unittest.TestCase):
    def test_engine_enforces_spread(self) -> None:
        p = scored_place(manual(1, []), Cluster.of(6), lambda u, node, view: -float(node))
        self.assertEqual(p.voters, [[0, 2, 4]])  # 1 and 3 share a rack with an earlier pick

    def test_engine_fills_what_the_scorer_filters(self) -> None:
        p = scored_place(manual(1, []), Cluster.of(6), lambda u, node, view: 1.0 if node == 0 else None)
        self.assertEqual(p.voters, [[0, 2, 4]])
        self.assertEqual(p.extra["engine_filled"], 2)

    def test_capacity(self) -> None:
        p = scored_place(manual(2, []), Cluster.of(6), lambda u, node, view: -float(node), capacity=1.0)
        self.assertEqual(p.voters, [[0, 2, 4], [1, 3, 5]])

    def test_locality_scorer(self) -> None:
        w = manual(2, [])
        w.size = [2.0, 1.0]
        p = scored_place(w, Cluster.of(6), locality([{1: 5.0}, {0: 5.0}]))
        self.assertEqual(p.voters, [[0, 2, 4], [0, 3, 5]])

    def test_rendezvous_matches_hash_shape(self) -> None:
        p = scored_place(manual(50, []), Cluster.of(10), rendezvous)
        self.assertTrue(all(len(set(v)) == 3 for v in p.voters))
        self.assertEqual(p.extra["engine_filled"], 0)


class RootIndexTest(unittest.TestCase):
    def test_model(self) -> None:
        p = root_index.Params()
        self.assertEqual(root_index.relay_depth(10, p), 1)
        self.assertEqual(root_index.relay_depth(100, p), 2)
        self.assertEqual(root_index.model(1000, 1.0, p)["all_msgs_per_create"], 999)
        self.assertEqual(root_index.model(1000, 1.0, p)["cache_msgs_per_create"], 12)
        self.assertEqual(root_index.crossover(1.0, p), 17)
        self.assertEqual(root_index.crossover(100.0, p), 14)
        self.assertEqual(root_index.crossover(1.0, root_index.Params(miss_rate=1.0)), math.inf)


if __name__ == "__main__":
    unittest.main()
