# write-buffer-reads

- Design: `docs/design/write-buffer.md` on `origin/design/write-modes` at `8cdbe61`, revision 4:
  step 1 item 4 (D3, D5, D6, D8, D9, D10, D12; WB-21 to WB-23). Reviews:
  `docs/reviews/write-buffer-design.md` there (WB-16 to WB-24, C7 to C11); the architect's model
  `.scratch/rev3/docid_model.py` in that worktree. Shared-reader D12 (`origin/design/shared-reader`).
  Scan structure: `origin/perf/lanebuf-rebased` (`bench/kv/proto/lanebuf*.hh`).
- Branch / worktree: `phase4/write-buffer-reads`, stacked on `phase4/write-buffer` at `9a9957a`
  (items 2 and 3, in code review), at `/Users/kronuz/code/Kronuz/strata-wt/write-buffer-reads`.
  Concurrency tier: a code review before it lands.
- State: building.
- Done criteria (the lead's; ticked only with evidence):
  - [ ] I4-1 WB-21: records in D3's byte layout (flags at offset 24, the docid prev field at 28 when
    its flag is set, then the lengths), and every producer that exists at step 1 (the sequencer
    under one writer, T9's replay) reserves the field from the operation's type; a test decodes a
    `replace_docid` record with and without the field.
  - [ ] I4-2 WB-23: docid maps built only under the lease, at `writable_buffer`, `locate_docid`'s
    frozen-buffer build (D8's exception), and the flush's install; readers probe through a const
    find that never builds and keys on the table pointer's acquire load; `docids_built` no longer a
    plain `mutable bool` read by readers.
  - [ ] I4-3 WB-23: grown key-map and docid-map tables retired under D10's generation stamp, never
    freed at once; a first docid-map build past D12's threshold posted to the pool under D12's rule
    (on a reactor thread).
  - [ ] I4-4 WB-23's T8 additions pass under TSan and ASan at one writer with readers on other
    threads: a first build and a growth of the active buffer's docid map while readers run the docid
    check; a frozen buffer's build while a reader holds its pre-freeze view.
  - [ ] I4-5 T8's D6 schedule passes: a `delete_docid` and a key-changing `replace_docid` of a
    keyed segment copy, then a `replace_docid` writing the deleted docid again, each read by both
    keys from another thread at positions on both sides, before the freeze, after it, and across a
    merge install that carries the pending delete.
  - [ ] I4-6 T9's mutants, each killed by its named test: `no_docid_check` (a reader skipping the
    docid check) and `walk_prev_only` (the docid walk following `prev` where the docid prev field
    holds a reference), both by T8's D6 schedule; a docid-map table freed at once, by I4-4 under
    ASan.
  - [ ] I4-7 (C10) WB-22's first-docid-only skip (D6): set before V passes the position, restored
    on a rejection; T8's D6 schedule passes with the skip, and a test shows a reader below the
    position skips the check.
  - [ ] I4-8 T8's model-check schedules for item 4 at one writer with readers on other threads pass
    under TSan and ASan, mirroring the architect's model (0 wrong reads), with every T9 mutant for
    item 4 killed by a named test.
  - [ ] I4-9 The core's suites pass unchanged; flushed bytes identical to the base on the items 2-3
    byte harness.
  - [ ] I4-10 Memory per buffered record and per open index reported against the base (the 12-byte
    position-index entries cost bytes: report them).
  - [ ] I4-11 CI green on the branch (push and move on).
- Done: `b7f8cce` ledger; `d17cad7` V, views holding buffers, reads at r, docid prev, lease-only
  docid builds, retirement by generation; `d0a91af` T8's D6 schedule and model check;
  `8c1f489` the retirement test.
- Next (in order):
  1. (C)'s scan (D9): the 12-byte prefixed position index, the binary counter of runs (ratio 8),
     fresh and compaction jobs applied by the lease holder as splices, the build flag past a
     64-entry tail, the select-32 tail, the prefix-first merge, run sets retired by D10. It
     replaces both scans that exist now (the lease holder's two-run `buffer_view` and a reader's
     private sort, `run_at`): one mechanism.
  2. I4-1's byte layout (below), I4-3's pool rule (D12), I4-9's byte check and I4-10's memory.
- Blocked on: nothing.

## What item 4 builds so far

- **V and view positions (D5, D7).** `visible_` is the accepted prefix, one per accepted
  transaction at one writer; a buffer's `base` is V when it became active and a record's position
  is `base + position`. At each `apply()`'s end (the group's end) `publish_cover()` publishes a
  view when its buffers are not the index's or a map table was replaced (rule 4), then V is
  stored with release in the reader state (rules 1 and 3; verdicts return after it, rule 2).
  Views hold the active and frozen buffers (`shared_ptr`) and `visible_`. With readers, a view
  build creates an empty active buffer, so the next write needs no view of its own (this keeps
  the shared-reader suites' publication counts unchanged). Inverting layouts keep views without
  buffers: search reads at a freeze (D11), and their tables are freed at once.
- **Reads at r.** `reader_handle::live()` loads V, acquires a view, and reads at
  `max(V, view.visible())`; `view::at(r)` is the same for a held view. `get_at`: the key's slot,
  `prev` while newer than r, frozen buffers newest first, then segments with the view's bits
  and D6's docid check against the view's active buffer, skipped below its first docid-only
  supersession. `scan_at`: per buffer, the published position index cut at r by binary search,
  sorted by key, newest first (a private run), merged with the segments; a segment copy selected
  as its key's newest passes the docid check, emitted or not.
- **Records (WB-21).** `slot` gave way to `docid_prev` (no size change); the scan's tie-break
  within a buffer compares references (one lane: they grow with appends). Stage 1 sets the
  docid-prev flag on every `replace_docid` record and fills the field with the docid's slot
  when it differs from `prev`.
- **Docid maps (WB-23).** `docids_built` is gone: the lease holder tests `docids.built()`;
  readers use `probe_docid`, a const find on the table pointer's acquire load. Builds happen in
  `writable_buffer`, in `locate_docid` (a frozen buffer's, D8's exception), and in the flush
  (`newest`); the owner's `digest()` also reaches `newest`.
- **Retirement (D10).** `version_map::grow` keeps the replaced table; `retire_tables()` stamps it
  with the current generation (or frees it when no reader reads buffers) and the replacement
  publishes a view; `free_tables()` frees stamps below every view the registry still names. The
  registry scan runs every 8 groups or past 32 retired objects. `reserve` grows to the final
  size in one step. The position index's directory is fixed at creation (23 blocks of 64 << k
  entries) and its count is published at the group's end.

