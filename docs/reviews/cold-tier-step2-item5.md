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

## Re-check

- Date: 2026-11-03.
- Reviewed: `phase/cold-tier-step2-item5` at strata:d4c503f (code strata:f6fd39f; strata:31da26e and strata:d4c503f are
  ledger-only), the fix round strata:5b696df (main with item 4 merged in), strata:7d03120, strata:e3157b2, strata:9b94267,
  strata:77459ce, strata:f6fd39f; the ledger's "Fix round" (`docs/status/cold-tier-step2.md:859-916`); `docs/mechanisms/catalog.md`
  "Shutdown" (`:140-166`); `docs/design/limits.md:67`; the developer's mutant results (`.scratch/mutants5-{relc,tsan}.txt`).
- Checked by:
  - macOS, Homebrew LLVM clang++, RelWithDebInfo, my builds `b-tsan` and `b-asan` rebuilt at strata:f6fd39f (`build7.sh`,
    under a light hold): `catalog_sweep_test`, `catalog_wake_test`, `catalog_test 300`, `catalog_stress_test 3000 4` four times
    (per_group 2 three times, 3 once), each under TSan and under ASan+UBSan: all rc 0, zero reports (`runs7.txt`,
    `run7-{tsan,asan}-*.out`).
  - Linux TSan, clang 18, on the AWS lease (`ip-172-31-36-43`, not the local VM), `git archive f6fd39f`
    (`linux-tsan7.sh`, `linux7/linux-tsan7.out`): the three tests ok, the stress three times at per_group 2 and once at 3,
    2 to 3 s each, zero reports, no timeout. Probes (`linux-probes7.sh`, `linux7/linux-probes7.out`) as below.
  - Probes, new in `probes7.cc` (macOS TSan and ASan+UBSan, Linux TSan; `probes7-{tsan,asan}.out`): R1 to R6 (B1, one per path
    that grants a pin after the flag), R7 (B1 under threads), R8 (B2's booking and the pass window), R9 and R10 (the
    hold-off), R11 (C23's close cap). The review's own probes as rerun at this round's builds (`probes6.out`).
  - Not run: the developer's mutants (their results read, not rerun; my R8 is the one new mutant-shaped check); FreeBSD; arm64;
    the preflight; benchmarks; fuzzing (no decoder).

### Verdict

**REJECT: one new blocker, small. B1, B3, B4, C21, C22 and R2 are closed; B2's fix demotes one slot per close, but the
booked close then waits for the pass's cursor to come back, N / `pass_visits` periods with the cap over and every other
close held back (B5). C23 is partial: the throwing close's 8-try cap breaks the convergence the note claims (C25).**

### Status

- **B1: closed** (`catalog.hh:920-929`, `:1601`, `:1613-1614`, `:732`). The handshake is sound: the miss's `fetch_add` and its
  load of `shutting_` are sequentially consistent and in that order; shutdown's exchange and its load of `occupied_` likewise; in
  the single total order either the miss's load follows the exchange (it sees the flag and fails with `catalog_closed`) or
  shutdown's load follows the miss's add (it sees the slot; every later change of `occupied_` is a read-modify-write, so the
  count stays above zero until that slot frees). The other grants never need the handshake: each happens on a slot already
  counted. A slot is counted from the miss's add until its transition to `free` (`note()` no longer counts `free` to `opening`,
  and line 972 is the only edge out of `free`); a slot in the table is never `free` (the free edge and the erase share one
  critical section, `:1505-1517`, `:1538-1548`); a handle needs `open` and its `gen` (`:990-994`), and both edges into `free` bump
  `gen`. Every path probed, each with shutdown joined between the pin's flag check and its grant:
  - R1 by key on an open slot (join at `pin.key`, under the shard mutex): the kick closes the slot, the pin waits, the close's
    end reopens it for the waiter (`:1507-1520`); granted, `shut_down()` false and `occupied` 1 while held, done after release.
  - R2 by handle (join at `pin.handle`): alone, the fast path's compare-exchange loses to the kick and falls back to the key;
    with another pin held, the fast path wins. Both: shutdown waits for the pin.
  - R3 a waiter on `demoting`, R4 on `opening` (the miss's open), R5 on `waking` (defer_io), shutdown joined before the chore
    runs: each handoff grants the pin, and shutdown completes only after its release.
  - R6 a negative entry's retry (`failed` to `opening`) while the joiner's `erase_failed` waits on the shard mutex the pin holds:
    granted on the counted slot, shutdown waits.
  - R7 under threads (pins by key, chore pins and re-pins by handle on 8 keys from three threads, opens failing, the sweep at
    microsecond idles, shutdown at a random moment): 200 rounds each at per_group 2 and 3 on macOS (TSan and ASan), 300 each on
    Linux TSan; about 3.3 M pins granted, 2,234 of them after the flag; zero pins held while `shut_down()` was true, every
    shutdown completed, every slot `free` with zero pins after.
  - The developer's reason for rejecting my fix holds only for test hooks (production never joins shutdown under a shard
    mutex), but the handshake is the better mechanism anyway: no pass over every shard, one sequentially consistent
    read-modify-write on the miss, which was already a read-modify-write. Accepted. N5 below is a formal gap in two side paths.
