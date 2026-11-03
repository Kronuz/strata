# strata code review: cold tier step 2, item 6 (S3-FIFO and C10)

- Date: 2026-11-03. Reviewer: code reviewer (fresh context).
- Reviewed: `phase/cold-tier-step2-item6` at strata:9d091f9 (worktree `strata-wt/cold-tier-step2-item6`), against the ledger's item 6
  section, `cold-tier.md` D6 rev 7, the architect's ghost ruling (`docs/reviews/cold-tier-step2-item6-ghost.md`, not relitigated), and
  item 5 at strata:ccf34cd (`strata-wt/cold-tier-step2-item5`).
- Read: `ghost_set.hh`, the whole item 6 diff of `catalog.hh` (queues, `evict()`, `pick()`, `demote_hot()`, `finish_open/finish_close/run_close`,
  C10's `add_use`), `catalog_policy_test.cc`, the S8 harness header, the stress's invariant checks.
- Built and run: TSan (Homebrew clang 23, `-O1 -g -fsanitize=thread`, macOS x86_64) `.scratch/review-item6/tsan`: `catalog_policy_test`
  ok (31 s), `catalog_stress_test 1500 4` ok (plain, defer_io, each with policy; churn 1,643 misses, 1,627 closes, 238 ghost hits), zero
  TSan reports. `ghost_half` and `ghost_bytes` ran inside it (55,000 hashes: 2,097,152 B, 38.1 B each, matching the ledger).
- Probes (kept in `.scratch/review-item6/`): `probe_dangling.cc` (CR1), `probe.sh` (builds a probe against the TSan flags), `cfg.sh`
  (configure; deps from item 5b's fetched sources since item 6's `.scratch/deps` links are dead). Not rerun: the S8 cells (57 s each at
  full size; the ledger's lock-step test, which passed here, equals `model_total` request by request).
- Host: load stayed above 12 for the first 20 minutes, so the build queued; no other contention.

## Verdict

**APPROVE WITH CONDITIONS.** The policy matches D6 and the ruling; the hit path takes neither the policy mutex nor the ghost; the ghost
bound and memory claim hold. One concurrency bug (CR1) leaves a freed slot in a queue; it needs a fix and a regression test before this
lands, because item 5's merge makes it likelier. Everything else is non-blocking.

## Blocking

**CR1. A pass can requeue a slot that another closer is freeing, leaving a freed slot in a queue (confirmed).**
`policy_requeue` (`catalog.hh:719-722`) pushes when `q == 0` and the word's generation equals the one `pick` read. `finish_close` calls
`policy_unlink` (`:1023`) and only later bumps the generation in its compare-exchange (`:1034`). A slot the pass has picked is out of its
queue, so `policy_unlink` is then a no-op; if the pass's requeue lands between the unlink and the compare-exchange, the generation still
matches, the slot is pushed, and the compare-exchange frees it with the entry still queued. Interleaving:

1. A closer other than the pass's own `begin_close(unused)` (item 5's idle and bound closes, the stress's direct closers at
   `catalog_stress_test.cc:197`) takes slot X to `closing`; X stays queued.
2. The pass picks X (unlinked), reads its word (frequency set, or `closing` and so busy) and goes to requeue.
3. The close chore reaches `finish_close`: `policy_unlink` does nothing, X is not queued.
4. The pass requeues X (`q == 0`, generation unchanged).
5. The chore's compare-exchange frees X. X is free and in a queue.

`probe_dangling.cc` forces it with two `before_cas` hooks (`policy.clear` stops the pass after the pick, `close.end` stops the chore after
the unlink) and two threads: `k0 slot state=0 (free) queue_of=2; queued small=9 main=1, resident slots=9` -> "BUG: a freed slot is still
in a queue", rc 1, both runs. `closed_behind_the_pass` (`catalog_policy_test.cc:301`) tests the neighbouring case (the other closer finishes
completely before the pass's `begin_close`, so the generation moved) and does not cover this window; the stress and its thread mutants do
not reach a window this narrow, and none of the 36 policy mutants removes the unlink-to-compare-exchange gap.

Consequences: `queue_sizes()` and `queued_over_soft_open()` count a slot that is not open, so `over()` is true once per leaked entry and the
pass closes a live slot it should not; the pass keeps visiting the free entry (busy, requeue) and spends visits; when the slot is reused,
`policy_enqueue` sees `q != 0` and leaves it at its stale queue position (an early-close candidate). It self-heals only on reuse. In this
branch alone the trigger needs a direct closer; after item 5 merges, the idle close begins on the pass thread and leaves the slot queued,
an idle (old, frequency clear) slot is near a queue's head, and the same pass's `evict` picks it next while the chore runs on another
thread: the realistic trigger. S8.7's "a close leaves no queue entry behind" and S8.8's invariant (every non-cold slot in exactly one queue,
no other slot in one) are not met in this window.

Fix (the developer's design choice, any one of these closes it): requeue only if the word's state is not `closing` or `free` (read under
`policy_mu_`; `run_close`'s refused-close site then requeues after `end_at` returns the slot to `prior`), or make the unlink and the
generation change one step with respect to `policy_mu_` (take `policy_mu_` across the compare-exchange; shard lock then policy mutex,
nothing takes them in the other order), or have `pick` leave closing slots queued. Add `probe_dangling.cc` as a test (it needs two threads
and two hooks; `catalog_policy_test` is single-threaded, so it goes beside the stress) and a mutant that removes the check.

## Findings that hold (no action)

1. **Concurrency on the word.** `begin_close(idx, unused)` tests pins zero and frequency zero in the same compare-exchange that sets
   `closing` (`:339-345`): a pin cannot slip between the choice and the close (it either adds its pin and frequency first, so the compare
   fails and the pass requeues, or lands after and waits on `closing`). `clear_freq` is a `fetch_and` on the frequency bits and `decay` a
   compare-exchange loop; a racing `add_pin`/`add_use` is a compare-exchange on the whole word, so it retries; no lost pin. `begin_demote`
   keeps its pins-zero compare. TSan clean; the ledger's seven thread mutants (T-enq, T-unl, T-req, T-pick, T-ghost, T-pass, T-clr) are
   not rerun.
2. **A hit takes neither the policy mutex nor the ghost.** Read of every caller of `policy_*`/`ghost_` (the grep is
   `catalog.hh:698-731, 762-786, 847, 1003-1004, 1023-1044`): `policy_enqueue` only in `finish_open` (a miss, or a `failed` retry),
   `policy_unlink` and `policy_ghost` only in `finish_close`, `policy_requeue`/`pick` only in `evict` and the refused-close path, queue
   sizes only for tests. The `owed`-to-`waking` and `failed`-to-`opening` rows of `try_pin` only add a use to the word (`add_use`).
   Lock order: shard mutex and policy mutex are never nested (`policy_unlink` before the shard lock, `policy_ghost` after it, the hash
   computed inside). The cost claim (+1 ns by key, inside the spread) is plausible for what the hit path now executes; I did not rerun
   `pinbench` (a measurement, off-limits to a reviewer).
3. **Ghost bounds.** `ghost_set::add` drops the oldest at `cap_` (`size_ == cap_` after `take(h)` removes a duplicate first, so a re-add
   does not evict); `catalog` builds it at `soft_open / 2` (`:272`), 0 for a catalog without a cap (add returns at once). The oracle test
   against a list and `ghost_half` (0, 1, 2, 3, 10, 11, 110,000) pass here. Memory: nodes 65,536 x 24 B + table 65,536 x 8 B = 2,097,152 B
   for a ghost of 55,000 (38.1 B each, 19 B per open index of 110,000): the claim is the measured bytes, and the table's load is under
   7/8. Between doublings it is up to 66 B an entry, as the header says. A transient doubling copy raises the peak to about 1.5x that; not
   a bound anyone relies on. A minor: `policy_ghost` counts `ghost_adds_` even when the capacity is 0 (soft_open under 2).
4. **Queue bounds under a burst.** The queues hold only open or warm slots (a slot enters at the end of its open, leaves at the end of
   its close, a picked slot is out only inside a pass), so they are bounded by the slot count (at most 4,194,304) and never by the policy;
   `soft_open` is a soft cap by design and a burst of misses exceeds it until passes (256 visits, 16 transitions each) catch up. A pass
   terminates: `visits`/`transitions` bound it, and with the limits lifted `skipped > round` ends a round with nothing to take. The
   stress's `burst_closes` (queues at most 16 and 4) and `churn_hammer` check this at rest.
5. **The S8 drive is the real catalog**, over stub indexes (`stub_index`, `defer_io`-shaped, a map while hot), a real executor-driven
   transition mix (uses meet opening/waking/demoting; the ledger's counts 501,756 to 3,014,323 opening waiters), not a model; the model is
   only the reference (`s3fifo_model.hh`, checked request by request against the catalog in `lockstep`). The 0.2-point margin is not
   measured against run-to-run variation because there is none: the trace is seeded (`mt19937_64` 42) and the executor is a manual
   single-threaded one, so A/A is exactly 0 and the catalog's gap to `model_total` is 0.0001. The margin then only sets which mutants the
   cells kill (ghost ignored -0.27 at theta 0.99 over all 8M, -0.98 on the loop cell; the 0.2 margin kills both but leaves the 0.27 with
   0.07 to spare). Anything between 0.0001 and 0.2 is left to `lockstep` (exact on 60,000 requests, three thetas). Non-blocking: a
   0.05-point margin would still be 500x the observed gap and would strengthen the cells.
6. **C10.** `add_use` on the `owed`-to-`waking` and `failed`-to-`opening` rows of a `try_pin`, and a `would_wait` on a transition changes
   nothing (`:564`, `:591`); `use_rows` covers every row of D4's table with its mutants (ledger S8.6). Saturation at 3 is tested.

## The seam to item 5 (ccf34cd)

`git merge-tree` of the two heads conflicts in `catalog.hh`, the stress, `catalog_world.hh`, CMake and three docs, as the ledger expects;
the code conflicts are the four listed seam fixes and nothing else that I found. Additions to the merge's list, none blocking this branch:

1. **CR1 reaches the merge.** Item 5's idle and bound closes (`begin_close(c.idx)` at its `:450`, the bound close at `:537`) leave the
   slot queued, so `evict` will pick closing slots it did not close; fix CR1 here first.
2. **Item 5's `run_close` has more exits than item 6's one refused-close site**: modes `sweep`, `bound`, `shutdown`, reposts at
   `:1165` and `:1228`, and the `try_finish_close` retry (`:1239-1276`). Item 6 requeues and clears `to_ghost` at the single `!ready`
   exit; each place in item 5 where a close ends at the prior state must do the same, and `to_ghost` must be cleared on every exit that does
   not reach `finish_close`.
3. **`begin_close(idx, unused)`**: item 5's signature at `:506` has no `unused`; the merge adds it for `evict` only, and item 5's idle and
   bound closes call it without (they close regardless of frequency, as the ledger says).
4. **`over()` from `live_ - closing_`** (seam fix 2): `evict`'s loop re-reads `over()` after each `begin_close`; `note(open -> closing)`
   raises `closing_` at once, so the count falls as item 6's queue count does. Keep that property (the pass must not book relief from a
   close that was refused).
5. **`to_ghost` is set before `begin_close` and cleared on refusal**: if item 5's idle close wins the slot between the two, the chore
   consumes the flag and ghosts a key the policy did not choose (harmless: one extra ghost entry). Noted, no change.

## Non-blocking notes (follow-ups, no second review)

- `policy_ghost` counts `ghost_adds_` at capacity 0; count only when added.
- `demote_hot` scans 256 slots from a cursor; the merge deletes it (seam fix 3), so its tests go with it.
- The ledger's cost row says the hit path is "unchanged but for the frequency's add"; true by the read in finding 2.

## Calibration

Added for `docs/reviews/calibration.md` (lead's to apply; I write only this file): a requeue that checks "not in a queue and generation
unchanged" misses a close in flight, because the generation moves at the close's end, not its start. For every guard on a deferred
put-back, construct the closer that finishes between the guard's read and its write and name the field that changes at the start of the
close.

## Verdict, conditions, not run

- **APPROVE WITH CONDITIONS.**
- Blocking: CR1 (a freed slot left in a queue; probe `.scratch/review-item6/probe_dangling.cc`).
- Conditions: fix CR1 with a two-thread regression test and a killed mutant; confirm with `probe_dangling` (exit 0, "ok"); a lead check
  that item 5's merge applies findings 1 to 4 of the seam list; Linux CI TSan is the authority for races on struct-copy writes
  (calibration 2026-10-29): this branch's CI must be green on it.
- Not run: ASan+UBSan build, the S8 cells at full size, the policy mutants and thread mutants, `pinbench` and any timing, Linux, FreeBSD,
  macOS arm64, item 5's merged tree, T4.
