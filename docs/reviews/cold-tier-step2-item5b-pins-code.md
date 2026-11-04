# strata review: item 5b pin awaiter fix (Xapiand X-POOL step 2 review CR1, CR2)

- Date: 2026-11-04. Code review of the fix round; the one review for it (concurrency and lifetimes).
- Reviewed: `phase/cold-tier-step2-item5b` at strata:c8160a9 (equal to `origin`), code commits strata:f052782 (CR1),
  strata:177a687 (CR2), strata:edb880b (orphaned waiters); `include/strata/core/catalog.hh`, `test/catalog_teardown_test.cc`,
  the sweep test's awaiter order, `docs/mechanisms/catalog.md` ("Waiters whose frames die", "Cancelled pins"), and the
  ledger's item 5b fix section (`docs/status/cold-tier-step2.md`, last section).
- Checked against: Xapiand `docs/reviews/phase7c-x-pool-step2-code.md` (CR1, CR2, probes in that worktree's
  `.scratch/xp2rev/`); task's `docs/design/cancellation.md` D4 (`lock(token)`), D2 (`cancel_callback`) and D8 (phases 1 to
  3, CN-R3, CR6's precondition), and task's `lock_waiter`; `docs/reviews/calibration.md`.
- Checked by: Mac x86_64, Homebrew LLVM, CI's sanitizer flags, ASan+UBSan and TSan built separately (my probe compiled
  against the developer's `.scratch/pinfix/{asan,tsan}` dependencies, read only). Xapiand's X-POOL probe rebuilt against
  this branch (x-pool's builds already point `FETCHCONTENT_SOURCE_DIR_STRATA` here; `src/database/pool.cc` recompiled).
- Not run: Linux (CI's linux-tsan and linux-asan-ubsan are green on strata:cee8079, the code head, and the ledger records a
  Linux floor on the local VM; calibration's rule makes Linux CI TSan the authority, and it is green); the developer's
  mutants (read from the ledger, not rerun); the full ctest; benchmarks (no hot-path change beyond one allocation per wait);
  arm64; FreeBSD; a merge build with item 5 (see PF4).

Finding IDs are PF1 onward, so they do not collide with the X-POOL review's CR1 and CR2, which this round fixes.

## Verdict

**APPROVE WITH CONDITIONS.** CR1 and CR2 are fixed. The original `destroyed_awaiter` probe passes against the fix under
both sanitizers, and it failed under both before. I attacked the waiter's state machine, its reference counts, the cancel
paths and shutdown, and found no lost or double release, no ABA, and no ordering hole. The one defect is in the new
`~catalog` orphaning (strata:edb880b): a token requested after the catalog is gone runs the pin's callback on the freed
catalog (PF1). That path is host-fault-only and cheap to fix, so it is a condition, not a rejection. The developer's
ruling on the self-wait stands.

What I checked and found sound (by reading, then by probes):

1. **One exchange per side decides `st`.** The delivery's `exchange(delivered)` and the destructor's `exchange(abandoned)`
   (`catalog.hh:1062`, `:1116`) each see the other's value when they come second. A delivered pin is released by exactly one
   side (`deliver` on `abandoned`, `abandon` on `delivered` with no error). `take()` clears `linked` under the list's mutex
   (`:139-143`), and `unlink_waiter` tests it under the same mutex (`:1083-1099`). So a cancel or destructor either unlinks
   and uncounts before any transition takes the waiter, or finds it taken and leaves the release to the exchange. There is
   no window where both or neither act.
2. **The pin count stays consistent with the lists.** Every transition reads its list and the word under the shard mutex
   and changes both there: `finish_open`'s `with_pins(…, 0)`, `split_requests`' `pins - failed`, `replace_from`'s
   `pins - waiters` drain count, and `finish_close`'s `pins != 0` reopen. `unlink_waiter` removes a node and one pin in the
   same critical section. A waiter's pin is never a drain holder's, so the plain `fetch_sub` without `drain_unpin` or `kick`
   is correct, and each state a waiter sits in has a transition in flight that ends it.
3. **The references and their ordering are correct.** `retain` is relaxed (always by a holder, under the mutex); `release`
   is `acq_rel` and deletes on the last reference. Every raw waiter pointer is covered by a reference: the list's while
   linked or taken, until `deliver` drops it last; the awaiter's until `await_resume` or the destructor. The cancel
   callback's raw `w` is covered by the awaiter's reference, because both `await_resume` and `~pin_awaiter` destroy the
   registration first, which waits for a callback running elsewhere (task D2). `deliver` reads `next` before it releases
   the current node, and a taken node's `next` changes only under the mutex (`split_requests`) before delivery. Waiters are
   never reused, so ABA has no handle: a stale pointer would have to outlive a reference, and none does.
4. **CR2 follows `lock(token)`.** A free pin is taken whatever the token. A requested token never joins (`acquire_impl(…,
   nullptr)`). A request between the join and the registration runs inline inside `cb_.emplace`, so the suspending
   compare-exchange declines. A cancel that loses to a take returns the delivery. A delivery from a cancel always posts when
   the frame has a scheduler (`:1067`), so the cancelled frame never resumes inside `request()`, as task's D6 sync_cancel
   rule requires.

## Rulings on the developer's question

**A task's second pin of a key does not reuse its first: upheld.** Reuse needs an owner identity that pins don't have:
they move across tasks and threads (I7), and tracking one would put a per-task map on the hot path. It would also defeat
the fence: the self-wait happens exactly when the first pin's index is fenced and a replace is draining, and handing that
index to the second pin is what the replace exists to prevent. The hold-and-wait is the caller's, as with any lock.
Xapiand should keep one pin per key per request (`search_batch` with a repeated endpoint pins once and shares it). The
token bounds the wait only if the request carries a deadline or is cancelled, so that dedupe is Xapiand's follow-up, not
optional. Nothing about user-visible behaviour changes. The architect does not need to see this unless they want
self-wait detection, which I don't recommend: it costs on the hot path for a caller bug.

## Findings

**PF1. A token requested after `~catalog` runs the pin's cancel callback on the freed catalog (condition).**
`orphan_waiters()` (`catalog.hh:2154-2172`) detaches the listed waiters and sets `w->mu = nullptr`, but each waiting
awaiter's `cancel_callback` stays registered on its token, holding `cancel_pin{c, w}` with the dead `c`
(`:215-219`). A later request runs `c->cancel_wait(w)` → `hook_noexcept` reads `h_` of the freed catalog. If that read
survives, `unlink_waiter` locks `*w->mu`, which is now null. Probe `orphan_cancel`: a view kept past its pin holds the slot
in `closing` (S7.5) with a request waiting, the host destroys the catalog, then requests the frame's token. ASan reports
`heap-use-after-free` in `cancel_callback<…cancel_pin>::invoke` from `cancel_state::request`, with the catalog freed by
`unique_ptr::reset` (`.scratch/pinrev/out-asan-orphan_cancel.log`). The documented case, the frame destroyed after
`~catalog` with no request (`orphan_destroy`), is clean, so the ledger's claim that "their frames, destroyed later, touch
only their waiters" holds for destruction but not for a token request.

This is reachable only after a host gives up an unfinished shutdown while a request still waits (Xapiand's 30 s shutdown
patience over a stuck close), and only if something requests that request's token before its root dies. Task's phase 1
stops the loops whose deadlines would fire, so it is fault-only. A related point: `~pin_awaiter` calls `c_->abandon()` on
the dead catalog in the same case (`:414`; `~shutdown_awaiter` at `:466`). That is safe today only because the `!w->mu` branch touches no member.

Fix (small): make the orphan mark the waiter's and test it before touching the catalog. For example, `cancel_pin` and the
awaiters' destructors check `w->mu` (or an `orphaned` flag) and return or only release, and `abandon`'s first branch
moves out of the member call. Add `orphan_cancel` as a regression. If the owner prefers to narrow the contract ("no token
of a waiting pin may be requested after `~catalog`"), it must be stated in catalog.md beside the orphan rule, and the probe
then documents the hazard.

**PF2. A frame destroyed while its scheduler still runs can receive a late resume (doc, non-blocking).** `deliver`
exchanges `delivered` and then posts (`:1062-1067`). A destructor that runs between the two finds `delivered` and releases
the pin correctly, but the resume is posted afterwards for a destroyed frame. Probe `shutdown_midflight` destroys frames
with the test scheduler open while the WorkPool delivers: 2 late posts in 200 seeds (`out-asan-shutdown_midflight.log`;
`test_sched::late()`, no sanitizer report because the test scheduler discards them). The same probe with task's phase 2
first (`shutdown_phases`, scheduler closed so posts drop) is clean under both sanitizers. This is task's CR6 precondition,
and D8 guarantees it. But catalog.md says the teardown holds "while the catalog's chores may still run on a pool", which
reads as stronger than it is. State the precondition: a frame dies only after its scheduler is closed, or no delivery can
run. With no scheduler (`sched` null), the delivery resumes inline, so it must not race destruction at all.

**PF3. `stats().cancels` excludes pins whose token was requested before they waited (doc, non-blocking).** Those throw
`task::cancelled` from `await_ready` without counting (`:423`). catalog.md says the counter is "the pins a token ended".
In `inline_threads` under TSan, 891 frames caught `cancelled` and `cancels` was 743 (equal to `unlinked`). Say "waits a
token ended", or count both.

**PF4. The item 5 seam (condition on the merge, not on this review).** item 5 is now at strata:6d785a7, 21 commits past
this branch's base strata:31da26e. A trial `git merge-tree` against it conflicts in `catalog.hh` (10 hunks), catalog.md,
the ledger and `limits.md` (`.scratch/pinrev/merge-item5-catalog.hh`). The `catalog.hh` conflicts are this item's earlier
round meeting item 5's S3-FIFO work, for example the free list in `last_use` against item 5's `q_next`. None are in the
waiter code. item 5 adds no new waiter-list user: the set of `take(s)`, `deliver(`, `wait_head` and `split_requests(`
uses is unchanged between the base and item 5. One new item 5 path, `close_refused` → `end_at(closing, prior)`, already
goes through `take`/`deliver`. Whoever merges must keep the doubly linked `wait_list` at every list site (none may revert
to `wait_head`/`append`). They must then rerun `catalog_teardown_test 200 300`, `catalog_sweep_test` and `catalog_test`
under ASan and TSan on the merged tree, with the earlier review's slot-size condition.

**PF5. A requested token still starts the open or wake (design note, non-blocking).** `lock(token)` has no side effect
for a requested token. The catalog's runs `try_pin`'s path, so a burst of cancelled requests on cold keys (clients that
disconnect) still opens their indexes. It is documented and arguably useful as a warm-up. I record it for the design's
D4 text alongside the heap waiter, which the ledger already flags as owed there.

## Blocking items / Required changes

None block the code as a fix of CR1 and CR2.

## Conditions

1. PF1: fix the post-`~catalog` cancel (or state the narrowed contract in catalog.md) with `orphan_cancel` as its test,
   before Xapiand passes request tokens to `pin()`, since that is what makes the path reachable. The lead confirms it; no
   re-review is needed.
2. PF4: on the merge with item 5, keep `wait_list` at every list site and rerun the teardown, sweep and catalog tests
   under both sanitizers on the merged tree.
3. PF2 and PF3: the two catalog.md sentences, in the same commit as PF1.

## Probes and artifacts

All in `.scratch/pinrev/` on this branch, except the X-POOL probe, which stays in Xapiand's x-pool worktree.

- `pin_probe.cc` (built by `mk.sh asan|tsan|asan-nd`; includes `test/catalog_teardown_test.cc` for its harness):
  - `orphan_cancel`: PF1. ASan heap-use-after-free (`out-asan-orphan_cancel.log`).
  - `orphan_destroy`: the documented orphan case. Clean (`out-asan-orphan_destroy.log`).
  - `inline_threads 300`: no scheduler, so deliveries resume inline on pool threads, cancels resume inline on a canceller
    thread, and a callback's frame destroys its own registration. Clean under ASan (1,645 pinned, 326 cancelled) and TSan
    (1,080 / 891; `cancels` 743 = `unlinked`). No pin left, every frame ended (`out-{asan,tsan}-inline_threads.log`).
  - `shutdown_phases 200` / `150`: waiting and holding frames on cold opens, shutdown started at once, a third of the
    tokens requested from a second thread, the shutdown frame destroyed in a third of the seeds, scheduler closed before
    the frames die, then a second shutdown awaiter. Shutdown ends, no late post, clean under ASan and TSan.
  - `shutdown_midflight 200`: the same without phase 2. PF2's 2 late posts; no sanitizer report.
  - `out-asan-nd-orphan_*.log` and `lldb-orphan_destroy.txt`: a first setup of the orphan probe whose waiter was
    already delivered at `~catalog`, which is the documented "pin held" host bug and not PF1. Kept for the record.
- X-POOL's `destroyed_awaiter` (Xapiand `~/code/Kronuz/Xapiand-wt/x-pool/.scratch/xp2rev/probe-{asan,tsan}`, rebuilt by
  its `mk.sh` after `ninja bin/xapiand_test_pool` in x-pool's `build-{asan,tsan}`; run directories `run-{asan,tsan}-pinrev`):
  160 iterations, `join=1`, no report under ASan or TSan (`out-xap-{asan,tsan}-destroyed_awaiter.log`). Before the fix it
  reported use-after-poison in `deliver` (ASan) and a race in `finish_open`/`deliver` with `join` false (TSan). The TSan
  run's rc 1 comes from the `quiet` wrapper's own error after the probe printed `ok` (`quiet` lines 432-433), not from
  the probe.
- `merge-item5-catalog.hh`, `item5-catalog.hh`, `base-catalog.hh`: PF4's trial merge and the two sides.

## Re-check: circular wait_list

- Date: 2026-11-04. Scoped re-check of C26 (strata:4319bb9): `wait_list` as one circular, doubly linked pointer, so the
  slot fits D4's 192 B after the merge with item 5; the `middle_waiter` order check (strata:32c9069); PF1 (strata:7b5a527)
  read only. Branch head strata:88c0565, equal to `origin`.
- Read: `include/strata/core/catalog.hh` (`wait_list` `:135-176`, `join` `:1128`, `deliver` `:1142`, `unlink_waiter`
  `:1167`, `cancel_wait`, `abandon`, `join_shutdown`/`maybe_complete_shutdown` `:2329-2361`, `orphan_waiters` `:2401`),
  `test/catalog_teardown_test.cc`, `slot_cost` in `test/catalog_test.cc`, the ledger's PF4 section, `git log 0cd69c9..88c0565`.

**Verdict: APPROVE TO LAND**, on the condition below. The list is correct at every site and no finding blocks.

### The list

- `push` appends at `head->prev` (FIFO); `unlink` handles the single member (`next == w`), the head (moves `head`), the
  tail and the middle, and clears the waiter's links; `take` breaks the circle once and clears `linked` on every member;
  `each` stops when `next` comes back to `head`. Every caller holds the list's mutex (`waiters_of`, `begin_transition`'s
  request scan, `replace_from`, `join`, `unlink_waiter`, `join_shutdown`, `maybe_complete_shutdown`, `orphan_waiters`).
- A delivery walks a taken, null-terminated list outside the mutex. Removing a member then is impossible by construction:
  `unlink_waiter` refuses a waiter whose `linked` is false, so a cancel or a destructor during the walk loses to the
  delivery (`cancel_wait` returns, `abandon` takes the `abandoned` exchange), and `deliver` reads `next` before it posts
  and drops the list's reference. A waiter joins once per await (`acquire_impl` `:1290-1307`), so no new `push` rewrites
  a taken waiter's links. `orphan_waiters` takes each list under its mutex and reads `next` before `release`.
- Size: `slot_cost`'s `static_assert(slot_bytes() <= 192)` is load-bearing. Mac libc++ prints 184 B (my ASan and TSan
  builds, `out-{asan,tsan}-catalog.log`); the developer's Linux VM prints 192 B on libstdc++ (`.scratch/pinpf/lx-*.out`);
  and CI at strata:88c0565 compiled `catalog_test.cc` with the assert on `linux-x86_64-gcc`, `linux-arm64-gcc` and
  `linux-x86_64-clang` and passed (run 41890673116; `ci/b5-88c0565-*.log`). No headroom on libstdc++, as the ledger says.

### Probes (Mac x86_64, Homebrew LLVM, CI's flags; ASan+UBSan and TSan built separately; all rc 0, no report)

- `wl_probe model 200000`: random `push`, `unlink` and `take` against a `std::deque`, checking after every step that the
  links are circular, `head->prev` is the tail, every member is linked, `each` visits in order, `take` returns the FIFO
  null-terminated with `linked` cleared, and an unlinked waiter keeps no links. 16,279 takes; unlinks of the head 14,120,
  the tail 13,969, the middle 17,143, a single member 20,015.
- `wl_probe conc 30`: four threads push and unlink their own waiters under one mutex while a fifth takes; every take is in
  push order.
- `wl_probe masks`: 1 to 5 waiters on an opening slot, each of the 62 subsets destroyed before the open ends (the empty
  list, a single, the head, the tail, the middle, every mix): the survivors get their pins in join order and the pins and
  `waiters_of` come back to zero, no late post.
- `wl_probe orphan3`: three waiters on a slot a kept view holds `closing`; `~catalog` orphans the circular list; the first
  and third tokens are requested (nothing resumes, nothing posts), the frames are destroyed in reverse.
- The pin review's probes against the merged tree (`pin_probe_merged.cc`, the original with `viewer` renamed, since the
  teardown test now defines one): `orphan_cancel`, `orphan_destroy`, `inline_threads 300` (concurrent cancels and inline
  deliveries: removals racing the walk), `shutdown_phases 150`: clean under both. `shutdown_midflight 150`: PF2's known
  late posts (seeds 135 and 10) and one probe artifact under ASan (seed 62, "ended 2 of 1": the shutdown frame finished
  before the probe dropped it, which the probe's `want` does not allow); no sanitizer report.
- Tests on the merged tree: `catalog_teardown_test 200 300` and `catalog_test 300`, rc 0, no report, both builds.

### Mutants (ASan; `mutants.py`, `mut.sh NAME`, `mutall.out`)

The suite (`catalog_teardown_test 40 60`) kills all four reviewer mutants, so the developer's W1 to W3 are not the only
guard; BASE survives everything.

| Mutant | Suite | `masks` | `model` |
|---|---|---|---|
| R1: the single-member test keys on `head == w` (unlinking the head of a longer list empties it) | killed, heap-use-after-free | killed | killed |
| R2: `unlink` leaves the successor's `prev` (a removed tail stays `head->prev`) | killed, heap-use-after-free | killed | killed |
| R3: `each` visits the head only | killed, `middle_waiter` count | killed | killed |
| R4: `take` leaves the taken waiters `linked` | killed, UBSan null member access in `unlink` | survived | killed |

### PF1

Reads right: `cancel_pin` and the static `abandon` test `w->mu` before touching the catalog, `orphan_waiters` clears it
under the list's mutex, and `orphaned_waiter(false|true)` is the original probe as a test; the ledger's P1 to P3 revert
mutants die. The unlocked read of `mu` races only a cancel concurrent with `~catalog`, which PF2's precondition excludes.

### The TSan `catalog_s8_smoke` time

Main's: run 41879552963 on strata:676285d, `linux-tsan` job, `catalog_s8_smoke` passed in 3,012.37 s and `format14` in
1,293.18 s (`ci/main-676285d-linux-tsan.log`). The branch before the merge (strata:cee8079, run 41841864067) has no
`catalog_s8_smoke` and passes `format14` in 753.82 s. So a 1,700 s Mac timeout is consistent with main's own cost, and
the suite runs over a stub index whose interface additions here are empty methods (`git diff 676285d 88c0565 --
test/catalog_s8_test.cc`). What is not yet measured is the branch's own number on the same runner: this branch's
`linux-tsan` job (run 41890673116) was still running when I wrote this.

### Findings

- **PF6 (non-blocking, the lead).** The slot is exactly 192 B on libstdc++; the next field costs either a mechanism change
  or a revised D4 budget. The static_assert makes that visible at compile time, which is what it should do.
- **PF7 (non-blocking, the lead).** `catalog_s8_smoke` with `S8_SCALE=20` under TSan is the longest job in the suite at
  50 min on main; worth a TSan-specific scale.

### Conditions

1. Before landing, run 41890673116 is green on every job, and its `linux-tsan` `catalog_s8_smoke` time is close to main's
   3,012 s (calibration: Linux CI TSan is the authority). A large excess is this branch's and comes back for a look.

Not run: the Linux VM (CI's Linux jobs and the developer's VM logs cover libstdc++); the full ctest; the developer's
W1 to W3 and P1 to P3 mutants (read from the ledger); TSan mutants; `catalog_s8_smoke` locally; benchmarks; FreeBSD and
arm64 beyond CI. Probes and logs in `.scratch/wlrev/` (`wl_probe.cc`, `pin_probe_merged.cc`, `build*.sh`, `run.sh`,
`mutants.py`, `mut.sh`, `mutall.sh`, `out-*.log`, `mut/`, `ci/`).
