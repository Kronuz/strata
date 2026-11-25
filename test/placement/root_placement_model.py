#!/usr/bin/env python3
"""A model of the root's placement (cluster-release D7), checked against root_placement_test --stdin.

Each seeded scenario is a set of nodes with loads (some repeated or 0, which placement refuses)
and a list of placements; the model computes every shard's members from the rule alone and the
binary's output must match it line for line. Usage: root_placement_model.py BINARY [SCENARIOS]
"""
import random
import subprocess
import sys

VOTER, LEARNER, WITNESS = 0, 1, 2


def model(nodes: dict[int, list[int]], rf: int, shards: int, bad_ids: bool) -> list[str]:
    if shards == 0:
        return ["error"] if bad_ids else []
    if bad_ids or not nodes or rf == 0:
        return ["error"]
    out = []
    for _ in range(shards):
        leader = min(nodes, key=lambda i: (nodes[i][1], nodes[i][0], i))
        rest = sorted((i for i in nodes if i != leader), key=lambda i: (nodes[i][0], nodes[i][1], i))
        data = min(rf, len(nodes))
        members = [(leader, VOTER)]
        if data == 2 and len(nodes) == 2:
            members.append((rest[0], LEARNER))
        elif data == 2:
            members += [(rest[0], VOTER), (rest[1], WITNESS)]
        else:
            members += [(i, VOTER) for i in rest[: data - 1]]
        for i, _ in members:
            nodes[i][0] += 1
        nodes[leader][1] += 1
        out.append(" ".join(f"{i}:{r}" for i, r in members))
    return out


def scenario(seed: int) -> tuple[str, list[str]]:
    r = random.Random(seed)
    ids = r.sample(range(1, 50), r.randint(0, 10))
    bad = False
    if ids and r.random() < 0.05:
        ids.append(r.choice(ids))  # a node twice
        bad = True
    if r.random() < 0.03:
        ids.append(0)
        bad = True
    nodes: dict[int, list[int]] = {}
    lines = []
    for i in ids:
        groups = r.randint(0, 20)
        leaders = r.randint(0, groups)
        lines.append(f"node {i} {groups} {leaders}")
        nodes.setdefault(i, [groups, leaders])
    expected = []
    for _ in range(r.randint(1, 15)):
        rf, shards = r.randint(0 if r.random() < 0.05 else 1, 5), r.randint(0, 6)
        lines.append(f"place {rf} {shards}")
        expected += model(nodes, rf, shards, bad)
    return "\n".join(lines) + "\n", expected


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    binary, count = sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 1000
    shards = 0
    for seed in range(1, count + 1):
        stdin, expected = scenario(seed)
        got = subprocess.run([binary, "--stdin"], input=stdin, capture_output=True, text=True, check=True).stdout.splitlines()
        if got != expected:
            print(f"FAIL seed {seed}:\n{stdin}expected:\n" + "\n".join(expected) + "\ngot:\n" + "\n".join(got), file=sys.stderr)
            return 1
        shards += len(expected)
    print(f"root placement model: {count} scenarios, {shards} shards placed alike: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
