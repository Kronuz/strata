> Status: CURRENT (2026-11-09) how `basic_index` in `include/strata/core/index.hh` commits,
> recovers, retains and checks through the manifest log (step 3 of `docs/design/manifest-log.md`
> revision 1: D4 to D8, C5, D42). The file: `manifest-log-file.md`. Records and replay:
> `manifest-log-codec.md`.

# The index on the manifest log

An index directory holds segment files and `manifest-<g>` logs, nothing else: the live docs, the
metadata and the field table are sections of the log's records. `parse_name` knows segments, logs,
`manifest-<g>.tmp`, spills and scratch files.

## What the index holds

- `m_`, the head: the `log_state` the newest log replays to. It owns the segments' entries with
  their deleted sets (`log_deleted`, D42), the metadata, the field table, the pins.
- `cur_`, the live scalars: last docid, next segment, schema version, configured bits, applied
  position. Transactions change these; the head changes only at a commit.
- The change list since the head: `deleted_since_` (segment, ordinal) and `meta_since_` (keys).
  An edit is built from it and from the segments born since the head, so building one costs time
  in proportion to the change, not to the state.
- The log: `log_` and its `log_appender`, `log_g_`, the end offset and the edit count, and
  `rollover_owed_`.

## Commit: `commit_files`

1. Unless a rollover is owed, the edit is built (`edit_now`) and held to the reader's own checks
   (`check_edit`; a refusal fences, since it is a writer bug). `check_edit` also gives the
   post-edit shape, so D7's bounds are tested against the state the log would hold.
2. Within the bounds, `append_edit` orders the commit's new segment files and the directory (only
   when it wrote any), writes the record in one write, syncs it, and replays the edit into `m_`. A
   failed write was truncated back by the appender, so `ENOSPC` is a `space_error` and the index
   stays as it was. A truncate-back or a sync that fails, with any exception (an allocation
   failure included), fences; so does any failure after the sync: the edit is durable and the head
   must follow (SR4's writer half).
3. Past the bounds, or when owed, `roll_over` writes the state as a snapshot to
   `manifest-<g>.tmp`, orders it with the new segments, renames it and syncs the directory (D5).
   An unforced rollover that meets `ENOSPC` on its temporary appends the edit instead: the bounds
   are soft. A forced one (first commit, dirty open, `open(..., to)`, install) throws. Any failure
   from the ordering point to the directory sync fences.
4. The per-segment figures, the change list and the pins reset, and the new head's file names are
   cached for retention.

`commit_allowance()` holds the largest edit the pending changes make, framed, and a snapshot only
while a rollover is owed (C5).

## Open: `recover`

1. The directory scan records each file's size; the highest log is chosen, or with `to` the
   highest whose snapshot is at or below `to`.
2. `read_log` replays it (to `to` when given), retrying an `EIO` once. `m_`, `cur_`, the metadata
   and the field table come from the state; the segments are checked against their entries by
   name and size and loaded lazily.
3. A log that does not end at its last whole record (a torn tail, records past `to`) is never
   appended to: `rollover_owed_`, and the next generation skips one past the torn record's (D6.7).
4. With `to` below the newest generation the open commits at once (a rollover), and `fallback()`
   reports `to`, `newest` (the newest generation the logs hold whole, or the first damaged one) and
   `discarded = newest - to`.
5. Otherwise the directory is synced, then the log is opened for appending and `sync_data`'d once:
   the open's only syncs. The directory sync makes a rollover's rename durable when a fault or a
   process kill stopped it before its own directory sync, so a later commit that writes no file
   never follows a head a power loss can revert.
   The open removes leftover `.tmp` files and runs no retention (D6.6).

## Retention: `retain`

Kept generations are the head, its pins, and the `retained_predecessors` generations before it.
Each names its segments and the log holding it (the greatest snapshot at or below it), read once
and cached (`kept_files`). Every other strata file on record, logs included, joins the reclaim
queue with no order between logs and data files and no directory sync (D8). A pinned generation
whose log cannot be read blocks the round (`retention_blocked()`); a predecessor that no log
reaches whole (a generation a torn tail skipped) names nothing and does not block.

## Install and `check()`

`install` reads the source's generation with `index_detail::read_state` (the same function as
`open(..., to)`) and owes a rollover. `check()` replays the head log from disk and compares the
state with `m_`, then checks the field table's chain and each deleted set against its segment.

## Proof

`test/index_test.cc` (install and open refuse resealed states; retention blocks on an unreadable
pin and skips a removed one), `test/retention_test.cc` (predecessors as edits of one log, the
fallback report), `test/fields_test.cc` (the table in the log, a torn tail rolls over),
`test/defer_io_test.cc` (an unreadable pinned log at a commit), `test/reclaim_test.cc`,
`test/directory_test.cc` and `test/lifecycle_test.cc` (crash points over the log; T4 case 1: every
mutation of each rollover and each old log's removal on commit-heavy workloads),
`test/apply_atomic_test.cc` (an allocation failing at each point of a commit: unfenced it changed
nothing, fenced it reopens to the old or new state), `test/digest_streams.cc` with
`test/digest_vs_ref.py` (T6: the same streams' verdicts and digests under this tree's headers and
another commit's).
