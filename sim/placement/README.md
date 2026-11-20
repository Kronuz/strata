# Placement simulator

Replays co-access workloads against the placement strategies of
`docs/design/placement-strategies.md` and reports, per strategy, replication overhead,
requests served on one node, fan-out, bytes moved per 1% membership churn, peers per node and
voter spread over failure domains. Results and conclusions: `docs/perf/placement-strategies-sim.md`.

Pure Python 3, standard library only; the offline partitioner is our own METIS-style multilevel
code (`partition.py`).

```sh
python3 sim/placement/run.py --smoke                     # unit tests and small runs, seconds
python3 sim/placement/run.py --graph PATH --nodes 10,100 # a SNAP or WOSN edge list, gzip ok
python3 sim/placement/run.py --synthetic 1 --nodes 1000  # 100k tenants, ~1M indexes
python3 sim/placement/run.py --root-index                # the root's cost model
```

Public graphs are not committed. Download them into the worktree's `.scratch/`:

| Graph | Source | License |
|---|---|---|
| Facebook New Orleans links (63,731 users, 817,090 links, SPAR's own dataset) | `https://socialnetworks.mpi-sws.org/data/facebook-links.txt.gz` (Viswanath et al., WOSN 2009) | No license stated; the authors ask users to cite the paper |
| Slashdot Zoo, February 2009 (82,168 users, 504,230 undirected links) | `https://snap.stanford.edu/data/soc-Slashdot0902.txt.gz` (Leskovec et al., 2009) | No license stated; SNAP asks users to cite the source |

| File | What it holds |
|---|---|
| `workloads.py` | Edge-list loader and the synthetic many-indexes workload |
| `cluster.py` | Nodes, failure domains, placements, metrics |
| `partition.py` | Multilevel k-way partitioner |
| `spar.py` | SPAR's online algorithm as published |
| `strategies.py` | Hash, count, affinity, partition, adaptive locality, churn |
| `root_index.py` | Cost model: root read copies everywhere against a per-node cache |
| `test_placement.py` | One hand-checked small case per strategy |
