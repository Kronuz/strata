# cold tier step 2: the catalog (developer's ledger), items 1 to 3

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

# Item 6: S3-FIFO (the closing policy)

> Status: BUILT (2026-11-02) item 6 on `phase/cold-tier-step2-item6`, built ahead of item 3's gate; item 3's fix round is merged in
> (strata:07484e2). Design: `docs/design/cold-tier.md` revision 7 on `design/cold-tier` (strata:5723f3d), D4's frequency bits,
> D6's policy and caps; CT-R10, CT-R31, CT-R41; criterion S8; carried condition C10 (I3-N5 of `docs/reviews/cold-tier-step2-item3.md`).

- Branch / worktree: `phase/cold-tier-step2-item6` from `origin/phase/cold-tier-step2-item3`, at
  `/Users/kronuz/code/Kronuz/strata-wt/cold-tier-step2-item6`.
- State: built and proved (below); one criterion cannot be met as written (Blocked on). It adds a concurrency mechanism (queues and
  a ghost under one mutex, fetch-ops on the word's frequency) and no decoder, parser or format. The lead decides the review.
- Done:
  - strata:763063b the criteria (S8.1 to S8.9) in this ledger.
  - strata:81846d0 `ghost_set.hh`, the queues and `policy_pass()` in `catalog.hh`, C10, the tests (`catalog_policy_test`,
    `catalog_s8_test`, `s3fifo_model.hh`, the stress's policy mode).
  - strata:a71fad9 item 3's fix round merged (conflicts: `add_pin`, the stress's schedulers, the limits table, this ledger).
  - strata:ed9c6f4, strata:05040bd the thread hammers (second pass caller, churn, burst of closes, frequency fetch-ops) and concurrent chores.
- Next: item 5 wires `evict()` into its `pass()` (below); then T4 (the lead) with the real host.
- Blocked on: S8.5's "ghost ignored" mutant is not killed by the hit ratio under D6's rule (Findings, 1): the architect or the lead rules.

## Done criteria (the lead's; a developer never edits or weakens one)

Item 5 (the sweep) is not built, so item 6 builds the pass the policy needs: `sweep_pass()` (demote over `soft_hot`, then
close over `soft_open` in S3-FIFO order). Item 5 wraps it with the timer, the caps' crossings, reclaim, the commit chore,
idle closes and `shutdown()`.

- **S8.1 Hit ratio.** The catalog's policy code over the model's trace with stub indexes, every request taking two pins
  (a use by key, a re-pin by handle), is within 0.5 point of `s8_variants`' reference (`.scratch/arch-r4/` on
  `strata-wt/cold-tier`) at theta = 0.99 and 1.2, with and without the scan, in both windows (all 8M; the first 1M after the
  warm-up, after the scan where there is one). References (all 8M, first 1M): theta = 0.99 0.8160 and 0.8146, after the scan
  0.8159 and 0.8135; theta = 1.2 0.9583 and 0.9512, after the scan 0.9577 and 0.9510.
- **S8.2 The trace and the capacity.** 1M keys, Zipf from `mt19937_64` seeded 42, 3M requests of warm-up, 8M measured; the
  scan adds one access to every key in the order `key * 2654435761 mod 1M` at the end of the warm-up. Capacity is the
  model's, `soft_open` 110,000, held by a sweep pass after every miss with its transition and visit limits lifted.
- **S8.3 The stubs run the states.** `soft_hot` 10,000, the stubs under `defer_io`, so the sweep demotes them to `owed`
  and hits wake them; transitions run on a manual executor drained every 4 requests, so uses meet `opening`, `waking` and
  `demoting` (and `closing`) as waiters: each counter at least 1. A hit is a use that meets any state but `free` and
  `failed`.
- **S8.4 The real limits.** A separate case runs a pass with the real limits (256 slots, 16 transitions) and reports the
  overshoot over `soft_open` and the hit-ratio difference, ungated.
