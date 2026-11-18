# strata review: the catalog's host interface for X-POOL (gaps F1 to F6)

- Date: 2026-11-02.
- Reviewed: Xapiand `docs/design/x-pool.md` at Xapiand:6108858e7 (D1 to D12, XP1 to XP7, F1 to F6);
  `include/strata/core/catalog.hh` on `phase/cold-tier-step2-item5` at strata:a748805 (which merges item 4), read
  against `phase/cold-tier-step2-item4` at strata:1b0c382 (fix round uncommitted in its worktree);
  `docs/mechanisms/catalog.md` (item 5); `include/strata/core/index.hh`, `counted_io.hh`, `resource_budget.hh`
  (item 5); Xapiand `src/database/pool.{h,cc}`, `src/database/shard.cc`, `src/engine/strata/shard.cc`,
  `docs/mechanisms/reader-views.md`, `src/server/search_views.cc` (restore), on the x-pool worktree.
- Checked against: `docs/design/cold-tier.md` revision 7 (strata:5723f3d): D4 (host list, word table), D5 (the
  wake, its bound, warm to cold, shutdown), D6, I7, I9, I10; my reviews `cold-tier-step2-item3.md` (C14, C15, C17)
  and `cold-tier-step2-item4.md` (C18 to C20); `docs/design/shared-reader.md` revision 4 (R4.1, the index-owned
  registry); the owner's one-shape rule and "engine not shaped by Xapiand".
- Checked by: two probes built with Homebrew LLVM clang++ against item 5's headers and fixtures
  (`-fsanitize=address,undefined`, dependency libraries from item 4's review build), in
  `.scratch/review-xpool/`: `probes.cc` (cases `fenced_stuck`, `absent_backoff`; zero sanitizer reports, output
  `probes-asan.out`) and `pinned_payload.cc` (a compile probe, `pinned_payload.out`). The registry cap is
  already proved by `test/shared_view_test.cc:78-92`, which I read rather than reran.
  - Not run: TSan; the catalog's own tests; CI; benchmarks; fuzzing; any Xapiand build.

## Verdict

**APPROVE WITH CONDITIONS: X-POOL proceeds on the rulings below. Two gaps (F2, F6) close without a new host
hook, four change the catalog, and two of those are catalog defects the probes found under the X-POOL
symptoms (a fenced index never leaves its slot; a payload must be movable). All of it is one item, 5b,
stacked on item 5 and landing before X-POOL's step 2.**

| Gap | Ruling | Shape |
|---|---|---|
| F1 create intent | Amended | `open_intent {open, create, resident}` on a pin; only `create` passes an *absent* negative entry; `resident` serves the autocommit |
| F2 payload before index | Rejected | Xapiand drops its per-index registry and admission gate for strata's index-owned registry (R4.1); one `index_options` serves |
| F3 a step at open | Amended | Folded into the payload's construction: `open_payload(opening, Index&, optional<H>&)` builds H in place and may replay; a throw fails the open |
| F4 reopen on request | Amended, widened | The catalog replaces a fenced index itself: `reopen(pin&&)`, a holder drain in `closing`, and every catalog chore treats `index_fenced` as "replace". Restore needs no reopen |
| F5 host descriptors | Accepted | `opening.counts` reaches the payload; the reservation helpers become public |
| F6 enumeration | Rejected as a work hook | Engine upkeep by check-ins (the turn's end, the catalog's demote and close chores); keyed `resident` pins for autocommit and FLUSH; `snapshot()` of keys for diagnostics |

What holds, and reproduces:
- **F1's symptom is real.** Probe `absent_backoff`: a key with no index fails its open with
  `strata::index_not_created`; 10 of 10 pins taken inside each backoff fail fast with no open; 5 s after the
  11th failure a pin still fails fast (`probes-asan.out`). The row is `catalog.hh:752-756`; nothing in a pin can
  pass it.
- **F2 is already served by strata.** With `index_options::readers` null, `reader()` makes the index's own
  registry, owned by the shared reader state with the index and its handles (`index.hh:1424-1432`, R4.1), capped
  at `reader_records`; the acquire past the cap throws `too_many_readers` without taking a record
  (`shared_view_test.cc:78-92`). Xapiand's `StrataReaders` (`src/engine/strata/shard.cc:184-195`, wired at
  `:463-466`) is that registry plus an `AdmissionGate` of the same size that refuses first; the adapter already maps
  `too_many_readers` to the same 503 (`:108`). Under the catalog no guard outlives its pin (I7), so the gate and
  the separate registry are a parallel mechanism, not a need.
