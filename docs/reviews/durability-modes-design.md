# strata review: durability modes, part A (preliminary)

- Date: 2026-10-19, read from 23:30 on 2026-10-18 to 01:40 PDT.
- Reviewed: `docs/design/durability-modes.md` (Part A, D1 to D16, the threat model, the fault
  model, T1 to T14, Q1 to Q7, first steps; Part B's outline) and `docs/status/durability-modes.md`
  (the owner's scope, the corrections, the lead's notes) at `8f8b043` on `design/durability-modes`,
  equal to `origin`; the probes `bench/durability/` and the raw files
  `results/2026-10-18-t1-macos.txt`, `results/2026-10-18-t2-macos.txt`, and `results/arith.out`
  at `0b407c9`.
- Checked against:
  - `docs/REPLICATION.md`, `docs/design/track-c.md`, `docs/ARCHITECTURE.md` (write path),
    `docs/PLAN.md` (Phase 3R, "Background work is measured under load"), `docs/DISTRIBUTION.md`,
    and `docs/reviews/system-2026-10-17.md` (SYS-8) at `8f8b043`, whose `main` content is `734949e`.
  - `design/peer-transport` at `38a6c6f` (D5, D6, D7, D9, D11, "Library changes", "For Detent").
  - `design/simulator` at `27cfb30` (SIM-R2, D5, D6, D9); `perf/small-indexes` at `7c23560` and its
    review (SI-R6 to SI-R11); `research/no-stalls` (I1, I4, I5).
  - `phase4/format13` at `0954761`: the ledger's freeze-readiness list (`format13.md:107-117`)
    and the fourth re-check's freeze table and F13-R40 to F13-R43 (`format-13-design.md`),
    written four minutes after this design's commit.
  - Code: `consensus` `61149a2` (`core.h`, `types.h`, `storage.h`, `checkpoint.h`,
    `retained_state.h`, `transport/codec.h`); `journal` `cb66f3d` (`turn_log.h`, `journal.h`),
    which `consensus` pins; strata `8f8b043` (`index.hh`, `reader.hh`, `bench/durability/`);
    `Kronuz/sha256` `2bb34f0`; Xapiand's MIT own code at `7e8cccb7d` (two spot checks below).
  - Detent `310e59c`, read only: `SHARED_LIBRARY_DECISIONS.md`, `LIBRARY_BOUNDARIES.md`.
  - `Kronuz/cpp-libraries` `docs/research/foundations-vision.md` at `8814481`.
  - `docs/reviews/calibration.md` at `8f8b043` (equal to `main`): all ten rows, applied below.
- Checked by:
  - Reading every file:line the design cites at the commit its header names. All match but the
    ones the findings name.
  - `quiet check`: exit 1 at 23:36 (`scale-threads-mac2`, strict), so nothing compiled or ran
    until it ended; exit 2 (`nostalls-core-r3`, light) from 23:47, so every run below used
    `nice -n 19` on one thread.
  - Probes in `strata-wt/review-durability/.scratch/review-durability/`, each with its `.out`:
    - `forgetful_model.py`: an explicit-state model of D2, D5, D6, and D7 at 3 voters (writes,
      replication with Raft's conflict rule, both commit cursors, barriers, OS crashes that keep
      only the synced prefix, re-learning, D6's three grant rules, and its timeout as an event).
      At every election it checks that the new leader holds every write acknowledged at `memory`
      and at `quorum`. `model-d10.out`: depth 10, one write, terms up to 2; `model-d12.out`: two
      writes, terms up to 3, to 6 million states. Atomic messages, so a pass is bounded evidence
      and a counterexample is real.
    - `core_probe.cc` against `consensus` `61149a2` with Homebrew clang 23.1.2 under ASan and
      UBSan (`core_probe.out`, no report): a restarted voter's grants, and the leader's handling
      of a second reply to one append.
    - `rescore_t2.py` (`rescore_t2.out`): T2's medians and ratios from the raw file, then the
      figures the estimate leaves out, under assumptions it labels.
  - T1's raw file against the design's table: create is 6 durability calls (6 `F_FULLFSYNC`,
    4 `fsync`), each commit 4 (4 and 3), 14 per index over create and two commits, 140 over 10
    indexes. All as the design states.
  - `HEAD` against `origin` for every repository read (calibration row 5): equal for the strata
    branches, `consensus`, `journal` (its untracked build directories aside), `sha256`,
    `cpp-libraries`, and Detent; `research/no-stalls` moved to `a9448fd` during the review (the
    design cites §5 at `b9dddfd`); Xapiand's local `master` is 26 commits ahead of its upstream and
    was read at `7e8cccb7d`, the design's commit.
  - Not run: the last line of this review.

Citations: `DM:N` is `docs/design/durability-modes.md` at `8f8b043`; `TC:N` is `track-c.md` there;
`PT:N` is `peer-transport.md` at `38a6c6f`. `core.h`, `storage.h`, `checkpoint.h`, and `codec.h`
are `consensus` `61149a2`; `turn_log.h` and `journal.h` are `journal` `cb66f3d`; `index.hh` and
`sizing.cc` are strata `8f8b043`.

## Verdict

**REJECT for first steps 1, 3, 4, and 5; steps 2, 6, and 7 may start. D5 holds; D6's timeout
loses `memory` writes inside the model; and the journal format, the core, and the wire would
freeze without four mechanisms they need (DM-R2, DM-R3, DM-R4, DM-R6).**

What holds, and reproduces:

- **D5 alone holds in the model.** With forgetful voters kept out of elections until they
  re-learn, the model finds no lost `memory` or `quorum` acknowledgement in 10,696,714 states (two
  writes, terms up to 3, depth 11), nor with only D6's every-voter-heard grants in 6,586,795
  (`model-d12.out`).
  The argument at DM:261-271 is the right one: majorities intersect in a voter that holds the
  entry or re-learned it, and terms survive restarts (D4).
- **D6's `quorum` floor holds in the model.** Beyond the model (every voter forgetful), neither
  grant rule produced a `quorum` loss at depth 10 (2,305,718 and 5,194,973 states), as the
  intersection argument at DM:330-332 predicts; a `heard` recovery loses only a write no voter
  held, as DM:333-336 says.
- **The rewind is needed, as the design found.** A leader never moves `next` below
  `matched + 1` (`core.h:967-969`) and never lowers `matched` (`core.h:945, 957-958`), so a voter
  that lost acknowledged entries would be resent nothing it lacks.
- **The promotion's five checks** against `Repair` are each right: the transfer refusal
  (`core.h:600`), the base bound (`:602`) against a checkpoint bounded by `applied_` (`:562`),
  the busy and delivery gate (`:599`), the epoch (`:606`), and skipping applied transactions by
  position (`index.hh:416-427`).
- **The measurements reproduce.** T1's counts as above; T2's medians and every derived figure
  (0.370× and 0.350× at rf = 3, 0.240× and 0.218× at rf = 5, 1.81 and 2.61 µs per follower,
  23,958 and 7,798 documents of replay) recompute from the raw file (`rescore_t2.out`).
- **The wire arithmetic** matches the peer transport's D9: 56 → 64 (PT:612), 104 → 136 and
  `control_frame_bound` 104 + 32·e + b → 136 + 32·e + b (PT:602-605), 96 → 112 and 112 → 128
  (PT:614), coalesced records 48 → 64 (PT:618). At 1,000 groups per node, a third led at rf = 3,
  the 16 bytes per record cost about 107 KB/s per node (6,660 records a second, arithmetic).
- **The Xapiand corrections** spot-check: a failed asynchronous sync is logged and dropped
  (`src/xapiand_fsyncher.h:39-47`), and the debouncer defaults are `opts.cc:114-136`'s. The new
  record kind `0x12` is free (`turn_log.h:87`), and `kern.bootsessionuuid` exists on the
  development Mac.
- **D11 and D13** are sound rejections, and the vocabulary (D10) is the right shape.

What blocks:

- DM-R1: D6's `recovery_timeout` lets a forgetful voter grant and campaign inside the model.
- DM-R2: a same-boot restart after a failed barrier, and on the per-group backend after any
  process crash, keeps a voter electable over a tail it lost or never synced.
- DM-R3: with apply at `commit_memory`, no strata checkpoint can satisfy D1, and one that
  violates it makes D6's repair diverge silently.
- DM-R4: `synced` has no path back to the leader, and a quiescent group never advances
  `commit_durable`.
- DM-R5: the core reads `durable_` at 34 sites; the design assigns a cursor to some, and
  membership changes, promotions, and witness truncation need the durable one.
- DM-R6: the turn log supersedes and reclaims at completion, which D3 moves from the barrier to
  the write.
- DM-R7: three changes reach Detent's groups without an opt-in.
- DM-R8: segment files get a second byte mover and a second flow control beside `RFTXFR01`.
- DM-R9: the version-2 fields are incomplete and freeze before the semantics are reviewed.
- DM-R15: Part B can reopen D1, D5, and the wire after they freeze.

## What freezes

| Element | Where | State | Findings |
|---|---|---|---|
| `RFTW0002`: the end-of-turn synced turn, the open record's boot identity, the clean-close record, recovery after S | D3, DM:195-207 | Changes required: same-boot recovery, supersession at the barrier, activation, S's definition | DM-R2, DM-R6, DM-R18 |
| The per-group journal's boot identity and clean-close marker | D3, DM:210-216 | Incomplete: its recovery stops at the frontier; Detent's backend | DM-R2, DM-R7 |
| Version 2 bodies of kinds 2, 3, 4, 9, 10 | D16, DM:628-631 | Sizes sound; the set incomplete | DM-R4, DM-R5, DM-R9 |
| Kinds 6 and 7 "unchanged" | DM:628 | Wrong: kind 6 needs `forgetful` | DM-R1 |
| Host kinds 256 to 271 and 512 to 527 for the segment fetch | DM:632 | A second mover; prefer `RFTXFR01` or a group-level kind before version 2 | DM-R8 |
| `Membership` + optional `acks` | DM:633 | Sound; its rule needs one statement | DM-R18 |
| `EntryKind::Recovery` | DM:634 | Storage and checkpoint readers call it corruption, not unsupported | DM-R7 |
| `commit_durable` in hard state | Absent | A row owed, or the "only grows" claim withdrawn | DM-R5 |
| Format 13: segment identity, per-version HLC, time bounds and tier mark | DM:404-408, 559-560, 639, 839-840 | Identity per F13-R41; the HLC's place undecided; F13-R43's attribute list not asked for | DM-R15, DM-R16 |

## Rulings on D1 to D16

| Decision | Ruling | Changes |
|---|---|---|
| D1 Commit in three steps | Sound with changes | Checkpoints of a past generation (DM-R3); supersession at the barrier (DM-R6) |
| D2 `memory` | Sound with changes | The window's cap and one model statement (DM-R1, DM-R13) |
| D3 The bounded barrier | Sound with changes | Same-boot recovery, the per-group backend, supersession, S (DM-R2, DM-R6, DM-R18) |
| D4 Hard state first | Sound | None |
| D5 Forgetful voters | Sound as an election rule (`model-d12.out`) | Failed barriers and same-boot restarts (DM-R2); the cursors (DM-R5); opt-in for Detent (DM-R7); the medium (DM-R15) |
| D6 Recovery after a correlated loss | Wrong as written; its `quorum` floor holds | DM-R1 |
| D7 Two cursors | Sound with changes | Checkpoints (DM-R3), the `synced` path (DM-R4), the site table (DM-R5) |
| D8 Segment mode | Sound with changes | One mover (DM-R8), a load-bearing bound (DM-R10), residuals (DM-R11), the estimate (DM-R12), identity (DM-R16) |
| D9 The failover gap | Sound with changes | DM-R10, DM-R14, DM-R17 |
| D10 One vocabulary | Sound with changes | Exact windows and read bounds (DM-R13) |
| D11 No multicast | Sound | None |
| D12 Partition-tolerant writes | Sound as a deferred shape | The HLC's place in format 13, before it freezes (DM-R16) |
| D13 No leaderless replication | Sound | None |
| D14 No stalls | Sound with changes | The memory window's cap (DM-R13); copy-forward's row (DM-R18) |
| D15 Live tuning | Sound with changes | The mode switch against D9's bound (DM-R10) |
| D16 Formats and wire | Changes required | DM-R5, DM-R7, DM-R8, DM-R9, DM-R15, DM-R16 |

## Findings

### Safety

1. **DM-R1. D6's timeout makes a forgetful voter grant and campaign inside the model** (D2, D6,
   T3, T5, the fault model).
   - `model-d10.out`, mode `timeout`, at most one voter forgetful at once, 7 steps: A leads term 1
     and writes v0; B writes it; A acknowledges at `memory`; A loses power before its barrier and
     restarts forgetful with nothing; B is unreachable (a partition, or its process down) until
     A's `recovery_timeout` passes with no leader; A's own log is as up to date as every log it
     has heard, so A campaigns (DM:304-307) and C grants; A leads term 2 without v0, writes its
     `Recovery` entry, and B truncates v0 at the heal (by Raft's rule, or by DM:321-324 if B
     learned the commit). A second trace has the follower lose power instead: it campaigns after
     its timeout, wins with C's grant, and asks A to replace an entry at or below A's commit,
     which today's core refuses (`core.h:868-871`) and D6's rule accepts. Modes `none` and
     `heard` find nothing at the same bound.
   - D6 says as much: after the timeout the winner holds only "every committed entry the
     reachable voters hold" (DM:307-308). The trace shows the timeout firing on one OS crash and
     one partition: inside D2 ("the OS crash or power loss of up to f of 2f + 1 voters",
     DM:172-174), T3 (DM:738), the simulator change ("a forgetful voter counts against f until it
     re-learns", DM:700), and T5 (DM:740). The fault table says `replicated` loses nothing there
     (DM:661), and Q3 recommends exactly this policy (DM:775-777). D6 is introduced for "more
     than f voters ... forgetful at once" (DM:295), but a voter cannot tell that from "f
     forgetful and the rest unreachable" with the signals D16 gives it: kinds 1, 6, and 7 stay
     unchanged (DM:628).
   - The four model statements also differ. D2 says "at once", T3 "in any `sync_interval` plus a
     barrier", the simulator and T5 "forgetful at once". The windowed one admits two forgetful
     voters at once (a slow re-learner, then a second power loss minutes later), which is outside
     D5's argument (DM:261).
   - Fix:
     1. Kind 6 carries `forgetful`. A forgetful voter applies D6's conditions only once it has
        heard, since its restart, f + 1 distinct forgetful voters of the membership in force,
        itself included. Inside the model no voter reaches f + 1, so none grants or campaigns
        before it re-learns, and `aggressive` loses only beyond the model too.
     2. The fault table states the availability this costs: at rf = 3, one forgetful voter and
        one unreachable voter stop elections until either returns, where `strict` would elect.
     3. One model statement, "at most f voters lose their OS or power or are forgetful at once,
        a forgetful voter counting until it re-learns", in D2, the summary, T3, T5, and the
        simulator change.
     4. T3 gains this trace as a forced schedule; T6 gains the mutant "D6 without the f + 1
        count", which the model above kills.

2. **DM-R2. A same-boot restart can keep a voter electable over a tail it lost or never synced**
   (D3, D5, T7, T9).
   - D5 makes a voter forgetful "after any failed barrier ... and never retries the sync as
     success" (DM:241-242), but a failed barrier fails the turn log (`turn_log.h:270-272`,
     `330-331`) and nothing durable records it. The restart sees the same boot identity and no
     clean close, which DM:234 does not call unclean, so the same-boot rule applies: "Recovery
     syncs each segment holding turns after S ... and the voter resumes, not forgetful"
     (DM:238-240). That retry is what SI-R8 rules out: "a reopen in the same boot reads it from
     the page cache ... After a failed `fsync`, Linux may report success to a retry over lost
     data" (`small-indexes-design.md` at `7c23560`).
   - Two outcomes, both inside the model. If the kernel dropped the pages, recovery keeps a
     shorter prefix and the voter, not forgetful, can elect a leader without a `memory` write
     after one more voter's power loss. If it kept them clean, the voter reports `synced` over a
     tail that is not on disk, `commit_durable` counts it, and a `quorum` write rests on one
     durable copy, which a disk loss removes.
   - The per-group backend is worse. Its recovery replays only to the manifest's frontier
     (`journal.h:333-358`); D3 says the variant "needs no format change" (DM:214). Every batch
     acknowledged from memory past the frontier is dropped by a plain process crash, which D2
     says the guarantee survives in any number, and the voter stays electable.
   - Fix:
     1. Same-boot recovery rewrites every turn after S from the bytes it read, then syncs, so a
        page left clean after a failed writeback is written again; a torn turn after S other than
        the last means pages were lost, and the voter recovers forgetful.
     2. A voter reports `synced` only for turns this process synced.
     3. On the per-group backend, same-boot recovery replays framed batches past the frontier to
        the first invalid one, rewrites and syncs them, and publishes the frontier; or that
        backend refuses `acks: written`.
     4. T9 rows: a failed barrier with the pages kept clean and with them dropped, each followed
        by a same-boot reopen; a process crash with batches past the per-group frontier.

3. **DM-R3. Apply at `commit_memory` leaves no checkpoint D1 allows** (D1, D6, D7).
   - D1: no checkpoint records an applied position above `commit_durable` (DM:155-158). D7: apply
     follows `commit_memory` (DM:357). strata's apply sets the index's applied position
     (`index.hh:456`), and `commit()` "makes everything applied durable" (`index.hh:519-527`), so
     a checkpoint always records the current position. Under sustained ingest
     `applied ≥ commit_memory > commit_durable` at every instant, and today's gate refuses the
     capture once apply runs ahead of the persisted commit index (`core.h:562`:
     `applied_ > durable_.commit_index`), which D4 lets lag.
   - A checkpoint taken at the current position breaks D6. Repair restores it with lost writes
     inside, and the redelivered replacements at those positions are skipped as already applied
     (`index.hh:424`): the replica diverges silently, against "Replicas never diverge"
     (DM:328), and T4's digest would catch it only after the fact.
   - Fix: strata checkpoints the newest refresh generation whose position is at or below
     `commit_durable`, the same generation positions an `Image` names (DM:383-391), keeping that
     generation's files, merge inputs included, until a later checkpoint covers them; the core's
     gate compares the capture's `through` with the durable cursor; D1 states the retained disk
     (one checkpoint interval of superseded files); T4 runs a D6 recovery after checkpoints
     taken under sustained ingest. Pausing apply until `commit_durable` catches up is the
     rejected alternative: it stalls every verdict for up to `sync_interval` and a round trip at
     each checkpoint, which D14 forbids.

4. **DM-R4. `synced` has no path back to the leader** (D7, D16, Track C's D12).
   - D7 sends the append reply at `Written` with `matched` as the written index and `synced`
     beside it (DM:366-374). A later barrier has nothing to ride: the leader drops a reply whose
     RPC has no flight (`core.h:939-940`) and one whose `matched` differs from the flight's end
     (`core.h:944`); `core_probe.out` shows both. `sync_through` forces the barrier
     (DM:515-518) but not the report.
   - Quiescence needs only "every member ... matched its last index and commit" (TC:635-637).
     A group quiesces with `commit_durable` behind; with no heartbeats, `commit_durable` never
     advances, its checkpoints (capped there, D1) stop, and its log never truncates. A `quorum`
     write whose entry commits at `commit_memory` can see its group quiesce before
     `commit_durable` reaches it, and its acknowledgement then waits forever.
   - Fix: a progress report the core accepts without a flight, keyed by term and incarnation,
     sent when a barrier moves `synced` past the leader's last `sync_through` and at most once a
     heartbeat otherwise (or a reply to an append that carries `sync_through` waits for the
     barrier; state which); quiescence requires `commit_durable` at the last index and known to
     every member; the `quorum` latency in a `replicated` index stated against `strict`'s (two
     barriers and a round trip, plus at most one heartbeat); T10 measures that latency, and T14
     checks that an idle group's `commit_durable` reaches its last index before it quiesces.

5. **DM-R5. Two cursors over 34 sites, and three of them need the durable one** (D7, D16;
   Track C's D3 and D9).
   - The core reads `durable_` at 34 places (`core.h:46-1185`). The design names a cursor for
     apply, verdicts, refresh, linearizable reads, `quorum` and `all`, the checkpoint ceiling,
     D6's floor, and the append's commit field (DM:348-351, 370). It names none for:
     - membership changes (`core.h:626-627`: an entry of the term committed, none above the
       commit index). On `commit_memory`, a second change can be admitted on a first that a
       correlated loss later removes, which Track C's single-step argument (its D3) never
       allowed for: beyond the model the membership itself, not only a write, would rest on
       page caches;
     - the promotion check (`core.h:650`) and the witness's bound (`data_matched`,
       `core.h:1136-1145`, from `matched`, which D7 makes the written index). A witness that
       truncates entries the data voters hold only in page cache turns a recoverable correlated
       loss into a lost write;
     - `deliver` (`core.h:1163-1165`), which reads the persisted commit index: with D4's
       commit index riding the bounded barrier, apply would wait for the barrier, against D7;
     - `Read` (`:739-747`), the uncommitted cap (`:722`), install's caught-up check (`:484`),
       and `Repair`'s bound (`:602`).
   - `commit_durable` must be clamped to the verified prefix in appends and coalesced
     heartbeats, as the commit is (`core.h:914`; TC:631-633). Otherwise an honest follower with
     a stale suffix learns a `commit_durable` above entries it must still replace, and fails
     closed against D6's floor (DM:321-324; calibration row 8's check).
   - The threat model's floor "only grows" (DM:652), but nothing persists `commit_durable`, and
     D16 has no hard-state row.
   - Fix: a site table like Track C's R2 (TC:227-251), each `durable_` use with its cursor and
     reason; membership admission, promotion, and witness truncation on the durable cursor;
     `commit_durable` clamped in kinds 3 and 9; `commit_durable` in hard state with its D16 row,
     or the claim withdrawn.

