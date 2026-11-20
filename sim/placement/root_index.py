"""Cost model for serving the root (the index of indexes): read copies everywhere or a cache.

Option ``all``: a read copy of the root on every node, fed through a relay tree of fan-out F
from its V voters; every lookup is local. Option ``cache``: lookups go to a per-node cache and
a miss asks the nearest root member. Both pay the root's own consensus commit (V - 1 appends).
"""

from __future__ import annotations

import math
from dataclasses import dataclass


@dataclass(frozen=True)
class Params:
    voters: int = 3
    fanout: int = 16
    entry_bytes: int = 256
    header_bytes: int = 64
    readers_per_index: int = 5  # nodes that look up a new index (its copies plus coordinators)
    miss_rate: float = 0.1  # capacity and refresh misses per node per second, warm cache
    hop_ms: float = 0.25  # one-way, same datacenter
    rtt_near_ms: float = 0.5  # to a root member in the node's own zone
    batch_ms: float = 2.0  # a relay batches appends this long before forwarding
    closed_lag_ms: float = 50.0  # follower reads trail the leader by the closed revision
    us_per_message: float = 5.0  # CPU per message sent or received, both ends
    usd_per_core_hour: float = 0.04


DEFAULT = Params()


def relay_depth(n: int, p: Params) -> int:
    """Hops from the voters to the farthest read copy in a tree of fan-out F."""
    left = n - p.voters
    depth, width = 0, p.voters
    while left > 0:
        width *= p.fanout
        left -= width
        depth += 1
    return depth


def model(n: int, rate: float, p: Params = DEFAULT) -> dict[str, float]:
    """Per-creation and steady-state costs of both options at ``n`` nodes and ``rate`` creations/s."""
    msg = p.entry_bytes + p.header_bytes
    commit = p.voters - 1
    all_msgs = commit + max(0, n - p.voters)
    cache_msgs = commit + 2 * p.readers_per_index
    all_per_s = rate * all_msgs
    cache_per_s = rate * cache_msgs + 2 * p.miss_rate * n
    depth = relay_depth(n, p)
    cold_misses = rate * p.readers_per_index / n
    miss_frac_per_node = cold_misses + p.miss_rate
    return {
        "nodes": n,
        "rate": rate,
        "all_msgs_per_create": all_msgs,
        "cache_msgs_per_create": cache_msgs,
        "all_bytes_per_create": all_msgs * msg,
        "cache_bytes_per_create": cache_msgs * msg,
        "all_msgs_per_s": all_per_s,
        "cache_msgs_per_s": cache_per_s,
        "all_node_in_per_s": rate,
        "all_relay_out_per_s": rate * p.fanout,
        "cache_member_in_per_s": (rate * p.readers_per_index + p.miss_rate * n) / p.voters,
        "all_cores": all_per_s * 2 * p.us_per_message / 1e6,
        "cache_cores": cache_per_s * 2 * p.us_per_message / 1e6,
        "all_usd_year": all_per_s * 2 * p.us_per_message / 1e6 * p.usd_per_core_hour * 24 * 365,
        "cache_usd_year": cache_per_s * 2 * p.us_per_message / 1e6 * p.usd_per_core_hour * 24 * 365,
        "relay_depth": depth,
        "all_visible_ms": p.rtt_near_ms + depth * (p.hop_ms + p.batch_ms),
        "cache_miss_ms": p.rtt_near_ms,
        "cache_misses_per_node_s": miss_frac_per_node,
        "cache_stale_ms": p.closed_lag_ms,
    }


def crossover(rate: float, p: Params = DEFAULT) -> float:
    """Smallest node count where ``cache`` sends fewer messages per second than ``all``.

    ``all`` sends ``rate * (n - 1)``; ``cache`` sends ``rate * (V - 1 + 2q) + 2 * m * n``. The
    cache wins once ``n * (rate - 2m) > rate * (V + 2q)``, and never when ``rate <= 2m``.
    """
    slope = rate - 2 * p.miss_rate
    if slope <= 0:
        return math.inf
    return math.floor(rate * (p.voters + 2 * p.readers_per_index) / slope) + 1


def table(nodes: list[int], rates: list[float], p: Params = DEFAULT) -> list[dict[str, float]]:
    return [model(n, r, p) for r in rates for n in nodes]
