# cold tier step 2: the catalog (developer's ledger)

> Status: BUILDING (2026-11-02) item 1 (index prerequisites) built and proved; items 2 to 7 not started.
> Design: `docs/design/cold-tier.md` revision 5 on `design/cold-tier` (strata:26c180f); criteria S1 to S9
> in `docs/status/cold-tier.md` there. Neither file is on this branch.

- Branch / worktree: `phase/cold-tier-step2-item1` from main strata:081b7e7, at
  `/Users/kronuz/code/Kronuz/strata-wt/cold-tier-step2-item1`.
- State: item 1 built; awaiting CI and the lead's landing. Small, no format, no decoder; it touches
  the lease word's storage (`owner.hh`), so the architect's preliminary GO WITH CONDITIONS applies
  (C13: criteria with their options and mutants, listed below).
- Done:
  - strata:8724081 `write_lease::held_here()`, the lease word in the records' shared context,
    `unleased_maps()` per index (I5-N3), `close_ready()` and `warm_quiescent()`; `test/index_prereq_test.cc`.
  - strata:c2a6580 a moved-from lease rebinds to its own word (ASan finding, below).
  - strata:5469e5c the delete-alone case for "nothing uncommitted" (the mutant that drops it survived
    until then); `docs/mechanisms/warm-indexes.md` describes both.
- Next: item 2 (counted IO and caps; CT-R27, CT-R32 first), item 3, and so on, each its own branch.
- Blocked on: nothing. No fuzz target: item 1 adds no decoder or parser.

## What was built, and where it differs from the text

- `load_context` keeps the lease's holder **word**, not a pointer to the `write_lease` (the text:
  "a pointer to the index's `write_lease`"). A record outlives its index (a reader's guard) and an
  index moves, so a pointer to the member would dangle. `write_lease` gains a constructor taking a
  `write_lease::word&` and, moved, keeps a bound word (an unbound one starts free, as before);
  `held_here()` and `held_by_this_thread(word)` compare it, chore bit masked, in every build.
  Cost: 8 bytes per `write_lease` (the word's pointer) and a second indirection on a lease acquire.
- `close_ready()` is a chore under the lease like `demote()`: it throws `index_fenced` on a closed
  index. The warm part is factored as `warm_quiescent()` (buffered record, pending merge, owed bit,
  unsynced segment), which `demote()` now calls.
- `debt_` is zero whenever `reclaim_` is empty (`reclaim_files()` and `reclaim_posted()` both reset
  it), so the debt clause is belt and braces; its mutant is equivalent (C3 below), and the test
  asserts `debt == 0` where `close_ready()` is true.
- Revision 6 says item 4 adds `held_here()`; it exists as of this item (holder word, chore bit masked, every build).

## S1 and S2, rule by rule

Tests are in `test/index_prereq_test.cc`; mutants are exact edits in `.scratch/mutants.py`, built and
run on the `rel` build (RelWithDebInfo `-O2 -g`, asserts and lease checks live) or `nochk` (the same
with `-DSTRATA_LEASE_CHECKS=0`), results in `.scratch/results/*.txt`. Options: `eager` is
`eager_segments`; `lazy` is it off; `defer` is `defer_io`.

| Rule | Test (options) | Mutants (result) |
|---|---|---|
| S1 B's record mapped inside A's chore counts on B | `unleased_per_index` (B lazy, no defer; A eager): a map of B under A's `lease_scope`, under A's `chore_scope`, from a reader thread counts (`unleased_maps()` +1 each, wrapper maps +1); under B's own lease and B's own chore it does not | U1 thread's lease count, today's code (killed); U3 every map counts (killed); U4 none counts (killed); U5 chore bit not masked (killed) |
| S1 a map in `promote()` does not count | `promote_not_counted` (eager, defer): 4 records mapped, count 0 | U3 (killed) |
| S1 runs with lease checks off | the whole file on `nochk` | U2 `held` semantics (true without checks) (killed on `nochk`); U2d the same on `rel` (survives: equivalent, as CT-R38.2 says) |
| S1 per-thread, per-move | `unleased_threads_and_moves` (lazy): 8 reader threads on distinct records, count equals the wrapper's maps; a moved index keeps the word | U6 a moved lease gets its own word (killed) |
| S2 5 files queued: false; after `reclaim()` and its deletes: true; after `close()` the budget is the open indexes' bytes | `queue_clause` (eager; deletes inline, and posted to an `inline_executor`): 6 queued, 7 segment files, `close_ready()` false, the permit above the bytes on disk; after `reclaim()` the queue is empty and, with an executor, still false until the deletes ran and `close_ready()` collected them; then true, `debt` 0, 1 segment file; after `close()` the budget is 0 and one segment file remains (nothing unaccounted). `queue_clause_control`: closing with the queue full leaves the budget at 0 and 7 segment files (the leak the clause prevents) | C1 drop the queue clause (killed); C2 drop the posted clause (killed); C5 no collect (killed); C3 drop the debt clause (survives: equivalent, see above) |
| S2 nothing uncommitted (CT-R38.3) | `uncommitted_clause` (eager): buffered, flushed-unsynced, and a delete alone each refuse; after `commit()` true; `close()` and a reopen has the write (and the delete); `uncommitted_control`: closing without the commit loses the write | C4 drop the clause (killed by the delete-alone case) |
| warm's clauses reach `close_ready()` | `warm_clauses` (eager; defer for the owed turn; merges on a `stepping_executor`): an outstanding `map_owed` turn, a pending merge, a merge output awaiting its sync, each with nothing applied; a closed index throws | C6 no `warm_quiescent()` (killed); C7 pending clause, C8 owed bit, C9 unsynced (killed here and by `warm_test`); C10 no `live()` (killed) |

`refusal_changes_nothing`: a refused `close_ready()` leaves the queue, digest and posted count as they were.

## Evidence

Compiler LLVM (`/usr/local/opt/llvm`), x86_64 macOS; builds in `.scratch/{rel,asan,tsan,nochk}` (CI's
flags: rel `-O2 -g` without NDEBUG; ASan+UBSan `-O1 -fno-sanitize-recover=undefined
-D_GLIBCXX_ASSERTIONS`; TSan `-O1 -g`).

- rel: ctest 62 of 62 (`.scratch/ctest-rel.log`); with `STRATA_EAGER_SEGMENTS=0` 62 of 62
  (`.scratch/ctest-rel-lazy.log`). `index_prereq_test` on `nochk` (lease checks off): ok.
- ASan+UBSan: 62 of 62, zero reports (`.scratch/ctest-asan-final.log`). The first run found a real
  bug in this item: a moved-from index's destructor took the shared lease word after the moved-to
  index freed it (heap-use-after-free in `index_prereq` and `shared_view`); a moved lease now rebinds
  its source to its own word (strata:c2a6580), U7 and the test's moved-from check kill its removal.
- TSan: 62 of 62, zero reports (`.scratch/ctest-tsan.log`).
- Mutants: U1 to U7 and C1, C2, C4 to C10 killed; U2d and C3 survive and are equivalent (the lease
  checks make `held()` exact in debug builds; `debt_` is zero whenever the queue is empty). Results in
  `.scratch/results/`, runner `.scratch/mutants.py`.
- `cpp-libraries/tools/preflight.sh . mac`: mac-hardened ok, mac-asan-ubsan ok (`.scratch/preflight-mac.log`);
  `. linux`: ok (`.scratch/preflight-linux.log`). FreeBSD not run (the lead's call).
- Default path: the existing 61 tests pass unchanged on every build above; `warm_test`'s lease and
  count assertions hold with the per-index count.
- CI: pushed; the lead watches it.
