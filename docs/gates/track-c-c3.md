# Track C, C3 gate report

C3 (repair at or below the commit cursor, D10) as built in `consensus` (`consensus:afd456d`)
and reached through `Driver::repair` (`consensus:57745e8`). The table below reports the rows at
`consensus:6a76867` (quest `consensus-c3-rows`), which adds T5 rows for two review findings
(F1, F2; below) and changes only `test/consensus_test.cc` and `tools/mutants.sh` since
`consensus:b7f48b2`; `include/consensus/core.h` is unchanged. The macOS, Linux VM (port 18782)
and FreeBSD VM (port 18784) rows ran at `consensus:b7f48b2414d8`, built `RelWithDebInfo` with
Ninja; only the rows of the section "Rows at consensus:6a76867" ran at the later commit. Each
job is a campaign job in the `consensus` repository, tagged to quest `track-c-c3-gate` or
`consensus-c3-rows`.

## Where the rows run

`Driver::repair(group, boundary)` posts `Repair{request, boundary}` to the group's Core and
completes with the `RepairCompleted` the Core emits (`driver.h`, `repair`). T5's rows are
therefore Core rows, in `consensus_test`'s `repair_at_or_below_commit` (it prints the count of
boundaries restored). The Driver's own row is `consensus_driver_test`'s `rows` case (a lone
voter repairs at its applied boundary, in a new epoch). The Driver simulation
(`driver_simulation.cc`) issues no repairs; the randomized repairs are in `simulation_test`.

| T5 row | Test |
|---|---|
| Restored at every boundary in `[base_index, applied]`, applied sequence equal to an undamaged replica's | `repair_at_or_below_commit`, loop over `b` |
| From a member's image at the commit index (a damaged, stalled voter) | same, "Detection to repair" block |
| Exactly at a nonzero `base_index` | same, "Exactly at a nonzero base" block |
| The node votes throughout (while the redelivery is outstanding, and elects a new leader) | same |
| Refused while busy (a batch pending) or with a delivery outstanding | same |
| A capture from before the repair is rejected | same |
| A leader transfers before repairing | same |
| A boundary below `base_index`, above applied, or of another term is rejected | same |

## Rows and jobs

| Row | macOS (x86_64, APFS) | Linux VM (18782) | FreeBSD VM (18784) |
|---|---|---|---|
| Build at `consensus:b7f48b2414d8` | `261118-025944-a6e140`, ok | `261118-042020-71911d`, ok (Ubuntu 24.04, g++ 13.3, 9 targets linked) | `261118-030028-8b42e8`, ok |
| T5 rows: `consensus_test` (`repair_at_or_below_commit`, 8 boundaries restored) | `261118-025951-ea8fde`, 39,611 checks, 0 failures | `261118-042022-3bd680`, 39,611 checks, 0 failures | `261118-030029-db899b`, ok (below) |
| `Driver::repair` rows: `consensus_driver_test` | same job, 124 checks, 0 failures | same job: 124 driver checks, 0 failures | `261118-042015-dba12c`, 39,611 checks and 124 driver checks, 0 failures (rerun) |
| T2 simulation with repairs, and the Driver simulation | same job: 200 seeds of 10,000 steps, 1,081 repairs, 1,730 elections; Driver: 20 seeds of 3,000 steps | same job: the same counts (200 seeds of 10,000 steps, 1,081 repairs, 1,730 elections; Driver: 20 seeds of 3,000 steps) | `261118-030029-db899b`, ok: the same counts |
| T2 mutant "repair below `base_index`" (`tools/mutants.sh` filtered to it) | `261118-025953-961e8c`: unit yes, membership no, simulation yes (400 seeds, first at seed 1) | `261118-042024-031f8b`: unit yes, membership no, simulation yes (400 seeds, first at seed 1) | `261118-042017-9d13e2`: unit yes, membership no, simulation yes (400 seeds, first at seed 1) |
| `consensus_test` built at that mutant fails (`--fails-without`) | `261118-025955-8f229e`: fails "a boundary below base_index is refused", 4 failures | in the mutant job's log (the mutated build runs the same suite) | in the mutant job's log (the mutated build runs the same suite) |

