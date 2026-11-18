# Track C, C3 gate report

C3 (repair at or below the commit cursor, D10) as built in `consensus` (`consensus:afd456d`)
and reached through `Driver::repair` (`consensus:57745e8`). Every row runs at `consensus` main,
`consensus:b7f48b2414d8`, built `RelWithDebInfo` with Ninja, on macOS (x86_64, APFS), the Linux
gate VM (document-gate's, port 18782) and the FreeBSD VM (port 18784). Each job is a campaign
job in the `consensus` repository at that commit, tagged to quest `track-c-c3-gate`.

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
| Build at `consensus:b7f48b2414d8` | `261118-025944-a6e140`, ok | `261118-042020-71911d`, queued | `261118-030028-8b42e8`, ok |
| T5 rows: `consensus_test` (`repair_at_or_below_commit`, 8 boundaries restored) | `261118-025951-ea8fde`, 39,611 checks, 0 failures | `261118-042022-3bd680`, queued | `261118-030029-db899b`, ok (below) |
| `Driver::repair` rows: `consensus_driver_test` | same job, 124 checks, 0 failures | same job as the row above, queued | `261118-042015-dba12c`, queued (rerun) |
| T2 simulation with repairs, and the Driver simulation | same job: 200 seeds of 10,000 steps, 1,081 repairs, 1,730 elections; Driver: 20 seeds of 3,000 steps | same job, queued | `261118-030029-db899b`, ok: the same counts |
| T2 mutant "repair below `base_index`" (`tools/mutants.sh` filtered to it) | `261118-025953-961e8c`: unit yes, membership no, simulation yes (400 seeds, first at seed 1) | `261118-042024-031f8b`, queued | `261118-042017-9d13e2`, queued |
| `consensus_test` built at that mutant fails (`--fails-without`) | `261118-025955-8f229e`: fails "a boundary below base_index is refused", 4 failures | in the mutant job's log | in the mutant job's log |

On FreeBSD the first rows job (`261118-030029-db899b`) passed T5's and T2's runs and failed
the Driver row because `consensus_driver_test`, linked last by the build job, was missing
after the VM's poweroff. `vm.sh` now rebuilds a missing target before a step and syncs
before it stops the VM; `261118-042015-dba12c` reruns the rows. The Linux jobs first ran
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