- **S8.5 Mutants, each killed by a window of S8.1** (the margin is 0.5 point): the ghost ignored (theta = 0.99, all 8M:
  3.1 points down); no frequency (theta = 0.99, all 8M: 3.5 down); every pin a use (theta = 0.99, first 1M after the scan:
  5.4 down); only `open` sets the frequency (no model number; the test records its window).
- **S8.6 Every request-pin row sets the frequency for a use (CT-R41, C10).** A test per row of D4's table: `open`,
  `owed` (the wake's winner, by key and by `try_pin`), `opening`, `waking`, `demoting`, `closing` (waiters), a `failed`
  retry (by key and by `try_pin`); a chore pin, a re-pin by handle and the miss's own pin set none. Mutant: only `open`
  sets it.
- **S8.7 The policy's choices** (D6), each with a test and a mutant: a new slot enters the small FIFO, or the main one when its
  hash is in the ghost; a ghost of 64-bit hashes the size of the main FIFO, a hit removing its entry; the small FIFO's head
  with the frequency set moves to the main FIFO with the frequency cleared, clear it closes and its hash enters the
  ghost; the main FIFO's head is decremented and reinserted while its frequency is set, else closed; the small FIFO is
  taken while over its 10% share, the main one otherwise; a hot candidate is demoted first and closes on a later pass;
  a candidate that is pinned, in a transition, or used since the choice is requeued, never closed; a refused close
  requeues its slot; a close leaves no queue entry behind; the decision to close is one compare-exchange on the word
  (unpinned, frequency clear).
- **S8.8 Concurrency.** The policy under threads (pins by key and by handle, a pass at full speed, opens and closes) is
  TSan-clean in a separate build, and ASan+UBSan-clean in another; the invariants at rest: every non-cold slot is in
  exactly one queue, the queues' sizes add up, the word counts exactly the pins held.
- **S8.9 Cost.** Reported, not gated: the pass's CPU per request in the trace, and a pin's cost (by key, by handle) against
  item 3's, under `quiet strict`.

## What was built, and where it differs from the text

- `include/strata/core/ghost_set.hh`: the ghost, 64-bit key hashes in a node pool, found through `hash_slots` (32-bit half, the full hash as
  the equality test), the oldest dropped at capacity, a hit removes its entry. The capacity is the main FIFO's size (90% of `soft_open`).
- `catalog.hh`: a slot carries `q`, `q_prev`, `q_next` (under `policy_mu_`, one mutex held for O(1) steps, taken on a miss and by the
  pass, never on a hit) and `to_ghost`. A slot is enqueued before its open ends (the small FIFO, or the main one when its hash is in the
  ghost), unlinked when a close ends, and put back by a refused close. `policy_pass(limits)` demotes hot slots over `soft_hot` (the maps the
  catalog holds, less the demotions in flight, oldest last-use first among the slots a 256-slot scan visits) then calls
  `evict(limits, result, over, before_close)`: while `over()`, take the small FIFO's head if that FIFO is over its tenth, else the main
  FIFO's; with the frequency set, small moves to main with the frequency cleared (`fetch_and`), main is decremented and put back at the tail;
  with it clear, a hot candidate (it holds a map) is demoted and put back, a warm one is closed by `begin_close(idx, unused = true)`, whose one
  compare-exchange tests pins zero and frequency zero; anything pinned, in a transition or used since is put back, its generation checked.
- C10 (I3-N5): `word::add_use` is a use without a pin; the `owed` to `waking` and `failed` to `opening` rows of a `try_pin` use it. A
  `try_pin` that makes a compare-exchange counts the use there, and its caller's retry passes `use = false`; one that answers `would_wait` on
  `opening`, `waking`, `demoting` or `closing` changes nothing and counts nothing (the retry is the use). Stated in `docs/mechanisms/catalog.md`.
- Differences from the text, for the architect:
  1. **The rule is total-based, as D6 says; the model's is per-FIFO.** `s8_variants` evicts the small FIFO's head when it holds a tenth, and the
     main FIFO's head when it is full, on every insertion; D6 evicts only over `soft_open`. `s3fifo_model.hh`'s `model_total` states D6 on lists;
     the catalog equals it request by request (lock-step test); it differs from the architect's model by up to +0.44 point (Findings).
  2. **The ghost takes only keys the pass closed from the small FIFO with a clear frequency** (D6's text and the model's), not every close: a
     key closed from the main FIFO, by a direct closer or by item 5's idle close is not ghosted (`to_ghost`, set by the pass before its
     compare-exchange and consumed by the close chore).
  3. **`lifted()` lifts the visit and transition limits, not the demotion scan's 256-slot window** (S8.2 says the limits are lifted). A scan of the
     whole table after every miss costs 110,000 reads per pass over 1.4M passes; the hit ratio does not depend on which hot slots are demoted.
  4. **A candidate the pass cannot close is put back at the tail of its queue** (pinned, in a transition, used since): a hot freq-0 candidate that is
     demoted first goes back to the tail of its queue, not the head.
  5. **Item 5 is being built beside this.** Its `pass()` (strata-wt/cold-tier-step2-item5) chooses closes oldest-use first and says item 6
     replaces that choice. This item builds `policy_pass()` as the stand-in the S8 test drives. Names that will meet in the merge:
     `soft_open` and `soft_hot` (host fields, 0 = no cap, as item 5's), `passes_` (mine is `policy_passes_`), `pass_limits` (mine `policy_limits`
     with `visits`, `transitions`, `scan`; item 5's are `pass_visits`, `pass_posts`). The seam: in item 5's `pass()`, replace the sorted
     candidate loop that calls `begin_close` with `evict(limits, result, over, before_close)` where `over()` is its `over_open + over_desc +
     over_vnode != 0` and `before_close(idx)` reads the slot's descriptors and vnodes for the relief it books; keep its hot-demotion as is (or
     use `demote_hot`). `evict` and `demote_hot` read `soft_open_` and `soft_hot_`, set from the host at construction.
