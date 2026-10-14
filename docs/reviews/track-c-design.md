# strata review: Track C design

- Date: 2026-10-14.
- Reviewed: `docs/design/track-c.md` at strata `7f8d7eb`.
- Checked against:
  - PLAN: Goals 3 and 5, "Every gate", Track C (C1–C5, its ordering against the Detent
    split, the gate per item), Track D's ordering (D1 with C5, D3 after C1 and C2), and
    Phase 3R's start rule and gate.
  - REPLICATION: Guarantee, Replica count (`rf = 2 + witness`), consistency modes,
    Checkpoints and repair, Cost, and "Consensus library gaps" G1–G7. DISTRIBUTION: Scale
    contract, Layers, and Open decision 2.
  - `consensus` at `b7073bb`: `include/consensus/{core,types,storage,worker,admission}.h`,
    `include/consensus/{retained_log,checkpoint,snapshot,snapshot_transfer,snapshot_channel}.h`,
    `include/consensus/transport/{codec,hello,profile,peers,session,tls}.h`, and
    `test/consensus_test.cc` (`Host`, `Network`, `reordered_crash_schedules`).
  - `journal` at `2c18fcb`, the commit `consensus` pins (its HEAD, `57e6250`, changes only
    `README.md`): `include/journal/journal.h` and the README.
  - Detent's `docs/AUTHORITY_TRANSPORT.md`, `LIBRARY_BOUNDARIES.md`, and
    `SCALE_TARGETS.md` at Detent `081856e`.
  - My Phase 3 design and lifecycle reviews, for the house style.
  - The Raft literature where a ruling turns on it: Ongaro's dissertation (2014) §3.8,
    §3.10, §4.1–4.3, §6.2, and §9.6; Ongaro's single-server change bug (raft-dev, 2015);
    Howard and Abraham, "Raft does not Guarantee Liveness in the face of Network Faults"
    (2020); etcd's raft (heartbeat commit, CheckQuorum, `ConfChangeV2`); CockroachDB's
    atomic replication changes; TiKV's Raft Engine; Cidon et al., "Copysets" (2013).
- Checked by:
  - Building `consensus` `b7073bb` with its pins (journal `2c18fcb`, completion `373f02a`,
    metrics `9d7e724`, reactor `d5bd39b`): Release, Apple clang 17, Ninja, `-j4`. 14 of 14
    tests pass in 67.9 s; `consensus_consensus_test` runs 37,351 checks with 0 failures.
  - Probes in `.scratch/review-trackc/probes/`, rebuilt and rerun by `run_probes.sh`, with
    the output in `run_probes.log`. Each finding names its probe. The mutant core in
    `probes/mutant/` is a scratch copy; no repository file changed.
  - Not run: sanitizers (no code under review exists yet), benchmarks, fuzzing, Detent's
    suite, the gate VM, the FreeBSD VM, and the M1.

## Verdict

**APPROVE WITH REQUIRED CHANGES.** The decisions have the right shapes. Several are unsafe
or unmeasurable as written, so no C-item's code starts until its required changes land and
a revision check passes.

What holds, and reproduces:

- The starting point matches `b7073bb`: the fixed configuration and its initialization
  record (`core.h:45-50`, `storage.h:55-63, 184`), one storage batch outstanding
  (`core.h:243-248`), one flight per peer (`core.h:204, 626`), quorum reads without leases
  (`core.h:406-425`), `CaughtUp` at or below commit (`core.h:300-305`), non-members
  dropped (`core.h:427`), and the voter list in the fingerprint (`profile.h:14-26`).
- Membership in the log, in force once appended and rolled back with a replaced suffix, is
  the dissertation's rule (§4.1). D3's commit-first rule is the 2015 fix, and one change
  at a time keeps one quorum rule.
- Pre-vote and the leader-contact guard are the dissertation's (§9.6, §4.2.3) and affect
  availability only.
- Learners, the witness's retention rule, and repair from the voter's own log leave Raft's
  safety argument as it is.
- One journal per core, coalesced heartbeats, quiescence, pipelining with per-flight
  correlation, and per-group send queues are the right directions at 1,000 groups per
  node, with precedents in TiKV and CockroachDB.
- The real core passes 2,000,000 randomized steps over 2,000 seeds with restarts that
  interrupt persistence, with 0 failures (`probe_harness_crash_points.cc`).

What blocks:

- Repair through `InstallPrepared` would change the role, term, leader, and commit index,
  and a rewind with a delivery outstanding skips entries (findings 11, 12).
- A coalesced heartbeat's commit is not clamped to the follower's matched index, so a
  follower can apply an entry no leader committed (finding 15).
- D11's one barrier per turn needs a journal that does not exist: an append takes four
  ordering points today, and truncation replaces the whole generation (finding 13).
- Pre-vote and the guard ship without CheckQuorum. Quiescence switches the guard off, and
  a stray message or a follower restart then elects a new leader in a healthy idle group
  (findings 7, 8).
- Member ids are undefined, so the new-incarnation rule does not stop one node from voting
  as two incarnations (finding 5).
- D2's fingerprint clause contradicts T1 and Detent's admission guarantee (finding 21).
- The design states no memory at 1,000 groups; an idle group holds 136,168 bytes today,
  and each group's retained log, up to 64 MiB, lives in memory (finding 14). Snapshots
  move one 64 KiB chunk per round trip under a 60 s total timeout, so a large shard never
  installs across zones (finding 17).
- T3, T6, and T7 have no targets, and T6 measures the best case. T2 defines no persistence
  model, and the existing one cannot see a persistence-ordering bug (findings 18, 22, 24).

## Rulings