- **F3, F5 and the slot's lifetimes compose with items 4 and 5** once H is built in place (XH3): the open chore
  runs with the slot `opening`, so no pin sees the index until `finish_open`'s exchange (`:874-909`); a throw
  already resets payload, reader, index and IO (`:862-867`); `retire` destroys H first, off the close chore
  (`:1221-1230`).
- **C14 and the wake's bound keep their meaning** under every new row (see "Composition").

What blocks X-POOL's step 2: XH3 and XH4 (catalog defects), and item 5b as specified. Nothing here blocks
X-POOL's step 1 (the seam), which starts now.

## Rulings

### F1. Create intent: amended

The developer's shape is right on the open and wrong on the negative entry. An absent index stays a negative
entry: a 404 storm on a missing key would otherwise cost an open per request (0.4 to 1.1 ms p50 on macOS,
`cold-tier.md` Limits), and the backoff is what keeps it at one probe per retry time. What changes is who may pass
it: a pin says what it wants of an absent index.

- `enum class open_intent : unsigned char { open, create, resident }` on `pin_request`, `pin_chore`, `repin` and
  `try_pin` (default `open`; `resident` only on chore pins, `logic_error` otherwise).
- **`create`** passes a `failed` slot before its retry time **only when the slot's error is `index_not_created`**
  (classified once, in `finish_open`, into a slot bit; never a rethrow on the pin path). Any other negative entry
  (EIO, `too_many_open`) gates a create pin like every pin. The slot keeps a `create` flag set under the shard
  mutex by any create pinner; the open chore reads it when it starts and passes it to the host. An attempt that
  ran without `create` and failed `index_not_created` while a create pinner joined does not fail its waiters:
  `finish_open` reposts the open, the slot stays `opening`, and every waiter, reads included, gets the created
  index. The repost happens at most once per attempt (the second attempt runs with the flag).
- **`resident`** never inserts and never opens: no slot, `failed` or `closing` gives an empty pin
  (`operator bool` false, no exception); `open` and `owed` grant as a chore pin does; `opening`, `waking`,
  `demoting` wait. This is Xapiand's autocommit pin (`pool.h:541`, "empty when the registry has no entry"), and
  FLUSH's (F6).
- `index_not_created` is the one absent signal: the host's `resolve` throws it for a missing directory when not
  creating, and the default `open_index` creates when `create` and the open finds none.
- Limit (stated in `cold-tier.md`): an index created outside the catalog is seen once its negative entry's retry
  time passes (at most 10 s). A host creates through a create pin.

### F2. Payload before index: rejected

