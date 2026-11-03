# strata review: cold tier step 2, item 3 (the catalog: slots, table, pins, the word)

- Date: 2026-11-02.
- Reviewed: `phase/cold-tier-step2-item3` at strata:b1615dd (`git log main..` : strata:73b54cf catalog,
  strata:0613c35 T6, stress, limits rows, mechanism note, strata:bd6a815 deterministic window tests,
  strata:b1615dd ledger). `include/strata/core/catalog.hh`, `basic_index::lease()` (`index.hh:1115-1117`),
  `test/catalog_test.cc`, `test/catalog_stress_test.cc`, `test/catalog_world.hh`, `docs/mechanisms/catalog.md`,
  `docs/design/limits.md:61-62`, the item 3 section of `docs/status/cold-tier-step2.md`.
- Checked against: `docs/design/cold-tier.md` revision 6 (strata:48ea20f on `design/cold-tier`; not on this
  branch, copied to `.scratch/review-item3/ref/cold-tier-r6.md`): D4 and its table (`:204-282`), D5
  (`:283-345`), I7, I9, I10 (`:576-603`); S4 (`docs/status/cold-tier.md:87-96` on `design/cold-tier`);
  `docs/reviews/cold-tier-design.md` CT-R13, CT-R15, CT-R29, CT-R30, CT-R34, CT-R35, CT-R38, CT-R40,
  CT-R41 and conditions C9, C10, C13; `docs/reviews/cold-tier-step2-items1-2.md` S2-N1, S2-N2.
