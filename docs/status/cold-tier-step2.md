# cold tier step 2: the catalog (developer's ledger), items 1 and 2

> Status: BUILDING (2026-11-02) items 1 (index prerequisites) and 2 (counted IO and caps) built and proved; items 3 to 7 not started. The combined review (`docs/reviews/cold-tier-step2-items1-2.md`) is folded in below.
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
  - strata:fec0f73 S2-B1: a lease or chore scope releases the word it acquired (a move under a scope
    left the shared word held); `move_under_lease` in `test/index_prereq_test.cc`.
- Next: item 3 (the sweep, consuming item 2's budget), and so on, each its own branch.
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

---

# Item 2: counted IO and caps

> Status: BUILDING (2026-11-02) the developer's ledger for `docs/design/cold-tier.md` revision 6, step 2,
> item 2 (D6; CT-R8, CT-R12, CT-R27, CT-R32, CT-R37; criterion S3).

- Design: `docs/design/cold-tier.md` revision 6 on `design/cold-tier` (strata:48ea20f; the file is not on this
  branch). Review: `docs/reviews/cold-tier-design.md`, item 2 GO, C12 (CT-R37's one sentence) folded in below.
- Branch / worktree: `phase/cold-tier-step2-item2` from main strata:80e8f5a, at
  `/Users/kronuz/code/Kronuz/strata-wt/cold-tier-step2-item2`. Independent of item 1 (no rebase onto it).
- State: built and proved; awaiting the lead's CI watch and review triage. The change touches no untrusted
  input and no format, but it is a concurrency primitive (counts under threads); the lead decides the review.
- Done:
  - strata:e17f179 `resource_budget`, `counted_io`, both tests and the kernel-model fixtures.
  - strata:6c4def5 this ledger's S3 table and mutants, `limits.md` rows.
  - strata:bc42a01 `check_core_includes.cmake` admits `sys/resource.h`, `sys/types.h`, `sys/sysctl.h`.
- Next: the lead's CI watch (Linux and FreeBSD jobs); item 3 consumes `resource_budget::add_listener`,
  `over_soft()`, `excess()` and `tick()` from the sweep, and `counted_io::make` from the resolver.
- Blocked on: nothing. No fuzz target: the item adds no decoder or parser (it reads `getrlimit`, two
  sysctls and `/proc/sys` numbers the kernel writes; no untrusted bytes).

### What was built

- `include/strata/core/resource_budget.hh`: `resource_budget` (CT-R27), `resource_reservation`,
  `resource_counts`, `too_many_open`, `resource_system` (the kernel's numbers behind a seam),
  `resource_options` (clock, reserve).
  - Three counts, maps, descriptors, vnodes, each with a derived soft cap, a hard cap and a separate
    `external` cap; the soft cap in force is the smaller of the derived and the external one.
  - `reserve(r, n, catalog, slot)` moves the count with a compare-exchange loop and throws `too_many_open`
    (an `EMFILE` system_error) when the sum would pass the hard cap. The design says `fetch_add`; a
    compare-exchange loop makes the same reservation before the system call but never shows a count
    above the hard cap, so a failing neighbour cannot refuse a call that fits (the stress test samples
    `used <= hard` continuously).
  - Caps (D6's table): Linux counts maps, `vm.max_map_count` less the process's maps at construction,
    soft 50% and hard 90%; elsewhere vnodes, `kern.maxvnodes`, 40% and 60%; descriptors on both, the soft
    `RLIMIT_NOFILE` less the reserve (smaller of 1,024 and a quarter of the limit, or the host's), 75% and
    95%. The constructor raises the soft limit to the hard limit bounded by `/proc/sys/fs/nr_open`
    (Linux) or `kern.maxfilesperproc`; fewer than 64 descriptors at the hard cap fails with `EMFILE`.
  - `tick()` (the sweep's call): re-reads `getrlimit`, re-derives the descriptor caps, clears the
    descriptor `external` cap when the limit changed, and otherwise after 60 s without an external
    failure raises each `external` cap by an eighth of its gap (at least 1) until it is gone.
  - `note_failure(reservation, errno)`: `EMFILE`, `ENFILE`, `ENOMEM` lower the `external` cap of the
    reservation's resource to the count before the reservation, stamp the clock, post to listeners.
  - `add_listener` / `remove_listener`: a soft-cap crossing (and a lowered cap, and a pass that finds the
    count over) posts to every listener on the crossing thread; catalogs will register their sweep's
    wake. `over_soft()` and `excess(r)` are what a pass reads.
- `include/strata/core/counted_io.hh`: `counted_io`, a `kronuz::journal::IO` over an inner IO.
  - `counted_io::make(counts, factory)` reserves the directory descriptor and vnode (CT-R32), then runs the
    factory that opens the directory; both go back if it throws and when the IO is destroyed.
  - Every `open_existing`, `create_exclusive`, `open_reclaim_candidate`, file-mode owner lock and
    `scan_directory` reserves a descriptor (files and owner locks a vnode too) before the call; `map()`
    reserves a map before `mmap`. A mapping shares its file's vnode reservation, so a file whose
    descriptor was closed while a mapping lives still counts one vnode until the mapping goes.
  - A system_error from a counted call goes to `note_failure` and on unchanged; any exception after a
    successful call destroys the kernel object and the reservation together (members are ordered so the
    object goes first).
  - Not forwarded: `native_directory_handle()` and the reusable-control calls, so a backend that opens
    files past the wrapper is not admitted (CT-R32's last sentence).
  - `sync_files`, `sync_files_and_directory` and `order_files_and_directory` unwrap the counted files.
- Tests: `test/resource_budget_test.cc` (the budget alone, a fake kernel and clock, one real `getrlimit`
  case), `test/counted_io_test.cc` with `test/counted_hooks.hh` (a kernel model over faultfs: a
  descriptor and map table with limits, chosen and random errnos, a hook inside `map()`, and a
  `new` that fails the n-th allocation of a thread).
- `docs/design/limits.md` gains the cold-tier rows.

### S3, criterion by criterion

Tests are `resource_budget_test` (RB) and `counted_io_test` (CI); each name is a function in the file.
Mutants are exact edits in `.scratch/mutants.py` (text-replace, build, run, restore), built in the
RelWithDebInfo build (`-O2 -g`, asserts live).

| S3 clause | Test | Mutant that dies |
|---|---|---|
| each cap at its hard value: the reservation that passes it throws `too_many_open`, no kernel call, nothing marked or fenced | RB `hard_caps`; CI `caps_by_io` (kernel call counters unchanged, `refused == 0`), `index_at_the_caps` (a read at each cap is a host error, `needs_heal` false, the read works after) | M12 hard cap off by one |
| reserve before the call (a barrier between two maps at the cap less one) | CI `reserve_before_call` (the kernel's map peak is 1) | M1 reserve after the system call |
| a failed call returns its reservation | CI `kernel_failures`, `lifetimes` | M10 reservation kept when the call fails |
| an exception after a successful call returns the reservation and the kernel object | CI `allocation_failures` (every allocation of every call fails in turn) | M3 kernel object leaks, M3b reservation kept |
| a lost install race returns its reservation and the kernel object goes | CI `lost_install_race` (8 threads in `map()` together, 3 rounds, an index over the counted IO), `lifetimes` | M2 mapping drops without returning |
| counts equal the kernel's at quiescent points (T7) | CI `fx::agrees` throughout, `stress` at 6 barriers, `posix_counts` (PosixIO against the real descriptor table, and `/proc/self/maps` on Linux) | M2, M14 |
| two catalogs on one budget never pass its hard caps | CI `two_catalogs` (kernel table = cap + 5, `refused == 0`), `stress` (two catalogs, a sampler asserting `used <= hard` and the kernel's peaks) ; RB `stress` | M4 per-catalog caps (dies in RB `stress`, CI `caps_by_io`) |
| the descriptor cap is reached by opens alone (directory descriptors) | CI `directory_descriptor` (182 IOs, the 183rd refused before the factory runs) | M5 directory descriptor unreserved |
| soft limit 256 under a higher hard one is raised, caps from the raised value | RB `raise` (injected), `real_raise` (real `setrlimit`/`getrlimit`) | M6 cap from the ceiling without the raise, M6b no raise |
| a lowered `RLIMIT_NOFILE` is picked up at the next pass | RB `real_raise` (300), `external_recovery` | M7 caps fixed at construction |
| an `EMFILE` below the cap lowers the soft cap | RB `external_pressure`; CI `kernel_failures`, `external_user` | M11 does not lower, M17 EIO counts |
| the lowered cap survives the passes (no second `EMFILE` while the user holds its descriptors) | RB `external_pressure`; CI `external_user` (20 passes, one kernel refusal in all) | M8 re-derivation overwrites |
| a changed limit clears it | RB `external_recovery` | M19 |
| 60 s quiet then an eighth of the gap per pass | RB `external_recovery` (injected clock: 59 s no change, then each pass exactly an eighth, at least 1); CI `external_user` | M9 never recovers, M20 rounds to nothing |
| (not in S3) the lowered cap is the count before the refused reservation | RB `external_pressure` | M21 |
| a crossing posts to every listener | RB `crossing` | M18 |

Mutant results are in the table at the end.

### Mutants

All 21 edits in `.scratch/mutants.py` die (RelWithDebInfo build; the cases that fail are named). The
budget-only mutants build `resource_budget_test` alone; the IO's build both tests.

| Mutant | Dies in |
|---|---|
| M1 reserve after the system call (map) | CI `caps_by_io`, `kernel_failures`, `reserve_before_call`, `index_at_the_caps`, `stress` |
| M2 mapping drops without returning its reservation | CI `lifetimes` |
| M3 kernel object leaks when the wrapper throws | CI `allocation_failures` (and `stress`) |
| M3b reservation kept when the wrapper throws | CI `kernel_failures`, `stress` |
| M4 per-catalog caps | RB `stress`; CI `caps_by_io` |
| M5 directory descriptor unreserved | CI `directory_descriptor`, `lifetimes`, `caps_by_io` |
| M6 cap from the per-process ceiling without the raise | RB `raise`, `real_raise` |
| M6b no raise | RB `raise`, `real_raise` |
| M7 caps fixed at construction | RB `real_raise`, `external_recovery` |
| M8 re-derivation overwrites the lowered cap | RB `external_pressure`, `external_recovery` |
| M9 a lowered cap never recovers | RB `external_recovery` |
| M10 reservation kept when the system call fails | CI `directory_descriptor`, `lifetimes`, `kernel_failures`, `external_user`, `stress` |
| M11 an EMFILE does not lower the soft cap | RB `external_pressure`, `external_recovery` |
| M12 hard cap off by one | RB `hard_caps` (aborts) |
| M14 a mapping does not share its file's vnode | CI `lifetimes`, `posix_counts` |
| M15 close releases though the backend has no close | CI `lifetimes` |
| M17 EIO counts as external pressure | RB `external_pressure` |
| M18 a crossing posts nothing | RB `crossing` |
| M19 a changed limit does not clear the external cap | RB `external_recovery` |
| M20 the raise rounds an eighth down to nothing | RB `external_recovery` |
| M21 the lowered cap is the count after the failed reservation | RB `external_pressure`, `external_recovery` |

M1 to M4 ran against both tests in the first pass; M3b's first mutant (an empty reservation in the
wrapper) also broke the success path, so it was rewritten to leak only when the allocation throws.

### Sanitizers

Both new tests, RelWithDebInfo with asserts, Homebrew LLVM clang++ on macOS x86_64:
- ASan+UBSan (`-fsanitize=address,undefined -fno-sanitize-recover=undefined`, `detect_leaks=1`): zero reports.
- TSan (`halt_on_error=1`): zero reports; the stress cases run eight threads over two catalogs with
  random kernel errors and allocation failures.
- Preflight (`cpp-libraries/tools/preflight.sh`, full suite 63 of 63): mac-hardened ok, mac-asan-ubsan ok,
  linux (gate VM, `-D_GLIBCXX_ASSERTIONS`) ok. FreeBSD not run (CI).
- Counts: stress cases run 77,815 and 72,000 operations; 16,696 are refused at a cap, 11,890 fail in the
  kernel model, 6,413 hit an injected allocation failure.
