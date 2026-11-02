# cold-tier

> Status: CURRENT (2026-11-02) the designer's ledger for `docs/design/cold-tier.md`.

- Design: `docs/design/cold-tier.md` revision 5 (hot, warm, cold and remote tiers; step 1's
  records, demotion and `map_owed`; step 2's catalog: slots and pins, the wake, counted IO and
  caps, S3-FIFO, the sweep).
- Review: `docs/reviews/cold-tier-design.md` (preliminary, APPROVE WITH CONDITIONS; re-checks 1,
  2 and 4); item 5's code review `docs/reviews/cold-tier-item5-code.md` on `main` (approve to
  land; I5-N2 and I5-N3 for step 2).
- Branch / worktree: `design/cold-tier` at `/Users/kronuz/code/Kronuz/strata-wt/cold-tier`;
  step 2's citations checked at main strata:081b7e7, Xapiand's at Xapiand:d5a2e746c.
- State: designing: revision 5, step 2, with the review of revision 4 (CT-R27 to CT-R39) folded
  in; the lead confirms the closures. Item 1 is being built (GO); its design text is unchanged.
- Step 1 on `main`: items 1 to 3 and D11's open check (strata:66ddb16; fixes strata:444c26f);
  CT-R5's rollback (strata:eb87572, strata:d3a59ee); item 5, `demote()`, `promote()`,
  `map_owed` (strata:081b7e7). K4 and item 6 remain.
- Done:
  - strata:4bc1a06 revision 0.
  - strata:8459c7b the architect's preliminary review.
  - strata:e6bdf25 revision 1 (CT-R1, R2, R3, R14, R16; N1; Q5).
  - strata:46ce154 the architect's re-check 1.
  - strata:e1bfb7f revision 2 (CT-R2's outputs, CT-R17 to CT-R20, C6).
  - strata:e44d98b the architect's re-check 2 at `c82ac9f` (C5, C6 closed; CT-R21).
  - strata:c49c4b2 revision 3 (CT-R21); strata:72a77f3 CT-R22, CT-R25, CT-R26 and K5.
  - strata:62f3eb1 revision 4: step 2. D4 to D6 rewritten; D3's read side (reads pin, the
    per-index unleased count); D7 and D10 (`close_ready()`, CT-R4); I4, I6, I7, I9, I10; T1 to
    T4 restated and T2d added; step 2's items 1 to 7 with S1 to S9; the table "Where each
    step-2 finding closes"; Q6. Text only, no build.
  - strata:e6e452c the architect's preliminary review of revision 4.
  - strata:26c180f revision 5: CT-R27 to CT-R39 in D3 to D6, I6, I7, I9, I10, T3 to T6 and the table; S1 to
    S9 moved here with CT-R38's fixes; Q6 ruled. Text only, no build.
- Next: the lead confirms CT-R27 to CT-R39 against the review. Items 2 to 7 start after their
  conditions (C9 to C13) are confirmed; item 1 is building.
- When the cold tier lands (owner, 2026-10-28): the hybrid-search go/no-go runs next. Rerun C1
  with strata included and 1,000-vector tenants, then whatever experiments show whether
  optimization can open a clear lead in idle cost and p99 over pgvector, Qdrant, LanceDB and
  Elasticsearch. The outcome is go (hybrid steps 1 to 4, `docs/design/hybrid-search.md` on
  `design/hybrid`) or dropping "best at AI retrieval" for good.
- Blocked on:
  - Item 6 (lazy as the default): K4's twelve cases, K7's two pin bumps, K3's rechecker run.
  - The lead's re-measured idle-index baselines (C2) before T1 to T4 and T2d gate.
  - Xapiand's adoption (X-POOL, C8) is Xapiand's change after step 2 lands.

## Done criteria for step 2

Each with a test and a mutant the test kills, built and run like step 1's (`.scratch/mutants.py`);
the item's ledger lists each rule with its options (`eager_segments`, `defer_io`, lease checks),
the mutant and its result (C7, C13). A test that cannot kill its mutant is not a test of the rule.
Design: `docs/design/cold-tier.md` revision 5, "Steps".

- **S1** (item 1). Index B's record mapped inside index A's chore counts on B; a reader's map
  counts; a map in `promote()` does not. Mutant: test the thread's lease count (today's code).
  Runs in a debug build and in a release build with lease checks off (`owner.hh:97`), where a
  second mutant, "count when `!held()`", dies (CT-R38.2; in debug builds it is equivalent).
- **S2** (item 1). With 5 files queued for reclaim, `close_ready()` is false; after `reclaim()`
  and its deletes, true; after `close()` the storage budget equals the bytes the open indexes hold
  on disk (`quiescence_probe` part 2 as the oracle). Mutant: drop the queue clause. With an applied,
  uncommitted write `close_ready()` is false; mutant: drop "nothing uncommitted" (oracle: a reopen
  still has the write, through the commit S7 makes first) (CT-R38.3).