6. **DM-R6. The turn log supersedes and reclaims at completion, which D3 moves to the write**
   (D1, D3, T9).
   - `completed()` runs once "the staged turn's barrier returned" (`turn_log.h:315-329`): it lets
     a forwarded copy replace its original and a checkpoint record supersede its group's earlier
     records (`:343-350`), then reclaims every segment left with no live record (`:378-392`).
     D3 releases turn k + 1 at turn k's write (DM:193-194) and says nothing of these updates.
   - Applied at the write, a copy forwarded into an unsynced turn retires its synced original,
     the original's segment is recycled, and a power loss loses both: a synced record, which a
     `quorum` write may rest on, is gone. A checkpoint record would release its group's records
     before its own barrier.
   - Fix: D3 states that copy replacement, supersession, and reclamation take effect at the
     barrier covering the turn, never at its write; T9 adds power loss after a copy-forward
     turn's write whose source segment would otherwise be reclaimed.

7. **DM-R7. Three changes reach Detent's groups without an opt-in** (D3, D5, D6, D16).
   - "After any failed barrier" (DM:232) applies to every group, so a Detent voter whose barrier
     fails would restart unable to vote, and a recovery could write `EntryKind::Recovery` into
     its log. "Older readers refuse the kind" (DM:634) cites the wire codec (`codec.h:78, 170`);
     the storage reader accepts kinds up to `Configuration` (`storage.h:242, 272`), and the
     checkpoint reader calls a later kind corruption (`checkpoint.h:107`), which F13-R42's lesson
     says must read as unsupported.
   - D3's per-group variant (DM:210-216) changes the backend Detent runs (SYS-8): a boot identity,
     a clean-close marker, and DM-R2's recovery.
   - The version-2 bodies add 32 bytes to every append and 16 to every reply of groups that never
     use them. Detent agreed to restart into "finalized version 2"
     (`SHARED_LIBRARY_DECISIONS.md:131`), the peer transport's, which holds none of these fields.
     Its rule: "nothing Detent calls today changes behavior unless Detent opts in" (`:10-11`).
     The design has no "For Detent" list (the peer transport's is PT:1329-1372).
   - Fix: forgetfulness after a failed barrier, the `Recovery` entry, and the per-group journal's
     new records and rule apply only to groups whose configuration allows memory
     acknowledgements, or to hosts that opt in (offered to Detent: it closes fsyncgate for it
     too); `Recovery` under a new batch magic, read as unsupported by older storage and
     checkpoint readers; a "For Detent" list with each change, its bytes, and its opt-in, which
     the lead adds to Detent's file; Detent's agreement before step 4.

### Mechanism and wire

8. **DM-R8. Segment files get a second byte mover and a second flow control** (D8, D16).
   - D8 moves files through host frames and keeps consensus's snapshot transfer for "the image
     itself (for strata, the manifest)" (DM:392-400). Host frames are node-level, authorized by
     the session's node with the group in the body (PT:444-446), and spend group zero's credit
     (PT:730). Snapshot chunks are group-level kinds 16 and 17 (PT:623-624) on the group's own
     bulk window, "so one mechanism paces snapshots" (PT:840-843; TC:796-797).
   - So strata's installs use two movers and two flow controls, and the host re-implements D5's
     binding. Its rule differs: a committed membership only (DM:649), where D5's rule 2.2 binds an
     uncommitted one, so an honest learner whose addition is uncommitted cannot fetch
     (calibration row 8). One group's multi-gigabyte catch-up shares group zero's credit with
     every other group's per-refresh fetches to that node, which Track C's R12 exists to prevent.
   - Fix, before version 2 freezes: one mover. Extend `RFTXFR01` (kinds 16 and 17) with a file-set
     transfer addressed by physical identity, each file with its size, CRC-32C, and SHA-256,
     skipping what the receiver holds, at group level with the group's bulk credit and D5's
     binding; segment shipping, D18's retry, repair, Q7, and Detent's one-file image all ride it.
     Or define the fetch as a group-level kind now, since 19 to 255 are refused at version 2
     (PT:626). Either way one implementation, in `consensus`, which strata drives.

9. **DM-R9. The version-2 fields are incomplete and freeze before their semantics are reviewed**
   (D16, first steps).
   - The fields land in version 2 "before the coordinated `DTC2` commit" or wait for version 3
     with the feature off (DM:620-624). Step 4 waits only for the peer transport's re-check and
     Detent's agreement, while step 3, the semantics, waits for the full review (DM:821-822).
   - Missing: `forgetful` on kind 6 (DM-R1); the `synced` report (DM-R4); `commit_durable`'s
     clamp and hard state (DM-R5); the mover's kinds (DM-R8); `Recovery`'s storage magic
     (DM-R7); Part B's possible cursor (DM-R15).
   - Fix: the coordinated commit waits for this design's full review of D5 to D8, or these fields
     leave version 2 and memory acknowledgements wait for version 3 under D7's feature rule
     (PT:523) (Q8); D16 lists every field above with its kind and size.

### Segment mode

10. **DM-R10. The replay bound is not load-bearing** (D9, D14, D15, T8).
    - "Replay: at most `election_min`" (DM:459) holds only while each follower's fetch keeps up.
      With the leader's uplink or a follower's disk saturated, every install lag passes the
      bound and the promoted follower replays all of it. D14's remedy, "the leader flushes
      earlier" (DM:596), makes more, smaller segments and more merge output to ship (+103% bytes
      at the 16 MiB configuration, `arith.out`). D15's switch from document to segment mode
      takes "the leader's next image whole" (DM:614), during which a follower's lag is the whole
      index.
    - Fix: a follower whose install lag passes the bound applies the tail itself through the
      document-mode path it already keeps for mixed versions (DM:431-432) until an `Image`
      covers it, so the replay stays within `election_min` at that follower's CPU, one
      mechanism; T8 runs with the leader's uplink saturated and during a mode switch.