- **B2: partially closed.** The defect as reported is gone: a hot candidate demoted for a closing cap books its close
  (`:755-772`, `:486-496`), and `over_open_by_one` demotes 1 and closes 1 with 39 still hot (`catalog_sweep_test.cc:925`; my P8 in
  `probes6.out`). The booked close itself is late: B5.
- **B3: closed** (`:1437`, `:1459-1467`, `:1485-1487`, `:1575-1578`). `shutdown_stuck_no_sweep` covers both orders (stuck before and
  after shutdown began, never `start()`ed) and my P3 completes once the view goes, without the sweep (`probes6.out`).
- **B4: closed.** The stress's quiescence stops the sweep first (`catalog_stress_test.cc:269`), its joiners are tasks resumed on
  schedulers that run until every one resumed (`:392-421`), and the hang's cause is found and fixed in the catalog, not only in
  the test: the developer's gdb stacks show `wait_chores()` against a pass reposting forever on refused closes (183,010 reposts
  of 183,198 passes), and the hold-off (`:1314-1317`, called at `:1307`, `:1402`, `:1413`, skipped at `:467` and `:583`) ends it.
  Linux TSan on the lease: the four tests and the stress four times, zero reports, no timeout. On the hold-off, as asked:
  - It does not hold back a close the caps need beyond what the refusal already does. It applies only to the slot that
    refused; the same pass and its reposts go on to the other candidates, since the refused slot's relief is unbooked at its
    end. A breach that lasts a period with every candidate refused could not be relieved earlier anyway. It never shortens a
    failed commit's 10 s probe (`:1316` keeps the later time) and delays the bound close by at most its own period (`:583`).
  - One imprecision (N6): on a real clock the effective hold-off is two periods, not one. The period's timer is armed when the
    pass ends (`:1634`), and the chore that refuses runs after that, so `probe_at` lands just after the next timer. R9 (the
    chore 1 ms after its pass, the reason cleared at once): retried at the second periodic pass. The tests use a manual clock
    on which no time passes in the chore, so `refused_no_spin` cannot see it.
  - The wake's backoff and C14's single gate are untouched: the gate reads only `streak` and `retry_at` (`:1077-1082`); pins never
    read `probe_at` (it is written at `:956`, `:1054`, `:1316`, `:1398`, `:1504`, read only by the pass and `begin_bound_close`). R10:
    an `owed` slot held off by a refused idle close starts its wake on the next pin by key at once, `wake_gated` 0.
- **C21: closed.** `per_group` defaults to 2 (`catalog_stress_test.cc:25`), the naming hook counts closes that met another inside
  it at once, and the check requires an overlap when two or more were named (`:432`). Across my 16 shutdown phases (macOS and
  Linux, per_group 2 and 3): 8 to 15 namings per phase, all but at most one of them overlapped. T2 dies by TSan
  (`mutants5-tsan.txt`); T3 needed the two tight joiners to die.
- **C22: closed** (`resource_budget.hh:225-227`, `:371-373`; `catalog.hh:429-432`). The edges are taken from the slot count's own
  read-modify-write, so concurrent reservations count each hot slot once, and a 1-to-0 edge always follows its 0-to-1 edge's
  increment (the count reaches zero only once the reservation that raised it is released). `soft_hot_counts_indexes` (four-segment
  indexes, `soft_hot` 2: one demotion) passes under both sanitizers. N7 notes one unreachable edge.
- **C23: partially closed.**
  - Closed: the busy lease's retry doubles 1 ms to 64 ms (`:1343-1346`; `shutdown_report_and_lease_backoff` reads 1, 2, 4, 8, 16, 32).
  - Closed: `shutdown_report()` (`:676-704`) reads each key under its shard's mutex after re-checking `key_hash`, so a slot
    re-keyed meanwhile is skipped, never read torn. The reasons are right in every case I made: `pinned`, `lease_busy`,
    `view_held` (the test), `close_throws` and `close_failed` (R11, `why=4`).
  - Closed on this branch: the note says an unfinished shutdown is abandoned by exiting (`catalog.md:160-162`); D5's wording is
    the lead's, on `design/cold-tier`.
  - Open (C25): the throwing close's 8 tries. After the eighth the slot stays `closing`, no timer is armed, and shutdown never
    completes; the report names it `close_failed`, so the host's deadline logs it and exits. But R11 (`close()` throws 8 times,
    then would succeed) shows shutdown unfinished for good after the error cleared, which contradicts the note's "it converges
    by itself once what holds it clears" (`catalog.md:153-156`) and my ruling 4.1. The cap's only effect is that `~catalog`
    returns after an abandoned shutdown, a path the same note forbids. So the limit is not load-bearing (`limits.md:67` calls it
    an estimate).
- **R2: closed** (`:1169-1186`, `:1212-1213`). `unposted_repost_fails_the_wake` passes on macOS (both builds) and Linux TSan, and
  Y17 dies.

### New blocking items

