# Track C, C2 gate report (in progress)

C2 (the shared journal, D11) as built in `journal` and `consensus` on `main`; the progress
table in `docs/design/track-c.md` lists the commits. This report collects the gate rows
measured so far and what is still owed.

## What is built

- **`RFTW0001`** (`journal` `2737613`, `915d1e5`): segments of blocks with per-block
  manifests, LevelDB-style fragments bound to segment and epoch, an open record per epoch,
  the torn-tail rule, damage costed per group, copy-forward reclamation, recycled segments,
  and staged turns.
- **The turn driver and the shared Store** (`journal` `ad92b2e`): `TurnDriver` writes the
  records the groups' Stores stage as one turn with one data barrier. A `journal::Store`
  with the shared backend stages its batches and checkpoint records there; artifacts stay
  files of the group, sealed before their checkpoint record is staged (Condition 25), and
  superseded ones are reclaimed only after the record's barrier.
- **R20's migration** (`journal` `ad92b2e`, `consensus` `149c4f0`): a per-group journal
  moves into the turn log; `journal.shared` marks every shared group's directory.
- **The `Worker` on the turn log** (`consensus` `149c4f0`); consensus's own `TurnJournal` is
  removed, so one implementation persists groups.

## T9: power loss

Every row cuts power after each I/O operation of a run, or at random points of a cluster,
on `journal::testing`'s disk (unsynced 512-byte sectors persist or not; namespace changes
persist only after a directory sync; from the cut on the disk stays dead).

| Row | Where | Result |
|---|---|---|
| Turn log alone, corrupted blocks, recycled segments | `journal` `test/turn_log_power_test.cc` | Every crash point; zero acknowledged records lost |
| An earlier epoch's torn turn across a block, then two reopens | `journal` `test/turn_log_test.cc` | Regression for the bug below |
| Shared Stores: batches, checkpoints, reclamation | `journal` `test/shared_store_test.cc` | 971 crash points per seed, zero failures |
| Migration from per-group journals (R20) | same | 1,575 groups migrated across the crash points, zero failures |
| Workers: single-voter groups proposing, checkpointing, migrating | `consensus` `test/shared_worker_test.cc` | 1,664 crash points, zero failures |
| T2 over shared journals: 3 nodes, 3 groups each, drops and reordering | `consensus` `test/shared_cluster_test.cc` | 24 seeds, 459 power cuts, 2,833 checkpoints, zero failures |

Each power-loss test was checked against mutants of the code it guards: acknowledging a
record before its turn's barrier, dropping migrated batches, and marking a migration before
its records are durable each fail their test.

**Found and fixed.** The cluster simulation found a recovery bug the single-log test never
reached (`journal` `848758b`): a manifest written by an earlier epoch's torn turn, in the
header of the block the turn crossed into, read as acknowledged once a later epoch wrote
further, and the next open declared its group lost. Recovery now counts a manifest only at
or before its own epoch's last kept turn.

## T8: robustness

- The shared journal's decoders (turn-log segments, group checkpoint records) have a
  libFuzzer target and a seeded replay test (`journal` `a678e44`). The fuzzer found at once
  that a checkpoint record shorter than a checksum reached `decode_manifest` unchecked;
  the decoder now checks its own length. Fifteen minutes after the fix ran 3.9M inputs
  clean; the gate's CPU-hour per decoder is owed.
- The migration marker carries no bytes to decode; its presence is the decision, tested
  in T9's migration rows.

## Still owed

- **T6**: the measured comparison of the shared journal with the per-group journal at 1,
  10, 100, and 1,000 groups (device flushes per turn, commit p99, CPU per command, idle
  memory, recovery time, reclamation bound, bytes written per byte appended), on the Linux
  gate VM, macOS, and FreeBSD. Both backends now exist behind one Store interface, so T6
  runs them side by side. The arm64 host is retired; its rows wait for a replacement.
- **T8**: one CPU-hour per decoder at the gate's commit, and the coverage targets.
- **D14's quiescence rows** (R18) and the coalesced heartbeats (D12).
- **Installs in the cluster simulation**: a node behind another's checkpoint base waits for
  the snapshot transport (C4); the simulation counts those cases (41 over 24 seeds).
