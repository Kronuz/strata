# basic_index's layout: which mechanism lives where

> Status: step 1 of quest index-restructure: member definitions out of the class body, one header
> per mechanism, moved by cpp-libraries' `tools/outline.py` and proved by `split_check.py --members`.
> Step 2 extracts subobjects where one wins: the file ledger so far (below).

## The shape

`basic_index<Layout>` (`include/strata/core/index.hh`) holds one shard's index: open and recovery,
transactions, the write buffer, flushes, the turn log, commits, checkpoints, storage, merges, reads
and shared readers. Each of these has its own state and invariants, so one class body holding every
definition (5,413 lines) was not one unit. The class body now holds what a reader needs to see the
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
| `index_turns.hh` | the turn log | attach_log, seal_log, join, leave, detach_log, join_log, replay_log, replay_one, flush_point, log_due, stage_checkpoint, await_durable, await_log, await_log_through; a large transaction's chunks (join_chunks, await_sealed) and the sealer's writer count (count_writing) |
| `index_commit.hh` | commit | commit, prepare_commit; the manifest log's edit (figures_now, target_scalars, target_pins, entry_of, edit_now, state_now); commit_files, durable, roll_over, append_edit, reopen_log; commit_empty (a deferred index's first manifest) |
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

## Subobjects (step 2)

### The file ledger (`index_ledger.hh`)

`index_detail::file_ledger`, basic_index's `ledger_`, holds each of the index's files and its
size until the filesystem frees its bytes. A file is in one of three states: on disk (on record,
`files()`), in flight (its delete posted to the executor, `posted()`), or unlinked (removed, its
blocks held until a directory sync makes the removal durable). Each transition is one call:
`track` (a file written, grown or truncated), `rename` (commit's temporary log to its manifest
name), `forget` (removed in place), `post` (a delete posted), `removed` (a posted delete's
removal), `unpost` (a failed or dropped delete, tracked again at the size left), `add_unlinked`
(a failed merge's removed output, never on record) and `synced` (a directory sync). Its invariant,
stated once in the class: `on_disk()` is the sum of the files' sizes, `in_flight()` the sum of the
posted ones', and no name is in both, so `counted()` holds each file's bytes once. That figure is
what `settle_storage` asks the storage permit for, with the reservations beside it.

It leaves basic_index because five members (files_, on_disk_, posted_, inflight_, unlinked_) and
their arithmetic were spread over eleven sites in five headers, each keeping the invariant by
hand; the transitions now name what happens to a file and the reader holds one member. Its tests
are `test/index/ledger_test.cc` (each transition, and a random sequence against a model of each
file's state) with four mutants in `test/index/ledger_mutants.py`, each killed by a named case.

What stays in basic_index: the reclaim queue (reclaim_, queued_) and the pacing figures (debt_,
obsoleted_, truncated_), and the reservations (reserved_, held_, allowance_). These are not a
file's state: they decide when and how much reclaim runs and what the permit holds for writes in
progress, and each is read beside the index's other state (merges, commits, the buffer).

## Candidates not extracted

The other places where state stands apart, and why each stays for now:

1. The turn log's state: its group state is already one object (`logged_`, a `log_state`); the
   flags beside it (join_owed_, attributes_owed_, replaying_) are each read at one or two sites
   in apply, commit and replay, so a class would add a layer without removing a member to track.
2. The manifest log writer (log_, appender_, log_g_, log_end_, log_edits_, rollover_owed_,
   next_generation_, deleted_since_, meta_since_): not attempted. Its state is commit's, written
   inside `durable()` and read by open's recovery and retention, so a cut needs its own design.
3. The retention window (kept_files_, pending_pin_, pending_unpin_, retain_owed_,
   retention_blocked_, inherited_owed_, inherited_below_): not attempted; it reads the ledger's
   files and the views, so it would follow the ledger, not stand beside it.
4. The undo log (undo_, mark_): rollback restores the buffer, the segments, the field table and
   the metadata, so its body is basic_index's whatever holds the vector.
5. The defer_io owed turns (drain_asked_, drain_ran_, promote_owed_, map_asked_): four flags of
   one protocol (cold-tier CT-R22), each set and read at one site.
6. The merge job's classes (merge_file, merge_output, merge_spill, merge_scratch) can leave the
   class body for index_merge.hh: a move, not a subobject, left for a later restructure.

Each later extraction is judged the same way: fewer members a reader must hold in mind, and the
invariant stated in one place.
