# Changelog

Notable changes to `strata`, newest first. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); the library has no releases, so
entries are dated.

## 2026-11-25

### Added

- `strata::group::open()` refuses a group whose recovered log ends before its index's durable head (P_H > E: the journal put back from an earlier copy, a copy of a running data root, a plain-mode power cut on macOS, or a journal that holds no group over an index with state). It throws `group_fenced` naming both positions ("the journal is older than the index") before anything on disk changes; before, the replay skipped every new entry at or below P_H as already applied. E is consensus's `Driver::GroupState::last`, so the consensus pin moves to 6d710cb. The open's three refusals and what each leaves on disk: [docs/mechanisms/group-open.md](docs/mechanisms/group-open.md) (`group_journal_behind_test`).

### Changed

- An install lands the learner's index on its bundle (docs/design/journal-binding.md D5). The leader pins strata's generation at the bundle itself, not at its head: a checkpoint captures, syncs and commits in one chore item and, while a peer awaits an install, that commit is the pin. A leader whose strata is past its latest bundle starts a pinning checkpoint at SnapshotNeeded, and a publication consensus refuses `Busy` during a flight retries after the flight's release. `make_pin` refuses a bundle strata is not at, and the learner's `check_stage` refuses a staged head past its bundle. A learner restarted right after an install no longer finds its index past its log's end (`group_install_bundle_test`, `group_install_test stage-head`).

### Fixed

- A replica repaired by a requested reinstall can checkpoint again: its checkpoints now name consensus's application epoch, which the repair advances (`Driver::reinstall`'s `RepairCompleted`). Before, every later checkpoint was refused `InvalidCheckpoint`, so the replica's log never compacted, and once it led, D5's pinning checkpoints never published (`simulation_group_corruption`, mutant `repair-epoch-dropped`).
