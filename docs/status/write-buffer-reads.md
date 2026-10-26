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
- Done: (none yet)
- Next: V and view positions; views holding buffers; live get with the docid walk and check;
  retirement by generation; then (C)'s scan (D9).
- Blocked on: nothing.

## Log

- 2026-10-26: started at `9a9957a`. Read revision 4 (D3, D5, D6, D8-D10, D12, changes 2-4), the
  re-check (WB-20 to WB-24), the model, items 2-3's code and ledger, shared-reader D12.
