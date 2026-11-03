# strata review: cold tier step 2, item 5 (the sweep and shutdown)

- Date: 2026-11-03.
- Reviewed: `phase/cold-tier-step2-item5` at strata:ccf34cd, `git log fd95fef..ccf34cd` (17 commits; item 4's
  fix round, strata:960c10d on its branch, not merged here). `include/strata/core/catalog.hh` (`start`,
  `post_pass`, `pass`, the budget listener, the 1 Hz timer, `erase_failed`, `shutdown`, `run_close` with
  `close_mode`, `after_close`, `retire`, `release_chunks`, `~catalog`), `test/catalog_sweep_test.cc`,
  `test/catalog_stress_test.cc`, `test/catalog_world.hh`, `docs/mechanisms/catalog.md`, the item 5 section of
  `docs/status/cold-tier-step2.md` (`:652-782`).
- Checked against: `docs/design/cold-tier.md` revision 7 (strata:5723f3d on `design/cold-tier`) D4, D5 (warm to
  cold, the bound, shutdown), D6 (caps, demotion, the sweep), I9; S7 and C15 as the ledger numbers them
  (`cold-tier-step2.md:666-694`); my reviews `cold-tier-step2-item3.md` (C15: I3-N2, I3-N3, I3-N6),
  `cold-tier-step2-item4.md` (C18, C19, I4-B1, I4-N2), `cold-tier-xpool-host.md`, and item 6's
  `cold-tier-step2-item6-ghost.md` (G6); Xapiand `docs/design/phase7c.md` D16 (the 35 s stop bound, exit 75) and
  `docs/status/phase7c-task-wave.md:14` (the four-phase teardown).
- Checked by: Homebrew LLVM 23.1.2 clang++ on macOS x86_64, RelWithDebInfo, `.scratch/review-item5/b-tsan`
  (`-fsanitize=thread`) and `b-asan` (`-fsanitize=address,undefined -fno-sanitize-recover=all`).
  - macOS TSan and ASan+UBSan: `catalog_sweep_test` (27 cases), `catalog_wake_test`, `catalog_test 300`,
    `catalog_stress_test 3000 4`: all ok, zero reports (`runs.txt`, `run-{tsan,asan}-*.out`).
  - Linux TSan (clang 18, `git archive ccf34cd`, `linux/linux-tsan.out`) ran on strata's local Linux VM
    (`tools/vm/linux.sh`, port 18772), not the lease: the lease's AWS session had expired and `aws-linux.sh` fell
    back to the VM. It ran without a host hold (my error), about 45 minutes of up to two cores.
    `catalog_sweep_test`, `catalog_wake_test`, `catalog_test 300` ok; **`catalog_stress_test 3000 4` fails** (B4).
  - Probes (`probes.cc`, ASan and TSan, macOS and Linux): `shutdown_vs_miss`, `shutdown_stuck_no_sweep`,
    `bound_after_chore_commit`, `reused_slot`, `over_open_by_one`, `shutdown_close_throws_forever`, `t2_window` at
    per_group 1 and 3 against the header and T2's mutant (`inc-t2/`); `stress_pg.cc` (the stress with the group's
    `per_group` from `PG`, and a print of any slot caught mid-transition) at 1 and 3, header and mutant.
  - Not run: the developer's 41 mutants (I reran T2 only); FreeBSD; arm64; the preflight; benchmarks; fuzzing (no
    decoder). The second Linux pass (`linux-tsan2.sh`, same VM) was stopped after three stress runs once I saw
    where it ran; its probes and the remaining stress runs did not run.

## Verdict

**REJECT: four blockers, all small. Shutdown can report done with a pin granted after it (B1); a soft-cap
crossing demotes the whole hot set for one close (B2); a stuck close at shutdown is retried only by a sweep the
fixture itself stops (B3); and the stress fails and hangs on Linux TSan at this commit (B4).**

