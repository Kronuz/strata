> Status: CURRENT (2026-11-20) `sim/` on quest `group-heal`, task `sim-port`; ported from
> `design/simulator` at `27cfb303` (design revision 1, Xapiand `docs/design/simulator.md` on that
> branch; reviews `docs/reviews/simulator-design.md` there). The group's fault rows are `sim-rows`.

# The deterministic simulator

`sim/` runs a cluster on one thread in simulated time. A seed fixes every draw, so a run is a
function of its seed: two runs of one seed take the same events in the same order and write the
same history, on every platform. Clients record what they saw in a history file, and the
linearizability checker judges it. It is header-only (`sim/include/simulation/`), depends on
nothing but the standard library, and builds alone (`cmake -S sim`), so it can leave for its own
repository; strata adds it with `add_subdirectory(sim)`.

## How it works

```
 seed -> stream ("network", "workload", ...)       one child stream per component, by label
          |
 engine: 4-ary heap of (time, sequence) -> event   one thread; ties run in scheduling order
          |            \-> trace hash (time, kind, node, payload digests), checkpoint per 2^16
 network: frames in order or not at all, resets told to both ends, clogs, partitions, datagrams
          |
 clients -> history file (D9) -> checker: per component, Wing-Gong search with Lowe's cache
```

- **Randomness** (`random.hh`). xoshiro256** seeded by splitmix64. `stream::child(label)` derives
  a child from the parent's key and the label alone, so adding draws to one component never
  shifts another's. Bounded draws use Lemire's method with a 128-bit product; distributions are
  integer quantile tables in parts per million. No floating point decides anything, and the
  interface target carries `-ffp-contract=off` for whatever links it.
- **Engine** (`engine.hh`). Events sit in a 4-ary min-heap of 24-byte entries ordered by time,
  then by a 64-bit sequence that never repeats; bodies live in reusable slots. A cancel marks
  the slot, and its heap entry is skipped when it surfaces, because a reused slot holds a newer
  sequence. Every event folds its time, kind and node into the trace hash before its handler
  runs, and every 65,536 events the hash is logged, so two diverging runs bisect to the first
  differing window. `engine_test` pins the hash for a fixed workload.
- **Clock** (`clock.hh`). A node's clock is `offset + t * rate_ppm / 10^6` in 128-bit integers,
  monotone at every rate, with its inverse for scheduling at a local time.
- **Network** (`network.hh`). Nodes sit in regions, zones and racks; a frame's latency comes
  from its distance class. A connection has four lanes, each in send order or not at all: a
  frame that reaches an unreachable link is lost, resets the connection, drops everything in
  flight both ways and tells both ends, so no lane shows a hole. Sends return nothing, so a node
  learns of a partition only from a reset. Bandwidth paces a link, a clog holds its frames, in
  flight too, until it clears, and a link's record lives only while it holds frames or a clog.
  Datagrams are lost, duplicated and reordered independently. Every draw comes from the sending
  node's stream. `network_test` pins the delivery trace.
- **History** (`history.hh`). Version 1 of D9's text format: one operation per line (client, key,
  op, invoked, returned, outcome, value, expected, version, batch, mode). The reader rejects
  other versions and names the line of every malformed record, a batch whose outcomes disagree,
  and a client that overlaps itself or continues after an unknown outcome. `sim_check FILE`
  checks one file (0 linearizable, 1 violation, 2 inconclusive, 3 malformed).
- **Checker** (`checker.hh`). The model is strata's register: absent at version 0, a put makes
  version v + 1, an erase makes the key absent, an expected version that differs rejects the
  write (and its whole batch) with the current version. Keys that batches or range deletes join
  form a component, checked alone (P-compositionality). A component's history splits wherever
  no finite operation is pending; the search over linearizations runs per segment with a cache
  of (linearized set, state), and the reachable configurations, the state plus the unknown
  writes not yet applied, carry across each cut. An unknown outcome, or a write acknowledged
  before commit (`local`, `none`), may take effect after its invocation or never. Past its node
  and configuration caps it answers inconclusive, never linearizable.

## End to end: `sim_run`

`sim_run SEED...` is the pipeline's own test. One server node holds the register; seven client
nodes on two racks issue puts (some conditional), erases and gets on 16 keys over 100 simulated ms
(`--keys`, `--ms`), while a fault every 2 to 20 ms partitions a client, clogs a link, resets a
connection or takes the server down for 0.5 to 10 ms. A client that hears no answer (a timeout or a
reset) records the operation as unknown and retires; a new client takes its node. The register
lives in one place, so every history must be linearizable. `--plant lost-write` acknowledges the
first put after a quarter of the run without storing it, and the checker must reject the history. A
seed takes well under a second in RelWithDebInfo (about 2,000 operations, 8,000 search nodes); at 8
keys and 200 ms one took 17 s, as unknown puts piling on few keys multiply the configurations
carried across cuts. The suite runs five seeds each way, and runs seed 7 twice in two processes,
requiring the same history file byte for byte (`sim/cmake/same_history.cmake`).

## Tests

| CTest | What it holds |
|---|---|
| `simulation_random`, `simulation_engine`, `simulation_network` | draws, ordering, cancel and slot reuse, the network's ordering and reset rules; the engine and network trace pins |
| `simulation_checker` | hand corpora, generated histories mutated into four violation classes, and a brute-force cross-check on 50,000 small histories |
| `simulation_history`, `simulation_sim_check_*` | the file format's round trip and rejections; `sim_check` on both fixtures |
| `simulation_run`, `simulation_run_lost_write`, `simulation_run_same_history` | the end-to-end run above |
| `simulation_engine_wrap` (`SIMULATION_SLOW_TESTS`) | 2^32 reuses of one slot, Release only |

The simulator's own targets build with `-Wall -Wextra -Werror` as private options.

## Open, from the design reviews

These are the simulator's known limits, owned by the steps that need them (`sim-rows` for the
group's rows):

- SIM-R7 residual: a connection epoch per end, so a frame sent before its sender hears of a
  reset is dropped; needed before a transport runs over the network.
- SIM-R20: `nearest` and `session` reads branch the search (2^k nodes for k reads of one state)
  and remove the cuts; fix before clients issue weak reads.
- SIM-R21: carried unknown writes multiply across a component's keys; a carried atom should be a
  candidate only while a finite atom of the segment touches its keys.
- SIM-R22: the caps bound counts, not memory, and the report keeps only the first inconclusive
  component.
- SIM-R25's other half: K11's mutations cover four of T3's twelve classes.
- Disks (D5), the fault planner (D6), production code inside the simulator (D7) and shrinking
  (D10) are not built yet.
