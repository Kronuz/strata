# Catalog delivery: the waiter's states

A pin or a `shutdown()` that must wait joins a list as a heap `waiter` (`include/strata/core/catalog.hh`); a
transition's end takes the list under the shard mutex (or shutdown's) and `deliver`s each waiter after unlocking.
The waiter's frame may be destroyed at any point (task's teardown, D8 phase 3), so the delivery and the
awaiter's destructor (`abandon`) agree through one atomic, `st`. The catalog's contract with its callers: **once
`abandon` returns, a delivery touches only the reference-counted waiter** (its notify and its reference), never the
frame or its scheduler, so a caller may destroy a scheduler once every pin that used it has returned or been
abandoned (task's phase 4).

```
 registered ──await_suspend──▶ suspended ──deliver──▶ posting ──post returns──▶ delivered
     │                           │                      │                           ▲
     │                           │                      └──abandon──▶ abandoned ────┘ the post returns;
     │                           │                                                    abandon waits for it
     └──deliver──▶ delivered     └──abandon──▶ abandoned
```

## Who may touch what

| `st` | The delivery | The awaiter's frame |
|---|---|---|
| `registered` | Writes `gen`, `error`; moves it to `delivered`, so `await_suspend` declines and the frame continues in place. | Owns `h`; suspends with a compare-exchange from `registered`. |
| `suspended` | Writes `gen`, `error`; moves it to `posting` and reads `h` and `sched`. | Suspended; its destructor may `abandon`. |
| `posting` | Posts `h` to `sched` (or resumes it inline when there is none), then exchanges `delivered`; if that exchange meets `abandoned`, it notifies. | `abandon` exchanges `abandoned`, sees `posting`, and waits on `st` until the delivery's exchange. Then it acts as for `delivered`. |
| `delivered` | Done with the waiter but for its reference. | The resume reads `gen`, `error`; or `abandon` releases the delivered pin (`gone_pins`). |
| `abandoned` | Resumes nothing; releases the pin it carried (`gone_pins`). | Gone. |

A linked waiter (no transition took it yet) never reaches this table: `abandon` unlinks it and uncounts its
pin under the list's mutex. The waiter's two references (the list's and the awaiter's) keep it alive for
whichever side ends last, so neither writes into freed memory.

## Why the wait

Without `posting` the delivery exchanged `delivered` and then posted. A frame destroyed in between saw
`delivered`, released the pin and returned; the owner went on to destroy the scheduler, and the post landed in
freed memory (Xapiand's test_pool heap-use-after-free, CI run 44672373841). The wait is bounded by one `post`,
a queue push, and runs only when a frame dies while its delivery is posting. The hot path stays lock-free: a
delivery to a suspended waiter is one load, one compare-exchange and one exchange, and it notifies only when
`abandon` is waiting.

## Preconditions

The caller's, all kept by task's teardown:

1. **A frame dies only after its scheduler is closed** (task's phase 2 before phase 3). The post `abandon` waits
   through still queues the dead frame's handle; only a closed scheduler drops it instead of resuming freed
   memory later.
2. **A frame with no scheduler is never destroyed while a delivery to it may run.** Its posting step is the
   inline resume itself.
3. **A frame's destruction holds nothing its scheduler's `post` needs, and a `post` never destroys the frame it
   is given**; either would deadlock `abandon` against the post it waits for. task's schedulers destroy roots
   outside their locks, and their posts only queue.

## Tests

`test/catalog_post_lifetime_test.cc` holds a delivery inside the waiter's post (a hand-driven scheduler) while
the test thread closes the scheduler, destroys the frame and destroys the scheduler, once with a pin and once with
a failed open's error (which `abandon` must not unpin). With the wait it passes;
without it, ASan reports the post into the freed scheduler. The hook site `abandon.posting` tells the test that
`abandon` is waiting, so it uses handshakes only. `test/catalog_teardown_test.cc` covers the other states.
