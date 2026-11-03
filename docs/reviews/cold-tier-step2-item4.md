# strata review: cold tier step 2, item 4 (the wake, chore pins and the bound)

- Date: 2026-11-02.
- Reviewed: `phase/cold-tier-step2-item4` at strata:fd95fef (equal to `origin`), which merges item 3's fix
  round (strata:b7e0deb). The item's commits: strata:38858ab (the wake, backoff, bound close, timer),
  strata:be6236b (C14), strata:1acee84, strata:f5b0ccf (schedules, destructor's timer wait), strata:c0fbfec
  (mechanism note, limits). `include/strata/core/catalog.hh`, `basic_index::uncommitted()` (`index.hh:1114`),
  `test/catalog_wake_test.cc`, `test/catalog_test.cc`, `test/catalog_stress_test.cc`, `test/catalog_world.hh`,
  `docs/mechanisms/catalog.md`, `docs/design/limits.md:63-64`, the item 4 section of
  `docs/status/cold-tier-step2.md` (`:484-648`).
- Checked against: `docs/design/cold-tier.md` revision 7 (strata:5723f3d on `design/cold-tier`): D4 and its
  composed rows, D5 (the wake, the bound, warm to cold), I9, I10; S5 and S6 as the ledger copies them
  (`cold-tier-step2.md:508-538`); my item 3 review (`docs/reviews/cold-tier-step2-item3.md`): C14, I3-N1,
  C15; `docs/reviews/calibration.md`.
- Checked by: two builds of the exact commit with Homebrew LLVM 23.1.2 clang++ (RelWithDebInfo,
  `-fno-omit-frame-pointer`), `.scratch/review-item4/b-tsan` (`-fsanitize=thread`) and `b-asan`
  (`-fsanitize=address,undefined -fno-sanitize-recover=all`).
  - TSan: `catalog_wake_test` ok (1 s); `catalog_test 300` and `1000` ok (1,000 seeds: 663 wakes, 15 reposts,
    376 failures, 69 overloads, 205 gated, 72 bound closes); `catalog_stress_test` 3,000 x 4 and 20,000 x 6 ok,
    zero reports.
  - ASan+UBSan: `catalog_wake_test`, `catalog_test 300`, `catalog_stress_test 3000 4`: ok, zero reports.
  - `probes.cc`, seven cases, under both builds; an oracle header (`inc-oracle/`) run through T6's 1,000 seeds
    and the 20,000 x 6 stress under TSan, and its mutant (`inc-oracle-nogate/`); a candidate fix header
    (`inc-fix-b1/`) run against the probe and `catalog_wake_test`.
  - Not run: CI's Linux, FreeBSD and arm64 jobs (Linux TSan is the authority for races on struct copies,
    calibration 2026-10-29); the developer's 31 mutants (I reran none; my own mutant is the no-gate oracle);
    the preflight on the merged tree; benchmarks; fuzzing (no decoder).

## Verdict

**REJECT: one small blocker. A slot at the bound whose host commit chore ran on `owed` never closes, so
S6's "at the 8th failure a committed slot closes" fails for the common writer. Everything else holds.**

What holds, and reproduces:
- **C14 is met.** `wake_gate()` (`catalog.hh:717-722`) is the one decision; the `owed` row (`:541-543`) and
  `end_at()` (`:763-766`) call it, and nothing else posts a wake (`post_wake_chore`, `:789-792`, has those two
  callers; `run_wake`'s repost continues a wake already started). Probe `gate_all_entries`: inside a failed
  wake's backoff a request pin by key, `try_pin`, a re-pin by a stale handle, a refused plain close with a
  request waiter and a refused bound close with one each fail with `wake_failed`, take no pin, queue no chore
  and arm no timer; a chore pin enters. The oracle (a hard check in `post_wake_chore` that no wake is posted
  inside a backoff) never fires over T6's 1,000 seeds and the 20,000 x 6 stress; with the gate removed from
  `end_at()` it aborts both (`oracle-runs.txt`). I3-N1 is closed.
- **A wake racing a refused close** (probe `refused_close_race`): a request and a chore pin waiting on
  `closing`, close refused. Wake succeeds: both granted at `open`. Wake fails: the request fails with the
  cause (ENOMEM), the chore is handed its pin at `owed`. Inside the backoff: no wake posted, the request fails
  with `wake_failed`, the chore is handed its pin at `owed`. Pins return to zero in every mode.
- **A request pin on `owed` inside a backoff** fails at once with no side effect (above); `wake_failed`'s code
  is the cause's (EMFILE for the kernel's EMFILE, `overload_code`).