- **S3** (item 2). Each cap driven to its hard value: the reservation that passes it throws
  `too_many_open`, no kernel error, nothing marked or fenced; a failed system call, an exception
  after a successful one (an allocation in the wrapper) and a lost install race return their
  reservations, and the kernel object goes too; counts equal the kernel's at quiescent points
  (T7); two catalogs on one `resource_budget` together never pass its hard caps; the descriptor
  cap is reached by opens alone (directory descriptors); with the soft limit set to 256 and a
  higher hard one, the budget raises it and caps from the raised value; a lowered `RLIMIT_NOFILE`
  is picked up at the next pass, and an `EMFILE` below the cap lowers the soft cap. Mutants:
  reserve after the system call (a barrier between two maps at the cap less one lets both pass);
  keep a lost race's reservation; release on error returns only; per-catalog caps; the directory
  descriptor unreserved; cap from `kern.maxfilesperproc` without the raise; caps fixed at
  construction (CT-R27, CT-R32, CT-R37, CT-R38.4).
- **S4** (item 3). T6's schedules over every writer of the word (D4's table), with slot reuse, a
  reopen in place, and a step between each load and its compare-exchange; a pin on a transition
  suspends and gets its pin from the transition's store; `try_pin` starts the open or wake and
  answers `would_wait`; a re-pin by handle across a reopen in place falls back to the key; a failed
  open fails pins at once until its retry time, then reopens; a second key on a held directory
  fails its open once. Mutants: time stored after the decrement (TSan and the schedule); a state
  stored or waiters collected outside the shard mutex (a parked waiter never resumed); waiters
  resumed under the mutex (deadlock on `inline_executor`); waiters re-pin after the store
  (starved by the sweep, `slot_model` A1); a plain store of the word (a pin lost); the sweep
  reading fields before its compare-exchange; a re-pin by handle that skips the `gen` check; no
  `gen` bump on a reopen in place (`slot_model` B1); no open backoff (an open per pin); a lock
  conflict retried (CT-R29, CT-R30, CT-R34, CT-R35).
- **S5** (item 4). Under `defer_io`, indexes demoted between pins, and T6 schedules that put the
  demotion between a pinner's load and its compare-exchange: no request pin is handed out before
  `promote()` completes, `unleased_maps()` stays zero, no `apply` answers `map_owed`; a chore pin
  on `owed` commits without a wake, and a wake behind its call reposts. Mutants: hand out the pin
  before the wake ends; pin `owed` as `open` (CT-R28, CT-R38.5); a chore pin that waits for the
  wake; a wake that waits on the lease instead of reposting.
- **S6** (item 4). Persistent faultfs `EMFILE` in `promote()`, on the host's timer injected so the
  backoff runs in milliseconds: request pins in backoff fail with `wake_failed` and post no
  chore; chore pins still enter; at the 8th failure a committed slot closes and the next pin
  reports the reopen's error; an uncommitted slot at the bound is committed by the host's commit
  chore, then closed, and its data reopens; a commit that fails leaves it `owed` with a 10 s probe;
  `too_many_open` does not count. Mutants: no backoff (a chore per pin); no close at the bound;
  close an uncommitted slot at the bound without the commit (the reopen lacks the write)
  (CT-R33, CT-R38.6).
- **S7** (item 5). Demotion only from `{0, open}`; cold only from warm with a maps count of zero
  and `close_ready()`; a queued reclaim posts and closes on a later pass; `reclaim(0)` runs under
  `demoting`; a view kept past its pin keeps the slot `closing` without blocking a thread; a
  crossing posts one coalesced pass that reposts while over the soft cap; `shutdown()` commits
  the uncommitted, closes every slot, names the failed commits, and `H` is destroyed after its
  close chore returns. Mutants: close from hot; close with a retired view; demote a pinned slot;
  close with a queued reclaim; `reclaim(0)` under `open`; one pass per crossing without the
  repost; `H` destroyed inside the close chore.
- **S8** (item 6). The catalog's policy code over a recorded Zipf trace with stub indexes, each
  request taking two pins (a use by key, a re-pin by handle), is within 0.5 point of
  `s8_variants`' reference at θ = 0.99 and 1.2, overall and over the 1M requests after a one-time
  scan of every key. Mutants: the pin does not set the frequency; the ghost ignored; every pin a
  use (0.7925 and 0.759 at θ = 0.99 in the model) (CT-R31, CT-R38.7).
- **S9** (item 7). T5's step-2 run under TSan, ASan and UBSan with its counters, and CI green on
  every platform.

## Done criteria for step 1

Each with its evidence on the branch, re-checked before landing.

