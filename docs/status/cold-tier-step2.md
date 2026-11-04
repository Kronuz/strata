# cold tier step 2: the catalog (developer's ledger), items 1 to 5

> Status: BUILDING (2026-11-02) items 1 (index prerequisites) and 2 (counted IO and caps) are on main; item 3 (slots, table, pins) is built and proved on `phase/cold-tier-step2-item3` (its section is last); items 4 to 7 not started. The combined review (`docs/reviews/cold-tier-step2-items1-2.md`) is folded in below.
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

## Review follow-ups (`docs/reviews/cold-tier-step2-items1-2.md`)

| Finding | Fix | Test | Mutant (dies in) |
|---|---|---|---|
| S2-B1 (item 1) a move under a lease or chore scope left the shared word held | `lease_scope` and `chore_scope` keep the `word*` they acquired (`owner.hh`); class comment updated | `index_prereq_test` `move_under_lease` (lease scope and chore scope; another thread takes the lease and commits; zero ownership breaks); also on the `nochk` build | the fix reverted: `move_under_lease` fails (lease) and hangs (chore, waiting on the stuck chore bit); a scope acquiring the lease's own word: killed in `unleased_per_index` and others |
| S2-N1 external cap has no floor | `note_failure` lowers it to `max(count, min_indexes)` | `resource_budget_test` `external_floor` (all three resources); `counted_io_test` `kernel_failures` and `external_user` now expect the floor (`external_user` leaves the catalog 70 descriptors, above it) | floor removed: `external_floor` |
| S2-N2 counts' lifetime | contract in the `resource_budget.hh` file comment; `~resource_counts` asserts its counts are 0 (debug) | `counts_lifetime` (a forked child destroys counts with a reservation held: SIGABRT) | assert removed: `counts_lifetime` |
| S2-N3 `RLIM_INFINITY` | `percent()` divides first (exact, no overflow) | `unlimited_limits` against a 128-bit oracle: soft below hard, maps and vnodes too | `x * p / 100`: `unlimited_limits` |
| S2-N4 `tick()` against `note_failure()` | `tick()` raises with a compare-exchange (a lowering since its load wins) and reads the clock after the load; `note_failure` stamps the time before it lowers | `tick_against_failure`: the clock hook lands a failure between the load and the raise; then 3,000 rounds of a failure thread against `tick()` | compare-exchange replaced by a store: the hook case |

The two probes `lease_probe` 1, 2, 4 and `budget_probe` 2, 3, 6 are covered by those tests, not rerun as binaries.

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


---

# Item 3: slots, table, pins and the word's protocol

> Design: `docs/design/cold-tier.md` revision 6 on `design/cold-tier` (strata:48ea20f, models strata:2f02600); D4, D5;
> CT-R13, CT-R15, CT-R29, CT-R30, CT-R31 (the use bit), CT-R34, CT-R35, CT-R41; criterion S4.

- Branch / worktree: `phase/cold-tier-step2-item3` from main strata:8d7467b, at
  `/Users/kronuz/code/Kronuz/strata-wt/cold-tier-step2-item3`.
- State: built and proved; the review's fixes (I3-B1, the stress harness's race, C16, the text notes) are in the last section; awaiting the lead's CI watch and landing. It adds a concurrency primitive and no
  decoder, parser, format or untrusted input, so no security review; the lead decides the code review.
- Done:
  - strata:73b54cf `catalog.hh` (slots, table, pins, the word's protocol, the wake and sweep stubs), `basic_index::lease()`, the first tests.
  - strata:0613c35 T6's schedules, the threaded stress, the limits rows, the mechanism note, this section.
  - strata:bd6a815 the `open.unlocked` hook and the deterministic tests for the two windows (a pin released between a
    transition's load and its store; a retry that joins the new open).
  - the last commit on the branch is this ledger's preflight line.
- Next: item 4 replaces `run_wake()` and adds the `wake_gate` check to the `owed` row (backoff, the bound, the timer
  repost, `chore_scope` before the final store); item 5 replaces `begin_demote()`/`begin_close()` with the sweep
  (timer, policy, reclaim, commit chore, `shutdown()`, `closing()` retries). Both plug in at the names below.
- Blocked on: nothing. Left out, named: CT-R15's debug assertion at `live()` (needs an index-level hook, not a
  catalog one) and "closing slots' IOs leaked" (item 5, it owns the `closing()`-stays case). No fuzz target: no
  decoder or parser.

## What was built, and where it differs from the text

- `include/strata/core/catalog.hh`: `strata::catalog<Index, H>`. It is templated on the index type, as core names no
  search type; `basic_index` gains `lease()` (the catalog's chores take a `chore_scope`; a chore pin asks
  `lease().held_here()`, which item 1 built). Slots live in 256-slot chunks never freed while the catalog lives, 4M
  slots at most (`docs/design/limits.md`); the table is 64 shards of `hash_slots` under one mutex each, held for a
  probe, an insert/erase, or a transition's final compare-exchange, never across I/O.
- The word is `pins:24 | state:4 | freq:2 | gen:34`; every writer is a compare-exchange or a fetch-op:
  request/chore pin by key (a use sets the frequency in whatever state it meets, saturating at 3), pin by handle
  (state `open`, `gen` matching, no mutex), the miss, unpin (time store, then `fetch_sub`), open end (opened or
  `failed`, `gen` + 1, the waiters failed and their pins removed in the same store), the wake's end, the
  demotion's end, the close's end (freed, or reopened in place, `gen` + 1), the sweep's two compare-exchanges, the
  policy's decrement.
- `pin_request()`, `pin_chore()`, `repin()` return an awaiter; a pin that meets a transition counts itself in the
  word, joins the slot's waiter list in the same critical section and suspends. A delivery before `await_suspend` makes
  it decline, so a pin handed over early never suspends. Waiters resume after the mutex is released, posted to the
  scheduler (`task::current()`) they suspended on; one with none is resumed inline after the unlock.
  `try_pin()` starts the open or the wake as a pin would, holds nothing, and answers `would_wait`.
- Opens are single-flighted by `opening`. A failed open leaves `{0, failed}` with the error and a retry time that
  doubles from 10 ms to 10 s per consecutive failure (`too_many_open` takes the same path); pins fail at once until
  then, and the first pin after it reopens. The first success resets the count. A second key on a held directory
  fails its open once with the lock's `EWOULDBLOCK` and meets the negative entry after that.
- Stubs behind the names the later items replace: `run_wake()` (promote under `chore_scope`, an immediate repost
  when the lease is busy, no bound or backoff, so no counts or `wake_gate`), `begin_demote()` and `begin_close()` with
  their chores (`run_demote`, `run_close`; the maps-zero and `close_ready()` tests are in the chore, after the
  compare-exchange, as "the sweep reads fields only after its compare-exchange" says).
- Choices the text leaves open, for the architect:
  1. A demotion that ends `owed` while a request pin waits (it registered during `demoting`) ends at `waking`, not
     `owed`: the table's two rows (`demoting` to `owed`, then `owed` to `waking`) composed in one store. A refused
     close with a request waiter does the same (`closing` to `waking`). Chore waiters stay waiting through that wake
     (CT-R40: no hold begins after the wake). With no request waiter, a demotion ends at `owed` and hands the chore
     waiters their pins.
  2. A retry from `failed` and the failure that follows each bump `gen`, so a failed round is two bumps; a
     reopen in place is one.
  3. The policy's decrement is a compare-exchange loop, not a `fetch_sub`: a decrement of zero would borrow from `gen`.
  4. `try_pin` on a miss inserts `{0, opening}`: the open completes into a slot nobody holds, which the sweep may take.
  5. `H` is made by `host::make_payload(key)` at open (default `H{}`); the index by `host::open_index` (default
     `Index::open`); the IO by `host::resolve(key, io_counts)`, which a host builds with `counted_io::make`.
  6. `hash_slots` takes `index_detail::key_hash(key)`; the shard is its low six bits.

## S4, clause by clause

Tests are in `test/catalog_test.cc` (inline_executor, single-threaded, deterministic) and
`test/catalog_stress_test.cc` (threads); fixtures `test/catalog_world.hh` (faultfs directories, the item 2 kernel model
and a real `resource_budget`, counted IO). Mutants are exact edits in `.scratch/mutants.py` (`mutants2.py` builds each as
a one-TU compile ahead of the real include dir, CI's flags `-O2 -g`, asserts live; `BASE`, the unedited control, passes).

| S4 clause | Test | Mutants that die (how) |
|---|---|---|
| T6 schedules over every writer of the word, slot reuse, a reopen in place, a step between each load and its compare-exchange | `catalog_test` `schedules`: 300 seeds x 160 steps; the `before_cas` hook runs another actor's lock-free compare-exchange (an unpin, a sweep's demote or close, a decay) before 11 sites' own, 1/3 of the time; the test thread drives pins by key and by handle, both kinds, the sweep, the clock, resolver and kernel failures; at every quiescent point: the word counts exactly the pins held, no waiter is left, only stable states, the flag agrees with `owed`, `unleased_maps()` zero, `gen` never down, the table has one entry per live slot | P1 (plain store of the wake's end), W1 (a waiter's pin not counted: the unpin assert, `pins == held`), S1 (sweep takes a pinned slot: a request pin handed out on a demoted slot) |
| a pin on a transition suspends and gets its pin from the transition's store | `single_flight` (8 requests, one open, all 8 counted when the first resumes), `wake_hands_pins`, `demotion_end` (3 cases), `failed_open_fails_waiters`, `wake_fails`, `resume_posted` (resumptions queue behind work already posted) | W1; R2 (resumed inline); D1 (no `waking` at a demotion's end); D2 (a failed wake keeps the request pins); D3 (a failed wake fails the chore waiters); C1 (a close with pinners strands them) |
| a pin released between a transition's load and its final store is not lost | `unpin_during_wake_end` (the hook releases a chore pin between the wake end's load and its store) | P1 (the loaded word is stored back: the pin returns) |
| a pin that retries inside the window after an open's final store joins the new open | `retry_joins_new_transition` (hook `open.unlocked`) | X1 (waiters collected outside the mutex: the old delivery takes the new waiter) |
| waiters resume outside the mutex | `resume_outside_mutex` (a scheduler-less waiter whose continuation takes every shard's mutex) | R1 (delivered under the mutex: deadlock, killed by the timeout) |
| `try_pin` starts the open or the wake and answers `would_wait` | `try_pin_starts`, `chore_pin_on_waking` | T1, T2 (a pin counted and never handed out); O1 (a request pin on `owed` pins as on `open`); O2 (a chore pin waits on `owed`); O3 (a chore pin enters `waking`); O4 (no `held_here()` exception) |
| a re-pin by handle across a reopen in place falls back to the key | `close_and_reopen` (gen + 1 once; the stale handle counts and succeeds by key; the freed slot's gen + 1 again and the miss's + 1) | G1 (a reopen keeps `gen`, slot_model B1), G2 (no `gen` check) |
| a failed open fails pins at once until its retry time, then reopens | `failed_open_backs_off` (10, 20, 40 ms, then reset by a success; `resolves` unchanged while it fails fast) | B1 (no backoff) |
| a second key on a held directory fails its open once | `second_key_same_directory` (`EWOULDBLOCK`, one resolve, the negative entry answers next) | L1 (the conflict retried) |
| the use bit (CT-R31, CT-R41) | `use_bit` (miss pin no use, saturation at 3, handle re-pin no use, chore pin no use, decay), `demotion_end` (a waiter's use set at registration) | F1 (only `open` sets it), F2 (no saturation: it carries into the state), F3 (the miss's pin is a use) |
| time stored before the decrement | `unpin_order` (the hook between the two sees the stamp) | U1 |
| cold only from warm with a maps count of zero | `close_and_reopen` (a hot close ends at `open`, `gen` unchanged) | S2 |
| sweep reads fields only after its compare-exchange; a state stored or waiters collected outside the shard mutex | `catalog_stress_test` under TSan | X2 (the open's final compare-exchange outside the mutex: data race), S3 (the sweep reads the index before its compare-exchange: data race), both reported by TSan in the stress |

slot_model's mutants: A1 (revision 4's waiters re-pin after the store) is not W1: W1 hands a pin the word never counted
(`pins == held` and the unpin assert catch it), and it does not re-pin. The starvation A1 names (a sweep taking the slot
between the store and the re-pin) is guarded by S1 (the sweep takes a pinned slot) and the T6 invariant `pins == held`.
A2 is the shipped protocol; B1 is G1/G2; B2 is the shipped `gen` bump. wake_model's four rules are item 4's (S5).

## T6 and the TSan stress against the slot model

The model's state changes (`slot_model.py` A2: `open` with the flag clear to `demoting`, to `open` with the flag set, which
the catalog names `owed`, to `waking`, to `open`; B2's `gen` bump), extended with the open, failure, close and reopen
rows of D4's table, are the 18 pairs of `allowed_edge()` in `catalog_test.cc` (free 0, opening 1, open 2, owed 3, waking 4,
demoting 5, closing 6, failed 7). Both tests read the catalog's per-edge counters (a counter at each state-changing
compare-exchange) and check them against the set:

- No state change outside the 18 was made in any run, and the schedules made all 18. 300 seeds x 160 steps (48,000
  steps; the same numbers on the rel, ASan+UBSan builds, as the schedule is seeded): 13,705 pins granted, 4,742 delivered a
  failure; 0>1 1,540, 1>2 1,315, 1>7 1,467, 2>5 319, 2>6 442, 3>4 107, 3>6 44, 4>2 68, 4>3 73, 5>2 168, 5>3 120, 5>4 31, 6>0 46,
  6>1 32, 6>2 403, 6>3 2, 6>4 3, 7>1 1,210.
- Pins met each of opening (2,999), owed (107, which start the wake), waking (58), demoting (131), closing (275) and failed (1,210
  fast failures); 3,417 re-pins by handle, 95 stale handles, 1,216 retries, 141 wakes. Another actor's compare-exchange was
  injected before the word's own at all 12 sites (pin.key 3,407, pin.handle 1,183, pin.miss 483, open.end 937, open.unlocked 923,
  transition.end 243, wake.end 44, close.end 17, sweep.demote 58, sweep.close 91, policy.decay 383, unpin 3,776).
- No stuck waiter: every actor of a schedule is finished after the executor drains (4,561 pins waited in the 300 seeds), and the
  stress runs every pin under a 60 s limit (`mt_sched::run_until`), which aborts with the slot table dumped; none fired.
