# The catalog's layout: which mechanism lives where

> Status: quest catalog-restructure. Step 1: member definitions out of the class body, one header
> per mechanism, moved by cpp-libraries' `tools/outline.py` and proved by `split_check.py --members`.
> Step 2: the closing policy's queues and order as a subobject (below).

## The shape

`catalog<Index, H>` (`include/strata/core/catalog.hh`, described in `catalog.md`) owns one
partition's non-cold indexes: their slots, pins and transitions, the sweep and its closing policy.
One class body held every definition (2,403 lines) of four mechanisms, each with its own invariants.
The class body now holds what a reader needs to see the whole: the data members, the nested types,
and every member's declaration with its comment, in the order of its sections (`// --- the sweep and
shutdown ---`, ...). A definition that fits on one line stays in the class beside its declaration
(the accessors, `pass()`, `shutdown()`).

Every other definition sits in the header of its mechanism, as a `template <class Index, class H>`
definition of `catalog<Index, H>::f`, nested types' members included
(`catalog<Index, H>::pin_awaiter::await_ready`). `catalog.hh` stays the header callers include: it
holds the namespace-scope types (`slot_state`, `catalog_detail::word`, the waiter and its list, the
errors, the shutdown report's types) and the class, and includes the mechanism headers after it.
Each mechanism header includes `catalog.hh`, so it compiles alone; `#pragma once` breaks the cycle.

## Which mechanism lives where

| Header | Mechanism | Members |
|---|---|---|
| `catalog_table.hh` | the table and what every mechanism shares | the constructor and destructor and their teardown (orphan_waiters, release_chunks); the slots' chunks (alloc_slot, free_slot); chores and timers (post, after, after_or_post, timer_end); the host's defaults and checks (default_payload, checked, fresh_uuid), pin_held (the index's call guard), the test hooks |
| `catalog_pin.hh` | the pin protocol | pin_request, pin_chore, repin, try_pin; acquire_impl and the by-handle try_fast; the waiter's life (join, deliver, unlink_waiter, cancel_wait, abandon) and the awaiter's (pin_awaiter, cancel_pin); unpin, drain_unpin, uncount_miss; pin's move and release; reopen, a holder's hand-back of a fenced index |
| `catalog_transitions.hh` | the slot transitions | the sweep's compare-exchanges that begin one (begin_demote, begin_close, begin_due_close, begin_bound_close); the edge count and the relief booked (note, book, unbook, book_due, release_due); the open, wake, demotion, close and replace chores and how each ends (finish_open, finish_wake, end_at, close_refused, after_close, retire, finish_close, erase_failed); shutdown (kick, shutdown_slot, join_shutdown, maybe_complete_shutdown, shutdown_awaiter) |
| `catalog_policy.hh` | the sweep and the closing policy | start, stop_sweep, post_pass, run_pass, arm_period; policy_pass; the soft caps (over_soft, closing_over, share); evict (each candidate decided from its word), policy_enqueue and policy_ghost (with their counts), S3-FIFO's frequency writes (clear_freq, decay) |
| `catalog_s3fifo.hh` | S3-FIFO itself | `catalog_detail::s3fifo`, the policy subobject (step 2), with `policy_limits` and `policy_result`; included by `catalog.hh` before the class |
| `catalog_observe.hh` | observation | snapshot, slots_made, waiters_of, key_at, table_size, stats, shutdown_report, set_pins_for_test: what the tests, the sweep and T4 read |

## Why the transitions stay one header

The slot's word (`pins:24 | shut:1 | state:3 | freq:2 | gen:34`) is one state machine, and each of
its edges is written by a begin and read by an end in another function: `begin_close` takes `{0,
open}` to `{0, closing}` and posts `run_close`, whose `close_refused` ends at the prior state through
`end_at`, or whose `after_close` and `retire` end at `free` or reopen through `finish_close`. The
relief a transition books (`book`, `book_due`) is released by the end that matches it (`end_at`,
`finish_close`, `replace_from`), and the ordering comments say so ("before note(), as in reopen",
"while still `closing`, as in end_at"). Cutting the open, the wake, the close and shutdown into
headers of their own would put each begin and its end, and each booking and its release, in
different files. A pin also begins transitions (a miss's free to opening, owed to waking, failed
to opening, the last unpin's `shut` close, reopen's replace): those compare-exchanges are a pin's
and live with the pins, and each posts a chore of this header and counts its edge with `note`.

Two calls a reader might expect elsewhere:

- `reopen` lives with the pins: it is a holder handing back its pin, and its compare-exchange is a
  pin's. The replace it begins is `run_replace`, in the transitions.
- `clear_freq` and `decay` live with the policy: they are S3-FIFO's frequency writes, the only word
  writes that change no state.

## What stays in the class, and why

Step 1 moves definitions only: every mechanism is still the catalog's, sharing its data members.
The nested types stay in the class body (slot, host, handle, stats_t, pin, pin_awaiter,
shutdown_awaiter, and the request and result types), as SPLITTING.md's class restructure keeps
them: the data members and the declarations name them.

## The policy subobject (step 2)

`catalog_detail::s3fifo<Slots>` (`catalog_s3fifo.hh`), the catalog's `policy_`, holds S3-FIFO's
state: the small and main FIFOs' heads, tails and sizes, the ghost, the small FIFO's cap, and the
mutex that guards them and every slot's links. It owns no slot. `Slots` gives it, for a slot index,
the links it alone writes (`prev`, `next`, `queue`: the slot's `q_prev`, `q_next`, `q`) and two reads
of the slot's word under its mutex: `gen(i)` and `queueable(i, gen)`, the generation still `gen` and
the state neither closing nor free. The catalog's `policy_slots` is that view.

What it decides, each in one place:

1. Order: a slot enters its FIFO's tail (`enqueue`), and `pick` takes the small FIFO's head while
   the small FIFO is over its cap (a tenth of `soft_open`) or the main one is empty, else the main
   FIFO's head.
2. The ghost: a key whose hash is in it enters the main FIFO, and the ghost gives the hash up; a
   key closed from the small FIFO unused enters it (`ghost`, called by `finish_close`).
3. A pass's walk (`evict`): while a closing cap is over and the limits allow (`visits`
   candidates, `transitions` posts), each candidate is decided by the catalog from its word; a used
   one goes to the main FIFO's tail, a kept one back to its own FIFO's tail, and a full round of kept
   candidates ends the pass.
4. The requeue's refusal: a slot goes back only while it is in no queue and `queueable`, read
   under the mutex `finish_close` takes to unlink it once it is `closing`, so a requeue is either
   refused or unlinked after.

What stays in the catalog, because it is the slot's word: the frequency (a use sets it in the pin's
compare-exchange; `clear_freq` and `decay` are the policy's word writes), each candidate's fate
(pinned, in a transition, refused lately, hot and demoted for the close it books, or closed), the
ghost mark a close carries (`to_ghost`), the counts in `stats()`, and when a pass runs (the sweep).

The links stay in the slot, where `q_next` is also the free list's link while the slot is free, so
the slot's 184 bytes (D4) do not change. The policy's own tests are the named cases of
`test/catalog_policy_test.cc` (fifo_order, ghost_to_main, pick_queue, evict_order, kept_round,
limits, requeue_refused, unlink_any), over a table of links with no catalog; the catalog's behavior
tests in the same file (lock-step with the model, each choice of D6 on a scene) run unchanged.
