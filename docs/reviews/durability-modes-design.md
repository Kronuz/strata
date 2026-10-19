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

## Re-check (revision 1)

- Date: 2026-10-19, read and run from 06:39 to 07:25 PDT.
- Reviewed: `design/durability-modes` at `fc30ed3` (revision 1 at `1efb815`, the lead's calls on Q9
  and Q10 at `fc30ed3`; `git log --first-parent 8f8b043..fc30ed3`, 12 commits, `main` merged at
  `1365cd1`): `docs/design/durability-modes.md` (its "Changes in revision 1" first),
  `docs/status/durability-modes.md` (the owner's answers to Q1 to Q6 and Q8, the lead's calls on
  Q9 and Q10), the model sources `bench/durability/model/forgetful_model.py` and
  `forgetful_check.cc`, `bench/durability/arith.py`, and the raw files
  `results/model-crosscheck.out`, `model-exhaustive.out`, `model-swarm.out`,
  `model-schedules.out`, `arith.out`, and `2026-10-18-t2-macos.txt`. The small-indexes design at
  `perf/small-indexes` `45985dc` (`git show`, D3 and D3-C). The security re-check (`c87385e`,
  pushed to this branch after `fc30ed3`) read for overlap; its findings are its own.
- Checked against: the preliminary review above (DM-R1 to DM-R18, C1 to C5) and the security
  review (DMS-1 to DMS-9); Track C at `fc30ed3` (D1, D3 and its replacement-window table at
  `track-c.md:270-275`, D4, D9); consensus `61149a2` (`core.h`: all 34 lines that read `durable_`,
  and `934-972`, `1145-1172`); journal `cb66f3d` (`turn_log.h:84-88`, `268-335`); strata
  `index.hh` at `fc30ed3` (`335-344`, `405-441`, `498-511`, `2165-2196`, `2253-2276`,
  `2300-2336`); the peer transport at `38a6c6f` (`624`, D9's sizes); format 13 at `43ee403`
  (F13-R41 `:470-482`, F13-R51 `:1116-1123`, bit 16 `:358`, F21 `:1152-1170`); PLAN's recovery
  tooling (`PLAN.md:663-680`); `calibration.md` (ten rows, unchanged since the preliminary
  review, applied below).
- Checked by:
  - `quiet check`: exit 2 (light holds `qexp-e4a`, `qexp-e4b`, `qexp-joint`, `qexp-m3`) or 0 at
    every check from 06:39 to 07:22, never 1; every build and run at `nice -n 19`, at most four
    jobs, each long run under a wall-clock cap (`run_bounded.sh`).
  - Builds: the designer's `forgetful_check.cc` with Homebrew clang 23.1.2, `-O2`, and `-O1` with
    ASan and UBSan (a depth-8 run: 1,432,552 states, the Python model's count, no report); my
    variant `forgetful_check_r1.cc` (the designer's file plus same-boot process restarts and
    round retries), `-O2` and with ASan and UBSan (1,489,228 states at depth 8, its Python
    counterpart's count, no report).
  - Reproduced: the cross-check counts, 9,963,394 states (three voters, depth 10, 29 s) and
    12,039,941 (five voters, depth 5, 47 s); every forced schedule under five modes
    (`schedules-rerun.out`): `dmr1` loses under `timeout`, `fplus5` under `timeout` and `fplus`,
    `heard5` under `timeout`, `heard`, and `fplus`, `fplus5b` fences under `timeout` and `fplus`,
    and `round` blocks all four; `live3` decides under `round`.
  - Deeper, wider, and new: the run table after the status table, each with its `.out`.
  - Probes, all in `strata-wt/review-durability/.scratch/review-durability/r1/`, kept with
    their outputs: `forgetful_r1.py` (restarts and retries on the designer's Python model, and
    the `restart3` schedules), `forgetful_check_r1.cc` (the same in C++), `forgetful_members.py`
    (the model with Track C's membership in the log and a replacement window, the `six` and
    `six_loss` schedules), `members_final_state.py`, `members_prefix_walks.py`, `rescore_r1.py`
    (revision 1's CPU figures from the raw T2 file), `rerun_schedules.sh`, `run_bounded.sh`, the
    runs' `cc-*.out`, `py-*.out`, `schedules-rerun.out`, `restart3-schedule.out`,
    `members-schedules.out`, `members-six_loss-final.out`, `rescore_r1.out`, and copies of
    `core.h`, `turn_log.h`, and the small-indexes and peer-transport designs at the commits
    above. The preliminary review's probes are one directory up, unchanged.
  - Not run: the last line of this section.

Citations: `DM:N` is `docs/design/durability-modes.md` at `fc30ed3`; `ST:N` is
`docs/status/durability-modes.md` there; `SI:N` is `small-indexes.md` at `45985dc`; `core.h` is
consensus `61149a2`; `r1/` is `.scratch/review-durability/r1/`.

### Verdict

**REJECT for first steps 1 and 3; steps 4, 5, and 8 may start under C9, C11, and C12; steps 2, 6,
and 7 continue, step 6 under C10. Revision 1 answers every preliminary finding, and its D6 is a real
correction: my DM-R1 fix (f + 1 forgetful voters heard since the restart) was unsound at five
voters, as the designer found (`fplus5`, reproduced), and counting again at the round's own term
holds for one fixed membership at every depth I reached. Two new safety findings put `memory`
writes at risk inside the model: forgetfulness does not survive a same-boot restart (DM-R19, 7
steps, three voters, one OS crash), and the exactness argument assumes one membership, which
Track C's replacement window at rf = 5 breaks (DM-R20, six voters, at most two forgetful at once,
a `memory` write gone from every replica). Both fixes are small, a persisted mark per group and a
majority count, each with its proof and its schedules.**

What holds, and reproduces:

- **D6 at a fixed membership.** The exactness argument (DM:521-533) is right when one membership
  is in force: a normal leader above U needs a quorum that avoids every forgetful answerer and the
  candidate, which 2f + 1 voters cannot supply. The designer's cross-check counts reproduce
  exactly, and my runs go deeper and add process restarts and retried rounds (the run table): no
  violation at three voters to depth 13 with two writes, at five voters to depth 8 with two
  writes, and with restarts and retries at three voters to depth 12 and at five voters to depth 8.
- **My DM-R1 fix was wrong**, and revision 1 says so with the trace: `fplus5` loses `v0` in 15
  steps with at most two voters forgetful at once (`schedules-rerun.out`), because the count since
  the restart gathers episodes that never overlap. The check that would have caught it is the one
  DM-R20 applies below: build the schedule in which forgetful episodes run one after another.
- **The journal and the cursors.** Same-boot recovery (DM:370-378), effects at the barrier
  (DM:353-359, against `turn_log.h:315-329`, where `completed()` applies notes and reclaims), S
  (DM:360-366), the site table (DM:673-699: all 34 `durable_` lines of `core.h` are listed, each
  with a cursor that matches the code it guards), the `synced` path and quiescence (DM:649-657),
  `commit_durable` as the hard state's commit index (DM:407-411, 1025).
- **The mover, the wire, and Detent.** D17's sizes recompute (`Begin` 6,480 + 52, `Request`
  168 + 16, `Missing` 4 + 795 × 8 within 6,532); the fields are version 3's (DM:997-1001); "For
  Detent" (DM:1216-1242) gates every change on `acks: written`.
- **The CPU figures recompute** from the raw T2 file (`rescore_r1.out`): follower 0.051× to
  0.112×, three voters 0.376× to 0.434×, five voters 0.250× to 0.318× of document mode's CPU,
  matching DM:131-132, 775-776, and T2's estimate (DM:1281); each is labeled an estimate
  (DM:771). Item 5 of the request is answered under DM-R12.
- **The small-indexes rules** match `45985dc` in substance: SI-R19's entry (DM:273-277 against
  SI:308-314), SI-R20 and SI-R26's restore (DM:287-297 against SI:365-376), SI-R25's replay
  (DM:298-309 against SI:401-421). SI-R24's claim sentence is DM-R22 item 7.

What blocks:

- DM-R19: a forgetful voter that restarts in the same boot, or closes cleanly and reboots, before
  it re-learns becomes electable without the entries it lost (blocks steps 1 and 3).
- DM-R20: in a six-voter replacement window, a recovery election wins with three forgetful
  answers from episodes that never overlap, and its leader later wins a normal election and
  replaces a `memory` write everywhere (blocks step 3; binds step 6's schedules).

### Status

| Item | Status | Where, at `fc30ed3` |
|---|---|---|
| DM-R1 D6's timeout | **Partially closed.** The kind 6 flag (DM:1011), the round at its own term (DM:504-533), the flags on kinds 1 and 2 (DM:1007-1008), one model statement (DM:325-327, 118-120, 1156-1157, T3 and T5 at DM:1282, 1284, the simulator change DM:1210), the availability price (DM:579-580, 1164), T3's forced schedules and T6's mutants (DM:1282, 1285) are all in. At a fixed membership the rule holds in every run below. It does not hold across a membership change (DM-R20), and it rests on forgetfulness that a restart can clear (DM-R19) | DM-R19, DM-R20 |
| DM-R2 Same-boot restarts | **Closed.** Rewrite and sync after S before the open record, forgetful on any torn turn after S (DM:370-378); `synced` only for this process's barriers (DM:377-378); the per-group backend refuses `acks: written` (DM:393-398); T9's rows (DM:1288). DM-R19 is a separate hole beside it | |
| DM-R3 Checkpoints | **Partially closed.** The rule is right: the newest refresh generation at or below `commit_durable`, its files kept until a later checkpoint covers them, the core's gate on `through` (DM:257-272, 686), T4's row (DM:1283). Its claim that today's retention keeps those files (DM:264-266) does not hold (DM-R23) | DM-R23 |
| DM-R4 The `synced` path | **Closed.** Every reply carries `synced`; a reply to an append whose `sync_through` reaches its entries waits for the barrier; coalesced records without a flight; quiescence on `commit_durable` (DM:649-661); T10, T14 (DM:1289, 1293). Reply order around a waiting reply is DM-R22 item 3 | |
| DM-R5 The cursors | **Closed.** The site table covers all 34 lines (DM:673-699; `grep` of `core.h`); membership, promotion, and witness on `commit_durable` (DM:682-683, 695-697); both commits clamped (DM:665-667, 1009, 1013); the hard state's commit index is `commit_durable` (DM:407-411, 1025). The leader's own match at `core.h:1151` is DM-R22 item 2 | |
| DM-R6 Supersession | **Closed.** `written()` and `synced(turn)` (DM:353-359); D14's row (DM:971); T9 (DM:1288) | |
| DM-R7 Detent | **Closed.** `acks: written` gates forgetfulness, `Recovery`, and the records (DM:415-416, 553-556, 1221-1229); new magics and `Unsupported` (DM:1024, 1027); "For Detent" (DM:1216-1242); Detent's agreement before step 4 (DM:1241-1242, 1344) | |
| DM-R8 One mover | **Closed.** D17 (DM:1048-1105); host kinds withdrawn (DM:1015) | |
| DM-R9 The field list | **Closed** for the wire (DM:1003-1046; Q8 moved them to version 3, DM:997-1001). DM-R19's fix adds one storage element (a persisted forgetful mark), which D16's lists must carry | DM-R19 |
| DM-R10 The replay bound | **Closed.** Followers apply past the bound (DM:747-754); no early flush (DM:811-814); the mode switch (DM:988); T8 (DM:1287) | |
| DM-R11 Blast radius | **Closed** as a statement (DM:1109-1114, 1168, 1193-1194, 762-764, T4 and T11 at DM:1283, 1290). The residual Q9's option 2 leaves is DM-R21 | DM-R21 |
| DM-R12 The estimate | **Closed.** SHA-256 through EVP, TLS and the stack, the verifier, the bracket (DM:765-787), the summary (DM:129-135), T2 at the default refresh over loopback TLS from both sources (DM:1281); every figure recomputes (`rescore_r1.out`). Item 5 of the request: every CPU ratio states its base and direction ("of document mode's CPU", "segment against document", "+0.01× of a group's document-mode CPU") except Q9's "(about 0.01× more)" (DM:1311), and each rests on T2's measured parts plus three labeled, unmeasured assumptions (EVP's rate, the transport's ns per byte, the verifier's 1 in 64); none is a measurement, and T2 is the gate. The preliminary review's bracket [0.37, 1.05] was arithmetic over T2's parts, not a measurement either; revision 1's is 1.135× at rf = 3 and 0.767× at rf = 5, larger because both modes now pay the transport and segment mode the hash. Two precision items, DM-R22 items 4 and 5 | DM-R22 |
| DM-R13 Exactness | **Partially closed.** Items 3 to 6 closed (`leader` DM:848, T7 at `memory` DM:1286, `relaxed` DM:1166, `bounded` DM:856, `session` and the term history DM:857, T11 DM:1290). Item 1: the cap is stated (DM:330-334, 967) but reads as the leader's client acknowledgements, while DM-R13's own case is a stall under the followers (DM-R22 item 1). Item 2: "Nothing lost (D5, D6: no recovery election wins within the model)" (DM:1162) is false under DM-R19 and DM-R20 | DM-R19, DM-R20, DM-R22 |
| DM-R14 Leases | **Closed** (DM:824-831; T5 DM:1284) | |
| DM-R15 Part B | **Closed.** `resident` at A's `memory` (DM:1352-1361); a synced journal in every preset (DM:316-318); the attribute lists (DM:1041-1046); the HLC (DM:1371-1373) | |
| DM-R16 Identity and HLC | **Closed.** F13-R41's identity and the whole-file check (DM:714-719, against `format-13.md:470-482`); bit 16 and tombstones as keyed documents (DM:917-928); the HLC as a foundations library (DM:929-933) | |
| DM-R17 D9's parts | **Closed.** 2,323 ms and 2,163 ms recompute (`arith.out:38`); the client's part (DM:804-806); T8 (DM:1287) | |
| DM-R18 Precision | **Closed**, all eight items (DM:384-386, 350-352, 632-635, 387-390, 1083-1094, 1293, 971, 360-362) | |
| DMS-1 to DMS-9 | **Closed** at the lines the security re-check lists (`c87385e`, its C1 to C7 table), which I checked against the same lines and agree with. Its new DMS-10 to DMS-13 are its findings; DMS-11 overlaps DM-R21 and is cited there | |
| SI-R19 | **Closed** (DM:273-277; SI:308-314) | |
| SI-R24 | **Closed in substance**: the caller leads, only unwaited checkpoints go to the pool, retention on the owner (DM:278-286). The claim rule is stated less exactly than SI:329-333 (DM-R22 item 7) | DM-R22 |
| SI-R20, SI-R26 | **Closed.** A record names only completed checkpoints; restore at the recorded generation through `Repair`, a leader transferring first, not forgetful; a failed journal barrier is D5's (DM:287-297, 428-429), as SI:365-376 says. (SI:365's heading still reads "a failed checkpoint is a failed barrier" above a body that says it is not; small-indexes' to fix) | |
| SI-R25, T16 | **Closed** as small-indexes states it (DM:298-309, 1036-1037, 1295; SI:401-421); the duplicate-`Create` case is the security re-check's DMS-10 | |
| C1 Done criteria | **Open, the lead's.** The ledger still has none (ST:13-14) | |
| C2 Security points | **Closed** by DMS-1 to DMS-3 and the threat model (DM:1145-1149) | |
| C3 Library commits | **Closed** as a rule ("For Detent" item 6, DM:1239-1242); checked at each commit | |
| C4 Both modes | **Closed** (DM:1273-1276) | |
| C5 Other documents | **Closed** (DM:1193-1196) | |

The model runs, mine against the designer's deepest (each one thread, `nice -n 19`, the times
the runs printed):

| Model | Voters, scope | Depth | States | Time | Result | File |
|---|---|---:|---:|---:|---|---|
| Designer's, `round` | 3; two writes, terms to 4, three OS crashes, symmetry | 12 (designer) | 48,190,011 | 337 s | holds | `model-exhaustive.out` |
| Designer's, `round` | the same | 13 | 119,235,974 | 975 s | holds | `r1/cc-round-n3-p2-d13.out` |
| Designer's, `round` | 5; one write, terms to 4, three OS crashes, symmetry | 8 (designer) | 67,817,052 | 1,128 s | holds | `model-exhaustive.out` |
| Designer's, `round` | 5; two writes, terms to 4, three OS crashes, symmetry | 8 | 72,045,304 | 1,186 s | holds | `r1/cc-round-n5-p2-d8.out` |
| Mine, restarts kept forgetful (the fix) and retried rounds | 3; two writes, terms to 4, three OS crashes, two restarts, symmetry | 12 | 149,268,279 | 1,245 s | holds | `r1/cc-r1-round-n3-p2-persist-retry-d12.out` |
| Mine, the same | 5; one write, terms to 4, three OS crashes, one restart, symmetry | 7 | 14,475,565 | 192 s | holds | `r1/gauge-r1-persist-retry.out` |
| Mine, the same | 5; one write, terms to 4, three OS crashes, one restart, symmetry | 8 | 109,734,286 | 1,755 s | holds | `r1/cc-r1-round-n5-persist-retry-d8.out` |
| Mine, restarts as D3 reads them | 3; one write, terms to 2, two OS crashes, one restart | 8 | 1,489,228 | 11 s (C++), 38 s (Python) | `memory` and `fence` at 7 steps | `r1/py-restart-text-n3-d8.out` |
| Mine, membership in the log | 6, a replacement window | forced, 43 and 49 steps | | | `memory` lost; blocked by DM-R20's majority count | `r1/members-schedules.out` |

### New findings

1. **DM-R19. Forgetfulness does not survive a restart, so a voter that lost entries becomes
   electable without re-learning** (D3, D5, D16, T7, T9).
   - D5 makes a voter forgetful after an unclean restart or a torn same-boot tail (DM:418-427)
     and keeps it so "until it re-learns" (DM:433-437). Nothing records that state. The open
     record stores only the boot identity (DM:382-386), the hard state gains "no new bytes"
     (DM:1025), and the rules for the next restart look only at the boot identity, the clean-close
     record, and torn turns: "a same-boot restart without one rewrites and syncs the tail and
     resumes, not forgetful" (DM:422-423), and T7 asserts it: "a voter restarts forgetful only
     when its tail was torn" (DM:1286).
   - So: A leads, `v0` is written on A and B and acknowledged at `memory`; A loses power and
     restarts forgetful without `v0`; A's process crashes (or the host restarts it after a failed
     barrier) in the new boot; the open record names this boot, no turn is torn, and A resumes
     not forgetful. A campaigns at term 2 and wins with C's vote, and its log lacks `v0`, which B
     then replaces. One OS crash at three voters, inside the model, which counts A until it
     re-learns (DM:325-327). `r1/restart3-schedule.out` replays it (7 steps); the
     exhaustive search with D3's restart rule finds it and a fence at depth 7
     (`r1/py-restart-text-n3-d8.out`: 1,489,228 states; the C++ variant agrees). A clean close
     followed by a clean reboot does the same: the newest epoch ends with a clean-close record,
     so the restart is not unclean (DM:367-369).
   - It also breaks D6's argument, whose step "an answerer that had answered was still forgetful
     then" (DM:528-529) assumes the flag lasts until re-learning.
   - Fix: forgetfulness is durable per group. A forgetful voter writes a mark for the group (in
     the turn log beside the open record, or one byte in the group's hard state) before it
     answers anything, and clears it with a re-learned mark written after the re-learned entries;
     neither needs its own sync, since a power loss makes the voter forgetful again and a
     same-boot restart reads both from the page cache. Every restart, same-boot, clean, or
     unclean, carries the mark forward; a clean close does not clear it. D16 lists the element
     and D3 or D4 states which (the choice decides whether step 1 or step 3 carries it). T7 reads
     "forgetful when its tail was torn or it was forgetful when it stopped"; T9 gains a forgetful
     voter's same-boot restart and its clean close and reboot, each before re-learning, each still
     forgetful; T6 gains the mutant "forgetfulness cleared by a restart", which `restart3` kills.
     With the mark kept, my variant holds where the designer's model holds (the table above).

2. **DM-R20. D6's exactness assumes one membership; in Track C's replacement window at rf = 5 a
   recovery election wins inside the model and a `memory` write is lost** (D2, D5, D6, Q10, T3,
   T6).
   - The argument bounds a later leader's quorum by "2f + 1 − (f + 1) = f voters" (DM:530-531),
     with the later leader counting the same 2f + 1 voters. Track C changes membership one voter
     at a time, a node using its newest durable configuration entry (`track-c.md:216-218`), and
     its replacement passes through six voters at rf = 5, "quorum 4, two failures"
     (`track-c.md:274`). A candidate X in force with six voters needs f + 1 = 3 forgetful answers;
     a leader W with the five-voter membership that removes the leaving voter Q needs 3 of 5, and
     with X and one forgetful answerer P excluded, 3 remain. So W can be elected between the
     answers, P re-learns from W, and Q, the leaving voter, loses power after that and answers
     X: the three forgetful answers come from episodes that never overlap, the same shape that
     broke my DM-R1 fix, now through a membership change.
   - `r1/forgetful_members.py` is the designer's model with the membership in the log (Track C's
     D1 and D3, admission on `commit_durable` as D7 puts it). The `six` schedule (43 steps): S
     leads at term 1, `v0` is written on S, T, X, and P and acknowledged at `memory`, S appends
     Q's removal and T syncs it; X, then Q, then P lose power, Q re-learning from S in between, so
     X has heard three forgetful voters; X starts a round at term 2 and P (forgetful) and R grant;
     T is elected at term 3 by R, S, and T under the five-voter membership; P re-learns from T; Q
     loses power again and grants X, flagged; after `recovery_timeout` X decides with 4 grants,
     3 flagged, lacking `v0`. At most two voters are forgetful at any instant, f for both
     memberships. `six_loss` (49 steps) continues: Q re-learns from X, X learns a higher term
     without T's log, wins a normal election at term 5 with Q, which holds X's log, and R and S,
     whose logs end below term 2, and its appends replace `v0` at S, T, and P under `recovery_term` 2 with no fence
     (D6's rule, DM:557-561). `members-six_loss-final.out`: no replica holds `v0`, which was
     acknowledged at `memory`. The loss needs the recovery winner to stop being forgetful once it
     leads, which the designer's model assumes (`forgetful_model.py:328` in
     `bench/durability/model/`) and D5's re-learning rule reads as ("a leader of its durable
     term", DM:437); the win alone already breaks D6's claim and the model's oracle.
   - The failure needs a forgetful candidate and an even membership with f of 2 or more (rf = 5's
     replacement, growth from 5 to 7): with a candidate that is not forgetful, the adjacent
     memberships leave no quorum between the answers, and at rf = 3's four-voter window (f = 1) a
     forgetful candidate overlaps its one other flagged answerer, two at once. The schedule uses the
     model's abstractions, atomic messages and no pre-vote; with pre-vote, T reaches term 2 through
     X's own request, whose refusal reaches X after it decides, which D6 allows (it counts the
     answers received, DM:517-520).
   - Fix: the decision counts a majority of the candidate's membership flagged forgetful,
     ⌊n/2⌋ + 1 of n voters, the candidate's own counted; that is f + 1 for odd n, so steady-state
     groups are unchanged, and f + 2 for even n. The argument, restated for the memberships Track
     C admits: a normal leader above U elected before the decision has the candidate's membership
     or one a single change away (the next change waits for the previous one at
     `commit_durable`), and must avoid the candidate and every flagged answerer; with n + 1
     voters that leaves at most ⌈n/2⌉ < ⌊(n + 1)/2⌋ + 1, and with one flagged answerer removed at
     most n − 1 − ⌊n/2⌋ < ⌊(n − 1)/2⌋ + 1, so none exists, no answerer re-learns, and more than
     f are forgetful at once. With that count, `six` stops at the decision (`members-schedules.out`,
     `--count major`). The start condition may stay f + 1. The model statement says what f is
     for an even membership (⌊(n − 1)/2⌋, Track C's table) and that it holds for every
     membership in force. D5 and D6 say whether a recovery winner is forgetful once it leads.
     The fault table gains the price: in a six-voter window, three forgetful voters stall until
     one returns or `manual`. Q10's "holds at every replication factor" (DM:1330-1331; ST:202-204)
     becomes "at every fixed membership, and in replacement windows with the majority count";
     option 1 stands with that count, and options 2 and 3 avoid DM-R20 too (rf = 3's window is
     safe), at the costs Q10 states. T3 gains `six` and `six_loss` as forced schedules; T6 gains
     the mutant "f + 1 at an even membership"; the checker gains membership changes (my Python
     model is a start; the designer's C++ port needs six voters).

3. **DM-R21. Q9's option 2 states a residual that nothing in the design produces** (D18, Q9, the
   fault model).
   - Q9's recommendation: "a rare single corrupt segment, found later, is rebuilt from the source
     or a backup" (DM:1316-1318), which the lead's call rests on (ST:200-201). Nothing finds it
     later. D18 samples each new flush and merge once, at 1 in 64 per follower (DM:1115-1124); a
     missed flush is merged, and the merge check re-runs the merge over the inputs the follower
     holds, the corrupt one among them, so it passes (the security re-check's DMS-11 makes the
     same point for an attacker; for an honest fault it means a miss is permanent). The digest
     comparison is equal by construction in an all-segment-mode group (DM:1112-1113), and T4's
     end-of-run rebuild is a test oracle, not a production check.
   - The per-event figure is in D18 (3.1% detected at rf = 3) but not in Q9, where the choice is
     made: a single corrupt flush reaches every replica undetected with probability 96.9% at
     rf = 3 and 93.9% at rf = 5 (`rescore_r1.out`), where in document mode the fault stays on the
     leader's own copy, which the digest comparison finds and another replica repairs. "Rare" has
     no rate.
   - The sample covers new segments and merges, not the `Image`'s live-docs generations
     (DM:711-712): a leader-side fault in deletions (an old version left live after a replace) is
     outside it.
   - "A backup" names nothing strata plans: PLAN's recovery tooling rebuilds derived structures
     from stored records and restores "from elsewhere: a replica, the journal, or a checkpoint"
     (`PLAN.md:675-678`), each of which holds the same bytes in an all-segment-mode group.
   - Fix, before the full design review: Q9 and D18 state the residual as it performs (detected
     with probability 1 − (63/64)^(rf − 1) per corrupt flush, permanent once missed, deletions
     unsampled, repair by re-indexing from the client's source or PLAN's stored-record rebuild
     when the stored records are intact, and no backup); or D18 samples the live-docs delta of
     each sampled range and keeps its own record of unsampled flushes for a later scrub. The
     lead's call on Q9 may stand; the owner sees this residual before the full review clears it.

4. **DM-R22. Precision** (D1, D2, D7, D8, D14, Q9).
   1. The memory window's cap (DM:330-334, 967) is stated for a core's `memory`
      acknowledgements, which only a leader gives; D2 and D14 say it binds every voter's written
      replies (a follower past it replies after its barrier), since DM-R13's case was a stall
      under the followers.
   2. The leader's own match in `advance_commit`, `last_index()` at `core.h:1151`, must be its
      written index for `commit_memory` and its synced index for `commit_durable`; the "Beyond
      `durable_`" list (DM:695-699) leaves it out.
   3. A reply that waits for its barrier (DM:650-652): a later reply retires every older flight
      (`core.h:946-948`), and a reply with no flight drops (`core.h:940`), so a follower that
      answers a later append at `Written` first loses the `synced` report the `quorum` write waits
      for, until the next reply. D7 states that replies stay in append order, or that the leader
      takes `synced` from any reply of the term as it does from coalesced records.
   4. The cadence bracket (DM:779-780) leaves out the verifier the rest of D8 includes: 1.165× at
      rf = 3 and 0.803× at rf = 5 with it (`rescore_r1.out`).
   5. T2's targets sit 1.6 to 4 points above the estimate's top (DM:775-776, 1281), and the
      follower's fails if EVP runs at about 0.35 GB/s or less without SHA extensions
      (`rescore_r1.out`: 0.121×); D8 says the gate's margin rests on that unmeasured rate.
   6. Q9's "(about 0.01× more)" (DM:1311) names no base; D18's figure does (DM:1126).
   7. SI-R24: D1's "it leads instead" (DM:285) becomes small-indexes' claim rule (SI:329-333),
      so the two designs state one rule: a caller that starts waiting while a posted batch is
      queued claims it and runs it on its own thread, the posted task finds it claimed and
      returns, and a batch running on a pool worker needs no other worker.

5. **DM-R23. A checkpoint of a past generation needs retention keyed on that checkpoint's own
   manifest; today's keys the head on the in-memory state** (D1, T16).
   - D1: files of later states stay unreferenced until a later checkpoint, and "merge inputs that
     a later merge replaced stay referenced by the checkpoint's manifest, so retention keeps them
     (`index.hh:2167-2195`)" (DM:262-266). Today's `retain()` keeps the head `m_.generation`
     (`index.hh:2170`) with the names of the in-memory manifest, `names_of(m_)`
     (`index.hh:2227-2231`), and the current segments `segs_` (`index.hh:2195`); `commit_files()`
     sets `m_` to what it wrote (`index.hh:2145`). Under D1 what the checkpoint writes is
     generation g's snapshot, while `m_` keeps the current state, whose applied position decides
     what apply skips (`index.hh:407-410`). If `m_` became the snapshot, apply would re-run the
     positions after g; if it stays current, the head's names are the current state's, so the
     merge inputs only g names, and the live-docs and metadata files encoded for g (DM:262-263),
     are obsolete to `retain()` and queued for `reclaim()` (`index.hh:2198-2220`). A crash before
     the next checkpoint then opens a manifest naming missing files (`index.hh:2317-2318`:
     "every file the manifest names must exist").
   - Fix: D1 states that the durable head is the checkpoint's manifest, its names taken from the
     batch entry's snapshot and kept beside the in-memory state's files, and that `retain()` and
     `manifest_files()` read the head's names from it, not from `m_`; T16 gains "a checkpoint of
     a past generation whose inputs a later merge replaced and whose live-docs files it wrote,
     then the owner's retention and reclaim, then a crash and an open: every file the manifest
     names exists and the digest equals that generation's". It binds step 5's checkpoint code.

### New blocking items

1. DM-R19: forgetfulness persisted per group, carried across every restart; D16's element; T6,
   T7, and T9 as above. Blocks step 1 (if the mark is the journal's) and step 3.
2. DM-R20: the majority count at the decision; the argument restated for adjacent memberships;
   the model statement for even memberships; the recovery winner's state; the fault table's
   price; Q10's wording; T3's `six` and `six_loss`, T6's mutant, and membership changes in the
   checker. Blocks step 3.

### What approval needs

The two items above in the design, with the checker's runs (the designer's C++ port at six voters
with membership changes, at least the forced schedules and a bounded search near them) under
the fixed count; then a re-check scoped to `git log fc30ed3..`. DM-R21 to DM-R23 go into the same
revision: DM-R23 binds step 5's checkpoint code, DM-R21 and DM-R22 the full design review; none of
the three blocks the re-check.

### Conditions

1. **C6. Done criteria** (C1, still open): the lead writes K1 to K6 into the ledger before any
   step lands, with DM-R19's and DM-R20's mutants in K3.
2. **C7. Calibration**, for the lead: "2026-10-19 | DM-R1's fix in the preliminary review | f + 1
   forgetful voters counted since the restart loses a `memory` write at five voters | revision
   1's `fplus5` | Build the schedule in which the counted episodes run one after another, at
   five voters as well as three". DM-R20 is the same check applied to membership.
3. **C8. Step 2** (completion's boot identity and directory barrier) continues as ruled; nothing
   here changes it.
4. **C9. Step 4** (version-3 fields) may start: DM-R19 and DM-R20 add no wire field. It lands
   only after the full design review (DM:1001, 1344) and the security re-check's conditions.
5. **C10. Step 6** (the simulator) adds `restart3`, `six`, and `six_loss` to T3's forced
   schedules, process restarts that keep forgetfulness, and membership changes in the
   replacement window, before its gate.
6. **C11. Step 5** (strata::group) may start; its checkpoint code waits for DM-R23's statement,
   its `Create` replay for DMS-10's rule, and its batcher states SI-R24's claim (DM-R22 item 7).
7. **C12. Step 8** (D17's file sets) may start under the security re-check's C10 (DMS-12).
8. **C13. DM-R21, DM-R22, and DM-R23** in the next revision; DM-R23 before step 5's checkpoint
   code, the others before the full design review.
9. **C14. The model's limits stated** where D6 cites it (DM:582-612): atomic messages, fixed
   membership (until DM-R20's extension), no process restarts or retried rounds in the
   designer's model (my variants add both: with forgetfulness kept across restarts they change
   no verdict at the depths run, and with D3's reading they find DM-R19), and the depths and
   scopes as in the table above.

Not run: no benchmark, timing, or A/B (the CPU figures are arithmetic over the design's labeled
assumptions; no EVP, TLS, or network rate measured); no strata, consensus, or journal build or
suite (the branch changes no library code); no simulator (unbuilt); no Linux, FreeBSD, or arm64;
no fuzzing; the designer's checker and my variants only to the depths and scopes in the run
table, with atomic messages and hash compaction (64-bit keys: a state omitted with probability
about n² / 2⁶⁵); the membership model only as forced schedules, 200 walks from the start to depth
45, and 20,000 walks per count from `six`'s step 33 to depth 14 (`members-prefix-walks.out`:
nothing under either count, so the walks are no evidence either way), no exhaustive search at six
voters; the security re-check's probes not rerun; T1 and T2 not re-measured; the prior art not
re-fetched; no Xapian or Xapiand GPL source opened.