| Decision | Ruling | Changes |
|---|---|---|
| D1 Membership in the log | Sound with changes | Base membership in the snapshot descriptor, not the key; members carry their transport identity; every voter-list site restated (R2) |
| D2 Fixed configuration as the initial membership | Sound with changes; its fingerprint clause is wrong as stated | One versioned wire format, or a byte-identical preset from the same code; limits move out of the fingerprint (R1) |
| D3 One voter at a time | Sound with changes | The replacement window and the failure-domain exception stated; joint consensus as the named fallback (R3) |
| D4 Quorums follow the membership | Sound with changes | A removed leader steps down whether or not its transfer succeeds; reads count members only (R3) |
| D5 Who may talk to whom | Sound with changes | Member ids are incarnations named on the wire; admission without a per-connection group binding (R2, R12) |
| D6 Leadership transfer | Sound with changes | When the target acts on `TimeoutNow`; copies and delays (R4) |
| D7 Pre-vote and the guard | Sound with changes | CheckQuorum added; the guard under quiescence (R4, R5) |
| D8 Learners | Sound | Quiescence covers learners (R5); the catch-up bound is R10's |
| D9 Witnesses | Sound with changes | The residual restated; commits stall at the witness's limit; roles at founding (R6) |
| D10 Repair | Sound with changes | Its own event; preconditions on deliveries, captures, and reads (R7) |
| D11 One journal per core | Sound with changes | The journal specified: append protocol, segments, per-group checkpoints, reclamation, recovery, corruption (R8) |
| D12 Coalescing and quiescence | Sound with changes | Commit clamped; wake and campaign rules; cost as a function of peers per node (R5) |
| D13 Forced checkpoints and pacing | Sound with changes | Retention during transfers on disk; timeouts; the catch-up condition (R9, R10) |
| D14 The prototype | Sound with changes | Peers per node varied, the measured node's own storage, memory, recovery, both journal shapes (R14) |
| D15 Pipelining | Sound with changes | Follower-side batching, keepalives and probes, the transport's queue, the default (R11) |
| D16 Flow control and multiplexing | Sound with changes | Per-group receive credit, one lane per group's appends, authentication at scale (R12) |

## Findings

### Membership and roles (C1)

1. **D1: configuration on append, rollback, and the base membership.**
   - Using the newest `Configuration` entry from append, committed or not, and reverting
     when a replaced suffix held it, is the dissertation's rule (§4.1). In this core a
     node cannot act on an entry before it is durable: one batch is outstanding at a time
     and `drive` returns while busy (`core.h:243-248, 581`). "From the moment it is
     appended" (`track-c.md:42-43`) means from the moment it is durable; the text says so.
   - The membership in force at `base_index` belongs in `RecoveredState`, the checkpoint
     bundle, and the install. It does not belong in `SnapshotKey` (`track-c.md:45-46`).
     The key is a correlation token on `SnapshotResponse` (`codec.h:86-93`) and in the
     fixed 128-byte transfer envelope (`snapshot_transfer.h:37`), and a membership is
     variable-length. The portable snapshot descriptor carries it instead; today's is a
     fixed 72-byte `RFTSNP01` frame (`snapshot.h:48, 70`), so it takes a new version. A
     receiver that keeps its suffix (`core.h:307`) uses the newest entry in that suffix,
     else the descriptor's base membership.
   - A `Membership` lists ids and roles (`track-c.md:39-41`). The transport needs a
     member's address and certificate pin before it can dial or admit it: addresses are
     fixed at construction and must match the voters (`peers.h:64-71`), and pins are bound
     to the configured voters (`tls.h:21-36, 45-66`). Entries carry each member's
     transport identity, or the design names the directory that supplies it.
   - The fixed voter list is consulted at 21 sites in 9 files: `core.h:45-50, 60, 207-208,
     293, 427, 589, 612`; `storage.h:57-61, 116-118, 131, 181-182, 222`;
     `checkpoint.h:77-79`; `worker.h:251, 549-551, 761-768`; `snapshot_channel.h:41-42`;
     `transport/peers.h:64-71`, `profile.h:14-26`, `session.h:112-125`, `tls.h:21-36`. D2
     keeps them for fixed configurations. The design states what each becomes in
     membership mode. Two need decisions: a recovered ballot may name any id the group
     ever admitted, not only a current voter (`core.h:60`, `storage.h:131, 222`); and the
     snapshot channel's peers are fixed at construction (`snapshot_channel.h:41-42`).

2. **D3: single-server changes against joint consensus.**
   - Safety holds. Rule (a) is the fix that accompanied the 2015 bug report: no
     configuration entry until the leader has committed an entry of its term. Rule (b)
     keeps one change outstanding. With both, configurations that can coexist differ by
     one voter, and their majorities intersect (§4.1). Witnesses count as voters, so the
     argument covers `rf = 2 + witness`.
   - Availability is where joint consensus (§4.3; etcd's `ConfChangeV2`; CockroachDB's
     atomic replication changes) differs:

     | Replacement | Add, then remove | Joint consensus |
     |---|---|---|
     | `rf = 3`, a move within one zone, all alive | 4 voters, 2 in one zone. That zone's outage leaves 2 of 4, under the quorum of 3; the group stalls until the zone returns | `{A,B,C}` and `{A,B,D}`: A and B satisfy both majorities; available |
     | `rf = 3`, a dead voter replaced | A, B, and the new voter all required (3 of 4) | A and B required; the new voter may lag |
     | `rf = 2 + witness`, a dead data voter replaced | A, the new data voter, and W required; every commit in the window is on two data copies | A and W required; commits in the window are on one data copy |

   - Remove, then add passes through two voters with no tolerance, which REPLICATION rules
     out as a configuration (`REPLICATION.md:113-115`).
   - The window is two commits, since learners catch up first (D3(c)). A leader change
     between the two steps extends it until someone proposes the removal again, and D3
     does not say who. During the window the group breaks DISTRIBUTION's failure-domain
     row (`DISTRIBUTION.md:21`).
   - Ruling: keep single-server changes. The design states the window per configuration as
     above, the failure-domain exception with a bound on concurrent replacements per
     domain, that the host drives a replacement to completion across leader changes and
     crashes, and the measurement that brings joint consensus in: Track D's zone-outage
     simulation showing more unavailable groups than a stated bound.

