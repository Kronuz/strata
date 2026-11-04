> Status: CURRENT (2026-11-03) as built on `phase/cold-tier-step2-item5b`; the design is
> `docs/design/cold-tier.md` (D4, D5, I7, I9, I10; CT-R13, CT-R15, CT-R23, CT-R28, CT-R29 to CT-R31, CT-R33,
> CT-R34, CT-R35, CT-R40, CT-R41) on `design/cold-tier`, and the host interface is the architect's ruling
> `docs/reviews/cold-tier-xpool-host.md` (F1 to F6). The wake (item 4), the sweep and shutdown (item 5) and the host
> interface (item 5b) are built; the policy that picks what a pass closes (S3-FIFO, item 6) is not: a pass takes the
> oldest use first.

# The catalog: slots, pins and the word

`strata::catalog<Index, H>` (`include/strata/core/catalog.hh`) owns the non-cold indexes of one
partition. Every call on one of them runs under a **pin**; a pin never blocks a thread.

```
 key ─▶ shard (64) ─▶ hash_slots ─▶ slot ─ word: pins:24 | state:4 | freq:2 | gen:34
                      under one mutex       io, index*, reader, H, waiters, counts, backoff
 states: free opening open owed waking demoting closing failed
```

## How it works

- **A slot is 184 B** (plus the host's payload `H`; `catalog_test` `slot_cost` prints it and `static_assert`s D4's
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

## Waiters whose frames die

A waiting pin's frame can be destroyed before the pin arrives: task's teardown (`docs/design/cancellation.md`
D8 phase 3 in the task library) destroys every root suspended anywhere, while the catalog's chores may still run
on a pool. The catalog keeps every primitive's teardown rule there: the frame dies, the catalog stays consistent.

- **The waiter lives on the heap**, in the shape of task's `lock_waiter`: a pin that will wait makes it before
  the compare-exchange that counts it (a failed allocation changes nothing), and its list and its awaiter each
  hold a reference. A delivery writes into the waiter, never into the frame, and drops the list's reference
  last, so a frame that dies mid-delivery never meets a write.
- **`st` is decided by one exchange each side**: `registered`, `suspended` (await_suspend's compare-exchange),
  `delivered` (the delivery's exchange), `abandoned` (the awaiter's destructor's exchange). Whichever comes
  second sees the other's value.
- **The destructor** (`abandon`) takes the list's mutex (the shard's, or shutdown's) and checks `linked`:
  1. Linked: no transition has taken it, so it unlinks it (the list is doubly linked) and subtracts its pin
     from the word in the same section. A waiter's pin is never a holder's, so the subtraction is a plain
     `fetch_sub`: no drain unpin, no kick (a transition is still running and will end on its own; a slot whose
     pins all left ends `{0, open}`, `free` after a close, or `{0, failed}`).
  2. Taken (`take()` clears `linked` under the mutex as it detaches the list): it exchanges `abandoned`. If the
     delivery came first (`delivered`) with a pin, the destructor releases it with an ordinary `unpin`; otherwise
     the delivery sees `abandoned`, resumes nothing, and releases the pin itself.
- **A delivery that is posted and never run** (task's phase 2 drops the post) is the `delivered` case: the frame
  dies with the pin handed over and released by the destructor.
- **The same holds for `shutdown()`'s waiters**, on shutdown's own list under `shut_mu_`; they carry no pin.
- `stats()` counts `unlinked` (waiters removed before their delivery) and `gone_pins` (pins handed to a frame
  that was gone, and released).
- **The catalog outlives every frame that awaits it** (task's ownership rule: a primitive awaited by frames of
  more than one root is destroyed after phase 3). One exception, a host that gives up an unfinished shutdown and
  destroys the catalog anyway: `~catalog` orphans the waiters still listed (as task's `~event` does), so their
  frames, destroyed later, drop only their own references.

## Cancelled pins

`pin_request`, `pin_chore` and `repin` take the caller's `task::cancel_token` last (a request's, a deadline's, a
chore's stop; empty by default), in the shape of task's `async_mutex::lock(token)` (D4 in task's cancellation
design):

- **A pin that need not wait is taken whatever the token**, as a free lock is.
- **A requested token never waits.** The pin runs as `try_pin` does (it starts the open or the wake and holds
  nothing) and throws `task::cancelled`.
- **Otherwise the pin joins, then registers its callback** in `await_suspend`, before the compare-exchange that
  suspends it. The callback (`cancel_wait`) is the destructor's first branch: under the list's mutex, if the waiter
  is still listed, it unlinks it and uncounts its pin, then delivers `task::cancelled` to it. A request that lands
  between the join and the registration runs the callback inside the registration; the delivery then precedes the
  suspension, which declines, and the frame continues in place.
- **A cancel that loses the race returns the real outcome**: a waiter a transition has already taken is left to
  that delivery (the pin, or the open's error).
- `await_resume` and the destructor first destroy the registration, which waits for a callback running on
  another thread, so the callback never meets a freed waiter. `stats().cancels` counts the pins a token ended.

**A task's second pin of a key does not reuse its first** (ruling on review CR2). The catalog has no notion of
a task: pins move across tasks and threads (I7), and nothing in a pin names its owner. Reuse would also defeat
the replace a fence starts: the self-wait (a task that holds a pin, hands another back fenced, and pins the key
again) waits for the replace, and the replace exists so that no new pin meets the fenced index; a reused pin
would be exactly that. The self-wait is the caller's hold-and-wait, as with any lock: a caller avoids it by not
holding two pins of one key across a fence (Xapiand can keep one pin per key per request), and ends it with its
token.

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
   failed, 10 s past retry ─▶ free        open, idle 30 s (or over a cap) ─▶ demoting ─▶ open | owed
   owed at the bound ─▶ bound close       warm, idle 10 min (or over a cap) ─▶ closing ─▶ free
   closing and stuck ─▶ try_finish_close()                  pass ends over a cap having posted ─▶ post_pass
```

- **A pass touches no file.** It runs as a chore on the host's executor (the timer's callback only posts it),
  reads each visited slot's word and atomics, and takes the transition with the word's compare-exchange; the
  chore does the I/O. A pass visits `pass_visits` slots from where the last stopped and posts at most
  `pass_posts` transitions. `budget->tick()` runs once per pass.
- **What it picks.** Hot is `open` with a nonzero maps count; warm is `open` with none, or `owed`. A hot slot idle
  `demote_idle_ns` is demoted; a warm slot idle `close_idle_ns` closes; over a cap (the budget's share, or the
  host's `soft_open` and `soft_hot`) candidates go oldest use first, a hot one demoted first and closing on a
  later pass. Only the compare-exchange needs `{0, ...}`; the chore decides after it.
- **What it counts.** `soft_hot` caps hot indexes. A slot's maps count moves through `counted_io`, and its 0-to-1
  and 1-to-0 edges keep the catalog's count of slots with a mapping (`resource_counts::holders_mapped`): one
  more atomic per edge, none per map. Each cap is read less the relief already in flight: demotions begun
  (their maps and hot indexes), closes begun (their descriptors and vnodes, and `closing` slots off the open
  count), and closes a demotion booked.
- **One demotion per close.** A hot slot demoted because a closing cap is over (`soft_open`, descriptors,
  vnodes) is that close's relief: the pass takes one index, its descriptors and vnodes off the caps and books
  them (`due`) until the slot's close begins or a pin takes it (a pin by key or handle on it, or a waiter its
  demotion hands a pin). The demotion's end posts a pass, which closes the booked slot first; so one index over
  `soft_open` demotes and closes one slot, and the rest of the hot set stays hot.
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
  every 64 ms until the view goes; a `close()` that throws retries 64 ms apart, `close_retries` (8) times,
  then the slot stays `closing` with no timer armed.
- **What holds it** is named by `shutdown_report()`: every slot not yet free, with its key, state, pins and why
  (`pinned`, `transition`, `lease_busy`, `close_throws`, `close_failed`, `view_held`, `waiting`), for the host
  to log.
- **An unfinished shutdown is abandoned by exiting, never by destroying the catalog.** The deadline is the host's
  (Xapiand's `join()`, D16: exit 75), which logs the report and exits. The catalog keeps no clock of its own.
  `~catalog` waits for every armed timer, so destroyed under a view still held it would wait with it.

`~catalog` after a shutdown finds nothing; one that finds open slots asserts in debug builds and keeps their IO,
with its lock and its reservations, for good (D5); one destroyed with a pin held asserts too, and that pin's
release is a use after free.

## The host interface (item 5b)

```
 host:  resolve(opening) ─▶ open_index(io, options, opening) ─▶ open_payload(opening, Index&, optional<H>&) ─▶ pins
 pin:   open | create | resident          reopen(pin&&)           snapshot()          io_counts::reserve / guarded
```

- **Intents (F1).** A pin says what it wants of an absent index. `open` is the pin of the rest of this note.
  `create` also passes a *negative entry* whose cached error is `index_not_created` (classified once, in
  `finish_open`, into the slot's `absent` bit): the slot goes `failed` to `opening` with `gen` + 1 and the open runs with
  `opening.create` set, so the host's `resolve` makes the directory and the default `open_index` makes the index. Any
  other negative entry (EIO, `too_many_open`) gates a create pin like every pin, so a 404 storm still costs one probe per
  retry time. The slot keeps a `create` flag, set under the shard mutex by any create pinner that joins an `opening` or
  `closing` slot; the open chore reads it when it starts (`attempt_create`). An attempt that ran without it and failed
  `index_not_created` while a create pinner joined does not fail its waiters: `finish_open` reposts the open with the flag,
  once, and every waiter, reads included, gets the created index. `resident` (chore pins only; `logic_error` otherwise)
  never opens and never makes a slot: no slot, `failed` or `closing` answers an empty pin (`operator bool` false,
  `try_status::absent`); `open` and `owed` grant as a chore pin does; `opening`, `waking` and `demoting` wait. A
  resident pin that waits on an `opening` slot whose open fails gets the open's error (`index_not_created` for an absent
  key), as every waiter does, not an empty pin; the same holds for the other two transitions. The autocommit treats the
  error like an empty pin: nothing to commit (Xapiand's commit chore, carried). This is the autocommit's pin and FLUSH's.
- **The payload is built in place (F3).** `open_payload(opening, Index&, optional<H>&)` replaces `make_payload`: it
  emplaces H in the slot (H need not move: a leaked slot keeps its payload where it is) and may then work on the index,
  a WAL replay. It runs in the open chore with the slot `opening`, so no pin sees the index first; a throw fails the
  open and its waiters, destroying H, the reader, the index and the IO in that order. A reopen runs it again.
- **The fence is met by the catalog (F4).** `index_fenced` says "reopen to continue". `reopen(pin&&)` takes a holder's
  pin back: `{p, open/owed}` to `{p - 1, closing}`, with the slot's `drain` set to the other holders under the shard
  mutex before the exchange. New pins by key wait in `closing` and by handle fall back to the key. An unpin whose
  `fetch_sub` returns `closing` is a holder's (waiters hold no granted pin there): it reloads the word with acquire,
  which pairs with the exchange's release so it sees `drain`, decrements it, and the last one posts the replace close.
  Every chore that meets a fence takes the same road: the demotion (`{p, demoting}` to `{p, closing}`, no holders), the
  wake (`{p, waking}` to `{p, closing}`, the holders being the pins that are not waiters on the list, counted under the
  mutex; its failure counts toward neither the backoff nor the bound), and the sweep's and the bound close (already
  `closing`). The replace close runs under `chore_scope` (a busy lease retries through the timer): the host's commit chore
  if the index is not fenced and has something uncommitted, then `close()` with no readiness check, then `retire` and
  `finish_close`, which frees the slot or reopens it in place for its waiters (`gen` + 1). A fence within 10 s of the
  slot's last replacement counts as a failed open: the reopen in place waits 10 ms doubling to 10 s, and a quiet 10 s
  starts the count over. Shutdown does not replace: it commits (a fenced index fails and is named) and closes.
- **The demotion checks in first (F6).** `run_demote` calls `check_in()` before it takes the lease, so a finished
  background merge is published. Its output stays unsynced until a commit, and warm needs it durable, so when that is
  all that is uncommitted (`Index::merge_output_unsynced()`) the chore runs the host's commit chore under its lease with
  no pin held (`commit_merge_output`; a failure leaves the slot as it is for a later pass). Only then can
  `warm_quiescent()` hold and the demotion take. `host::commit` therefore runs when a merge's output is the only thing
  uncommitted, besides the bound close, shutdown and the replace close (S14).
  `run_close` does not check in, because an idle close never meets a finished merge. Every idle close follows a
  demotion, and a demotion takes only when `warm_quiescent()` holds, which means no merge is pending. A write after it
  maps a segment, so the slot is hot again and demotes (with its check-in) before it can close. An `owed` slot cannot
  merge (it applies only `map_owed`), whatever `eager_segments` says. Shutdown's close drops a finished merge with or
  without a check-in, as `close()` ends the merges and the output was never synced.
- **`snapshot()`** is `{key, state, pins}` for every slot that is not free, under each shard's mutex, with no access
  to an index: FLUSH snapshots, pins each key `resident` and commits; diagnostics read it. It loads the slot's word
  first and rechecks `key_hash` after: a slot that went to a key of another shard meanwhile shows that owner's word,
  whose `key_hash` store is visible with it, so the slot is skipped and `key` is never read under the wrong mutex.
- **Host files (F5).** `opening.counts` is the slot's `io_counts`. `io_counts::reserve(resource)` and
  `guarded(reservation, f)` are the rule `counted_io` follows, public: a payload reserves a descriptor, and a vnode,
  before each file it opens, runs the call under `guarded`, and keeps the reservations in H, which goes before the slot's
  counts.
- **A call holds a pin (C17, CT-R15).** In debug builds `live()` asks the slot's word, through
  `Index::guard_calls`, whether the caller holds a pin or the slot is inside a transition; a breach is an
  `owner_break`.

## Tests

`test/catalog_teardown_test.cc` (waiters whose frames die and cancelled pins: window tests, a property
test against a model of the pin counts, a threaded test), `test/catalog_host_test.cc` (S10 to S16 and C17: one case per ruling, the window tests on the hooks `reopen`,
`drain.unpin` and `replace.begin`), `test/catalog_test.cc` (cases per criterion, and T6's seeded schedules with another actor's
compare-exchange injected before every one of the word's, now with the bound close, held leases,
timers, fault bursts, passes, the clock past the idle times, and a shutdown after each history),
`test/catalog_wake_test.cc` (S5 and S6, one case per rule, on the host's timer injected; a second thread
holds the lease by handshake), `test/catalog_sweep_test.cc` (S7 and C15, one case per clause),
`test/catalog_stress_test.cc` (threads, a WorkPool, a timer thread running the real sweep at microsecond
idle times beside a thread that takes the primitives at random, the kernel and the resolver failing, and a
shutdown with the pinners still running), fixtures in `test/catalog_world.hh`.