- **K1.** Record and body, compare-exchange install, a loser that destroys only its own body;
  T6 over the install under TSan, with the mutants "body cleared in place", "body stored without
  compare-exchange", "loser reads the body it destroyed" and "facts written by a reader's map"
  (CT-R17).
- **K2.** D9's table in code, each map point's failure rule with a faultfs-injected test (open,
  `mmap` and check failures): no fence for host errors, damaged-and-skipped merge sources,
  commit's prepare leaving the index as `refresh()` leaves it and unfenced. Flush and merge output
  maps under faultfs `map()` failures keep the bytes and never fence (`output_map_probe` as the
  oracle). A damaged segment's first map inside commit's prepare: `corrupt_segment`, unfenced,
  `needs_heal()` true, a commit after `heal()` succeeds (CT-R19). An `EMFILE` at scheduling:
  `refresh()` succeeds, the swallowed-round count rises, the next round merges (CT-R20).
- **K3.** CT-R5's rollback on `main`, `apply_throw_probe` as its test, failing before and passing
  after. Satisfied in code (strata:eb87572, strata:d3a59ee; `apply()` `index.hh:697`, `rollback()`,
  `reserve_undo`, `test/apply_atomic_test.cc`'s allocation sweep); still owed: a rechecker with a
  build runs the probe, the sweep and the revert check on `main` (no re-check of CT-R5a's fix is on
  file).
- **K4.** The twelve damage cases with lazy segments; a missing or resized manifest-named file
  fails the open (D11).
- **K5.** Item 5: `demote()`, `promote()` and `map_owed` (D3, D10, I5, I8; CT-R22, CT-R25, CT-R26).
  Each rule has a test and a mutant the test kills, built and run like step 1's
  (`.scratch/mutants.py`), and the ledger lists each with its options (`eager_segments`, `defer_io`),
  the mutant and its result (C7). A test that cannot kill its mutant is not a test of the rule.
  1. Warm's predicate: `demote()` returns false and changes nothing (the same record pointers,
     view, digest, owed flag and owed bit) with a buffered record, a pending merge, or an unsynced
     record. Mutants: drop each clause (three). "Not closing" is not a clause: `close()` fences, so
     `demote()` and `promote()` after `close()` throw `index_fenced` from `live()`; mutant: skip
     `live()`. Under `defer_io`, an `apply` after a refused `demote()` is accepted with `calls()`
     unchanged; mutant: set the flag before the predicate (CT-R25.3, .4).
  2. Every record twinned: with `eager_segments = false`, so that at least one record has no body
     when `demote()` starts (asserted), and with a reader that held the old view and maps late
     after `demote()` and `free_views()`, no record in `segs_` has a body (the `twin_race` shape,
     T6). Mutant: twin only bodied records. Under eager segments the mutant is equivalent, so the
     test does not run there (CT-R25.1).
  3. The twin's fields: mutants "twin drops `deletes_dirty`" (oracle: commit, reopen, the delete
     holds; CT-R18), "twin drops the marker" (`needs_heal()` false after `demote()`), "twin drops
     the bits" (a deleted document readable by `get`).
  4. Forced publication and freeing: after the last guard releases, the index holds 0 maps, counted
     as every mapping the index keeps, retired views included (the wrapper's live mappings), not
     over `segs_`, where the twins are already unmapped. Mutants: skip the forced publication
     (`publish_point()` keeps the old view); skip `free_views()` (CT-R25.5).
  5. Invisible to reads (I5): position and digest equal before `demote()`, after it, and after
     `promote()`, with an uncommitted delete. Mutant: build the twin's bits from the live-docs file.
  6. `map_owed` (CT-R21, I8): on a `defer_io` index, `demote()` then an `apply` that replaces by key,
     and one that deletes by docid, answer `map_owed`, apply nothing, and leave
     `faultfs::filesystem::calls()` unchanged; `promote()`, then the same transaction at the same
     position is accepted with `calls()` unchanged. Mutants: drop the check in `apply` (calls rise;
     the debug build's `no_input_map_scope` assertion fires); `demote()` never sets the flag
     (same). Without `defer_io`, `demote()` then a replace-by-key apply is accepted and maps
     (`calls()` rises); mutant: answer `map_owed` without `defer_io` (CT-R25.2; the default-path
     test never calls `demote()`, so it cannot kill it). `default_path_unchanged` still runs.
  7. The answer ends: after a completed `promote()`, no `apply` answers `map_owed`. Mutant:
     `promote()` leaves the flag set. A `demote()` between `promote()` and the retry returns false
     and the retry is accepted; mutant: drop the owed bit (CT-R22). A faultfs `map()` failure
     (`ENOMEM`) inside `promote()` rethrows unchanged, unfenced, with the flag set and the bodies
     mapped so far kept; the next `apply` answers `map_owed` and a second `promote()` completes.
     Mutant: `promote()` clears the flag on a host error (the next `apply` maps: calls rise). A
     record whose first map fails its checks gets the marker, `promote()` completes, and an `apply`
     that reaches it throws `corrupt_segment`, rolled back, with `calls()` unchanged. A twin that
     carries the marker from before `demote()`: `promote()` completes and clears the flag; mutant:
     `promote()` rethrows the `corrupt_segment` of a marked record (CT-R25.7). `promote()` with the
     flag clear makes no `calls()` (CT-R26).
  8. T6's mutant "close with a posted delete" (D10's cold predicate, carried for step 2's sweep).
  9. T5 on one index with readers holding guards across `demote()` and `promote()` and releasing on
     other threads, an index idle after posting deletes, and the same run under `defer_io` answering
     `map_owed` by `promote()`; zero ASan, UBSan and TSan reports, and counters that show the
     interleavings ran: late maps through a retired view, install compare-exchange losses, and a
     `promote()` racing a reader's map, each at least 1 (CT-R25.6).
  10. The turn's bound (CT-R22): a host loop that answers each owed answer by its chore and retries,
     with an adversary that calls `demote()` at every opportunity, sees at most one of each owed
     answer and at most three retries per transaction.
  11. Reads after `promote()` (CT-R23): a `get` and a scan make no `calls()` and `unleased_maps()`
     does not rise; before `promote()` a lease-less read raises it. The flag is
     `promote_owed()`, an `std::atomic<bool>`; a debug build asserts, at `apply`'s check, that with
     the flag clear every record is bodied or marked; mutant: `promote()` clears the flag with a
     record unmapped (the assertion fires) (CT-R26).
- **K6.** T1's step-1 rows: hot within 5% of today; demoted with its own target (N1); 1 and 10
  segments.
- **K7.** prospect's construction sites and Xapiand's seam listed, with the pin bump that adopts
  `seg()`.
- **K8.** CI green on every platform, ASan, UBSan and TSan.

## Log

- 2026-10-27: revision 0 drafted. Decisions D1 to D8, invariants I1 to I7, gate rows T1 to T7,
  questions Q1 to Q4. No build or benchmark run (design only).
- 2026-10-27: revision 1 after the preliminary review: D9 to D11, D3 twins every record, N1,
  K1 to K8, Q5; Q1 to Q4 marked ruled.
- 2026-10-27: revision 2 after re-check 1: C5's text (CT-R2 outputs, CT-R17 to CT-R20, the
  marker's lifetime, the prepare's wording, Q5) and C6's tests in K1, K2, K5.
- 2026-10-30: revision 3 after re-check 4 (CT-R22, CT-R25, CT-R26 folded into D3, D10, I8, T5, T6, item 5
  and K5, with C7's listing). One refinement of CT-R22's fix: the owed bit is cleared by an `apply`
  that ends the turn (any answer but the three owed ones, or a throw) and by `close()`, not by
  `refresh()` or `commit()`, because `refresh()` is `flush_owed`'s chore and a clear there lets a
  `demote()` in before the retry, which answers a second `map_owed` (the bound would be five
  retries, not three). The architect's model variant 3 ("refuse `demote()` while an answer is
  outstanding") is this. Text only.
- 2026-10-30: revision 3 after re-check 2 at `c82ac9f`: CT-R21 decided (warm admits `defer_io`;
  `promote()` and `map_owed`, with step 2's catalog promoting at the wake), citations refreshed, K3
  and K5 restated. No owner question. No build or benchmark (design only).
- 2026-11-02: revision 4, step 2. Every step-2 finding decided in D3 to D6 and D10 (CT-R4, R6,
  R8, R10 to R13, R15, R23, R24; I5-N2, I5-N3; X-POOL). CT-R23's backstop: reads pin, and the pin
  is handed out only after the wake's `promote()`; the sentinel stays step 3, and refusing
  `demote()` under `defer_io` is rejected. Q6 raised. Citations at strata:081b7e7 and
  Xapiand:d5a2e746c. Text only, no build or benchmark.
- 2026-11-02: revision 5 after the preliminary review of revision 4: one process-wide resource
  budget (CT-R27), the owed state in the slot word (CT-R28), the word's protocol with waiters'
  pins (CT-R29), `gen` on every close (CT-R30), uses (CT-R31), the directory descriptor reserved
  (CT-R32), chore pins and commit-then-close at the bound, `shutdown()` (CT-R33), open backoff
  (CT-R34), resumption outside the mutex (CT-R35), named caps and coalesced passes (CT-R36),
  moving limits (CT-R37), S1 to S9 here (CT-R38), CT-R39. S1 and S2 gain test-only mutants
  (CT-R38.2, .3); item 1's design text is unchanged. Q6 ruled (a).