On FreeBSD the first rows job (`261118-030029-db899b`) passed T5's and T2's runs and failed
the Driver row because `consensus_driver_test`, linked last by the build job, was missing
after the VM's poweroff. `vm.sh` now rebuilds a missing target before a step and syncs
before it stops the VM; `261118-042015-dba12c` reran the rows and passed. The Linux jobs first ran
behind hybrid-go-no-go's chain, which failed, and were resubmitted without it.

Both VMs were found stopped and are left stopped (the job stops its VM on every exit and
refuses one already running).

## Reading the rows

- The mutant that lets a repair below `base_index` through fails the unit suite (T5's row
  "a boundary below base_index is refused") and the randomized simulation, at its first
  seed; the membership suite does not reach it.
- The Driver reaches Core through one forward, so T5's multi-boundary rows are Core rows.
  The Driver's own row is a lone voter repairing at its applied boundary; the Driver
  simulation issues no repairs. A Driver-level repair-below-`base_index` row, one that
  expects the call to fail with `rejected`, does not exist; it belongs to `consensus`.
- T5's "from each member" is the "Detection to repair" block, which restores one member's
  image at the commit index of a stalled voter; the loop over boundaries restores the
  voter's own local image.

## Rows at consensus:6a76867

The correctness review (F1, F2) found two T5 mutants the unit suite did not kill. `consensus-c3-rows`
adds the rows and registers the mutants in `tools/mutants.sh` (macOS, x86_64, Ninja):

| Row | Job | Result |
|---|---|---|
| Build and full test at `consensus:6a76867` | `261118-144151-83ff5c` | 64 of 64 tests pass |
| P1, repair ignores the term at the base boundary (F1): `consensus_test` at the mutant fails | `261118-044856-2cb29b` (fails-without, A1) | 3 failures, first "a boundary at base_index with another term is refused" |
| P2, repair above the commit index (F2): `consensus_test` at the mutant fails | `261118-044857-1a8ab1` (fails-without, A2) | 3 failures, first "a boundary above commit is refused" |
| Reviewer's probe: unmutated `consensus_test`, then `tools/mutants.sh` narrowed to P1 and P2 (`~/.scratch/xapiand/review-track-c-c3-gate-correctness/run.log`) | reviewer, light holds | 39,668 checks, 0 failures; P1: unit yes, membership no, simulation no (100 seeds); P2: unit yes, membership no, simulation no (100 seeds) |
| Reviewer's isolation of P2 with row 1099 dropped (`isolate.log`) | reviewer | control 39,667 checks, 0 failures; P2: 1 failure, "a boundary above commit is refused", from the new row at `consensus_test.cc:1144` |

The mutated builds in P1's and P2's jobs stop with `missing application batch` after their
first failures, so the full counts differ from the unmutated run. P1 and P2 are caught by the
unit suite alone: the membership suite and the randomized simulation do not reach them.
The Linux and FreeBSD rows above ran before these rows were added (`consensus:b7f48b2`); the
new rows are macOS-only until a Linux or FreeBSD job runs at `consensus:6a76867` or later.

## Residuals

- F4: repair liveness under load is unmeasured. `consensus-sim-loops` A5 owns it.
- N2: `261118-144151-83ff5c` configures with the default generator, not Ninja, so
  `consensus_mutants` (registered only under Ninja, `CMakeLists.txt:115-117`) does not run in
  it. That P1 and P2 are in `tools/mutants.sh` is shown by the reviewer's probe, not by that job.
- N1: rows 1099 and 1144 share the name "a boundary above commit is refused", so P2's
  fails-without log cannot tell which row failed; the reviewer's isolation shows row 1144 kills
  P2 on its own.
- Driver-level repair rows (F3) are `consensus-sim-loops` A3.