What holds, and reproduces:
- **S7.1 to S7.4, S7.6, FREE and C15.2 to C15.5 hold as the ledger's table says** (`cold-tier-step2.md:732-747`):
  every sweep case passes under both sanitizers on macOS and Linux TSan. The pass reads only atomics before its
  compare-exchange (`catalog.hh:415-436`); demotion and close take `{0, ...}` by compare-exchange
  (`:491-520`); the close chore checks `close_ready()` and maps zero after it (`:1184-1206`); a queued reclaim
  is posted and the slot ends at its prior state (`:1203-1206`); the erase runs under the shard mutex and
  re-checks `key_hash` (`:1329-1353`).
- **C18 holds** (the bound close through the pass): `pass_closes_at_the_bound`, and probe `bound_after_chore_commit`
  shows item 4's B1 shape (a host commit on `owed` leaves maps 1, nothing uncommitted) closing on the first pass
  through the C19 demotion in the close chore (`:1186-1189`): `state=3 maps=1 failures=8` then `state=0
  bound_closes=1`, macOS and Linux.
- **C19 holds**: `owed_with_maps_closes`, and the same path above.
- **C15.3 holds**: probe `reused_slot` on a slot freed and taken by another key: frequency 0, relief counts 0/0/0,
  `failing()` 0, and the old key's handle goes stale and re-pins by key onto another slot.
- **The catalog is correct with its group running chores in parallel.** `stress_pg` at `per_group` 3: zero TSan
  reports on macOS and Linux (`sp-stress_pg-3.out`, `linux/linux-tsan.out`). This settles open item 1 (below).

What blocks: B1, B2, B3, B4.

## Findings

**B1. Shutdown completes while a pin by key is between its `shutting_` check and its miss.** `acquire_impl`
reads `shutting_` once, before the shard mutex (`catalog.hh:731`); the miss inserts and counts the slot in
`occupied_` later (`:812-849`, `note()` at `:843`). `maybe_complete_shutdown` completes when `occupied_` is zero
(`:1408`). A shutdown that starts in between finds no slot, completes, and the miss then opens a slot and hands
out a pin.
- Probe `shutdown_vs_miss` (deterministic: the `pin.miss` hook joins a shutdown): `shut_down()=1, pin granted=1,
  slot state=2 (open) pins=1 occupied=1`, macOS ASan and Linux TSan. The slot does close later (the unpin kicks
  it), so the stress's final counts pass; but the awaiter has already resumed.
