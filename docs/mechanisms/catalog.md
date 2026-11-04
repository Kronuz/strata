> Status: CURRENT (2026-11-02) as built on `phase/cold-tier-step2-item3`; the design is
> `docs/design/cold-tier.md` (D4, D5, I7, I9, I10; CT-R13, CT-R15, CT-R23, CT-R28, CT-R29 to CT-R31, CT-R33,
> CT-R34, CT-R35, CT-R40, CT-R41) on `design/cold-tier`. The wake (item 4), the sweep and shutdown (item 5) are built;
> the policy that picks what a pass closes (S3-FIFO, item 6) is not: a pass takes the oldest use first.

# The catalog: slots, pins and the word

`strata::catalog<Index, H>` (`include/strata/core/catalog.hh`) owns the non-cold indexes of one
partition. Every call on one of them runs under a **pin**; a pin never blocks a thread.

```
 key ─▶ shard (64) ─▶ hash_slots ─▶ slot ─ word: pins:24 | state:4 | freq:2 | gen:34
                      under one mutex       io, index*, reader, H, waiters, counts, backoff
 states: free opening open owed waking demoting closing failed
```

## How it works

- **A slot is 160 B** (plus the host's payload `H`; `catalog_test` `slot_cost` prints it and `static_assert`s D4's
  192 B). The index is behind a pointer, allocated by the open and freed by the close, so a free or cold slot
  holds none of its 1,496 B.

- **The word** is the only place a pin, a state or a frequency is decided. Every write is a
  compare-exchange or a fetch-op, so none loses a pin. A slot's other fields are written by a
  transition before its final compare-exchange and read after a pin's.
- **A pin by key** takes its shard's mutex for one probe and one compare-exchange. On `open` it adds
  a pin (and, for a use, a frequency step, saturating at 3). On any other state it adds its pin
  *and* joins the slot's waiter list in the same critical section, then suspends. A **chore pin**
  enters `open` and `owed`, and `waking` when its thread holds the index's lease; otherwise it waits
  like a request pin. A request pin on `owed` moves the slot to `waking` and posts the wake.
- **A pin by handle** (`{slot, gen}`) is one compare-exchange while the slot is `open` and `gen`
  matches; anything else falls back to the key. Every close, a reopen in place included, bumps `gen`.
- **A transition's end** (open, wake, demotion, close) makes its final compare-exchange *under the
  shard mutex*, takes the waiter list in the same section, and after unlocking delivers each waiter:
  its pin was already counted, so delivery only wakes it (posted to the scheduler it suspended on;
  inline only for a waiter with none). A failed open stores `{0, failed}` and fails its waiters; a
  failed wake stores `{p - w, owed}` for its `w` request waiters and hands the chore waiters theirs.
- **Two composed edges** are stored in one compare-exchange where D4's table has two rows (`end_at`). A
  demotion that ends `owed` while a request pin waits (it registered during `demoting`) goes to `waking`, not
  `owed`: `{p, owed}` would hand that waiter a pin on a slot that still owes its bodies (I9). A close that is
  refused back to `owed` with a request waiter does the same (`closing` to `waking`). The request waiters keep
  their counted pins and the wake delivers them; chore waiters wait for the wake's end too (CT-R40). With no
  request waiter the end is `owed`, or `open` for a refused close of an `open` slot, and the waiters get their
  pins at once.
- **The count stops at the field.** A pin on a slot at 16,777,215 pins (`word::full`) throws `too_many_open`
  in `acquire_impl` and in the by-handle fast path, before any compare-exchange, so the 24 bits never carry.
- **A scheduler outlives the pins that await on it**, and every chore that may resume one: a
  delivery is still inside `post()` when the resumed pin finishes. `wait_chores()` (and `~catalog`) end the
  last delivery.
- **`try_pin`** starts the open or the wake as a pin would, holds nothing, and answers `would_wait`.
- **Opens** are single-flighted by the slot's `opening` state. A failed open leaves a negative entry
  (`failed`, the error, a retry time doubling from 10 ms to 10 s); pins fail at once until then and
  the first pin after it reopens. A second key on a held directory meets the directory lock's
  `EWOULDBLOCK` once, as a host error.

## The wake

