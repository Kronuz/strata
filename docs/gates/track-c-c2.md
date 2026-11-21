# Track C, C2 gate report (rerun at today's mains; T9 fails at `journal:178030b`)

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
- A journal directory refuses group- or other-writable modes, so a rig that extracts group-writable
  files fails its tests. The first Linux run (`261118-082315-f9c2a9`) failed three tests that way
  (`journal_turn_seal_supersede`, `journal_crc_fixture`, `journal_host` `loop_slots`: "journal
  directory requires exclusive ownership"). The cause is the sync, not the VM's session umask: `git
  archive` applies its `tar.umask` (default 002), so the extracted fixtures and directories are 664
  and 775. The rerun at `journal:d90797c` (`261118-115920-3261e1`) still failed `journal_crc_fixture`
  for that reason ("journal file requires exclusive ownership and a singly linked regular inode"),
  and the reruns of this section use `git -c tar.umask=022 archive`.

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
| T9 mutants: acknowledging a record before its turn's barrier (`no-sync-data`), a shared append that does not wait for its barrier (`no-barrier-wait`) | Each is killed by `journal_shared_store_test` | Invalid: the jobs failed in `mutants.py`'s cmake configure on a stale `.scratch/m-*` cache left by my first two jobs (rig, not code; jobs `261118-082445-df9788`, `261118-082447-920b26`, `261118-082955-4b86f4`, `261118-082956-6d198b`). Rerun at `journal:d90797c`, below | Rerun | `261118-115853-7b6023`, `261118-115854-27e504` |
| T2 over shared journals: Workers, single-voter groups proposing, checkpointing, restoring images (`consensus_shared_worker_test`) | Zero failures at every crash point | macOS: 896 crash points, 11,729 checks, 0 failures; 119,810 commands delivered, 22,356 checkpoints published, 2,869 images restored | Pass on macOS | `261118-082353-311736` |
| T2 over shared journals: 3 nodes, 3 groups each, drops, reordering and power cuts (`consensus_shared_cluster_test`) | Zero failures | macOS: 24 seeds, 464 power cuts, 149,689 checks, 0 failures; 135,451 commands applied, 2,545 checkpoints published, 47 catch-ups left to installs | Pass on macOS | `261118-082353-311736` |
| T2 simulation, core (200 seeds x 10,000 steps) and through the `Driver` on a journal `Host` (20 seeds x 3,000 steps) | Every oracle holds | macOS: ok. Core: 1,730 elections, 2,109 restarts, 1,081 repairs, 433 committed memberships. Driver: 129 elections, 110 crashes, 6,142 proposals, 473 reads | Pass on macOS | `261118-082353-311736` |
| Many-group open and close, real-file Workers (`wave_open` at 64, 256 and with a power cut; `consensus_driver_test`; `consensus_worker_test`) | Pass | macOS: 5 of 5; 124 driver checks and 96,142 worker checks, 0 failures | Pass on macOS | `261118-082353-311736` |
| T2's budget (at least 10^7 steps over at least 10,000 seeds) through the `Driver` on journal `Host`s | Every oracle holds | **Failed on the Driver at seed 399.** The core ran all 10,000 seeds x 3,000 steps clean (48,274 elections, 33,985 restarts, 20,467 repairs, 22,072 committed memberships). The Driver simulation stopped at its seed 399: "the healed group did not apply a new entry on every data voter"; node 3 had committed 120 and applied 120, with its application at 0. Seeds 1 to 398 passed. Attribution replays of seed 399 are in the next rows | Fail, under investigation | `261118-083057-825f85` |
| Driver seed 399 replayed alone (`SIM_DRIVER_SEEDS=1 consensus_simulation_test 1 3000 399`), at `journal:4d364f1`, at `consensus` main's own pins (`journal:4272229`) and at `journal:d90797c` | Passes, or fails at every journal | **Fails identically at all three journals**: "the healed group did not apply a new entry on every data voter"; nodes 1 and 2 committed 120, applied 120, application 120; node 3 committed 120, applied 120, application 0 (`application` counts what the application applied since that node's last restart); the same 8 elections, 8 crashes, 306 proposals, 18 commits awaited. The seed is deterministic and independent of the journal's version (`4272229`, `4d364f1`, `d90797c`), so it is a finding in `consensus`, not in `journal`. Detent attributed it (their msg-178) to the simulation's harness: the term-8 leader drops the heal entry at index 126 term 6 and the heal loop never proposes again; node 3 applying 0 is expected (a witness on odd seeds). `consensus:85ed0e6` (2026-11-18) re-proposes the healed entry and keeps 399 as a regression seed: at `5923b6c` seeds 399 and 727 fail (Detent's job `261120-124431-f7bb46`), at `5923b6c` plus `85ed0e6` they pass (`261120-124619-d71c0d`) | Pre-existing harness bug, fixed upstream; rerun at `consensus:8801b99` below | `261118-115910-a06ad3` (journal 4d364f1), `261118-115900-e84a48` (own pins, journal 4272229), `261118-115901-bbd336` (journal d90797c) |
| T2's budget past seed 399 (Driver seeds 400 to 10,000, 3,000 steps each; core seeds 400 to 10,000) | Every oracle holds | Core: 9,601 seeds x 3,000 steps ok (46,389 elections, 32,631 restarts, 19,667 repairs, 21,214 committed memberships). Driver: stopped at seed 727 with the same message as 399, "the healed group did not apply a new entry on every data voter": the same harness bug, fixed by `consensus:85ed0e6` | Fail at `5923b6c` (harness, fixed upstream); rerun at `consensus:8801b99` below | `261120-122853-f50b1e` |
| T2 over shared journals, the cluster test at 10 times its seeds | Zero failures | macOS: 240 seeds x 3,000 steps, 4,521 power cuts, 1,508,840 checks, 0 failures; 1,366,883 commands applied, 26,076 checkpoints published, 417 catch-ups left to installs | Pass on macOS | `261118-083059-454b43` |
| The same `consensus` rows at `consensus` main's own pins (`journal:4272229`) | Pass | macOS, own pins: build ok; Workers 896 crash points and 11,729 checks, 0 failures; cluster 24 seeds, 469 power cuts, 150,145 checks, 0 failures; simulation (200 seeds x 10,000 steps, Driver 20 seeds x 3,000) ok with the counts of the pinned run; wave_open, driver and worker 5 of 5 | Pass on macOS | `261118-083105-13747c` (build), `261118-083106-671b49` (rows) |
| T8: the shared journal's decoders, seeded replay (`journal_fuzz_replay`, `journal_fuzz_framing_replay`) | Pass | macOS: 2 of 2 | Pass on macOS | `261118-082443-2bc92d` |
| T8: libFuzzer (Clang 23, ASan and UBSan) on the turn-log segment decoder and the group checkpoint record, at least one CPU-hour per decoder | No crash, timeout or leak | macOS: five jobs of 1,500 s each from distinct seeds on `journal_fuzz` (journal:4d364f1), all exit 0 with no crash artifact: 1,454,571, 1,708,314, 1,594,622, 1,472,812 and 1,337,596 executions, 7,567,915 in total. `journal_fuzz` takes both decoders through `data[0]` and cannot pin one, so the CPU-hour is of the binary: 125 wall-minutes at about 1,000 to 1,100 executions a second (the 30 s probe ran 34,514 at 1,113 a second, `261118-082230-c83150`), run niced while the host was loaded, so the time is wall-clock, not measured CPU. Each decoder's own share is not measured | Pass on macOS, per-decoder share not measured | `261118-082454-b80095`, `261118-082456-8710c4`, `261118-082457-6ddcb3`, `261118-082459-df4d64`, `261118-082501-0416fc` |
| Linux VM: `journal` rows and mutants (`journal:d90797c`) | As on macOS | Linux 6.8 x86_64, GCC 13.3: turn log 2,042 crash points (678 with a second loss), 276 corrupted blocks, 7,540 checks, 0 failures; `turn_log` 53 checks; shared Store 4,366 crash points, 734,100 checks, 0 failures; seal, bound, placement, host, driver, framing, CRC fixtures and replay all pass; `jpower 200`: 8,647 corrupted blocks, 51,928 crash points (17,247 with a second loss), 196,648 checks, 0 failures; `no-sync-data` killed (827 crash points, 1,285 failures), `no-barrier-wait` killed (63 crash points, 66 failures). History: `261118-082315-f9c2a9` passed the turn-log and shared-Store rows and the replay and failed 3 tests; `261118-082957-dff3b5` exit 75 ("VM linux is running (not ours)"), which skipped its chain; `261118-115920-3261e1` at `journal:d90797c` passed the turn-log (2,042 crash points, 7,540 checks), shared-Store (4,366 crash points, 734,100 checks), seal, bound and placement rows, the replay and both mutants (killed), and failed `journal_crc_fixture` (rig: `git archive`'s `tar.umask` 002, "Commits and rig") | Pass on Linux | `261120-122827-7d5c46` (`261118-115920-3261e1` invalid, rig) |
| Linux VM: `consensus` rows (`consensus:5923b6c` over `journal:d90797c`) | As on macOS | Workers 896 crash points, 11,729 checks, 0 failures; cluster 24 seeds, 464 power cuts, 149,689 checks, 0 failures; simulation 200 x 10,000 and Driver 20 x 3,000 ok; wave_open, driver, worker 5 of 5. The counts equal macOS's | Pass on Linux | `261120-122828-f3c9b9` |
| FreeBSD VM: `journal` rows and mutants (`journal:d90797c`) | As on macOS | FreeBSD 14.4 amd64, clang 19.1.7: turn log 2,040 crash points, 312 corrupted blocks, 7,641 checks, 0 failures; shared Store 4,366 crash points, 734,100 checks, 0 failures; seal through replay all pass; `jpower 200`: 8,227 corrupted blocks, 51,998 crash points, 195,619 checks, 0 failures; both mutants killed (as on Linux) | Pass on FreeBSD | `261120-122831-927ca9` |
| FreeBSD VM: `consensus` rows (`consensus:5923b6c` over `journal:d90797c`) | As on macOS | The Linux row's counts exactly | Pass on FreeBSD | `261120-122833-d9a63d` |

The first Linux run's T9 rows (job `261118-082315-f9c2a9`) passed before the umask fix; the
reruns above supersede it as the Linux evidence.

## T9 on faultfs (`journal:d90797c`)

`journal:d90797c` (pinning `faultlab:a3b71d1`) runs `journal_test` and the power-loss tests on
faultlab's faultfs, and `memory_io.h` is gone. The rows above ran at `journal:4d364f1` on
`journal::testing`'s disk; these rerun at `d90797c`, with `completion:d8b538c` (journal's pin).
The macOS rows are filled; the VM rows follow the rerun table.

| Row | Target | Result | Status | Job |
|---|---|---|---|---|
| Build `journal` at `d90797c`, macOS | Builds | macOS x86_64, Apple clang 17.0.0, RelWithDebInfo with Ninja: 26 of 26 steps, every test and fuzz-replay binary linked | Pass on macOS | `261118-115848-f3b916` |
| `journal` rows on faultfs: turn log, shared Store, seal, bound, placement, host, replay | Zero failures | macOS: `journal_turn_log_power` 2,040 crash points (677 with a second loss in recovery), 312 corrupted blocks, 7,641 checks, 0 failures; `journal_turn_log` 53 checks, 0 failures; `journal_shared_store` 4,366 crash points (2,183 in each of `sync` and `tasks`), 734,100 checks, 0 failures (337,931 batches and 82,522 checkpoints acknowledged, 13,588 checkpoints recovered, 118,934 artifacts reclaimed, 44,726 groups closed and reopened); seal, bound and placement 43 of 43; host, driver, framing and CRC fixtures 4 of 4; seeded replay 2 of 2 (3,615 and 9,318 inputs). The crash-point counts equal the rows at `4d364f1` above; the check counts differ (7,641 against 4,712 and 734,100 against 729,748), and are not read as a change in behavior | Pass on macOS | `261118-115851-dc4a0e` |
| T9: turn log on faultfs at 200 seeds (`journal_turn_log_power_test 200`) | Zero acknowledged records lost, zero groups lost by power loss alone | macOS: 8,227 corrupted blocks; 51,998 crash points (17,266 with a second loss in recovery); 195,619 checks; 0 failures | Pass on macOS | `261118-115852-e136e7` |
| Mutants `no-sync-data` and `no-barrier-wait` on faultfs | Killed by `journal_shared_store_test` | macOS: `no-sync-data` killed (at 827 crash points, 1,285 failures, first "a group without records acknowledged nothing"); `no-barrier-wait` killed (at 64 crash points, 67 failures, "store append has not completed") | Pass on macOS (both killed) | `261118-115853-7b6023`, `261118-115854-27e504` |
| `consensus` main over `journal:d90797c`: build and rows (`consensus` tests may still include `memory_io.h`; a build failure is the result) | Pass | macOS: builds (`consensus:5923b6c` with `completion:d8b538c`; no `memory_io.h` failure); Workers 896 crash points, 11,729 checks, 0 failures (119,810 commands delivered, 22,356 checkpoints published, 2,869 images restored); cluster 24 seeds, 464 power cuts, 149,689 checks, 0 failures; simulation (200 seeds x 10,000 steps; Driver 20 seeds x 3,000) ok with the counts of the run at `journal:4d364f1`; wave_open, driver and worker 5 of 5 (124 driver checks, 96,142 worker checks, 0 failures) | Pass on macOS | `261118-115856-711c4e`, `261118-115858-715ac7` |
| Linux and FreeBSD VMs: the above, with `jpower 200` | Same | Pass on both (the VM rows of the rerun table) | Pass | `261120-122827-7d5c46`, `261120-122828-f3c9b9`, `261120-122831-927ca9`, `261120-122833-d9a63d` |

## At `consensus` main `8801b99` and `journal` main `178030b`

`consensus:8801b99` pins `journal:9e11713` (past `d90797c`) and `completion:d8b538c`, so its rows run
at its own pins with no `--ref` override. `journal` rows run at `journal:178030b`, 8 commits past
that pin (turn-log recovery: double damage, dropped tail turns). Same scripts as above.

**T9 fails at `journal:178030b`.** `journal_turn_log_power_test 200` on faultfs fails on macOS
(`261120-151854-18b807`) and on the FreeBSD VM (`261120-152238-d74166`) with the same seven
failures, at the same seeds and with the same counts (1,036,264 checks). Acknowledged records are
lost in groups recovery keeps, and damage goes unreported:

| Block size | Mode | Seed | Segment, block | Check that failed |
|---|---|---|---|---|
| 512 B | `both` | 91 | 25, 1 | the damage is reported |
| 512 B | `both` | 140 | 24, 1 | the damage is reported |
| 2 KiB | `last` | 145 | 4, 2 | the damage is reported; a group not lost keeps every acknowledged record |
| 2 KiB | `last` | 199 | 4, 1 | the damage is reported; a group not lost keeps every acknowledged record |
| 256 B | `both` | 122 | 42, 6 | a group not lost keeps every acknowledged record |

At `journal:d90797c` the same command passed on macOS (`261118-115852-e136e7`: 51,998 crash points,
8,227 corrupted blocks, 195,619 checks, 0 failures) and on both VMs ("T9 on faultfs" above). The
512 B case at `178030b` has the same 51,998 crash points and 8,227 corrupted blocks as that passing
run, and fails. The Linux VM passes the row at `178030b`, but it draws other cases (its crash-point
and corrupted-block counts differ from macOS's and FreeBSD's, which match each other exactly), so
its pass does not clear the failure. `journal` is Detent's: the failure is reported to them as
their msg-183, where it is being fixed, and this report does not attribute it. T9 at `journal`
main is red until a rerun at their fix passes.

| Row | Platform | Result | Status | Job |
|---|---|---|---|---|
| `journal` build and rows | macOS | Builds. Turn log on faultfs at six block sizes: 512 B 2,040 crash points (677 with a second loss), 312 corrupted blocks, 295 doubly damaged boundaries; 4 KiB 1,150; 2 KiB 1,156; 1,000 B 1,402; 768 B 1,614; 256 B 2,262 (827 corrupted blocks); 40,913 checks, 0 failures; `turn_log` 64 checks; shared Store 4,366 crash points, 734,100 checks, 0 failures (the counts at `d90797c`); seal, bound, placement 43 of 43; host, driver, framing, CRC 4 of 4; replay 2 of 2 | Pass on macOS | `261120-151850-4592f2` (build), `261120-151852-86e99e` |
| T9: `jpower 200` on faultfs | macOS | **Fails: 1,036,264 checks, 7 failures** (the table above). 512 B: 51,998 crash points (17,266 with a second loss), 8,227 corrupted blocks, 2 failures; 4 KiB: 28,784, 1,662 corrupted, 0 failures; 2 KiB: 28,864, 3,414 corrupted, 4 failures; 1,000 B: 35,434, 0 failures; 768 B: 40,926, 0 failures; 256 B: 56,236, 21,102 corrupted, 1 failure. Passed at `journal:d90797c` (`261118-115852-e136e7`) | **Fail** (Detent's msg-183) | `261120-151854-18b807` |
| Mutants `no-sync-data`, `no-barrier-wait` | macOS | Both killed by `journal_shared_store_test`: `no-sync-data` at 827 crash points, 1,285 failures, first "a group without records acknowledged nothing"; `no-barrier-wait` at 64 crash points, 67 failures, "store append has not completed" | Pass on macOS (both killed) | `261120-151857-83dd88`, `261120-151859-75f543` |
| `consensus` build and rows (Workers, cluster, simulation, wave_open, driver, worker) | macOS | Builds at its own pins. Workers 896 crash points, 11,729 checks, 0 failures; cluster 24 seeds, 453 power cuts, 150,052 checks, 0 failures (45 catch-ups left to installs); core simulation 200 x 10,000 ok (1,754 elections, 2,108 restarts, 1,100 repairs, 442 committed memberships); Driver 20 x 3,000 plus 1 regression seed ok, hosts booted with 2, 3 and 4 loops (18 re-partitions), 1,529 elections, 116 crashes, 6,423 proposals; wave_open, driver (153 checks) and worker (96,196 checks) 5 of 5 | Pass on macOS | `261120-151902-b711ce` (build), `261120-151904-2b2fc3` |
| T2's budget: 10,000 seeds x 3,000 steps, core and Driver | macOS | Core: 10,000 seeds ok (48,335 elections, 33,961 restarts, 20,493 repairs, 22,047 committed memberships). Driver: 10,000 seeds ok (3 x 10^7 steps): hosts booted with 2, 3 and 4 loops (8,648 re-partitions), 669,497 elections, 53,374 crashes, 2,980,067 proposals, 522,436 commits awaited, 140,195 checkpoints (41,791 images restored), 335,188 repairs requested and none pending at a seed's end. Seeds 399 and 727, which stopped the Driver at `5923b6c`, pass | Pass on macOS | `261120-151907-369f91` |
| Driver seeds 399 and 727 replayed | macOS | Seed 399 ok (75 elections, 7 crashes, 305 proposals, 58 commits awaited); seed 727 ok with one regression seed beside it (150 elections, 12 crashes, 602 proposals, 112 commits awaited) | Pass on macOS | `261120-151910-c43dad`, `261120-151912-4175b8` |
| Cluster at 10 times its seeds (240 x 3,000) | macOS | 240 seeds, 4,471 power cuts, 1,510,826 checks, 0 failures; 1,370,557 commands applied, 26,260 checkpoints published, 408 catch-ups left to installs | Pass on macOS | `261120-151915-b0da39` |
| `consensus` rows | Linux VM | Linux 6.8 x86_64, GCC 13.3: Workers 896 crash points, 11,729 checks, 0 failures; cluster 24 seeds, 453 power cuts, 150,052 checks, 0 failures; simulation 200 x 10,000 and Driver 20 x 3,000 ok; wave_open, driver (153 checks) and worker (96,196 checks) 5 of 5. The counts equal macOS's | Pass on Linux | `261120-152227-124538` |
| `journal` rows, `jpower 200` and mutants | Linux VM | Turn log 512 B 2,042 crash points (678 with a second loss), 276 corrupted blocks, six block sizes, 40,019 checks, 0 failures; `turn_log` 64 checks; shared Store 4,366 crash points, 734,100 checks, 0 failures; seal, bound, placement 43 of 43; host, driver, framing, CRC 4 of 4; replay 2 of 2. `jpower 200`: 1,046,208 checks, 0 failures (512 B 51,928 crash points and 8,647 corrupted blocks; 256 B 56,350 and 21,952). Both mutants killed (`no-sync-data` 827 crash points, 1,285 failures; `no-barrier-wait` 63, 66). The counts differ from macOS's and FreeBSD's: other cases, so this pass does not clear T9 | Pass on Linux; does not clear T9 | `261120-152229-2096f8` |
| `consensus` rows | FreeBSD VM | FreeBSD 14.4 amd64, clang 19.1.7: the Linux row's counts exactly | Pass on FreeBSD | `261120-152236-dd591f` |
| `journal` rows, `jpower 200` and mutants | FreeBSD VM | Turn log, shared Store, seal through replay: macOS's counts exactly (40,913 and 734,100 checks, 0 failures; 43 of 43, 4 of 4, 2 of 2). **`jpower 200` fails with macOS's seven failures**: the same seeds, block sizes and counts (1,036,264 checks). Both mutants killed (`no-sync-data` 827 crash points, 1,285 failures; `no-barrier-wait` 63, 66) | **Fail** on T9 (Detent's msg-183); the other rows pass | `261120-152238-d74166` |

## Obsolete, owed, and moved

- **R20's migration rows** (a per-group journal moving into the turn log, T9's migration row, the
  migration marker's decoder): obsolete. `journal:beedebc` removed the migration and the
  `journal.shared` marker; no format reads either. The pre-driver counts for them stay below.
- **T9 on faultfs**: `detent:journal-faultfs` landed (`journal:d90797c`); its rows are in the section above. The first rerun table ran on `journal::testing`'s disk model.
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