- Why it matters: S7.7's "the task resumes when no slot is left" is the one guarantee that makes destruction
  safe. A host that destroys the catalog when shutdown resumes (D5's contract) then has a live pin into freed
  memory, which is C15.1's use-after-free reached without any host bug. The same window opens whenever the last
  slot frees while a pin is in flight, not only on an empty catalog.
- Fix: decide `catalog_closed` and the miss's insert under the same shard mutex (re-check `shutting_` there),
  and let shutdown complete only after `join_shutdown` has passed every shard mutex once after setting
  `shutting_` (a flag `maybe_complete_shutdown` reads), so a miss either counted itself first or sees
  `shutting_`. The probe's case goes into `catalog_sweep_test` with its mutant (the check outside the mutex).

**B2. Over a closing cap the pass demotes every hot candidate, not the one it means to close.** For a hot
candidate the pass demotes when any of `over_hot`, `over_open`, `over_desc`, `over_vnode` is nonzero
(`catalog.hh:442`), but takes the demotion's relief only off `over_hot` (`:446`). `over_open` stays above zero,
so the next candidate is demoted too, and the pass reposts (`:459`) and demotes more.
- Probe `over_open_by_one`: 40 hot slots, `soft_open` 39 (one over), nothing idle. macOS: 40 demotions and 1
  close (`passes=8 reposts=2 demotes=40 closes=1`, nothing left hot); Linux: 32 demotions, 1 close.
- Why it matters: D6 says a hot candidate "is demoted first and closes on a later pass": one candidate for one
  close. As built, one index over `soft_open` (or one descriptor over its share) turns the hot set warm, and under
  `defer_io` every one of those indexes pays a wake (a promote of all its records) on its next request: the
  many-indexes workload's worst case, at every crossing. G6 keeps this loop when item 6's `evict()` merges, so
  the defect does not go away with S3-FIFO.
- Fix: a hot candidate demoted for a closing cap counts as that close's relief (`over_open` by 1, `over_desc` and
  `over_vnode` by its counts) and books it in flight until the slot closes or a pin takes it, so the next pass
  closes that slot instead of demoting another. The probe's case in `catalog_sweep_test` (one demotion, then one
  close), and its mutant (today's line).

**B3. A stuck close at shutdown is retried only by the sweep.** `after_close` leaves a slot whose `close()`
left `closing()` true for the pass to retry (`catalog.hh:1243-1248`, `:423-427`). Shutdown arms nothing of its
own for it. The fixture's own shutdown order stops the sweep first (`catalog_world.hh:210`), as does
`~catalog` (`:346`).
- Probe `shutdown_stuck_no_sweep`: a view kept past its pin, shutdown begun: `sd_done=0 state=6 stuck=1
  timers=0`; the view released and 10 s of settled timers later, still `sd_done=0 state=6`; only `start()` then
  finishes it. macOS and Linux.
- Why it matters: the host bug D5 tolerates (a view kept past its pin) becomes a shutdown that never ends after
  the bug's view is gone. A close that throws and a busy lease already retry through `after()` at shutdown
  (`:1164-1166`, `:1227-1229`); the stuck close is the one path that does not.
- Fix: in shutdown mode, `after_close` arms a `try_finish_close()` retry through `after()` (64 ms) instead of
  relying on the pass. The probe's case in `catalog_sweep_test` with its mutant.

**B4. The stress fails on Linux TSan at this commit.** `catalog_stress_test 3000 4`: five `CHECK`s at
`catalog_stress_test.cc:241` (a slot in neither `free`, `open`, `owed` nor `failed` at the quiescence check);
`stress_pg` at `per_group` 1 (the same test, the knob at its default): eight such `CHECK`s and a TSan data race
on `catalog_detail::waiter::~waiter()` (`catalog.hh:90`); and the T2 mutant's build at `per_group` 1 timed out
at 900 s (`linux/linux-tsan.out`). The ledger's Linux run was on strata:e20219b; the final tree's was not run
(`cold-tier-step2.md:767-768`).
- macOS does not reproduce it: `stress_pg` at `per_group` 1 under TSan, 12 runs (6 with the sweep stopped), zero
  slots mid-transition and zero reports (`repro-mac.out`). Two causes are visible in the code, unconfirmed on
  Linux (the second pass below prints the states and runs with the sweep stopped). The sweep's 1 ms timer keeps posting passes after the pinners join
  (`catalog_stress_test.cc:171` starts it; nothing stops it before `:234`), so `wait_chores()` can return
  with a demotion or close just begun. And the shutdown phase's joiner registers awaiters with `await_ready()`
  and never suspends them (`:354-355`); it leaves when `shut_down()` turns true, which `maybe_complete_shutdown`
  sets before it delivers to the waiters (`catalog.hh:1413-1416`), so the main thread destroys the deque of
  awaiters while `deliver` may still write them.
- Fix: stop the sweep and drain before the quiescence checks (`stop_sweep()`, then the world's quiesce once item
  4's merges in); have the joiner suspend each awaiter on a scheduler it drains, or wait for resumption before
  destroying them. Then a Linux TSan run of the four tests on the final tree, and the hang explained or gone.
- The second cause is confirmed: `linux/stress-stop-2.out` (the sweep running) reports the race with the main
  thread's `~shutdown_awaiter` at `stress_pg.cc:376` against `maybe_complete_shutdown`'s `deliver`
  (`catalog.hh:1416`) on a pool thread, the awaiter allocated by the joiner (`:357`).
- **The stress also hangs on Linux, cause not found.** With the sweep running at `per_group` 1, two of four runs
  never finished (T2's build at 900 s in `linux-tsan.out`, which at `per_group` 1 behaves as the header; and
  `stress-stop-1.out`, empty after 7 minutes, killed); the one run with the sweep stopped first finished
  (`stress-stop1-1.out`). The pinners abort after 60 s and `shut_down()` gives up after 60 s, so the hang is
  outside both: `wait_chores()` while the sweep's 1 ms timer keeps the group busy is the first suspect, a wait
  with no idle moment. Not confirmed. L4 requires it gone or explained, on Linux.

## Rulings on the developer's open items (`cold-tier-step2.md:697-721`, `:765-767`)

1. **T2 is benign as tested, and the code is right; the test cannot reach the race by construction.** The
   stress's catalog posts every chore to one group, and `work_pool_executor` runs one item of a group at a time
   by default (`work_pool_executor.hh:32`, `per_group = 1`), so no two shutdown closes ever overlap and their
   `push_back`s are ordered by the group. The stress's "met another naming" counts even arrivals, not
   overlaps: an arrival that waited out its 30 ms alone still pairs with the next (`catalog_stress_test.cc:317-321`).
   Probe `t2_window` (a true rendezvous of up to 2 s): at `per_group` 1, zero overlaps in three namings (each waited its 2 s alone); at 3, four namings and
   three overlaps, the header clean, the mutant flagged by TSan in `run_close`'s `push_back` (macOS; Linux
   through `stress_pg-t2` at 3). Nothing in the executor's contract (`executor.hh:21-43`) promises one item per
   group, and a host may choose another value; the catalog holds at 3 (above). Required (C21): the stress runs
   with `per_group` 2 or more (at least its shutdown phase), its pairing counter counts overlaps, and T2 dies
   there. Keep the mutex.
2. **Oldest use first until item 6: accepted.** Item 6's `evict()` replaces the sorted candidate loop (G6). B2 is
   not about order and is fixed here, since G6 keeps this hot-demotion path.
3. **`soft_hot` caps hot indexes, as D6 and CT-R36 say ("the hot ones"), not mapped records.** A record cap per
   catalog only restates the budget's maps soft cap in another place, and for a ten-segment index it keeps a tenth
   of the indexes the host asked for. Hot indexes are countable without a hook in the index: the slot's maps count
   moves through `counted_io`, and its 0-to-1 and 1-to-0 edges (the `fetch_add`/`fetch_sub` results) maintain a
   catalog count of hot slots at one more atomic per edge, none per map. Condition C22: before item 6 merges
   (T3 and T4 measure against it).
4. **No clock in the catalog; a reported abandonment, at the host's deadline.** The 35 s belongs to Xapiand's
   `join()` (D16), which owns the whole stop's deadline and abandons with exit 75; a second bound inside the
   catalog cannot know how much of it is left. What the catalog owes:
   1. Shutdown converges by itself once its blockers clear: B3 (a blocker), and the busy lease's retry doubles
      to 64 ms like the wake's instead of 1 ms forever (`catalog.hh:1165`).
   2. The host can see what holds it: a report of every slot not yet free (key, state, pins, and why: pinned,
      lease busy, `close()` throwing, a view held), for Xapiand to log before it exits.
   3. The note and D5 say an unfinished shutdown is abandoned by exiting, never by destroying the catalog: with
      a throwing `close()` the retry chain re-arms every 64 ms forever (probe `shutdown_close_throws_forever`:
      200 firings, `sd_done=0`, 200 close failures), and `~catalog`'s loop (`:351-355`) waits on that chain.
   Items 2 and 3 are condition C23, before X-POOL.
5. **A re-pin by handle fails after shutdown begins: accepted.** A re-pin is a new pin; letting it through lets
   one request extend shutdown without bound. The host's own chore pins (an autocommit) fail the same way, which
   is right since shutdown commits. The note says so (`catalog.md`, "Shutdown").
6. **Destruction with a pin held: accept the debug report; impossibility comes from the order, not a mechanism
   in the pin.** Making the pin's release safe after destruction would need a shared reference per pin, a
   read-modify-write on one line for every pin of every key, which T4's hottest key forbids. Instead:
   1. Once B1 is fixed, a shutdown that has resumed proves no pin exists and none can be taken, so the contract
      "destroy only after `shutdown()` resumes" makes the use-after-free unreachable without a host bug.
   2. The host's teardown destroys every root that can hold a pin before it awaits shutdown: Xapiand's
      four-phase order (chores cancelled and drained, hosts closed, roots destroyed, then the indexes;
      `phase7c-task-wave.md:14`). X-POOL's teardown test asserts it under ASan (condition on X-POOL, carried
      with C17).
   Release builds only count the break (`owner.hh:53-56`); that is accepted.