## Evidence

- ASan+UBSan (Debug -O1, Homebrew clang 23.1.2, macOS x86_64): `write_buffer_reads_test`:
  D6 schedule 18 reads at 9 positions, 0 wrong; model check 200 seeds with held views (52,035
  accepted, 20,077 rejected, 53,396 reads, 0 wrong, 0 verdicts wrong) and 8 seeds with 3 reader
  threads (2,041 accepted, 67,234 reads, 0 wrong); growth under readers 468,002 reads, 0 wrong.
  `write_buffer_test` and `shared_view_test` unchanged and passing.
- Mutants (`.scratch/wbr/mutant.py`, in place, rebuilt, restored; logs `.scratch/wbr/out/`), 12
  of 12 killed on ASan+UBSan: `no_docid_check` and `walk_prev_only` by `d6_schedule` (and the
  model check); `check_ignores_r`, `key_ignores_r`, `index_unpublished` by `d6_schedule`;
  `fdo_never_set` by `d6_schedule` and `first_docid_only`; `fdo_not_restored` by
  `first_docid_only`; `no_dprev` by `d6_schedule`'s field check; `no_cover` by `d6_schedule`
  and the model check; `v_before_cover` by the threaded model check (wrong reads at r 16, 55);
  `table_freed_at_once` by `growth_under_readers` (UBSan: a reader's probe of a freed table,
  "applying non-zero offset to null pointer", rc -6, twice); `tables_freed_early` by
  `tables_wait_for_views`.

## Notes for the designer and the reviewer

- **I4-1's byte layout.** The core's record is a typed struct around the layout's `document`
  (items 2-3); D3's byte layout (flags at 24, the docid prev field at 28, then lengths, key and
  value inline) is a separate change to every record producer and reader, and it is also what
  closes D3's memory gap. Item 4 carries the field and both flags in the typed record; I4-1 stays
  open until the layout lands.
- **Replay** does not exist in the core at step 1 (the journal is step 3), so the only producer
  is stage 1.
- **The empty active buffer** a view build creates (only with readers) is not in the design,
  which creates a buffer at the next write; without it, the first write after every refresh
  publishes a view (D5's rule), which changes `shared_view_test`'s publication counts.
- **Merges** while records are buffered still publish no view (the shared reader's rule): the
  current view keeps its old segments, which are exact at its position, and later records sit
  in the active buffer it holds. A view lists a merge's output once something else publishes.