```
 owed ─request pin─▶ waking ─promote() under chore_scope─▶ chore_scope ends ─▶ final compare-exchange ─▶ open
   │ gate: failed wake's backoff?          │ lease busy: repost through the timer, 1 ms doubling to 64 ms
   └─▶ wake_failed, no wake, no pin        └─ failed: {p - w, owed}, w request pins failed, backoff
```

- **One gate.** `wake_gate()` decides whether a request pin may start a wake: inside a failed wake's
  backoff (10 ms doubling to 10 s per consecutive failure) it answers the slot's `wake_failed`, which
  carries the last error. The `owed` row and `end_at()` (a demotion or a refused close that ends at
  `owed` with a request waiter, and so would go to `waking`) both ask it; in the backoff the end is
  `owed`, the request waiters fail and lose their pins in the same compare-exchange, and the chore
  waiters are handed theirs.
- **The wake never waits.** It takes `chore_scope`; if a chore pin's caller holds the lease it posts
  itself again through the host's timer (counted in `wake_reposts`, outside the failure count) and
  frees its worker. The host supplies the timer (`catalog` refuses one without): a chore that slept
  would hold the group's only lane, which every transition of the partition shares, and a cancelled
  group would drop the wake. The timer's callback only posts, so no device work runs on the timer's
  thread; `~catalog` waits for the callbacks to leave (a mutex and condition, not the callback's last
  atomic). A timer that throws, a callback whose post throws (it runs the wake's failure on the timer's
  thread instead), and a `wake_failed` that cannot be built each fail the wake with that error instead of
  leaving the slot `waking`; a gated pin's `wake_failed` is shared by every pin in the
  backoff, so handlers must not mutate it.
  A chore pin that meets `waking` waits for the wake's end unless its own thread holds the lease, so no
  hold begins after the wake starts and a stream of chore pins cannot starve it.
- **The end.** `promote()`, then `chore_scope` ends, then the final compare-exchange under the shard
  mutex (so a resumed request acquires the lease from free, not nested in the wake's hold), then the
  waiters are delivered. A failure writes its fields (count, error, retry time) before the exchange.
- **The bound.** A failed wake counts toward `wake_bound` (8) unless it was `too_many_open` (the
  budget's cap: a refusal, not a fault; it takes the backoff only). `begin_bound_close()` is the
  sweep's compare-exchange for a slot at the bound: `{0, owed}` to `{0, closing}`, and a chore that
  commits what is uncommitted with the host's commit chore, demotes whenever the slot has maps (its
  commit's, or a chore pin's commit during the backoff), checks `close_ready()` and closes. Item 5's
  sweep calls it; until then only tests do. A failed commit ends at `owed` with a 10 s probe time and counts in
  `failing()`. The next pin's reopen reports the lasting error.

## The sweep

```
 timer 1 Hz / budget crossing ─▶ post_pass (one queued at a time) ─▶ pass chore: visit <= 256 slots, post <= 16
   failed, 10 s past retry ─▶ free        open, idle 30 s (or over soft_hot) ─▶ demoting ─▶ open | owed
   owed at the bound ─▶ bound close       warm, idle 10 min ─▶ closing ─▶ free
   over a closing cap ─▶ evict (S3-FIFO) ─▶ closing, or demoting for a booked close
   closing and stuck ─▶ try_finish_close()                  pass ends over a cap having posted ─▶ post_pass
```

- **A pass touches no file.** It runs as a chore on the host's executor (the timer's callback only posts it),
  reads each visited slot's word and atomics, and takes the transition with the word's compare-exchange; the
  chore does the I/O. A pass visits `pass_visits` slots from where the last stopped and posts at most
  `pass_posts` transitions. `budget->tick()` runs once per pass.
- **What it picks.** Hot is `open` with a nonzero maps count; warm is `open` with none, or `owed`. A hot slot idle
  `demote_idle_ns` is demoted, and so is one over `soft_hot` or the maps' share (oldest use first among the
  window); a warm slot idle `close_idle_ns` closes. Over a closing cap (`soft_open`, the descriptors' or vnodes'
  share) the policy chooses (below). Only the compare-exchange needs `{0, ...}`; the chore decides after it.
- **What it counts.** `soft_hot` caps hot indexes. A slot's maps count moves through `counted_io`, and its 0-to-1
  and 1-to-0 edges keep the catalog's count of slots with a mapping (`resource_counts::holders_mapped`): one
  more atomic per edge, none per map. Each cap is read less the relief already in flight: demotions begun
  (their maps and hot indexes), closes begun (their descriptors and vnodes, and `closing` slots off the open
  count), and closes a demotion booked.