3. **D4: self-removal, reads, and removed peers.**
   - A self-removing leader whose target is unreachable "aborts after one election
     timeout, when proposals resume" (`track-c.md:100-101`). If it resumes, it keeps
     sending to its former followers, D7's guard then refuses every other candidate, and
     the group keeps a leader outside its membership. The dissertation steps it down once
     the removal commits (§4.2.2). It steps down at commit, transfer or not.
   - Reads seed their acknowledgements with the leader (`core.h:417`) and compare a count
     to `quorum()` (`core.h:211, 667`). A removed leader's own acknowledgement, and one
     from a voter removed after the read was admitted, do not count. Reads count
     acknowledgements from voters of the membership in force when the read becomes ready.
   - A removed voter learns of its removal only from the entry itself. The leader keeps
     replicating to it until the entry commits, then drops its flight and releases any
     snapshot to it with a new `SnapshotReleaseReason`, so the host unpins the source.

4. **D5: admission from any authenticated peer of the group.**
   - Safe under the fault model. Raft's safety does not depend on whether a sender is in
     the receiver's configuration; servers accept appends and grant votes without
     consulting it (§4.1), and terms and the log comparison decide. Today's check
     (`core.h:293, 427`) is defense in depth.
   - Cost: a vote granted to a non-member spends the voter's ballot for that term, which
     pre-vote limits; the transport admits former and future members, not only current
     voters.
   - "The transport binds the group identity" (`track-c.md:85`) holds for today's
     per-group connections (`hello.h:48-53`) and fails under D16, where one connection
     carries every group. A frame's group id then selects the group, and the receiver
     admits it only if it hosts that group. Group identities are never reused across a
     group's deletion and re-creation, or a stale replica can join the new group.
   - The join record is right. A joining node persists it before acknowledging anything
     and denies vote and pre-vote requests; an entry naming it a learner changes neither.

5. **D5: the new-incarnation rule needs ids defined.**
   - The rule is right, and its reason is wider than double voting: a node that lost its
     log can also forget entries it acknowledged, which loses committed entries (§3.8).
   - "Id" is undefined. Today it is the authenticated node id (`types.h:133`), with one
     pin per node (`tls.h:45-66`). If member ids are node ids, a new incarnation needs a
     new node identity and certificate, and every group on that node changes at once. If
     they are per-group replica ids, frames name the source and destination replica: in an
     add-then-remove on one node, the old and the new incarnation are both voters there,
     and a candidate that maps node ids to members can count one node's grant for either.
   - Member ids are per-group incarnation ids bound to a node identity; frames carry them;
     a node answers only for incarnations it holds; and the design states the order of a
     same-node replacement, or forbids one.

6. **D6: `TimeoutNow` copies and delays.**
   - A flag that bypasses the guard is needed (§3.10, §4.2.3). A late or duplicated
     `TimeoutNow` still starts elections the leader did not ask for. A copy that arrives
     after the transfer aborted, in the same term, elects the target against the leader's
     decision. A copy that reaches a voter removed in the meantime, which has not yet seen
     its removal, bypasses the guard and raises a healthy group's term, which T3 forbids.
   - `TimeoutNow` carries the leader's term, its last index, and a transfer token. The
     target acts only as that leader's follower in that term, with its log at that index,
     once per token, and only while it is a voter in its own membership. Learners and
     witnesses are never targets. T3 delivers duplicated and delayed copies.

7. **D7: pre-vote and the guard need CheckQuorum.** (`probe_asymmetric_leader.cc`)
   - A leader that can send but not receive keeps its followers in contact, so the guard
     refuses every candidate while nothing commits. Today's core already stalls this way:
     with every message to the leader lost, it stays leader in term 1 for 3,000 ms (20
     election timeouts), its followers never campaign, and a write never commits. A leader
     steps down only on a higher term (`core.h:432-434`).
   - The dissertation steps a leader down after an election timeout without a successful
     round to a majority (§6.2). Howard and Abraham (2020) show pre-vote without that
     check failing to recover from partial network faults; etcd ships both.
   - A leader steps down when its voters have not answered within an election timeout. A
     pre-vote never changes the receiver's term; a refusal carrying a higher term updates
     the pre-candidate's.
   - D7 does not say whether fixed configurations use pre-vote. If they do, T1's
     byte-identical wire fails (finding 21).

8. **D7 with D12: quiescence turns the guard off.**
   - A quiescent group's followers have not heard from the leader within `election_min`,
     so they grant any pre-vote with an up-to-date log. "Any proposal, read, or message
     wakes the group" (`track-c.md:171`), and a quiescent leader sends nothing, so a woken
     follower times out and wins. Three things then elect a new leader in a healthy idle
     group: a stray message from a removed or partitioned node; a follower's restart,
     which starts its timer and hears nothing (a node restart does it for each of its
     follower groups, about 667 of 1,000 at `rf = 3`); and a false suspicion from gossip.
   - The opposite failures: a leader whose group fences on a live node sends nothing while
     gossip reports the node alive, so its followers never wake (under D11 one journal
     fault fences every group on that core); and followers cut off from a live leader that
     gossip still reaches through indirect probes stay quiescent too.
   - Gossip is Track D2, which waits on the membership-transport decision
     (`PLAN.md:383-385, 400-402`). C2's prototype and Phase 3R run before it exists.
   - A woken follower asks the leader to resume before its own timer runs, and campaigns
     only when the leader does not answer within an election timeout. A quiescent follower
     grants a pre-vote only after the same check fails. A leader that fences, steps down,
     or transfers tells its followers. A restarting replica asks before it campaigns.
     Until Track D2 lands, the failure detector is the node-pair connection's keepalive,
     and gossip later accelerates it. T3 covers each case.

9. **D8: learners.** Sound. Quiescence counts voters only (`track-c.md:169-170`), so a
   lagging learner in an idle group receives nothing. The leader keeps replicating to
   unmatched learners while quiescent, or quiescence requires every member matched. A
   learner that never catches up is finding 17's.