11. **DM-R11. Segment mode spreads a leader's faults and delays followers' reads** (D8, D10, T4,
    T11).
    - In document mode each voter builds its own segments, and the digest comparison catches the
      one that diverges (REPLICATION.md:64-66). In segment mode followers install the leader's
      bytes, so a leader-side apply bug, nondeterminism, or bit flip before checksumming
      (REPLICATION.md:19) reaches every replica under valid CRC-32C and SHA-256, and only the
      log, which checkpoints truncate, remains to rebuild from. T4's equality (DM:739) holds by
      construction between a leader and the followers that installed its bytes. D13 rejects
      leaderless replication partly for leaving "no digest to compare replicas by"
      (DM:578-579).
    - Followers' visible state trails the leader by a refresh plus the `Image`'s commit, the
      fetch, and the install. `prefix` (the `replicated` default, DM:510), `bounded`, and
      `session` reads on followers see that lag, which D10 does not state; OpenSearch's caveat is
      in the prior art (DM:715).
    - Fix: the fault model and REPLICATION state the blast radius; a verifier applies a sampled
      range in document mode on one replica per group on a schedule and compares its digest with
      the installed image at that position, a mismatch reported as corruption; T4 seeds a
      leader-side fault in segment mode that the verifier must catch; D10 states the follower
      lag, and T11 tests `session` and `bounded` against it.

