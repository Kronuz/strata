# cold-tier step 2, item 2: counted IO and caps

> Status: BUILDING (2026-11-02) the developer's ledger for `docs/design/cold-tier.md` revision 6, step 2,
> item 2 (D6; CT-R8, CT-R12, CT-R27, CT-R32, CT-R37; criterion S3).

- Design: `docs/design/cold-tier.md` revision 6 on `design/cold-tier` (strata:48ea20f; the file is not on this
  branch). Review: `docs/reviews/cold-tier-design.md`, item 2 GO, C12 (CT-R37's one sentence) folded in below.
- Branch / worktree: `phase/cold-tier-step2-item2` from main strata:80e8f5a, at
  `/Users/kronuz/code/Kronuz/strata-wt/cold-tier-step2-item2`. Independent of item 1 (no rebase onto it).
- State: see Done.
- Done: (below, as commits land)
- Next: sanitizers, mutants, preflight, hand to the lead.
- Blocked on: nothing. No fuzz target: the item adds no decoder or parser (it reads `getrlimit`, two
  sysctls and `/proc/sys` numbers the kernel writes; no untrusted bytes).

## What was built

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

## S3, criterion by criterion

(filled as each is proved; see the table at the end)
