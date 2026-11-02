> Status: CURRENT (2026-11-02) as built on `phase/cold-tier-step2-item3`; the design is
> `docs/design/cold-tier.md` (D4, D5, I7, I10; CT-R13, CT-R15, CT-R29 to CT-R31, CT-R34, CT-R35, CT-R41)
> on `design/cold-tier`. The wake (item 4) and the sweep (item 5) are stubs here.

# The catalog: slots, pins and the word

`strata::catalog<Index, H>` (`include/strata/core/catalog.hh`) owns the non-cold indexes of one
partition. Every call on one of them runs under a **pin**; a pin never blocks a thread.

```
 key ─▶ shard (64) ─▶ hash_slots ─▶ slot ─ word: pins:24 | state:4 | freq:2 | gen:34
                      under one mutex       io, index, reader, H, waiters, counts, backoff
 states: free opening open owed waking demoting closing failed
```

## How it works

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
- **`try_pin`** starts the open or the wake as a pin would, holds nothing, and answers `would_wait`.
- **Opens** are single-flighted by the slot's `opening` state. A failed open leaves a negative entry
  (`failed`, the error, a retry time doubling from 10 ms to 10 s); pins fail at once until then and
  the first pin after it reopens. A second key on a held directory meets the directory lock's
  `EWOULDBLOCK` once, as a host error.

## Stubs

`begin_demote()` and `begin_close()` are the sweep's compare-exchanges (`{0, open}` to `{0, demoting}`,
`{0, open/owed}` to `{0, closing}`) with the chores a pass would run; `run_wake()` is the wake
(`promote()` under `chore_scope`, reposting at once while the lease is busy, no bound or backoff).
Item 5 replaces the first two with the timer-driven sweep; item 4 the last.

## Tests

`test/catalog_test.cc` (cases per criterion, and T6's seeded schedules with another actor's
compare-exchange injected before every one of the word's), `test/catalog_stress_test.cc` (threads,
a WorkPool, the sweep's stubs at full speed, the kernel and the resolver failing), fixtures in
`test/catalog_world.hh`.