- **A lease busy forever** (probe `lease_forever`): 5,000 reposts over 320 s of simulated time, the delay
  capped at 64 ms, zero failures counted, the slot `waking`, the request still suspended, the bound close
  refused (the slot is not `owed`); on release the wake completes and the request gets its pin. This is D5
  and I10 as written (a hold that never ends is a host bug).
- **Teardown with a wake in flight** (probe `teardown_inflight`, WorkPool of 3 and a timer thread, 200 rounds
  destroying the catalog with no wait while the wake is anywhere between an armed timer and its final
  exchange): no hang, zero TSan and ASan reports. One standard-library caveat, I4-N2.
- **The schedules and the stress** reproduce the ledger's counts: `catalog_test 300` exactly
  (`cold-tier-step2.md:624`, wakes 186), and at 1,000 seeds reposts 15, failures 376, overloads 69, gated 205,
  bound closes 72 (13 commits, 8 failed) exactly; the ledger's "wakes 491" at 1,000 seeds (`:626`) is 663
  (`wakes_posted`) in the run, a transcription to correct.

What blocks: I4-B1.

## Rulings on the developer's choices (`cold-tier-step2.md:575-584`)

1. **S6's close at the bound left to item 5's sweep: accepted.** The sweep is item 5's (D5, step 5), and
   item 4 ships the primitive and tests it by calling it. The bound has no caller until then, so
   `limits.md:63` must not say Load-bearing yet (I4-N4), and item 5 owes S6 end to end through the sweep (C18).
2. **A commit at the bound followed by `demote()`: accepted, and widened.** Demoting in the close chore is
   right: the commit's maps go with a publication, no pin is handed out while the slot is `closing` (waiters
   only), and the slot closes in the same chore instead of costing another pass. It is too narrow as built:
   the demotion must run whenever the slot has maps, not only after this chore's own commit (I4-B1). I9's
   text ("only `demote()` makes a record without a body, under `{0, demoting}`") becomes false; revise it
   (I4-N3).
3. **`host::timer` optional: overruled. Make it required, one shape.** The lead is right, and the cost is
   worse than one worker. The catalog posts every transition of the partition to one group (`:283`), and
   `work_pool_executor` defaults to `per_group = 1` (`work_pool_executor.hh:32`), so a sleeping repost holds
   the partition's only transition lane. Probe `no_timer_stall`: with 4 wakes reposting on held leases, a miss
   on another key opened after 175 to 286 ms without a timer, 0.3 to 0.8 ms with one (TSan and ASan builds,
   one run each; the gap is the mechanism, not noise). A sleeping repost also drops the wake when its group
   cancels (`sleep_until` false, `:806`; `executor.hh:23`), stranding `waking` and its waiters. And no test
   runs that path: `catalog_world.hh:150-161` always sets a timer. Delete the `sleep_until` branch of `after()`
   and assert a timer at construction (I4-N1).
4. **The gate at the demotion edge is never taken: correct, and not a latent path.** A demotion starts only
   from `{0, open}`; every edge into `open` resets `streak` (a wake's success, `:868-871`; an open's success,
   `:691-696`); `demoting` blocks any wake until it ends. So `streak == 0` whenever `end_at(demoting, ...)` runs.
   Evidence: the oracle's hard check `from == demoting && streak != 0` never fires over 107 composed demotion
   edges in T6's 1,000 seeds and 32 in the stress. Keep the call (it is C14's one gate, and the list scan is
   already paid) and make the claim a debug assert at that edge, so item 5's `reclaim(0)` pass or any later
   edge into `demoting` that breaks it fails loudly.
5. **`wake_failed` derives from `std::system_error`: accepted.** Its `code()` is the cause's (EMFILE for
   `too_many_open` and for the kernel's EMFILE, ENOMEM for ENOMEM; `io_error` only for a non-system cause), so
   a host that sorts errors by code files a gated pin with the fault that gated it, which is the intent; the
   cause stays reachable through `cause()`. One gated pin's exception object is shared by every pin in the
   backoff (`s.error`); handlers must not mutate it, which the mechanism note should say in a line.