12. **DM-R12. The 0.35 to 0.37 estimate leaves out the hash, the transport, refresh, and D9's
    cadence** (D8, Q1, Q4, T2).
    - SHA-256: `Kronuz/sha256` is "portable C++, with no SHA instructions" (its README:68), and
      the install probe checks CRC-32C only (`sizing.cc:230`). At an assumed 200 to 400 MB/s, a
      pass costs 1.09 to 2.17 µs/doc at one flush and 3.04 to 6.07 at 16 MiB (`rescore_t2.out`),
      as much as the follower's whole measured cost or more. The leader hashes every shipped
      byte once, and a follower again when it fetches from a non-leader (DM:650): rf = 3 becomes
      0.379× to 0.387× (leader-sourced) and 0.395× to 0.420× (peer-sourced) at one flush, 0.358×
      to 0.365× and 0.373× to 0.397× at 16 MiB. A peer-sourced follower at one flush is 0.068× to
      0.093× a document-mode follower: under T2's 0.1× (DM:737), over Q1's "a twentieth or less"
      (DM:759-760).
    - Transport: the install probe reads the leader's files from the local page cache and writes
      without a sync (`sizing.cc:219-238`). No network, no TLS, no leader serving rf − 1 copies,
      no follower `fsync` is in the figures; T2's target includes them (DM:737), the summary and
      Q1 do not say so. OpenSearch's own result includes "a loss at 9 replicas" (DM:715).
    - Refresh and cadence: neither configuration refreshes, but the default 1 s refresh flushes
      (ARCHITECTURE.md:79-82, 105-106), which sets segment sizes, merge output, and the shipped
      bytes. The one-flush run holds 100,000 documents unshipped, 4.17× D9's replay bound of
      23,958: a cadence document mode may run when refresh is relaxed for bulk ingest (as
      Elasticsearch users set `refresh_interval: -1`) and segment mode may not. Segment mode at
      the 16 MiB cadence against document mode at one flush is 1.047× (136.3 against
      130.2 µs/doc): more CPU, not 0.37. For one large index the estimate brackets
      [0.37, 1.05]; where many indexes share the node's buffer budget (ARCHITECTURE.md:102-103)
      both modes flush small, and the 0.35 pairing applies.
    - Fix: D8, the summary, and Q1 state the figures as leader-sourced without hash or transport,
      with the bracket; T2's procedure measures at the default refresh, over loopback TLS with
      SHA-256 (or a hardware-accelerated hash, by extending `Kronuz/sha256` with SHA-NI and
      ARMv8 SHA2), from both sources, and for one large index at the cadence D9 allows against
      document mode at its own best cadence.