No strata change. X-POOL D1 drops `StrataReaders` and the `AdmissionGate` from `IndexEntry`; the catalog's one
`index_options` sets `readers = nullptr` and `reader_records = readers_per_index`; `views_refused()` counts
`too_many_readers`. H is made after the index and destroyed before it (today's order), which is right once
nothing the index points at lives in H.

### F3. A step at open: amended, folded into the payload

The need is real (the WAL replays before the first reader, `src/database/shard.cc:482-537`), and generic: any
host may validate or load state before the first pin. But a separate `after_open(H&, Index&)` beside
`make_payload` is two callbacks for one act, and the payload as built cannot be Xapiand's anyway (XH3). One
callback builds H in place and may then work on the index:

```cpp
struct opening {
	std::string_view key;
	io_counts counts;     // the slot's: what the host opens for this index reserves here (F5)
	bool create = false;  // a create pin asked (F1)
};
std::function<std::unique_ptr<kronuz::journal::IO>(const opening&)> resolve;
std::function<Index(kronuz::journal::IO&, const typename Index::options&, const opening&)> open_index;
// Emplaces H (it need not move), then may run host work on the index (a WAL replay); a throw fails the
// open, destroying H, then the reader, the index and the IO. Default: h.emplace().
std::function<void(const opening&, Index&, std::optional<H>&)> open_payload;
```

`make_payload` goes (migrate callers, no shim). The step runs in the open chore with the slot `opening`, before
`finish_open`'s exchange: no pin, request or chore, sees the index first. The step runs with no `chore_scope`;
index calls take the lease themselves, and nothing else can hold it at `opening`.

### F4. Reopen on request: amended and widened

Two corrections to the gap as written, then the shape.

- **Restore replaces nothing.** A RESTORE indexes into a create-or-open index with the WAL off
  (`search_views.cc:1658`, `handler.cc:989`) and deletes the old WAL at release (`shard.cc:1353-1363`);
  `shard.cc:1401` is the generic stale reopen. Restore needs F1's `create` only.
- **The real case is strata's own, and the catalog is broken on it today (XH4).** `index_fenced` says "reopen to
  continue" (`index.hh:96-99`), and the catalog owns every open. Probe `fenced_stuck`: an index fenced by a failed
  sync under a chore pin stays `open` through five sweep rounds 11 minutes apart (each demotion and each close is
  taken and ends `open`, `pass_closes` 0); a new request pin is handed the same index (writes throw
  `index_fenced`, reads serve its last view). `demote()` and `close_ready()` call `live()` (`index.hh:1067-1069`,
  `:1087-1089`), which throws on a fenced index (`:2645-2647`); `run_demote` and `run_close` swallow it and end at
  the prior state (`catalog.hh:1100`, `:1173`). The same holds for a fenced `owed` index: `promote()` calls
  `live()`, the wake fails, backs off, reaches the bound, and the bound close fails `close_ready()` the same way.
  Only `shutdown()` closes it.

The shape: **the catalog replaces a fenced index, and new pins wait while its holders drain.**

- `void reopen(pin&& p)`: a holder whose call threw `index_fenced` hands its pin back. Under the shard mutex,
  `{p, open/owed}` goes to `{p-1, closing}` with the slot's `drain` set to `p-1` (the other holders) before the
  exchange; at zero the replace close is posted at once. On `waking` (only a chore pin with `held_here()` holds
  one there) it is a plain unpin: the wake's `promote()` meets the fence and takes the next row.
- **Holders drain in `closing`.** New pins by key wait there already (`:741-751`), and by handle fall back to the
  key (`:832`). An unpin whose `fetch_sub` returns `closing` is a holder's (waiters hold no granted pin there), so
  it issues an acquire fence (pairing with the exchange's release, so it sees `drain`), decrements `drain`, and
  the last one posts the replace close. The common unpin pays one branch on the value it already has.
- **Every catalog chore that meets `index_fenced` replaces instead of ending at the prior state:** the demotion
  (`{p, demoting}` to `{p, closing}`, drain 0, a new edge), the sweep's and the bound close (already `closing`), and
  the wake (`{p, waking}` to `{p, closing}`, drain = pins less the waiters on the list, counted under the mutex;
  request and chore waiters keep waiting; the failure counts toward neither the backoff nor the bound).
- **The replace close** runs under `chore_scope` (a timer retry if the lease is busy, as shutdown's): the host's
  commit chore only if the index is not fenced and `uncommitted()`, then `close()` with no readiness check, then
  `retire` and `finish_close`, which frees the slot or reopens in place for its waiters (`{p, opening}`, `gen` + 1,
  CT-R30). The reopen runs the host's `open_payload`, so Xapiand's WAL replays what the fence lost. `index.hh`
  gains `bool fenced() const noexcept`, read under the lease.
- **A persistent fault must not become a reopen storm.** A fence within 10 s of the slot's last reopen counts as a
  failed open, and the reopen in place waits for the retry time (10 ms doubling to 10 s); the count decays after 10 s
  without a fence. Xapiand's replay commits at open, so there a persistent EIO already fails the open and backs off.
- Xapiand: `index_fenced` maps to `reopen(std::move(pin))` and a retry by key; `corrupt_segment` to `heal()` under
  the turn; `index_closed` cannot reach a pinned caller. `mark_stale` and `stale_` go.

### F5. Host descriptors: accepted

`opening.counts` is the slot's `io_counts`. The payload reserves a descriptor and, on macOS, a vnode on it before
each file it opens, returns them if the call fails, and notes `EMFILE`, `ENFILE` and `ENOMEM` as external
pressure, exactly as `counted_io` does (`counted_io.hh:44-56`, `:150-151`). Promote that rule to a public helper
(`io_counts::reserve(resource)` and `io_counts::guarded(reservation, f)`), so a host's file follows the one rule
and evicting the slot names what it frees. Reservations held in H return when `retire` resets H; `counts` is
declared first in `slot` and outlives them (`catalog.hh:172-173`).

### F6. Enumerating open indexes: rejected as a work hook

What Xapiand's housekeeping does is engine work: publish finished merges and free released views and queued
files (`shard.cc:623-640`). A host hook would run strata's upkeep through the host, and a `for_each_open` would be
a second walk beside the sweep's cursor. Each piece has an owner already:

- **The turn's end checks the index in.** Strata's checkout contract (`index.hh:809-850`): a client's hold ends
  with `check_in()`, which publishes finished merges and frees released views under the lease when it is free.
  Xapiand's `Turn` release calls it. It must be the turn holder's: a check-in is safe only while no thread makes
  the owner's lease-less reads (`index.hh:828-835`), which Xapiand's writer makes under its turn, so the catalog
  cannot run one on a pinned slot.
- **The catalog's demotion and close chores check in first** (5b). `warm_quiescent()` refuses while any merge is
  pending (`index.hh:4208-4210`), and finished merges publish only at a writer call or a check-in
  (`:2750-2762`; `run_merge`, `:3146`, only sets the job's `done` flag). Today `run_demote` and `run_close` never
  check in, so an idle index whose background merge finished stays hot. Read from the code; the test below
  settles it.
- **Autocommit stays keyed:** the debounced timer per written key (`manager.cc:526-528`) takes a `resident`
  chore pin. When a commit leaves merges pending, Xapiand re-arms it, which covers a read-only busy index.
- **FLUSH and diagnostics:** `snapshot()` returns `{key, state, pins}` for every non-free slot, under each shard
  mutex, with no index access. FLUSH pins each key `resident`, awaits the turn and commits; a key closed since the
  snapshot gives an empty pin (it closed committed). `busy()`, `count()` and `dump_databases()` read the snapshot
  and the stats. Hosts stop walking the test accessors (`slots_made()`, `key_at()`, `:542-563`).

## Item 5b: the host interface (one item)

**Placement.** Stacked on item 5's branch as `phase/cold-tier-step2-item5b`, built by item 5's developer once
item 5 is in review (one writer per file set; both edit `run_open`, `run_close`, `unpin` and `pass()`), rebased onto
item 5's fix round. It lands after item 5 and before X-POOL's step 2 lands; Xapiand builds step 2 against the 5b
branch meanwhile. Item 6 (S3-FIFO) rebases onto it. It carries C17 (CT-R15's pin check at `live()`), owed before
X-POOL.

**The interface.** `catalog::host` loses `make_payload` and gains `open_payload`; `resolve` and `open_index` take
`const opening&` (F1, F3, F5). The pin calls take `open_intent`. New: `reopen(pin&&)`, `snapshot()`,
`index::fenced()`, the public reservation helpers. The slot gains `drain` (32 bits), the `create` and `absent`
bits; it stays within D4's 192 B (`slot_cost`).

**The word's new rows** (D4's table gains them; design revision 8):

| Writer | From | To |
|---|---|---|
| create pin | `{p, failed}` before its retry time, slot absent | `{p+1, opening}`, `gen` + 1; posts the open with `create` |
| open ends, absent, a create pinner joined after it started | `{p, opening}` | unchanged; the open is reposted with `create` |
| resident pin | no slot, `{p, failed}`, `{p, closing}` | none: an empty pin |
| `reopen(pin)` | `{p, open/owed}` | `{p-1, closing}`, drain `p-1`; at 0 posts the replace close |
| unpin, a holder in a drain | `{p, closing}` | `{p-1, closing}` (fetch_sub, then acquire fence); the last posts the replace close |
| wake ends, `index_fenced` | `{p, waking}` | `{p, closing}`, drain = holders; waiters keep waiting |
| demotion ends, `index_fenced` | `{p, demoting}` | `{p, closing}`, drain 0; posts the replace close |
| replace close ends | `{p, closing}` | `free`, or `{p, opening}`, `gen` + 1, as every close |

**Done criteria.** Each criterion has a test and a mutant the test kills, on `inline_executor` and on the work
pool, with the options it runs with (C7's shape); separate TSan and ASan+UBSan builds; Linux TSan in CI is the
authority (calibration 2026-10-29).

- **S10 create and resident (F1).** (a) Ten read pins on an absent key fail `index_not_created`; a create pin
  inside the backoff creates; the reads after it see the index. Mutant: create obeys the negative entry (the
  `absent_backoff` probe's behavior). (b) A create pin joins an `opening` slot whose attempt runs without create:
  every waiter, read and create, gets the created index, none fails, opens 2. Mutant: `finish_open` fails the
  waiters. (c) A create pin on an EIO negative entry fails fast. Mutant: create passes every negative entry. (d) A
  resident pin on an absent key: empty pin, `table_size()` and `resolves` unchanged; on `open` granted; on
  `closing` empty. Mutant: resident inserts.
- **S11 payload in place and the open step (F3).** `pinned_payload.cc`'s H (deleted move, an atomic member)
  compiles and runs as a catalog payload. The step applies a document and the first waiter's read sees it; a step
  that throws fails the open and its waiters, destroys H, reader, index, IO in that order (a destructor log), and
  leaves a negative entry. Mutants: hand out the pins before the step; destroy the index before H.
- **S12 the drain and reopen (F4).** Three holders; one calls `reopen` after a fenced commit; pins by key and by
  handle that arrive wait in `closing`; the last holder's unpin posts the close; waiters get a fresh, unfenced
  index with `gen` + 1, the host step re-ran, `pins == held` throughout. Mutants: post the close at `reopen` with
  holders (ASan); drop the acquire fence (TSan, and on arm64 CI); count waiters as holders (a hang the test times
  out). T6's 1,000 seeded schedules gain `reopen` and the drain unpin as writers of the word.
- **S13 fence in every chore (F4).** `fenced_stuck` becomes a test: an idle fenced slot is replaced on the next
  pass; a fenced `owed` slot's wake replaces it, its request waiters get the new index, `wake_failures` and the
  bound count unchanged; a bound close of a fenced slot closes. Mutant: today's catch-all (ends at `open`). A
  persistent sync fault: reopens bounded by the open backoff (at most 10 in the first 10 s: one at once, then 10 ms doubling). Mutant: no decay
  rule.
- **S14 check-in before demote and close (F6).** An idle index whose background merge finished (auto-merge on a
  work-pool executor) demotes on the first pass after the merge, and closes later. Mutant: no check-in (it stays
  hot every pass).
- **S15 snapshot (F6).** Keys and states of every non-free slot, `owed` and `failed` included; a FLUSH-shaped test
  (snapshot, resident pins, commit) commits every uncommitted index and skips a key closed in between. Mutant:
  the snapshot skips `owed`.
- **S16 host files counted (F5).** A payload opening two files reserves on `opening.counts`; the slot's
  descriptor and vnode counts include them; at the hard cap the open fails `too_many_open` with no kernel error
  and backs off; after retire the counts return to zero, and T7's comparison holds with host files. Mutant:
  reserve after the system call.
- **C14 kept.** Item 4's oracle (no wake posted inside a backoff, `cold-tier-step2-item4/.scratch/review-item4/
  inc-oracle/`) rerun over T6 and the stress with the new rows, clean; its no-gate mutant still aborts.
- **C17.** CT-R15's debug check at `live()`: a call on a catalog index with no pin and no transition aborts.
- **Text.** `cold-tier.md` revision 8 (D4's host list and table, D5's fence replacement and the create intent,
  Limits: outside creation, a read-only busy index's merges); the mechanism note's sections for the intents, the
  drain and the fence; `limits.md` rows for the drain's 2^24 holders (the pin field) and the reopen backoff.
- **No hot-path regression.** One strict-hold smoke A/B of a pin and unpin by key and by handle on the hottest key
  against item 5 (the unpin gains a branch); not a gate row.

## Composition with items 4 and 5

- **C14.** No new row posts a wake: `create` changes only the `failed` row and the open chore; the drain, the fence
  rows and the replace close end in `closing`, `opening` or `free`; a reopen in place opens hot (under `defer_io`
  the open maps every segment, D5's "cold to open"), so its waiters never meet `owed`. The gate stays the one decision
  at the `owed` row and the composed edges (`catalog.hh:727`, `:960-963`). I9 holds: a request pin is handed a pin
  only at an open's or a wake's end.
- **The wake's bound** (D5, I5-N2): it counts failed wakes other than `too_many_open`; a fenced wake now replaces
  the index and counts nothing, which is right (the bound exists for a wake that cannot succeed, and a fresh open
  can). The open backoff is unchanged for every error but the one `create` passes.
- **Item 4's fix round** (L1, C20: the bound close demotes when the slot has maps; the timer required) and item 5's
  close modes are what the replace close reuses; it is a fourth `close_mode`, and its timer retry needs I4-N1's
  required timer.
- **Shutdown** (C15): new pins fail with `catalog_closed` before any intent is read; a drain in progress at
  shutdown ends, reopens for its waiters, and their unpins `kick` the shutdown close, so it converges.

## Findings

**XH1. F1's create intent (amended, blocking for step 2).** As in "Rulings"; S10.

**XH2. F2 is a parallel registry (no strata change).** `StrataReaders` and its gate duplicate R4.1's index-owned
registry and its `too_many_readers` cap. X-POOL D1 and `reader-views.md` 2 change; Remove, not refuse.

**XH3. The catalog cannot host a payload that does not move (blocking; a catalog defect).** Probe
`pinned_payload.cc`: an H with a deleted move fails to compile at `catalog.hh:861` (`emplace` of
`make_payload`'s value) and `:1447` (`release_chunks` moves the payload into a leaked holder). Xapiand's
`IndexEntry` holds a `task::async_mutex` turn and atomics (`pool.h:213`, `:221`). Fix: F3's `open_payload`
builds H in place; `release_chunks` keeps a leaked slot's payload where it is (its chunk is already kept
forever) instead of moving it; S11.

**XH4. A fenced index never leaves the catalog (blocking; a catalog defect, not X-POOL's).** Probe
`fenced_stuck` (above). It also holds the slot's descriptors, vnodes and maps for the process's life and wastes a
pass post every round. Fix: F4's replacement in every chore; S12, S13.

**XH5. F4's restore case is not a reopen.** Restore needs `create` only (F1). Correct the X-POOL Gaps row.

**XH6. Idle indexes with finished merges stay hot (from the code; S14 settles it).** The catalog's chores never
check in, so `warm_quiescent()` stays false while a finished merge waits to be published. Fix: `check_in()`
before `chore_scope` in `run_demote` and `run_close` (at `{0, ...}` no host call holds the lease, D5).

**XH7. F6's FLUSH and diagnostics.** D8's FLUSH needs every open key; `snapshot()` plus `resident` pins serve it
without a host walk of test accessors.

**XH8. X-POOL text to change** (Xapiand's, for its designer): D1 (no `StrataReaders`, no gate), D2 (the adapter's
`fenced`/`retiring`/`epoch` and `mark_stale` go: `index_fenced` is `reopen(pin)`), D3 (`pin_chore` is `resident`),
D6 (resolve with `create`, `open_index`, then `open_payload` opens the blobs and replays), D8 (FLUSH by snapshot),
the housekeeping pass deleted (the turn's end checks in; autocommit re-arms while merges are pending), F4's row
(XH5). XP2 gains a fenced-index case: a pin after `reopen` reads the replayed documents.

## Blocking items / Required changes

- **L1 (XH1, XH3, XH4, XH6, XH7).** Item 5b as specified, with S10 to S16, C14's oracle and C17, before X-POOL's
  step 2 lands.
- **L2 (XH2, XH5, XH8).** X-POOL's design revised to the rulings before step 2's review.

## Conditions

- **C21 (item 5b).** `cold-tier.md` revision 8 carries the new rows and D5's fence paragraph; the lead's skim, no
  further architect review unless 5b's code departs from the table above.
- **C22 (item 5b's review).** The code review probes the drain: the acquire fence's pairing, a holder's unpin
  racing `reopen`'s exchange, and `finish_wake`'s holder count under the mutex; at high effort (concurrency).
- **C23 (X-POOL's gate).** XP2 and XP4 run with `defer_io` and a fault that fences, so the replacement and the
  replay are exercised end to end; the docs E2E on that build.
- Carried: C15, C16, C18, C19, C20 as in my item 3 and item 4 reviews; C17 moves into item 5b.

## Probes and artifacts

All in `strata-wt/cold-tier/.scratch/review-xpool/`, kept:
- `probe.sh SRC`: builds against item 5's headers and `test/` fixtures with item 4's review build's dependency
  headers and libraries (ASan+UBSan), into `bin/`. `syntax.sh SRC`: the same, `-fsyntax-only`.
- `probes.cc`, cases `fenced_stuck`, `absent_backoff`; output `probes-asan.out`; build log `build.log`.
- `pinned_payload.cc`, a non-movable payload; `pinned_payload.out` (the two errors).
- Rerun: `quiet light xpool-arch 30 "x-pool ruling" -- ./probe.sh probes.cc && bin/probes`.

Not run: TSan; the catalog's tests; CI on any platform; benchmarks; fuzzing; Xapiand builds.