## Findings

**I4-B1. A slot at the bound never closes after a chore pin committed on `owed` (blocking; small).**
- S5's own case is a chore pin on `owed` that commits without a wake (`chore_pin_commits`): the host's
  periodic commit. A commit maps the records it writes, so the slot's maps go from 0 to 1. At the bound,
  `run_close` (`:917-930`) computes `ready = close_ready() && maps == 0`, false; it demotes only inside
  `if (!ready && bound && uncommitted())`, and nothing is uncommitted any more, so it ends at `owed`. Every
  later pass does the same, and a plain `begin_close` is refused for the same reason.
- Probe `bound_after_chore_commit` (both builds): write, demote, 8 EMFILE wakes, a chore-pin commit (maps 1,
  uncommitted 0), then three bound-close passes 20 s apart: each is taken and ends `owed`, maps 1,
  `bound_closes` 0; the plain close too. With the fault still on during the chore's commit the commit throws,
  stays uncommitted, and the bound close then commits and closes: the existing tests only reach that branch.
- Why it matters: the bound exists to release a slot whose wake keeps failing (I5-N2, CT-R33); the writer
  whose host committed during the backoff, the ordinary case for any index written before it went warm,
  keeps its descriptor, vnode and map forever.
- Fix: in the bound branch, commit if uncommitted, then demote whenever the commit did not fail, whatever
  committed earlier. The candidate in `.scratch/review-item4/inc-fix-b1/` (the `uncommitted()` test moved
  inside the branch, `demote()` after it) makes the probe close on the first pass with and without the fault,
  and `catalog_wake_test` still passes (`fix-b1-runs.txt`). Add the probe's case to `catalog_wake_test` with
  the mutant "demote only after this chore's commit" (today's code), and give T6's drive a chore commit on
  `owed` before the bound close.
- The plain close has the same hole (an idle `owed` slot whose host committed can never go cold, since the
  sweep demotes only `{0, open}`). That is item 5's and D5's to settle (C19); doing it in the shared close
  chore now, for both paths, is the one-mechanism shape and costs the same three lines.

**I4-N1. The timer becomes required (choice 3).** Remove `after()`'s sleeping branch (`:805-807`) and the
host comment's fallback (`:188-190`); assert `h_.timer` in the constructor; the mechanism note and the
ledger say a host supplies it. Item 5's sweep needs it anyway.

**I4-N2. The timer callback touches `this` after the decrement the destructor waits for.** `after()`'s
callback does `timers_.fetch_sub` and then `timers_.notify_all()` (`:801`). Once the decrement makes it zero,
`~catalog` (`:289-296`) can see zero, wait the group, see zero again and free the catalog while the callback
has not yet called `notify_all` on the destroyed atomic. libc++ and libstdc++ key their waiter tables by
address, so it does not crash in practice (`teardown_inflight`, 200 rounds, clean), but it is undefined
behavior. Fix: decrement and notify under a mutex the destructor takes after it sees zero (a
`std::condition_variable` instead of the atomic wait), so the callback has left before the free.

**I4-N3. Design and note text.** I9 (`cold-tier.md` r7) says only `demote()` makes a record without a body,
"under `{0, demoting}`"; the bound close runs it under `closing`. Revise I9 to "under `{0, demoting}`, or in
the close chore under `closing`; neither hands out a pin before its end." D5's bound paragraph should say the
close chore demotes when the slot has maps (I4-B1). The mechanism note's bound bullet follows.

**I4-N4. `limits.md:63` calls the bound Load-bearing with no caller.** Until item 5's sweep calls
`begin_bound_close()`, nothing enforces it. Mark it "Load-bearing from item 5 (the sweep calls it)" or land the
row with item 5.

**I4-N5. A throw in the wake's own plumbing strands `waking`.** `after()` counts the timer before
`h_.timer(...)` (`:798-799`); a host timer that throws leaves `timers_` above zero (the destructor hangs) and
the wake chore exits by the throw, which the executor drops (`executor.hh:24`), so the slot stays `waking`
with its waiters. `finish_wake` builds `wake_failed` (a rethrow and an allocation) under the shard mutex
(`:865`), before its exchange; a `bad_alloc` there strands it the same way. Build the `wake_failed` before
the lock; undo the count if `h_.timer` throws and fail the wake through `finish_wake` with the error. Low
likelihood, but the result is a hang, not an error.

**I4-N6. The stress barely reaches the repost.** 20,000 x 6 under TSan made 130 wakes and 1 repost
(`run-tsan-stress-20000-6.out`); 3,000 x 4 made 2. The repost's cross-thread path (the timer thread posting
into the pool while chores hold leases) is proved almost only on `inline_executor`. Lengthen the chore holds or
add held leases in the stress until a run makes a hundred or more reposts.