- `test/catalog_world.hh`: `soft_open`, `soft_hot` and `remake()`. `test/catalog_stress_test.cc`: the executor takes `per_group`.

## Findings

1. **The "ghost ignored" mutant of S8.5 survives the hit ratio under D6's rule.** Ignoring the ghost's hits moves the catalog from 0.8159 to
   0.8162 at theta = 0.99 (all 8M; first 1M 0.8145 to 0.8101), and the design's rule on lists (`model_total`, no catalog) from 0.8160 to 0.8162
   (theta = 1.2: 0.9593 to 0.9581). In the architect's per-FIFO model it costs 3.1 and 1.7 points. With eviction only over the total, the
   small FIFO can grow past its tenth when nothing is promoted, so the ghost's second chance adds nothing here. Ghost behavior is proved by the unit
   tests (G1 to G4 die). Options: (a) rule D6 per-FIFO as the model does (small head out when the small FIFO holds a tenth, main head out when
   it is full; item 5's pressure closes then use the total rule), which makes S8.1's references the design's own and the mutant killable; (b) keep
   the total rule and replace that mutant's ratio kill with the unit tests. I stopped at the question and built D6 as written.
2. **The catalog beats the architect's model by up to 0.44 point at theta = 1.2** (first 1M: 0.9556 against 0.9512; after the scan 0.9543 against
   0.9510), inside the 0.5 point S8.1 allows, with 0.06 point of margin. It equals the design's rule on lists to 0.0001 in every cell, so the gap is
   the rule (Differences 1), not the implementation.
