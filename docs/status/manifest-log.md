> Status: CURRENT (2026-11-04) the manifest-log design's ledger; supersedes nothing.

# manifest-log

- Design: `docs/design/manifest-log.md` (draft); reviews: none yet. The architect reviews it at
  max before anything is built (durability format).
- Branch / worktree: `design/manifest-log` at `~/code/Kronuz/strata-wt/manifest-log`, from
  `main` `2769928`.
- State: designing
- Done criteria (set by the lead; contributors tick, never edit):
  - none yet
- Done: nothing committed yet.
- Doing: reading the commit, manifest, retention and open paths (`include/strata/core/index.hh`,
  `files.hh`, `fields.hh`), durability-modes D1, Track C D11 and journal's turn log at
  `journal:bfad97f`, small-indexes and cold-tier, limits, RocksDB 11.11's MANIFEST code; writing
  the file-work floor probe `.scratch/mlog/mlog_probe.c` (runs under `quiet strict mlog`).
- Next: run the probe; draft the design; set State to "designing (ready for preliminary review)".
- Blocked on: nothing.
- Findings folded in: none.
- Design vs code: nothing built.
- Last verified: nothing built or run yet.

## Log

- 2026-11-04: worktree created from `main` `2769928`; reading started; probe written.
