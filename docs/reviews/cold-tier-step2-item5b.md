# strata review: cold tier step 2, item 5b (the catalog's host interface for X-POOL)

- Date: 2026-11-03. The one review for this change (C22, high effort).
- Reviewed: `phase/cold-tier-step2-item5b` at strata:a12af34, against its merge base with item 5, strata:31da26e
  (`include/strata/core/catalog.hh`, `counted_io.hh`, `index.hh`; `test/catalog_host_test.cc`; the additions to
  `catalog_test.cc` and `catalog_stress_test.cc`; `docs/mechanisms/catalog.md`; `docs/design/limits.md`); the ledger's item 5b
  section (`docs/status/cold-tier-step2.md:911-1095`); the developer's mutant scripts and logs in
  `~/code/Kronuz/strata/.scratch/cold-tier-step2-item5b/`; main at strata:8bb648d (item 6's slot).
- Checked against: my ruling `docs/reviews/cold-tier-xpool-host.md` on `design/cold-tier` (F1 to F6, XH1 to XH8, S10 to S16,
  C14, C17, C21, C22); `cold-tier.md` revision 7 D4 and D5; `limits.md`; the C++20 memory model ([intro.races], release
  sequences).
- Checked by: Mac x86_64, Homebrew clang 23.1.2, separate TSan and ASan+UBSan builds with CI's flags
  (`.scratch/review-item5b/{tsan,asan}`): `catalog_host_test`, `catalog_sweep_test`, `catalog_wake_test`, `counted_io_test`,
  `catalog_test 300`, `catalog_stress_test 3000 4` and the probes, rc 0 and no sanitizer report in either build
  (`run-tsan.txt`, `run-asan.txt`). Linux x86_64 on the AWS lease (`tools/vm/aws-linux.sh`, c7a.8xlarge, Ubuntu clang
  18.1.3), TSan, built from `git archive a12af34`: the same suites with `catalog_test 1000` (172 s; 3,418 `reopen`, 3,285
  `drain.unpin`, 767 `replace.exchanged` hook sites taken) and `catalog_stress_test 8000 6`, plus the probes (P5b and P5c ran on the Mac
  only), rc 0, no report (`linux-tsan.out`). Ten probes and two mutant headers, all in `.scratch/review-item5b/` (listed at the end).
  - Not run: benchmarks (the by-key smoke is the lead's, on main); fuzzing (no decoder changed); arm64; FreeBSD; C14's
    oracle and the developer's 31 mutants (read from their logs, not rerun); CI (green on strata:0fa33ca per the ledger).

## Verdict

**APPROVE WITH CONDITIONS: item 5b builds what the ruling asked, and the drain, the fence replacement and the intents hold
under both sanitizers on two platforms. Two small defects go before it lands (a create storm the backoff was meant to stop,
and an ordering race in `snapshot()`), with no further review; the slot's budget must be settled at the merge with item 6.**

What holds, and reproduces:
- **The drain is correct, and the acquire is needed** (C22). `reopen` stores `drain` and exchanges under the shard mutex
  (`catalog.hh:591-622`); a holder's unpin that sees `closing` reloads the word with acquire before it decrements
  (`:1173-1178`). Every write of the word is a read-modify-write (no `word.store` anywhere), so the reload reads from the
  exchange's release sequence and the store of `drain` happens before the decrement. The window tests
  (`unpin_races_reopen_exchange`, `unpin_after_reopen_exchange`, `drain_unpins_race`, `wake_holder_unpins_after_exchange`)
  and the stress's drain phase pass under TSan on both platforms. Ruling on question 2 below.
- **A fenced wake counts its holders right** (C22). A waiter's pin and its place on the list are made in one critical
  section (`acquire_impl`, the `join` under `sh.mu`), and `replace_from` counts the list under the same mutex
  (`:1675-1704`); a holder's unpin between the count and the exchange fails the exchange and the loop recounts.
- **The create intent** (S10). A create pin joining while the non-create attempt is inside `open_index` (past `resolve`),
  and a read pin joining the reposted attempt, all get the one created index with one repost and no failure (probe P2,
  both platforms). An EIO negative entry gates a create pin (`create_obeys_other_errors`). One gap: F1 below.
- **H is built in place** (S11, XH3). The address H's constructor saw is the address the pin hands out, for an H with
  deleted copy and move (P3); `release_chunks` no longer moves the payload.
- **`index_fenced` means "replace" in every chore** (S13, XH4): the demotion, the wake, the bound close (tests) and the
  sweep's idle close of a fenced `owed` slot (P4: freed, one replace, no descriptor left). Shutdown does not replace, as
  ruled.
- **The drain converges under shutdown** (the ruling's "Composition"). A drain in progress when shutdown begins holds it
  back until the last holder unpins; the waiter that joined first gets the reopened index; a pin after shutdown gets
  `catalog_closed`; shutdown completes (P7).
- **`opening.counts` is the slot's** (S16, F5): `run_open` builds it from the slot's counts (`:1186`), and
  `host_files_counted` proves the counts, the hard cap before the kernel call, and the return to zero.
- **C17.** The guard follows the index through a replacement (the reopened index breaks on a call with no pin) and
  catches an `owed` slot with no pin (P6), beside `call_without_pin_breaks`.
- **S14's commit of a merge's output** (`commit_merge_output`, `:1502`) is the step S14 needed; ruling on question
  1 below.

What blocks landing: L1 (F1) and L2 (F2), each a few lines and a test; the lead confirms them. Nothing blocks X-POOL's
step 2 building against this branch.

## Rulings on the developer's questions

**Q1. Keep the commit of a merge's output (strata:f70fb2c).** Without it S14 cannot hold: the check-in publishes the
merged segment, `warm_quiescent()` refuses it while it is unsynced (`index.hh:4223-4226`), and nothing but a commit syncs
it. The host's commit chore is the right one to run: it is the only commit that keeps a host's log in step with the
index (Xapiand's WAL replay checks `revision == db_revision`), and the catalog already runs it at the bound close, at
shutdown and in the replace close. It runs only when nothing but merge output is uncommitted
(`index.hh:858-860`), and a failure waits for the slot's next pass, so it adds at most one commit per idle slot per
period. Conditions: C24 (the contract text) and C25 (X-POOL's commit chore).

**Q2. The acquire is needed; keep it, and record H8 as unkillable by test.** In the C++ model, a release `fetch_sub`
that reads the exchange's value does not synchronize with it. Without the acquire nothing orders `reopen`'s store of
`drain` before the holder's decrement, so the decrement may come first in `drain`'s modification order. The store then
overwrites it, the count never reaches zero, and the replace close is never posted: the slot hangs in `closing` with its
waiters. With the acquire, the reload reads from the exchange's release sequence (all of the word's writes are
read-modify-writes), so the store happens before the decrement and coherence forbids that order. No test can kill the
mutant: TSan models atomics as reading the latest value, and arm64 hardware most likely orders a release RMW before a
following acquire-release RMW anyway. The language model is what the code must satisfy, not the hardware. The
acquire costs one load on the drain path only; the common unpin pays nothing for it.

**Q3. Drop the check-in before a close.** H14b survives because it is unreachable, not because the test is weak. Every
idle close follows a demotion. A demotion takes only when `warm_quiescent()` holds, which means no pending merge. Any
write after it maps a segment, so the slot is hot again and demotes (with its check-in) before it can close. The probes
tried three routes to a close that meets a finished merge, and none reached one. A warm `open` slot written by a request pin
goes from 0 maps to 4 and demotes first (P5c). An `owed` slot cannot merge at all (P5b, below). The `eager_segments`
setting changes neither (P5, P5c). Shutdown's close drops a finished merge with or without a check-in, because `close()`
ends the merges and the output was never synced. Drop it, and state the invariant in the mechanism note's "Chores check
in first" bullet. This reverses the half of XH6 I wrote from the code alone; the demotion's check-in stays.

**Q4. The repack is acceptable on this branch; the slot's budget is not settled on main (F3, C26).** `wake_bound` at most
255 is load-bearing and stated (`limits.md`, the slot counts failures in a byte; the backoff saturates long before, at
2^10 × 10 ms). The free list in `last_use` is safe: a free slot is never read for its use (the sweep reads `last_use`
only for `{0, open/owed}`, and a stale read on a slot freed meanwhile ends at a compare-exchange that fails), and nothing
writes `last_use` between `free_slot` and `alloc_slot` but those two, under `free_mu_`. The silent clamp at
`catalog.hh:406` should be a refusal like the timer's (N3). The open problem is the merge: probe `slot_size` with item 6's
queue fields added (`q_prev`, `q_next`, `q`, `to_ghost`, as on main) measures **200 B on the Mac and 208 B on Linux**, so
`slot_cost`'s `static_assert(... <= 192)` fails on both once item 5 or 5b meets main.

**Q5. Not decided here.** The by-key path gains two intent compares and a dead `sets_create` store on `open`, and the
unpin gains one branch on a value it already holds. The 1.7 ns reading sits inside the A/A control's spread (52.6 to
57.5 ns). The lead's strict-hold A/B on main decides.

## Findings

**F1. A create pin passes the negative entry left by a create attempt: a storm (required, L1).** `finish_open` sets
`absent` for any `index_not_created` (`catalog.hh:1237`), including one from an attempt that ran *with* create, and a
create pin then passes the retry time (`:1040`). Probe P1: a host whose `open_index` does not create (a replica, a
read-only host, or a host bug); after one failed read, 10 create pins inside the backoff make 10 resolves and 10 opens,
each failing absent: no backoff at all, at 0.4 to 1.1 ms per request (`cold-tier.md` Limits). The ruling bounded a
missing index to one probe per retry time, and the bypass exists only to pass a non-create attempt's answer. Fix:
`s.absent = absent && !s.attempt_create;`, and a case in `create_passes_absent` (or its own) with a non-creating
`open_index`: one resolve for ten create pins inside the backoff. Mutant: today's line.

**F2. `snapshot()` reads a slot's key under the wrong mutex in a reuse window (required, L2).** `snapshot()` rechecks
`key_hash` before it loads the word (`catalog.hh:636-640`). Take a free slot whose stale `key_hash` still names this shard.
A key in another shard can claim it between the recheck and the word load. The snapshot then sees that owner's `opening`
or `open` and copies `s.key` holding the old shard's mutex, while the new owner's later `finish_close` clears the key under
its own shard's mutex (`:1850`). Nothing orders the two, so it is a data race on `std::string`. `shutdown_report()` has
the safe order (`:795-799`): load the word, then recheck `key_hash`; seeing the new owner's word (acquire) also shows its
`key_hash` store, sequenced before its exchange, so the slot is skipped. Fix: the same order in `snapshot()`. No probe
reaches the two-load window reliably; this is from the code, and the fix is the existing idiom.

**F3. The composed slot exceeds D4's 192 B (condition C26).** As in Q4: 200 B (Mac) and 208 B (Linux) with item 6's fields,
from 184 and 192. The static assert is the budget's load-bearing form, so the merge that brings item 5 onto main turns CI
red. Room exists without a new mechanism, for example a single waiter pointer (a circular list keeps O(1) append in FIFO
order: 8 B) and `retry_at` and `probe_at` as 32-bit offsets on the executor's clock like `last_open` (8 B). That is the
developer's to choose at the merge, measured on both standard libraries.

**F4. S14's test does not assert "the first pass after the merge" (minor).** `check_in_before_demote` allows up to 12
passes and asserts `passes >= 1` (`catalog_host_test.cc:738`); the cascade makes it 2 on both platforms (`linux-tsan.out`).
Assert the count the cascade gives (2), so a regression that needs more passes fails.

**F5. A resident pin that meets a failing open gets the open's error, not an empty pin (minor; text).** P8: a resident chore
pin that waits on `opening` of an absent key ends with `index_not_created` thrown. "Opening waits" is as ruled, but the
mechanism note's "never opens ... answers an empty pin for a key with no open or owed index" reads as if no error can
come out. Say that a resident pin waiting on `opening`, `waking` or `demoting` takes that transition's error; X-POOL's
autocommit treats it as nothing to commit (C25).

**F6. S11's destructor order is asserted for H only (minor).** `payload_in_place` proves H goes while the index's maps and
the IO's descriptor are held; the reader, index, IO order after H is unasserted (unchanged from item 3, `run_open`'s catch,
`:1199-1203`). A destructor log on a probe IO would close it; optional.

**F7. Text drift (minor).** The ledger's "What was built" says the last replacement's time is "32 bits of milliseconds";
the code keeps 16 bits of 2^27 ns (`:1840`; it wraps every 2.4 h, and a fence exactly a wrap later counts as a repeat once:
harmless, and worth one line). The ledger's "the AWS lease needs `aws login`" no longer holds: this review ran on it.

**Observation for X-POOL (not 5b's).** P5b: a chore pin on an `owed` `defer_io` slot that applies gets `map_owed`, which
leaves the index's `map_asked_` set until a later apply ends the turn; until then `warm_quiescent()` is false and every
idle close of the slot is taken and refused (16 of 16 passes). This is item 4's `defer_io` contract: a `map_owed` answer
asks the host to wake and retry. Under X-POOL only request pins apply, and they wake first, so it holds; C25 makes it
explicit.

## Blocking items / Required changes

- **L1 (F1).** `absent` only from an attempt without create; the test and its mutant.
- **L2 (F2).** `snapshot()` rechecks `key_hash` after the word load.
- **L3 (Q3).** Remove the check-in from `run_close` (`catalog.hh:1575`); the invariant in the mechanism note; H14b leaves the
  mutant list with that reason.

The lead confirms L1 to L3 from the diff and a green CI; no re-review.

## Conditions

- **C24 (Q1, text).** `host::commit`'s comment and `cold-tier.md` revision 8 D4: the catalog runs the host's commit chore
  at the bound close, shutdown, the replace close, and in a demotion or close when only a merge's output is unsynced
  (nothing applied), under the chore's lease with no pin held.
- **C25 (X-POOL).** Xapiand's commit chore with nothing applied writes no WAL revision of its own beyond what the index
  commit needs; its autocommit treats an empty resident pin and a resident pin's open error alike (F5); no chore pin applies
  on an `owed` slot (the observation above). For X-POOL's design revision, not this branch.
- **C26 (F3, Q4).** The merge that brings item 5 (and 5b) onto main keeps `slot_cost`'s 192 B on libstdc++ and libc++,
  with item 6's fields, measured by `slot_cost` on both. If it cannot without a mechanism change, the lead brings the
  layout back to me before D4's number moves.
- **N3 (Q4).** `catalog` refuses `wake_bound > 255` with `invalid_argument` instead of clamping silently.
- Carried: C15, C16, C18, C19, C20 as in my item 3 and item 4 reviews; C21 (revision 8's text) and C23 (X-POOL's gate)
  from the ruling. C17 and C22 close here.

## Probes and artifacts

All in `strata-wt/cold-tier-step2-item5b/.scratch/review-item5b/`, kept:
- `cfg.sh KIND`, `build.sh DIR TARGET...`: the TSan and ASan+UBSan builds (CI's flags, dependency sources from item 5's
  rel build, same pins); `runsan.sh tsan|asan`: the six suites and the probes, results `run-{tsan,asan}.txt` and
  `run-*-*.out`.
- `probe.sh KIND SRC [MUTDIR]`: compiles a probe like `catalog_host_test`, a mutant header directory ahead when given;
  binaries in `bin/`.
- `probes.cc` (includes `catalog_host_test.cc` with its `main` renamed): P1 `create_storm`, P2
  `create_joins_in_open_index`, P3 `payload_address`, P4 `fenced_owed_sweep_close`, P5 `close_meets_finished_merge`, P5b
  `owed_close_meets_finished_merge`, P5c `warm_open_close_meets_finished_merge` (with and without eager segments), P6
  `c17_after_replace`, P7 `drain_at_shutdown`, P8 `resident_meets_failing_open`. Outputs `probes-tsan.out`,
  `run-asan-probes-asan.out`, and the Linux run.
- `mut-h14b/` (no check-in before a close: the P5 routes still close, as Q3 says) with `probes-tsan-mut-h14b.out`;
  `mut-h8/` (no acquire; built for the record, not run: Q2); `mut-item6slot/` with `slot_size.cc` (184 to 200 B on the Mac,
  `slot.out`; 192 to 208 B on Linux, `linux-tsan.out`).
- `linux-tsan.sh`: `git archive` of the commit plus the probes onto the AWS lease, TSan build, suites and probes;
  output `linux-tsan.out`.
- `code.diff`: the reviewed diff of the three headers against strata:31da26e.
- Rerun: `./cfg.sh tsan && ./build.sh $PWD/tsan catalog_host_test ... && ./probe.sh tsan $PWD/probes.cc && ./runsan.sh tsan`
  under `quiet light`; `./linux-tsan.sh` for Linux.

Not run: benchmarks, fuzzing, arm64, FreeBSD, C14's oracle and the developer's mutant set (read from their logs), CI.