- **One demotion per close.** A hot slot `evict` demotes because a closing cap is over (`soft_open`, descriptors,
  vnodes) is that close's relief: the pass counts one index, its descriptors and vnodes off the caps by booking
  them (`due`) until the slot's close begins or a pin takes it (a pin by key or handle on it, or a waiter its
  demotion hands a pin). The demotion's end begins that close itself, in `end_at`, while the booking stands (no
  waiter took the slot, no maps left, the sweep running and shutdown not begun), so the close does not wait for a
  pass's cursor to come round to the slot: one index over `soft_open` demotes and closes one slot, in the same
  pass whatever the slot count, and the rest of the hot set stays hot.
- **A refusal is held off a period.** A close the slot refuses, or a demotion that leaves maps, sets the slot's
  `probe_at` to one `sweep_period_ns` after the pass that chose it, and the pass skips it until then, so a
  pass that reposts over a cap does not retry the same refusals at once; the next periodic pass tries again.
- **A crossing posts one pass.** `post_pass()` is coalesced (`pass_queued_`, cleared when the pass ends), the
  budget's listener calls it, and a pass that posted something and still sees pressure after its planned
  relief posts the next at once. A pass that posts nothing does not (everything pinned), so a full
  catalog does not spin; the 1 Hz timer, armed once and re-armed when a pass ends, tries again. The
  timer's callback and the listener hold a shared `life`, so one that outlives the catalog finds it gone.
- **The demotion chore** runs `demote()` and, when the slot's maps count is still nonzero (a retired view a
  reader kept), `reclaim(0)`, all under `demoting`.
- **The close chore** (`close_mode`): `sweep` needs `close_ready()` and a maps count of zero, and posts
  `reclaim()` for a queued reclaim, then ends at the prior state to close on a later pass (CT-R4); `bound`
  commits first; `shutdown` commits and closes whatever the state. `close()` runs in a `try`: a throw ends
  the close at the prior state and counts in `close_failures`. A close that leaves `closing()` true (a view
  kept past its pin) keeps the slot `closing`, is reported in debug builds (`owner_break`) and counted in
  `stuck_closes`; the pass retries `try_finish_close()` in a chore, and its pinners stay suspended.
- **H goes after the close chore.** `after_close()` posts `retire()`, which destroys H, the reader, the index and
  the IO, then makes the close's final compare-exchange. `in_close_chore()` is true on a thread inside a close
  chore, for an H destructor to assert against.
- **A negative entry** is erased `negative_ttl_ns` after its retry time: under the shard mutex, `{0, failed}` to
  `free`, `gen` + 1, the table entry gone. A slot that is reused starts clean at the miss (failures, error,
  retry and probe times, last use).

## The closing policy

Over `soft_open` (or the descriptors' or vnodes' soft cap) the sweep's pass closes in S3-FIFO order. Hot slots
over `soft_hot`, idle slots and the bound close stay the pass's own (the sweep, above).

- **Queues.** A slot enters the small FIFO (a tenth of `soft_open`) when its open ends, or the main
  FIFO when the 64-bit hash of its key is in the ghost. The links live in the slot, under one
  `policy_mu_` held for O(1) steps, taken on a miss and by the pass, never on a hit. The ghost
  (`ghost_set.hh`) holds the hashes of the keys closed last, half as many as `soft_open` (limits.md has the size's reason and its bytes).
- **Frequency.** The word's 2 bits, set by a use in the compare-exchange it already makes, in every
  state it meets (a hit costs no extra store). A `try_pin` that makes a compare-exchange
  (`owed` to `waking`, a `failed` retry) counts its use there; its caller's retry passes `use = false`.
- **`evict`** runs after the pass's window, while a closing cap is over (`closing_over()`: the open slots over
  `soft_open` and the descriptors' and vnodes' shares, each less the closes begun or booked, so a refused
  close books no relief). It takes the small FIFO's head while that FIFO is over its share, else the main FIFO's.
  A candidate leaves its queue and then: with its frequency set, small moves to main with the frequency
  cleared (`fetch_and`), main is decremented and put back at the tail; with it clear, a hot one (it holds a
  map) is demoted for the close it books (one demotion per close) and put back, a warm one is closed by
  `begin_close(idx, unused)`, whose one compare-exchange tests pins zero and frequency zero together;
  anything pinned, in a transition or used since is put back. A put-back checks `gen`, so a slot closed and
  reused behind the pass is not queued again.
