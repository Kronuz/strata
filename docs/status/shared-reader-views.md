# shared-reader-views

- Design: `docs/design/shared-reader.md` revision 2 (`b1568a7` on `design/shared-reader`), step 1
  items 3 and 5; its review `docs/reviews/shared-reader-design.md` there (the revision 2 review,
  SR-D18 to SR-D21, is in the design worktree's uncommitted copy). The lease is the write-buffer
  design's D2 (`design/write-modes`, `docs/design/write-buffer.md`).
- Branch / worktree: `phase4/shared-reader-views` from main `cee731b`, at
  `/Users/kronuz/code/Kronuz/strata-wt/shared-reader-views`. Lock-free concurrency: a code review
  and the architect's scoped check before it lands.
- State: in progress.
- Done criteria:
  - [ ] K1 Items 3 and 5 as the design states them; decisions cited by ID in comments where needed
    and in commit messages.
  - [ ] K2 SR-D19 and SR-D20 closed: the lease token is the primary owner check; the worker mark is
    a second check; a test per decision fails with its change reverted.
  - [ ] K3 At least one mutant per new ordering or ownership check, killed by a named test.
  - [ ] K4 Clean under TSan, ASan and UBSan locally (Homebrew LLVM); the full suite passes.
  - [ ] K5 CI green on every platform for the branch.
  - [ ] K6 Public API changes listed with their callers migrated.
- Done: nothing yet.
- Next: build, run the suite under the sanitizers, run the mutants.
- Blocked on: nothing.

## Log

### 2026-10-25

- Read the core ledger and review, the design's D1 to D12 and step 1, the revision 2 review
  (SR-D19, SR-D20 in the design worktree), and the write buffer's D2 lease.
- Decisions taken from the review text, for the reviewer to confirm:
  - SR-D20: step 1's lease is the debug-checked holder token only. Every owner duty (each public
    writer call, `reader()`, `publish_through()`, `close()`, `try_finish_close()`, `open()`, the
    destructor) runs in a `lease_scope` on the index's `write_lease`; the outermost scope
    compare-exchanges the token from free (acquire) and stores free at its end (release). A second
    thread's scope while one holds it is reported. Release builds keep no token.
  - SR-D19: `owner_only()` (at bit shares and drops and segment destruction) requires a lease held
    on this thread in debug builds. The worker mark stays as the second check in every build:
    background work holds no lease, so owner state changing on a marked thread with no lease is
    counted. A marked thread that takes a lease (a host request run on the pool) is owner work.