7. **Pending relief for closes and demotions: accepted** (184 B a slot, as the ledger reports; not rechecked),
   with B2's booking added to it.

## Notes (not blocking)

- **N1.** `shut_down()` turns true before the awaiters are delivered (`catalog.hh:1413-1416`); the note should say
  a host acts on the awaiter's resumption, never on polling `shut_down()` (B4's joiner is that mistake).
- **N2.** A stuck close keeps counting in `closing_`, so `over_open` treats it as relief forever (`:403-405`). A
  host bug, but the pass then never closes a replacement; consider leaving it out once `stuck` is set.
- **N3.** `start()` after `stop_sweep()` returns without re-arming (`:364`); either document it as one-shot or
  allow the restart.
- **N4.** The ledger's "4 of them met another naming" lines (`catalog_stress_test.cc:371`) are counts of even
  arrivals (ruling 1); reword once C21 counts overlaps.

## Blocking items / Required changes

- **L1 (B1).** `catalog_closed` decided under the shard mutex at the miss; shutdown completes only after it has
  passed every shard mutex once; `shutdown_vs_miss` as a test with its mutant.
- **L2 (B2).** A hot candidate demoted for a closing cap books that cap's relief until it closes or is pinned;
  `over_open_by_one` as a test (one demotion, one close) with its mutant.
