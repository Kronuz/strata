# Track C, C3 gate report (jobs running)

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

Results are filled from the jobs' logs when they finish.

| Row | macOS | Linux VM | FreeBSD VM |
|---|---|---|---|
| Build at `consensus:b7f48b2414d8` | `261118-025944-a6e140` | `261118-030042-dd890d` | `261118-030028-8b42e8` |
| T5 rows, `Driver::repair` rows, T2 simulation (`ctest` `consensus_consensus`, `consensus_driver`, `consensus_simulation`) | `261118-025951-ea8fde` | `261118-030044-f57019` | `261118-030029-db899b` |
| T2 mutant "repair below `base_index`" (`tools/mutants.sh` filtered to it, 400 seeds of 10,000 steps) | `261118-025953-961e8c` | `261118-030045-a0fff5` | `261118-030031-7bd707` |
| `consensus_test` built at that mutant fails (`--fails-without`) | `261118-025955-8f229e` | in the mutant job's log | in the mutant job's log |