### Exactness

13. **DM-R13. Several loss windows and read bounds are not exact** (D2, D10, D14, the fault
    model, T7, T11).
    1. **`memory`.** The window is `sync_interval` plus the barrier's actual latency
       (DM:36, 175-178), and D14 lets a slow device widen it with no cap: `max_unsynced`
       "refuses nothing" (DM:594). A device that stalls for 30 s leaves 30 s of acknowledged
       writes unsynced on its voter, and on a majority when the stall is shared (one storage
       backend under several voters). Fix: past `max_unsynced`, or k · `sync_interval` since a
       core's oldest unsynced write, that core's memory acknowledgements wait for its barrier:
       the acknowledgement degrades to `quorum`'s latency, no write is refused, and D2 states the
       window as the smaller of the cap and the interval plus a barrier. Lucene, Tantivy, and
       Xapian have no counterpart (they acknowledge after a commit's syncs), so the cap is named
       by its mechanism.
    2. **`replicated` under an OS crash of at most f voters** (DM:661) is not "nothing lost"
       while DM-R1 stands.
    3. **`leader`.** "Survives ... a leader process crash it returns from within an election
       timeout" (DM:492) and "if another voter is elected before it returns" (DM:660) both miss
       the case: the restarted node comes back a follower whose timer starts at its restart
       (`core.h:432`), its peers' timers started at their last heartbeat and fire first, and the
       first to campaign wins with the other follower's grant, the ex-leader refusing, and
       without the tail (`core.h:765, 803, 1002-1010`). The condition is "if another voter wins
       the next election". T7 then contradicts D10: it requires every write acknowledged at
       `leader` or stronger to survive 1,000 `SIGKILL`s (DM:742). Its level must be `memory`.
    4. **`relaxed` under a partition** (DM:664). A quiescent leader's CheckQuorum starts afresh
       when the first write wakes it (TC:357-360), so the window runs `election_max` from that
       write, not from the partition.
    5. **`bounded(K, T)`** (DM:500). A deposed leader's messages carry a stale commit index for up
       to `election_max` on a minority side, so the real-time staleness is T plus `election_max`,
       not Cosmos DB's bound against the true state the table names.
    6. **`session`** (DM:501). "Whose log holds the token's term at its index" cannot be checked
       once a checkpoint truncates below the index. Fix: a term history per group (the first
       index of each term, as Kafka's leader-epoch cache keeps), retained across checkpoints.
    - Fix for 2 to 6: each restated in D10 and the fault table; T7's level `memory`; T11 for a
      minority side's `bounded` reads and for tokens below the base.

14. **DM-R14. Leases assume restarted voters wait, and they do not** (D9, T5).
    - `core_probe.out`: a voter restarted at term 5 grants a pre-vote at term 6 one millisecond
      after its restart, and a vote at the next step. The guard reads leader contact held only in
      memory (`core.h:765, 770, 803`). "The old lease ends before any follower's timer can fire"
      (DM:473) does not cover a voter that grants on request, so a candidate can win inside the
      old lease and lease reads go stale.
    - Fix: with leases on, a voter that starts grants no vote or pre-vote for the lease duration
      after its start, as if it had heard a leader then (the assumption a lease rests on, made
      explicit); a new leader serves lease reads only after a quorum answered a heartbeat of
      its term and it committed an entry of its term; T5 adds restarts with leases on. Leases are
      off by default, so this binds before they are built.

15. **DM-R15. Part B can reopen what A freezes** (D1, D5, D16, Q7).
    - Of Part B's open choices (DM:829-840), four reach A's frozen surfaces:
      1. "Whether the acknowledgement is A's `memory` ... or process memory only": with process
         memory, a process crash loses acknowledged writes, and D5's same-boot rule ("resumes, not
         forgetful", DM:238-240) is wrong for such groups.
      2. "What `commit_durable` means without a synced log": a group that never syncs never
         advances `commit_durable`, so D1's ceiling and D6's floor stop its checkpoints; a
         disk-tier position as a third cursor would be a wire field version 2 freezes without.
      3. Retention needs "each segment's time bounds readable without opening it (a format-13
         reservation)" (DM:839-840), which D16 does not ask for (DM:639). Format 13's fourth
         re-check places it in the manifest's attribute list (F13-R43), with a memory tier's "not
         yet on disk" mark and HLC bounds as later tags.
      4. A retention drop "stamped with the leader's time" (DM:843-844) is a second clock beside
         D12's HLC unless it is the HLC.
    - Fix: A decides B's acknowledgement medium now (recommended: A's `memory`, written to the
      kernel, so D3, D5, and the wire stand), or D5 defines forgetfulness per medium and D16
      reserves the cursor; D1 and D7 say what bounds checkpoints in a group with no synced log;
      D16 asks format 13 for F13-R43's attribute list with the time-bound and tier tags;
      retention stamps use the HLC.

