> Status: STUB (2026-11-23) on quest `group-heal`, task `sim-rows`. The suite's short rows are
> below; the gate rows (at least 10^7 steps over at least 1,000 seeds for 3 and 5 voters, and 100
> installs of a 1 GiB file set) and their jobs are the lead's, written when they run.

# Phase 3R gate: replication groups

strata::group grows, heals and survives every fault class the simulator injects, with no
`quorum`-acknowledged write lost, no corrupt byte served, and every history linearizable.

## The simulator rows

`test/group/sim_test.cc` (`group_sim_test ROW [VOTERS]`, ctest `simulation_group_*`, built by
`sim/CMakeLists.txt`) drives the voters' world (`test/group/world.hh`): each node's journal host,
strata index and file set over faultfs, the network and every IO in seeded order on one thread.
Clients record a D9 history (`sim/include/simulation/history.hh`), written and read back through
the file format, then checked by `sim/include/simulation/checker.hh`:

- puts, conditional puts and erases at the leader, acknowledged at `quorum`;
- `linearizable` gets by id through `read_applied`, at any data member;
- `prefix` gets read a replica's applied state; each must equal the key's state after some prefix
  of the log at or below the replica's applied index, monotone per process (consistent prefix
  only, so they never branch the checker's search, SIM-R20);
- uploads: three chunks and a final that writes a key from the staging file. One cut by a leader
  change (its chunks or final in different terms) writes nothing; a whole one writes its value.

After each run the cluster heals and settles: every acknowledged command is at its placement,
every data member's digest equals a replay of the log, and the history is linearizable.

| Row | Faults | Short budget |
|---|---|---|
| `faults_3`, `faults_5` | process crashes, power cuts (faultfs keeps or loses each unsynced 512-byte sector: torn and reordered writes), power cuts at one of the data device's next 40 mutations, two-sided partitions, an isolated leader, clock skew (25-124 % fast), checkpoints with installs, uploads cut by a leader change | 8 seeds of 2,000 events each |
| `barriers_3`, `barriers_5` | the follower's power cut before every mutation of its data device across a window: an upload whose final retires its staging file, then a checkpoint that syncs, commits, publishes and unlinks it (every barrier among them) | one run per crash point |
| `membership_3`, `membership_5` | replace a follower, replace the leader, add a voter, remove a follower, under traffic | one run |
| `growth_1` | cluster-release D7 from a lone voter: 0, 1, 2, 1, 0 replicas, with the two-data-voter window and a witness | one run |
| `device_3` | a log device that persisted a non-prefix subset of its unsynced sectors at a power cut, answered by the journal's checksums | seeds until two such logs |
| `corruption` | A3's simulator half: a damaged segment, manifest or file-set file fences its replica, which reinstalls at or below commit | skipped (77) until task `repair` merges |

Each row asserts its coverage: every fault class it names happened at least once.

## Mutants

`test/group/sim_mutants.py BUILD_DIR NAME [--target=group_sim_test] ROW VOTERS`: each must fail its row.

| Mutant | Rule | Row |
|---|---|---|
| `apply-skips-entry` | apply runs every committed entry | `faults 3` |
| `recovery-as-found` | recovery takes the file set as the bundle names it (lengths and names) | `barriers 3` |
| `read-skips-probe` | `read_applied` waits for the Driver's quorum probe | `faults 3` |
| `apply-abandoned-chunk` | an upload of a lower term is retired at the first entry of a higher one | `faults 3` |