- **L3 (B3).** The shutdown close retries a stuck close through `after()`; `shutdown_stuck_no_sweep` as a test
  with its mutant.
- **L4 (B4).** The stress stops the sweep before its quiescence checks and suspends its joiners' awaiters; the
  Linux hang explained or gone; a Linux TSan run of the four tests on the final tree (three stress runs), zero
  reports, no timeout.

## Conditions

- **C21 (ruling 1).** The stress at `per_group` 2 or more; the pairing counts overlaps; T2 dies.
- **C22 (ruling 3).** `soft_hot` counts hot indexes by the maps count's edges, before item 6 merges.
- **C23 (ruling 4).** The shutdown report of slots not yet free; the busy-lease retry doubling; D5 and the note
  say an unfinished shutdown is abandoned by exit; before X-POOL.
- **C24 (ruling 6).** X-POOL's teardown destroys every pin-holding root before awaiting `shutdown()`, tested
  under ASan; carried with C17.
- Carried: item 4's fix round (I4-B1's test, I4-N1 timer required, I4-N2, I4-N5, I4-N6) merges before this lands
  and the four tests rerun; G6's four corrections at item 6's merge (C2 there); C16, C17 as in item 3.

## Probes and artifacts

All in `strata-wt/cold-tier-step2-item5/.scratch/review-item5/`, kept:
- `cfg.sh VARIANT`, `runtests.sh VARIANT` (the four tests; `runs.txt`, `run-{tsan,asan}-*.out`); builds `b-tsan`,
  `b-asan`; `probe.sh VARIANT SRC [INCLUDE_DIR]` builds a TU with a build's flags into `bin-VARIANT/`.
- `probes.cc`: `shutdown_vs_miss` (B1), `shutdown_stuck_no_sweep` (B3), `bound_after_chore_commit` (C18, C19),
  `reused_slot` (C15.3), `over_open_by_one` (B2), `shutdown_close_throws_forever` (ruling 4), `t2_window
  [per_group]` (ruling 1). `owed_uncommitted_idle` is a probe error (an apply on `owed` answers `map_owed`
  under `defer_io`), kept and ignored. Outputs `probes-asan.out`, `p4-asan.out`, `t2-*.out`, `t2-mac.out`.
- `stress_pg.cc`: the stress with `PG` (per_group) and `STOP` (stop the sweep before the checks) and a print of
  slots caught mid-transition; `sp-*.out`, `sp-mac.out`, `repro-*.out`.
- `inc-t2/`: T2's mutant header.
- `linux-tsan.sh` (`linux/linux-tsan.out`) and `linux-tsan2.sh` (stopped; `linux/stress-stop-2.out` holds the race
  report). Both target `aws-linux.sh`; with the lease's session expired they reach the local VM, so run them only
  inside a host hold or after `aws login`.
- Rerun B1: `quiet light ct5-review 30 "item 5" -- ./probe.sh asan probes.cc && bin-asan/probes shutdown_vs_miss`.