10. **D9: the witness.**
    - The election and the hand-off are right: the witness wins only with the most current
      log, ships it, and transfers. The retention rule keeps every entry a lagging data
      voter lacks.
    - The stated residual (`track-c.md:130-132`) cannot arise under that rule: a data
      voter cannot "lag past what the witness retained" while the witness truncates only
      through its matched index. It arises in three cases the design does not name: the
      witness's log reaches its limit; the witness was replaced by a new incarnation while
      a data voter lagged; or the lagging data voter is a new incarnation that must
      install.
    - At the limit, today's follower capacity check refuses appends (`core.h:503-518`), so
      commits stall rather than truncate. The design makes that the rule: truncating would
      leave committed entries on one data copy and break REPLICATION's guarantee for `rf =
      2 + witness` (`REPLICATION.md:8-12, 111`). "Down" in the residual means crashed; a
      lost disk on the only current data voter is a second failure.
    - A `2 + witness` group cannot be founded. D2 makes every founding member a voter
      (`track-c.md:50-51`), and building one up from fewer members passes through a
      two-voter group, which REPLICATION rules out (`REPLICATION.md:113-115`). The initial
      membership carries roles.
    - The witness's truncation point is a new field in appends (`track-c.md:129-130`), a
      wire change for finding 21. The design allows one witness per group, in `rf = 2 +
      witness`, or states the other mixes it allows.

### Repair (C3)

11. **D10 cannot reuse `InstallPrepared` as it is.** (`probe_install_side_effects.cc`)
    - On both sides of the commit index the path adopts the request's term
      (`core.h:298-299`) and follows the sender as its leader (`core.h:301, 317`); above
      it, it also sets the commit index to the boundary (`core.h:308`). A leader receiving
      node 2's image at its own commit index becomes node 2's follower in the same term
      and rejects proposals; a follower receiving node 3's image records node 3 as its
      leader.
    - The install branch (`core.h:307-321`) taken with a repair flag and `b < commit`
      lowers the commit index and moves leadership to whichever peer supplied the image.
    - Repair is its own event. It changes no hard state, role, leader, or commit index;
      any member may supply the image; and it raises `base_index` to `b` only when `b >
      base_index`, keeping the suffix.

12. **D10: what a rewind meets.**
    - A delivery in flight. `Applied{x}` passes when `x` equals `delivered_through_`
      (`core.h:383-390`). With `a + 1 … x` delivered and a rewind to `b < a`, that batch's
      completion sets `applied_` to `x`, and entries `b + 1 … a` are never applied to the
      restored image. Install refuses the same race by fencing (`core.h:324`). Repair is
      refused while a delivery is outstanding or the core is busy, and the Worker's
      `delivered_` and `application_` (`worker.h:194-201`) reset with it.
    - A capture. `LocalCheckpoint` checks only `through <= applied_` (`core.h:357`). A
      capture of the damaged state taken before the repair, submitted once `applied_`
      passes its `through` again, publishes the damage as the new base and truncates the
      log that could repair it. Repair advances an application epoch, captures carry it,
      and the core rejects older ones.
    - Reads. Linearizable reads wait as stated. A follower's `nearest` and `session` reads
      come from its visible snapshot, which moves back to `b`; the host serves none until
      `applied` returns to its visible position before the repair, as REPLICATION's
      fencing already implies (`REPLICATION.md:178-179`). A leader that must repair
      transfers leadership first, unless it is the only current voter.
    - Redelivered entries reproduce verdicts the leader already reported; the host does
      not report them again.
    - Rewinding and redelivering is safe once these hold: the image at `b` is the state of
      a committed prefix, apply is deterministic (REPLICATION's contract), and `b + 1 …
      commit` comes from the voter's own durable log. The range `[base_index, commit]`
      with the term check is right: the boundary is committed, and Log Matching makes both
      prefixes equal. Rejecting a boundary below `base_index` is right.

### Multi-group (C2)

13. **D11: the journal it assumes does not exist.** (`probe_append_barriers.cc`)
    - One 1 KiB `append_batch` issues 2 file syncs, 1 rename, and 1 directory sync, and
      creates a temporary manifest, in each of 100 appends. A batch is durable only once
      the manifest names it, because recovery discards bytes past the manifest's frontier
      (`journal.h:81, 289-293`). One barrier per turn needs a new append protocol whose
      recovery scans past the frontier and keeps a contiguous, checksummed tail. That is
      REPLICATION's two-barrier frontier design (`REPLICATION.md:153-157`), which the
      design drops from G7 (`track-c.md:11`).
    - The journal truncates only by publishing a checkpoint that covers its exact current
      sequence, as a new generation with at most 8 dependencies (`journal.h:368-374, 410`;
      README line 34). Per-group checkpoints and "reclaims a segment once no group needs
      it" (`track-c.md:160-162`) need segments, per-group checkpoint records, per-group
      sequence numbers (recovery checks one contiguous sequence today, `storage.h:163`),
      and a per-group low-water mark. One idle or lagging group pins every segment after
      its base; TiKV's Raft Engine rewrites such groups' live entries forward so old files
      can go. "A group tag" in the record framing (`track-c.md:254`) understates the
      change.
    - Corruption. Recovery stops at the first bad record (`journal.h:262-287`). In a
      shared journal, every group with a record after that point loses its tail, and by D5
      each one rejoins as a new incarnation and copies its data again: one bad sector
      costs up to every group on the core. The design states that blast radius, or uses a
      format that resynchronizes at block boundaries, so a bad block costs only the groups
      with records in it.
    - Ruling: one journal per core is the right direction at 1,000 groups (fewer files and
      barriers; CockroachDB and TiKV share one log across groups). Its design lands before
      journal code, and T6 measures the per-group alternative too: Linux filesystems
      coalesce concurrent fsyncs into one log commit, and on macOS one `F_FULLFSYNC`
      covers earlier device writes (journal README line 113), so per-group journals with
      batched barriers may meet the budget with less new format.

14. **Memory and storage per group at 1,000 groups.** (`probe_group_memory.cc`)
    - An idle three-voter group holds 136,168 heap bytes today (Worker, Core, Store, and
      SnapshotChannel), the same per group at 1, 10, and 100 groups. 131,376 of them are
      the snapshot channel's two input buffers, allocated per peer at construction
      (`snapshot_channel.h:41-42, 165`; `snapshot_transfer.h:37`). At 1,000 groups that is
      136 MB before any log entry.
    - The retained log lives in memory as an AVL tree of entries with their payloads
      (`retained_log.h:35-40`), up to `log_bytes` per group: 64 MiB by default
      (`types.h:79`), 64 GiB for 1,000 groups. Recovering a shared journal rebuilds all of
      it.
    - Each Worker owns a Store with its own admission pools and checkpoint replacement
      reservation (`worker.h:87-90, 206-215`), so storage reservations grow with the group
      count too.
    - D11–D14 state no memory, and T6 measures none. DISTRIBUTION requires the memory per
      idle index stated (`DISTRIBUTION.md:29`), and Goal 5 makes memory a must-win row.
    - The design states memory per idle and per active group at 1,000 groups; bounds the
      retained log by a node-wide budget, or keeps it in the journal behind an in-memory
      index; allocates snapshot buffers per transfer; and admits storage from one node
      pool on `ledger`'s capacity and permit core (`PLAN.md:283-286`) instead of 1,000
      worst-case reservations.

15. **D12: the coalesced heartbeat's commit is unsafe as written.**
    - `(group, term, commit)` (`track-c.md:166`) carries no previous index or term, so the
      follower cannot check Log Matching. Today a follower raises its commit index only to
      `min(request.commit, previous + entries)` after that check (`core.h:480-486,
      524-525`). A follower holding a divergent, uncommitted suffix from an older term
      that adopts the leader's commit from a heartbeat applies an entry no leader
      committed. etcd's raft sends `min(matched, committed)` in heartbeats for this
      reason. The frame carries the commit clamped to that follower's matched index in the
      current term.
    - The design does not say whether a coalesced reply acknowledges a read probe
      (`core.h:209-214, 540-542`). If it does, the frame carries per-group probe tokens;
      if not, reads keep using appends.

16. **D12: what coalescing saves depends on placement.**
    - At `rf = 3`, a node with 1,000 replicas leads about 333 groups, with 666 follower
      replicas, and follows 667. Placed at random over 100,000 nodes, they reach about
      1,325 distinct peers, so a node pair shares about one group and coalescing removes
      almost nothing: at the 20 ms heartbeat (`types.h:88`) with every group
      non-quiescent, about 133,000 frames per second per node, coalesced or not. With 16
      partner nodes per node (copysets), at most 3,200.
    - "10^3 groups per node cost about what one does at rest" (`DISTRIBUTION.md:58-60`)
      therefore rests on quiescence and on a placement property that Track D3 must
      provide. The design states the cost as a function of distinct peers per node, the
      bound placement must meet, and the connections per node that follow (finding 20).

17. **D13: snapshots cannot catch up a large shard.**
    - The sender keeps one 64 KiB chunk in flight and waits for its credit
      (`snapshot_channel.h:140-144`), so a transfer moves 64 KiB per round trip: 6.55 MB/s
      at 10 ms. Both ends give up after 60 s in total, not 60 s idle (`types.h:90`;
      `core.h:345, 597`; `snapshot_channel.h:39, 113`). One attempt moves at most 375 MiB
      at 10 ms and 3.66 GiB at 1 ms. A 10 GiB shard needs 27 minutes at 10 ms, so it never
      installs and its learner never catches up.
    - While a snapshot is sending, the leader cannot checkpoint (`core.h:361-363`; the
      test asserts it, `consensus_test.cc:1077-1078`), so its in-memory log grows until
      proposals fail with `LogFull` (`core.h:398`). At 1 MB/s of writes the 64 MiB default
      fills in 67 s.
    - A queued transfer times out as well: `AwaitSource` runs on the same clock from the
      request (`core.h:597, 629`), so a node-wide limit (`track-c.md:179-180`) turns into
      repeated releases and requests.
    - The transfer keeps a window of chunks in flight within the bulk lane's credit; its
      timeout measures inactivity; retention during a transfer costs disk, not memory
      (finding 14), so writes do not stall; a queued request does not time out; and the
      design states the catch-up condition (the image's bytes over the transfer rate,
      against the retained log over the write rate) and what the host does when it fails.

18. **D14 and T6 measure the best case.**
    - Three nodes in one process put every group's followers on the same two nodes
      (`track-c.md:184-186`), so coalescing is perfect, which finding 16 shows it is not
      at scale.
    - The simulated nodes share one device, so the measured node's barriers and latency
      include its peers'.
    - Not measured: memory per group; journal recovery time at 1,000 groups; reclamation
      with one lagging group; a node returning after downtime with lagging groups; a heavy
      group's effect on a light group's p99 in the shared turn; strata's own commit and
      checkpoint barriers (Phase 3's two directory syncs per commit) in the node's budget;
      and the per-group journal alternative.
    - Platforms: Linux and macOS. FreeBSD's filesystems order and coalesce barriers
      differently, and LIBRARY_BOUNDARIES' first gate covers all three
      (`LIBRARY_BOUNDARIES.md:198`).

### Pipelining and transport (C4)

19. **D15: pipelining.**
    - Per-flight tokens, probes counted for the flight that carried them, and `matched`
      never moving back keep today's correlation (`core.h:531-538`). Keepalives stay
      outside the window: their responses are ignored (`core.h:536`) and they carry no
      probe (`core.h:618-622`).
    - Throughput. A follower persists one batch at a time and drops messages while busy
      (`core.h:243-248, 427`), and the transport holds one decoded input per connection
      until it is admitted (`peers.h:355-375`). A follower therefore takes one barrier per
      append RPC: pipelining hides the round trip, not the barrier. The follower merges a
      group's queued appends into one batch, or the design states the throughput bound.
    - `send` drops a frame once four are queued on a connection (`peers.h:33, 121-126`),
      so a deeper window, or one shared by many groups, loses frames at the sender. D16's
      queues replace that bound with per-group bounds in frames and bytes.
    - The default pipeline for membership groups is not stated, and T7 compares against
      "the default".

20. **D16: one busy group blocks the connection.**
    - Send-side round-robin is right. On the receive side, the reader decodes one frame,
      holds it, and reads nothing more until the worker admits it; a `Busy` worker ends
      the admission pass (`peers.h:151-189, 355-375`). With every group on one connection,
      a group waiting on its barrier stalls every other group on it. Per-connection credit
      alone does the same: one group's buffered frames consume the window. HTTP/2 and QUIC
      keep per-stream windows beside the connection window for this reason (RFC 9113 §5.2;
      RFC 9000 §4).
    - The receiver keeps per-group queues with per-group credit, or drops a busy group's
      frames, which Raft tolerates. T7 slows a follower's receive path, not only its link.
    - Lanes. "Large appends" on the bulk lane and small ones on the control lane
      (`track-c.md:204-206`) reorder one group's appends across two connections, which
      turns pipelining into rejections. A group's appends stay on one lane.
    - Authentication. Per-node pins and fixed addresses (`tls.h:45-66`, `peers.h:64-71`)
      do not follow a membership. The design names where a node-pair connection learns a
      peer's pin (the union of its groups' membership entries, or a cluster authority),
      and the peer set changes without a restart. Connections per node are twice the
      distinct peers: about 2,650 TLS connections under random placement (finding 16).
    - The snapshot channel has its own per-chunk credit (`snapshot_channel.h:140-144`);
      with D16 that is two flow-control mechanisms on one lane. One remains.

### Compatibility (D2, T1)

21. **D2 against T1, Detent, and the transport.**
    - The initialization record stays compatible. It is immutable (`storage.h:55-63,
      184`), a founding member's record is today's, and a joining member writes a new
      record type. A storage batch's version is per batch (`storage.h:41, 191`). Today's
      reader rejects an unknown entry kind (`storage.h:205`) as corruption, so the new
      kind rides a new batch magic. The checkpoint bundle embeds the initialization record
      (`checkpoint.h:77-79`), so a joined member's bundle needs a new version too.
    - The fingerprint clause contradicts T1. `DTPROF01` hashes the sorted voters and every
      consensus limit (`profile.h:14-30`). Dropping the voters changes a fixed
      configuration's hello, and Detent documents that a different quorum fails admission
      (`AUTHORITY_TRANSPORT.md:9`). New limits (`members`, `pipeline`, `window_bytes`)
      change the hash as well.
    - Other wire changes: `PreVote`, `TimeoutNow`, the transfer flag, coalesced frames and
      replies, window updates, the witness's truncation field, group ids in frames, and
      the snapshot descriptor with a membership. `DTC1` rejects unknown tags and entry
      kinds (`codec.h:61-62, 136-138, 171-172`).
    - A per-group fingerprint cannot exist on a connection that carries every group (D16).
    - Limits in the fingerprint conflict with Detent's requirement that limits be raisable
      online at a log index (`SCALE_TARGETS.md:35, 59`), and with rolling changes: members
      with different limits cannot connect.
    - LIBRARY_BOUNDARIES allows format changes as their own versioned commits, since
      Detent is greenfield (`LIBRARY_BOUNDARIES.md:25, 187, 201`). Two wire paths, one for
      fixed groups and one for membership groups, contradict one implementation per
      mechanism.
    - Detent asks for voter replacement in `consensus` (`SCALE_TARGETS.md:34, 181`): C1 is
      Detent's item too, not a change Detent only tolerates.

### Gate rows

22. **T2's model cannot see what it must.** (`probe_harness_crash_points.cc`)
    - The test's `Host` completes every `Persist` inside the step that emits it
      (`consensus_test.cc:97-99`), and restarts fall between steps
      (`consensus_test.cc:750`), so no restart finds a pending batch: 0 of 24,128 restarts
      over 2,000 seeds of 1,000 steps.
    - A mutant core that sends vote grants and append acknowledgements before their batch
      is durable passes that model over 2,000,000 steps with 0 failures. A model whose
      restarts keep or lose a pending batch at random catches it in 5 of 2,000 seeds (209
      failed prefix checks), and in none of the first 200. The real core passes both
      models with 0 failures.
    - "Every committed membership a single-voter step from the previous one"
      (`track-c.md:225`) misses the 2015 bug: its two conflicting configurations are each
      one step from the first. Leader completeness, checked at every election against a
      global record of committed entries, catches it.
    - A ballot oracle (one grant per voter per term, across restarts) and a durability
      oracle (every grant and successful acknowledgement matches the sender's durable
      state when sent) catch persistence-ordering bugs at their first occurrence instead
      of through a later divergence.
    - Clock skew is missing; Phase 3R's gate has it (`PLAN.md:424-426`). Safety does not
      depend on clocks, since there are no leases (`track-c.md:25`), but D7's guard and
      D12's timers read local time.
    - T2's harness is C5's first version, so Track D1 and Phase 3R grow one harness
      (`PLAN.md:380-382, 400`).

23. **Failure modes and where they stand.**

    | Failure mode | Where | Status |
    |---|---|---|
    | A crash at each persistence point of a membership change | T2 | The model never crashes with a batch pending (finding 22); a crash between the promotion and the removal leaves four voters until the host acts (finding 2) |
    | A configuration entry in a replaced suffix | D1, T2 | The rule is right; random schedules rarely produce it, so T2 forces it and counts its occurrences per seed |
    | A leader removed while a read or snapshot is in flight | D4 | Reads count its own acknowledgement; flights to removed peers are not released (finding 3) |
    | Clock skew | T2, T3 | Not in either row (finding 22) |
    | Duplicated or delayed `TimeoutNow` | D6, T3 | Unbounded elections (finding 6) |
    | A learner that never catches up | D8, D13, T4 | Never installs past 375 MiB at 10 ms; quiescence starves it (findings 9, 17) |
    | Corruption in a shared journal | D11 | Blast radius unstated (finding 13) |
    | A leader that can send but not receive | D7, T3 | Stalls without CheckQuorum (finding 7) |
    | A fenced group on a live node, while quiescent | D12, T3 | Followers never wake (finding 8) |

24. **Targets and procedures.**

    | Row | Target | Procedure | Missing |
    |---|---|---|---|
    | T1 | Byte-identical records and wire for fixed configurations | Fixtures; Detent's suite | Fixtures generated at `b7073bb` by a named deterministic schedule (the repository has none); Detent's commit, options, clean clone, and serial run (`LIBRARY_BOUNDARIES.md:199`); macOS, Linux, and FreeBSD (`:198`); restated by R1 |
    | T2 | 10^6 steps over 1,000 seeds | Randomized simulation | Finding 22 |
    | T3 | "Within a bound of election timeouts" | After healing | The bound in milliseconds from a stated timing profile (Detent's 100–200 ms elections campaigned continuously over real storage, `AUTHORITY_TRANSPORT.md:37`); CheckQuorum, quiescence, and `TimeoutNow` cases (findings 6–8) |
    | T4 | Functional | Not stated | The witness at its limit with a data voter lagging (commits stall, nothing truncates); a witness replaced while a data voter lags; founding `2 + witness`; a learner installing a shard larger than one attempt moves today, under writes, at 10 ms |
    | T5 | Functional | Not stated | Repair with a delivery outstanding, a capture pending, reads pending on a leader, and a late `Applied`; "equals an undamaged replica's" defined as the applied sequence or the digest |
    | T6 | None: every figure is reported, none passes or fails | D14 | Targets (at most 1 barrier per core per turn at every group count; 0 barriers and a stated frame rate at 1,000 quiescent groups; a light group's p99 within a stated ratio of one group's beside a heavy neighbor; memory per group); what counts as a barrier; finding 18's gaps; BENCHMARKS' trial rules |
    | T7 | "A stated bound" | `netem` | The bound; the default pipeline; real storage on the follower; a slowed receive path; snapshot throughput |
    | T8 | One CPU-hour per new decoder | Fuzzing, sanitizers, coverage | Decoders: the join record, the checkpoint bundle with a membership, the snapshot descriptor, `PreVote`, `TimeoutNow`, and the shared journal's segment and checkpoint records; a fixture per format change (`PLAN.md:111`); a scale statement per component (`PLAN.md:115-116`); Goal 5's audit of the changed libraries (`PLAN.md:286-287`) |

### Plan and documents

25. **Consistency.**
    - REPLICATION closes G1, G3, G4, and G6 before Phase 3R (`REPLICATION.md:203`). PLAN
      starts Phase 3R after C1–C4 (`PLAN.md:415`), which adds G2 and G5, and puts G6
      inside Phase 3R (`PLAN.md:419`), as the design does (`track-c.md:10-11`).
    - G7 is "Linux NVMe measurement, fewer barriers per append, and defaults retuned for
      bulk ingest" (`REPLICATION.md:200-202`). The design keeps the measurement only
      (`track-c.md:11`), and D11 depends on the fewer barriers (finding 13).
    - PLAN starts Track C after coordination with the Detent work (`PLAN.md:268-269`), and
      DISTRIBUTION routes transport changes through it first (`DISTRIBUTION.md:129-130`).
      The design records no coordination for D2, D11, or D16.

## Unaffected

These stand as written:

- The starting point (`track-c.md:15-29`), checked against `b7073bb`.
- Membership as an entry kind, the newest entry in force, and rollback (D1).
- A fixed configuration as the initial membership (D2, its first bullet).
- D3's rules (a)–(c) and one quorum rule.
- Learners (D8), and the witness's election, hand-off, and retention rule (D9).
- Repair keeping the log and hard state, its boundary range, and loss of state as a new
  incarnation (D10).
- Group commit across groups, per-group checkpoints, forced checkpoints for laggards, and
  a node-wide transfer limit (D11, D13), as directions.
- Optimistic `next`, one flight after a failure, and `pipeline = 1` for fixed
  configurations (D15).
- One connection per node pair and lane, a group id in every frame, and send-side
  round-robin (D16).

## Ruling

**APPROVE WITH REQUIRED CHANGES.** Each required change lands in the design, and the
revision check confirms it, before the code it governs:

- Before any C1 code: R1–R6 and R13, with R14's rows for T1–T4.
- Before any C3 code: R7, with R14's row for T5.
- Before the D14 prototype: R5's cost statement and R14's row for T6. Before any journal
  format code: R8 and R9.
- Before any C4 code: R10–R12, with R14's row for T7.
- R15 with the revision.

A revision check covers R1–R15 and T1–T8. Phase 3R's start rule is unchanged: C1–C4 pass
first.

## Required changes

1. **R1. Wire and formats (D2, T1; finding 21).** The design picks one: (a) a single
   versioned wire format that Detent adopts in a coordinated commit, with T1 restated as
   storage records byte-identical plus the listed wire changes; or (b) fixed
   configurations emit today's bytes from the same code, as a preset. The fingerprint
   covers the wire version and the node's application profile, not per-group voters.
   Consensus limits move into the replicated configuration, applied at a log index, or the
   design states that a group's limits change only by restarting it. The design lists
   every format and wire change (the batch magic, the join record, the checkpoint bundle,
   the snapshot descriptor, `PreVote`, `TimeoutNow`, the transfer flag, coalesced frames,
   window updates, the witness's field, group ids) with its version tag, its fixture, and
   a test that the previous reader rejects it. It states whether fixed configurations use
   pre-vote and CheckQuorum.
2. **R2. Members and ids (D1, D5; findings 1, 4, 5).** A member is a per-group incarnation
   id, a node identity, a role, and a transport identity (address and pin), or the design
   names the directory that holds the last two. Frames name the source and destination
   incarnation; a node answers only for incarnations it holds; a same-node replacement has
   a stated order or is forbidden. The base membership lives in `RecoveredState`, the
   checkpoint bundle, the install, and a versioned snapshot descriptor, not in
   `SnapshotKey`. Group identities are never reused. The design states the membership-mode
   rule at each of the 21 voter-list sites in finding 1, including which ids a recovered
   ballot may name.
3. **R3. Changes and removal (D3, D4; findings 2, 3).** The design states, per
   configuration (`rf = 3`, `rf = 5`, `rf = 2 + witness`), what a replacement's window
   tolerates; the failure-domain exception to `DISTRIBUTION.md:21` and a bound on
   concurrent replacements per domain; that the host completes a replacement across leader
   changes and crashes; and the zone-outage result that would bring joint consensus in. A
   removed leader steps down when the removal commits, whether or not its transfer
   succeeds. Reads count acknowledgements only from voters of the membership in force when
   they become ready. A removed voter receives appends until its removal commits, and its
   flights and snapshots are then released.
4. **R4. Elections (D6, D7; findings 6, 7).** CheckQuorum: a leader steps down when its
   voters have not answered within an election timeout. A pre-vote never changes the
   receiver's term. `TimeoutNow` carries the leader's term, last index, and a transfer
   token; the target acts once per token, as that leader's follower in that term, at that
   index, while a voter in its own membership; learners and witnesses are never targets.
   The design states the timing profile T3's bounds derive from.
5. **R5. Quiescence and coalescing (D12; findings 8, 9, 15, 16).** The heartbeat carries
   the commit clamped to the follower's matched index. The design states the wake and
   campaign rules of finding 8, notification by a leader that fences, steps down, or
   transfers, the restart path, the failure detector before Track D2 (the node-pair
   keepalive), whether coalesced replies acknowledge read probes, and that quiescence
   keeps replicating to unmatched learners. It states frames, bytes, and connections per
   node per second as functions of distinct peers per node, at rest and under load, and
   the bound on peers per node that Track D3's placement must meet.
6. **R6. Witness (D9; finding 10).** The residual is restated as the three cases of
   finding
   10. At the witness's retained-log limit, commits stall and nothing truncates. The
       initial membership carries roles, so `2 + witness` can be founded. The design
       states which witness mixes it allows.
7. **R7. Repair (D10; findings 11, 12).** Repair is its own event that changes no hard
   state, role, leader, or commit index, and accepts an image from any member. It is
   refused while the core is busy or a delivery is outstanding; it resets the Worker's
   delivery state; it advances an application epoch that captures carry, and older
   captures are rejected. The host verifies the image whole before activation, serves no
   reads until `applied` returns to the pre-repair visible position, and does not report
   redelivered verdicts again. A leader transfers before repairing unless it is the only
   current voter.
8. **R8. The shared journal (D11; finding 13).** Before journal code, the design
   specifies: the append protocol and its barriers per turn (the two-barrier frontier, or
   one), with recovery of a torn tail; segments; per-group checkpoint records and sequence
   numbers; the per-group low-water mark; reclamation in bounded slices, with lagging
   groups' entries rewritten forward or a forced checkpoint, and the space bound that
   results; recovery time and memory at 1,000 groups; whether a group can move between
   cores; and the corruption policy with its blast radius, or a format that resynchronizes
   at block boundaries.
9. **R9. Memory and storage per group (C2; findings 14, 17).** The design states memory
   per idle and per active group at 1,000 groups, with today's 136,168 bytes per idle
   group as the baseline. The retained log is bounded by a node-wide budget or kept in the
   journal behind an in-memory index, so retention during a transfer costs disk. Snapshot
   buffers are allocated per transfer. Storage admission is one node pool on `ledger`'s
   capacity and permit core.
10. **R10. Snapshots (D13; finding 17).** The transfer keeps a window of chunks in flight
    within the bulk lane's credit, and its timeouts measure inactivity. A queued request
    does not time out. The design states the catch-up condition and the host's action when
    it fails. One flow-control mechanism carries snapshots (finding 20).
11. **R11. Pipelining (D15; finding 19).** The default pipeline for membership groups; a
    follower merges a group's queued appends into one batch, or the design states the
    throughput bound; keepalives stay outside the window and never count for probes; the
    sender's per-group queue bounds in frames and bytes; and the window's memory per node.
12. **R12. Multiplexing (D16; findings 4, 20).** Per-group receive queues with per-group
    credit, or the receiver drops a busy group's frames. A group's appends stay on one
    lane. The design states how a node-pair connection authenticates a peer and learns its
    pin, that the peer set follows membership without a restart, and connections and
    memory per node as functions of distinct peers.
13. **R13. T2's harness (findings 22, 23).** Restarts keep or lose pending batches at
    random; the oracles add leader completeness at every election, one grant per voter per
    term across restarts, and durability of every grant and acknowledgement when sent;
    clock rates vary per node. Rare schedules are forced and counted per seed: a
    configuration entry in a replaced suffix, a crash between promotion and removal, a
    removed leader with a read and a snapshot in flight, and a delayed `TimeoutNow`. The
    harness detects each of these seeded faults within the seed budget, with the seeds per
    fault recorded: D3(a) off, D1's rollback off, a self-removed leader counting itself,
    sends before durability, an unclamped heartbeat commit, and repair below `base_index`.
    The harness is C5's first version.
14. **R14. Targets and procedures (findings 18, 24).** T1–T8 each have the target and the
    procedure that finding 24 lists as missing. T6 varies distinct peers per node (2, 16,
    and at least 1,000 simulated), keeps the measured node's storage to itself, adds
    memory per group, recovery time, reclamation with a lagging group, a returning node, a
    heavy and a light group on one core, strata's commit and checkpoint barriers, and the
    per-group journal alternative, and runs on Linux, macOS, and FreeBSD under BENCHMARKS'
    trial rules. T8 lists every new decoder and fixture, a scale statement per component,
    and where Goal 5's audit of the changed libraries lands.
15. **R15. Documents and coordination (finding 25).** REPLICATION's line on which gaps
    close before Phase 3R agrees with PLAN and the design. The design carries G7 whole, or
    PLAN and REPLICATION move its parts. The design records the Detent coordination for
    D2, D11, and D16: what changed in the shared libraries, and Detent's agreement or its
    commits. DISTRIBUTION's claim about 1,000 groups at rest names its conditions:
    quiescence and bounded peers per node.
