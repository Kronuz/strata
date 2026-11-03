> Status: CURRENT (2026-11-02) as built on `phase/cold-tier-step2-item3`; the design is
> `docs/design/cold-tier.md` (D4, D5, I7, I9, I10; CT-R13, CT-R15, CT-R23, CT-R28, CT-R29 to CT-R31, CT-R33,
> CT-R34, CT-R35, CT-R40, CT-R41) on `design/cold-tier`. The wake (item 4) is built; the sweep (item 5) is a stub.

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
  frees its worker. The timer's callback only posts, so no device work runs on the timer's thread.
  A chore pin that meets `waking` waits for the wake's end unless its own thread holds the lease, so no
  hold begins after the wake starts and a stream of chore pins cannot starve it.
- **The end.** `promote()`, then `chore_scope` ends, then the final compare-exchange under the shard
  mutex (so a resumed request acquires the lease from free, not nested in the wake's hold), then the
  waiters are delivered. A failure writes its fields (count, error, retry time) before the exchange.
- **The bound.** A failed wake counts toward `wake_bound` (8) unless it was `too_many_open` (the
  budget's cap: a refusal, not a fault; it takes the backoff only). `begin_bound_close()` is the
  sweep's compare-exchange for a slot at the bound: `{0, owed}` to `{0, closing}`, and a chore that
  commits what is uncommitted with the host's commit chore, demotes (the commit's maps go), checks
  `close_ready()` and closes. A failed commit ends at `owed` with a 10 s probe time and counts in
  `failing()`. The next pin's reopen reports the lasting error.

## Stubs

`begin_demote()` and `begin_close()` are the sweep's compare-exchanges (`{0, open}` to `{0, demoting}`,
`{0, open/owed}` to `{0, closing}`) with the chores a pass would run. Item 5 replaces them with the
timer-driven sweep, which also decides when to call `begin_bound_close()`.

## Tests

`test/catalog_test.cc` (cases per criterion, and T6's seeded schedules with another actor's
compare-exchange injected before every one of the word's, now with the bound close, held leases,
timers and fault bursts), `test/catalog_wake_test.cc` (S5 and S6, one case per rule, on the host's
timer injected; a second thread holds the lease by handshake), `test/catalog_stress_test.cc` (threads,
a WorkPool, the sweep's stubs at full speed, the kernel and the resolver failing), fixtures in
`test/catalog_world.hh`.
