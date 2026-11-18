# Track C, C2 gate report (rerun at today's mains, in progress)

C2 (the shared journal, D11) rerun as campaign jobs at today's `journal` and `consensus` mains.
The report has three parts: the rerun (new results, each with its job), the rows that are
obsolete or owed, and the results from the pre-driver commits, kept as they were written
and not mixed with the new ones.

## Commits and rig

| Piece | Commit | Note |
|---|---|---|
| `journal` main | `journal:4d364f1` | Includes the host layout (`journal:beedebc`) and the many-group lifecycle fixes. |
| `consensus` main | `consensus:5923b6c` | Pins `journal` at `journal:4272229` and `completion` at `completion:7731315`. |
| `completion` | `completion:f4c19b1` | `journal` main's own pin. |

- Every job is tagged to quest `track-c-gates` and built `RelWithDebInfo` with Ninja.
- `journal` rows run in `journal` at `journal:4d364f1`.
- `consensus` rows run at `consensus:5923b6c` with `--ref` pins of `journal:4d364f1` and
  `completion:f4c19b1`, passed as `FETCHCONTENT_SOURCE_DIR_JOURNAL` and
  `FETCHCONTENT_SOURCE_DIR_COMPLETION`.
- The pins are needed: `consensus` main at its own `journal:4272229` and `completion:7731315`
  builds, but `journal` main calls `IO::sync_data_files`, which `completion:7731315` lacks
  (job `261118-082201-3b769c`: `host.h:241`, "only virtual member functions can be marked
  'override'"). `consensus` main has to move both pins before it can take `journal` main. A row at
  `consensus` main's own pins runs beside the pinned one (jobs in the table).
- Platforms: macOS x86_64 (APFS), the Linux gate VM (document-gate's, port 18782) and the FreeBSD
  VM (port 18784). Both VMs are left stopped.
- A journal directory refuses group- or other-writable modes, so a rig with a group-writable umask
  fails its tests; the first Linux run (`261118-082315-f9c2a9`, umask 0002 in the VM's ssh
  session) failed three tests that way (`journal_turn_seal_supersede`, `journal_crc_fixture`,
  `journal_host` `loop_slots`: "journal directory requires exclusive ownership"). The reruns set
  `umask 022`.

## What is built

- **`RFTW0001`**: segments of blocks with per-block manifests, LevelDB-style fragments bound to
  segment and epoch, an open record per epoch, the torn-tail rule, damage costed per group,
  copy-forward reclamation, recycled segments, and staged turns.
- **The turn driver and the shared Store**: `TurnDriver` writes the records the groups' Stores stage
  as one turn with one data barrier; a `journal::Store` with the shared backend stages its batches
  and checkpoint records there, and artifacts stay files of the group, sealed before their
  checkpoint record is staged (Condition 25).
- **The host layout** (`journal:beedebc`): every group and loop of a host in one directory under
  one owner lock; loops' turn logs and groups' artifacts are prefixed views of it. It replaced
  R20's migration and the `journal.shared` marker.
- **The `Worker` on the turn log**, and the `Driver` on a journal `Host`.

## The rerun

Targets are T9's (power loss), T8's (robustness) and T2's (safety over shared journals), from
strata `docs/design/track-c.md`, "Gate rows". Counts are what each test printed. A count that
differs from the pre-driver results below reflects a changed test, not a change in the code's
behavior, and is not read as a ratio.

| Row | Target | Result | Status | Job |
|---|---|---|---|---|
| T9: turn log alone, corrupted blocks, recycled segments, a second loss in recovery (`journal_turn_log_power`, `journal_turn_log`) | Zero acknowledged records lost at every crash point; one corrupted block costs only the groups with records in it | macOS: 2,040 crash points (677 with a second power loss in recovery), 312 corrupted blocks, 4,712 checks, 0 failures; `turn_log`: 53 checks, 0 failures | Pass on macOS | `261118-082443-2bc92d` |
| T9: shared Stores: batches, checkpoints, reclamation, close and reopen (`journal_shared_store`) | Same | macOS: 4,366 crash points (2,183 in each of its `sync` and `tasks` modes), 729,748 checks, 0 failures; 337,931 batches and 82,522 checkpoints acknowledged, 13,590 checkpoints recovered, 118,664 artifacts reclaimed, 44,726 groups closed and reopened | Pass on macOS | `261118-082443-2bc92d` |
| Turn seal, turn bound, placement (new since the pre-driver results; 43 tests) | Pass | macOS: 43 of 43 | Pass on macOS | `261118-082443-2bc92d` |
| Host, driver, framing, CRC fixtures | Pass | macOS: 4 of 4 | Pass on macOS | `261118-082443-2bc92d` |
| T9 mutants: acknowledging a record before its turn's barrier (`no-sync-data`), a shared append that does not wait for its barrier (`no-barrier-wait`) | Each is killed by `journal_shared_store_test` | Pending | Pending | `261118-082955-4b86f4`, `261118-082956-6d198b` |
| T2 over shared journals: Workers, single-voter groups proposing, checkpointing, restoring images (`consensus_shared_worker_test`) | Zero failures at every crash point | macOS: 896 crash points, 11,729 checks, 0 failures; 119,810 commands delivered, 22,356 checkpoints published, 2,869 images restored | Pass on macOS | `261118-082353-311736` |
| T2 over shared journals: 3 nodes, 3 groups each, drops, reordering and power cuts (`consensus_shared_cluster_test`) | Zero failures | macOS: 24 seeds, 464 power cuts, 149,689 checks, 0 failures; 135,451 commands applied, 2,545 checkpoints published, 47 catch-ups left to installs | Pass on macOS | `261118-082353-311736` |
| T2 simulation, core (200 seeds x 10,000 steps) and through the `Driver` on a journal `Host` (20 seeds x 3,000 steps) | Every oracle holds | macOS: ok. Core: 1,730 elections, 2,109 restarts, 1,081 repairs, 433 committed memberships. Driver: 129 elections, 110 crashes, 6,142 proposals, 473 reads | Pass on macOS | `261118-082353-311736` |
| Many-group open and close, real-file Workers (`wave_open` at 64, 256 and with a power cut; `consensus_driver_test`; `consensus_worker_test`) | Pass | macOS: 5 of 5; 124 driver checks and 96,142 worker checks, 0 failures | Pass on macOS | `261118-082353-311736` |
| T2's budget (at least 10^7 steps over at least 10,000 seeds) through the `Driver` on journal `Host`s | Every oracle holds | Pending: 10,000 seeds x 3,000 steps, core and Driver | Pending | `261118-083057-825f85` |
| T2 over shared journals, the cluster test at 10 times its seeds | Zero failures | Pending: 240 seeds x 3,000 steps | Pending | `261118-083059-454b43` |
| The same `consensus` rows at `consensus` main's own pins (`journal:4272229`) | Pass | Pending | Pending | `261118-083105-13747c` (build), `261118-083106-671b49` (rows) |
| T8: the shared journal's decoders, seeded replay (`journal_fuzz_replay`, `journal_fuzz_framing_replay`) | Pass | macOS: 2 of 2 | Pass on macOS | `261118-082443-2bc92d` |
| T8: libFuzzer (Clang 23, ASan and UBSan) on the turn-log segment decoder and the group checkpoint record, at least one CPU-hour per decoder | No crash, timeout or leak | Pending: 5 jobs of 1,500 s from distinct seeds on `journal_fuzz`, which takes both decoders through `data[0]` and cannot pin one, so the CPU-hour is of the binary (125 CPU-minutes), with executed units recorded per job. A 30 s probe ran 34,514 executions at 1,113 per second with no crash (`261118-082230-c83150`) | Pending | `261118-082454-b80095`, `261118-082456-8710c4`, `261118-082457-6ddcb3`, `261118-082459-df4d64`, `261118-082501-0416fc` |
| Linux VM: `journal` rows and mutants | As on macOS | Pending (the first run, `261118-082315-f9c2a9`, passed T9's turn-log and shared-Store rows and the replay; 3 tests failed on the VM's umask) | Pending | `261118-082957-dff3b5` |
| Linux VM: `consensus` rows | As on macOS | Pending | Pending | `261118-082959-bbd67d` |
| FreeBSD VM: `journal` rows and mutants | As on macOS | Pending | Pending | `261118-083001-9d463a` |
| FreeBSD VM: `consensus` rows | As on macOS | Pending | Pending | `261118-083002-d7b981` |

The first Linux run's T9 rows (job `261118-082315-f9c2a9`) passed before the umask fix; the
reruns above supersede it as the Linux evidence.

## Obsolete, owed, and moved

- **R20's migration rows** (a per-group journal moving into the turn log, T9's migration row, the
  migration marker's decoder): obsolete. `journal:beedebc` removed the migration and the
  `journal.shared` marker; no format reads either. The pre-driver counts for them stay below.
- **T9 on faultfs**: owed to `detent:journal-faultfs`, which has not landed. T9's rows above run
  on `journal::testing`'s disk model, as before.
- **D14's quiescence rows** (R18) and coalesced heartbeats (D12): `detent:consensus-quiescence`
  (A4, idle CPU at 1M groups; A6, idle bytes per group).
- **T2 on several loops' turn logs of one host**: `consensus-sim-loops`. Both simulations here run
  one turn log per node.
- **T6** (shared against per-group journal at 1, 10, 100 and 1,000 groups): moved after the
  release (cluster-release section 4; Xapiand D35: no hardware). The measurement is owed and not
  run.
- **Installs in the cluster simulation**: a node behind another's checkpoint base waits for the
  snapshot transport (C4, `group-install`); the cluster test counts those cases (47 over 24
  seeds in the rerun).

## Results at the pre-driver commits

These are the results as first measured at `journal` `2737613` through `ad92b2e` and `consensus`
`149c4f0`, before the `Driver`, the host layout and the many-group fixes. They are kept as the
earlier record and are not rerun by the table above.

Every row cuts power after each I/O operation of a run, or at random points of a cluster, on
`journal::testing`'s disk (unsynced 512-byte sectors persist or not; namespace changes persist
only after a directory sync; from the cut on the disk stays dead).

| Row | Where | Result |
|---|---|---|
| Turn log alone, corrupted blocks, recycled segments | `journal` `test/turn_log_power_test.cc` | Every crash point; zero acknowledged records lost |
| An earlier epoch's torn turn across a block, then two reopens | `journal` `test/turn_log_test.cc` | Regression for the bug below |
| Shared Stores: batches, checkpoints, reclamation | `journal` `test/shared_store_test.cc` | 971 crash points per seed, zero failures |
| Migration from per-group journals (R20) | same | 1,575 groups migrated across the crash points, zero failures (obsolete since `journal:beedebc`) |
| Workers: single-voter groups proposing, checkpointing, migrating | `consensus` `test/shared_worker_test.cc` | 1,664 crash points, zero failures |
| T2 over shared journals: 3 nodes, 3 groups each, drops and reordering | `consensus` `test/shared_cluster_test.cc` | 24 seeds, 459 power cuts, 2,833 checkpoints, zero failures |

Each power-loss test was checked against mutants of the code it guards: acknowledging a record
before its turn's barrier, dropping migrated batches, and marking a migration before its records
are durable each fail their test.

**Found and fixed.** The cluster simulation found a recovery bug the single-log test never reached
(`journal` `848758b`): a manifest written by an earlier epoch's torn turn, in the header of the
block the turn crossed into, read as acknowledged once a later epoch wrote further, and the next
open declared its group lost. Recovery now counts a manifest only at or before its own epoch's
last kept turn.

**T8.** The shared journal's decoders (turn-log segments, group checkpoint records) have a
libFuzzer target and a seeded replay test (`journal` `a678e44`). The fuzzer found at once that a
checkpoint record shorter than a checksum reached `decode_manifest` unchecked; the decoder now
checks its own length. Fifteen minutes after the fix ran 3.9M inputs clean; the gate's CPU-hour
per decoder was owed then, and is the rerun's row above.