3. **Mutants of S8.5 killed by the hit ratio**: no frequency, 3.16 points down (theta = 0.99, all 8M); only `open` sets the frequency, 2.14 down
   (the architect expected near no frequency; at theta = 1.2 it is 0.33 down, so only theta = 0.99 kills it; the unit test kills it in every row);
   every pin a use, 5.43 down (theta = 0.99, first 1M after the scan).

## S8, criterion by criterion

Tests: `test/catalog_s8_test.cc` (trace and stubs), `test/catalog_policy_test.cc`, `test/catalog_stress_test.cc` (policy mode, hammers),
`test/s3fifo_model.hh` (the architect's model, the design's rule on lists, the Zipf trace). Mutants are exact edits in `.scratch/mutants6.py`
(one-TU builds with CI's flags, `-O2 -g`, asserts live; `BASE` passes).

| Criterion | Test | Result |
|---|---|---|
| S8.1 hit ratio within 0.5 point, theta 0.99 and 1.2, with and without the scan, both windows | `catalog_s8_test t099 t099s t12 t12s`; the same cells run the architect's model (reproduces the criterion's references to 0.0001) | theta 0.99: 0.8159 and 0.8145 (ref 0.8160, 0.8146); after the scan 0.8158 and 0.8134 (0.8159, 0.8135). theta 1.2: 0.9592 and 0.9556 (0.9583, 0.9512); after the scan 0.9587 and 0.9543 (0.9577, 0.9510). All within 0.5 point; the worst is +0.44 |
| S8.2 trace and capacity | 1M keys, `mt19937_64` 42, 3M warm-up, 8M measured, the scan in the order `key * 2654435761 mod 1M`; `soft_open` 110,000, a pass after every miss (and after every drain), limits lifted | peak resident 110,000, over the cap by 0 in every cell |
| S8.3 the stubs run the states | `defer_io`-shaped stubs (a map while hot), `soft_hot` 10,000, a manual executor drained every 4 requests; two pins per request | uses met opening 501,756 to 3,014,323, waking 703 to 1,098, demoting 1,637 to 3,647 (closing 0 to 8: not in the criterion's list); wakes posted 1.0M to 2.8M |
| S8.4 real limits (256 visits, 16 transitions), ungated | `real099`, `real12` | overshoot 10 and 4 slots over 110,000; hit ratio 0.8159, 0.9592 (the lifted cell's, to 0.0001) |
| S8.5 mutants | `.scratch/mutants6.py`, `S8-*`; the S8 mutant `S8_EVERY_PIN_USE=1` | no frequency killed (-3.16); only `open` killed (-2.14); every pin a use killed (-5.43); ghost ignored SURVIVES (Findings 1) |
| S8.6 every request-pin row sets the frequency | `use_rows`: open, a handle re-pin and a chore pin (none), demoting waiter, owed (by key, by `try_pin`), waking waiter, opening waiter (the miss's pin none), closing waiter, failed retry (by key, by `try_pin`), saturation at 3 | F4all (only `open`), F4, F4b, F4c, C10a, C10b, F6 (no saturation) killed |
| S8.7 the policy's choices | `ghost_oracle`; `lockstep` (60,000 requests at theta 0.99, 1.2, 0.6, each request equal to `model_total`); `ghost_and_entry`, `promote_and_decay`, `hot_first`, `pinned_head`, `used_since`, `closed_behind_the_pass`, `refused_close`, `direct_close`, `demote_hot`, `policy_limits_hold` | G1 to G4, P1 to P14, H1 to H4, L1, L2, F0, F5 killed (33 of 34; L3 below) |
| S8.8 concurrency | `catalog_stress_test`: policy mode with a second pass caller and three concurrent chores, `freq_hammer`, `churn_hammer`, `burst_closes` | TSan and ASan+UBSan below; thread mutants T-enq, T-unl, T-req, T-pick, T-ghost, T-pass, T-clr all killed under TSan (L3 dies as T-pass) |
| S8.9 cost | `.scratch/pinbench.cc` | below |

## Evidence