- Stress (`catalog_stress_test`, 24 keys, WorkPool of 3, pinner threads on `mt_sched`, a sweeper at full speed, resolver and
  kernel failures flipped, chore pins writing under the host's turn), the whole run per build:
  - TSan, 4 threads x 1,500 ops: 5,950 pins plain and 5,855 under `defer_io` (390 wakes, 1,106 pins that waited), zero reports.
  - Release, 6 threads x 20,000 ops: 119,871 and 118,525 pins (392 wakes; 101,000 reads and 17,000 chore holds each), zero stuck.
  - ASan+UBSan, 4 x 400: clean.
  At rest every word is `{0, free/failed}`, and the kernel model's descriptors and maps and the catalog's counts are zero (I6).

## Evidence

Homebrew LLVM clang++ on macOS x86_64. Builds in `.scratch/{rel,relc,asan,tsan}`.
- `rel` (`-O2 -g -DNDEBUG`): schedules and stress as above. ASan+UBSan (`-O1`, `_GLIBCXX_ASSERTIONS`, `-fno-sanitize-recover=undefined`):
  `catalog_test` 300 seeds ok, `catalog_stress_test` ok. TSan (`-O1`): `catalog_test` 100 seeds ok, `catalog_stress_test` ok, 0 reports.
- Mutants: 26 of 26 edits killed in `catalog_test` on CI's flags (U1 W1 P1 G1 G2 B1 L1 R1 R2 F1 F2 F3 O1 O2 O3 O4 D1 D2 D3 C1 C2 T1
  T2 S1 S2 X1), and X2, S3 in the TSan stress; the unedited control `BASE` passes in both. R1 dies by the test's timeout (the
  deadlock it makes). First run of P1 and X1 survived the random schedules once the schedule's random stream changed
  (X1 only died at 600 seeds), so each has a deterministic test now (`unpin_during_wake_end`, `retry_joins_new_transition`).
  Results: `.scratch/mutants2-relc.txt`, `.scratch/mutants2-tsan.txt`; the runner `.scratch/mutants.py` + `mutants2.py`.
- Two bugs the tests found in the first build, both mine: a slot's `resource_counts` was declared after the IO whose reservations
  return into it (destroyed first: the `~resource_counts` assert), and the test's coroutine lambdas were temporaries (ASan
  stack-use-after-scope); `actor` now owns its closure.
- Preflight, the full suite, staged by `.scratch/pf.sh` (the same builds and flags as `cpp-libraries/tools/preflight.sh`, in holds
  under 30 minutes): mac-hardened ok (66 of 66), mac-asan-ubsan ok (66 of 66), linux gate VM `-D_GLIBCXX_ASSERTIONS` ok (67 of 67),
  FreeBSD gate VM ok (66 of 66). It ran on strata:bd6a815's tree.
- CI: pushed; the lead watches it.


## Fix round for the review (`docs/reviews/cold-tier-step2-item3.md`, strata:4772d63, approve with conditions)

- State: I3-B1, the linux-tsan failure, C16 and the text notes are fixed on the branch; C14, C15, C17 and the
  rest of the carried list are item 4's, item 5's and the host's. Proof is below; preflight and CI are named at the end.
- Done:
  - strata:f407bd0 the stress harness's schedulers (the linux-tsan race).
  - strata:9476224 I3-B1 and C16 in `catalog.hh`, `pin_cap` and `slot_cost`, `limits.md:62`, the mechanism note.
  - this ledger.

### I3-B1: the pin count stops at the field

- `word::full(w)` (`pins == max_pins`); `acquire_impl` throws `too_many_open` before any compare-exchange, whatever the
  state it meets, and `try_fast` throws once `gen` and `open` match. The debug assert is gone: the check is the mechanism.
  `limits.md:62` names it.
- Test `pin_cap`: a slot with `set_pins_for_test(cap)` refuses a request pin, a handle re-pin, a chore pin and a
  `try_pin` with `too_many_open`, and the word is bit-for-bit unchanged (`handle_pins` still 0); at `cap - 1` the
  handle re-pin is granted and fills the field; the pins return to 1 and release to zero.
