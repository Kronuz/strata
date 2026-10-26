# install-own-files

- Branch / worktree: `fix/install-own-files` from main `48984ab`, at
  `/Users/kronuz/code/Kronuz/strata-wt/install-own-files`. Touches file lifetimes and mappings:
  a code review before it lands.
- Bug: `install()` kept the installed segments mapped from the SOURCE index's files (`s->ref` was
  rewritten, the mapping was not). A live source in the same process that merged, released the
  checkpoint and reclaimed truncated files the target still mapped (SIGBUS on a real filesystem; the
  faultfs abort under `STRATA_FAULTFS_MAP=1`). Found by the views code review
  (`docs/reviews/shared-reader-views-code.md`, "the pre-existing hazard").
- Fix: in `install()`, after a segment's copy and live-docs file are written and synced, a mapped
  segment is reopened from the target's own file (`load_segment` with the new ref, the live file read
  with it) and replaces the source-mapped object before the swap into `segs_`. A segment read
  without a mapping already owns its bytes (`owned`), so it keeps them and `s->ref` is rewritten as
  before.
- Extra bytes: none copied. The reopen maps the file just written; it checks the footer, identity and
  live docs but not the whole-segment checksums (`whole=false`). Architect ruling
  (`docs/reviews/install-architect.md`, C1): D5's "verifies each whole" is the checkpoint as read
  from the source, which `install()` does before anything changes; the written copy is checked as
  every written file is (footer, identity and live docs at reopen, regions at first read, whole by
  `heal()`'s re-read, open and the scrubber). A whole-segment verify of the copy would read the page
  cache, not the medium. A copy damaged in the write installs, and surfaces as `corrupt_segment` at
  first read with `heal()` naming it, where main served it silently; unmapped, the verified bytes
  stay in memory and the next open reports it (`install_errors_test`). A host error (EMFILE, ENOMEM)
  from the reopen propagates as itself, unfenced (`fix/install-errors`, C2). One extra open and mmap
  per mapped segment. Unmapped IO reads nothing more.
- Docs: the corrected D5 sentence is in `docs/design/phase-3.md`; the scrubber note (C5) is in
  `docs/design/media-faults.md`, "Next". The install paragraph of `durability-modes.md` (only on
  `origin/design/durability-modes`) says the follower verifies "each whole as it arrives"; that is
  the received bytes and stands, but when that branch lands it should add the same copy-check
  sentence as D5.
- Views: the source-mapped objects are never published. `got.segments` is local until the swap, and
  the replacement happens before it, under the lease; a view taken before the install holds the
  target's old segments (its own files, kept by `retain()`), and the first view after it is published
  by `publish_point()` from the reopened objects. No view can hold a source-mapped object, so none
  outlives the source's reclaim.
- Test: `test/install_own_test.cc` (`install_own`, `install_own_mapped`): the reviewer's probe plus
  deletes in the checkpoint (live-docs path), a guard held across the install, a view after it, the
  source's merge/release/reclaim, then reads, and a reopen of the target.
  - Revert check: with `index.hh` at main, `install_own_mapped` aborts "a file truncated below a live
    mapping" (rc 134); unmapped passes (it never mapped). With the fix both pass.
  - Mutant: the reopen with the live-docs ref cleared (deletes lost) fails four checks in the mapped run.

## State
In review. Full ctest under ASan+UBSan (Homebrew LLVM): 45/45 pass. TSan, the touched tests (index, core_kv, lifecycle, background_merge, shared_view, install_own; read and mapped): 11/11 pass. CI not dispatched.

## Done
`17e8b3c` (the fix and `install_own_test`)

## Next
Code review; the lead dispatches CI.

## Blocked on
Nothing.