16. **DM-R16. Format 13's identity and the HLC need this design's choice before the freeze** (D8,
    D12, D16).
    - "The producing term in the identity" (DM:404-405). F13-R41 shows today's ids collide across
      replicas (A's and B's segment 2 share `0200000000000000 4141414141414141` at 732 and 740
      bytes) and that a producing term repeats across replicas that flush in one term, as
      document mode does; and the footer's CRC does not bind the 16-byte prefix (CRC-32C over any
      valid prefix is `0x48674bc7`). So "files it lacks" (DM:387) must compare identity, size,
      and the whole file's checksums, and the identity is F13-R41's (128 random bits, or a
      per-node incarnation with a counter).
    - "A per-version HLC, reserved in format 13" (DM:559-560). Format 13's table answers "the
      durability design chooses": the version field (F9's u64) or a `keys` sub-structure under an
      unlisted bit, and "LWW also needs a delete's HLC until the conflict window closes ...
      tombstones as keyed documents". D12 chooses neither and says nothing of deletes. The HLC is
      also a foundations need with no library yet (`foundations-vision.md`, "Foundations the
      vision is missing").
    - Fix: D8 adopts F13-R41's identity and the whole-file check; D12 chooses the HLC's place and
      tombstones before format 13 freezes, and names the HLC as a foundations library, not strata
      code.

17. **DM-R17. D9's components are misattributed, and the client's part is missing** (D9, T8).
    - A pre-vote persists nothing (`core.h:764-767, 1007-1010`), so the pre-vote round is a round
      trip, not "two barriers and a round trip" (DM:457; `arith.py`'s `rounds = 2`). The core
      persists the no-op before it sends (`core.h:1088-1090`; appends leave from `drive` after
      `Persisted`, `:470-473`), so a `quorum` first commit is two barriers and a round trip, not
      one barrier (DM:458). The total is 2,323 ms, not 2,403 (−3%); about 2,164 ms for `memory`.
    - The gap ends at the leader's first acknowledgement, but a client also waits out its request
      to the dead leader and is redirected, which T8's "next acknowledged write" includes.
    - Fix: the components corrected; the client's detection and redirect stated (strato's
      timeout and retry), or T8 measured at the leader.

18. **DM-R18. Precision** (D3, D5, D7, D14, D16, T14, the threat model).
    1. "The journal holds no clean-close record" (DM:234): the newest epoch must end with one,
       or a clean close, a same-boot reopen, and then an OS crash would read clean.
    2. "So turns land in file order" (DM:194) holds in the page cache, not on disk, where
       writeback reorders; the recovery rule (DM:197-200) already handles disk order and T9's
       sector subsets test it. The sentence should say so.
    3. `acks` "takes effect when committed" (DM:355), "applies by position, as limits do"
       (DM:633), a membership is "in force from the moment it is durable" (TC:216-218), and D5
       reads the newest durable entry (DM:231-232). One rule: a node's own replies and
       forgetfulness follow the newest durable entry, the leader's client acknowledgements the
       committed one.
    4. `RFTW0002`'s activation: a node rolled back to N − 1 cannot read its own `RFTW0002`
       journal, so the driver writes it only after memory acknowledgements activate (PT:524).
    5. Limits: "a fixed number of fetches per requesting node at once" (DM:653) names no number
       or mechanism (calibration row 6; Elasticsearch's recovery chunk concurrency is the
       counterpart, and Lucene, Tantivy, and Xapian have none).
    6. T14's target is "the bound `research/no-stalls` sets" (DM:749), not yet set.
    7. D14's copy-forward row is "Unchanged" (DM:597), but DM-R6 changes when it releases
       segments.
    8. S, "the turn its writer last saw synced" (DM:195-196), must be the last turn whose write
       returned before a barrier that has since returned started, since a write concurrent with
       a barrier may or may not be covered.

## The open questions

- **Q1, Xapiand's default: `replicated`,** on DM-R1 (its loss row) and DM-R12 (its CPU claims
  restated). It still loses less than today in every row: today's process crash costs seconds.