- Fails with the fix reverted (NDEBUG, so the wrap is real): B1a (no check in `acquire_impl`) and B1b (none in
  `try_fast`) each die at `catalog_test.cc:844`, `CHECK(by_key && by_handle && by_chore)`; B1c (`> max_pins`, one too high) the
  same; B1d (refuses at `max_pins - 1`) aborts on the granted-at-`cap - 1` pin. With asserts live (CI's flags) B1a to B1c die
  earlier, on the unpin assert, the pin count having wrapped.
- The architect's `aba` probe still prints `add_pin at max_pins: pins=0`: it calls the raw helper, which is unchanged and
  caller-guarded (its comment says so). The two callers are what the test covers.

### The linux-tsan race (run 41539265981): the harness

- Cause: `mt_sched` lived on each pinner thread's stack. A chore's `deliver` posts the resumed pin and then
  `post()` returns through `pthread_cond_signal`, after the unlock; the pinner could pick the handle up, finish and
  destroy the scheduler in that window (TSan: `~mt_sched` at `catalog_stress_test.cc:44` against `post`, :51). It is the
  harness, not the catalog: `deliver` reads the waiter's `next`, `h` and `sched` before it hands the pin over and touches
  nothing of the waiter after, and `~catalog` and `wait_chores()` wait for every chore (`group_->wait()`), so no delivery
  outlives the catalog's users. The contract is now in the mechanism note: a scheduler outlives the pins that await on it
  and the chores that may resume one.
- Reproduced locally under TSan before the fix: the unmodified stress did not (0 of 3 at 1,500 x 4, 0 of 40 at 30 x 4 in the
  stock 20 ms-poll harness), because the window is the instructions between an unlock and a signal. A copy of the test
  with 300 us between the unlock and the signal and the consumer polling every 20 us hit it 30 of 30 runs at 3 ops x 4
  threads (a SEGV in `pthread_cond_signal` on the destroyed condition variable, reported in `run_open`'s deliver; the CI
  report came through a wake chore). The same copy of the fixed test: 0 of 30. Files in `.scratch/repro/`.
- Fix: `stress()` owns a `std::deque<mt_sched>` (one per pinner, one for the final committer) declared before the world.

### C16: the index behind a pointer

| | Before | After |
|---|---|---|
| `sizeof(slot)` (H = `payload{int}`) | 1,656 B (160 - 8 + the 1,504 B `optional<kv_index>`; SZ1's inline buffer measures the same) | 160 B |
| a free or cold slot | 1,656 B kept for the catalog's life | 160 B |
| an open slot | 1,656 B | 160 B + 1,496 B for the index, one allocation beside the open's other work |

- `index` is `std::unique_ptr<Index>`: `make_unique` in `run_open`, `reset()` in the close and in the open's failure. D4's "about 192
  B plus `H`" holds (160 B); the first miss's 256-slot chunk is about 41 KB, not 420 KB, and a burst to 157,900 non-cold
  indexes leaves about 25 MB of slots behind it, not 250 MB.
- `slot_cost` prints the size, `static_assert(slot_bytes() <= 192)`, and checks a closed slot holds no index. SZ1 (an inline
  index buffer in the slot) fails the build on the `static_assert` (`1656 <= 192`); SZ2 (the close keeps the pointer) dies at
  `catalog_test.cc:871`.
- The index's address is now stable per open and not per slot; `index_of()` returns it. `Index` needs to be move-constructible
  (it already was, for `optional::emplace`).

### Text notes (I3-N8)

1. Design revision 7 (the two composed edges in D4's table and I9's wording): the architect's list is the lead's to apply on
   `design/cold-tier`; this branch carries the mechanism note (below) and does not edit the design.
2. S4's mutant mapping: corrected in the "S4, clause by clause" paragraph on slot_model's mutants (A1 is not W1).
3. Options each test runs with (C13). All catalog tests build the index with `keyed = true`, `eager_segments = true`,
   `auto_merge = false` (the `world` defaults; no case turns `eager_segments` off). `defer_io` and segments per directory:
   - `defer_io = false`, 2 segments: `single_flight`, `demotion_end`'s second case, `close_and_reopen`, `failed_open_backs_off`,
     `failed_open_fails_waiters`, `second_key_same_directory`, `resume_posted`, `resume_outside_mutex`, `retry_joins_new_transition`;
     1 segment: `use_bit`, `unpin_order`, `pin_cap`, `slot_cost`.
   - `defer_io = true`, 2 segments: `try_pin_starts`, `wake_hands_pins`, `wake_fails`, `demotion_end`'s first two cases,
     `chore_pin_on_waking`, `unpin_during_wake_end`.
   - `schedules`: 5 directories of 2 segments, `defer_io` on even seeds; the stress: 24 directories of 2 segments, once with
     each `defer_io`.
4. The composed edges are in `docs/mechanisms/catalog.md` ("Two composed edges").
5. `wake_reposts` is 0 in every run again (catalog_test, both stresses, the architect's hot-key stress): the stub's repost path
   is never taken. Item 4's S5 must make it non-zero (carried).

### Carried, not done here

- C14 (item 4): the wake's gate as one function, called at the `owed` row and both composed edges; S5's case and mutant. I3-N1.
- C15 (item 5): `shutdown()`, `~catalog`'s debug assert and leak per D5, slot fields reset on reuse, the close chore's
  `closing()` retries and its caught throws. I3-N2, I3-N3, I3-N6. The architect's `teardown_pin` probe still aborts (a pin
  released after the catalog is gone); that is N2, unchanged.
- C17 (before X-POOL): CT-R15's debug pin check at `live()`. I3-N7.
- C10 (item 6) gains I3-N5: a `try_pin` that is a use sets no frequency on its compare-exchange.

### Proof of the fixes

Homebrew LLVM 23.1.2 clang++ on macOS x86_64, builds in `.scratch/f-{rel,tsan,asan}` (separate TSan; ASan+UBSan with
`-fno-sanitize-recover=all`); outputs in `.scratch/fix-out/`.
- `ctest`, Release (`-O3 -DNDEBUG`), the whole suite: 66 of 66 passed (275 s).
- `catalog_test 300`: ok under TSan, ASan+UBSan and Release; the schedule is unchanged (48,000 steps, 13,705 pins granted,
  4,742 failed, the 18 state changes with the same counts), as `pin_cap` and `slot_cost` add cases ahead of it.
- `catalog_stress_test`: TSan 1,500 x 4 and 8,000 x 6 (the architect's sizes), ASan+UBSan 1,500 x 4: ok, zero reports.
- The architect's probes (`.scratch/review-item3/`, kept; rerun in `.scratch/fix-out/probes/`), TSan and ASan: `lost_wakeup`,
  `close_pin`, `aba`, `backoff`, `sync_use`, `closing_stuck`, `teardown_waiter` ok under both; `teardown_pin` aborts as before
  (I3-N2, item 5); `hotkey_stress` TSan 3,000 x 8 and ASan 20,000 x 8 ok, zero reports. His `mut.py` D1 X1 P1 C1 AM1 to AM4:
  8 of 8 killed.
- Mutants (`.scratch/mutants.py` + `mutants2.py`, one-TU builds with CI's flags `-O2 -g`, asserts live): all 28 earlier
  edits die again on the new code (U1 W1 P1 G1 G2 B1 L1 R1 R2 F1 F2 F3 O1 O2 O3 O4 D1 D2 D3 C1 C2 T1 T2 S1 S2 X1 in
  `catalog_test`; X2 and S3 by TSan in the stress; the control `BASE` passes), and the six new ones die:

  | Mutant | Dies in |
  |---|---|
  | B1a no cap check in `acquire_impl` | `pin_cap`: `CHECK(by_key && by_handle && by_chore)` (NDEBUG); on the unpin assert with asserts live |
  | B1b no cap check in `try_fast` | the same line (the handle re-pin wraps) |
  | B1c the cap one too high (`>`) | the same line |
  | B1d the cap one too low (`>= max_pins - 1`) | `pin_cap` (the pin granted at `cap - 1` throws) |
  | SZ1 the index inline in the slot again | build: `static_assert` in `slot_cost` (`1656 <= 192`) |
  | SZ2 the close keeps the index allocated | `catalog_test.cc:871` (`slot_cost`) |

  Results: `.scratch/mutants2-relc.txt`, `.scratch/mutants2-tsan.txt`, `.scratch/mut-ndebug.out` (B1a to B1d under `-DNDEBUG`).
- The linux-tsan race: reproduced and gone as under "The linux-tsan race" above.
- Preflight on strata:9476224's tree (`.scratch/pf3.sh`, the staged form of `cpp-libraries/tools/preflight.sh`, holds under 30
  minutes): mac-hardened ok (66 of 66), mac-asan-ubsan ok (66 of 66), linux gate VM `-D_GLIBCXX_ASSERTIONS` ok (67 of 67),
  FreeBSD gate VM ok (66 of 66).
- CI: pushed; the lead watches it. Last known (before this round): run 41539265981, linux-tsan red on the harness race above.


---

# Item 4: the wake, chore pins and the bound

> Design: `docs/design/cold-tier.md` revision 6 on `design/cold-tier` (strata:2f02600); D4, D5, I9, I10;
> CT-R23, CT-R28, CT-R33, CT-R38.5, CT-R38.6, CT-R40, I5-N2; criteria S5 and S6 (copied below, never edited here).

- Branch / worktree: `phase/cold-tier-step2-item4` from `phase/cold-tier-step2-item3` (strata:b1615dd), at
  `/Users/kronuz/code/Kronuz/strata-wt/cold-tier-step2-item4`. If item 3's review changes the catalog, its fixes merge in.
- State: fix round after the review's REJECT (I4-B1 and the notes) built and proved on the host, see "Fix round" at the end of this
  section; main merged in (strata:be5c5af). Before it: built and proved on the host (Release, TSan, ASan+UBSan, 31 of 31 mutants dead). It
  changes a concurrency protocol (the wake's end, the bound, the timer), no decoder, parser or format, so no security review.
  Item 3's fix round (I3-B1's pin cap, C16's slot layout) had not landed when this stopped: merge `phase/cold-tier-step2-item3`
  again when it does, then rerun `catalog_wake_test`, `catalog_test` and `catalog_stress_test`.
- Done:
  - strata:38858ab the wake, the backoff, the bound close, the timer, `catalog_wake_test`.
  - strata:be6236b C14 (I3-N1): `wake_gate()` shared by the `owed` row and `end_at()`; the schedules and the stress extended.
  - strata:1acee84, strata:f5b0ccf coverage in the schedules; the destructor's timer wait; the slot starts over after a bound close.
  - strata:c0fbfec the mechanism note (`docs/mechanisms/catalog.md`) and the limits rows.
- Next: item 5 replaces `begin_demote()`/`begin_close()` with the sweep, which decides when to call `begin_bound_close()`
  (after `probe_at`), erases negative entries, and owns `shutdown()` (it must also wait for the armed timers, as `~catalog` does: `timers_mu_`, `timers_cv_`, `timers_`).
- Blocked on: nothing. Left out, named: the sweep (item 5). No fuzz target: no decoder or parser.
- Scope note: S6's close at the bound is "the sweep" in D5, which is item 5; item 4 builds the primitive (`begin_bound_close()`
  and its chore) and tests it by calling it. `begin_demote()`, `begin_close()` and `run_demote()` are unchanged.

## Done criteria (copied from `docs/status/cold-tier.md` on `design/cold-tier`)

- **S5** (item 4). Under `defer_io`, indexes demoted between pins, and T6 schedules that put the
  demotion between a pinner's load and its compare-exchange: no request pin is handed out before
  `promote()` completes, `unleased_maps()` stays zero, no `apply` answers `map_owed`; a chore pin
  on `owed` commits without a wake, and a wake behind its call reposts. CT-R40, each rule a case
  on `inline_executor` with an injected timer, the `wake_model` property as its oracle:
  1. A stream of chore pins does not starve the wake: callers that pin, lease and release in a
     loop from before `waking` on; the request gets its pin after the holds that began before the
     wake, and every chore pin taken after `waking` resumes from the wake's final compare-exchange.
  2. A chore pin whose thread holds the lease enters `waking` (a nested pin inside a commit
     call); the wake completes after the hold ends.
  3. A repost goes through the timer: while a chore call holds the lease, the reposts follow the
     1 ms to 64 ms doubling, `wake_reposts` counts them, and the retry count stays zero.
  4. The wake ends `chore_scope` before its final compare-exchange: a resumed request's
     `lease_scope` on `inline_executor` acquires from free (its `outer_` is true), not nested.
  5. A wake's `too_many_open` (the budget's hard cap reached) fails the request pins, sets the
     backoff, and the next request pin before its retry time fails at once with no chore posted.
  Mutants: hand out the pin before the wake ends; pin `owed` as `open` (CT-R28, CT-R38.5); a chore
  pin that waits on `owed` (CT-R33's commit waits for a request); a wake that waits on the lease
  instead of reposting; a chore pin that enters `waking` (case 1 starves); no `held_here()`
  exception (case 2 never completes); an immediate repost (case 3's count passes its bound); the
  final compare-exchange inside `chore_scope` (case 4 nests); `too_many_open` without the backoff
  (case 5 posts a wake per pin). Each is a switch of `wake_model.py`, where it fails the same
  property (CT-R40).
- **S6** (item 4). Persistent faultfs `EMFILE` in `promote()`, on the host's timer injected so the
  backoff runs in milliseconds: request pins in backoff fail with `wake_failed` and post no
  chore; chore pins still enter; at the 8th failure a committed slot closes and the next pin
  reports the reopen's error; an uncommitted slot at the bound is committed by the host's commit
  chore, then closed, and its data reopens; a commit that fails leaves it `owed` with a 10 s probe;
  `too_many_open` does not count and takes the backoff (S5 case 5). Mutants: no backoff (a chore per pin); no close at the bound;
  close an uncommitted slot at the bound without the commit (the reopen lacks the write)
  (CT-R33, CT-R38.6).

## Plan, and how S6 meets the sweep

- The wake: `run_wake(idx, delay)` takes `chore_scope`; a busy lease reposts through the host's timer (1 ms doubling to 64 ms,
  `wake_reposts`, outside the bound); `promote()`; the scope ends; `finish_wake()` writes the failure fields under the shard
  mutex before its compare-exchange and collects the waiters there.
- The `owed` row's gate: a request pin that meets `owed` with a backoff still running fails at once with `wake_failed` and posts
  nothing; a chore pin never looks at it.
- The bound: `wake_bound` (8) counted failures (`too_many_open` takes the backoff but is not counted). S6's close at the bound
  is the sweep's pass (D5), which is item 5; item 4 builds the primitive the pass calls, `begin_bound_close()` (a new
  compare-exchange beside `begin_close()`, `{0, owed}` to `{0, closing}` when the count is at the bound), with its chore: the
  host's commit chore if anything is uncommitted, then `close()`; a failed commit ends at `owed` with a 10 s probe time
  (`probe_at`, read by the sweep) and `failing()`. S6's three bound clauses are tested by calling it directly.

## What was built, and where it differs from the text

- `include/strata/core/catalog.hh`: `run_wake(idx, delay)`, `finish_wake()`, `wake_gate()`, `begin_bound_close()`, the close
  chore's bound branch, `after()` (the timer), `wake_failed`; `host` gains `timer`, `commit` and `wake_bound`. `basic_index` gains
  `uncommitted()` (the applied-since-commit count, a chore's read under the lease).
- The wake takes `chore_scope` and never waits: a busy lease posts the wake again through `host::timer` after 1 ms, doubling to
  64 ms (`wake_reposts`; not a failure). The scope ends before the final compare-exchange, which hands the waiters their pins;
  chore pins that meet `waking` wait for it unless their thread holds the lease (item 3's rows, now tested against a second
  thread). The timer's callback only posts. `~catalog` waits for armed timers and for the group until both are quiet.
- A failed wake writes, under the shard mutex before its compare-exchange: the count (only when it was not `too_many_open`), the
  streak (every failure; the backoff's exponent, 10 ms doubling to 10 s), `wake_failed` carrying the error, and the retry time. A
  request pin that meets `owed` inside the backoff fails at once with that `wake_failed`, posts nothing, and takes no pin. The
  wake's success resets all of it.
- `wake_gate()` is the one function; `end_at()` (a demotion or a refused close that ends at `owed` with a request waiter) asks it
  too: inside the backoff the end is `owed`, the request waiters fail and lose their pins in the same compare-exchange and the
  chore waiters keep theirs (C14, I3-N1).
- `begin_bound_close(idx)`: `{0, owed}` to `{0, closing}` when `failures >= wake_bound` and the probe time has passed (both only
  hints, read atomically); the chore re-checks the count after the exchange, then: `close_ready()`; if not ready and something is
  uncommitted, the host's commit chore, then `demote()` (a commit maps the records it writes, and a slot with maps cannot close),
  then `close_ready()` and `maps == 0`; ready: `close()`. A failed commit ends at `owed` with `probe_at` = now + 10 s and counts
  in `failing()`. A reset (a wake that completed and a demotion between the sweep's load and its exchange) ends at `owed`.
- Choices the text leaves open, for the architect:
  1. S6's close at the bound is the sweep's (item 5); item 4 ships the primitive. The sweep must call it for a slot with
     `wake_failures_of(i) >= wake_bound` and `now >= probe_at_of(i)`; `begin_close()` stays the plain cold transition.
  2. The commit at the bound is followed by `demote()` so the slot can close in the same chore; the alternative is to end at `owed`
     and let a later pass demote and close. Widened (I4-B1): the close chore demotes after its commit step whenever the slot has maps,
     whatever committed them (a chore pin's commit during the backoff maps the records it writes). The bound close therefore demotes
     under `closing`, not under `{0, demoting}`: no pin is handed out while the slot is `closing`.
  3. `host::timer` was optional; overruled (I4-N1): it is required. A sleeping repost holds the catalog group's only lane (the
     partition's transitions share one group, `per_group = 1`), which stalled another key's miss for 175 to 286 ms against 0.3 to
     0.8 ms, and a cancelled group dropped the wake and stranded its waiters. The sleeping branch is gone; `catalog`'s constructor
     throws `std::invalid_argument` for a host without a timer; every host in the tests (`world`) supplies one.
  4. `too_many_open` takes the backoff with the same exponent as faults (the streak counts both) but not the bound's count.
  5. A demotion's end cannot be inside a backoff (a wake that succeeded reset it), so the gate at that edge is never taken; it
     is the same call as at the refused close, which is. Kept (C14's one gate), and the claim is a debug `assert` at that edge
     (`end_at()`: `from != demoting || streak == 0`), so a later edge into `demoting` that breaks it fails loudly.
  6. `wake_failed` derives from `std::system_error` (the cause's code, or `io_error`), so a handler for a wake's own errors catches it.

## S5 and S6, clause by clause

Tests are in `test/catalog_wake_test.cc` (WT) and `test/catalog_test.cc` (T6 `schedules`), `test/catalog_stress_test.cc` (ST).
All run with `defer_io`, `eager_segments` on, `keyed`, two segments per index, on `inline_executor` and the world's injected timer
(WT); the stress runs a WorkPool and a timer thread. Mutants are exact edits in `.scratch/mutants4.py`, one-TU builds ahead of the real
include dir with CI's flags (`-O2 -g`, asserts live); the unedited control passes. IDs below are that file's.

| Clause | Test | Mutants that die |
|---|---|---|
| S5: no request pin before `promote()`; `unleased_maps()` zero; no `apply` answers `map_owed` | WT `pin_after_promote` (3 rounds, a waiter resumed by the wake's own thread, an `apply` at the pin); T6 and ST check every request pin and every chore write | A1 (the pin first), A2 (`owed` pinned as `open`) |
| S5: a chore pin on `owed` commits without a wake | WT `chore_pin_commits` (an uncommitted delete, committed on `owed`, no wake, nothing queued) | A3 (a chore pin waits on `owed`) |
| S5 case 1: a stream of chore pins does not starve the wake | WT `stream_of_chore_pins` (a second thread holds the lease from before `waking`; the request, a chore pin taken after `waking` and the stream's next pin all wait; the wake completes at the first timer after the hold) | A5 (a chore pin enters `waking`) |
| S5 case 2: a chore pin whose thread holds the lease enters `waking`; the wake completes after the hold | WT `nested_pin_enters` | A6 (no `held_here()` exception) |
| S5 case 3: the repost goes through the timer, 1 ms to 64 ms, counted, outside the bound | WT `repost_through_timer` (13 reposts, the delays 1, 2, 4, 8, 16, 32, 64 ms…, `wake_failures_of` zero, the executor idle between) | A4 (the wake waits on the lease), A7 (immediate repost; dies by the timeout, the busy loop it is), A11 (reposts count), A12 (no doubling) |
| S5: the timer's thread does no device work | WT `timer_thread_does_no_device_work` (the map happens on the executor's thread, not the firing one) | A13 |
| S5 case 4: the wake ends `chore_scope` before its final compare-exchange | WT `scope_ends_first` (the resumed request's `lease_scope` is outermost) | A8 |
| S5 case 5: a wake's `too_many_open` fails the request pins, sets the backoff, the next pin fails at once with no chore | WT `overload_backs_off` (three overloads, the backoff 10, 20, 40 ms to the nanosecond, `wakes_posted` unchanged, never at the bound, recovery) | A9 (no backoff), A10 (counted) |
| S6: failed wakes back off (10 ms doubling), request pins fail with `wake_failed` and post no chore, chore pins still enter | WT `bound_and_backoff` (persistent `EMFILE`, 8 failures, each backoff probed at its edge) | A14 (no gate), A16 (the edge off by one) |
| S6: the 8th failure; a committed slot closes at the bound and the next pin reports the reopen's error | WT `bound_and_backoff` (the 7th does not, the 8th does; no commit chore; `EMFILE` from the reopen; the slot starts over: its open backs off 10 ms) | B1 (no close), B6 (bound too low), B9 (no reset on close) |
| S6: an uncommitted slot at the bound is committed by the host's commit chore, then closed, and its data reopens | WT `bound_commits_then_closes` (the erase survives the reopen) | B2 (close without the commit), B8 (no demotion after the commit) |
| S6: a failed commit leaves it `owed` with a 10 s probe | WT `bound_commit_fails` (the probe to the nanosecond, `failing()`, the probe renewed, then the close) | B3 (closes anyway), B4 (no probe), B5 (not counted) |
| S6: `too_many_open` does not count | WT `overload_backs_off`, T6 (`wake_overloads`) | A10 |
| a success resets the count | WT `count_resets` (7 failures, a success, 7 again, never the bound) | A17 |
| C14: the gate at the composed edge | WT `refused_close_in_backoff` (in the backoff the refused close ends at `owed` and fails the waiter with no wake; after it, a wake and a pin) | A15 (gate only at the pin) |
| the final store is a compare-exchange (a failed wake) | WT `unpin_during_wake_fail_end` | C1 |
| the backoff is in place at the store | WT `backoff_visible_at_the_store` (hook `wake.unlocked`); ST under TSan | C2 (the fields after the unlock: dies in the test and as a TSan data race in ST) |
| a delivery leaves another transition's waiters | WT `failed_wake_delivery_leaves_new_waiters` | C3 |
| the bound close is a compare-exchange | WT `bound_close_loses_to_a_pin` | C4 |
| the chore checks the bound again after the exchange | WT `bound_close_after_a_reset` (a wake and a demotion rebuild the identical word under the sweep) | B7 |
| the destructor waits for an armed timer | WT `destructor_waits_for_timers` | C5 (the timers uncounted: the callback meets a destroyed catalog, a hang) |
| I4-B1: a slot whose maps a chore pin's commit made closes at the bound (no uncommitted write left, the commit's maps demoted; with the fault on, the bound close commits itself) | WT `bound_after_chore_commit` (both variants end `free`, maps 0, the erase kept); T6's drive commits through a chore pin on `owed` before the bound close | F1 (demote only after this chore's commit), F1b (never) |
| I4-N1: the host supplies the timer | WT `host_requires_timer` | F2 |
| I4-N2: the timer's callback has left the catalog before the destructor returns | WT `callback_leaves_before_free` (hook `timer.end`) | F3 |
| I4-N5: a timer that throws, or a `wake_failed` that cannot be built, fails the wake and leaves no slot `waking` | WT `throwing_timer_fails_the_wake`, `wake_failed_build_throws` (hook `wake.failure` throws `bad_alloc`) | F4, F5, F6, F6b |
| choice 4: no demotion ends inside a backoff | the `assert` in `end_at()`, run by every test that demotes | F7 (a success keeps the streak), F7x (the same without the assert) |

## T6 and the stress against the model

`catalog_test`'s `schedules` keep item 3's 18 state changes (`allowed_edge()`), now with: the bound close as an actor and an
injection site (`sweep.bound`), `wake.unlocked` as a site, held lease scopes on pins (so a wake meets a hold and reposts), a drive
action that takes a warm slot past a bound of 2 with persistent faults or the budget's cap and then calls the bound close, commit
failures, timers fired and the clock advanced, and the checks that a chore pin sees `waking` only on a thread holding the lease and
that an `apply` on an `open` slot never answers `map_owed`. Every allowed edge is still made and the sites all get injections.
300 seeds x 160 steps (48,000 steps) on Release: 10,502 pins granted, 4,951 failed; wakes 186, 4 reposts, 124 failures, 11 overloads,
70 fast failures in a backoff; 24 bound closes (5 commits, 3 failed); injections at `sweep.bound` 9, `wake.unlocked` 61, `wake.end` 68.
1,000 seeds (at strata:fd95fef): wakes 663, 15 reposts, 376 failures, 69 overloads, 205 gated, 72 bound closes (13 commits, 8 failed). The stress
(`catalog_stress_test`: WorkPool of 3, a timer thread, 4 pinners, a sweeper that also calls the bound close and flips commit and kernel
faults; faults last 40 ms) at 6,000 ops: wakes 83, 4 reposts, 62 failures, 6 bound closes (10 commits, 5 failed), zero stuck.

## Evidence

Homebrew LLVM clang++ on macOS x86_64; builds in `.scratch/{rel,tsan,asan}`.
- Release (`-O2 -g`): `catalog_wake_test`, `catalog_test` 100, 300 and 1,000 seeds, `catalog_stress_test` 3,000 and 20,000 ops: ok.
- TSan (`-O1`, `halt_on_error=1`): `catalog_wake_test`, `catalog_stress_test 1500 4` ok, zero reports; `catalog_test 100` ok.
- ASan+UBSan (`-O1`, `-fno-sanitize-recover=undefined`): `catalog_wake_test`, `catalog_test 300`, `catalog_stress_test 800 4`: zero reports.
- Mutants: 31 of 31 edits in `.scratch/mutants4.py` die (A1 to A17, B1 to B9, C1 to C5; C5 under ASan, A13 and C1 to C4 also under
  TSan; C2 as a TSan data race in the stress); the unedited control passes. Results: `.scratch/mutants4-{relc,tsan,asan}.txt`.
  First run of B9 survived: the reopen after a bound close did not check the backoff; `bound_and_backoff` now does.
  Item 3's table (`.scratch/mutants3.py`, rerun on this tree; D1, D3, S2 and R1's anchors re-expressed for `end_at()` and
  `run_close()` as they changed): all 28 edits die (26 in `catalog_test`, X2 and S3 as TSan data races in the stress), the control passes.
- Preflight (`.scratch/pf.sh`, the staged form of `cpp-libraries/tools/preflight.sh`, holds under 30 minutes), on strata:6ebb4f0's tree
  (item 4 before item 3's fix round): mac-hardened ok (67 of 67), mac-asan-ubsan ok (67 of 67), linux gate VM
  `-D_GLIBCXX_ASSERTIONS` ok (68 of 68), FreeBSD gate VM ok (67 of 67).
- Item 3's fix round merged in (strata:b7e0deb; strata:f407bd0 the stress harness's schedulers, the linux-tsan race, strata:9476224
  I3-B1 and C16; conflicts in `catalog.hh` (both blocks kept), the stress test (the deque of schedulers and the bound), `limits.md`
  and this ledger). On the merged tree: Release `catalog_wake_test`, `catalog_test` 300 and 1,000 seeds, `catalog_stress_test`; TSan
  `catalog_wake_test`, `catalog_test 100`, `catalog_stress_test` 1,500 and 3,000 ops, and ASan+UBSan `catalog_wake_test`, `catalog_test 300`, `catalog_stress_test 800 4`: zero reports. The preflight was not rerun on it.
- CI: pushed; the lead watches it.

## Fix round after the architect's review (`docs/reviews/cold-tier-step2-item4.md`, strata:679a18b: REJECT on I4-B1)

- I4-B1: `run_close`'s bound branch commits if uncommitted and then demotes whenever the slot has maps (`maps != 0`), not only
  after its own commit. The plain close is unchanged: it has the same hole for an idle committed `owed` slot, which is C19 (item 5).
- I4-N1 (choice 3): `host::timer` is required (the constructor throws `std::invalid_argument`); `after()`'s sleeping branch is gone.
- Choice 4: the gate call at the demotion edge stays; "never inside a backoff" is an `assert` in `end_at()`.
- I4-N2: the timer's count is a mutex and condition variable, not an atomic: the callback's last touch of the catalog is its unlock
  under `timers_mu_`, which `~catalog` takes before it returns (a hook `timer.end` sits there for the test).
- I4-N5: `after()` undoes its count when the host's timer throws, and `run_wake` fails the wake with that error through
  `finish_wake`; `finish_wake` builds the `wake_failed` before the mutex and, if that allocation fails, files the allocation error as
  the backoff's error (hook `wake.failure`). A host timer that throws must not have armed its callback.
- I4-N6: the stress has two holder threads that demote a cold slot, chore-pin it, start a wake with a request `try_pin` and hold the
  lease for 2 to 40 ms (under the key's turn), so the wake reposts; it reports the holds and reposts and requires one. The first
  version of it (one repost per hold at best) left a wake waiting on its timer at the end of the run, which FreeBSD's preflight
  caught as `catalog_stress` failing on two runs of three (a slot `waking` at the quiescence check); `world::quiesce()` now waits for the
  chores and the timers' reposts before the check and in `drain()`.
- I4-N4: `limits.md`'s bound row reads "Load-bearing from item 5 (the sweep calls it)"; the mechanism note says so too.
- The 1,000-seed wake count is corrected above (663, not 491).
- Left as the review has them: I4-N7 (the `wake_gated` count per `end_at()`, `failing()`'s comment, a report for a wake that reposts
  for ever) is item 5's to settle with the sweep.

### Design text the lead owns (`docs/design/cold-tier.md`)

1. I9: "only `demote()` makes a record without a body, under `{0, demoting}`" becomes "... under `{0, demoting}`, or in the bound
   close chore under `closing`; neither hands out a pin before its end".
2. D5's bound paragraph: the close chore demotes when the slot has maps, after its commit step, whatever committed them (I4-B1); and
   for C19, the plain close does too, so an idle committed `owed` or `open` slot can go cold.
3. D4/D5's host list (Q2): the timer is required, not optional; a repost never sleeps on the executor (the partition's one group
   lane).

### Conditions carried to item 5

- **C18.** The sweep calls `begin_bound_close()` for `owed` slots at the bound once their probe time has passed; S7 carries S6 end
  to end through the sweep (no direct call), with the mutant "the sweep never calls it"; `limits.md`'s bound row is load-bearing
  from then (I4-N4).
- **C19.** An `owed` or `open` slot with maps, idle and committed, can go cold: the close chore demotes when the slot has maps (the
  I4-B1 fix generalized to the plain close), and D5's warm-to-cold paragraph and I9 say so.
- C15, C16, C17 as in item 3.

### Evidence for the fix round

Homebrew LLVM clang++, macOS x86_64, no build type, `-O1 -g` plus the sanitizer (asserts compiled out: CMake defaulted to Release's `-DNDEBUG`, so F7 and F8 died on CHECKs; the re-check killed the choice 4 assert's own mutant in a Debug build), builds in `.scratch/ct4fix/b-{tsan,asan}`
(`cfg.sh`, `run.sh`); every build and run in a `quiet light` hold.
- TSan (`halt_on_error=1`): `catalog_wake_test` ok (24 cases), `catalog_test` 300 and 1,000 seeds ok (1,000 seeds: 687 wakes, 12 reposts,
  388 failures, 73 overloads, 234 gated, 72 bound closes, 54 chore commits on `owed` in the drive), `catalog_stress_test` 3,000 x 4 and
  20,000 x 6 ok (the long run: 231 holds on a warm slot, 481 wake reposts; it was 1 repost before, and 2 at 3,000 x 4, now 508), zero reports.
- ASan+UBSan: `catalog_wake_test`, `catalog_test` 300 and 1,000, `catalog_stress_test` 3,000 x 4 and 8,000 x 4 (456 reposts): zero reports.
- Mutants (`.scratch/mutants4fix.py`, one-TU builds, ASan+UBSan; the unedited control survives): F1 and F1b (I4-B1), F2 (I4-N1), F3 (I4-N2),
  F4, F5, F6, F6b (I4-N5) all die in `catalog_wake_test`; under TSan F1, F3, F4, F5, F6, F6b die too (F4 by the destructor's hang, a
  timeout). F7 and F8 (a streak left over at a demotion) die in `catalog_wake_test` on a behavioural CHECK first (the streak also
  moves the backoff's exponent), so the `assert` at the demotion edge is not attributed a kill of its own; it has never fired over the
  runs above, and the oracle's check of the same claim (below) never fires either.
- The architect's probes (`.scratch/ct4fix/probes_fix.cc`, a copy of `review-item4/probes.cc` that no longer builds a catalog without a
  timer), all seven cases under TSan and ASan: 0 failures; `bound_after_chore_commit` passes (it failed before); `no_timer_stall`: a miss on
  another key opens after 0.4 to 0.9 ms with 1 and 4 wakes reposting on held leases. The oracle (`inc-oracle`, the two hard checks ported to
  this header) over `catalog_test` 1,000 and the 20,000 x 6 stress under TSan: never fires; `inc-oracle-nogate` aborts both.
- Preflight (`cpp-libraries/tools/preflight.sh`) on the tree at strata:960c10d: mac-hardened ok, mac-asan-ubsan ok (67 of 67), linux
  (`aws-linux.sh` lease, `-D_GLIBCXX_ASSERTIONS`) ok (68 of 68; the local ssh client hung after the remote ctest finished, so the line
  is read from the remote `build.log`), freebsd ok. The first FreeBSD run, on strata:c6aff1d, failed `catalog_stress` (two runs of three);
  the cause and fix are in I4-N6 above.
- CI: pushed (strata:960c10d); the lead watches it. Not waited on.
- State at hand-off: all of the review's required change (L1/I4-B1) and C20's items (I4-N1, the demotion-edge assert, I4-N2, I4-N5) are in,
  with their tests and mutants. Blocked on: nothing. Next: the lead's design edits above, then item 5 (C18, C19, C15 to C17).

Carried to item 5 (re-check R2): a timer callback whose `post()` throws leaves the repost unrun and the slot `waking`.

---

# Item 5: the sweep and shutdown

> Status: BUILDING (2026-11-02) item 5 of `docs/design/cold-tier.md` revision 7 (strata:5723f3d on `design/cold-tier`): D5, D6, D10;
> CT-R4, CT-R6, CT-R36; criterion S7; item 3's review condition C15 (I3-N2, I3-N3, I3-N6 and the dropped throw) and `failed` to `free`.

- Branch / worktree: `phase/cold-tier-step2-item5` from `phase/cold-tier-step2-item4` (strata:6ebb4f0), at
  `/Users/kronuz/code/Kronuz/strata-wt/cold-tier-step2-item5`. Item 3's and item 4's fix commits merge in as they land.
- State: fix round for the architect's REJECT (`docs/reviews/cold-tier-step2-item5.md`: B1 to B4, C21 to C23, item 4's R2) built and
  proved; see "Fix round" below. Item 4 (strata:4795b7e on main) is merged in. It adds a concurrency protocol and no decoder, parser or
  format, so no security review.
- Done: see "Commits" below.
- Next: the architect's re-check of B1 to B4 and C21 to C23. Item 6's merge seam (G6; the code review's notes on `run_close`'s refused-close
  exits and `begin_close`'s `unused` parameter) is item 6's, not merged here. Item 6 replaces the pass's candidate choice with S3-FIFO.
- Blocked on: nothing. For the lead: D5 on `design/cold-tier` should say an unfinished shutdown is abandoned by exiting, never by
  destroying the catalog (C23; the mechanism note says so); C24 is X-POOL's (below).

## Done criteria (S7 copied from `docs/status/cold-tier.md` on `design/cold-tier`, numbered here; never edited)

- **S7** (item 5). Demotion only from `{0, open}`; cold only from warm with a maps count of zero
  and `close_ready()`; a queued reclaim posts and closes on a later pass; `reclaim(0)` runs under
  `demoting`; a view kept past its pin keeps the slot `closing` without blocking a thread; a
  crossing posts one coalesced pass that reposts while over the soft cap; `shutdown()` commits
  the uncommitted, closes every slot, names the failed commits, and `H` is destroyed after its
  close chore returns. Mutants: close from hot; close with a retired view; demote a pinned slot;
  close with a queued reclaim; `reclaim(0)` under `open`; one pass per crossing without the
  repost; `H` destroyed inside the close chore.

Numbered:

1. **S7.1** Demotion only from `{0, open}`. Mutant: demote a pinned slot.
2. **S7.2** Cold only from warm (not hot) with a maps count of zero and `close_ready()`. Mutants: close from hot; close with a retired view.
3. **S7.3** A queued reclaim posts and closes on a later pass. Mutant: close with a queued reclaim.
4. **S7.4** `reclaim(0)` runs under `demoting`. Mutant: `reclaim(0)` under `open`.
5. **S7.5** A view kept past its pin keeps the slot `closing` without blocking a thread.
6. **S7.6** A crossing posts one coalesced pass that reposts while over the soft cap. Mutant: one pass per crossing without the repost.
7. **S7.7** `shutdown()` commits the uncommitted, closes every slot, and names the failed commits.
8. **S7.8** `H` is destroyed after its close chore returns. Mutant: `H` destroyed inside the close chore.

Item 3's condition C15 (`docs/reviews/cold-tier-step2-item3.md`), each a case and a mutant here:

9. **C15.1** (I3-N2) Destroying the catalog with a pin held asserts in debug builds (the ASan probe `teardown_pin` is a use-after-free of the catalog).
10. **C15.2** (I3-N2) `~catalog` leaks open slots' IO with its lock (D5) rather than destroying them; asserts in debug builds.
11. **C15.3** (I3-N3) A reused slot starts with no failures, error, retry time or probe time, and the new key's last use.
12. **C15.4** (I3-N6) A close that leaves `closing()` true keeps the slot `closing`, reports it in debug builds, counts it, and the sweep retries `try_finish_close()`.
13. **C15.5** (I3-N6) A throw from `close()` ends the close at the prior state (sweep) and is counted, not dropped by the executor.
14. **FREE** `failed` to `free`: the sweep erases a negative entry 10 s past its retry time (D5); a pin that arrives first reopens it.

## What was built, and where it differs from the text

- `include/strata/core/catalog.hh`: `start()`, `stop_sweep()`, `post_pass()`, `pass()`, `shutdown()` (an awaitable), `catalog_closed`,
  `shutdown_result`, `erase_failed()`, `kick()` / `shutdown_slot()`, `run_close(idx, prior, close_mode)`, `after_close()`, `retire()`,
  `run_stuck_retry()`, `release_chunks()`, and the host's `sweep_period_ns`, `demote_idle_ns`, `close_idle_ns`, `negative_ttl_ns`,
  `soft_open`, `soft_hot`, `pass_visits`, `pass_posts`. `docs/mechanisms/catalog.md` describes them; `limits.md` gains two rows.
- The pass reads only atomics (the word, `last_use`, the slot's counts, `failures`, `probe_at`) before its compare-exchange; `key_hash` became
  atomic so the erase can find a slot's shard. It calls `begin_bound_close()` for an `owed` slot at the bound (the lead's S6 note).
- Where the text is silent or I chose:
  1. Candidate order is oldest use first (item 6 replaces it with S3-FIFO). Over a cap a hot slot is demoted first and a warm one closes.
  2. `soft_hot` caps the catalog's mapped-record count (its maps count), not a count of hot indexes: a hot index is not countable without a
     hook in the index; the two agree for one-segment indexes (T3/T4's fixtures).
  3. A pass that posted nothing does not repost, so a catalog with everything pinned does not spin; the 1 Hz timer tries again. A pass counts the
     closes in flight as relief already posted (a pass that runs beside them does not repeat them); demotions in flight are not counted.
  4. Shutdown closes with `close()` after the commit chore, not through `close_ready()`; it posts no `reclaim()`. A close that leaves
     `closing()` true keeps shutdown waiting (the pass retries each period); a `close()` that throws is retried after 64 ms.
  5. Pins by handle fail with `catalog_closed` too once shutdown began (a request that re-pins during shutdown fails).
  6. The timer callbacks of the sweep hold a shared `life`, not the `timers_` count, so `~catalog` does not wait up to a period for an armed one.
  7. The plain close of an `owed` slot with leftover maps (a commit's, C19) demotes them away in the chore; a hot `open` slot is still refused (the pass
     demotes it first).
  8. `~catalog` finding open slots calls `owner_break` and keeps their IO, index, reader and H forever, and the chunks that hold them; with a pin held
     it also reports, and that pin's release is a use after free in any build (an assert in debug builds is all that can be done).
  9. Test seams: hook sites `chore.demote`, `chore.close`, `close.call`, `erase.pre`, `sweep.erase`, `shutdown.close`, and `catalog::in_close_chore()`.
- Tests that changed: `catalog_wake_test`'s `destructor_waits_for_timers` destroys a catalog with an open slot and turns the owner break off; the
  schedules' chore-pin check reads the word when the task resumes, so it allows `waking` with another pin (the wake began after the grant).

## S7 and C15, clause by clause

Tests are in `test/catalog_sweep_test.cc` (SW; one case per clause, on `inline_executor` and the world's injected timer), `catalog_test.cc` (T6
schedules) and `catalog_stress_test.cc` (ST). Options: lazy segments (`eager` off) where a slot must be warm at once, `defer_io` where a slot
goes `owed`, `auto_merge` off, `keyed`. Mutants are exact edits in `.scratch/mutants5.py` (`relc`: one-TU builds ahead of the include dir, CI's
flags; `tsan`: the stress and the sweep test under TSan).

| Clause | Test | Mutants (all die) |
|---|---|---|
| S7.1 demotion only from `{0, open}` | SW `demote_window` (a pin lands between the load and the exchange), `demote_idle` (a pinned slot is skipped; 29.9 s no, 30.1 s yes; an `owed` slot is not demoted) | Z1 demote a pinned slot; Z28 demote at the close time |
| S7.2 cold only from warm, maps zero, `close_ready()` | SW `close_from_warm_only` (hot slots are demoted, only warm ones close, no close is refused), `close_chore_refuses` (hot, a retired view kept by a guard, an uncommitted write), `warm_idle_closes` (599.9 s no, 600.1 s yes), `close_window` | Z3 close from hot; Z4 close with a retired view; Z2 close a pinned slot; Z29 close at the demotion time |
| S7.3 a queued reclaim posts and closes on a later pass | SW `queued_reclaim` (deletes inline and posted: `reclaim()` posted once, the slot ends at `open`, the next pass closes) | Z5 close with a queued reclaim; Z6 never posted |
| S7.4 `reclaim(0)` under `demoting` | SW `reclaim_zero_under_demoting` (the chore sees `demoting`; `demote()` refuses with a buffered write while a released retired view is freed by `reclaim(0)`: the maps count falls) | Z7 chore under `open` (the chore's state check and `end_at`'s assert); Z8 no `reclaim(0)` |
| S7.5 a view kept past its pin keeps the slot `closing`, no thread blocked | SW `stuck_close` (reported once, counted, a waiter suspended, two retries fail, the third succeeds after the guard goes and the waiter reopens in place on `gen` + 1; without a waiter the slot frees) | Z9 never retried; Z10 a throw escapes (C15.5) |
| S7.6 a crossing posts one coalesced pass that reposts while over | SW `coalesced_pass` (100 posts, one pass), `repost_while_over` (7 slots, cap 2, 2 posts a pass: 3 passes, 2 reposts, 5 closes; a pass that posts nothing does not repost), `budget_crossing` (the budget's listener), `periodic_timer` | Z11 no repost; Z12 no coalescing; Z26 no listener; Z27 timer not re-armed; Z32 `over_open` never counts; Z37 closes in flight not counted; Z34 one slot a pass |
| S7.7 `shutdown()` commits, closes every slot, names the failed commits | SW `shutdown_commits_and_closes` (six slots: committed, commit fails, clean, negative entry, pinned: waits for the pin; new pins fail with `catalog_closed`; data survives a reopen except the named one), `shutdown_every_state`, `shutdown_empty_and_waiting`, `transitions_race_shutdown` (an open, a demotion and a wake that end after shutdown began), `shutdown_lease_busy`; T6 (a shutdown after every history), ST `stress_shutdown` (pinners running, commits failing together, joiners) | Z17 no commit; Z18 not named; Z19 pins let in; Z20 unpin does not look; Z21 open end; Z38 demotion end; Z39 wake end; Z22 closes a pinned slot (hangs, timeout); Z23 completes early; Z35 no retry on a busy lease; T2, T3 (TSan) |
| S7.8 H destroyed after its close chore returns | SW `shutdown_commits_and_closes` (`payload::destroyed_in_close == 0`), ST (the same under pool threads) | Z13 H destroyed inside the chore |
| C15.1 destroyed with a pin held | SW `destroyed_pinned_asserts` (a forked child aborts; with the assert off, two breaks) | Z25 not reported |
| C15.2 destroyed open: leaked, not destroyed | SW `destroyed_open_leaks` (one break, the budget still charged, the directory lock still held) | Z24 destroyed |
| C15.3 a reused slot starts clean | SW `reuse_is_clean` (three failed opens, erased, another key on the slot fails once: 10 ms, not 40; its last use is its own) | Z15 not reset |
| C15.4 stuck close | S7.5 | Z9 |
| C15.5 a throw from `close()` | SW `close_throws` (hook `close.call`: the slot ends at `open`, counted, closes next time) | Z10 |
| FREE `failed` to `free` | SW `negative_entry_erased` (9.99 s no, 10 s yes; edge counted; a retry arrives first and wins), `negative_entry_race` (a retry between the pass's look and its erase) | Z14 erased at the retry time; Z30 ignores the state; Z31 table entry stays; T1 erase outside the mutex (TSan) |
| S6 end to end through the sweep (the lead's C18) | SW `pass_closes_at_the_bound` (7 failed wakes: left alone; the 8th: a pass commits (fails), probes at 10 s, commits, closes; the write survives) | Z33 the pass never calls the bound close |
| C19 an idle `owed` slot with maps closes through the plain close | SW `owed_with_maps_closes` | Z36 no demotion in the chore |

## Item 6's seam ruling (docs/reviews/cold-tier-step2-item6-ghost.md, G6)

Applied here, to the pass: corrections 1 and 3 as in-flight counts. A begun demotion books the slot's maps, a begun close its descriptors and
vnodes (`book()`), released when the transition ends (`unbook()`: `end_at`, `finish_close`, a stuck close); the pass reads the caps' shares less
those (`pending_maps_`, `pending_desc_`, `pending_vnodes_`), so a pass beside chores not yet run does not repeat them (`relief_in_flight`;
mutants Z40 to Z42). Correction 2 is the pass's own measure (`live_ - closing_`, kept by `note()`); correction 4 is the one coalesced chore.
Item 6's `evict(limits, result, over, before_close)` plugs in where the sorted candidate loop is.

## Evidence

Homebrew LLVM clang++ on macOS x86_64; builds in `.scratch/{rel,tsan,asan}`. Final tree: the commit this section is in.
- Release (`-O2 -g`, asserts live): `catalog_sweep_test` (27 cases), `catalog_wake_test`, `catalog_test` 300 seeds (48,000 steps; 3,984 passes,
  575 demotes, 840 closes, 1,673 negative entries erased, 541 shutdown commits, every allowed edge made including `failed` to `free`, every site
  injected), `catalog_stress_test 3000 4` (sweep at microsecond idle times, 2,665 passes and 7,997 closes in the plain run; shutdown with the
  pinners running: 3,000+ pins, 10 to 14 shutdown commits failing together, joiners): ok.
- ASan+UBSan (`-O1`, `-fno-sanitize-recover=undefined`, `_GLIBCXX_ASSERTIONS`): the four tests (`catalog_test 100`, `catalog_stress_test 1500 4`): zero
  reports. TSan (`-O1`, `halt_on_error=1`): the same, zero reports.
- Linux (aws-linux lease, clang++ 18, ASan+UBSan and TSan, the four tests with 300 seeds and 3,000 ops): zero reports on strata:e20219b's tree
  (before the pending-relief change); the lease reached its hours limit (4.01 h, "System is going down") before the final tree's run, which is not run.
- Mutants (`.scratch/mutants5.py`, one-TU builds ahead of the include dir, CI's flags): Z1 to Z15 and Z17 to Z42 (41 edits), T1 and T3 (TSan), Z1, Z2 and Z13 also under the TSan stress: all die; the control passes. Z22 (shutdown closes a pinned slot)
  dies by the test's timeout (the hang it makes). **T2 survives** under TSan on macOS and on Linux: the failed commits' names pushed without the
  mutex. The stress makes pairs of pool threads meet at the naming (`shutdown.name`, 5 of 10 per run), and a standalone program with the same
  rendezvous is flagged, but TSan reports nothing on the catalog; I could not find the order that hides it. Open for the reviewer.
- First runs that failed and what they were: the pass closed 7 of 7 slots where 5 were due (a close's `close()` ran the scheduler nested and a second
  pass saw the slot still open; closes in flight now count as leaving); `catalog_wake_test` and the schedules destroyed catalogs with open slots or
  pins (the destructor now says so); the schedules let a late pin take a lease hold and a wake behind it never ended (no holds while settling).
- Preflight (`.scratch/pf.sh`, the staged form of `cpp-libraries/tools/preflight.sh`) on strata:e20219b: mac-hardened ok (68 of 68), mac-asan-ubsan ok
  (68 of 68), FreeBSD gate VM ok (68 of 68); on the final tree (strata:23a71e8): mac-hardened ok (68 of 68), mac-asan-ubsan ok (68 of 68), FreeBSD gate VM ok (68 of 68). Linux preflight: not run
  (the lease above), so Linux is CI's.
- CI: pushed to `origin/phase/cold-tier-step2-item5`; the lead watches it.

## Fix round (`docs/reviews/cold-tier-step2-item5.md`, strata:f8c339f: REJECT on B1 to B4; C21 to C24; item 4's R2)

Item 4 (main strata:4795b7e) merged in first (strata:5b696df): `run_close` keeps item 5's close modes and takes item 4's bound path
(demote whenever maps remain); the timer count is item 4's mutex and condition; `start()` no longer checks for a timer the
constructor requires. Item 4's two timer tests that destroy a catalog with an open slot turn the owner break off (item 5's leak).

| Finding | Fix | Test | Mutants (all die) |
|---|---|---|---|
| B1 shutdown completes between a miss's check and its count | The miss counts its slot in `occupied_` before it checks `shutting_`, both `seq_cst`; `join_shutdown` sets the flag (`seq_cst`) before `maybe_complete_shutdown` reads the count (`seq_cst`). Chosen over "check under the shard mutex and pass every shard mutex": the review's probe and `transitions_race_shutdown` join shutdown from hooks that run under a shard mutex, which that fence would deadlock. `note()` no longer counts `free` to `opening`; the failure paths undo the count and complete shutdown | SW `shutdown_vs_miss` (the probe: join at `pin.miss`; the pin is granted and shutdown completes only after its release), `shutdown_after_count` (join at the new `pin.counted` site; the pin fails with `catalog_closed`, shutdown completes), `shutdown_commits_and_closes` now also fails a pin by key on an open slot and a re-pin by handle | Y1 check before the count, Y2 no check after it, Z19 no unlocked check |
| B2 a closing cap demotes the whole hot set | A hot slot demoted while a closing cap is over books that close (`due`: one index in `pending_open_`, its descriptors and vnodes in its `pend_desc`/`pend_vnodes`) and takes that relief off the pass; the booking goes when the close begins (`begin_close`, `begin_bound_close`, `shutdown_slot`) or a pin takes the slot (by key, by handle, or a waiter the demotion's end hands it; a demotion that leaves maps drops it too). A booked demotion's end posts a pass, which closes booked slots first | SW `over_open_by_one` (the probe: 40 hot, `soft_open` 39: one demotion, one close, 39 still hot; then the booking dropped by a waiter, a re-pin by handle and a pin by key), `close_from_warm_only` (now 2 demotions for 2 closes, the newest stays hot) | Y3 the reviewed code, Y4 relief taken but not booked, Y5 booked slot not closed, Y6, Y18, Y19 pins leave the booking, Y7 no pass at the end |
| B3 a stuck close at shutdown is retried only by the sweep | In shutdown, `after_close` arms `arm_stuck_retry` (64 ms through `after()`); `shutdown_slot` arms it for a close stuck before shutdown began; a failed retry re-arms while shutting down | SW `shutdown_stuck_no_sweep` (the probe, both orders: never `start()`ed, the retry runs every 64 ms, the view goes, shutdown completes) | Y8, Y9, Y10 |
| B4 the stress fails and hangs on Linux TSan | (1) `stop_sweep()` before `quiesce()`. (2) The joiners are tasks, each resumed on its joiner's `mt_sched`, which runs until all of its tasks resumed: nothing is destroyed while `deliver` writes it. (3) The hang, from a gdb `thread apply all bt` on the VM (below): a pass reposting forever on refused transitions. A refused close (not a queued reclaim, not a failed commit) or a demotion that leaves maps now holds the slot off for a sweep period (`hold_off`, the slot's `probe_at`), and the pass skips held-off slots | SW `refused_no_spin` (hot slots whose buffered writes refuse the demotion; warm slots whose lease a commit holds: each tried once, then the reposts end; a period later they are tried again); ST on Linux TSan, 10 runs | Y20, Y21, Y22 |
| C21 the stress at `per_group` 2, overlaps counted | `pool_exec` takes `per_group` (argv[3], default 2); the naming hook waits up to 200 ms for another naming inside it at once and counts the closes that met one; the check requires overlaps when two or more were named | ST: 9 to 14 of 10 to 15 namings overlap per phase | T2 (the names pushed without the mutex) now dies by TSan; T3 (waiters taken without the mutex) dies with two tight joiners |
| C22 `soft_hot` counts hot indexes | `resource_counts::holders_mapped`, kept by `reserve`/`release` at the 0-to-1 and 1-to-0 edges of a slot's maps count; the pass reads `soft_hot` against it, less `pending_hot_` (demotions in flight), and the maps share (`over_maps`) separately | SW `soft_hot_counts_indexes` (four-segment indexes: `soft_hot` 2 with three hot demotes one), `relief_in_flight` (hot-index and maps-share bookings) | Y15 records again, Y16 no 1-to-0 edge (the counts' destructor asserts), Z40 the maps share read without the demotions in flight |
| C23.1 busy-lease retry | 1 ms doubling to 64 ms, like the wake's | SW `shutdown_report_and_lease_backoff` (1, 2, 4, 8, 16, 32 ms, then 64) | Y11 |
| C23.2 what holds shutdown | `shutdown_report()`: every slot not free, with key, state, pins and `shutdown_hold` (`pinned`, `transition`, `lease_busy`, `close_throws`, `close_failed`, `view_held`, `waiting`), keys read under the shard mutex | SW `shutdown_report_and_lease_backoff`, `shutdown_stuck_no_sweep`, `shutdown_close_throws_bounded` | Y13, Y14 |
| C23.3 abandoned by exiting | The mechanism note says so (Shutdown); D5 is on `design/cold-tier` (Blocked on, for the lead) | — | — |
| C23.4 a throwing close does not re-arm forever | `close_retries` = 8 tries 64 ms apart, then the slot stays `closing`, `close_failed`, no timer armed; `~catalog` returns and leaks it | SW `shutdown_close_throws_bounded` (8 failures, report, destruction returns with one owner break) | Y12 |
| R2 an unposted timer repost leaves the wake `waking` | `after()` takes a failure handler the callback calls when its post throws (site `timer.post`); the wake's repost fails the wake with that error | WK `unposted_repost_fails_the_wake` (bad_alloc at the post: the waiter gets it as the cause, the slot `owed`, nothing armed) | Y17 |

- C24 (X-POOL's teardown destroys every pin-holding root before awaiting `shutdown()`, under ASan) is X-POOL's, carried with C17.
- Notes: N1 is in the mechanism note (act on the resumption, never on `shut_down()`); N2 to N4 are not changed (N4's counter is
  reworded by C21). The slot stays 184 B on libc++ and 192 B on libstdc++ (D4's figure): the new bytes pack into padding and the
  wake streak is a saturating 16-bit count. `limits.md` gains the shutdown retries row.
- The hang (B4), on the review's own ccf34cd TSan build on the local Linux VM (`.scratch/linux-hang/`; gdb installed there): of 8 runs of
  `stress_pg 3000 4` at `per_group` 1 with the sweep running, run 8 hung past 240 s and run 5 failed the quiescence check after 149 s.
  The hung run's stacks (`bt-8.txt`): the main thread in `WorkGroup::wait` from `wait_chores()` at the quiescence check (`stress_pg.cc:236`),
  every other test thread joined, one pool worker serving the catalog's group (`WorkPool::serve`), two parked. Run 5's plain phase made
  183,198 passes, 183,010 of them reposts, and 1,464,352 closes: over `soft_open` with every candidate's close refused (the chore pins'
  uncommitted writes), each pass posted the same closes, saw the cap still over, and reposted at once, so the group was never idle and
  `wait()` never returned. macOS's timing rarely lets every candidate be refused at once. Fixed in the catalog (hold-off), not only in the
  test (stop_sweep).

### Evidence (final tree: strata:f6fd39f)

- Release (`-O2 -g`, Homebrew LLVM): `catalog_wake_test`, `catalog_sweep_test` (35 cases), `catalog_test 300` (3,662 passes, 454 demotes,
  502 closes, 1,662 negative entries erased, 483 shutdown commits), `catalog_stress_test 3000 4`: ok.
- macOS TSan and ASan+UBSan (`-O1 -g`): the four tests, the stress 3 times each: zero reports.
- Linux (local VM, clang 18): see below.
- Mutants (`.scratch/mutants5.py`, one-TU builds ahead of the include dir; `.scratch/mutants5-{relc,tsan}.txt`): the 41 earlier ones
  (Z3, Z23, Z29, Z35, Z40, Z42 re-anchored on the changed lines) and Y1 to Y22 all die in the release build; T1, T2, T3, Z1, Z2, Z13, Z20 to
  Z23, Z31 die under TSan; the control survives.
- The review's probes (`.scratch/review-item5/probes6.out`, built against this round's ASan and TSan builds): `shutdown_vs_miss` pin
  granted, shutdown not done until its release; `shutdown_stuck_no_sweep` done once the view goes, no `start()`; `over_open_by_one` 1 demotion,
  1 close, 39 hot; `shutdown_close_throws_forever` 8 failures then no timer; `bound_after_chore_commit` and `reused_slot` as before (its
  frequency CHECK at `probes.cc:155` fails as in the review's own run); `t2_window` at 1 and 3 as before.

## Commits

See `git log phase/cold-tier-step2-item4..phase/cold-tier-step2-item5`; the fix round is strata:5b696df (merge), strata:7d03120,
strata:e3157b2, strata:9b94267, strata:77459ce, strata:f6fd39f. Not done: item 5b.

# Item 5b: the catalog's host interface for X-POOL (developer's ledger)

> Status: REVIEWED (APPROVE WITH CONDITIONS), FIX ROUND DONE (2026-11-03) `phase/cold-tier-step2-item5b`, stacked on `phase/cold-tier-step2-item5` (strata:ccf34cd) with item 4's
> fix round merged (strata:3c2112f); builds ahead of both gates. Spec: the architect's ruling `docs/reviews/cold-tier-xpool-host.md` on
> `design/cold-tier` (strata:94d65f0), gaps F1 to F6, findings XH1 to XH8, conditions C21, C22. Awaiting its one review (C22, high effort).

- Branch / worktree: `phase/cold-tier-step2-item5b` from strata:ccf34cd, at `/Users/kronuz/code/Kronuz/strata-wt/cold-tier-step2-item5b`.
- Consumer: Xapiand `docs/design/x-pool.md` revision 2 (`Xapiand-wt/x-pool`).
- State: S10 to S16, C14's oracle and C17 built and proved (Mac and Linux, rel, ASan+UBSan, TSan; preflight on mac, linux and freebsd; CI green);
  item 4's and item 5's fix rounds are merged. The one review (`docs/reviews/cold-tier-step2-item5b.md`, APPROVE WITH CONDITIONS) is folded in: L1 to
  L3, N3, C24, F4 to F7 are done ("Fix round for the review" below); C25 and the X-POOL note are carried; C26 (the slot's 192 B with item 6's fields)
  is for the merge with item 6, which item 5 does first.
- Done: see "Commits" at the end of this section.
- Next: the lead confirms L1 to L3 from the diff and a green CI (no re-review); `cold-tier.md` revision 8 (the text is below, for the design branch's
  owner); at the item 5 merge, `slot_cost` is fitted to 192 B on libstdc++ and libc++ (C26: a single waiter pointer, `retry_at` and `probe_at` as
  32-bit offsets, the review's suggestion).
- Blocked on: nothing.

## Done criteria (copied from `docs/reviews/cold-tier-xpool-host.md`, "Item 5b"; never edited)

Each criterion has a test and a mutant the test kills, on `inline_executor` and on the work pool, with the options it runs with (C7's
shape); separate TSan and ASan+UBSan builds; Linux TSan in CI is the authority (calibration 2026-10-29).

- **S10 create and resident (F1).** (a) Ten read pins on an absent key fail `index_not_created`; a create pin inside the backoff creates; the
  reads after it see the index. Mutant: create obeys the negative entry (the `absent_backoff` probe's behavior). (b) A create pin joins an
  `opening` slot whose attempt runs without create: every waiter, read and create, gets the created index, none fails, opens 2. Mutant:
  `finish_open` fails the waiters. (c) A create pin on an EIO negative entry fails fast. Mutant: create passes every negative entry. (d) A
  resident pin on an absent key: empty pin, `table_size()` and `resolves` unchanged; on `open` granted; on `closing` empty. Mutant: resident
  inserts.
- **S11 payload in place and the open step (F3).** `pinned_payload.cc`'s H (deleted move, an atomic member) compiles and runs as a catalog
  payload. The step applies a document and the first waiter's read sees it; a step that throws fails the open and its waiters, destroys H,
  reader, index, IO in that order (a destructor log), and leaves a negative entry. Mutants: hand out the pins before the step; destroy the
  index before H.
- **S12 the drain and reopen (F4).** Three holders; one calls `reopen` after a fenced commit; pins by key and by handle that arrive wait in
  `closing`; the last holder's unpin posts the close; waiters get a fresh, unfenced index with `gen` + 1, the host step re-ran,
  `pins == held` throughout. Mutants: post the close at `reopen` with holders (ASan); drop the acquire fence (TSan, and on arm64 CI); count
  waiters as holders (a hang the test times out). T6's 1,000 seeded schedules gain `reopen` and the drain unpin as writers of the word.
- **S13 fence in every chore (F4).** `fenced_stuck` becomes a test: an idle fenced slot is replaced on the next pass; a fenced `owed` slot's
  wake replaces it, its request waiters get the new index, `wake_failures` and the bound count unchanged; a bound close of a fenced slot
  closes. Mutant: today's catch-all (ends at `open`). A persistent sync fault: reopens bounded by the open backoff (at most 10 in the first
  10 s: one at once, then 10 ms doubling). Mutant: no decay rule.
- **S14 check-in before demote and close (F6).** An idle index whose background merge finished (auto-merge on a work-pool executor) demotes on
  the first pass after the merge, and closes later. Mutant: no check-in (it stays hot every pass).
- **S15 snapshot (F6).** Keys and states of every non-free slot, `owed` and `failed` included; a FLUSH-shaped test (snapshot, resident pins,
  commit) commits every uncommitted index and skips a key closed in between. Mutant: the snapshot skips `owed`.
- **S16 host files counted (F5).** A payload opening two files reserves on `opening.counts`; the slot's descriptor and vnode counts include
  them; at the hard cap the open fails `too_many_open` with no kernel error and backs off; after retire the counts return to zero, and T7's
  comparison holds with host files. Mutant: reserve after the system call.
- **C14 kept.** Item 4's oracle (no wake posted inside a backoff, `cold-tier-step2-item4/.scratch/review-item4/inc-oracle/`) rerun over T6 and
  the stress with the new rows, clean; its no-gate mutant still aborts.
- **C17.** CT-R15's debug check at `live()`: a call on a catalog index with no pin and no transition aborts.
- **Text.** `cold-tier.md` revision 8 (D4's host list and table, D5's fence replacement and the create intent, Limits: outside creation, a
  read-only busy index's merges); the mechanism note's sections for the intents, the drain and the fence; `limits.md` rows for the drain's 2^24
  holders (the pin field) and the reopen backoff.
- **No hot-path regression.** One strict-hold smoke A/B of a pin and unpin by key and by handle on the hottest key against item 5 (the unpin
  gains a branch); not a gate row.

Also required of this branch (the lead's brief): a deterministic window test per new transition (create joining a non-create open; reopen
draining holders; fenced replacement racing a pin); a mutant per ordering choice; separate TSan and ASan+UBSan builds on Linux
(`tools/vm/aws-linux.sh`) and the Mac; the stress against the model; then `cpp-libraries/tools/preflight.sh` for mac, linux, freebsd.
Review condition C22 (for the code review, at high effort): the acquire fence's pairing, a holder's unpin racing `reopen`'s exchange, and
`finish_wake`'s holder count under the mutex.

## What was built, and where it differs from the text

- `catalog.hh`: `open_intent` on `pin_request`, `pin_chore`, `repin` and `try_pin` (`resident` on a request pin throws `logic_error`);
  `catalog::opening {key, counts, create}`; `host::resolve(opening)`, `open_index(io, options, opening)` (the default makes the index when
  `create` and the open finds none) and `open_payload(opening, Index&, optional<H>&)` (the default emplaces `H{}`; an H that is not
  default-constructible needs the host's, checked by the constructor); `make_payload` is gone. `reopen(pin&&)`, `snapshot()`, `try_status::absent`,
  the replace close (`run_replace`, `replace_from`), `drain_unpin`, and the slot's `drain`, flag bits and reopen-backoff fields.
  `release_chunks` keeps a leaked slot's payload in its slot (XH3). `counted_io.hh`: `io_counts::reserve` and `guarded` (public). `index.hh`:
  `fenced()`, `guard_calls()` with the debug check at `live()` (C17), `merge_output_unsynced()`.
- **The slot is 184 B**, under D4's 192 B (`catalog_test` `slot_cost`): the new fields share the old padding. `streak` is a saturating 16 bits,
  `fences` 8 bits, the last replacement's time 16 bits of 2^27 ns (134 ms units; it wraps every 2.4 h, compared by difference, so a fence exactly one wrap later counts as a repeat once: harmless), the five flags bit-fields.
- **The acquire is a load of the word, not a fence.** `drain_unpin` reloads the word with acquire after its `fetch_sub` saw `closing`; it pairs with
  `reopen`'s release exchange as the ruling's fence does, and TSan models it (it does not model `atomic_thread_fence`). `drain` is an atomic, so
  the mutant that drops the acquire (H8) can only be seen as a race on non-atomic data; there is none, so it is the one mutant TSan on x86 cannot kill (below).
- **`run_replace` is a function of its own**, not a fourth `close_mode`: it has no readiness check, no prior state and no bound, and its commit chore
  runs only for an index that is not fenced. The timer retries use `after_or_post` (a host timer that throws posts at once, so a reopen or a replace
  is never lost), the form item 4's fix round made necessary (`after()` can throw).
- **S14 needed one step beyond the ruling.** The check-in publishes a finished merge, but the merge's output stays unsynced until a commit, and
  `warm_quiescent()` refuses an unsynced segment, so the index stayed hot after the check-in (measured: `diag: pending 0 uncommitted 0 demote() 0`,
  and still 0 after the host committed once more, as the commit started the next merge of the cascade). `run_demote` now runs the
  host's commit chore when only a merge's output is unsynced (`commit_merge_output`, `Index::merge_output_unsynced()`; its own commit,
  strata:f70fb2c; the review kept it, Q1). Without it S14's "demotes on the first pass after the merge" cannot hold. The review dropped the
  check-in from `run_close` (L3): see the fix round.
- A fence found by `shutdown` is not replaced: its commit fails and names the key, as before.
- Item 4's fix round (strata:3c2112f) is merged (strata:ab3f9bd). Conflicts: the constructor (timer required, `life_`), `run_wake` (the timer's
  try block and the fence catch), `finish_wake` (`failure` built before the mutex), `run_close`'s bound branch (item 4's "demote whenever the slot has
  maps" inside item 5's structure), the timers' mutex and condition variable, the tests' `totals` and `world`. The merged tests that destroyed
  a catalog with open slots (`callback_leaves_before_free`, `throwing_timer_fails_the_wake`) switch off the owner-break abort around the
  destruction, as item 5's `catalog_wake_test` already did; two item 5 sweep tests (`queued_reclaim`, `leftover_maps`) and one wake test called an
  index with no pin and now pin first (C17 found them).
- The Linux run before the review was on the local QEMU gate VM (`tools/vm/linux.sh`); the AWS lease (`tools/vm/aws-linux.sh`, the `agents` profile) needs no login and runs the fix round (below).

## Done criteria, one by one

Tests are in `test/catalog_host_test.cc` unless named; `inline_executor` throughout (the work pool is `catalog_stress_test`); the options of
each case are in the case. Mutants are `.scratch/mutants5b.py` (one or two exact edits, a one-TU build ahead of the real headers, CI's flags);
results `.scratch/mutants5b-*.txt`.

| Criterion | Test | Mutants (result) |
|---|---|---|
| S10a | `create_passes_absent`: ten reads fail `index_not_created` (nine of them without an open); a create pin inside the backoff creates; reads see it; the created index commits and reopens | H1 create obeys the negative entry; H25 absent never classified (killed) |
| S10b | `create_joins_open`: a create pin joins an attempt that runs without the flag (started inside `resolve`); both get the index, `create_reposts == 1`, no failure | H2 `finish_open` fails the waiters (killed) |
| S10c | `create_obeys_other_errors`: a create pin on an EIO entry fails fast, `resolves` unchanged | H3 create passes every negative entry (killed) |
| S10d | `resident_never_opens`: no slot, `failed` (past its retry time too), `closing`: empty pin, `table_size()` and `resolves` unchanged; `open`, `owed` granted, no wake; `opening` waits; a request pin throws | H4 inserts, H4b retries a failed slot, H4c waits on `closing` (killed) |
| S11 | `payload_in_place` on `pinned` (deleted copy and move, an atomic); the step applies a document, the waiters see it, state `opening` with 2 counted pins during the step; a throwing step fails both waiters, H sees the index's maps and the IO's descriptor still held, then all counts are 0 | H5 pins before the step, H6 index before H (killed) |
| S12 | `reopen_drains_holders`: three holders, a fence, `reopen`; by-key and by-handle pins wait; the old index is alive while holders remain (read through it); the last unpin posts the close; `gen` + 1, `made == 2`, waiters served; window tests `unpin_races_reopen_exchange` (an unpin between the load and the exchange), `drain_unpins_race` (two holders in the drain, one close), `reopen_plain_unpin`; `reopen_commits_unfenced` | H7 close posted at `reopen` with holders, H18 holder unpin is plain, H21 no drain count, H26 drain one short (killed); H8 drop the acquire: see "TSan" below |
| S13 | `fenced_idle_is_replaced`, `fenced_owed_wake_replaces`, `fenced_wake_counts_holders`, `fenced_bound_close_closes`, `reopen_backoff` (12 reopens: gaps 0, 10, 20, 40 ms ... as asserted; 10 in the first 10 s; a quiet 10 s reopens at once) | H10 demotion ends at open, H11 fenced wake is a failed wake, H12 close ends at the prior state, H9 waiters counted as holders, H13 no decay, H13b no backoff, H30 never marks replacing (killed) |
| S14 | `check_in_before_demote`: a cascade of background merges on the inline executor; the test commits nothing; the pass demotes (2 passes, 2 host commits) and a later pass closes | H14 no check-in, H27 no commit of a merge's output (killed); H14b (the close's check-in) is removed with the code (L3, the fix round) |
| S15 | `snapshot_and_flush`: five slots (open, owed, failed, open, closed after the snapshot); FLUSH-shaped loop commits k0, skips the failed and the closed key, `resolves` unchanged | H15 snapshot skips `owed` (killed) |
| S16 | `host_files_counted`: two files per open reserved on `opening.counts`; slot counts = the index's + 2; the kernel model equals the budget (T7); at the hard cap the second file's reservation throws `too_many_open` before the kernel is asked (`refused == 0`), the open fails and backs off, counts return to 0, a retry works | H16 reserve skips the slot's counts (killed) |
| C14 | the oracle (a wake posted inside a backoff aborts) over `catalog_test 1000` and `catalog_stress_test 8000 6`: clean; the no-gate variant at the composed edge aborts | O1 clean (survived, as expected), O2 killed (abort) |
| C17 | `call_without_pin_breaks`: a call with no pin counts a break, none under a pin or in a transition | H17 no pin check (killed) |
| T6 / stress | `catalog_test` schedules gain `fence_one` (a failed sync under a held pin, then `reopen` or left for the wake, the sweep or the next holder), the drain's unpin (every release in `closing`), the new edges `{demoting, closing}` and `{waking, closing}` (required), the new hook sites; `catalog_stress_test` gains a fence under the chore's turn and `reopen` of the pin from `one_op`'s handler | all |

## Text for `docs/design/cold-tier.md` revision 8 (the design branch is not this branch; for its owner)

- **D4, the host list.** `host` loses `make_payload` and gains `open_payload(opening, Index&, optional<H>&)`; `resolve` and `open_index` take a
  `const opening&` (`key`, the slot's `io_counts`, `create`). New: `reopen(pin&&)`, `snapshot()`, `Index::fenced()`, `io_counts::reserve/guarded`,
  `open_intent {open, create, resident}` on the pin calls.
- **D4, the word's new rows** (the architect's table): create pin on `{p, failed}` before its retry time with the cached error `index_not_created` to
  `{p + 1, opening}`, `gen` + 1 (the open is posted with `create`); an open that ends absent while a create pinner joined after it started, unchanged
  and reposted; resident pin on no slot, `failed` or `closing`, an empty pin; `reopen(pin)` on `{p, open/owed}` to `{p - 1, closing}`, drain `p - 1`
  (at 0 the replace close is posted); a holder's unpin in a drain, `{p - 1, closing}` (the last posts the replace close); wake ends `index_fenced`,
  `{p, waking}` to `{p, closing}`, drain = holders; demotion ends `index_fenced`, `{p, demoting}` to `{p, closing}`, drain 0; replace close ends,
  `free` or `{p, opening}` with `gen` + 1 (the reopen waits for the backoff when fences repeat). `closing` also holds the sweep's and the bound close's
  fenced slots (already `closing`).
- **D5.** The fence is replaced, not kept: the demotion, the wake, the sweep's and the bound close all meet `index_fenced` as "replace"; the replace close
  runs the host's commit chore only for an index that is not fenced and has something uncommitted, then `close()` with no readiness check. A fence within
  10 s of the slot's last replacement counts as a failed open (10 ms doubling to 10 s, a quiet 10 s starts over). Shutdown does not replace. The demotion
  checks the index in first and, when only a finished merge's output is unsynced, runs the host's commit chore (`commit_merge_output`); the idle close
  does not check in, since every idle close follows a demotion and a demotion takes only with no merge pending (L3).
  **The host's commit chore (C24)** therefore runs in the catalog at the bound close, at shutdown, in the replace close (for an index that is not fenced),
  and in a demotion when a merge's output is the only thing uncommitted (S14), always under the chore's lease with no pin held; a host's commit must
  tolerate being called with nothing applied since its last one (a WAL host writes no revision of its own for it, C25, Xapiand's).
  The create intent: a create pin passes a negative entry only for `index_not_created`; an absent index stays a negative entry for every other pin.
- **Limits.** An index created outside the catalog is seen once its negative entry's retry time passes (at most 10 s); a host creates through a create
  pin. A read-only busy index's finished merges are published by the next chore's check-in or the host's commit, not by a timer. `limits.md` has the
  rows for the drain's 2^24 holders and the reopen backoff. A resident pin that waits on an `opening`, `waking` or `demoting` slot takes that transition's
  outcome: when the open fails it gets the open's error, not an empty pin (F5); the autocommit treats it as nothing to commit. `host::wake_bound` above
  255 is refused with `invalid_argument` (N3).

## Evidence (2026-11-03, on the merged tree strata:3034c32; Mac x86_64 with Homebrew LLVM; Linux x86_64 on the gate VM)

Builds in `.scratch/{rel,asan,tsan}`, CI's flags: rel `-O2 -g` without NDEBUG, ASan+UBSan `-O1 -fno-sanitize-recover=undefined
-D_GLIBCXX_ASSERTIONS`, TSan `-O1`. Item 4's fix round and item 5's fix round (B1 to B4, strata:31da26e) are merged.

- **rel (Mac):** `catalog_host_test`, `catalog_wake_test`, `catalog_sweep_test`, `counted_io_test` ok; `catalog_test 300` ok (1,500 fences, 1,300
  replaces, 1,000 drain unpins over 300 seeds); `catalog_stress_test 3000 4` ok. The slot is 184 B (`slot_cost`; 192 B on Linux).
- **ASan+UBSan, TSan (Mac):** the same six runs, rc 0, no report (`.scratch/run-asan.txt`, `run-tsan.txt`); TSan `catalog_stress_test 20000 6`
  rc 0 (`.scratch/tsan-long.out`). The stress's drain phase (2 keys, 8 pinners, holds across a read and a spin; a fence is a failed open to the
  backoff, so each key affords 7): 24,000 pins, 5 to 13 fences, 12 to 31 holder unpins in a drain, 19 to 47 pins waited in `closing`.
- **Linux (gate VM, local QEMU):** TSan and ASan+UBSan, the five suites (`catalog_host_test`, `catalog_sweep_test`,
  `catalog_wake_test`, `catalog_test 300`, `catalog_stress_test 3000 4`), rc 0, no report (`.scratch/linux-tsan-m3.out`, `linux-asan-m3.out`).
  A Linux TSan run on the tree before the merge reported one stress CHECK (a request pin may see its slot draining after a holder's reopen), fixed in
  the test (strata:de1b8be). The VM's disk was full for a time (other agents' `bench` and `preflight` trees, 90 GB) and a link failed with ENOSPC;
  the runs above are after it cleared.
- **Mutants** (`.scratch/mutants5b.py`; results `.scratch/m5b-*.out`, the control BASE survives in every build, as it should): 31 on the Mac in
  `relc` (`catalog_host_test`, plus `catalog_test`'s schedules for the sched ones): 30 killed, H14b survives (below). TSan: H28 and H29 (the drain
  count stored after the exchange, in `reopen` and in a fenced wake) killed by the hooks `reopen.exchanged` and `replace.exchanged`; H8 survives
  (below). ASan: H7, H9, H18, H21, H28 killed (by the test's CHECK abort, which comes before the use after free). C14's oracle (a wake posted inside
  a backoff aborts), over `catalog_test 1000` and `catalog_stress_test 3000 4` in TSan: the clean variant O1 survives, the variant without the gate
  at the composed edge (O2) aborts.
- **H8 (drop the acquire in `drain_unpin`) cannot be killed by a test (H8, the review's Q2); the acquire stays.** In the C++ model a release `fetch_sub`
  that reads the exchange's value does not synchronize with it. Without the acquire nothing orders `reopen`'s store of `drain` before the holder's
  decrement, so the decrement may come first in `drain`'s modification order; the store then overwrites it, the count never reaches zero, the replace
  close is never posted, and the slot hangs in `closing` with its waiters. With the acquire the reload reads from the exchange's release sequence (every
  write of the word is a read-modify-write), the store happens before the decrement, and coherence forbids that order. TSan models atomics as reading
  the latest value, and arm64 hardware most likely orders a release RMW before a following acquire-release RMW anyway, so no run shows the difference;
  the language model, not the hardware, is what the code satisfies. The acquire costs one load on the drain path only. It is the one mutant that
  survives by design.
- **H14b (no check-in before a close) is gone with the code (L3).** Every idle close follows a demotion, a demotion takes only when `warm_quiescent()`
  holds (no pending merge), and a later write maps a segment, so the slot is hot again and demotes (with its check-in) first; an `owed` slot cannot
  merge. The reviewer's three routes (P5, P5b, P5c) are tests now (`close_meets_no_finished_merge`, `warm_close_after_write`).
- **Hot-path smoke A/B** (`bench/catalog_pin/pin_smoke.cc`, `.scratch/smoke.sh`; `quiet strict`; Mac x86_64, `-O2 -DNDEBUG`, one thread, the slot
  `open`, nine rounds of 10^6 pin and unpin pairs; three interleaved rounds of item 5 (strata:31da26e's headers), this branch twice, item 5 again
  as the A/A control): by key, the mean of the rounds' medians is 54.7 ns for item 5, 56.4 ns for this branch, 54.8 ns for the A/A control (rounds
  range 52.6 to 57.5 ns); by handle 41.3, 41.5 and 40.6 ns. By handle: unchanged. By key: this branch reads 1.7 ns (3%) above item 5 and its rounds
  (55.0 to 57.4) sit at the top of the control's range; not proved either way here, and not a gate row (`.scratch/smoke-run.out`). On the earlier
  tree (before item 5's fix round) the same A/B read 55.1, 55.1 and 53.6 ns by key (`.scratch/smoke-run-pre-merge.out`).
- **Preflight** (`.scratch/pf.sh`, the staged form of `cpp-libraries/tools/preflight.sh`, every hold under 30 minutes): on the tree before item 5's
  fix round, mac-hardened ok, mac-asan-ubsan ok, freebsd ok, 69 of 69 tests each. On the final tree (strata:3034c32): mac-hardened ok (69 of 69),
  mac-asan-ubsan ok (69 of 69), freebsd ok (69 of 69), linux ok (70 of 70, the local gate VM).
- **CI** on strata:0fa33ca (pushed): every job green, `linux-tsan`, `linux-asan-ubsan`, `linux-arm64-gcc` and `macos-*` included
  (run 41691469287).

## Fix round for the review (2026-11-03)

Review: `docs/reviews/cold-tier-step2-item5b.md`. Commit strata:a64038a (code, tests, mechanism note, limits row); this section is the ledger's.

| ID | Change | Test | Revert check (mutant in `.scratch/mutants5b.py`) |
|---|---|---|---|
| L1 (F1) | `finish_open`: `s.absent = absent && !created` (`created` is the attempt's create flag, read before the flags clear), so a create attempt's `index_not_created` is an ordinary negative entry | `create_storm_gated`: a host whose `open_index` does not create; a read fails absent, ten create pins inside the backoff: the first passes the read's entry (2 resolves, 2 opens), the other nine fail fast (`failed_fast == 9`), ten absent in all; past the retry time one more attempt | L1 (today's line): killed, 10 resolves |
| L2 (F2) | `snapshot()` loads the word, then rechecks `key_hash` (the hook `snapshot.slot` sits between them) | `snapshot_skips_reused_slot`: a freed slot with a stale `key_hash` of shard A, claimed by a key of shard B from the hook: the snapshot is empty (skipped), the next snapshot lists it. The window is reachable with the hook, so the test is deterministic, not a TSan race | L2 (the old order): killed (`snap.empty()`). The old order's race itself (a second thread's `key.clear()` on the new owner's mutex) is not reproduced; the test pins the observable half of the same ordering |
| L3 (Q3) | `run_close` no longer calls `check_in()`; the mechanism note says why it is never needed | the reviewer's routes as tests: `close_meets_no_finished_merge` (P5: demote first, then close), `warm_close_after_write` eager and not (P5c: the write maps a segment, so a demotion with its check-in comes before the close); P5b is the carried X-POOL note below | L3 (put the call back) survives, as it must: the call is dead (equivalent mutant, the former H14b); H14 and H27 (the demotion's check-in and the merge-output commit) are killed by P5's test too |
| C24 | `host::commit`'s comment lists where the catalog runs it (bound close, shutdown, replace close, a demotion when only a merge's output is unsynced); revision 8's D5 text below says so | text | n/a |
| H8 | the mutant table records H8 as unkillable (the C++ memory-model argument, above); the acquire stays | n/a | n/a |
| N3 | the constructor throws `invalid_argument` for `wake_bound > 255` | `wake_bound_refused` (255 accepted, 256 refused) | N3 (the clamp): killed |
| F4 | `check_in_before_demote` asserts `passes == 2` | same test | F4 (a check-in on every second demotion): killed |
| F5 | `open_intent`'s comment and the mechanism note: a resident pin waiting on an `opening`, `waking` or `demoting` slot takes that transition's error | `resident_meets_failing_open` (P8): the error, not an empty pin | text |
| F6 | not done (not cheap) | `payload_in_place` asserts H goes while the index's maps and the IO's descriptor are held. The reader, index, IO order after H would need destructor hooks in the index and the IO; mutants F6a (IO before index) and F6b (index before reader) survive both ASan+UBSan and the host test, so the order is unasserted (unchanged from item 3) | F6a, F6b survive |
| F7 | the drift is fixed in "What was built" (16 bits of 2^27 ns, wrapping every 2.4 h; the AWS lease needs no login) | text | n/a |

Also tests for the reviewer's other probes that the suite missed: `create_joins_in_open_index` (P2, a create pin joining inside `open_index`),
`payload_address` (P3), `fenced_owed_sweep_close` (P4), `c17_after_replace` (P6), `drain_at_shutdown` (P7). H12 (a fenced close ends at the prior
state) is now also killed by P4's test.

**Evidence of the fix round** (tree strata:a64038a plus the ledger; CI's flags; Mac x86_64 with Homebrew LLVM; Linux x86_64 on the AWS lease, Ubuntu clang 18.1.3,
`tools/vm/aws-linux.sh`, `.scratch/linux-fix.sh`, output `.scratch/linux-fix.out`):
- **Mac TSan and ASan+UBSan** (separate builds): `catalog_host_test`, `catalog_sweep_test`, `catalog_wake_test`, `counted_io_test`, `catalog_test 300`,
  `catalog_stress_test 3000 4`, rc 0 and no report in either (`.scratch/review-item5b/fixrun-{tsan,asan}.txt`).
- **Linux TSan and ASan+UBSan** (separate builds from `git archive`): the same suites with `catalog_test 1000` and `catalog_stress_test 8000 6`, rc 0, no
  report in either. TSan `catalog_test 1000`: 3,418 `reopen`, 3,285 `drain.unpin`, 767 `replace.exchanged` hook sites taken.
- **Mutants** (`.scratch/mutants5b.py relc`, one-TU builds, CI's flags): BASE survives (control); L1, L2, N3 and F4 killed (the first three are the fix
  reverted); L3 survives (equivalent, above); H14 and H27 killed; H12, H1 and H3 killed. ASan: L2 killed; F6a and F6b survive (above).
- **Preflight** (`.scratch/pf.sh`, every hold under 30 minutes): mac-hardened ok (69 of 69), mac-asan-ubsan ok (69 of 69), freebsd ok (69 of 69), linux ok (70 of 70) on the local gate VM (the AWS lease could not resume at that hour: `VcpuLimitExceeded`, the account's 96 vCPU cap, held by another agent's instance; the sanitizer runs above had it earlier).
- The reviewer's probes `probes.cc` are tests now; the old probe binaries are not rerun.

**Carried, not 5b's.**
- **C25 (Xapiand's X-POOL gate).** Xapiand's commit chore with nothing applied writes no WAL revision of its own beyond what the index commit needs; its
  autocommit treats an empty resident pin and a resident pin's open error alike (F5); no chore pin applies on an `owed` slot (next item).
- **X-POOL note (P5b).** A chore pin that applies on an `owed` `defer_io` slot gets `map_owed`, which leaves the index's `map_asked_` set until a later
  apply ends the turn. Until then `warm_quiescent()` is false, and every idle close of the slot is taken and refused (16 of 16 passes in the probe), so
  the slot is never idle-closed. This is item 4's `defer_io` contract (a `map_owed` answer asks the host to wake and retry). Under X-POOL only request
  pins apply, and they wake first, so it holds; C25 makes it explicit.
- **C26.** At the merge with item 6 `slot_cost` is fitted to 192 B on both standard libraries; not done here, as asked.

## Questions for the lead and the architect (answered by the review)

1. S14 (a decision I made; its own commit, strata:f70fb2c, easy to revert): the check-in alone leaves the index hot, because a merge's output is
   unsynced until a commit. `run_demote` and `run_close` run the host's commit chore when that is the only thing uncommitted. Keep it, or leave it to
   the host's autocommit and weaken S14?
   Answered (Q1): keep it; C24 is its contract text.
2. `run_close`'s check-in has no observable effect in any test (H14b). Keep it as ruled, or drop it?
   Answered (Q3): dropped (L3).
3. The by-key smoke reads 3% above item 5 on this host (above); the lead's measurement on `main` decides whether it is real.
4. `cold-tier.md` revision 8 is not on this branch; its text is above for the design branch's owner.

## Commits

On `phase/cold-tier-step2-item5b` (strata:0344df8 the criteria; strata:fd2ed66 the host interface; strata:ab3f9bd the merge of item 4's fix round; strata:eefc81d the Mac
proof; strata:f70fb2c the merge-output commit (S14); strata:c545ad8 the merge of item 5's fix round; strata:3034c32 the slot back to 184 B), then the ledger and test commits; strata:a64038a the review's fix round (L1 to L3, N3, C24, F4, F5), then its ledger commit;
`git log phase/cold-tier-step2-item5..phase/cold-tier-step2-item5b` lists them all. The branch is pushed; the lead watches CI.

# Item 5b: waiters whose frames die, and cancelled pins (Xapiand X-POOL step 2 review CR1, CR2)

> Status: BUILDING (2026-11-04) CR1 and CR2 built and proved on the Mac; the full floor next. Review: Xapiand
> `docs/reviews/phase7c-x-pool-step2-code.md` (CR1, CR2; probes `destroyed_awaiter`, `self_wait` in that worktree's
> `.scratch/xp2rev/`). Shape: task's `docs/design/cancellation.md` D4 (`lock(token)`) and D8 phase 3.

- Branch / worktree: `phase/cold-tier-step2-item5b` at `/Users/kronuz/code/Kronuz/strata-wt/cold-tier-step2-item5b`.
- Builds: `.scratch/pinfix/{rel,asan,tsan}` (Homebrew LLVM, CI's flags); harness `.scratch/pinfix/{bt,run,mut,mutall}.sh`,
  mutants in `.scratch/pinfix/mutants.py`.

## What was built

1. **CR1.** The waiter moves from the awaiting frame to the heap, in task's `lock_waiter` shape: the slot's list and the
   awaiter each hold a reference; `st` gains `abandoned`. `~pin_awaiter` (`abandon`) unlinks a waiter no transition has
   taken (the list is now doubly linked; `take()` clears `linked` under the shard mutex) and subtracts its pin from the
   word; a waiter already taken is marked `abandoned`, and whichever of the delivery and the destructor comes second
   releases a delivered pin. `shutdown()`'s waiters get the same treatment on `shut_list_`. `stats()` gains `unlinked`
   and `gone_pins`. How it works: `docs/mechanisms/catalog.md`, "Waiters whose frames die".
2. **CR2.** `pin_request`, `pin_chore` and `repin` take the caller's `task::cancel_token` last (default empty), in the
   shape of task's `lock(token)`: a pin that need not wait is taken whatever the token; a requested token never waits
   (the pin runs as `try_pin` and throws `task::cancelled`); otherwise the callback is registered in `await_suspend`
   after the join, and a request unlinks and uncounts the waiter and delivers `task::cancelled` (a request between
   the join and the registration continues in place); a cancel that loses to a delivery returns the delivery. The
   destructor and `await_resume` deregister first. `stats().cancels`; a `cancel` hook. How it works: catalog.md,
   "Cancelled pins".
3. **Ruling on the self-wait (CR2's second question): a task's second pin of a key does not reuse its first.** The
   catalog has no notion of a task (pins move across tasks and threads, I7), and a reused pin would hand the fenced
   index to the very pin the replace exists to keep away from it. The self-wait is the caller's hold-and-wait; Xapiand
   avoids it by keeping one pin per key per request across a fence (`search_batch` with a repeated endpoint), and ends
   it with the request's token. No semantics change, so no owner question; the architect may overrule.

## Evidence (Mac x86_64, Homebrew LLVM, RelWithDebInfo with CI's sanitizer flags, separate builds)

`catalog_teardown_test 200 300` (`test/catalog_teardown_test.cc`): window tests (destroyed while linked; inside the
open's delivery, "open.unlocked", for a pin and for an error; after a delivery posted and never run, for a pin and an
error; the middle of three waiters; a waiter on a replace's drain; a frame awaiting shutdown), a seeded property test
(200 seeds x 80 steps: frames holding or awaiting request, chore, create and resident pins destroyed at random points,
540 of them inside a delivery's window; demotions, closes, fences, failing resolves; the word's pin count checked
against the model at every quiescent point; then every frame destroyed and shutdown must end), and a threaded test
(300 rounds: frames waiting on cold opens destroyed on the test thread while a 3-thread WorkPool delivers; then
shutdown, the pool's `join`, must end within 30 s).

| Build | Result |
|---|---|
| rel | rc 0; threads: 164 frames destroyed waiting, 159 unlinked, 5 pins of gone frames released |
| asan (ASan+UBSan) | rc 0, no report; 828 / 801 / 27 |
| tsan | rc 0, no report; 983 / 947 / 36 |

**Revert check** (`mut.sh FLAVOR REVERT`: the header at strata:c39e716 with this test, its `stats()` lines dropped):
rel, asan and tsan fail at `destroyed_while_linked` (pins 1, waiters 1 after the frame died; then shutdown never ends);
asan reports the reviewer's `use-after-poison` READ in `catalog::deliver` from `run_open`. Threads only
(`REVERT threads 300`): asan `use-after-poison` on a pool thread; tsan `round 0: k1 keeps 1 pins`.

CR2's cases in the same binary: `cancel_while_waiting`, `cancel_before` (a cold key fails at once with the open
started, an open key is pinned), `cancel_between_join_and_registration` (requested from the "pin.miss" hook: continues
in place, nothing posted), `cancel_loses_race` (requested at "open.unlocked": the pin arrives, `cancels == 0`),
`cancel_during_destroy` (a token requested on a second thread, held inside the callback by the "cancel" hook while
the test thread destroys the frame), `self_wait_cancelled` (the reviewer's `self_wait`: closing, pins 2, drain 1, one
waiter; the token ends it, the task's other pin goes, the replace frees the slot); the property test requests
waiting frames' tokens at random; the threaded test requests a third of the tokens from a second thread.
Rerun after CR2 (`.scratch/pinfix/run.sh 200 300`): rel, asan and tsan rc 0, no report; threads asan 605 destroyed
waiting / 615 unlinked / 23 gone pins / 38 cancelled, tsan 720 / 723 / 58 / 68.

**Mutants** (`mutall.sh`, `mutall2.sh`, `mutall3.sh`; BASE survives as the control):

| Mutant | Killed by |
|---|---|
| M1 abandon never unlinks | `destroyed_while_linked` (pins 1 at once) |
| M2 a delivery to a gone frame keeps the pin | `destroyed_in_delivery` (pins 1) |
| M3 the destructor keeps a delivered, unresumed pin | `destroyed_after_delivery` (pins 1) |
| M4 unlink leaves the pin counted | `destroyed_while_linked` |
| M5 a delivery resumes a gone frame | `destroyed_in_delivery` (a post to a destroyed frame) |
| M6 `take()` leaves `linked` set | rel SIGSEGV; asan heap-use-after-free |
| M7 unlink uncounts as a holder's unpin | `drain_waiter` (the drain's count drops) |
| M8 shutdown's waiter not marked joined | `shutdown_waiter_destroyed` (shutdown resumes a destroyed frame) |
| M9 the list takes no reference | asan heap-use-after-free |
| C1 the callback never unlinks | `cancel_while_waiting` (pins 1 after the request) |
| C2 a requested token waits | `cancel_before` (`cancels == 1`) |
| C3 registration after the suspending exchange | `cancel_between_join_and_registration` (resumed through the scheduler, not in place) |
| C4 the destructor abandons before it deregisters | `cancel_during_destroy`: asan heap-use-after-free; tsan values (the threaded test alone did not kill it: noted) |

**Found by the full ctest:** `catalog_sweep`'s `shutdown_close_throws_bounded` destroys the catalog with a shutdown
awaiter still joined (an unfinished shutdown given up on), then the awaiter: its new destructor locked the dead
catalog's mutex (`mutex lock failed`, abort). `~catalog` now orphans the waiters still listed, as task's `~event`
does; the test is the regression (it aborts without `orphan_waiters()`). Three other sweep tests held a joined
shutdown awaiter past their catalog; they now destroy it first, the documented order.

## Design vs code

- The design (cold-tier D4) keeps the waiter "in the pin's frame"; it is now on the heap, allocated only by a pin
  that waits (the granted path allocates nothing). The design branch's owner should carry this into D4's text.

## Next

The full floor (ctest, Mac TSan and ASan+UBSan, Linux on the local VM through `tools/vm/aws-linux.sh`, AWS off), push.
Xapiand then passes the request's token to `pin()` (`DatabasePool::pin`, `pin_chore`), which is Xapiand's change.

## Blocked on

Nothing.
