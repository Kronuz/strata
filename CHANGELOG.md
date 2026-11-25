# Changelog

Notable changes to `strata`, newest first. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); the library has no releases, so
entries are dated.

## 2026-11-25

### Added

- `strata::group::open()` refuses a group whose recovered log ends before its index's durable head (P_H > E: the journal put back from an earlier copy, a copy of a running data root, a plain-mode power cut on macOS, or a journal that holds no group over an index with state). It throws `group_fenced` naming both positions ("the journal is older than the index") before anything on disk changes; before, the replay skipped every new entry at or below P_H as already applied. E is consensus's `Driver::GroupState::last`, so the consensus pin moves to 6d710cb. The open's three refusals and what each leaves on disk: [docs/mechanisms/group-open.md](docs/mechanisms/group-open.md) (`group_journal_behind_test`).