- **Q2, `sync_interval`: 100 ms,** with DM-R13's cap.
- **Q3, the recovery policy: `balanced`,** with DM-R1's f + 1 count; `aggressive` then loses only
  beyond the model.
- **Q4, merges: shipped,** with their hashing and transport counted (DM-R12).
- **Q5, `memory` at rf = 1: option 1.** At rf = 1 it is process-crash safety, and D6 never runs.
- **Q6, one vocabulary: option 1.**
- **Q7, the split: option 1 only with DM-R15,** or Part B can reopen frozen formats and the wire.
- **Q8 (new), the coordinated version-2 commit:** it waits for this design's full review of D5 to
  D8 (recommended), or version 2 ships without these fields and memory acknowledgements wait for
  version 3. The owner and Detent decide, since Detent restarts once into version 2.

## Which work starts when

| Step | Starts | Waits for |
|---|---|---|
| 1. `RFTW0002` | After the re-check | DM-R2, DM-R6, DM-R7's gating, DM-R18 items 1, 2, 4, and 8 |
| 2. Boot identity and the cross-IO directory barrier | Now | Additive; Detent's suite at its commit |
| 3. `consensus`: cursors, forgetful voters, `Recovery`, `Repair{promotion}`, the ceiling | After the re-check | DM-R1, DM-R3's core gate, DM-R4, DM-R5, DM-R7 |
| 4. Version-2 fields | After the re-check and Q8 | DM-R1, DM-R4, DM-R5, DM-R8, DM-R9, DM-R15 |
| 5. `strata::group` | Phase 3R | DM-R3, DM-R8, DM-R10, DM-R11, DM-R16 |
| 6. The simulator's fault classes and checker modes | Now | DM-R1's trace as a forced schedule and its single model statement |
| 7. T1 on Linux and FreeBSD | Now | Nothing |

