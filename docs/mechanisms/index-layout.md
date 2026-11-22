# basic_index's layout: which mechanism lives where

> Status: step 1 of quest index-restructure (2026-11-21): member definitions out of the class
> body, one header per mechanism. Step 2 (subobjects) has not started; its candidates are below.

## The shape

`basic_index<Layout>` (`include/strata/core/index.hh`) holds one shard's index: open and recovery,
transactions, the write buffer, flushes, the turn log, commits, checkpoints, storage, merges, reads
and shared readers. Each of these has its own state and invariants, so one class body holding every
definition (5,309 lines) was not one unit. The class body now holds what a reader needs to see the
whole: the data members, the nested types, and every member's declaration with its comment, in the
order the sections (`// --- writer ---`, ...) give them. A definition that fits on one line stays
in the class beside its declaration (accessors, forwarding calls).

Every other definition sits in the header of its mechanism, as a `template <class Layout>`
definition of `basic_index<Layout>::f`. `index.hh` stays the header callers include: it holds the
class and includes the mechanism headers after it. Each mechanism header includes `index.hh`, so it
compiles alone; `#pragma once` breaks the cycle.

## Which mechanism lives where

| Header | Mechanism | Members |
|---|---|---|
| `index_open.hh` | lifecycle and recovery | create, create_deferred, open, move assignment, the destructor, close, try_finish_close; recovery (recover, lock_directory, check_directory_trust, clear_uncreated, load_files and the segment loaders, read_checked); heal and needs_heal; warm (demote, close_ready, promote, warm_quiescent) |
| `index_apply.hh` | transactions | apply, abandon and evaluate (stage 1); the undo log (rollback, remove_copy, push_record, reserve_undo, note_docid_only, bump_last_docid, writable_buffer); the writer calls' plumbing (check_in, run_chores, guarded, live, fence) |
| `index_buffer.hh` | the write buffer | lookups of its copies (locate_docid, lookup_key, find_key_in, the docid map), memory accounting (charge, needs_flush, buffer_limit, map_bytes, the inline-record predicates), fits, lane_fits, invert_pending, clear_buffer |
| `index_flush.hh` | freeze and flush | refresh, freeze, apply_pending, flush, flush_active, flush_documents, install_flushed; the segment files they write (write_unsynced, streamed_segment, map_output); segment identity (new_identity, seed_identities) |
| `index_turns.hh` | the turn log | attach_log, seal_log, join, leave, detach_log, join_log, replay_log, flush_point, stage_checkpoint, await_durable, await_log, await_log_through |
| `index_commit.hh` | commit | commit, prepare_commit; the manifest log's edit (figures_now, target_scalars, target_pins, entry_of, edit_now, state_now); commit_files, durable, roll_over, append_edit, reopen_log |
| `index_checkpoint.hh` | checkpoints and retention | checkpoint, release, install; retain, kept_files, names_of |
| `index_storage.hh` | storage accounting and reclamation | reclaim, reclaim_for_space, the posted deletes (run_delete, collect_deletes, requeue_delete, finish_deletes, wait_deletes), reclaim_posted, reclaim_files, file tracking (track, remove_file, forget_file, sync_dir, sync_inherited), commit_allowance, drain, settle_storage, estimate_bytes |
| `index_merge.hh` | background merges | merge, force_merge, force_merge_deletes, maybe_merge, wait_merges, run_merges, start_merges, schedule, run_merge, publish_merges, install_merge, carry_pending, mark_damaged, end_merges, abandon_merges |
| `index_read.hh` | reads | segments, find_key, get, find_docid, has_docid, scan and scan_keys; the scan runs (maintain_runs, start_runs, take_runs); reads at a position (get_at, scan_at, part_at, key_at, docid_at); the segment reads every read shares (get_segments, scan_segments); digest, digest_of, check |
| `index_publish.hh` | shared readers | reader, publish_through, views_held; the publication (build_view, publish_view, publish_point, publish_cover, retire_current, retire_tables, free_tables, free_views) |

Why these cuts: they follow the class's own section comments and the designs behind them
(standalone-turn-log, manifest-log, shared-reader, write-buffer), so each header is the code one
design governs. Two calls a reader might expect elsewhere:

- `heal` lives with recovery: it reloads segments through the same loaders open uses.
- `install` lives with checkpoints: it adopts another index's checkpoint, and retention decides
  which checkpoints stay.

## What stays in the class, and why

Step 1 moves definitions only: every mechanism is still basic_index's, sharing its data members.
The nested types stay in the class body (about 1,100 lines: open_segment, record, buffer, view,
reading, reader_handle, snapshot, and the merge job's file, output, spill and scratch classes),
because the data members and the declarations name them.

Step 2's candidates, where state and invariants stand apart from the rest:

1. The storage ledger: on_disk_, reserved_, unlinked_, inflight_, posted_, obsoleted_, debt_,
   truncated_, held_, allowance_, files_, reclaim_, queued_ and the delete channel. Its invariant
   (every file's bytes counted once, as on disk, posted, unlinked or freed) is stated across
   index_storage.hh today.
2. The turn log's state: logged_, visible_, join_owed_, attributes_owed_, replaying_, the group
   attachment. Its invariants (V never passes the synced sequence, the join waits for its
   attribute's edit) belong to standalone-turn-log alone.
3. The manifest log writer: log_, appender_, log_g_, log_end_, log_edits_, rollover_owed_,
   next_generation_, deleted_since_, meta_since_ (the change list since the head).
4. The retention window: kept_files_, pending_pin_, pending_unpin_, retain_owed_,
   retention_blocked_, inherited_owed_, inherited_below_.
5. The undo log: undo_ and mark_, with rollback's order.
6. The defer_io owed turns: drain_asked_, drain_ran_, promote_owed_, map_asked_ (cold-tier
   CT-R22's one answer per transaction).
7. The merge job's classes (merge_file, merge_output, merge_spill, merge_scratch), which only
   run_merge uses, can leave the class body for index_merge.hh.

Each is judged on maintenance when step 2 runs: fewer members a reader must hold in mind, and the
invariant stated in one place.