- Checked by: two builds of the exact commit with Homebrew LLVM 23.1.2 clang++ (`-O1 -g`, asserts live),
  `.scratch/review-item3/b-tsan` (`-fsanitize=thread`) and `b-asan` (`-fsanitize=address,undefined
  -fno-sanitize-recover=all`).
  - `catalog_test 300`: ok under both (TSan 11.9 s, ASan 4.5 s); 48,000 steps, 13,705 pins granted, the 18
    state changes with the ledger's counts exactly (`run-{tsan,asan}-catalog.out`).
  - `catalog_stress_test`: TSan 1,500 x 4 and 8,000 x 6, ASan 1,500 x 4: ok, zero reports
    (`run-tsan-stress.out`, `run-tsan-stress-long.out`, `run-asan-stress.out`).
  - `hotkey_stress.cc` (the stress on 2 keys, 8 threads, the sweeper at 0-8 µs): TSan 3,000 x 8, ASan
    20,000 x 8 (317,752 pins, 407 wakes, 342 `demoting` to `waking`): ok, zero reports.
  - `probes.cc`, eight cases, under both builds; `slot_size.cc`; 8 mutants by `mut.py` (4 of the
    developer's, 4 of mine), all killed.
  - Not run: CI's Linux, FreeBSD and arm64 jobs (run 41539265981, not waited on), the developer's 26
    mutants beyond the 4 rerun, any benchmark, fuzzing (no decoder).

## Verdict

**APPROVE WITH CONDITIONS: the word's protocol holds under both sanitizers and every probe; lands after
I3-B1, a one-line cap the limits row already claims.**

What holds, and reproduces:
- Every write to the word is a compare-exchange or a `fetch_sub` (`catalog.hh:280,296,308,501,529,558,569,615,
  646,715,780`); transitions make their final compare-exchange and take the waiter list under the shard
  mutex and deliver after unlocking (`:600-625`, `:636-656`, `:690-723`, `:772-793`).
- No lost wakeup: a waiter is counted and listed in one critical section (`:501-509`), the `st` handshake
  makes an early delivery decline the suspension (`:223-227`, `:415-431`); `lost_wakeup` resumes a request
  and a chore waiter through `demoting` to `waking` to a failed wake (request failed, chore handed its pin
  on `owed`, pins 0), then a request waiter through a good wake (pin on `open`, flag clear).
- A pin during a close waits and is reopened in place on `gen` + 1, by key and by a pre-close handle
  (`close_pin`: both resume on gen 2 with the new instance, serial 2).
- No ABA through slot reuse: `k0`'s handle on a slot freed and taken by `k1` (gen 1 against 3) falls back
  to the key and lands on `k0`'s own new slot (`aba`); the full-word compare-exchange in `try_fast`
  (`:548-564`) admits no stale state.
- Backoff under a lasting open failure: 10, 20, 40 ms and on to the 10 s cap (`backoff`; the probe steps 50
  ms past 50 ms); 1,354 fast failures resolved nothing; five waiters on one retry cost one resolve, all
  failed, pins 0; a `try_pin` in the backoff answers `failed` and posts nothing.
- The state machine as built is the design's plus two composed edges (below), checked by `allowed_edge()`
  (`catalog_test.cc:571`) and the stress's list; all 18 made in every 300-seed run.

What blocks: I3-B1 (the pin count wraps in release builds).

## The design call: a demotion that ends `owed` with a request waiter goes to `waking`

**Ruling: correct, and the design text is what is wrong.** D4's row "demotion ends `{p, demoting}` to
`{p, owed}`" (`r6:267`) hands every waiter its pin, and with `p > 0` those include request pins
registered during `demoting`. Handing them a pin on `owed` breaks I9 (`r6:588-591`: a request pin "gets its
pin only from the wake's end"), so a read would map on a reactor. Storing `owed` and leaving them suspended
is a lost wakeup, since nothing else posts the wake. Storing `owed` and re-running their acquire is
revision 4's re-pin after the store, starvable by the sweep (CT-R29, `slot_model` A1). The build's edge
(`end_at`, `catalog.hh:630-657`) is the table's two rows, `demoting` to `owed` and the first waiter's
`owed` to `waking`, composed in one compare-exchange under the mutex. The waiters are already in
`pins`, so it is exactly the state the decomposed rows reach, with no window between them. Chore waiters
staying suspended through that wake is right too: CT-R40's rule is that no lease hold begins after the
wake, and a chore waiter handed its pin at `waking` would begin one (mutant AM3, killed;
`lost_wakeup` asserts the chore resumes on `owed` after the failed wake). The refused close's
`closing` to `waking` (`run_close` to `end_at(closing, prior)`, `:753-755`) is the same composition and the
same ruling.

No intermediate state is needed. The obligations it creates are I3-N1 (the wake's gate applies at
the composed edges too) and the text change in I3-N8.

**The 18 edges against the design.** D4's table has 14 state-changing pairs (`free`>`opening`,
`opening`>`open`, `opening`>`failed`, `failed`>`opening`, `open`>`demoting`, `demoting`>`open`,
`demoting`>`owed`, `owed`>`waking`, `waking`>`open`, `waking`>`owed`, `open`>`closing`, `owed`>`closing`,
`closing`>`free`, `closing`>`opening`). D5's prose adds `closing`>`open` and `closing`>`owed` ("ends at its
prior state", `r6:334`). The build adds the two composed edges, `demoting`>`waking` and `closing`>`waking`.
That is the 18; the built set and the allowed set are equal, and none is missing that item 3 owns. Not built,
and owned later: `failed`>`free` (the sweep's erase of a negative entry, item 5) and the shutdown's closes
(item 5). The non-state rows (pins on `open`, chore pins on `open`/`owed`, `held_here()` on `waking`, waits on
the transitional states, the fast failure, unpin, the policy's decrement) match `acquire_impl`
(`:454-499`), `try_fast`, `unpin` and `decay`. The decrement is a compare-exchange loop where the table says
fetch-op; the ledger's reason (a `fetch_sub` at zero borrows from `gen`) is right.

## Findings

**I3-B1. The pin count wraps silently in release builds (blocking; small).**
- `add_pin` is `w + one_pin` (`catalog.hh:66`) and the only guard is `assert(pins < max_pins)` (`:450`;
  none in `try_fast`, `:558`). Probe `aba`: `add_pin` on `{16,777,215, open}` gives `{0, open}`, the carry
  dropped off the top. With the count at zero and waiters registered, `begin_close` takes the slot and the
  close frees it under live pins.
- `limits.md:62` calls the row Load-bearing with "asserts below it in debug builds" as the mechanism. A
  debug assert is not a mechanism in the build that serves.
- Reaching it takes 16.7M concurrent pins on one key, so a flood behind a slow open; unlikely, but the
  failure is memory corruption, not an error.
- Fix: in `acquire_impl` and `try_fast`, a slot at `max_pins` fails the pin with `too_many_open` (overload,
  ARCHITECTURE's error) before the compare-exchange; `limits.md:62` names that. A test that sets a slot's
  pins near the cap through a test hook, and the mutant "no check" (the wrap).

**I3-N1. The wake's gate must cover the composed edges (condition on item 4).**
- The ledger's "Next" puts item 4's `wake_gate` on "the `owed` row" only. `closing` to `waking` is reachable
  on a slot inside a wake backoff: a failed wake leaves `owed`; the sweep closes `{0, owed}`; a request pin
  waits on `closing`; the close is refused (anything uncommitted); `end_at` posts a wake inside the backoff,
  around the 8-failure bound and `too_many_open`'s backoff (D5, `r6:321-330`).
- Fix: one function decides "start the wake or fail with `wake_failed`" and both `acquire_impl`'s `owed`
  row (`:458-467`) and `end_at` (`:642-653`) call it; in the backoff, `end_at` ends at `owed` and fails the
  request waiters in the same compare-exchange. S5 gains that case and the mutant "gate only at the pin".

**I3-N2. Destruction with pins held is a use-after-free, and open slots are destroyed, not leaked
(condition on item 5).**
- Probe `teardown_pin` (ASan): `~catalog` (`:249-252`) waits its group and frees the chunks with a pin held;
  the later `pin::release()` reads the freed catalog: heap-use-after-free. No assert fires in a debug build.
- D5 (`r6:343-345`) says a catalog destroyed without `shutdown()` asserts in debug builds and leaks every
  open slot's IO with its lock; as built it destroys each slot's index (uncommitted work dropped) and then
  its IO, under any views a closing index leaked (CT-R15's remainder, which the ledger defers to item 5).
- `teardown_waiter`: a waiter on a pending open is resumed by `group_->wait()` inside the destructor and
  releases cleanly; no problem there.
- Fix: item 5's `shutdown()`; now, cheaply, `~catalog` asserts every word is `{0, free/failed}`.

**I3-N3. A reused slot keeps its old key's backoff and time (condition on item 5).**
- The miss (`:512-540`) writes the key and the word but not `failures`, `error`, `retry_at` or `last_use`
  (`:120,132-134`). Unreachable today: a freed slot came from a successful close (`failures` 0), and no
  `failed` slot is freed. Once item 5 erases negative entries, a new key's first failure starts at its
  predecessor's doubled delay, and the policy sees the old key's last use. Reading only.
- Fix: reset them on the miss, or on the erase; a test that erases a negative entry and fails a new key once
  (10 ms).

**I3-N4. A slot costs about 1.6 KB, not 192 B, and free slots keep it (condition before T1).**
- `slot_size`: `sizeof(kv_index)` 1,496 B, `optional<kv_index>` 1,504 B, inline in the slot (`:129`); with
  the key, the waiter list, counts, reader, error and links, about 1.6 KB, against D4's "about 192 B plus
  `H`" (`r6:215`, and N1's warm sum, `r6:696`). The first miss allocates a 256-slot chunk (about 420 KB),
  and freed slots stay on the free list for the catalog's life.
- So the catalog keeps its high-water mark of non-cold indexes: after a burst to macOS's hard vnode cap
  (157,900) and back to cold, about 250 MB stays allocated. Cold's "nothing in the process" holds per index,
  not per catalog. While a slot is in use the index costs the same inline or on the heap.
- Fix (designer's call): the index behind a pointer made at open (one allocation beside the open's 54
  syscalls; the high-water then about 150 B a slot), or D4 and T1 state the inline cost and the retention.

**I3-N5. A `try_pin` that is a use sets no frequency on the compare-exchange it makes (condition C10,
item 6).** Probe `sync_use`: `try_pin(use)` on `owed` moves the slot to `waking` with frequency 0
(`:463`); on the `failed` retry likewise (`:490`). CT-R41 says a use sets it "whatever state it meets". Fix:
`add_pin`'s use bit without the pin on those two rows, or the ledger states that a `try_pin`'s use counts on
the retry that pins.

**I3-N6. A close that leaves `closing()` true strands the slot (item 5, confirmed).** Probe `closing_stuck`:
a view kept past its pin with no mapping (lazy segments) passes the maps-zero test; `close()` leaves the
index closing and `run_close` returns (`:758`); a later pin waits with no end. D5 calls this a host bug that
item 5's retries cover. Also by reading: `close()` (`:757`) runs outside the `try`, and its throws
(`require_buffers_unpinned`, `index_fenced`, `index.hh:645-652`) are dropped by the executor, which strands
the slot in `closing` with its waiters; item 5's close chore catches and ends at the prior state.

**I3-N7. CT-R15's debug pin check at `live()` is not built (condition before a host adopts the catalog).**
The ledger names the deferral. It is the check that catches a host keeping an index past its pin, the bug
behind I3-N6 and N2; it belongs in before X-POOL moves Xapiand onto the catalog.

**I3-N8. Text (not blocking).**
1. Design revision 7: D4's table gains `{p, demoting}` to `{p, waking}` and `{p, closing}` to
   `{p, waking}` ("a request waiter is registered and the flag is set; posts the wake through the gate; chore
   waiters wait for its end"), and the refused close's `{p, closing}` to `{p, prior}`; I9's "it ends at
   `owed`" gains "or at `waking` when a request pin waits".
2. Ledger: S4's mutant "waiters re-pin after the store" is mapped to W1, which hands an uncounted pin and
   does not re-pin; the starvation it names is guarded by S1 (the sweep takes a pinned slot) and the
   `pins == held` invariant. Say that rather than "A1 is W1".
3. Ledger, C13: the options each test runs with (`keyed`, `eager_segments` on unless a case says,
   `auto_merge` off, `defer_io` per case) in one line.
4. `docs/mechanisms/catalog.md`: the two composed edges.
5. `wake_reposts` is 0 in every run (catalog_test, both stresses, the hot-key probe's 526 wakes over its two runs): the
   stub's repost path is never taken. Item 4's S5 must make it non-zero.

## Blocking items / Required changes

- **L1 (I3-B1).** A slot at `max_pins` fails the pin with `too_many_open` in `acquire_impl` and `try_fast`;
  `limits.md:62` names that mechanism; a test at the cap and its mutant.

## Conditions

- **C14 (item 4).** I3-N1: the wake's gate is one function, called at the `owed` row and at both composed
  edges; S5's case and mutant.
- **C15 (item 5).** I3-N2, I3-N3, I3-N6: `shutdown()`, `~catalog`'s debug assert and leak per D5, slot fields
  reset on reuse, the close chore's `closing()` retries and its caught throws.
- **C16 (before T1 measures warm).** I3-N4: the slot's measured cost and retention in D4 and T1, or the index
  behind a pointer.
- **C10 (carried, item 6)** gains I3-N5.
- **C17 (before X-POOL).** I3-N7: CT-R15's pin check at `live()` in debug builds.
- Carried and checked: CT-R13 (unpin's time then `fetch_sub`, `unpin_order`; the sweep reads fields only after
  its compare-exchange, S3 under TSan), CT-R29, CT-R30, CT-R34, CT-R35 (C9) are met in code with the S4
  mutants; CT-R15's `hash_slots` reuse and the second key's `EWOULDBLOCK` are met; S2-N2's counts lifetime
  holds (`counts` declared before `io` and `index`, `:125-129`). C8 and C11 are not this item's.

## Probes and artifacts

All in `strata-wt/cold-tier-step2-item3/.scratch/review-item3/`, kept:
- `probe.sh VARIANT SRC`: builds a probe with the `b-{tsan,asan}` flags and libraries into `bin-VARIANT/`.
- `probes.cc` (cases `lost_wakeup`, `close_pin`, `aba`, `backoff`, `sync_use`, `closing_stuck`,
  `teardown_pin`, `teardown_waiter`); results `probes-asan.out`, `probes-2.out`.
- `hotkey_stress.cc` (the stress with 2 keys, every op on them, the sweeper at 0-8 µs); `hotkey-{tsan,asan}.out`.
- `slot_size.cc`, `slot_size.out`.
- `mut.py ID...` (the developer's `mutants.py` list plus AM1 to AM4, built on `b-asan`); `mut.out`: D1, X1,
  P1, C1, AM1 (only the first waiter's kind read), AM2 (handle pins in any state), AM3 (the composed edge
  hands every waiter its pin), AM4 (a failed open keeps the waiters' pins): all killed in `catalog_test`.
- `ref/cold-tier-r6.md`, `ref/cold-tier-design-review.md`: the design and its review from `design/cold-tier`.
- Rerun: `quiet light ct3-review 30 "item 3 review" -- cmake --build .scratch/review-item3/b-tsan --target
  catalog_test catalog_stress_test -j4` (and `b-asan`), then `./probe.sh asan probes.cc` and
  `bin-asan/probes CASE`.

Not run: CI's Linux, FreeBSD and arm64 jobs; the developer's other 22 mutants; benchmarks; fuzzing;
the 300-seed schedules under TSan more than once.