## Blocking items / Required changes

Before steps 1, 3, 4, and 5, and before the re-check:

1. DM-R1: kind 6 carries `forgetful`; D6's conditions only after f + 1 forgetful voters heard;
   one model statement; T3's forced schedule and T6's mutant.
2. DM-R2: same-boot recovery rewrites the tail; a torn turn before the last makes the voter
   forgetful; `synced` only for this process's barriers; the per-group backend's same-boot rule
   or its refusal of memory acknowledgements; T9's rows.
3. DM-R3: checkpoints of the newest generation at or below `commit_durable`, with retention and
   the core's gate; T4's row.
4. DM-R4: the `synced` report's path and latency; quiescence on `commit_durable`.
5. DM-R5: the site table; membership, promotion, and witness truncation on the durable cursor;
   the clamp; `commit_durable`'s persistence.
6. DM-R6: supersession, copy replacement, and reclamation at the barrier; T9's row.
7. DM-R7: opt-ins for Detent; `Recovery`'s magic and unsupported classification; the "For
   Detent" list.
8. DM-R8: one mover for segment files, at group level, decided before version 2.
9. DM-R9: the complete field list and Q8's order.
10. DM-R15: B's acknowledgement medium decided, or A's rules stated per medium; F13-R43 asked for.

In the revision, not blocking: DM-R10 to DM-R14 and DM-R16 to DM-R18.

## Conditions

1. **C1. Done criteria.** The ledger has none (`status/durability-modes.md:9`). The lead writes
   them before step 2. Proposed: K1, step 2 with tests on Linux, macOS, and FreeBSD and Detent's
   suite; K2, step 1 with T9's rows, DM-R2's and DM-R6's included; K3, step 3 with T6's mutants,
   DM-R1's and DM-R5's included, and T12; K4, ASan, UBSan, and TSan over every touched test and
   coverage at TESTING's targets; K5, CI green on all jobs and Detent's suite at each library
   commit; K6, step 4 only after the full design review and Q8.
2. **C2. For the security reviewer,** through the lead: the fetch's binding by committed
   membership against D5's rule 2.2 (DM-R8); which fetches need SHA-256 and what it costs
   (DM-R12); a member that sends `forgetful` probes falsely can bring D6's count to f + 1 early,
   which is the compromised-voter row (PT:213), but the row should say so.
3. **C3. Library commits** record Detent's build and its `LIBRARY_BOUNDARIES.md` entry (Track C's
   condition 1).
4. **C4. Both replication modes.** T3 to T5, T8, T11, and T13 run in document and segment mode,
   and the gate rows state that matrix's cost.
5. **C5. The changes to other documents** (DM:679-704) gain DM-R11's residual for REPLICATION and
   DM-R13's restatements.

## Verdict and required changes, in one line each

**REJECT for first steps 1, 3, 4, and 5; steps 2, 6, and 7 proceed under C1 and C3.**

1. DM-R1: D6's timeout grants inside the model; count f + 1 forgetful voters first.
2. DM-R2: same-boot restarts trust a tail a failed barrier or the per-group frontier lost.
3. DM-R3: apply at `commit_memory` leaves no checkpoint D1 allows; checkpoint a past generation.
4. DM-R4: `synced` cannot reach the leader, and quiescent groups stop at `commit_durable`.
5. DM-R5: 34 `durable_` sites; configuration, promotion, and witness on the durable cursor.
6. DM-R6: turn-log supersession and reclamation must wait for the barrier.
7. DM-R7: failed-barrier forgetfulness, `Recovery`, and the per-group change reach Detent.
8. DM-R8: a second byte mover and flow control for segment files.
9. DM-R9: version 2's fields are incomplete and freeze before review.
10. DM-R10: the replay bound depends on fetch bandwidth; followers apply past it.
11. DM-R11: segment mode spreads leader faults and delays follower reads; a verifier.
12. DM-R12: the CPU estimate omits the hash, transport, refresh, and D9's cadence.
13. DM-R13: the `memory`, `leader`, `relaxed`, `bounded`, and `session` statements, and T7.
14. DM-R14: restarted voters grant at once, which leases must forbid.
15. DM-R15: Part B's medium, cursor, and format needs reach A's frozen surfaces.
16. DM-R16: F13-R41's identity and the HLC's place in format 13.
17. DM-R17: D9's components and the client's redirect.
18. DM-R18: clean close per epoch, disk order, the `acks` rule, activation, limits, T14, S.

Not run: no benchmark, timing, or A/B, and no SHA-256, network, or TLS throughput measured
(DM-R12's sensitivities are arithmetic over labeled assumptions); no strata build or suite (the
branch changes no library code, and CI skips docs); no `consensus` or `journal` suite, only
`core_probe.cc` against their headers; no simulator (unbuilt); no Linux, FreeBSD, or arm64; no
fuzzing; the model only to depth 10 or 11 at 3 voters, two writes, and terms up to 3, with
atomic messages; the security reviewer's review of the threat model not read (it runs
separately); the prior art's quotations (OpenSearch, KIP-966, Cosmos DB, PostgreSQL, MongoDB)
not re-fetched; no Xapian or Xapiand GPL source (`src/xapian`, `sortable_serialise`) opened.