**I4-N7. Small.** `wake_gate()` counts one `wake_gated` and one `failed_fast` per `end_at()` however many
request waiters it fails. `failing()` is not cleared by a wake that succeeds or by a host commit, though the
comment says "have not closed or committed since" (`:419`); either clear it on a wake's success or reword.
A wake that reposts forever (`lease_forever`) is visible only as a growing `wake_reposts`; item 5's sweep could
report a slot `waking` past some age, as D5 has it report a stuck `closing`.

## Blocking items / Required changes

- **L1 (I4-B1).** The bound close demotes whenever the slot has maps after its commit step (or none was
  needed), so a slot committed by a chore pin closes on the next pass; the probe's case in
  `catalog_wake_test` with its mutant; a chore commit on `owed` in T6's drive.

## Conditions

- **C18 (item 5).** The sweep calls `begin_bound_close()` for `owed` slots at the bound once their probe time
  has passed; S7 carries S6 end to end through the sweep (no direct call), with the mutant "the sweep never
  calls it"; `limits.md:63` is load-bearing from then (I4-N4).
- **C19 (item 5, D5).** An `owed` or `open` slot with maps, idle and committed, can go cold: the close chore
  demotes when the slot has maps (the I4-B1 fix generalized to the plain close), and D5's warm-to-cold
  paragraph and I9 say so (I4-N3).
- **C20 (with L1).** I4-N1 (timer required), the debug assert at the demotion edge (choice 4), I4-N2, I4-N5.
  None needs another review; the lead confirms them with the probes below.
- Carried: C15 (item 5: `shutdown()`, `~catalog`'s assert and leak, pins at destruction), C16, C17 as in item 3.
  C14 is closed.

## Probes and artifacts

All in `strata-wt/cold-tier-step2-item4/.scratch/review-item4/`, kept:
- `cfg.sh VARIANT`, `runtests.sh VARIANT` (the three tests; `run-*.out`, `runs.txt`); builds in `b-tsan`, `b-asan`.
- `probe.sh VARIANT SRC [INCLUDE_DIR]`: builds a probe or a test TU with that build's flags, optionally ahead
  of a variant header, into `bin-VARIANT/`.
- `probes.cc`, cases `refused_close_race`, `gate_all_entries`, `overload_code`, `lease_forever`,
  `bound_after_chore_commit` (fails today, by design of the probe), `teardown_inflight`, `no_timer_stall`;
  outputs `probes-tsan-1.out`, `probes-asan-1.out`, `probes-{tsan,asan}-{teardown_inflight,no_timer_stall}.out`.
- `inc-oracle/`: the header with two hard checks (no wake posted inside a backoff; no demotion ends with a
  streak); `inc-oracle-nogate/`: the same with `end_at()`'s gate removed. Results `oracle-runs.txt`,
  `oracle-*.out` (clean; the mutant aborts T6 and the stress).
- `inc-fix-b1/`: the candidate L1 fix; `fix-b1-runs.txt`, `fix-b1-probe.out`, `fix-b1-wake.out`.
- Rerun: `quiet light ct4-review 30 "item 4 review" -- cmake --build .scratch/review-item4/b-asan --target
  catalog_wake_test -j4`, then `./probe.sh asan probes.cc && bin-asan/probes bound_after_chore_commit`.

Not run: CI's Linux, FreeBSD and arm64 jobs; the developer's 31 mutants; the preflight on the merged tree;
benchmarks; fuzzing.