- **A close's end** unlinks the slot (a close that no pass began leaves no entry) and adds its hash to the
  ghost when the pass took it from the small FIFO with its frequency clear (`to_ghost`, carried by a demotion's
  booking too); the idle and bound closes are not ghosted. A close the index refuses (`close_refused`, every
  exit that ends at the prior state) clears the mark, then puts the slot back in the main FIFO. A slot is
  queued only while its state is neither `closing` nor `free`, read under `policy_mu_`, so a pass's put-back
  cannot land between a close's unlink and its end.
  `close_refused` reads `gen` while the slot is still `closing` (ours), before `end_at`: a close another thread ends
  and a reopen of the slot for another key after that move `gen` on, so the put-back is refused and the open's own
  enqueue decides the queue (a slot put back while `opening` and then failed would be freed still linked).
- **Limits.** A pass visits at most `pass_visits` slots and examines at most that many queue candidates, and
  posts at most `pass_posts` transitions (`policy_limits`); `lifted()` removes both. One pass runs at a time:
  the coalesced chore.

## Shutdown

`co_await catalog.shutdown()`: new pins fail with `catalog_closed` (by key and by handle); each slot closes as
soon as it is `{0, open/owed}` (the first sweep over the slots, then an event on every unpin to zero and
every transition's end) after the host's commit chore if anything is uncommitted; a failed commit names the key
and closes it uncommitted; a negative entry is erased. The task resumes when no slot is left, with the keys
whose commit failed. No thread waits. A host acts on that resumption, never on polling `shut_down()`, which
turns true just before the waiting tasks are resumed.

- **No pin after it.** A miss counts its slot in `occupied_` before it checks the shutdown flag, and shutdown sets
  the flag before it reads the count, all sequentially consistent: either the miss sees shutdown and fails with
  `catalog_closed`, or shutdown sees the slot and waits for it. So a resumed shutdown proves no pin exists and
  none can be taken, which is what makes destroying the catalog then safe.
- **It converges by itself** once what holds it clears, with or without the sweep: a busy lease retries 1 ms
  doubling to 64 ms; a close left stuck by a view (before or after shutdown began) retries `try_finish_close()`
  every 64 ms until the view goes; a `close()` that throws retries 64 ms apart, with no count to run out, until it does not.
- **What holds it** is named by `shutdown_report()`: every slot not yet free, with its key, state, pins and why
  (`pinned`, `transition`, `lease_busy`, `close_throws`, `view_held`, `waiting`), for the host
  to log.
- **An unfinished shutdown is abandoned by exiting, never by destroying the catalog.** The deadline is the host's
  (Xapiand's `join()`, D16: exit 75), which logs the report and exits. The catalog keeps no clock of its own.
  `~catalog` waits for every armed timer, so destroyed under a view still held it would wait with it.

`~catalog` after a shutdown finds nothing; one that finds open slots asserts in debug builds and keeps their IO,
with its lock and its reservations, for good (D5); one destroyed with a pin held asserts too, and that pin's
release is a use after free.

## Tests

`test/catalog_policy_test.cc` (the ghost against a list, the catalog in lock-step with the S3-FIFO
model request by request, each choice of the policy on a scene, every request-pin row's frequency),
`test/catalog_s8_test.cc` (S8's trace over stub indexes; `s3fifo_model.hh` is the reference),
`test/catalog_test.cc` (cases per criterion, and T6's seeded schedules with another actor's
compare-exchange injected before every one of the word's, now with the bound close, held leases,
timers, fault bursts, passes, the clock past the idle times, and a shutdown after each history),
`test/catalog_wake_test.cc` (S5 and S6, one case per rule, on the host's timer injected; a second thread
holds the lease by handshake), `test/catalog_sweep_test.cc` (S7 and C15, one case per clause),
`test/catalog_stress_test.cc` (threads, a WorkPool, a timer thread running the real sweep at microsecond
idle times beside a thread that takes the primitives at random, the kernel and the resolver failing, and a
shutdown with the pinners still running), fixtures in `test/catalog_world.hh`.