**B5. A close booked by a demotion waits for the pass's cursor to come back, with the cap's relief held meanwhile.** The
demotion's end posts a pass when the booking stands (`catalog.hh:1150`), but a pass serves only the `pass_visits` slots from
its cursor (`:449-452`), which has moved past the demoted slot. Until the cursor returns, `pending_open_` counts the booked
close as relief (`:437`), so `over_open` stays at zero and no pass closes any other slot either.
- Probe R8 (`b2_due_window`): slots 0 to v-1 hot and oldest, the rest warm, `soft_open` n-1. With n 64 and 8 visits the cap is met
  after 8 periodic passes; with n 256, after 32 (macOS TSan and ASan, Linux TSan). Every pass in between visits warm, idle
  candidates that would have met the cap at once (the reviewed code closed one on the second pass). The 7 extra demotions at
  n 256 are idle demotions after 30 s of aged clock, not the booking.
- Why it matters: the delay is n / `pass_visits` periods. On the many-indexes workload (a million indexes, the default 256
  visits, a 1 s period) that is about 3,900 s: an hour with the soft cap over and every close for it held back, and a booking
  added for each new excess meanwhile.
- Fix: begin the booked close at the demotion's end when the booking still stands (no pin, maps zero): `begin_close(idx)` in
  `end_at` where it now posts a pass, so the relief and its close move together. Alternatively a small list of booked slots
  that each pass serves before its window. R8 as a test with `pass_visits` below the slot count (the cap met by the second
  periodic pass), and its mutant (today's `post_pass()`).

### What approval needs

- L5 (B5): the booked close begins at the demotion's end (or is served by the next pass whatever its window); R8 as a test with
  its mutant; the four tests rerun under TSan and ASan+UBSan. A rechecker can confirm it.

### Conditions

- **C25 (C23, ruling 4.1).** A shutdown `close()` that throws is retried while shutting down, as the stuck close is (64 ms apart),
  and reported `close_throws` meanwhile; `close_retries` and its `limits.md` row go, or the note states the give-up and names a
  load-bearing reason. R11 as a test (throws 9 times, then succeeds: shutdown completes). Before X-POOL, with C23.
- Carried: C23.3's D5 wording (the lead's, on `design/cold-tier`); C24 with C17 on X-POOL; G6's corrections at item 6's merge
  (C2 there); C16, C17 as in item 3.

### Notes (not blocking)

- **N5.** Two formal gaps beside the handshake, safe on x86_64 and arm64 hardware as built, free to close. First,
  `maybe_complete_shutdown` loads `shutting_` with acquire (`:1614`); a caller that saw the flag that way is not ordered after
  the exchange in the single total order, so in the C++ model its sequentially consistent load of `occupied_` may miss a
  miss's count (multi-copy-atomic hardware forbids it). Second, the miss's three failure paths (`:925`, `:933`, `:946`) decrement
  with relaxed and then load the flag with acquire: a store-then-load pair that can reorder on arm64, so a miss failing on an
  allocation just as shutdown reads a stale count could leave shutdown waiting (the freeing paths avoid it through `free_mu_`,
  which `slots_made()` also takes). Make that load and those decrements `seq_cst`.
- **N6.** The hold-off is two periods on a real clock (R9). Take its time from the pass that chose the slot, or subtract a slack,
  if one period is what is meant; the comment says "the next period".
- **N7.** `reserve(r, 0, catalog, slot)` on a zero count counts a hot holder that `release()` never uncounts (it returns early on
  zero). Unreachable today (`counted_io.hh:44` reserves 1); an assert on `n != 0` keeps it so.
- The review's N2 (a stuck close counted as relief forever) and N3 (`start()` after `stop_sweep()`) stand as noted.

### Probes and artifacts (re-check)

All in `strata-wt/cold-tier-step2-item5/.scratch/review-item5/`, kept:
- `build7.sh` (rebuilds `b-tsan` and `b-asan` at the head), `run7.sh VARIANT` and `run7-both.sh` (the four tests, the stress at
  per_group 2 and 3, then `probes7` built by `probe.sh`): `runs7.txt`, `run7-{tsan,asan}-*.out`, `probes7-{tsan,asan}.out`.
- `probes7.cc`: `b1_hit_after_flag` (R1), `b1_handle_after_flag` (R2), `b1_demoting_handoff` (R3), `b1_open_handoff` (R4),
  `b1_wake_handoff` (R5), `b1_failed_retry` (R6), `b1_invariant_stress ROUNDS PG` (R7), `b2_due_window N V` (R8, fails today),
  `hold_off_two_periods` (R9), `hold_off_vs_wake` (R10), `close_throws_transient` (R11).
- `linux-tsan7.sh` (the tree, the four tests and the stress on the lease; `linux7/linux-tsan7.out`; its probe build failed on a
  parse error since fixed) and `linux-probes7.sh` (the probes against that build; `linux7/linux-probes7.out`).
- Rerun B5: `quiet light ct5-recheck 30 "item 5" -- ./probe.sh asan probes7.cc && bin-asan/probes7 b2_due_window 256 8`.
