# Durability and replication modes, part A: the consistency spectrum and replication

- Status: **designing (revision 2, ready for re-check)**, 2026-10-19. Part A of two. This file
  decides the acknowledgement and read levels, the commit split, replication by segments, recovery
  after correlated failures, the presets, and what each costs. Part B (memory residency and
  retention) is outlined at the end and designed after A's re-check (Q7, decided).
- Request and scope: the owner, 2026-10-18 (`docs/status/durability-modes.md`): split "commit",
  relax persistence per index where replicas hold the data in memory, keep stronger consistency
  where an index needs it, find the indexing win, and let every system on strata choose its side.
- Amends `docs/REPLICATION.md` (guarantee, levels, write path, replication, Xapiand mapping) and one
  row of `docs/DISTRIBUTION.md`; extends Track C's D5, D10, D11, D12, D13, D15, and D18
  (`docs/design/track-c.md`); claims wire fields for version 3 through the peer transport's version
  range (Q8; `design/peer-transport` at `38a6c6f`, D6, D7, D9), never version 2; takes
  small-indexes' D3-A as its checkpoint batcher (`perf/small-indexes` at `7c23560`); builds on
  format 13's reservations (on `main` since `026ee10`); changes the simulator's fault model and
  checker modes (`design/simulator` at `27cfb30`); supersedes rocksdb-survey's Q3 (Q5); takes
  `research/no-stalls`'s recommendation for D9's cadence and the background tiers (`467b5b3`).
- Read at: strata `main` `026ee10` (merged here at `457528b`; every strata `file:line` below is at
  it, and `format-13.md` there is byte-identical to `phase4/format13-on-main` `43ee403`, where
  revision 1 read it); consensus `61149a2`; journal `cb66f3d` (consensus's pin; journal `main`
  `dbf753f` changes only `admission.h` and `resources.h`); completion `5251cf9` (strata's pin);
  Xapiand `master` `7e8cccb7d`, its MIT own code only (nothing under `src/xapian`, no
  `sortable_serialise`); the storage library `671eaac`; `research/no-stalls` `467b5b3`;
  `perf/small-indexes` `2e9949e` and `45985dc` (SI-R24's claim rule); threadpool `628679a` (strata's
  pin); Kronuz/cpp-libraries `1104f00`; Detent `f5a8a7a`.
- Reviews folded in: the architect's preliminary review (`docs/reviews/durability-modes-design.md`:
  REJECT for first steps 1, 3, 4, and 5; DM-R1 to DM-R18, C1 to C5) and the security review
  (`docs/reviews/durability-modes-security.md`: APPROVE WITH CONDITIONS; DMS-1 to DMS-9, C1 to C7);
  the owner's answers of 2026-10-19 (Q1 to Q6 as recommended, Q3 with DM-R1's fix, Q8); and
  small-indexes' re-check (`docs/reviews/small-indexes-design.md` on `perf/small-indexes` at
  `2e9949e`, its C3: SI-R19, SI-R20, SI-R24, SI-R25, SI-R26); then both re-checks of revision 1:
  the architect's (`docs/reviews/durability-modes-design.md`, "Re-check (revision 1)": REJECT for
  first steps 1 and 3; DM-R19 to DM-R23, C6 to C14) and the security reviewer's
  (`docs/reviews/durability-modes-security.md`, "Re-check (revision 1)": APPROVE WITH CONDITIONS;
  DMS-10 to DMS-13, C8 to C12), with their probes (`strata-wt/review-durability/.scratch/
  review-durability/r1/`, `strata-wt/review-durability-sec/.scratch/durability-sec-r1/`) rerun on
  this branch's model. What changed, per finding: "Changes in revision 2", then "Changes in
  revision 1".
- Measured here, as branch evidence for choices, never as gate rows (PLAN, "What waits"): T1's sync
  counts and T2's sizing on the development Mac (`0b407c9`, unchanged). Verified here by
  computation: D5 and D6 in an explicit-state model (`bench/durability/model/`, results in
  `bench/durability/results/model-*.out` and, for revision 2's restarts and memberships,
  `results/r2/`). Every other figure is the cited code's or document's, arithmetic
  (`bench/durability/arith.py`, `results/arith.out`), or a labeled estimate.

## Revision 2

1. **Forgetfulness survives every restart (DM-R19).** The open record names the newest epoch whose
   recovery found voters forgetful, and every hard state a forgetful voter writes carries a mark
   that only re-learning clears, so a same-boot restart, a failed barrier's restart, or a clean
   close and reboot finds a forgetful voter still forgetful. Revision 1's rule lost a `memory` write
   in 7 steps at three voters with one OS crash (`restart3`), and with a torn same-boot tail and no
   OS crash at all (`torn3`); with the mark, the model's searches with restarts and torn tails find
   nothing (D6's evidence).
2. **The recovery decision counts a majority of the candidate's membership (DM-R20).** That is
   f + 1 at every odd size, so steady-state groups are unchanged, and f + 2 in Track C's
   replacement windows, where f + 1 loses a `memory` write at rf = 5 in both halves of the window
   (the architect's `six`, and `grow6`, built here). D6's argument is restated for adjacent
   memberships; the price is a stall with exactly f + 1 forgetful voters in a window, which an
   operator's `manual` statement resolves.
3. **Segment mode's check, priced as it performs (DM-R21, DMS-11).** Each follower's samples are its
   secret, deletions ride their flush's sample, debt defers samples instead of skipping them, and
   merges are stated as checked for merge faults only. Q9 returns to the owner with each option's
   residual for one corrupt flush, and a third option, digest anchoring, which T2 prices.
4. **Creation and checkpoints (DMS-10, DM-R23).** Only a UUID's first `Create` creates; a duplicate
   applies as `exists`, and replay never wipes an index. Retention keys on the checkpoint's own
   manifest, the durable head, beside the in-memory state.
5. **Bounds (DMS-12, DMS-13).** D17's file list is bounded before it is buffered and its sizes are
   summed without overflow; D5's rewind floor is the leader's own arithmetic, one unsynced window,
   at most once per `recovery_timeout`.
6. **Precision (DM-R22),** and every strata citation re-pinned to `main` `026ee10`, where format 13
   landed.

## Changes in revision 2

| Finding | What changed | Where |
|---|---|---|
| DM-R19 | The open record names V, the newest epoch whose recovery found voters forgetful, durable before its epoch's first turn as Track C's condition 8 has it; the hard state gains a forgetful mark (one byte under `RFTBTCH3` and the new bundle magic, and in the group checkpoint record), set in every hard state a forgetful voter writes and cleared in the batch that completes re-learning; a group whose newest hard state predates V recovers forgetful, so every restart keeps forgetfulness, a clean close included; a recovery winner clears its own; T7 asserts the fix instead of the bug; T9's restart rows; T6's mutants; the model gains same-boot restarts and torn tails | D3, D4, D5, D16, T6, T7, T9, D6's evidence |
| DM-R20 | The decision counts ⌊n/2⌋ + 1 of the candidate's membership flagged; the exactness argument restated for a membership and its successor; the model statement says f for even memberships; the window stall priced, with `manual`'s statement able to admit f + 1; Q10 corrected; T3's `six`, `six_loss`, `grow6`, `grow6_loss`; T6's mutant; the model gains membership in the log | D2, D6, fault model, Q10, T3, T6, D6's evidence, "Changes to other documents" |
| DM-R21 | D18 and Q9 state option 2's residual as it performs (per corrupt flush 3.1% and 6.1% detected, permanent once merged, no backup), sample deletions, and offer digest anchoring as option 3; T2 prices option 3; T4's choosing and deletion mutants | D18, Q9, fault model, threat model, T2, T4, `arith.out` |
| DM-R22 item 1 | The memory window's cap binds every voter: past it a core answers at its barrier, a follower's replies and a leader's own match alike | D2, D14 |
| DM-R22 item 2 | The leader's own match in `advance_commit` (`core.h:1151`) is its written index for `commit_memory` and its synced index for `commit_durable` | D7 |
| DM-R22 item 3 | The leader takes `synced` from every reply of its term before the flight rule can drop it; replies need no order | D7 |
| DM-R22 item 4 | The cadence bracket counts the verifier: 1.165× at rf = 3, 0.803× at rf = 5 | D8, `arith.out` |
| DM-R22 item 5 | T2's margin rests on EVP's unmeasured rate (the follower's target fails below about 0.36 GB/s); T2 measures the rate first | D8, T2, `arith.out` |
| DM-R22 item 6 | Q9's costs name their base | Q9 |
| DM-R22 item 7 | D1 states small-indexes' claim rule word for word | D1 |
| DM-R23 | Retention and `manifest_files()` take the head's names from the checkpoint's manifest, the durable head, beside the in-memory state's files; T16's crash-after-reclaim case | D1, T16 |
| DMS-10 | Only a UUID's first `Create` creates; the leader refuses a duplicate for an index it holds, one in the log applies as `exists`, and replay opens at any checkpoint record of the UUID; T16's retry case | D1, D16, T16 |
| DMS-11 | Follower-secret samples, deferral with `verify_dropped` and `verify_lost`, merges stated as merge-fault-only, the leader's row re-priced, T4's choosing mutant | D14, D15, D18, threat model, T4 |
| DMS-12 | The list's size, count, and total checked from `Begin` before buffering, against `transfer_list_bytes` and the committed `Image`; the sum without overflow; T15's case | D15, D17, T15, `limits.md` |
| DMS-13 | The rewind floor is the larger of the reported `synced` and `matched` less one unsynced window by the leader's count; at most once per `recovery_timeout` per peer; T6's mutant reworded | D5, threat model, T6 |
| Architect C6 | The lead's: K3 should carry DM-R19's and DM-R20's mutants (the ledger's next steps) | Ledger |
| Architect C7 | The calibration row, as the lead asked | `docs/reviews/calibration.md` |
| Architect C8 to C12 | Steps 2, 4, 5, 6, 7, and 8 may start under their conditions; step 6 gains the new schedules and profiles; step 5's checkpoint code and `Create` replay have their rules | First steps, T3 |
| Architect C13, C14 | DM-R21 to DM-R23 in this revision; the model's limits stated where D6 cites it | As above; D6 |
| Security C8 to C11 | DMS-10 to DMS-13 above | As above |
| Security C12 | The compromised voter's row: the forged flag loses a write only after `recovery_timeout` with the write's holders unreachable, never when every voter answers | Threat model |
| `main` `026ee10` | Merged at `457528b`; strata citations re-pinned (`index.hh`, `files.hh`); format 13 cited on `main` | Header, "What exists today", D1, D8, D16, D17, D18 |

## Revision 1

1. **D6 corrected and verified (DM-R1, Q3).** The owner's rule, a forgetful voter granting only
   after it has heard f + 1 forgetful voters, holds at three voters but, counted since the restart,
   loses an acknowledged `memory` write inside the model at five voters: the count gathers forgetful
   episodes that never overlap (a constructed schedule of 15 steps, and one of 26 that random search
   found on its own). Revision 0's every-voter-heard rule fails there too. Revision 1 keeps the
   count as what starts a recovery election and counts again among that election's answers at its
   own term, where terms make the count exact. The model checks it exhaustively at three voters to
   depth 12 and at five voters to depth 8, and by swarm walks at five voters to depth 30 (D6, "Model
   evidence"; Q10).
2. **The journal (DM-R2, DM-R6, DM-R18).** A same-boot restart rewrites and syncs every turn after
   the last synced one before trusting it, and recovers forgetful if any of them is torn; the turn
   log's supersession, copy replacement, and reclamation take effect at the barrier, never at the
   write.
3. **The cursors (DM-R3, DM-R4, DM-R5, DMS-6).** Checkpoints record a past refresh generation at or
   below `commit_durable`; every reply carries `synced`, and a reply to an append that asks for a
   barrier waits for it; a table assigns each of the core's 34 `durable_` reads its cursor; the
   append's `commit` carries `commit_durable` to every session, which is also the hard state's
   commit index, so no reader one version back ever persists more than is durable.
4. **One byte mover (DM-R8, DMS-1, DMS-5).** Segment files ride consensus's snapshot transfer,
   extended with file sets (D17): one flow control, one binding, one SHA-256 rule (OpenSSL's EVP)
   for every source, and stated limits per requester.
5. **Detent and the wire (DM-R7, DM-R9, DM-R15, Q8).** Nothing reaches a group without `acks:
   written` in its configuration; the fields are version 3's, through the version range; Part B
   acknowledges at A's `memory`, so nothing A freezes reopens.
6. **Segment mode's costs, stated (DM-R10, DM-R11, DM-R12, DMS-4).** Followers apply past the
   replay bound themselves; a sampled rebuild verifies the leader's segments (D18, Q9); the CPU
   estimate now counts SHA-256, TLS and the network stack, the verifier, and the refresh cadence.
7. **The checkpoint batcher and creation (small-indexes' C3).** A batch entry is a named
   generation's snapshot (SI-R19); whoever waits leads its batch, so no caller waits on a pool its
   peers hold (SI-R24); a failed checkpoint restores its index at the recorded generation from the
   intact journal and leaves the voter not forgetful (SI-R20, SI-R26); the creation record carries
   the index's UUID, and its replay is decided by the log (SI-R25).

## Changes in revision 1

| Finding | What changed | Where |
|---|---|---|
| DM-R1 | Kind 6 carries `forgetful`; the f + 1 count (Q3) lets a voter start a recovery election, whose candidate leads only when every answer at its term is a grant and f + 1 are flagged forgetful (kind 1 gains `recovery`, kind 2 `forgetful`); any voter with the count may start one, so a group whose longest log is on a voter that is not forgetful recovers; one model statement in D2, the summary, T3, T5, and the simulator change; the availability cost in the fault table; T3's forced schedules (DM-R1's trace and the two five-voter traces) and T6's mutants. The architect's fix as ruled is shown unsound at five voters, so the correction goes to the owner as Q10 | D2, D6, D16, fault model, T3, T6, Q10 |
| DM-R2 | Same-boot recovery rewrites every turn after S from the bytes it read and syncs before the open record; any torn turn after S makes the voter forgetful; `synced` reports only turns this process synced (recovery's barrier included); the per-group backend refuses `acks: written`; T9's rows | D3, D5, D16, T9 |
| DM-R3 | strata checkpoints the newest refresh generation at or below `commit_durable`, keeping its files until a later checkpoint covers them; the core's gate compares `through` with `commit_durable` (`core.h:562`); the retained disk stated; T4's row | D1, D7, T4 |
| DM-R4 | `synced` in every reply and coalesced reply record; a reply to an append whose `sync_through` reaches its entries waits for the barrier; quiescence requires `commit_durable` at the last index and known to every member; `quorum` latency stated against `strict`'s; T10's and T14's rows | D7, D10, T10, T14 |
| DM-R5 | The site table (each of `core.h`'s 34 `durable_` reads, its cursor, and why); membership admission, promotion, and witness truncation on the durable cursor; `commit_durable` clamped in kinds 3 and 9; `commit_durable` is the hard state's commit index, so "only grows" holds across restarts with no new bytes | D7, D16 |
| DM-R6 | Copy replacement, checkpoint supersession, and reclamation take effect at the barrier covering the turn (`completed()` splits into `written()` and `synced()`); D14's copy-forward row changes; T9's row | D3, D14, T9 |
| DM-R7 | Forgetfulness (after an unclean restart, or a failed barrier's restart that finds a torn tail), the `Recovery` entry, and every new journal record apply only to groups whose configuration has `acks: written` (Detent may opt in); `Recovery` batches under a new magic and checkpoints holding one under a new bundle magic, which older readers refuse; N's readers classify unknown magics, kinds, and versions as unsupported; a "For Detent" list; Detent's agreement before step 4 | D3, D5, D6, D16, "For Detent", first steps |
| DM-R8 | Segment files move through consensus's snapshot transfer, extended with file sets at version 3 (D17): group level, the group's bulk credit, the peer transport's D5 binding (an uncommitted membership binds, so an honest learner receives); strata's host kinds withdrawn | D8, D16, D17, threat model |
| DM-R9 | Every field listed with its kind and size, for version 3 (Q8: through N and N − 1, never holding version 2); the fields' semantics freeze only after the full design review (first steps 3 and 4) | D16, first steps |
| DM-R10 | A follower whose install lag passes the bound applies the tail itself through the mixed-version document path until an `Image` covers it; the leader no longer flushes early; T8 with the uplink saturated and during a mode switch | D8, D9, D15, T8 |
| DM-R11 | The blast radius in the fault model and REPLICATION; a verifier rebuilds a sample of flushes and merges on followers (D18; Q9 for the default); T4 seeds a leader-side fault; the follower lag in D10 and T11 | D8, D10, D18, fault model, T4, T11, Q9 |
| DM-R12 | The estimate redone with SHA-256 (OpenSSL's EVP, with and without SHA extensions), TLS and the stack, the verifier, and the cadence bracket; the summary and Q1's claims restated; T2 measures at the default refresh, over loopback TLS, from both sources | D8, summary, T2, `arith.out` |
| DM-R13 | `memory`'s window capped (past 4 × `sync_interval` or 64 MiB unsynced, a core's memory acknowledgements wait for its barrier); `replicated`'s row with DM-R1's fix; `leader`'s condition ("another voter wins the next election"); `relaxed`'s window from the waking write; `bounded`'s real-time bound on a minority side; `session` tokens checked against a term history kept past checkpoints; T7 at `memory` | D2, D10, D14, fault model, T7, T11 |
| DM-R14 | With leases on, a starting voter grants nothing for the lease duration, and a new leader serves lease reads only after a quorum answered its term's heartbeat and it committed an entry of its term; T5 with restarts | D9, T5 |
| DM-R15 | Part B's `resident` acknowledges at A's `memory` (written to the kernel), so D3, D5, and the wire stand; every preset keeps a synced journal, so `commit_durable` always advances; format 13's attribute lists named for the time bounds, the tier mark, and the disk tier's position; retention stamps use the HLC | D1, D7, D12, D16, Part B |
| DM-R16 | D8 adopts format 13's physical identity (F13-R41) and the whole-file check; D12 takes bit 16 `version_clocks` and chooses tombstones as keyed documents for deletes (F13-R52); the HLC is a foundations library | D8, D12, D16 |
| DM-R17 | D9's parts corrected (a pre-vote is a round trip; a `quorum` first commit two barriers and a round trip): 2,323 ms, 2,163 ms for `memory`; the client's timeout and redirect stated; T8 at the leader and at the client | D9, T8, `arith.out` |
| DM-R18 | The clean-close record ends the newest epoch; disk order said; one `acks` rule; `RFTW0002` only after activation; the limits named; T14's target concrete; D14's copy-forward row; S defined | D3, D7, D14, D17, T14 |
| DMS-1 | No requester-driven fetch remains: a receiver asks with `Request`, and the source streams; limits per (group, receiver), per receiver node, and per node, with their mechanisms; chunk rules are the snapshot channel's; transfers only for committed `Image` entries; reservation before the first chunk; the removed-member sentence; T14's row | D17, threat model, T14 |
| DMS-2 | A `forgetful` reply lowers `matched` no lower than the highest `synced` that voter reported to this leader, at most once per peer restart (the hello's boot identity, reported to the core as `PeerRestarted`); counters; the outgoing slot rule routed with SR22's C16; T6's mutant | D5, D16, D17, T6 |
| DMS-3 | The threat row names the flag's cost; the leader logs and counts each forgetful grant that made it, and a follower each range it replaces under `recovery_term`; T6's mutant with its verdict outside the model | D6, threat model, T6 |
| DMS-4 | T4's oracle for segment-mode groups is a rebuild, not the leader's bytes; the poisoned image's persistence in the threat model; Q9 for the owner (one applying member, a sampled rebuild, or both); the install entry's decoders fuzzed from the network | D18, threat model, T4, T15, Q9 |
| DMS-5 | One rule: every fetched file, from any source, streams through OpenSSL's EVP SHA-256 and CRC-32C and is compared before its rename; Kronuz/sha256 leaves the design; T2 measures EVP with and without SHA extensions | D8, D17, T2 |
| DMS-6 | `commit` carries `commit_durable` to every session; a member on a version-2 session replies only after its barrier, is counted for both cursors by `matched`, and a forgetful voter grants nothing over it, so one member's version holds back only itself; `acks: written` applies only after activation, and every replica refuses it before; T12's case | D7, D16, T12 |
| DMS-7 | Value rules for every new field and record, as proposed but one: `forgetful` stays allowed on a failed append reply, where the leader needs it | D16 |
| DMS-8 | The leader writes its best activated format; a member that cannot read an installed segment applies the batches itself, detected at install; no member announces formats | D8 |
| DMS-9 | Session tokens above the group's known last index refused at once; unclean-recovery, floors, acks, and replication mode take the index's administrative privilege; `manual` runs through the peer transport's admin path (signed statements); D14's barrier row says `quorum` writes cost today's barriers | D10, D14, D15, threat model |
| Architect C1 to C5 | Proposed done criteria in the ledger's next steps for the lead (C1); the security points answered by DMS-1, DMS-2, DMS-3 and the threat model (C2); library commits record Detent's build (C3, "For Detent"); T3 to T5, T8, T11, and T13 in both modes, with the matrix's cost (C4); "Changes to other documents" gains DM-R11's residual and DM-R13's restatements (C5) | Gate rows, "Changes to other documents", "For Detent" |
| Security C1 to C7 | DMS-1 to DMS-9 above; C7's value rules in D16 | As above |
| Owner Q1 to Q6, Q8 | Recorded as decided; Q3 with the correction above (Q10); Q8's version range in D16 | Open questions, D16 |
| Format 13 | The physical identity (F13-R41), bit 16 `version_clocks`, the per-segment and index-level attribute lists (F13-R43, F13-R51), and the delete's HLC (F13-R52), each taken where this design needs it | D8, D12, D16, Part B |
| `research/no-stalls` | D9's cadence is the no-stalls controller's (refresh stretched under merge debt up to 30 s, never shortened for shipping); every background task here reports its debt in its unit to that one controller; Part B's disk tier the same | D9, D14, Part B |
| Small-indexes' re-check (`2e9949e`, its C3): SI-R19, SI-R20, SI-R24, SI-R25, SI-R26 | The checkpoint batcher's entry is a named generation's snapshot, and a standalone `commit()` the same entry; whoever waits leads its batch on its own thread, and only checkpoints nobody waits on go to the pool; a checkpoint record names only completed checkpoints, and a failed checkpoint restores its index at the recorded generation through `Repair` from the intact journal, not forgetful (SI-R26: the journal's failed barrier stays D5's); the creation record is strata's `Create` with the index's UUID, its replay decided by the log, a manifest with another UUID refused with nothing removed; T16 | D1, D16, "How small-indexes' D3 relates", T16, first steps |

## Summary

- **Commit splits in three** (D1). An acknowledgement waits only for the core's shared journal
  barrier, visibility is refresh, and today's commit protocol becomes a background checkpoint of a
  durably committed generation, batched across indexes. A tiny index's 10 device flushes on the
  acknowledgement path (measured, T1) become a share of one barrier per turn.
- **A new acknowledgement, `memory`** (D2, D3, D7): a majority of voters wrote the batch to their
  journals, and each syncs within `sync_interval`. The model: at most f voters lose their OS or
  power or are forgetful at once, a forgetful voter counting until it re-learns, across restarts,
  with any number of process crashes; f of each membership in force. Within it nothing
  acknowledged is lost; beyond it, `memory` writes in the
  capped window (D2) that no recovery quorum held. A second cursor, `commit_durable` (a majority
  synced), bounds checkpoints and `quorum` acknowledgements, so no recovery loses what a
  checkpoint or a `quorum` write holds.
- **Forgetful voters** (D5): a voter that may have lost acknowledged entries (an OS crash or power
  loss, or a torn tail found by a same-boot restart, such as the one a failed barrier forces) keeps
  its identity, acknowledges appends, and votes only after it re-learns from a leader, staying
  forgetful across every restart until then (a mark in its hard state). When more than f are
  forgetful at once, a recovery election (D6) counts a majority of its membership flagged
  forgetful at its own term and elects a log at least as up to date as every answer's; it never
  loses a durably committed entry.
- **Segment mode** (D8, D17, D18): followers append the log but install the leader's segments,
  moved by the snapshot transfer's file sets. Estimated from T2's measured parts with SHA-256, TLS
  and the stack, and a sampled verifier, a three-voter group spends 0.38× to 0.43× of document
  mode's CPU and a five-voter group 0.25× to 0.32× at the same refresh cadence (`arith.out`). A
  follower whose install lags applies the tail itself, so the replay at promotion stays within one
  election timeout, and where the refresh is relaxed for bulk ingest segment mode's cost tends to
  document mode's plus its shipping, hashing, and verification.
- **One vocabulary** (D10): five acknowledgement levels (`all`, `quorum`, `memory`, `leader`,
  `none`), five read levels on Cosmos DB's spectrum (`linearizable`, `bounded`, `session`,
  `prefix`, `eventual`), and presets (`strict`, `replicated`, `relaxed`; Part B's `resident`;
  D12's `available`) that strato, shale, the log system, and the analytics system share.
- **Partition-tolerant writes** (D12) are designed now and built later, as host-level side logs
  merged by one ordinary command on the one log; leaderless replication is rejected (D13).
- **No stalls and live tuning** (D14, D15): every background task this design adds reports its
  debt to the one controller `research/no-stalls` recommends, and each bound says whether it
  changes live, through Detent, or through the log.

| Preset | Acknowledged (default) | Replication | Default read | Loses, beyond the model | Failover gap (estimate) |
|---|---|---|---|---|---|
| `strict` | `quorum` | document | `session` | nothing (Track C D11's media limit aside) | 2.3 s |
| `replicated` | `memory` | segment | `prefix` | `memory` writes in the capped window that no recovery quorum held, when more than f voters are forgetful at once | 2.2 s plus a replay of at most 1 s |
| `relaxed` | `leader` | segment | `prefix` | also the leader's writes not yet on a majority, at any failover | as `replicated` |
| `resident` (Part B) | `memory` | Part B | `prefix` | Part B | Part B |
| `available` (D12, later) | `memory`; `provisional` on a minority side | segment | `eventual` on a minority side | the older of two concurrent writes to a key, by its hybrid clock | none for the minority side's writes |

## What exists today (verified)

### Xapiand (MIT own code at `master` `7e8cccb7d`)

| Claim | Evidence | Correction to the lead's analysis |
|---|---|---|
| One cluster-wide Raft elects the master | `src/server/raft_delegate.h:53-123` (delegate of `cluster::Raft`) | Holds |
| Each shard's primary comes from a separate majority poll the Raft leader runs; it picks the eligible active node with the highest revision | `src/server/discovery.cc:653-716` (nodes answer with UUID and revision), `718-801` (the leader counts a majority of the shard's nodes; selection `776-787`) | The range is 653-801 |
| The Xapian database is opened `DB_NO_SYNC` whenever the WAL is built in; durability is the WAL's | `src/database/shard.cc:77-81`, `350`, `356` | Not in the lead's list; it matters for `?commit` |
| The WAL's volume syncs through the debounced fsyncher (`STORAGE_ASYNC_SYNC`) | `src/database/wal.cc:68`, `114`, `129`; `src/xapiand_fsyncher.h:29-51`; storage `storage.h:1090-1113` | Holds |
| A failed asynchronous sync is logged and dropped: nothing retries it or fences the shard | `src/xapiand_fsyncher.h:39-47` | New; an observation for Xapiand's backlog, outside this design |
| `DB_SYNCHRONOUS_WAL` makes the WAL *write* inline, not its sync: the volume stays `STORAGE_ASYNC_SYNC` | `wal.cc:1162-1178` (`execute` against `enqueue`); `wal.cc:676`, `695` | REPLICATION.md:165 maps it to `ack=local`; it is process-crash safety only, `memory` at rf = 1 (D10) |
| `?commit` commits Xapian (no sync), enqueues the WAL commit line, and triggers replication | `search_views.cc:2333-2337`; `shard.cc:1466-1468`, `750-858` (`814-820`, `823-827`) | REPLICATION.md:166 maps it to `ack=quorum` plus `refresh=true`; it gives visibility, not power-loss durability |
| Effective debouncer defaults are set in `opts.cc`, overriding `opts.h`'s initializers | `src/opts.cc:114-136`, `628-650`; `opts.h:114-133` | See the next table |
| Debouncer: the first call fires after `debounce`, later calls after `busy`, capped at a random force time; a throttle spaces fires | `debouncer.h:177-211`, `228-281` | |
| Replicas pull after `DB_UPDATED` with a random 0 to 3 s delay plus the trigger debounce | `discovery.cc:488-545` (`535`) | Not in the lead's list |
| Under the async hook, the storage v2 commit's two syncs around the header write are both deferred, so the data-before-header order is not kept | storage `storage.h:1116-1148` | An observation for Xapiand's backlog, outside this design |

| Debouncer | Debounce | Busy | Forced | Throttle |
|---|---:|---:|---:|---:|
| committer (autocommit) | 1,000 ms | 3,000 ms | 8,000 to 10,000 ms | 0 |
| fsyncher (WAL sync) | 500 ms | 800 ms | 2,500 to 3,500 ms | 1,000 ms |
| db_updater (`DB_UPDATED`) | 100 ms | 500 ms | 4,900 to 5,100 ms | 1,000 ms |
| trigger_replication (pull) | 100 ms | 500 ms | 4,900 to 5,100 ms | 1,000 ms |

What a client can lose today, derived from those constants (not measured):

| Failure | Idle (one write) | Sustained writes |
|---|---|---|
| Primary process crash, then failover to a replica | Writes not yet pulled: commit at 1 s, update at 1.1 s, pull at 1.2 to 4.2 s | Commits forced every 8 to 10 s: up to about 13 s |
| Primary power loss | The unsynced WAL tail: 0.5 s, plus the async writer queue | Up to 3.5 s plus the queue |

The lead's "about a second" is the idle commit delay alone.

### strata, consensus, and journal

- One Raft group per shard (`strata::group`, REPLICATION.md:26-54), with root and placement groups
  above (DISTRIBUTION.md:38-63): leadership is per shard, as in CockroachDB and TiKV.
- Document mode: every voter applies every batch (REPLICATION.md:84-86). The batch carries the
  prepared document (REPLICATION.md:61-63; ARCHITECTURE.md:85-91), so followers repeat stage 1,
  inversion, flush, and merges, never analysis.
- The consensus core holds one storage batch outstanding and acts on it only once `Persisted`
  completes it (`core.h:424-429`, `439-475`); followers persist their hard state, commit index
  included, with each batch (`core.h:785`, `914`); commit counts voters' durable matches
  (`core.h:1146-1161`); delivery reads the persisted commit index (`core.h:1162-1172`); a follower
  refuses to replace an entry at or below its commit index (`core.h:868-871`); `Repair` restores an
  image at a committed boundary and redelivers from the voter's own log (`core.h:597-609`); an
  install at or below the commit index is refused as caught up (`core.h:484`); a leader never lowers
  a follower's matched index (`core.h:945`, `957-969`; Track C's D15, `track-c.md:730-731`).
- The turn log (`RFTW0001`, Track C D11) writes one turn and its barrier at a time
  (`turn_log.h:281-284`; `turn_driver.h:66-99`); each turn ends with a record counting its records
  (`turn_log.h:303-304`); supersession, copy replacement, and reclamation run at `completed()`, once
  the turn's barrier returned (`turn_log.h:315-329`, `343-350`, `378-391`); recovery keeps the newest
  epoch's last turn only whole and calls damage before a later complete turn corruption
  (`turn_log.h:451-453`, `534`, `555`). Standalone durability is the same mechanism as a group of one
  (`workload-profiles.md:100-106`; rocksdb-survey K2). The per-group journal's recovery replays
  only to its manifest's frontier and truncates the rest (`journal.h:333-365`).
- Storage batch magics: a batch holding a `Configuration` entry uses its own magic so an older
  reader refuses it (`storage.h:46-48`, `120`, `241-242`, `272`); the checkpoint reader refuses an
  unknown bundle magic (`checkpoint.h:71-72`) and calls an unknown entry kind corruption
  (`checkpoint.h:107`). Every reader in journal and consensus throws `Corruption`, with no separate
  "unsupported" class (`journal/codec.h:33`).
- `index::install` replaces the whole content under fresh local names (`index.hh:751-812`,
  `773-776`), after reserving storage for it (`index.hh:763-767`); strata's apply skips positions at
  or below the applied one (`index.hh:415-418`) and records the position (`index.hh:447`);
  `commit()` makes everything applied durable (`index.hh:506-518`). Track C's D18 retry already
  asks for an install of only the files a receiver lacks (`track-c.md:783-789`); the survey's Q7
  asks for adopting an externally built segment through the log (`rocksdb-survey.md:365`; owner
  decision `:386-388`).
- The snapshot transfer moves one application blob per image (`snapshot.h:35-45`, `RFTSNP02`) in
  chunks of at most 65,536 bytes at contiguous offsets (`snapshot_transfer.h:29-78`;
  `worker.h:312-319`), started by the leader (`core.h:1106-1110`), on the group's bulk-lane window
  at version 2 (`peer-transport.md:840-843`); a receiver cannot ask for one.
- Read copies are learners, up to every node (`placement-strategies.md:111-127`).

**Syncs to create one tiny index and commit it, measured (T1)** through a counting IO over
`PosixIO`, with the syscalls counted by an interposed library (macOS, APFS, development Mac;
`results/2026-10-18-t1-macos.txt`, measured at `0b407c9`; the cited lines are at `026ee10`, where
the same calls sit):

| Step | Durability calls | `F_FULLFSYNC` | `fsync` | Where |
|---|---:|---:|---:|---|
| `create` | 6 | 6 | 4 | `owner.lock` (completion `posix.h:348-358`); `clear_uncreated`'s directory sync (`index.hh:2285-2297`); the first manifest's metadata file, directory, manifest temporary, directory (`index.hh:2116`, `2157`, `2161`, `2165`) |
| First commit, one document | 4 | 4 | 3 | The segment (`sync_files`, `index.hh:2116`), the directory (`2157`), the manifest temporary (`2161`), the directory (`2165`) |
| Each later commit | 4 | 4 | 3 | The same |

Create plus the first commit: 10 durability calls, 10 device flushes and 7 plain `fsync`s on macOS,
the same for each of 10 indexes. On Linux the same 10 calls are 10 `fsync`s by `posix.h:121-129`,
`175-186`, and `443-453` (derived, not measured here: the gate VM is a shared measurement host).
small-indexes measured 10 on its host (`small-indexes.md:19-21`).

## Decisions

### D1. Commit becomes three steps

- **Durability** is the core's turn-log barrier, shared by every group on the core: one barrier per
  turn today, at most one per `sync_interval` in groups that acknowledge from memory (D3).
- **Visibility** is refresh: memory only, every second by default (ARCHITECTURE.md:105-106), on
  the no-stalls controller's schedule (D9).
- **Checkpoint** is today's commit protocol (`index.hh:2075-2168`: the new files synced together,
  the directory, the manifest under a temporary name, the rename, the directory), moved off the
  acknowledgement path. It runs in the background, batched across indexes through one node-level
  commit group (small-indexes' D3-A in SI-R6's form, below), and is published by the group
  checkpoint record that lets the log truncate (`track-c.md:582-587`; condition 25).
- **A checkpoint records a past generation (DM-R3).** Apply runs at `commit_memory` (D7), so the
  current state is ahead of `commit_durable` under ingest, and a checkpoint of it would hold writes
  D6 can remove; `Repair` would then restore them and skip their replacements as applied
  (`index.hh:415-418`), and the replica would diverge silently. So strata checkpoints **the newest
  refresh generation whose position is at or below `commit_durable`**, the generations an `Image`
  names (D8): its segments (immutable), live-docs files encoded from that refresh's deleted bitmaps
  (which its reader snapshot holds), its metadata, and its position. The cost is disk: the flush
  and merge output written since the checkpointed generation, about one checkpoint interval of it.
  The core's gate compares the capture's `through` with `commit_durable` instead of refusing
  whenever apply runs past the persisted commit (`core.h:562`, D7's site table).
- **Retention keys on the checkpoint's own manifest (DM-R23).** Today one manifest is both the
  durable head and the live state: `commit_files()` sets the in-memory manifest `m_` to what it
  wrote (`index.hh:2166`), and `retain()` keeps the head `m_.generation` with the names of `m_`
  and the current segments `segs_` (`index.hh:2188-2216`, `2248-2252`), queueing every other file
  for `reclaim()` (`index.hh:2219-2244`). A checkpoint of a past generation splits the two: what it
  writes is generation g's snapshot, while `m_` stays the current state, whose applied position
  decides what apply skips (`index.hh:415-418`; were `m_` the snapshot, apply would re-run the
  positions after g). So the index holds both: the **durable head**, the checkpoint's manifest as
  the batch entry snapshotted it (SI-R19, below), and the **current state**, `m_` and `segs_`.
  `retain()` and `manifest_files()` take the head's generation and names from the durable head,
  never from `m_`, and keep them beside the current state's files, so the merge inputs only g
  names and the live-docs and metadata files encoded for g stay until a later checkpoint's head no
  longer names them. Files of later states that no durable manifest names are the open's to
  reclaim after a crash (`index.hh:2353-2358`), and the open requires every file the head names
  (`index.hh:2338-2340`). A standalone `commit()` checkpoints its current generation, so its head
  and `m_` coincide, as today. T16 tests it: a checkpoint of a past generation whose inputs a
  later merge replaced and whose live-docs files it wrote, then the owner's retention and reclaim,
  then a crash and an open.
- Rejected: pausing apply until `commit_durable` catches up at each checkpoint. It stalls every
  verdict for up to `sync_interval` and a round trip, which D14 forbids.
- **The batcher's entry and threads (SI-R19, SI-R24).** A batch entry is a checkpoint of a named
  generation: the index, the generation, and that generation's files and manifest, snapshotted by
  the owner when it submits, so `commit_files` works from the snapshot, never from the index's
  segments at phase time. A standalone `commit()` (an index with no journal) submits its current
  generation: the same entry, one implementation. strata starts no threads (`pool_executor.hh:1-4`),
  and the pool's `TaskGroup::wait()` blocks without running queued work (threadpool
  `task_group.hh:223-228`, strata's pin `628679a`), so callers waiting on a batch posted to a
  shared pool can hold every worker the batch needs: small-indexes' probe, a pool of 2 and two
  committing owners, returned neither within 3 s. So whoever waits leads: a `commit()` that finds no
  batch running runs one on its own thread, taking every entry already waiting, and later callers
  wait for it or lead the next, as RocksDB's write group does. Only checkpoints nobody waits on,
  this design's background ones, are posted to the pool's background class. A caller that comes to
  wait while such a batch is queued and has not started claims it and runs it on its own thread;
  the posted task finds it claimed and returns; and a batch already running on a pool worker
  completes without another worker, so waiting for it cannot deadlock. This is small-indexes' claim
  rule word for word (`small-indexes.md:329-333` at `45985dc`), so the two designs state one rule
  (DM-R22 item 7). Retention and reclamation run on each owner after its entry completes
  (`index.hh:516-517`), never in the batch.
- **A failed checkpoint (SI-R20, SI-R26).** A checkpoint record names only a checkpoint whose every
  phase completed, so the log never truncates past an index's last completed one. A failed
  checkpoint is not a failed journal barrier: the index fences its checkpoints, never retries a
  sync as success, and is restored at the generation its last checkpoint record names
  (`recover(to)`, which commits that generation again as the head, `index.hh:2332-2335`,
  `2350-2352`), never at the highest manifest on disk, through Track C's repair path (`Repair` at
  that boundary and redelivery from the voter's own log, a leader transferring first,
  `track-c.md:432-450`). The journal is intact and holds every entry above that generation, so
  nothing acknowledged is lost and the voter is not forgetful; a failed journal barrier is D5's (a
  process restart, then D3's same-boot rule). Which failures fail one index and which fail every
  index of a batch on a device is small-indexes' D3 (SI-R7).
- **Creation (SI-R25, DMS-10).** The journal records an index's creation as strata's `Create`
  command (D16), carrying the UUID `create()` already takes (`index.hh:339`, `348`) and every
  manifest stores (`files.hh:95`). The log keeps an index's creation record, and every record after
  it, until a checkpoint record of that index lands, which truncation by records gives; that
  record then supersedes the creation record (`track-c.md:582-587`).
  - **Only the first `Create` of a UUID creates.** A `Create` is its UUID's creation record only
    when no checkpoint record and no earlier `Create` of that UUID precede it in the retained log.
    Any other `Create` of the UUID is a duplicate, an honest client's retry after a lost reply
    among them: the leader refuses to propose one for an index it holds (the retry gets the verdict
    `exists`, which strato's "create if absent" reads as success), and one that reaches the log
    anyway, proposed before the leader applied the first, applies as `exists` and changes nothing.
  - **Replay decides by the log.** With any checkpoint record of the index in the log, the index
    opens at the newest one's generation (above) and replays what follows it, a duplicate `Create`
    there as a no-op. With none, the log holds the creation record and every write since, so replay
    removes the residue under strata's names (those `clear_uncreated` removes, `index.hh:2292-2294`)
    and applies from the creation, provided every manifest present carries the record's UUID. A
    duplicate is never taken for the creation record, so a retry never wipes an index. Read as
    revision 1 wrote it, a retry after a checkpoint record replayed an index of 100 documents at
    generation 7 into 2 documents at generation 0, on each replica that restarts (the security
    re-check's `create_replay.out`).
  - A manifest with another UUID, another incarnation's index at that path, refuses the replay with
    a named error and removes nothing. Only this replay clears past a manifest or a segment; every
    other `create()` and `open()` keeps today's narrow `uncreated()` and its refusal
    (`index.hh:2276-2282`, `2291`).
- **Per tiny index** (derived from T1's counts): the acknowledgement path pays 1/N of a shared
  barrier, N being the groups in the turn; a checkpoint's 4 durability calls run in the background,
  and on macOS 4 device flushes per device per batch once completion has a cross-IO directory
  barrier (SI-R10); create's 6 fold into the first checkpoint, since the journal records the
  creation (small-indexes' D3-C, journaled; the creation record above). Idle indexes rarely
  checkpoint: the turn log copies their live records forward instead (`track-c.md:523-535`).
- Every preset keeps a synced journal, `resident` included (Part B, DM-R15), so `commit_durable`
  advances in every group and bounds every checkpoint. Standalone (rf = 1) is the same mechanism, a
  group of one.

### D2. A new acknowledgement, `memory`

- **Acknowledged when** a majority of voters, the leader included, wrote the batch to their
  journals (the write returned, so each kernel holds it) and the leader applied it, so the reply
  carries the verdict (D7). Each voter's barrier follows within `sync_interval` (D3).
- **The model** (one statement, used in the summary, T3, T5, and the simulator change): at most f
  voters lose their OS or power or are forgetful at once, a forgetful voter counting until it
  re-learns, across every restart (D5); process crashes, partitions, and clock skew in any number.
  f is ⌊(n − 1)/2⌋ of a membership of n voters, and the bound holds for every membership in force:
  2f + 1 voters in steady state, 2f + 2 in Track C's replacement windows (six voters and f = 2 at
  rf = 5, four and f = 1 at rf = 3, `track-c.md:270-275`), where the two adjacent memberships have
  the same f (DM-R20).
- **Survives** every failure within the model (D5, D6: no recovery election wins within it).
- **Lost** only beyond it, and only writes no recovery quorum held (D6) among those a majority's
  kernels held unsynced: **the memory window**. D14 caps it per core, on every voter (DM-R13,
  DM-R22 item 1): once a core's oldest unsynced written entry is 4 × `sync_interval` old (400 ms)
  or its unsynced bytes reach `max_unsynced` (64 MiB), the core stops answering at `Written`: a
  follower's append replies wait for its barrier, and a leader counts its own match for
  `commit_memory` at its synced index, so every `memory` acknowledgement that rests on that core
  takes `quorum`'s latency, and nothing is refused. The stall DM-R13 named, under the followers, is
  the follower's half of the rule. So a stalled device keeps at most 400 ms or 64 MiB of a core's
  written entries counted toward `commit_memory`, however long it stalls. Every voter that wrote a
  batch has synced it once its barrier returns; from then on it is as safe as `quorum`.
- Precedents with the same guarantee: PostgreSQL's `synchronous_commit = remote_write` ("data
  preservation if a standby instance of PostgreSQL crashes, but not if the standby suffers an
  operating-system-level crash"), MongoDB's majority writes with
  `writeConcernMajorityJournalDefault: false` ("could possibly roll back in the event of a
  transient loss ... of a majority of nodes"), and Elasticsearch's `index.translog.durability:
  async` (fsync every `sync_interval`, 5 s by default, dynamic per index).

### D3. The bounded barrier

- Per core: a barrier at most every `sync_interval` covering every turn written since the last, and
  an immediate one whenever a staged record needs it: hard state (D4), a `quorum` or `all` write
  (`sync_through`, D7), a configuration entry, a checkpoint record, or a segment switch.
- **One write in flight.** Several turns sit between barriers, which the turn log's
  one-turn-in-flight rule forbids today (`turn_log.h:279-283`; condition 7). The rule becomes one
  write in flight: turn k + 1 is written after turn k's write returns, never before, so turns land
  in file order in the page cache. Writeback may still reach the disk out of order; the recovery
  rule below takes any subset of the unsynced sectors, which T9's sector subsets test (DM-R18).
- **Effects at the barrier (DM-R6).** A turn's copies replacing their originals, a checkpoint
  record superseding its group's earlier records, and the reclamation of segments left with no
  live record (`turn_log.h:316-329`, `343-350`, `378-391`) take effect when the barrier covering the
  turn returns, never at its write. `completed()` splits in two: `written()` releases the next
  turn's write (offsets, block manifests, and sequences advance); `synced(turn)` applies the notes
  and reclaims for every turn through it. A power loss after a copy-forward turn's write therefore
  finds the synced original still live and its segment not recycled.
- **The end-of-turn record gains S**, the last turn whose write returned before a barrier that has
  since returned started (a write concurrent with a barrier may or may not be covered, DM-R18): 8
  bytes beside today's 4-byte count (`turn_log.h:303-304`). Recovery takes the highest S among
  complete turns of the newest epoch. Turns through S keep today's rules: damage there is
  corruption and costs the groups the block manifests name. After S it keeps the longest whole
  prefix and drops the rest, complete or not: none of those turns was synced, so this voter may
  have acknowledged what it drops.
- **Unclean restarts** (the kernel boot identity changed and the newest epoch does not end with a
  clean-close record): recovery keeps that prefix and the voter recovers forgetful (D5) in groups
  that allow memory acknowledgements.
- **Same-boot restarts (DM-R2).** The page cache holds every turn whose write returned, unless a
  failed writeback dropped pages, which a new process cannot learn from the kernel (an `fsync` that
  failed has reported its error, and errseq reports it once). So before it writes the open record,
  recovery rewrites every complete turn after S from the bytes it read and syncs them; a page left
  clean but unwritten after a failed writeback is written again, and a failure fences as any
  barrier does. If any turn after S is torn, the last included, pages were lost or a write was in
  flight, and the voter recovers forgetful: conservative, since a process killed inside its one
  write in flight also tears the last turn, and costs that voter one re-learning. With no torn turn
  each group recovers as it stopped: forgetful exactly where its hard state carries the mark or
  predates the open record's V (below; D4, D5; DM-R19). A voter reports `synced` only for turns a
  barrier of this process covered, recovery's included.
- **The verdict outlives its evidence (DM-R19).** The new epoch's open record is what ends the
  evidence for an unclean or torn verdict: after it, a restart in this boot reads as same-boot, and
  the old epoch's tail is no longer the newest epoch's (`track-c.md:510-516`). So the open record
  carries the verdict, 8 bytes: V, the newest epoch whose recovery found its voters forgetful, this
  epoch when this recovery did, else the previous open record's V. It is durable before the epoch's
  first turn, as every open record is (`track-c.md:476-478`, condition 8), so no restart finds the
  open without its verdict. A group in a `written` configuration recovers forgetful when this
  recovery's verdict says so, when its newest hard state carries the mark (D4), or when its newest
  hard state was written in an epoch older than V: a group that has written nothing since the
  verdict, idle or not yet replayed, stays forgetful across any number of later restarts until it
  re-learns. An open record recovery cannot read, which Track C infers (`track-c.md:483-486`),
  counts as naming its own epoch.
- **Damage inside turns after S** reads as their torn tail, Track C D11's stated limit
  (`track-c.md:560-562`) widened from one turn to those turns. A strict record never sits there
  unsynced for long: it forces a barrier, and the next turn records it synced.
- **Format `RFTW0002`**: the 12-byte end-of-turn payload, a 16-byte kernel boot identity in the open
  record (D5) beside V (above), a clean-close record (kind `0x12`, free at `turn_log.h:87`), and the
  group checkpoint record's hard state with the forgetful mark (D4). The clean-close record is
  written only after a barrier covering every written turn, so every mark is synced before it, is
  itself synced, and counts only as the newest epoch's last record: a later open record cancels it
  (DMS-7). A clean close never clears a mark. A boot identity of zero or one the platform cannot
  read reads as unknown, which recovery treats as unclean.
- **Activation (DM-R18).** A turn driver writes `RFTW0002` only after memory acknowledgements are
  activated on every node (the peer transport's D7 formats rule, `peer-transport.md:524`), and only
  when it has a `sync_interval`; before that, and without one, it writes `RFTW0001`, so a node
  rolled back to N − 1 before activation reads its own journal. Both are read.
- **Default `sync_interval`: 100 ms** (Q2, decided). The turn driver takes the smallest bound among
  its groups' presets and the node's knob (D15).
- **The per-group journal refuses `acks: written` (DM-R2, DM-R7).** Its recovery replays only to
  the manifest's frontier and truncates the rest (`journal.h:333-365`), so a process crash would
  drop batches acknowledged from memory. It stays Detent's backend, unchanged. If Track C's T6
  keeps it for strata (SYS-8), its memory mode is an addendum then, opt-in per store: same-boot
  recovery replays framed batches past the frontier to the first invalid one, rewrites and syncs
  them, publishes the frontier, and recovers forgetful on a torn batch.

### D4. Hard state and configuration stay durable before anything depends on them

- A term or a vote is synced before the vote reply or any message at the new term (dissertation
  §3.8: persisting the term and vote "is necessary to prevent the server from voting twice in the
  same term"). A configuration entry is synced before its node acts on it: a membership is in
  force from the moment it is durable (`track-c.md:216-218`), and an `acks` change (D7) must not be
  forgotten.
- The hard state's commit index holds `commit_durable` in a group that allows memory
  acknowledgements (D7), and it rides the bounded barrier: §3.8 lets the commit index "safely be
  reinitialized to zero on a restart", and `commit_memory` restarts from it. `StorageBatch` gains an
  urgency flag (barrier now, or within `sync_interval`); a batch whose hard state changes only the
  commit index or the forgetful mark takes the second.
- **The forgetful mark (DM-R19).** The hard state gains one byte: whether this voter is forgetful in
  this group (D5). Every hard state the core writes while forgetful carries it set, so a hard state
  written after the verdict's epoch (D3's V) still says forgetful; the storage batch that completes
  the voter's re-learning carries it cleared, after the re-learned entries, so a cleared mark never
  survives without the entries it vouches for: turns land in file order and recovery keeps a whole
  prefix (D3). It rides every place the hard state is persisted: storage batches, the turn log's
  hard-state records (`track-c.md:494-496`), checkpoint bundles (`checkpoint.h:97-98`), and the
  group checkpoint record (`track-c.md:582-587`), so no checkpoint or truncation drops it. It needs
  no barrier of its own: a power loss before a set or a cleared mark is synced makes the voter
  forgetful again by the unclean rule (D3), and a restart in the same boot reads it from the page
  cache, so the mark is lost only together with a condition that sets it again. It exists only in
  groups whose configuration has `acks: written` (D16).

### D5. Forgetful voters

A voter is **forgetful**, in a group whose newest durable configuration entry has `acks: written`
(D7), or under a host that opts in (Detent may: "For Detent"):

- after an **unclean restart** (D3): the kernel boot identity changed and the newest epoch does not
  end with a clean-close record. The boot identity is a new completion primitive (additive): Linux's
  `/proc/sys/kernel/random/boot_id`, macOS's `kern.bootsessionuuid` (read on the development Mac),
  and FreeBSD's `kern.boot_id` (r359053, 2020). The open record stores it (D3);
- after a **same-boot restart that finds a torn turn after S** (D3, DM-R2); a same-boot restart
  without one rewrites and syncs the tail and resumes as it stopped, forgetful only if it was
  (below), since that restart lost nothing acknowledged;
- after a **failed barrier**, only as the rule above then finds: the barrier fences the core
  (`core.h:476`), Linux may drop the unsynced pages (small-indexes SI-R8), and the node restarts its
  process to reopen the journal, as PostgreSQL panics on a failed `fsync`; the restart in the same
  boot runs the rule above (newly forgetful only when a turn after S is torn), never a retry of the
  sync as success. A failed checkpoint is not a failed barrier: its index is restored from the
  intact journal and the voter stays as it was (D1, SI-R20). Every forgetful episode therefore
  begins with a new process, whose new boot identity in the hello (`peer-transport.md:550`) renews
  the leader's rewind allowance below.

**Forgetfulness is durable (DM-R19).** A forgetful voter stays forgetful until it re-learns, across
every restart in between: a same-boot process restart, a failed barrier's restart, and a clean close
followed by a reboot each find it still forgetful, because the open record names the verdict's epoch
until the group writes again (D3) and every hard state it writes while forgetful carries the mark
(D4), which only re-learning clears. Revision 1 decided each restart from the boot identity, the
clean-close record, and torn turns alone, so a voter that lost entries in a power loss and then
restarted its process before re-learning came back not forgetful and electable without them: the
architect's `restart3` loses a `memory` write in 7 steps at three voters with one OS crash, and
`torn3` does the same with a torn same-boot tail and no OS crash at all
(`results/r2/schedules-cc.out`; the searches in D6's model evidence). D6's argument needs exactly
this: an answerer stays forgetful until it re-learns.

While forgetful, a voter keeps its incarnation, hard state, and log, acknowledges appends (each
acknowledgement is prefix-complete: the consistency check, `core.h:854-857`, matched its log to
the leader's through the entry), and is counted for commit. Outside D6's recovery elections it
grants no vote or pre-vote and never campaigns, and it acts on no `TimeoutNow`, until it
**re-learns**: its log matches a leader of its durable term or later through that leader's last
index at their first contact (`leader_last`, a new append field, D16). The re-learned prefix needs
no sync of its own: a vote reads the log in memory, which holds at least the durable log, and a
second unclean restart makes the voter forgetful again. The storage batch that completes the
re-learning carries the cleared mark (D4), so a restart finds the cleared mark only together with
the re-learned entries. A recovery winner (D6) is not forgetful once it leads: it is its term's
leader, the authority re-learning is measured against, and it clears its mark in the batch that
holds its `Recovery` entry (the model clears its flag at the decision).

A forgetful voter's replies say so (`forgetful`, D16), and the leader then lowers its matched index
for that voter and resends from there. Today's core never does: "Durable peers cannot lose a
matched prefix on restart; never rewind below verified progress" (`core.h:957-969`; Track C's D15,
"`matched` never moves back", `track-c.md:730-731`), so a voter that lost acknowledged entries would
never receive them again. The rewind is bounded (DMS-2):

1. **The floor (DMS-13).** A forgetful reply lowers `matched` to no less than the leader's floor for
   that voter: the larger of the highest `synced` the voter reported to this leader and `matched`
   less the voter's largest possible unsynced window, the longest run of entries the leader sent
   it, ending at `matched`, whose bytes fit `max_unsynced` (64 MiB). Both figures bound an honest
   voter's loss: within the model a completed barrier is durable, and past `max_unsynced` its core
   answers only after its barrier (D2's cap). The second is the leader's own arithmetic, so a voter
   that reports `synced` 0 buys one window, as an honest one costs, and no more. The leader counts
   payload bytes, which the journal's framing only exceeds, so its window is never shorter than the
   voter's. A reply below the floor is counted (`forgetful_below_synced`) and treated as the floor.
2. **Once per restart, at most once per `recovery_timeout`.** The leader rewinds a peer at most once
   per restart of that peer's node, and at most once per `recovery_timeout` per peer (DMS-13): the
   hello carries a boot identity fresh at each process start (`peer-transport.md:550`), which the
   host reports to the core as `PeerRestarted{incarnation}` (additive, D16), and a stream of fresh
   boot identities buys one window per timeout. Later flagged replies change no index and are
   counted (`forgetful_repeated`); the flag's standing meaning, that the voter grants nothing, is
   unaffected. An honest voter pays only if it restarts forgetful twice within one
   `recovery_timeout`: its second rewind waits for the rest of it (`forgetful_deferred`), and
   meanwhile it grants nothing and falls behind. First contact under a new leader is standard Raft:
   `matched` starts at 0 and the hint-driven backoff (`core.h:962-966`) walks back.
3. Lowering a matched index never lowers a commit already counted, and Track C's promotion check
   (its D3) and the witness's bound then see the lower figure, as they should (D7's site table).

**Why commits at the memory majority stay safe while at most f voters are forgetful.** Let e be
written by a majority Q_e in term T and counted committed there. Take a later term U whose leader
won a quorum Q_U of voters that are not forgetful (D6 shows no recovery election wins within the
model). Majorities intersect, those of adjacent memberships included (Track C's D3, one voter at a
time), so some x is in both. Either x has not restarted forgetful since writing e, and holds e (no
leader of a term between T and U lacks e, by induction on terms, so none truncated it); or x
restarted forgetful, stayed so across any later restart (the mark), and re-learned from a leader of
a term below U, which held e by induction, through that leader's last index, so x holds e again.
Since x voted for U's leader, that leader's log is at least as up to date as x's and holds e
(dissertation §3.6.3). The base case is T's own leader, which holds its entries while it leads.
Terms survive every restart (D4), so no voter votes twice. The model checks it exhaustively to
bounded depths at three and five voters, with same-boot restarts and torn tails, and near the
replacement windows' schedules at six (D6).

Precedents: Viewstamped Replication Revisited §4.3 ("When a replica recovers after a crash it
cannot participate in request processing and view changes until it has a state at least as recent
as when it failed"); here the voter keeps acknowledging appends, which Raft's consistency check makes
safe, and stays out of elections only. Kafka's KIP-966 removes a broker that shut down uncleanly
(detected by its clean-shutdown file and broker epoch) from the eligible leaders; Kafka's own
design notes that a replica rejoining the ISR "must fully re-sync again even if it lost unflushed
data in its crash".

D5 also closes Track C D11's stated limit on votes (`REPLICATION.md:183-189`: a vote in a damaged
newest turn is unproven) for groups that allow memory acknowledgements: such a voter restarts
forgetful, votes nothing until it re-learns, and re-learning takes it to its leader's term, past any
term whose vote it may have lost. Extending the rule to `strict` groups would close the limit
everywhere, at the price of D6's wait after every correlated power loss; it is offered to a host on
the turn log as an opt-in, not proposed as a default.

**Residual.** A writeback error between the crash and the reopen that the kernel never reports:
Linux 4.17 and later deliver an unseen error to the next `fsync` once (errseq), but an inode
evicted under memory pressure loses it (PostgreSQL's "Fsync Errors" wiki and the errseq-v4.17
pull). That case is outside the model, as a disk that acknowledges unpersisted flushes is
(REPLICATION.md:17).

### D6. Recovery after a correlated loss

When more than f voters are forgetful at once, no quorum of voters that are not forgetful exists,
so D5 alone stalls the group. A forgetful voter granting anyone before then could elect a log that
lacks a write another voter still holds; a voter cannot tell "more than f forgetful" from "f
forgetful and the rest unreachable" without a signal, and a count of forgetful voters heard over
time does not tell it either (below). D6 makes the count exact with the one thing Raft already
orders: terms.

- **Starting a recovery election.** A voter sends pre-vote requests when its election timer fires
  with no leader, each flagged `forgetful` when it is (kind 6, D16). A voter that has heard such
  flagged requests from f + 1 distinct voters of its membership in force (f of that membership, D2),
  itself counted when it is forgetful, since its restart or since it last heard from a leader (the
  owner's Q3 rule), may start a **recovery election**: it moves to its term + 1, votes for itself,
  and sends vote requests flagged `recovery` (kind 1, D16). Any voter may: when the longest log
  belongs to a voter that is not forgetful, only it can win, since a forgetful voter grants it no
  normal vote (the `live3` schedule).
- **Answering one.** A voter that receives a recovery request moves to its term as for any request
  at a higher term, unless it heard from a current leader within `election_min` (Track C's guard,
  `core.h:770`). It grants when it has not voted for another in that term and the candidate's log
  is at least as up to date as its own (§3.6.1), forgetful or not, and its reply carries whether it
  is forgetful (kind 2, D16). A forgetful voter grants nothing else.
- **Winning one (DM-R20).** The candidate leads only when every answer at its term is a grant, a
  majority of its membership in force answered flagged `forgetful` (⌊n/2⌋ + 1 of n voters, its own
  counted when it is forgetful: f + 1 at an odd n, so steady-state groups count as revision 1 did,
  and f + 2 at an even n, Track C's replacement windows), and either every voter of that membership
  answered or `recovery_timeout` passed with no leader. A refusal ends the round; the candidate may
  start another at the next term. The start condition stays f + 1: it only lets a round begin.
- **Why the count at the election's own term is exact.** Take the first recovery election ever
  won, by a candidate X at term U whose membership C has n voters, and its flagged answers, a
  majority of C. Each answerer was at term U or below when it answered (one above U refuses, which
  ends the round), and stays forgetful until it re-learns from a leader of term U or later, across
  any restart (D5's mark). No other leader of term U exists: the candidate's grants and its would
  be two majorities of one term. Consider the first leader W of a term above U elected between the
  first flagged answer and the decision. It won a normal election, since no recovery election won
  before, so its voters were not forgetful when they granted: an answerer that had answered was
  still forgetful then, with no leader of term U or later to re-learn from before it, and one that
  answers later was still at a term at or below U, so it had not voted at the later term; X itself
  voted at U and would have left its round to vote at W's term. W counts its own membership C',
  which is C or one change away from it: Track C admits a change only when no configuration entry
  lies above the leader's `commit_durable` (its D3, the rule that closes the single-server bug;
  D7's site table), so the memberships that can elect at once are a membership and its successor.
  W's quorum avoids X and every flagged answerer in C'. With C' = C at most n − (⌊n/2⌋ + 1) voters
  remain; with C' = C plus a voter, at most (n + 1) − (⌊n/2⌋ + 1) = ⌈n/2⌉ < ⌊(n + 1)/2⌋ + 1; with
  C' = C less a voter, which may be a flagged answerer or X, at most (n − 1) − ⌊n/2⌋ <
  ⌊(n − 1)/2⌋ + 1. Each is fewer than a majority of C', so W does not exist. No answerer re-learns
  before the decision, so at the decision a majority of C, more than f of it, is forgetful at once,
  beyond the model. Within the model no recovery election wins, and every leader is D5's.
- **Why f + 1 is not enough across a membership change (DM-R20).** Revision 1 counted f + 1 flagged
  answers and argued with one membership (2f + 1 − (f + 1) = f). In rf = 5's replacement window a
  candidate X of the six-voter membership needs 3; a leader W of the five-voter membership that
  removes the leaving voter Q needs 3 of 5, and with X and one flagged answerer P excluded, 3
  remain. So W is elected between X's answers, P re-learns from W, and Q loses power after that and
  answers X: three flagged answers from episodes that never overlap, at most two voters forgetful at
  any instant. The architect's `six` decides without an acknowledged `memory` write in 43 steps, and
  `six_loss` (49 steps) then replaces it on every replica that held it. The growth half of the same
  window fails the same way: in `grow6` (48 steps, constructed here) X counts under the six-voter
  membership that promotes the learner, and W, which never synced the promotion, is elected under
  the five-voter founding membership it still holds. With the majority count, both stop at the
  decision (four flagged answers needed, three exist), and the bounded searches near them find
  nothing (model evidence below). At rf = 3's four-voter window f + 1 = 2 flagged answers overlap in
  time for a forgetful candidate; the majority count asks 3 there too, one rule for every n.
- **Why the count since the restart is not.** At five voters (f = 2), voter 1 loses power and hears
  voter 0 forgetful, voter 0 then re-learns a write from voter 2, and voter 2 later loses power:
  voter 1 has heard three forgetful voters, while at most two were forgetful at any instant. It
  campaigns without the write and wins with two voters that never held it, and voter 0 still holds
  it (the `fplus5` schedule, 15 steps). Random search finds the same failure on its own: the
  checker's swarm walks turned up `fplus5b` (26 steps), where a voter counts three forgetful voters
  over episodes that never overlap and an honest follower must later replace a committed entry.
  Revision 0's every-voter-heard rule fails the same way, since voter 1 heard voter 0 before voter
  0 re-learned (`heard5`). At three voters both rules hold within the model, because two forgetful
  voters at once are already beyond it.
- **Policies** (`unclean_recovery`, a replicated index setting, D15; the administrative privilege,
  DMS-9). `balanced` (default for `replicated` and `available`, Q3): `recovery_timeout` 5 minutes,
  KIP-966's. `aggressive` (`relaxed`'s default): `recovery_timeout` of one election timeout; it now
  loses only beyond the model too. `manual`: no timeout and no recovery election of its own; an
  operator's statement through the peer transport's admin path (signed, audited,
  `peer-transport.md` D17; DMS-9) lets one recovery election run under `balanced`'s rule. The
  same statement may admit a count of f + 1 for that one election, which is the operator's
  admission of loss the dissertation's §3.8 describes, for the window stall below.
- **The `Recovery` entry.** A recovery leader writes a `Recovery` entry first (its term and its last
  index at election, 16 bytes), then its no-op, and every append it and its successors send names
  the term of the newest `Recovery` entry in their log (`recovery_term`, D16). The entry exists only
  in groups whose configuration has `acks: written`, under its own storage batch magic and, in a
  checkpoint's retained suffix, its own bundle magic, which older readers refuse by magic as they
  refuse membership batches (`storage.h:46-48`, `241`; `checkpoint.h:71-72`), and which N's readers
  classify as unsupported, never as damage (DM-R7, D16).
- **Replacement.** A follower replaces a conflicting entry above its `commit_memory` as Raft always
  does. It replaces one at or below its `commit_memory` only when the entry is above its
  `commit_durable` and older than the append's `recovery_term`, and never one at or below
  `commit_durable`. Anything else still fails closed (`core.h:868-871`), so a conflict outside a
  recovery stays a detected bug. After such a replacement its `commit_memory` falls to the
  divergence point, and if its applied index passes that point, it serves no read until it repairs:
  `Repair` at its newest checkpoint, which D1 keeps at or below `commit_durable` and so below the
  divergence point, then redelivery from its own log (Track C's D10, `core.h:597-609`). Replicas
  never diverge.
- **Loud, not covert (DMS-3).** A leader that wins a recovery election logs at warning level each
  forgetful grant that made it (incarnation and node), with its own last index and term, and raises
  `recovery_grants`; a follower that replaces entries under `recovery_term` logs the range (first
  index, count, their terms) and raises `recovery_replaced`. A lying voter that flags `forgetful`
  falsely can complete the decision's count with f honest forgetful voters at an odd membership
  (f + 1 in a window); that is the compromised-voter row of the threat model.
- **Never lost:** a durably committed entry (synced on a majority M), so every `quorum` and `all`
  acknowledgement. Any recovery majority intersects M in a voter whose durable log holds the entry,
  and that voter grants only a log at least as up to date as its own.
- **Lost:** `memory` writes no voter of the recovery majority held (under `balanced` with every
  voter answering, only those no voter held at all), and `leader` writes never committed. Readers
  may have seen the `memory` ones, since visibility follows `commit_memory` (D7). A session token
  naming a lost write reports it lost (D10).
- **Availability** (the fault table): at rf = 3, one forgetful voter and one unreachable voter stop
  elections until either returns or the forgetful one re-learns, where `strict` would elect. In a
  replacement window the majority count adds one stall (DM-R20): with exactly f + 1 voters
  forgetful at once and every candidate holding the even membership (six voters at rf = 5, four at
  rf = 3), no normal quorum of voters that are not forgetful exists and a decision needs f + 2
  flagged answers, so the group waits for an operator's `manual` statement (above), unless a
  candidate holding the window's odd membership can count a majority of it. The window lasts two
  commits after the learner caught up (`track-c.md:267-268`); revision 1's f + 1 would have
  decided there, and lost writes in rf = 5's window doing so.

**Model evidence** (DM-R1). `bench/durability/model/forgetful_model.py` extends the architect's
probe (atomic steps; partitions and process crashes unbounded; at most FMAX voters forgetful at
once) with failed elections, each voter's `commit_durable` and the floor and fence checks, the
owner's rule (`fplus`), and revision 1's recovery election (`round`, any voter with the count
starting one, one answer per step, so other steps interleave). `forgetful_check.cc` is its C++ port,
reducing states by voter symmetry; with `--compat` both reproduce the architect's counts exactly
(696,862, 1,209,748, and 1,970,161 states at depth 10), and the two agree on every revision-1
configuration compared (`results/model-crosscheck.out`). Results (`results/model-*.out`):

| Voters | Rule | Within the model (at most f forgetful at once) | Beyond it |
|---|---|---|---|
| 3 | `timeout` (revision 0's D6) | `memory` lost: DM-R1's trace (`dmr1`, 7 steps) | Not run |
| 3 | `none` (D5 alone), `heard` (revision 0's every-voter-heard rule) | Hold: the architect's state spaces at depth 10, reproduced exactly | Not run |
| 3 | `fplus` (Q3 as ruled) | Holds: 31,776,242 states at depth 12 (two writes, terms to 4, three OS crashes; voter symmetry, 64-bit keys), and 6,391,864 at depth 10 in both checkers (one write, terms to 3) | Not run |
| 3 | `round` (revision 1) | Holds: 48,190,011 states at depth 12 (two writes, terms to 4, three OS crashes; voter symmetry) | No `quorum` or floor loss: 32,162,295 states at depth 11 (64-bit keys), and 22,599,017 when every voter must answer (`round_heard`); `memory` losses, as beyond the model allows, the shortest of each (11 steps) a write no voter still held |
| 5 | `timeout`, `heard`, `fplus` | `memory` lost: `fplus5` (15 steps; `timeout`, `fplus`) and `heard5` (15 steps; `heard`); fences found by swarm walks (`heard`, 25 steps; `fplus`, `fplus5b`, 26 steps) | Not run |
| 5 | `round` (revision 1) | Holds: 67,817,052 states at depth 8 (one write, terms to 4, three OS crashes; voter symmetry, 64-bit keys); 20,000,000 kind-first swarm walks to depth 30 (445,216,259 steps; terms to 5) find nothing; the forced `fplus5`, `fplus5b`, and `heard5` are blocked | No `quorum` or floor loss: 79,154,748 states at depth 8 (64-bit keys; no loss of any kind that shallow), and 20,000,000 walks to depth 30 (443,382,315 steps; four OS crashes), which find 82 `memory` losses, as beyond the model allows |
| 3 | `round`, liveness | Not applicable | `live3`: the one voter that holds the write is not forgetful, starts a recovery election, and wins it; nothing lost |

The swarm walks are weaker than exhaustive search at a given depth, and calibrated as such
(`results/model-swarm.out`): kind-first walks find the `timeout` rule's loss 197 times in 300,000
walks, while 20,000,000 uniform walks missed the `fplus5b` pattern that kind-first walks found once
in about 10,700,000. Walks with two writes and terms to 6 (10,000,000 each, to depth 40) found
nothing under `round` and nothing under `fplus` either, so at that size they are no evidence and
the table does not count them. So the exhaustive depths are the stronger evidence, and the
one-write walks reach where they cannot.

**Revision 2's runs** (DM-R19, DM-R20; `results/r2/`, each under `run_bounded.sh`'s cap at
`nice -n 19`, one thread; the depth, states, and time are each run's own, and the procedure is
`bench/durability/model/runs_r2.sh`). The model gains, each off unless asked for: same-boot process
restarts, which clear forgetfulness as revision 1 read D3 (`--restart text`) or keep it until
re-learning (`--restart persist`, what D3's V and D4's mark implement); torn same-boot tails, which
keep any prefix from the synced one and make the voter forgetful (`--torn`); retried rounds
(`--retry`); membership in the log, a node's membership in force being the newest configuration entry
in its synced prefix, a change admitted only with an entry of the leader's term at or below its
`commit_durable` and no configuration entry above it, and every count taken over the counting
node's membership (`--base`, `--script`); and the decision's count, f + 1 or a majority
(`--count`). Every revision-1 configuration keeps its state count in both checkers, and the two
agree on every new feature compared, six voters included (`results/r2/crosscheck.out`). The C++
checker replays the schedule files in `bench/durability/model/schedules/` and searches from a
schedule's prefix.

| Run | Scope | Depth | States | Time | Result |
|---|---|---:|---:|---:|---|
| Restarts as revision 1 read them | 3 voters, one write, terms to 2, two OS crashes, one restart | 8 | 1,489,228 | 4 s | `memory` and fence at 7 steps: `restart3` found by search (the architect's count, which Python reproduces) |
| Torn tails as revision 1 read them | 3 voters, one write, terms to 2, no OS crash, two restarts | 8 | 864,657 | 3 s | `memory` and fence at 7 steps, `torn3`'s shape |
| The mark, torn tails | 3 voters, one write, terms to 3, no OS crash, two restarts | 10 | 15,605,421 | 43 s | Holds |
| The mark, torn tails, retried rounds | 3 voters, two writes, terms to 4, three OS crashes, two restarts; symmetry, 64-bit keys | 11 | 81,276,327 | 539 s | Holds |
| The same | The same | 12 | RUN-N3-D12 |
| The mark, torn tails, retried rounds | 5 voters, one write, terms to 4, three OS crashes, one restart; symmetry, 64-bit keys | 7 | 18,393,574 | 249 s | Holds |
| The same | The same | 8 | RUN-N5-D8 |
| Forced: `restart3`, `torn3` | 3 voters | 7 | | | `memory` lost as revision 1 read restarts; blocked at the restart under the mark (`schedules-cc.out`, `schedules-py.out`) |
| Forced: `six`, `six_loss` | 6 voters, rf = 5's window, node 2 leaving | 43, 49 | | | f + 1: `memory` lost, at most two forgetful at once, and in `six_loss` no replica holds it; majority: stops at the decision |
| Forced: `grow6`, `grow6_loss` | 6 voters, rf = 5's window from five founders, node 5 promoted | 48, 52 | | | The same |
| From `six`'s step 38, f + 1 / majority | The removal synced on two voters, two forgetful episodes past, the round started | 5 | 11,792,871 / 11,792,870 | 52 s each | f + 1: `memory` at 5 steps; majority: holds |
| From `grow6`'s step 43, f + 1 / majority | The promotion synced on four voters, the round started | 5 | 13,519,083 / 13,519,082 | 58 s each | f + 1: `memory` at 5 steps; majority: holds |
| From `six`'s step 37, f + 1 / majority | One step earlier | 6 | RUN-SIX37 |
| From `six`'s step 33, both counts | Three OS crashes done, nothing decided | 5 | 40,827,263 each | 166 s each | Both hold; f + 1's loss is 10 steps away |
| From `grow6`'s step 38, both counts | The same point of the growth half | 5 | 44,300,503 each | 167 s each | Both hold; 10 steps away |
| rf = 3's window from its founding state, both counts | 4 voters: 3 founders, node 3 promoted, node 1 removed; one write, terms to 3, two OS crashes; symmetry | 10 | 16,223,354 each | 112 s each | Both hold; no recovery decision is reachable that shallow |
| rf = 3's window from `rf3win`'s step 24, both counts | The promotion synced on all four, `v0` at `memory`; terms to 4, two OS crashes | 8 | 69,156,115 each | 208 s each | Both hold, with equal counts: no decision differs within 8 steps |
| Walks, calibration | From `six`'s step 33, f + 1, 2,000,000 kind-first walks to depth 14 | 14 | 26,553,911 steps | 149 s | Nothing found under f + 1 either, so walks are no evidence at six voters and the table counts none |

**The model's limits (C14).** Messages are atomic, a request and its effect in one step, and there
is no pre-vote phase beyond the flagged probes, so the model cannot interleave a vote's delivery
with other steps; partitions and process crashes are unbounded, but restarts are bounded by
`--maxr`, writes by `--maxp`, terms by `--maxt`, and OS crashes by `--maxos`, as each row states.
Membership changes follow one script, a promotion then a removal, over given founders: sequences of
more than two changes, witnesses, and learners never promoted are outside it. Forgetful voters are
bounded over all nodes, learners and removed members included, which restricts the adversary
slightly. The six-voter searches start from the schedules' prefixes and reach 5 or 6 steps, so they
show that the majority count blocks every decision f + 1 makes within that reach, not that no other
six-voter schedule loses a write; D6's argument covers the rest, and T3's simulator profiles extend
it (C10). The C++ runs compact states into 128-bit keys, or 64-bit ones where a run says so.

A counterexample is real; a pass is evidence bounded by the depth, the scope (one or two writes,
terms up to 4 or 5, three or four OS crashes), and, for the C++ runs, hash compaction: 128-bit keys,
or 64-bit ones (`--key64`) where a table would not fit the host's memory, omitting a state with
probability about n² / 2⁶⁵ at n states.

Precedents: Kafka's `unclean.leader.election.enable` (false by default since 0.11) and KIP-966's
`unclean.recovery.strategy` (Aggressive, Balanced, None; "the one with the highest leader epoch
plus the longest log end offset will be the new leader"); RAMCloud, which after a correlated power
failure waits for backups to reboot unless "an operator can request that the system continue with
some loss of data"; and §3.8: "If a majority of the cluster loses its persistent state ... a
system administrator would need to admit the possibility of data loss." Here the loss is bounded
by the memory window, the decision is fenced by a term, and the policy is per index.

### D7. Two commit cursors

| Cursor | Counts | Drives |
|---|---|---|
| `commit_memory` | A majority wrote the entry (the leader's own write included, as §10.2.1 counts the leader's disk write in parallel) | Apply and its verdicts, `memory` acknowledgements, refresh, the index a linearizable read captures, the uncommitted cap, delivery |
| `commit_durable` | A majority synced it | `quorum` acknowledgements (`all`: every voter synced), the checkpoint ceiling (D1), D6's floor, membership admission and promotion, witness truncation, `Repair`'s bound, and the hard state's commit index |

- **Groups that forbid memory acknowledgements** (`acks: synced`, or no `acks` field, in the
  configuration entry, D16) answer appends only after their barrier, so the two cursors are equal:
  today's Raft, and Detent's fixed groups unchanged.
- **One `acks` rule (DM-R18).** A node's own replies and forgetfulness follow the newest durable
  configuration entry, as every membership rule does (`track-c.md:216-218`); the leader's client
  acknowledgements follow the committed one. Switching to `synced` syncs everything before it (D4).
  `acks: written` is a log format (D16): every replica refuses to apply it before activation.
- **Apply follows `commit_memory`** so a `memory` reply carries its verdict: expected-version checks
  and schema checks are evaluated at apply and reported by the leader (REPLICATION.md:146-149). The
  rejected alternative, apply at `commit_durable`, needs no rewind after a correlated loss, but
  every reply with a verdict would then wait for the next barrier, up to `sync_interval` (100 ms),
  slower than `quorum` for a single client.
- **`leader`** acknowledges at the leader's own write, before commit, so its reply names the
  batch's log position and no verdict; `none` acknowledges at admission. Conditional writes need
  `memory` or stronger.
- **Core events**: `Written{token}` (new, additive) before `Persisted{token}`. In a group that allows
  memory acknowledgements the core starts the next storage batch at `Written` (one write in flight,
  D3), sends a leader's appends at `Written` (§10.2.1), and releases actions that need durability
  (a vote grant, a term change) only at `Persisted`. A host that never reports `Written` gets
  today's behavior.
- **The `synced` path (DM-R4).** Every append reply and coalesced reply record carries the
  follower's `synced` index beside `matched` (its written one). A follower answers an append whose
  `sync_through` reaches its entries only after the barrier that covers them, so the reply carries
  both; others it answers at `Written`. The leader takes a coalesced record's `synced` without a
  flight, keyed by term and incarnation as Track C's records are, clamped to `matched`, and takes
  every append reply's `synced` the same way, before the flight rule can drop the reply: a reply
  with no flight drops (`core.h:940`) and a later reply retires every older flight
  (`core.h:946-948`), so a follower that answers a later append at `Written` before an earlier one
  that waits for its barrier would otherwise lose the report a `quorum` write waits for until its
  next reply (DM-R22 item 3). Replies need no order. A group quiesces only when `commit_durable`
  equals its last index and every member acknowledged an append or heartbeat record carrying it: the
  leader's last heartbeats before quiescence carry `sync_through` at the last index. So an idle
  group's checkpoints and log truncation never stop, and a `quorum` write never waits on a quiesced
  group.
- **`quorum` latency.** In a group that allows memory acknowledgements, the leader sends at
  `Written` and the write's `sync_through` forces both barriers at once: one barrier, the slower of
  the leader's and the follower's, plus a round trip. In `strict` the leader persists before it
  sends (`core.h:470-473`, `1088-1090`): two barriers and a round trip. T10 measures both.
- **The wire (DMS-6).** The append's `commit` field and the heartbeat record's commit carry
  `commit_durable` to every session, which is what a reader one version back takes as its commit
  index and persists (`core.h:785`, `914`); a new field carries `commit_memory` (version 3, D16).
  The follower clamps both to its verified prefix (`core.h:914`; heartbeat records clamped by the
  leader to that follower's matched index in the current term, `track-c.md:629-633`), so an honest
  follower with a stale suffix never learns a floor above entries it must still replace. A member
  on a version-2 session replies only after its barrier, as a version-2 binary does, so its
  `matched` is synced and the leader counts it for both cursors; a forgetful voter grants nothing
  over such a session, and a leader rewinds such a member never (its `matched` is already its
  floor). One member's version holds back only itself, and the group keeps its `acks`.

**Every `durable_` read in the core, and its cursor (DM-R5).** `core.h` at `61149a2`:

| Sites | Today | Revision 1 |
|---|---|---|
| `46`, `454`, `460` | `durable_` from recovery, a published install, a completed batch | Unchanged: the persisted hard state, whose commit index holds `commit_durable`; `commit_memory` starts from it at recovery and at install (an image is at or below its source's `commit_durable`, D1) |
| `188`, `497`, `502`, `577`, `586`, `785`, `1185` | The persisted hard state read, compared, or carried | Unchanged; at `785`, a `commit_durable` change rides the bounded barrier (D4) |
| `147`, `548`, `982`, `1108` | `durable_.term`: a transfer's leader term | Unchanged: terms |
| `86` | `committed()` | `commit_memory`; `durably_committed()` returns `commit_durable` |
| `193`, `324` | The committed membership; the wire-limit cache key | `commit_durable`: a membership is committed when durable |
| `198-199`, `626-627` | Membership admission: an entry of the term committed, no configuration above the commit | `commit_durable`, so a second change never rests on a first that a correlated loss can remove |
| `650` | A promoted learner's progress at least the commit | Its synced progress at least `commit_durable`, and its written progress at least `commit_memory` |
| `1047` | The replacement's removal waits for an entry of the term committed | `commit_durable` |
| `484` | An install at or below the commit is caught up | `commit_memory`: the log already holds it |
| `562` | A checkpoint refused when apply passes the persisted commit | Refused when `through` passes `commit_durable` (D1); `through ≤ applied` stays |
| `602` | `Repair`'s boundary at most the commit | `commit_durable`, where images are (D1) |
| `612` | An application completion past the commit fences | `commit_memory` |
| `722` | The uncommitted cap | `commit_memory`: it bounds what `leader` acknowledgements risk |
| `739`, `747` | A read needs an entry of the term committed, and captures the commit | `commit_memory` |
| `1131` | The append's `commit` | `commit_durable` (DMS-6); the new field carries `commit_memory` |
| `1158` | `advance_commit`'s floor | Two counts: `commit_memory` over written matches, in memory; `commit_durable` over synced ones, persisted at the bounded barrier |
| `1163`, `1165` | Delivery up to the persisted commit | `commit_memory`, in memory, so apply never waits for a barrier |

Beyond `durable_`: the fail-closed replacement (`868-871`) takes D6's rule; the witness's bound
(`913`) takes `commit_durable` and `data_matched` (`1136-1145`) the data voters' synced progress,
so a witness never truncates what the data voters hold only in page cache; the clamp (`914`)
covers both cursors; `matched` (`945`) is the written index; the rewind (`957-969`) takes D5's
exception; and the leader's own match in `advance_commit`, `last_index()` (`1151`), becomes its
written index for `commit_memory`, its synced one once D2's cap binds, and its synced index for
`commit_durable` (DM-R22 item 2).

### D8. Segment mode

- **Followers append, never index.** The log carries the prepared batch in both modes. A
  segment-mode follower appends and acknowledges it (counted for both cursors), acknowledges its
  deliveries without applying their document batches, as Track C's damaged host does
  (`track-c.md:425-431`), applies only strata's control commands (`Image`, settings), and keeps the
  leader's segment files instead.
- **The leader announces each shippable state in the log.** After a refresh publishes a flush or a
  merge, the leader proposes an `Image` command (a strata command, no new consensus kind) naming its
  state at its applied position p: each segment's physical identity, size, CRC-32C, and SHA-256
  (OpenSSL's EVP, computed once as the flush or merge writes the file), and, for a new segment, the
  log range it covers (D18); the live-docs generations; the metadata reference. Document-mode
  replicas skip it at apply, so the digest is unchanged.
- **Segment identity (DM-R16)** is format 13's (F13-R41, `format-13.md:470-482`): the producing
  term, then a base drawn at open plus a count through splitmix64's bijection, recorded in the
  manifest and checked at open and install, and kept by install. Ids from different terms never
  collide, and two replicas flushing in one term (document mode) collide only by chance, about
  n² / 2⁶⁵. Since a footer's CRC binds no prefix, a file a follower "holds" must match the `Image`'s
  identity, size, CRC-32C, and SHA-256 of the whole file.
- **Installing.** A segment-mode follower that applies a committed `Image` asks a member for the
  files it lacks (D17's `Request`), verifies each whole as it arrives (D17), and installs the image
  once its files are present: searchable at its next refresh, durable as a checkpoint only once
  the entry's index is at or below `commit_durable` (D1). An install is `index::install` made
  incremental: today's writes every segment under a fresh local name (`index.hh:751-812`,
  `773-776`); the incremental form keeps the files the receiver holds and writes the rest.
- **Not the consensus install path.** An install at or below the commit index is refused
  (`core.h:484`); an `Image` is a log entry whose files move through D17, not a snapshot.
- **Promotion.** A segment-mode follower that wins an election rewinds its application to its
  installed position p and replays p + 1 to `commit_memory` from its own log before it applies new
  entries. Checked against consensus's repair path (`core.h:597-609`):
  1. `Repair` refuses a leader of more than one voter (`core.h:600`). Transferring helps no one in
     segment mode, where every follower is equally behind, so `Repair` gains a `promotion` flag
     the core accepts from a leader when its host declares segment mode; document mode keeps the
     refusal.
  2. `Repair` needs its boundary at or above the log's base (`core.h:602`), while a checkpoint is
     bounded by the applied cursor (`core.h:562`), which a segment-mode follower advances as it
     acknowledges deliveries, ahead of its installed state. The core gains a checkpoint ceiling the
     host sets (additive), and a segment-mode follower checkpoints only its durably installed image.
  3. `Repair` waits for no delivery outstanding and no persistence (`core.h:599`): after the new
     leader's no-op, one delivery, which the segment-mode host acknowledges at once.
  4. `Repair` advances the application epoch (`core.h:606`), so no capture from before the rewind
     becomes a base.
  5. A position inside an entry rewinds to the entry before it; strata skips the transactions it
     already applied (`index.hh:415-418`).
  During the replay, new writes are appended and committed, and their replies wait for their
  verdicts; reads wait for the applied cursor (`core.h:1176`). D9 bounds the replay.
- **Catching up past the replay bound (DM-R10).** A follower whose install lag passes D9's bound
  applies the committed tail itself through the document-mode path it keeps for mixed versions,
  into its own buffer, flushing only when that buffer fills, until an installed `Image` covers its
  applied position; its own segments for that range then give way to the leader's. So the replay
  at promotion stays within `election_min` whatever the leader's uplink, a follower's disk, or a
  mode switch (D15) does, at that follower's own CPU, and the leader never flushes early to hold
  it (D9). At worst a follower does document mode's work and then receives the leader's files: one
  mechanism, the one the mixed-version rule already needs.
- **Merges are shipped** (Q4, decided): a follower receives each merge's output. The no-stalls
  survey's copy merges (I5, about 20× cheaper per merged byte) would make replaying merges cheaper
  than shipping their output; Q4 is revisited then.
- **Mixed versions (DMS-8).** The leader writes its best activated segment format. A member that
  cannot read an installed segment (`unsupported_segment` at install) applies the batches itself
  from its installed position. No member announces formats, so no member's claim holds the
  group's format back.
- **Followers' reads (DM-R11).** A segment-mode follower's visible state trails the leader's by the
  leader's refresh, the `Image`'s commit, the transfer, and the install. `prefix`, `bounded`, and
  `session` reads on followers see that lag (D10).
- **The cost, redone (DM-R12).** From T2's measured parts on one thread at 100,000 documents
  (`results/2026-10-18-t2-macos.txt`), plus labeled assumptions: SHA-256 through OpenSSL's EVP at
  0.5 GB/s without SHA extensions (the development Mac's i9-9980HK has AVX2 and AES-NI, no SHA-NI)
  or 1.5 GB/s with them, hashed once by the leader and once by every follower whatever the
  source (DMS-5); TLS 1.3 and the kernel's TCP path at 1 to 2 ns per byte moved, both ends together,
  on the log in both modes and on the segments too in segment mode; D18's verifier at its default
  rate. Figures from `arith.py` (`results/arith.out`), estimates all:

  | Configuration | Follower, segment against document | Group at rf = 3 | Group at rf = 5 | Bytes per follower |
  |---|---:|---:|---:|---|
  | One flush (256 MiB buffer), 1 segment | 0.067× to 0.098× (0.082× to 0.112× with the verifier) | 0.394× to 0.424× (0.404× to 0.434×) | 0.271× to 0.307× (0.283× to 0.318×) | 1,179 B/doc of log; 1,614 with segments (+37%) |
  | 16 MiB buffer, 37 segment files | 0.036× to 0.057× (0.051× to 0.072×) | 0.366× to 0.388× (0.376× to 0.399×) | 0.238× to 0.263× (0.250× to 0.275×) | 1,179 B/doc of log; 2,394 with segments (+103%) |

  Revision 0's 0.370× and 0.350× left out the hash, TLS, the stack, and the verifier. The bracket
  for one large index: segment mode held to the 16 MiB cadence against document mode at one flush
  costs 1.165× at rf = 3 and 0.803× at rf = 5 of document mode's CPU with the verifier the table
  counts (1.135× and 0.767× without it; `arith.out`, DM-R22 item 4). T2's targets sit 1.6 to 4
  points above the estimate's top, and that margin rests on EVP's rate, which nothing here
  measured: at one flush and 2 ns per byte the follower's 0.12× target fails below about
  0.36 GB/s without SHA extensions (0.121× at 0.35 GB/s), and the group targets near 0.25 GB/s
  (0.453× and 0.338× against 0.45× and 0.33×; `arith.out`, DM-R22 item 5). T2 measures the rate
  on each platform before it judges the targets. Revision 1 never holds segment mode
  to a smaller cadence: the leader flushes on the no-stalls controller's schedule in both modes
  (D9), and a lagging follower applies the tail itself; so at equal cadence the table holds, and
  where the refresh is relaxed for bulk ingest segment mode tends to document mode's cost plus its
  shipping, hashing, and verification. Where many indexes share the node's buffer
  budget (ARCHITECTURE.md:102-103), both modes flush small, the 16 MiB row's case. OpenSearch
  reports segment replication at 40% higher throughput in its first benchmarks and, at 9 replicas,
  p50 CPU falling from 69.857% to 8.833%, with a loss in one configuration. T2 measures.

### D9. The failover gap, bounded

The gap from a leader's failure to the next acknowledged write, at the default profile (heartbeat
100 ms, elections drawn from 1,000 to 2,000 ms, `track-c.md:376-380`; arithmetic in `arith.py`,
with b = 80 ms and a 1 ms round trip):

| Part | Document mode | Segment mode |
|---|---|---|
| Detection | The election timer: at most `election_max`, 2,000 ms; a quiescent group adds three missed 500 ms keepalives (`track-c.md:647-650`) | The same |
| Pre-vote | A round trip: a pre-vote persists nothing (`core.h:764-767`, `1007-1010`) | The same; a forgetful voter waits per D6 |
| Vote | Two barriers and a round trip, 161 ms (`track-c.md:836-838`) | The same |
| First commit | `quorum`: the no-op persisted, then sent (`core.h:1088-1090`, `470-473`), two barriers and a round trip; `memory`: sent at `Written`, a round trip | The same |
| Replay | None | At most `election_min`, 1,000 ms (D8's catch-up) |
| Total (estimate) | 2,323 ms for `quorum`, 2,163 ms for `memory`; 1,500 ms more when quiescent | 1,000 ms more |

- **The client's part (DM-R17).** A client's request to the dead leader waits out strato's request
  timeout, then retries through its routing; the client sees the gap above plus that timeout. T8
  reports both, at the new leader and at the client.
- **The cadence (no-stalls, DM-R10).** The leader refreshes on the schedule `research/no-stalls`
  recommends (its section 6, items 1 and 3): one second by default, the flush on the pool's flush
  class, the interval stretched under merge debt up to the 30 s cap that note names, and admission
  slowed only past a high mark, never stopped. Each refresh that publishes a flush or a merge
  proposes an `Image`. Revision 0's rule, flushing early whenever a voter's install lag neared the
  bound, is withdrawn: it made more, smaller segments and more merge output to ship, the opposite of
  that controller's first lever ("flushing every 10 s instead of every 1 s is one tier less", its
  §2.1). D8's catch-up holds the bound instead.
- **The replay bound** is `election_min` divided by the follower's apply cost per document. The
  replay is apply only, since the flush runs on the pool (no-stalls item 1): about 100,000
  documents at 10 µs each (no-stalls §2.1, an estimate here); at T2's whole per-document cost
  (apply, flush, merge, and commit), 23,958 and 7,798 documents (`arith.out`), the conservative
  figures until T8 measures.
- A segment-mode follower whose install lag passes the bound adds the excess to its election timer,
  so the most current follower usually wins; that affects liveness only.
- During the replay, writes are accepted and committed, and their replies wait for their verdicts:
  the gap is latency, not refusal.
- **Leases** (optional, off by default; DM-R14): a leader serves linearizable reads without a probe
  until its heartbeat's start plus `election_min` divided by the clock-drift bound (§6.4.1), and
  expires its lease before transferring. Today a restarted voter grants at once (`core.h:765`,
  `770`, `803`: leader contact lives only in memory; `core_probe.out` shows a pre-vote granted 1 ms
  after a restart). So with leases on, a voter that starts grants no vote or pre-vote for the lease
  duration after its start, as if it had heard a leader then, and a new leader serves lease reads
  only after a quorum answered a heartbeat of its term and it committed an entry of its term.
  CockroachDB's leader leases (25.1) report recovery "within seconds".
- After a correlated loss of more than f voters, D6's policy adds its wait: until every voter
  answers under `balanced`, at most `recovery_timeout`.
- `relaxed` adds a loss, not a gap: writes the old leader acknowledged and no majority held are
  truncated at the failover.

### D10. One spectrum, one vocabulary

**Acknowledgement levels** (`ack`), strongest first. REPLICATION.md's `local` retires: at rf = 1 it
is `quorum`, and at rf > 1 a write synced only on the leader is truncated at failover like a
`leader` write (Q6, decided).

| Level | Acknowledged when | Survives | Lost when (window) |
|---|---|---|---|
| `all` | Every voter synced it | Any f failures; every replica current | Never within the model; times out while any voter is down |
| `quorum` | A majority synced it (`commit_durable`) | Any f failures of any kind | Never within the model |
| `memory` | A majority wrote it (`commit_memory`), with its verdict | Every failure within the model (D2) | Beyond it: more than f voters forgetful at once, and the write in the capped memory window (D2) where no recovery majority held it (D6) |
| `leader` | The leader wrote it; no verdict | Follower failures; a leader process crash when the leader wins the next election again | When another voter wins the next election before a majority wrote it: after a leader's restart, its peers' timers fire first (`core.h:432`), and the first to campaign wins with the other's grant (`core.h:765`, `803`, `1002-1010`); the leader's OS crash before its barrier |
| `none` | The leader admitted it; no verdict | Follower failures | Any leader failure before its write; then as `leader` |

**Read levels** (`read`), strongest first, on Cosmos DB's five-level spectrum:

| Level | Served by | Sees |
|---|---|---|
| `linearizable` | The leader, after a quorum read probe (or within its lease) and a refresh covering the captured `commit_memory` | Every write acknowledged at `memory` or stronger before the read started. Get-by-id reads the live version map; searches pay a forced refresh |
| `bounded(K, T)` | Any replica that received a group message from the leader within T (its own clock) and whose visible position is at most K entries behind the commit index that message carried; otherwise it asks the leader for its commit index (a follower read index) or forwards | At most K entries behind the commit index it learned within T. On a minority side a deposed leader's messages carry a stale commit index until CheckQuorum steps it down, so the real-time staleness is at most T plus `election_max` (DM-R13). A quiescent group sends no heartbeats (`track-c.md:635-637`), so its replicas take the round trip after T. Cosmos DB throttles writes to hold its bound; strata never does (D14) |
| `session` | A replica whose visible position is at least the client's token and whose term history holds the token's term at its index | The client's own writes and monotonic reads. A token is (term, index, position in batch). Each replica keeps a **term history** per group (the first index of each term with a committed entry, as Kafka's leader-epoch cache keeps), past every checkpoint, so a token below the log's base is still checked (DM-R13); a token whose entry D6 lost reports `lost` instead of matching a new entry at the same index. A token above the group's last index known to the replica (its own, or the `leader_last` it received) is refused at once and counted, never waited on (DMS-9) |
| `prefix` | Any replica's latest refreshed snapshot (today's `nearest`, renamed) | The state after some prefix of the one log, which every replica applies in order: Cosmos DB's consistent prefix. A segment-mode follower trails its leader by a refresh, the `Image`'s commit, the transfer, and the install (D8) |
| `eventual` | A replica on a partition's minority side under D12 | Its last prefix plus its own side log's writes; converges after the merge |

**Presets**, each a named set of defaults; every bound is also a knob (D15):

| Preset | Default ack (floor) | Memory acks | Replication | Default read | `sync_interval` | Recovery | Use |
|---|---|---|---|---|---|---|---|
| `strict` | `quorum` (`quorum`) | No | Document | `session` | None needed: every barrier is immediate | Not needed | Writes that must never be lost: authorities (root, placement), Detent's groups, billing-like indexes |
| `replicated` | `memory` (`none`) | Yes | Segment | `prefix` | 100 ms | `balanced` | Most of Xapiand's indexes (Q1, decided); strato's default; shale's default |
| `relaxed` | `leader` (`none`) | Yes | Segment | `prefix` | 100 ms | `aggressive` | Highest single-client ingest; logs and metrics; Kafka `acks=1` users |
| `resident` | `memory` | Yes | Part B | `prefix` | Part B | Part B | Scuba-like analytics (Part B) |
| `available` | `memory`; `provisional` on a minority side | Yes | Segment | `eventual` on a minority side | 100 ms | `balanced` | Writes that must never be refused (D12, later) |

- **Per request**, a write or read may ask for a stronger level at any time and a weaker one down to
  the index's floor (`minimum_ack`, `minimum_read`); a request below the floor is refused, naming
  the floor, and never silently changed. A `quorum` write in a `replicated` index sets
  `sync_through`, so its barriers run at once.
- **Privileges (DMS-9).** Choosing a preset, the floors, `unclean_recovery`, `acks`, and the
  replication mode takes the index's administrative privilege in strato, never a writer's.
- **The stack speaks this vocabulary**: strato (Xapiand's HTTP API, `ack`, `read`, `refresh`),
  shale (the replicated key-value store), the log system on journal and consensus (Kafka's `acks=all`
  with `min.insync.replicas` is `quorum`, `acks=1` is `leader`, `acks=0` is `none`), the analytics
  system (`resident`, `prefix`), and Detent's authority (`strict`, fixed groups).

| Elsewhere | Here |
|---|---|
| PostgreSQL `synchronous_commit`: `on` (standbys flushed), `remote_write` (standbys' OS has it), `local`, `off` (before the local flush) | `quorum`, `memory`, the retired `local`, `none` |
| MongoDB `w: majority, j: true`; `j: false` majority; `w: 1`; `w: 0` | `quorum`; `memory`; `leader`; `none` |
| Elasticsearch `translog.durability` `request`; `async` | `all` (it waits for every in-sync copy); `memory` over every copy rather than a majority |
| Cosmos DB strong, bounded staleness, session, consistent prefix, eventual | `linearizable`, `bounded`, `session`, `prefix`, `eventual` |
| CockroachDB follower reads (`with_max_staleness`, exact staleness of at least 4.2 s) | `bounded` |
| Xapiand async WAL; `DB_SYNCHRONOUS_WAL`; `?commit` | `none` at rf = 1; `memory` at rf = 1; `refresh=true` |

### D11. Rejected: UDP multicast for data

Bandwidth is not the cost; re-indexing CPU is (D8). Multicast is unavailable on most cloud and
Kubernetes networks, and UDP would need ordering, retransmission, fragmentation, and DTLS. The peer
transport (TLS over TCP) stays.

### D12. Partition-tolerant writes: designed now, built later

- **Behavior.** In an index that opts in (`available`), a replica that cannot reach a leader within
  an election timeout accepts writes into a **side log**: a local journal of operations, each
  stamped with a hybrid logical clock (HLC; Kulkarni et al., OPODIS 2014) and its origin
  incarnation, acknowledged `provisional` at the replica's own durability. Its reads at `eventual`
  see its last prefix plus its side log.
- **Merge on healing.** Once the replica reaches a leader, it submits its side log as one `Merge`
  command, an ordinary command in the one log. Apply resolves each operation deterministically
  against the current state: last writer wins by HLC (ties by origin incarnation) for whole
  documents, and conflict-free merges for typed fields, counters as deltas and sets as
  observed-remove sets (Shapiro et al., 2011). Conditional writes on a minority side are re-validated
  at the merge, where they may be rejected; the merge reports each operation's final verdict.
- **The conflict window** runs from the partition's start to the merge's commit. Two concurrent
  writes to one key keep the later HLC; the other is a lost update, reported. Counters and sets lose
  nothing.
- **What the contract needs now**, so the log and apply leave room for it:
  1. The leader stamps each prepared write with its HLC; the prepare step already resolves
     timestamps (REPLICATION.md:61-63).
  2. The determinism contract: apply's conflict rule is a function of state, operation, and HLC only.
  3. **Where the HLC lives (DM-R16)**: format 13 reserves bit 16, `version_clocks`, a `keys`
     sub-structure holding an HLC per document version beside F9's `versions`, unconfigured costing
     no byte (`format-13.md:358`, `1282`, `1292-1302`), and its digest adds the HLC to the `D` record
     only in an index configured with it (`format-13.md:1159-1161`). Format 13 is now done with
     its part.
  4. **Deletes (DM-R16)**: last-writer-wins needs a delete's HLC until the conflict window closes,
     which live-docs bits do not hold. Of the two places format 13 leaves (F13-R52), this design
     chooses **tombstones as keyed documents**: a delete in an `available` index writes a version of
     its key marked deleted, carrying its HLC, so the merge compares one clock per key through the
     keys structure's one lookup whatever the last operation was; merges drop a tombstone once no
     side log can still conflict with it. The live-docs encoding under mask bit 16 stays unused.
     The release of bit 16 fixes the kind and layout (format 13's rule 2).
  5. The HLC is a **foundations library**, not strata code: hybrid logical clocks are on the
     foundations vision's missing list (`foundations-vision.md:57-61`), no Kronuz library has one
     (the catalog's `times` and `epoch` hold monotonic and wall clocks), and shale and Part B's
     retention stamp with it too. Where it lives (its own repository or an extension of `times`)
     is the library catalog's call.
- **Cost if never used**: one setting; an 8-byte HLC per prepared batch in the log; no segment bytes
  (bit 16 stays clear); no apply work.
- **Raft is untouched**: side logs live outside the log until merged, the minority replicas stay
  followers, and a merge is one command. It is not a second replication mechanism.

### D13. Rejected: leaderless replication

Dynamo-style replication (DeCandia et al., SOSP 2007; Cassandra; ScyllaDB's vnodes) as a second
mechanism beside consensus per shard is rejected:

- **Two repair paths**: read repair, hinted handoff, and Merkle-tree anti-entropy, beside the log's
  catch-up, install, and repair.
- **Two failure models**: sloppy quorums beside majorities, so every guarantee in D10 needs a second
  statement.
- **Two test matrices**: the simulator, the checker, and every gate row twice.
- **No total order**: no consistent prefix, no deterministic apply, and no digest to compare
  replicas by, which strata's corruption defense relies on (REPLICATION.md:64-66). Segment mode
  weakens that defense too, which D18 answers.
- What it buys, writes during partitions, D12 gives on the one log. ScyllaDB has moved the other
  way: strongly consistent topology on Raft and tablets with one leader per tablet (ScyllaDB 6.0),
  with Raft-based strongly consistent tables announced beside its Paxos lightweight transactions.

### D14. No stalls: what this design adds to background work

Every task below is background work judged at sustained ingest (PLAN, "Background work is measured
under load", PLAN.md:126-131), scheduled on the shared pool, and reported as a debt in its own unit
to the one controller `research/no-stalls` recommends (its section 6: debt answered first with the
refresh interval, last with admission that follows measured throughput, never with a stop; the debt
unit for merges ScyllaDB's bytes × tiers remaining). This design adds no controller of its own.

| Work | Its debt | Its deadline | The cliff today, and the rule |
|---|---|---|---|
| Checkpoints (D1) | Retained log bytes over the group's share | The share (Track C's D17) | At the retained-log limit, proposals are refused (`core.h:725-730`). Admission follows the measured checkpoint throughput past a high mark, before the limit; the limit, a load-bearing cap of disk per node (Track C's D17, 16 GiB), names its mechanism |
| Barriers (D3) | Unsynced bytes, and the age of the oldest unsynced written entry, per core | `sync_interval` | **The memory window's cap** (DM-R13, DM-R22 item 1): past 4 × `sync_interval` (400 ms) or `max_unsynced` (64 MiB), a core answers at its barrier instead of at `Written`, a follower's append replies and a leader's own match alike (D2), so the `memory` acknowledgements resting on it take `quorum`'s latency, refusing nothing. The mechanism: the bytes and time an OS crash can cost a core beyond the interval. Lucene, Tantivy, and Xapian have no counterpart; they acknowledge only after a commit's syncs. A `quorum` write forces a barrier at once, today's cost for every write (DMS-9) |
| `leader` writes not yet committed | Uncommitted entries | `uncommitted_entries` (1,024, `core.h:722`, counted at `commit_memory`) | Refused at the cap today; `leader` acknowledgements slow with the replication lag first |
| Image shipping and installs (D8, D17) | Install lag per follower | D9's replay bound | Shipping never blocks a write; past the bound, a follower applies the tail itself (D8); D17's limits pace the bytes |
| Segment-mode verification (D18) | Samples queued, and the bytes of the sampled segments they pin | None: under debt a sample waits, never skipped (DMS-11) | Runs at the pool's background class and never delays an install; a sample waits in a queue whose pinned segments are at most `verify_pinned_bytes` (1 GiB per node), and one the queue cannot hold is counted `verify_dropped` and logged, as is one whose log range a checkpoint truncated before it ran (`verify_lost`); the queue's debt is reported to the controller like install lag |
| Copy-forward (Track C D11) | Segments pinned by idle groups | Two segments per core of slack | Bounded slices per turn (`track-c.md:523-535`); its copies replace their originals and segments leave only at the barrier covering them (D3, DM-R6) |

### D15. Live tuning

Detent's integer knobs are process-wide, with no per-group overrides in v1 (Detent's README), so
node-wide defaults and ceilings live in Detent and per-index values in strata's replicated index
settings, which apply at a log position on every replica.

| Bound | Where | Live | Why it is safe |
|---|---|---|---|
| `sync_interval` | Detent (node), capped by each preset | Yes, at the next barrier | It changes only the `memory` window, which D2 states per interval |
| `max_unsynced` and the window's age cap (4 × `sync_interval`) | Detent (node) | Yes | They change when acknowledgements wait for the barrier, never what is lost within the model |
| Refresh interval | Index setting, with a Detent default; stretched by the no-stalls controller up to its cap | Yes | Visibility only |
| Checkpoint thresholds (bytes, time) | Detent (node) | Yes | Within the group's share (Track C's D17) either way |
| `recovery_timeout` | Detent (node) | Yes | When a recovery election may decide without every voter's answer; the majority count still gates it (D6) |
| `unclean_recovery`, read and ack floors, defaults | Index setting (replicated), administrative privilege | Yes | Every replica decides by the same log position |
| `acks: synced` or `written` | The group's configuration entry, administrative privilege | Yes, through the log, after activation | In force when durable at each node and committed for the leader's replies (D7); the switch to `synced` syncs everything before it (D4) |
| Replication mode | Index setting (replicated), administrative privilege | Yes, through the log, and heavy | Document to segment: followers keep applying until the leader's next `Image` installs whole through D17, so D8's catch-up holds the replay bound meanwhile. Segment to document: followers replay from their installed position, which D9 bounds |
| `verify`: `sample`, `digest`, or `replica` (D18, Q9) | Index setting (replicated), administrative privilege | Yes, through the log | Detection only; `replica` moves one voter to document mode as the replication-mode row does |
| Verification rate and `verify_pinned_bytes` (D18) | Detent (node) | Yes | Detection only; each follower draws its samples from its own secret seed, so no rate change tells a leader which flush is checked |
| Transfer limits and `transfer_list_bytes` (D17) | Detent (node) | Yes | Pacing and refusal only |
| Lease reads | Index setting | Yes | Off takes effect at once; on, after one election timeout |
| Turn-log geometry, segment identity format, journal version | Build or format | No | Formats change through their reviews and activation (Track D8) |

### D16. Formats and wire

Every row below has a version, a fixture, and a previous-reader rejection test (T12), and a value
rule (DMS-7). **The wire fields are version 3's (Q8, decided)**: version 2 proceeds on its own
schedule, and these fields join through the version range (`peer-transport.md` D6, D7). A group
uses memory acknowledgements only after `acks: written` activates; a member whose session runs
version 2 is handled by D7's per-session rule, so no group feature waits on every session. The
fields' semantics freeze with this design's full review (first steps 3 and 4).

**Wire, version 3** (sizes with the 40-byte frame header, `peer-transport.md:589-605`):

| Kind | Change | Size | Value rules (DMS-7) |
|---|---|---|---|
| 1 `VoteRequest` | + `recovery` (D6) | 72 → 80 | A boolean, as the codec reads one (`codec.h:124-127`); from any candidate that counted f + 1 forgetful voters, forgetful or not (D6); the receiver cannot check that count, and a request sent without it wins nothing, since the decision needs a majority of the candidate's membership answering flagged `forgetful` |
| 2 `VoteResponse` | + `forgetful` (D6) | 56 → 64 | Only with `granted`; a grant from a voter whose policy forbids it is the voter's lie (DMS-3), not a decode error |
| 3 `AppendRequest` | + `commit_memory`, `leader_last`, `sync_through`, `recovery_term`; `commit` now carries `commit_durable` (D7) | 104 → 136 without entries; kind 3 is control when its length is its session version's size without entries (104 at version 2, 136 at version 3); `control_frame_bound` at version 3 is `136 + 32·rpc_entries + rpc_bytes` | `commit ≤ commit_memory`, `sync_through ≤ leader_last`, `leader_last ≥ previous + entries`, `recovery_term ≤ term`; the follower clamps both commits to its verified prefix |
| 4 `AppendResponse` | + `synced`, `forgetful` | 96 → 112 (112 → 128 with its compacted boundary) | `synced` only with `success`, at most `matched`; `forgetful` with either, since a forgetful voter's reply is usually a failed consistency check, where the leader must see it (DMS-7's "only with success" would hide it) |
| 6 `PreVoteRequest` | + `forgetful` (the owner's Q3) | 64 → 72 | A boolean |
| 7 `PreVoteResponse` | Unchanged | 64 | |
| 9, 10 coalesced heartbeat and reply records | Heartbeat + `commit_memory`, `sync_through` (its commit carries `commit_durable`, clamped by the leader to that follower's matched index); reply + `synced`, `forgetful` | Each record 48 → 64 (`peer-transport.md:618`) | As kinds 3 and 4; a reply record's `synced` is taken without a flight, clamped to `matched` (D7) |
| 16, 17 `Chunk`, `TransferControl` | `RFTXFR02`: file sets (D17): `Begin` with an `RFTSNP03` descriptor, and two control kinds, `Request` and `Missing` | `Begin` 6,480 → 6,532 (`snapshot.h:53` at 255 members, plus 52); `Request` 184 (`Accepted`'s 168 plus an entry's index and term); `Missing` at most 6,532 | D17's rules |
| Host kinds | Revision 0's segment fetch on host kinds 256 to 271 and 512 to 527 is withdrawn (DM-R8) | | |

**Log and storage formats**, each activating only after every node runs the version (the peer
transport's D7 formats rule; Track D8 builds the activation, until then by procedure,
`peer-transport.md:1414-1416`):

| Change | Where | Rule |
|---|---|---|
| `Membership` + optional `acks` (`synced`, `written`) | consensus log format | Absent reads as `synced`, so old configurations and Detent's keep their bytes; applies by position (D7's rule); every replica refuses `written` before activation |
| `EntryKind::Recovery` | consensus log format | Payload exactly 16 bytes (term, last index at election), its last index at or below the entry's own index; only in `written` groups; a batch holding one, or a hard state with the forgetful mark, uses a new batch magic (`RFTBTCH3`, after `storage.h:46-48`'s precedent: entry kinds through `Recovery`, and one byte after a present hard state, the mark), and a checkpoint whose retained suffix holds one, or whose hard state carries the mark, a new bundle magic (after `checkpoint.h:71-72`), which older readers refuse by magic |
| Hard state | consensus storage | One new byte, the forgetful mark (D4, DM-R19), only under `RFTBTCH3` and the new bundle magic, 0 or 1 and refused above; absent, as under every older magic, it reads 0. Its commit index holds `commit_durable` in `written` groups (D4, D7) |
| `RFTW0002` | journal format | End-of-turn payload exactly 12 bytes, its S at most the turn it ends; the open record's 16-byte boot identity; the clean-close record `0x12`; the open record's 8-byte V, the newest epoch whose recovery found voters forgetful (D3); the group checkpoint record's and the hard-state record's hard state with the mark (`track-c.md:494-496`, `582-587`); written only after activation (D3); `RFTW0001` still read |
| `Unsupported` | journal's codec (additive) | A `Corruption` subclass for a magic, kind, or version this binary does not read, so N's readers report N + 1's formats as unsupported, never as damage that makes a group rejoin as a new incarnation (DM-R7; F13-R42's lesson); existing handlers still catch it |

**APIs** (additive; absent, today's behavior): consensus's `Written` event, `Repair{promotion}`, the
checkpoint ceiling, `StorageBatch`'s urgency, `PeerRestarted{incarnation}` (D5), the file-set
transfer (D17); completion's kernel boot identity and a cross-IO directory barrier (SI-R10).

**strata** (versioned with strata's command encoding): the `Image` command (each file's identity,
size, CRC-32C, SHA-256, and a new segment's covered log range; file count bounded by the remaining
bytes over the smallest file record before allocation, as `files.hh:174` does; identities strictly
increasing; each size and the sum reserved before the first chunk, D17); the `Create` command (the
index's 16-byte UUID and its settings; only a UUID's first `Create` creates, and a duplicate applies
as `exists` and changes nothing, D1, SI-R25, DMS-10); the `Merge` command (D12); the session token's
term (D10); the term history, an index-level attribute (per replica, `format-13.md:1116-1123`),
required in `written` groups.

**Format 13**, its freeze-readiness table (`format-13.md:1307-1330`): the physical identity
(F13-R41, done), bit 16 `version_clocks` (done), the per-segment attribute list (F13-R43, done) and
the index-level one (F13-R51, done), whose tags arrive later with their releases: the retention
slot's [min, max] per segment (a cache tag, Part B), the memory tier's "not yet on disk" mark per
segment (a required tag, Part B), the disk tier's position and a shared-memory restart's valid mark
(index-level, Part B), and the term history (index-level, above). Nothing here waits on a format 14.

### D17. One byte mover: file sets on the snapshot transfer (DM-R8, DMS-1, DMS-5)

Every file strata moves between members rides consensus's snapshot transfer (`RFTXFR01`, kinds 16
and 17, Track C's D18), extended at version 3 with file sets: one mover, one flow control (the
group's bulk-lane window, `peer-transport.md:840-843`), one binding (the peer transport's D5, under
which an uncommitted membership binds, so an honest learner whose addition is uncommitted receives),
in consensus, which strata drives.

- **Descriptor `RFTSNP03`**: `RFTSNP02`'s fields (`snapshot.h:35-45`) plus the file list's size and
  SHA-256, the file count, and the total bytes (52 bytes).
- **The stream** starts with the file list: per file its physical identity (16 bytes), size (8),
  CRC-32C (4), and SHA-256 (32), then the application's metadata (strata's manifest), at most the
  descriptor's list size and checked against its SHA-256 before anything in it is decoded.
- **Bounds before buffering (DMS-12).** The receiver checks the `Begin` before it buffers a byte of
  the list: the list size is at least the file count × 60 and at most `transfer_list_bytes` (8 MiB
  per transfer, a Detent knob, D15), and the receiver takes it from the transfer's memory, a slot's
  for a large transfer (4 incoming per node, Track C's D13) and the node's `shipping_bytes` pool
  for a small one (below), so lists never hold more than those budgets. The mechanism is the
  memory a receiver holds before the list's SHA-256 can refuse it; Lucene, Tantivy, and Xapian ship
  no file lists to compare with. For an `Image`, whose committed entry the receiver holds, the file
  count and the total bytes must equal the entry's. Once the list is checked, its sizes are summed
  with an overflow check and must equal the descriptor's total bytes, which today's policy already
  bounds (`maximum_application_bytes`, `snapshot.h:49`, `59`). Any disagreement or overflow
  answers `Result{Rejected}`, counted, before the first chunk (T15).
- **`Missing`** (receiver to source, control): the receiver answers the list with the ranges of
  files it lacks; a file it holds must match all four of identity, size, CRC-32C, and SHA-256. At
  most 795 ranges fit the control bound (8 bytes each after a 4-byte count); a receiver that needs
  more asks a covering superset. Before it answers, it reserves the missing files' bytes from its
  storage permit (the rule of `index.hh:763-767`, moved before the first chunk); a refused
  reservation answers `Result{Rejected}`, reported, retried only after space frees.
- **Chunks** keep the snapshot channel's rules: a token, an offset equal to the bytes received, at
  most 65,536 bytes, a final marker (`snapshot_transfer.h:68-78`; `snapshot_channel.h:241-242`;
  `worker.h:316-319`). One changes: a chunk for no outstanding transfer drops and is counted, where
  the channel today calls it corruption (`snapshot_channel.h:240`), since an honest source's chunk
  can cross the receiver's cancel or its `Missing`. The source streams only the missing files, in
  list order.
- **One hash rule (DMS-5).** The receiver streams each file to disk through OpenSSL's EVP SHA-256
  (the `SHA2-256` method fetched once, a context per file) and CRC-32C as the bytes arrive, and
  compares the size and both checksums with the list before the file's rename; any mismatch fails
  the transfer, counted, whatever the source. CRC-32C stays what today's install checks per section
  (`index.hh:2442-2482`, `verify_all` at `2468`). For an `Image`, the list must also equal the
  committed entry's files, so a member serving files cannot pass bytes the leader did not commit.
  OpenSSL 3 is already linked through `consensus::transport` (`consensus/CMakeLists.txt:26`), the
  platform's library, whose version the transport logs at startup (its threat model's dependency
  row, `peer-transport.md:205-228`); Kronuz/sha256 (portable, without SHA instructions, its
  README:68) is not added.
- **`Request`** (receiver to source, control): asks a member to stream a committed `Image` entry
  (its index and term) or its latest checkpoint. A source refuses one for an entry not committed in
  its own log, keeps at most one transfer per (group, receiver), and at most 4 per receiver node,
  served FIFO across receiver nodes and work-conserving (SR22's rule, its outgoing form).
- **Two sizes, one mover.** A transfer whose missing bytes exceed 64 MiB (`transfer_slot_bytes`)
  takes one of `snapshot_concurrency`'s slots (4 outgoing and 4 incoming per node, Track C's D13),
  at most one per destination node while another destination waits; the lead routes that rule to
  Track C's D13 with SR22's C16, since `snapshot_concurrency` is unbuilt (`git grep` finds none at
  `61149a2`). A smaller transfer, one refresh's segments, takes no slot and holds at most the
  group's bulk window of read-ahead (16 chunks, `track-c.md:776-778`) from a node pool of 64 MiB
  (`shipping_bytes`), so a thousand groups shipping each second share a node's bandwidth by credit,
  not by four slots.
- **Uses**: installs and Track C's D18 retries (the leader's push, as today; a retry sends only what
  `Missing` names); segment shipping (D8: a follower's `Request` for each committed `Image`);
  repair (Track C's D10: the damaged host's `Request` to a member for its latest checkpoint, with
  only the damaged files missing); the survey's Q7 (an externally built segment named by an
  `Image`). Detent's `RFTSNP01` and `RFTSNP02` transfers are unchanged.
- **Limits**: none of Lucene, Tantivy, or Xapian ships files between replicas; each limit above
  names its mechanism (Elasticsearch's recovery counterparts are
  `indices.recovery.max_concurrent_file_chunks` and `node_concurrent_recoveries`).
- **A removed member** stays bound to its incarnation until compaction drops the entries naming it
  (`peer-transport.md` D5, rule 2.1), so it can still receive that index's files in that window; it
  held them already.

### D18. Verifying segment mode (DM-R11, DMS-4, DM-R21, DMS-11; Q9)

- **What segment mode gives up.** In document mode every voter builds its own segments, and the
  digest comparison catches the one that diverges (REPLICATION.md:64-66). In segment mode followers
  install the leader's bytes, so a leader-side apply bug, nondeterminism, or a bit flip before
  checksumming (REPLICATION.md:19) reaches every replica under valid checksums; T4's digest
  equality holds by construction; and in an all-segment-mode group no member holds an independent
  image to repair from, so a poisoned image outlives its leader.
- **The sampled rebuild** (Q9's option 2, the lead's call). Each segment-mode follower rebuilds a
  sample of the leader's flushes from its own log: for a new segment the `Image` names with its
  covered log range, it applies that range in document mode into a scratch buffer, flushes it to a
  scratch segment that is never published, and compares the two segments' logical digests, both
  computed by the verifier with format 13's digest records over the segment's documents (F21,
  `index.hh:644-652`). A mismatch is corruption: logged at error level, counted, and the follower
  stops installing that group's images and applies the batches itself from its installed position,
  becoming the group's independent copy until an operator acts. Revision 2 makes the sample hold
  against a leader that chooses, and widens it to deletions:
  - **Samples are each follower's secret (DMS-11).** A follower draws its samples from its own
    randomness, seeded per process from the operating system's generator and never sent, so every
    flush is sampled with probability 1/64 whatever the leader computes. A rule the leader could
    compute (every 64th flush) would let it plant a segment it knows no follower checks.
  - **Deletions ride their flush's sample (DM-R21).** For a sampled range the follower also checks
    the live-docs generations the `Image` names for it: it applies the range's deletes and
    replacements, by key, to the image it holds, and compares the deleted ordinals with the
    `Image`'s bitmaps. So a leader-side fault in deletions, an old version left live after a
    replace, is sampled as its flush is, at the cost of the range's key lookups, without inversion.
  - **Debt defers a sample, never skips it (DMS-11).** Under debt a sample waits in a queue with its
    segment pinned from reclamation, never delaying an install, and the queue's pinned bytes are at
    most `verify_pinned_bytes` (1 GiB per node, D14). A sample the queue cannot hold is counted
    `verify_dropped`, and one whose log range a checkpoint truncated before it ran is counted
    `verify_lost`; both are logged, so a leader that floods ingest to drown its samples is seen.
  - **Merges are checked for merge faults only.** A sampled merge is re-run over the inputs the
    follower holds and the outputs compared. The inputs are the leader's segments, so a corrupt
    flush that reaches a merge passes a correct merge, and a missed flush stays missed. Rebuilding
    the merge from the log range its inputs cover (DMS-11's alternative) fails for every merge whose
    inputs predate the last checkpoint, which truncated that range. Option 3 below is the detection
    path through merges.
- **Rate and cost.** One in 64 flushes and one in 64 merges per follower by default (live, D15), at
  the pool's background class. Its cost, about a 64th of a document-mode follower's work per
  document, is in D8's table (+0.01× of a group's document-mode CPU); the deletion check's key
  lookups are unmeasured and small beside the rebuild it rides (an estimate).
- **What option 2 catches, and what it leaves (DM-R21).** Per corrupt flush, honest or planted (with
  the secret seed the attacker's figure is the honest one): detected with probability
  1 − (63/64)^(rf − 1), 3.1% at rf = 3 and 6.1% at rf = 5, so missed 96.9% and 93.9% of the time
  (`arith.out`). A missed flush is missed for good: once merged, the merge check passes; the digest
  comparison is equal by construction in an all-segment-mode group; and T4's end-of-run rebuild is
  a test oracle, not a production check. A systematic fault shows within tens to a hundred
  flushes: after 64 corrupt flushes it is still unseen with probability 13.3% at rf = 3 and 1.8% at
  rf = 5, after 128 with 1.8% and 0.03%. Against a leader that drives the debt or computes the
  rule, revision 1's sample fell to 0.3% at a skip rate of 0.9 and to 0 (`d18_residual.out`);
  deferral and the seed hold it at the honest figure. It detects; it does not repair, and strata's
  plan holds no backup: PLAN's recovery tooling rebuilds derived structures from stored records
  and restores "from elsewhere: a replica, the journal, or a checkpoint" (`PLAN.md:663-680`), and
  in an all-segment-mode group each of those holds the same bytes. So repair is re-indexing from
  the client's source, or the tooling's rebuild from stored records when only derived structures
  (postings, the term dictionary, norms) are wrong.
- **Digest anchoring** (Q9's option 3, proposed here, its cost unmeasured). F21's digest is a sum of
  record hashes modulo 2⁶⁴ (`index.hh:644-652`), so it can be kept incrementally. A segment-mode
  follower keeps the index's expected digest from the log: each applied document adds its records,
  computed from the prepared batch (its `D`, length, `P`, and stored records: the leader's tokens
  grouped per document, no segment built), and a delete or a replace subtracts the old version's,
  from an 8-byte contribution kept per live document. At each `Image` it digests what it installs
  the same way, walking each new segment once, merge outputs included, keeping each live ordinal's
  contribution, and subtracting those of newly deleted ordinals, and compares the two sums. A
  content fault in a flush, a merge, or a deletion is then detected at its first install, with no
  sampling and no escape through merges, while the follower's log still holds everything from its
  last good image, so the follower that detects it becomes the group's independent copy (above)
  before the fault can be merged away. Its cost is those per-document records on every follower, 16
  bytes per live document, and a decode of every installed segment at every merge tier, about a
  merge's read side; no figure here measures it, so T2 prices it before the owner chooses. It leaves
  what the digest does not cover (skip data, block maxima), which the sampled rebuild's digest
  comparison misses as well.
- **Per index, `verify`** (a replicated index setting under the administrative privilege, D15):
  `sample` is the sampled rebuild, `digest` digest anchoring, and `replica` (Q9's option 1) keeps
  one voter in document mode, an independent digest and repair source, through the mixed-version
  path; `strict` keeps every voter in document mode. Q9 picks the default.

## Threat model

What this design adds to the peer transport's (`peer-transport.md:205-228`): file sets on the
snapshot transfer, the new message fields, and the forgetful and recovery signals. Members are
trusted as Raft trusts them (REPLICATION.md:8-12: "nodes are not Byzantine"); the rows below say
where that matters.

| Actor | Capability | Stopped by | Residual |
|---|---|---|---|
| A node with a pinned key that holds no member of the group | Receive or request an index's files | File sets are group-level transfers: the peer transport's D5 binding admits only an incarnation a membership the replica holds binds to the session's node, a committed one or an uncommitted one (rule 2.2), else the frame drops, counted | None beyond its handshake slots |
| A member that requests files | Make a source read its disk and spend its bulk lane | A `Request` names a committed `Image` or the source's latest checkpoint; at most one transfer per (group, receiver) and 4 per receiver node, FIFO and work-conserving; whole images in `snapshot_concurrency`'s slots, at most one per destination node while another waits; the receiver's own bulk credit paces the bytes (D17) | Its share of the source's bulk lane and disk while it asks; a removed member until compaction drops its binding |
| A compromised member serving files | Serve forged bytes to a receiver | Every file, whatever its source, must match its size, CRC-32C, and SHA-256 in the list, and an `Image`'s list must equal the committed entry's, before the rename (D17) | None for `Image` files; a whole-image install is checked as an install is today (`index.hh:2417-2482`) |
| A compromised leader | Commit an `Image` naming crafted files; send false `commit_durable`, `sync_through`, `recovery_term` | Followers refuse to replace anything at or below the `commit_durable` they learned, which only grows and is their persisted commit index (D7); the install's decoders bound every length before allocating and are fuzzed with the network as the source (T15); D18's verifier compares rebuilt segments | Every segment-mode follower installs the same bytes, so a crafted file fences them all, and a poisoned image outlives the leader's removal with no independent copy unless `verify: replica` keeps one (D18, Q9). The leader chooses what it plants, so D18's sampled rebuild holds its honest figure only because each follower's samples are its own secret and debt defers them (DMS-11): a planted flush is caught with probability 3.1% at rf = 3 and 6.1% at rf = 5, and never after its first merge; under Q9's option 3 at its first install. A leader that floods ingest past the verifier's queue makes samples drop, counted and logged (`verify_dropped`). Barrier storms through `sync_through` on followers' devices, as a compromised leader can already stall its group |
| A compromised voter lying in replies | Claim `synced` above what it synced; claim `forgetful` | Nothing in consensus: a voter that lies about durability breaks Raft's assumption | A false `synced` can make `commit_durable`, and so a `quorum` acknowledgement, count a copy that is not on disk. A false `forgetful` costs the leader a rewind no deeper than the leader's own floor, one unsynced window, at most once per restart and once per `recovery_timeout` per peer (D5, DMS-13; under `aggressive`, one window per election timeout). In a vote or pre-vote it completes D6's count with f honest forgetful voters at an odd membership, turning today's fail-closed conflict into a recovery that rolls `memory` writes above `commit_durable` back at honest replicas; D6 makes it loud (logs and counters), not impossible. That loss needs `recovery_timeout` to pass with the write's holders unreachable and never happens when every voter must answer (the security re-check's `liar_schedules.out`: 10 steps at rf = 3 with one honest OS crash, blocked when every voter must answer), so `manual`, or no `recovery_timeout`, resists a compromised voter at the price of availability (C12). Both are the compromised-voter row (`peer-transport.md:213`) |
| A member offering wire version 2 | Push its sessions back a version | D7's per-session rule: it receives `commit_durable` in `commit`, replies after its barrier, and its grants are never forgetful ones | Its own memory acknowledgements are slower; the group's are not (DMS-6) |
| A client with valid credentials | Session tokens; per-index settings; `quorum` writes on a shared core; the `manual` command | Tokens above the known last index refused at once; settings and `acks` behind the administrative privilege; `manual` only as a signed admin statement (`peer-transport.md` D17) (DMS-9) | A stream of `quorum` writes drives a core's barrier rate, today's cost for every write |
| Local access on a node | Fake a boot identity or a clean-close record | Whoever writes a node's journal is that node (the peer transport's local-file row); an unreadable boot identity reads as unclean (D3) | A false clean close keeps a voter that lost entries in elections, as a deleted journal would |

## The fault model per preset

The model (D2): at most f voters lose their OS or power or are forgetful at once, a forgetful
voter counting until it re-learns, across restarts; f of each membership in force, the same f for
both memberships of a replacement window.

| Failure | `strict` | `replicated` | `relaxed` |
|---|---|---|---|
| Process crashes, any number, kernels alive | Nothing lost | Nothing lost (D3's same-boot rewrite; a torn tail makes the voter forgetful, and a forgetful voter stays so across later restarts until it re-learns, D5's mark) | Writes on the leader alone, if another voter wins the next election |
| OS crash or power loss with at most f voters forgetful at once | Nothing lost | Nothing lost (D5, D6: no recovery election wins within the model) | The leader's unreplicated writes, if it is among them |
| More than f voters forgetful at once | Nothing lost, but Track C D11's stated limit (damage in the newest turns) | `memory` writes in the capped window (D2) that no recovery majority held (D6); never `quorum` writes | As `replicated`, plus the leader's unreplicated writes; under `aggressive` the recovery runs after one election timeout |
| One forgetful voter and one unreachable voter at rf = 3 | Elects | Elections stop until either returns or the forgetful one re-learns (D6's price) | The same |
| Exactly f + 1 voters forgetful at once in a replacement window, every candidate holding the even membership (six voters at rf = 5, four at rf = 3) | Elects (no voter is forgetful) | Elections stop until an operator's `manual` statement admits the loss: no normal quorum exists, and D6's decision needs f + 2 flagged answers there (DM-R20) | The same |
| Disk loss of at most f voters | Nothing lost: a new incarnation re-replicates (`track-c.md:321-323`) | The same | The leader's unreplicated writes, if its disk |
| A partition | The minority side refuses writes; `prefix` and `bounded` reads on any side (`bounded` up to T plus `election_max` stale on a minority side); `linearizable` on the majority side | The same | A quiescent old leader acknowledges until CheckQuorum steps it down, at most `election_max` from the write that wakes it (`track-c.md:357-360`); those writes are truncated at the heal |
| A leadership change with no failure | Nothing lost | Nothing lost | Writes on the old leader alone |
| A leader-side fault (an apply bug, nondeterminism, memory corruption before checksumming; REPLICATION.md:19) | Caught by the digest comparison across independent replicas | Reaches every segment-mode follower. Under D18's sampled rebuild (Q9's option 2) one corrupt flush is detected with probability 3.1% at rf = 3 and 6.1% at rf = 5 and never after it is merged, a systematic fault within tens to a hundred flushes; under option 3 at its first install. With no document-mode member, repair is re-indexing from the client's source: strata's plan holds no backup (D18) | As `replicated` |

## How small-indexes' D3 relates

- **D3-A** (batched syncs over today's files) is the checkpoint batcher, in the form SI-R6
  recommends: no host API, `commit()` stays the one call, and a node-level commit group shares the
  syncs of commits already waiting. SI-R9's condition holds: it is the engine's one sync batcher
  below `commit()`, and D1's background checkpoints use it. D1 takes its re-check's rules
  (`perf/small-indexes` `2e9949e`, C3): the entry a named generation's snapshot (SI-R19), whoever
  waits leading its batch and only unwaited checkpoints on the pool (SI-R24), and a failed
  checkpoint restored at its recorded generation from the intact journal, which leaves the voter
  not forgetful (SI-R20, as SI-R26 corrects it). Its failure split carries over (SI-R7, SI-R8);
  what changes is that a failed checkpoint no longer fails an acknowledged write, which the journal
  holds. Its rows T8a to T8c, T9, and T10 are the batcher's; T16 adds what strata::group owes.
- **D3-B** (a shared journal) is D1 and D3 on Track C's turn log, so it is replaced here.
- **D3-C** (fewer syncs at create) composes: standalone, as small-indexes' re-check approves it;
  journaled, through the creation record's replay, decided by the log and the index's UUID (D1,
  SI-R25), with its T11's journaled half and T16.

## Changes to other documents

Each is the owning document's to make; the lead applies the ones on `main`.

| Document | Change |
|---|---|
| `REPLICATION.md:8-24` | The guarantee gains `memory`'s line and the model statement (D2, f of each membership in force); the outside-the-model list gains "more than f voters forgetful at once" for `memory` writes, and segment mode's leader-side residual with its figures and no backup (D18, DM-R11, DM-R21) |
| `REPLICATION.md:64-66` | The digest comparison is independent only between replicas that apply; segment mode's oracle is D18's rebuild |
| `REPLICATION.md:68-86` | The write path shows the two cursors (D7) and segment mode beside document mode (D8) |
| `REPLICATION.md:117-149` | The acknowledgement and read tables become D10's, with DM-R13's restatements: `leader`'s condition, `bounded`'s minority-side bound, `session`'s term history and refusal of tokens above the known last index; `local` retires and `nearest` becomes `prefix` (Q6); tokens gain the term |
| `REPLICATION.md:151-158` | Cost: the bounded barrier and the memory window's cap (D3, D14) |
| `REPLICATION.md:160-172` | Xapiand mapping: `DB_SYNCHRONOUS_WAL` is `memory` at rf = 1; the async WAL is `none` at rf = 1; `?commit` is `refresh=true` |
| `REPLICATION.md:183-189` | Track C D11's limit widened to the turns after the last recorded synced turn (D3), and closed for `written` groups by D5 |
| `DISTRIBUTION.md:111` | "Replaced by quorum-acknowledged journals" becomes "by journals acknowledged at the index's level (D10)" |
| `PLAN.md:555-556` | Phase 3R lists document-mode replication; segment mode, `memory`, D17, and D18 join its scope, or follow it (the lead's call) |
| `track-c.md` D3 (`:253-286`) | A recovery election in a replacement window counts a majority of the candidate's membership flagged forgetful, and exactly f + 1 forgetful voters there stall until `manual` (D6, DM-R20) |
| `track-c.md` D5 (`:301-327`) | Forgetful voters keep their incarnation; losing durable hard state or the log still means a new incarnation |
| `track-c.md` D10 (`:419-456`) | `Repair{promotion}` (D8); D6's truncation path and repair; repair's files through D17's `Request` |
| `track-c.md` D11 (`:460-625`), conditions 7 and 24, T9 | One write in flight; effects at the barrier; the end-of-turn S; `RFTW0002`; the clean-close record; same-boot recovery; the verdict's epoch V in the open record and the forgetful mark in the hard-state and group checkpoint records (D3, D4); T9's rows |
| `track-c.md` D12 (`:627-667`) | The records' new fields; quiescence on `commit_durable` (D7) |
| `track-c.md` D13 (`:669-674`), D18 (`:775-797`) | File sets (D17); `transfer_slot_bytes`, `shipping_bytes`; the outgoing slot rule per destination node with SR22's C16 |
| `track-c.md` D15 (`:730-731`) | "`matched` never moves back" gains its exception: a reply flagged `forgetful` lowers it to its floor, once per peer restart (D5) |
| `track-c.md` format table (`:182-200`) | D16's rows |
| `peer-transport.md` D9 (`:587-672`) | Version 3's kinds 1, 2, 3, 4, 6, 9, 10, 16, and 17 at D16's sizes; revision 0's host kind ranges withdrawn |
| `simulator.md` D5, D6, D9 | A process crash that keeps the page cache, apart from an OS crash or power loss that drops unsynced writes, with the boot identity; process restarts that keep the forgetful mark, and torn same-boot tails; membership changes through Track C's replacement window; the model statement (D2); D6's recovery election with the majority count; the checker's `memory` mode is definite within the model and unknown beyond it, `leader` unknown; T3's forced schedules, `restart3`, `torn3`, `six`, `six_loss`, `grow6`, and `grow6_loss` among them (C10) |
| `limits.md` | Rows for `transfer_list_bytes` (D17) and `verify_pinned_bytes` (D18), each with its mechanism |
| `small-indexes.md` D3 | As the section above |
| `format-13.md` | Nothing owed: its reservations are done (D16); the tags arrive with their releases |
| `rocksdb-survey.md:318`, `:386-388` | `memory` at rf = 1 is the process-crash-only setting, so RocksDB's WAL-without-sync rows compare against it (Q5, decided) |
| `workload-profiles.md:100-106` | Standalone durability offers `memory` and `quorum` (Q5) |

## For Detent

The lead adds these to Detent's `SHARED_LIBRARY_DECISIONS.md`. Every change is additive: nothing
Detent calls today changes behavior unless Detent opts in (its rule, `:10-11`).

1. **Nothing changes in a group without `acks: written`**, which is every Detent group: no
   forgetful state, after a failed barrier or otherwise, no forgetful mark in the hard state, no
   `Recovery` entry, no recovery election, no new journal record, replies after the barrier as
   today, `commit` equal to the commit index.
2. **Opt-in offered** to a host on the turn log: forgetfulness after an unclean restart or a torn
   same-boot tail, a failed barrier's restart included (D3, D5), at the price of D6's wait after a
   correlated power loss. Detent's
   per-group journal needs none: it publishes its frontier only after the sync, and recovery
   discards what lies past it (`track-c.md:115-117`).
3. **The per-group journal** (Detent's backend) is untouched and refuses `acks: written` (D3).
4. **Version 3's bytes**, when Detent moves its pin past version 3: kinds 1 (+8), 2 (+8), 3 (+32),
   4 (+16), 6 (+8), records 9 and 10 (+16 each), and `RFTXFR02` (D16). In a `synced` group the new
   fields are zero or equal to existing ones. Detent restarts once into version 2 as agreed
   (`SHARED_LIBRARY_DECISIONS.md:131`); version 3 follows through the version range, with no
   second flag day.
5. **New API, all optional**: `Written`, `Repair{promotion}`, the checkpoint ceiling,
   `StorageBatch`'s urgency, `PeerRestarted`, file-set transfers (Detent's one-blob transfers
   unchanged), completion's boot identity and directory barrier, and journal's `Unsupported` (a
   `Corruption` subclass, so existing handlers catch it).
6. **Library commits** (Track C's condition 1, the architect's C3): Detent's suite runs at each
   additive step's commit with the pin moved in a scratch build, recorded in Detent's
   `LIBRARY_BOUNDARIES.md`. Detent's agreement goes in `DEPENDENCY_UPGRADES.md` before first step 4
   lands.

## Prior art

| Source | What it shows | Taken here |
|---|---|---|
| Ongaro's dissertation (2014), §3.8 (`basicraft/consensus.tex` at `0c10ea1`) | Terms, votes, and entries persist before they count; the commit index may be lost; "If a majority of the cluster loses its persistent state ... a system administrator would need to admit the possibility of data loss" | D4; D6's bounded, per-index version of that admission |
| The same, §3.6, §6.4.1, §10.2.1, §11.6 | Leader completeness by up-to-dateness; leases of `election timeout / clock drift bound`, with clients tracking the index they saw; the leader's disk write in parallel; a primary-copy primary "reflects uncommitted entries" and must roll back after losing leadership | D6's grants; D9's leases and D10's tokens; D7's leader counting itself and sending at `Written`; D8 applies only committed entries, so nothing rolls back outside D6 |
| Liskov and Cowling, "Viewstamped Replication Revisited" (MIT-CSAIL-TR-2012-021), §4.3, §5.1 | A recovering replica takes no part until its state is as recent as when it failed; "The log can be pushed to disk in the background"; failure independence, UPSes, or non-volatile memory cover power failures | D5's re-learning, kept out of elections only; D3's background barrier |
| Apache Kafka's design ("Replicated logs" and "Unclean leader election", as hosted at docs.confluent.io/kafka/design/replication) | No fsync requirement; a rejoining replica "must fully re-sync again even if it lost unflushed data"; `unclean.leader.election.enable` false since 0.11 | D5; D6's policy; D10's mapping of `acks` |
| KIP-966, Eligible Leader Replicas (Accepted; KAFKA-15332) | Unclean shutdown detected by a clean-shutdown file and broker epoch; such a broker leaves the eligible set; unclean recovery Aggressive, Balanced (default), None, after 5 minutes, electing "the highest leader epoch plus the longest log end offset"; Kafka's controller decides, as one authority | D5's detection; D6's three policies and timeout; D6 fences its decision with a term where Kafka has a controller |
| Kafka's leader-epoch cache | The first offset of each leader epoch, kept with the log | D10's term history for session tokens |
| OpenSearch segment replication (docs.opensearch.org; issues #2212, #3988, #6369, #9027) | Replicas copy segments and keep a translog; 40% throughput and lower CPU reported, a loss at 9 replicas; no `wait_for` refresh; gets go to the primary; a promoted replica re-indexes its translog's tail; renamed-segment collisions; the oldest version promoted in mixed clusters | D8's mode, promotion, segment identity, and mixed-version rule |
| CockroachDB (follower reads, leader leases, `kv.raft_log.disable_synchronization_unsafe`) and TiKV (`raftstore.sync-log`) | Bounded-staleness follower reads; leader leases; an unsynced Raft log only for tests, "risks data loss or data corruption"; TiKV removed `sync-log` in 5.0 and forces it on | `bounded`; leases; acknowledgements from memory kept apart from the durable cursor, so Raft's safety never rests on an unsynced acknowledgement except through D5's re-learning |
| Azure Cosmos DB, consistency levels (learn.microsoft.com, 2025-09-03) | Five read levels on one replication protocol; bounded staleness by K versions or T time, with writes throttled past the bound; overrides apply to reads | D10's read levels; bounded reads that never throttle writes (D14) |
| PostgreSQL (`synchronous_commit`; asynchronous commit) | `remote_write` survives a standby's process crash, not its OS crash; asynchronous commit risks three `wal_writer_delay`s (200 ms by default) of data, never consistency | D2's guarantee; D10's mapping |
| MongoDB write concern | `writeConcernMajorityJournalDefault: false` acknowledges from memory and may roll back after a transient loss of a majority | D2; D6's rollback, here a repair |
| Elasticsearch `index.translog.durability`; peer recovery's file-chunk and recovery concurrency settings | `async` fsyncs every `sync_interval` (5 s by default, at least 100 ms), dynamic per index; recovery paced by concurrent chunks and recoveries per node | D3; D15; D17's limits |
| Ongaro et al., "Fast Crash Recovery in RAMCloud" (SOSP 2011) | Backups buffer the log in DRAM and answer at once, which needs batteries or capacitors to be as durable as disk; after a correlated power failure, wait for backups or accept loss | D2's explicit window, without special hardware; D6 |
| Abraham et al., "Scuba" (VLDB 2013); Goel et al., "Fast Database Restarts at Facebook" (SIGMOD 2014) | In memory, pruned by age and space every 15 minutes, best-effort partial results; asynchronous disk writes, "losing a tiny amount of data ... acceptable"; shared memory across clean restarts only (2 to 3 minutes against 2.5 to 3 hours from disk) | Part B |
| ScyllaDB and Cassandra time-window compaction | Windows compacted within, never across; whole expired SSTables dropped; one TTL per table; 20 to 30 windows | Part B |
| DeCandia et al., "Dynamo" (SOSP 2007); ScyllaDB 6.0 | Hinted handoff, read repair, and Merkle trees; Raft topology and tablets | D13 |
| Kulkarni et al., HLC (OPODIS 2014); Shapiro et al., CRDTs (SSS 2011) | Causal timestamps close to physical time; conflict-free types | D12 |
| Linux errseq (4.13; errseq-v4.17); PostgreSQL's "Fsync Errors" wiki | An unseen writeback error is reported once to the next `fsync`; an evicted inode loses it | D3's same-boot rewrite; D5's residual |
| Boot identities: Linux `boot_id`; macOS `kern.bootsessionuuid`; FreeBSD `kern.boot_id` (r359053) | A per-boot identifier on every platform strata targets | D5's detection |
| Groce et al., "Swarm Testing" (ISSTA 2012) | Random tests that each enable a random subset of features reach deep behaviors that uniform random tests miss | D6's model evidence at five voters |
| `research/no-stalls` (`467b5b3`), its sections 2 and 6 | Debt answered by refresh first and by admission from measured throughput last, never a stop; flush off the owner thread; the 30 s refresh cap | D9's cadence; D14; Part B's disk tier |

## Gate rows

strata's rows run in strata; consensus's and journal's in their repositories at the item's commit,
on Linux, macOS, and FreeBSD. Performance rows (T2, T8, T13, T14) are the lead's, on `main`. Rows
T3 to T5, T8, T11, and T13 run in document and segment mode, which doubles each of their budgets
(T3, for example, becomes 10^7 steps over 10,000 seeds per profile and per mode; the architect's
C4).

| Row | Target | Procedure |
|---|---|---|
| T1 Syncs per tiny index | Under D1, creating an index and acknowledging its first write issue no sync of their own on the acknowledgement path: the write waits for its turn's barrier, at most one device flush per core per turn whatever the group count; a checkpoint costs at most 4 durability calls per index and, on macOS, at most 4 `F_FULLFSYNC`s per device per batch. Today (measured, macOS): 10 calls and 10 device flushes for create plus the first commit | `bench/durability/count_syncs` with its counting IO at 1, 10, and 1,000 indexes; syscalls by `syncspy` on macOS, `strace -c -f -e trace=fsync,fdatasync,sync_file_range` on Linux, `truss -c` on FreeBSD. Counts, no hold |
| T2 Segment-mode CPU | At 100,000 documents of the 1M corpus's first lines on one thread, at the same cadence in both modes (the default one-second refresh, and T2's two buffer configurations), with SHA-256 through OpenSSL's EVP on the leader and every follower and the bytes over loopback TLS 1.3, D18's verifier at its default rate: a three-voter group's CPU per document at most 0.45× of document mode's, a five-voter group's at most 0.33×, a segment-mode follower's at most 0.12× a document-mode follower's. Reported beside, not gated: one large index with segment mode at the 16 MiB cadence against document mode at one flush; EVP's rate on each platform, measured before the targets are judged (D8's margin, DM-R22 item 5); and Q9's option 3, digest anchoring on every follower, on the same runs, so the owner chooses with its figure (DM-R21). This design's estimate, the verifier included: 0.38× to 0.43×, 0.25× to 0.32×, 0.05× to 0.11× (`arith.out`) | `bench/durability/sizing`'s phases driven through strata::group's real apply, `Image`, D17's transfer over loopback, and install paths, from the leader and from a peer; EVP on x86_64 with and without SHA extensions and on arm64; five interleaved trials under a light hold, as BENCHMARKS' rules state |
| T3 The `memory` window | Within the model (D2): zero `memory`, `quorum`, or `all` writes lost. Beyond it: zero `quorum` or `all` writes lost, and every lost `memory` write in the capped window (D2) before the loss. The beyond profile must report loss, or its oracle is blind. Forced schedules, each with no loss under D6 as revised: DM-R1's trace (`dmr1`) and the five-voter traces `fplus5`, `fplus5b`, and `heard5`; the restarts DM-R19 found, `restart3` and `torn3`, under the persisted mark; the replacement windows DM-R20 found, `six`, `six_loss`, and `grow6`, `grow6_loss`, under the majority count (`bench/durability/model/schedules/`); and `live3`, where the one voter that holds the write is not forgetful, elects it. The profiles include same-boot process restarts that keep the mark, torn tails, and Track C's replacement window at rf = 3 and rf = 5 (C10) | The simulator, tier C, with D5's fault classes: 10^7 steps over 10,000 seeds per profile, at 3 and 5 voters and through Track C's replacement windows at 4 and 6, in both modes; the checker's `memory` mode; the forced schedules as scripted runs |
| T4 No divergence | After every schedule, D6's recoveries included, each replica's digest equals the leader's at equal applied positions, and a replica whose applied index passed a divergence point repaired before serving a read; a D6 recovery after checkpoints taken under sustained ingest diverges no replica (DM-R3). In segment mode the oracle is not the leader's bytes: D18's verifier catches a seeded leader-side fault (a mutant flush that drops one posting per segment) within the run; a mutant leader that corrupts one flush in 64 of its choosing under sustained ingest is caught within the run, which carries at least 439 corrupt flushes at rf = 3 and 220 at rf = 5, so every one is missed with probability below 10⁻⁶ under the secret seed (`arith.out`; DMS-11); a deletion fault, an old version left live after a replace, is caught by the same samples; `verify_dropped` and `verify_lost` are reported; and an independent rebuild per group at the end matches every member | The simulator and Track C's T2 oracles; the digest per replica after each step; the seeded mutant |
| T5 Linearizability per level | Histories of `linearizable` reads and `memory` or stronger writes are linearizable within the model; `leader` and `none` outcomes are unknown; `session` and `bounded` reads meet their necessary conditions; with leases on, under restarts of random voters (DM-R14) | The simulator's checker (D9 of its design) with the `memory` and `leader` modes, in both modes |
| T6 Forgetful voters and recovery | A forgetful voter grants no vote or pre-vote outside a recovery election, acts on no `TimeoutNow`, and campaigns only in a recovery election before it re-learns; a recovery candidate leads only with every answer a grant and a majority of its membership flagged forgetful; each mutant is caught within the seed budget, its first seed recorded: a forgetful vote outside a round; re-learning through `commit_memory` instead of `leader_last`; a leader that ignores `forgetful` and never rewinds; a rewind below the leader's floor, or twice in one restart or in one `recovery_timeout` (DMS-2, DMS-13); a D6 truncation at `commit_durable`; a checkpoint above `commit_durable`; D6 without the f + 1 count (DM-R1); the count taken since the restart instead of at the round's term (`fplus`); forgetfulness cleared by a restart, which `restart3` and `torn3` kill (DM-R19); the mark cleared in a batch before the re-learned entries' (DM-R19); f + 1 flagged answers deciding at an even membership, which `six` and `grow6` kill (DM-R20); a voter that flags `forgetful` on a grant it would refuse, whose rollback the checker counts as a compromised-voter outcome outside the model, never as a pass (DMS-3) | consensus's `simulation_test` with the forgetful fault class; `tools/mutants.sh` |
| T7 Process restarts | Under one boot identity, 1,000 `SIGKILL`s of random voters at random points under writes: every write acknowledged at `memory` or stronger survives (DM-R13), and a voter restarts forgetful exactly when its tail was torn after S or it was forgetful when it stopped (DM-R19); a forgetful voter killed again before it re-learns restarts forgetful every time, and one that re-learned restarts not forgetful | Three processes on one host, the kill loop and a checker of acknowledged writes, replacing `harness/wal_recovery_check.sh` (PLAN's Phase 7 gate) |
| T8 Failover gap | Killing the leader (process and power) at sustained ingest: the next acknowledged write at the new leader within D9's total at p99 over 100 kills per preset (2.3 s for `quorum`, 2.2 s for `memory`, plus 1.0 s in segment mode), the replay within `election_min`, also with the leader's uplink saturated and during a document-to-segment switch; the gap at the client reported beside, with strato's timeout | A three-process loopback cluster, and the simulator with the measured barrier profiles, in both modes |
| T9 Journal with turns between barriers | Power lost at every point with any subset of the unsynced turns' 512-byte sectors persisting, with recycled segments (condition 10): recovery keeps exactly the longest whole prefix after the last recorded S; no synced record is lost but within D3's stated limit; damage at or before S costs only the groups named; `RFTW0001` journals recover unchanged. Same boot: a failed barrier with the pages kept clean, and with them dropped, each followed by a reopen: the first rewrites and syncs the tail and is not made forgetful, the second recovers forgetful; a torn tail recovers forgetful. A power loss after a copy-forward turn's write whose source segment would otherwise be reclaimed loses no synced record (DM-R6). A forgetful voter's same-boot restart, and its clean close followed by a reboot (the boot identity injected), each before it re-learns, recover it forgetful; a kill or a power loss at every point of a recovery, its open record included, leaves the next recovery forgetful in every group that was, and a group that writes nothing across two later restarts recovers forgetful at both (V); a cleared mark never survives without its re-learned entries (DM-R19). The per-group journal refuses `acks: written` | journal's T9 harness, extended |
| T10 Overrides, floors, and `quorum` latency | Every combination of preset, requested `ack`, and requested `read`: stronger honored, weaker below the floor refused naming the floor, never silently changed. A `quorum` write in a `replicated` index acknowledged after one barrier and a round trip, against two barriers and a round trip in `strict` (DM-R4) | strata::group tests; the latency over loopback |
| T11 Bounded and session reads | A `bounded(K, T)` read never serves state more than K entries behind the commit it learned within T, and on a minority side never more than T plus `election_max` stale; a `session` read honors read-your-writes and monotonic reads against a segment-mode follower's lag; a token whose entry D6 lost reports `lost`, also below the log's base; a token above the known last index is refused at once | The simulator's checker; strata::group tests, in both modes |
| T12 Formats and wire | Every row of D16 has its fixture, its previous-reader rejection test, and its value rules' refusals; the kinds' sizes as D16 states; a `Membership` without `acks` reads as `synced`; `RFTBTCH3` and the new bundle magic refused by older readers and read as unsupported by unknown versions, and their forgetful mark refused above 1 (DM-R19); a version-2 session in a `written` group receives `commit_durable` in `commit` and never a commit above it, replies after its barrier, and is counted for both cursors (DMS-6); Detent builds and passes its suite at each additive step's commit | With Track C's T1 and the peer transport's T3 |
| T13 Live tuning | Under sustained ingest, changing `sync_interval`, the window's caps, refresh, checkpoint thresholds, floors, and the verification rate takes effect within one interval, with no restart and the foreground write p99 within 1.5× of steady state across the change; switching `acks` and the replication mode through the log keeps every acknowledged write and the digest, the replay within `election_min` across a mode switch | Three processes under ingest, Detent's file source, in both modes |
| T14 Background under load | 10 minutes of sustained ingest at 1,000 groups per node: retained log per group within its share; install lag within D9's bound; unsynced bytes within `max_unsynced`; an idle group's `commit_durable` reaching its last index before it quiesces; with one peer receiving at its full window, another follower's install within 1.5× of the unslowed run (DMS-1); and the foreground write and query p99 within 1.5× of the same ingest with checkpoints, shipping, and verification deferred, on the same build (DM-R18) | With no-stalls' harness, on the gate VM; the deferred run as the baseline |
| T15 File-set transfers | A transfer sends only the files `Missing` names, skipping every file the receiver holds with equal identity, size, CRC-32C, and SHA-256; a file whose bytes differ, from any source, is refused before its rename; a `Request` for an uncommitted `Image` is refused; a chunk for no outstanding transfer drops, counted, and closes no session; a reservation refused before the first chunk is reported; a `Begin` whose list size exceeds `transfer_list_bytes` or falls below its file count × 60, whose file count or total bytes differ from the committed `Image`'s, or whose sizes overflow or do not sum to its total, is refused before any list byte is buffered, counted (DMS-12); the per-receiver and per-node limits hold; the install's decoders (`load_segment` whole and every reader format 13 adds) fuzzed with the network as the source, one CPU-hour per decoder (TESTING.md's row; DMS-4) | consensus tests with an application whose image is a set of immutable chunks (Track C's T4); strata::group tests; the fuzz targets in Phase 3R's gate |
| T16 Checkpoints and creation (SI-R19, SI-R20, SI-R24, SI-R25, DM-R23, DMS-10) | A crash at every point of a checkpoint batch over 3 to 100 groups leaves each index at its last recorded checkpoint or the batch's, and the log holds everything above it; a checkpoint failed at each phase fences that index's checkpoints and restores it at its recorded generation through `Repair`, with no acknowledged write lost and the voter not forgetful; more owners committing at once than the pool has workers all return; a creation record replays over a first checkpoint's residue, also after that checkpoint's last directory sync failed (in the same boot and after a power loss), and a manifest with another UUID refuses the replay, nothing removed; a duplicate `Create` of the same UUID, a client's retry after a lost reply, after a checkpoint record and before one: replay opens at the checkpoint's generation with every document, or applies from the first `Create`, on every replica, in the same boot and after a power loss, and the duplicate applies as `exists` (DMS-10; `create_replay.py`'s fixed arm is the oracle); a checkpoint of a past generation whose inputs a later merge replaced and whose live-docs files it wrote, then the owner's retention and reclaim, then a crash and an open: every file the manifest names exists, and the digest equals that generation's (DM-R23) | Small-indexes' T9 to T11 harness (faultfs at each phase, lifecycle crash points) and its re-check's probes as tests (`owner_waits_on_pool.cc`, `stale_incarnation.cc`, in `strata-wt/review-small-indexes/.scratch/si-review/`), through strata::group once step 5 exists |

## Open questions for the owner

**Decided on 2026-10-19** (`docs/status/durability-modes.md`): Q1, Xapiand's default preset is
`replicated`; Q2, the default `sync_interval` is 100 ms; Q3, `balanced`, with DM-R1's fix: a
`forgetful` flag on pre-vote requests and D6's grants only after f + 1 forgetful voters are heard
(see Q10); Q4, merges shipped now, revisited with copy merges; Q5, `memory` at rf = 1 offered,
superseding rocksdb-survey's Q3; Q6, one vocabulary, `local` retired and `nearest` renamed `prefix`;
Q7, the split, followed; Q8, the version-2 wire does not wait: this design's fields join version 3
through the N and N − 1 range (D16).

**Q9. Segment mode's independent check (DMS-4), restated with its residual (DM-R21, DMS-11).** The
lead called option 2 on revision 1's statement that "a rare single corrupt segment, found later, is
rebuilt from the source or a backup". Nothing finds it later, and no backup exists, so the choice
returns to the owner with each option's residual measured against the same fault, one corrupt
flush, honest or planted (D18; `arith.out`):
1. **A document-mode member per group by default** (`verify: replica`): an independent digest and a
   repair source in every group. Costs about a third of a three-voter group's document-mode CPU
   more (that follower does document mode's work), which halves segment mode's saving. Residual:
   none for faults the digest covers.
2. **D18's sampled rebuild on every segment-mode follower by default**, as revised (secret samples,
   deletions sampled with their flush, deferral under debt): +0.01× of a group's document-mode
   CPU. Residual: one corrupt flush detected with probability 3.1% at rf = 3 and 6.1% at rf = 5,
   missed 96.9% and 93.9% of the time, and never detected after its first merge; a systematic
   fault within tens to a hundred flushes (after 64 corrupt flushes still unseen with probability
   13.3% at rf = 3, 1.8% at rf = 5); no repair source and no backup, so repair is re-indexing from
   the client's source. Before revision 2, a leader that computed the sampling rule or drove the
   debt brought detection to 0.
3. **Digest anchoring on every segment-mode follower by default** (D18): every content fault in a
   flush, a merge, or a deletion detected at its first install, and the detecting follower becomes
   the group's repair source. Its cost is unmeasured (per-document records from the log, 16 bytes
   per live document, and a decode of each installed segment per merge tier); T2 prices it.
4. 1 with 2 or with 3, by default.

Option 1 per index and `strict` stay one setting away under each.

Recommendation: 3, if T2 measures it below option 1's price at the same replication factor;
otherwise 1. A default should keep the corruption defense REPLICATION.md:64-66 promises: under 2 a
single corrupt flush, the kind document mode's digest comparison catches, goes unseen about 97
times in 100 at rf = 3 and stays unseen for good, with nothing to repair from; 2 detects
systematic faults only. 3 detects at install and turns the detecting follower into the repair
source; at the price of 1 or more, 1's always-independent replica is the stronger of the two. 2
stays available per index (`verify: sample`) for data its owner can re-derive. Until the owner
answers, the lead's call stands and the design builds what both need: D18's sampled rebuild as
revised (step 5), and digest anchoring behind `verify: digest`, so T2 can measure it. Neither
blocks a first step.

**Q10. D6's correction to Q3's rule (DM-R1, DM-R20).** The ruled rule, f + 1 forgetful voters heard
since the restart, loses a `memory` write inside the model at five voters (the `fplus5` schedule,
and `fplus5b`, found by random search; D6), as revision 0's every-voter-heard rule does (`heard5`);
at three voters it holds. Revision 1's correction, f + 1 flagged answers counted again at the
recovery election's own term, holds at every fixed membership the model reached but not across a
membership change: in rf = 5's replacement window it loses a `memory` write (`six`, `grow6`; D6),
so revision 2 decides on a majority of the candidate's membership, which is f + 1 at every odd size.
1. Keep the ruled count as what starts a recovery election, and decide on a majority of the
   candidate's membership flagged among that election's answers at its own term (D6).
2. Keep the ruled rule as written and allow memory acknowledgements only at rf = 3.
3. Keep the ruled rule as written and run only `manual` recovery at rf ≥ 5.

Recommendation: 1, which this revision follows (the lead's call, with the count corrected): it keeps
the flag on kind 6 and the f + 1 start, adds one flag to kinds 1 and 2 (D16), and decides as
revision 1 did at every odd membership. It holds in the model at every fixed membership checked,
three and five voters with restarts and torn tails, and in Track C's replacement windows with the
majority count; with f + 1 it does not hold in rf = 5's window (D6's model evidence, with each
run's depth and scope). Its price is the window stall (D6, the fault table). Options 2 and 3 avoid
DM-R20 as well, since rf = 3's window does not fail this way (the architect's re-check), at the
costs they state. It does not block the first steps that may start now.

## First steps

Steps 2 and 4 to 8 may start now, under the architect's re-check of revision 1 (its C8 to C12) and
the security re-check's conditions, which this revision answers; steps 1 and 3 wait for this
revision's re-check (DM-R19 and DM-R20 blocked them), and nothing lands before the full design
review clears.

| Step | Where | Starts | Waits for |
|---|---|---|---|
| 1. `RFTW0002`: one write in flight, effects at the barrier, the end-of-turn S, the boot identity in the open record, the clean-close record, same-boot recovery, the verdict's epoch in the open record and the mark in the hard-state and group checkpoint records (DM-R19), activation, with T9's rows (D3, D4) | `Kronuz/journal`, branch `durability-barrier` | After this revision's re-check | journal's code review; Detent unaffected (named-file backend) |
| 2. The kernel boot identity and a cross-IO directory barrier (D5; SI-R10) | `Kronuz/completion` | Now (C8) | Additive, with Detent's suite at its commit |
| 3. `Written`, the two cursors and the site table, forgetful voters with the durable mark (`RFTBTCH3`'s byte, the bundle's) and the rewind's floor and allowance (DMS-13), the recovery election with the majority count (DM-R20), the `Recovery` entry under its magics, `Repair{promotion}`, the checkpoint ceiling, `Membership::acks`, `PeerRestarted`, `Unsupported`, with T6 (D2, D4 to D7) | `Kronuz/consensus`, branch `durability-modes` | After this revision's re-check | Steps 1 and 2; the full design review before it lands |
| 4. D16's version-3 wire fields, through the version range | `Kronuz/consensus`, after the peer transport's version 2 lands | Now (C9) | The full design review of D5 to D8 and D17, and the security re-check's conditions, before it lands; Detent's agreement |
| 5. strata::group: D1's checkpoints of a past generation on small-indexes' D3-A commit group with retention on the durable head (SI-R19, SI-R24's claim rule, DM-R23), the failed-checkpoint restore (SI-R20), `Create` and its replay with the first-`Create` rule (SI-R25, DMS-10), apply at `commit_memory`, `Image`, incremental install, D8's catch-up, D18's sampled rebuild as revised and digest anchoring behind `verify: digest` (Q9), D10's levels, floors, and term history, with T10, T11, T15, and T16 | strata, Phase 3R's branch | Phase 3R's start (C1 to C4); may start now (C11) | Small-indexes' batcher (its step 6, after SI-R24) and its journaled create (SI-R25) |
| 6. D5's fault classes with restarts that keep the mark and torn tails, the model statement, D6's recovery election with the majority count, membership changes through the replacement window, the checker's modes, and T3's forced schedules, `restart3`, `torn3`, `six`, `six_loss`, `grow6`, and `grow6_loss` among them (T3 to T5; C10) | `design/simulator` | Now | Nothing else |
| 7. T1 on Linux and FreeBSD | The lead | Now | Nothing |
| 8. D17's file-set transfer, with the list's bounds and the checked sum (DMS-12) | `Kronuz/consensus`, with step 4 | Now (C12) | The peer transport's version 2 |

## Part B: memory residency and retention (outline, designed after A)

- **`resident`** acknowledges at A's `memory` (decided here, DM-R15): every voter writes the journal
  to its kernel and syncs it on its bounded interval, so D3, D5, D6, and the wire stand, and
  `commit_durable` bounds its checkpoints as in every preset (D1). What is memory-first is its
  segments: built in RAM in the on-disk format, so one byte layout serves both (format 13's
  freeze-readiness row), searchable at once, and written to disk by a background disk tier. A
  checkpoint references only segments on disk; a manifest that names a RAM segment (a
  shared-memory restart) marks it with a required per-segment tag (F13-R43). Restarts: from peers
  (A's D17), from the disk tier plus the log, or from shared memory after a clean shutdown only, with
  a valid mark as an index-level tag (F13-R51), as Scuba does ("We do not use shared memory to
  recover from a crash").
- **The disk tier's debt** (no-stalls): bytes in RAM not yet on disk, reported to the one controller;
  past a high mark, admission follows the disk tier's measured write throughput, never a stop. Its
  lag bound is the `resident` preset's stated loss beyond the model, as D2's window is
  `replicated`'s.
- **Retention**, two levels to compare and split: time-windowed segments in strata (the `log`
  profile's time order, a merge policy that never merges across windows, dropping a window as whole
  segments, per-document TTL as a filter until its window drops), and time-based shards above it
  (rollover in the shard manager, Track D4). The likely split: strata owns windows and whole-segment
  drops, using each segment's retention slot [min, max] as a per-segment cache tag (F13-R43; format
  13's first defined tag); rollover belongs to the shard manager, not strata. A retention drop is a
  log command stamped with the leader's HLC (D12's clock, a foundations library), so every replica
  drops the same windows at the same position.
- **What A leaves for B**: D5's forgetful rule; D6's recovery; apply at `commit_memory` with
  `Repair`; the checkpoint ceiling; D10's vocabulary; D17's mover for restarts from peers.

## Status

Revision 2 complete; nothing is built beyond the probes and the model. Measured on this branch (the
development Mac, as evidence for D1 and D8's choices): T1's counts with no hold, T2's sizing under a
light hold, both at `0b407c9`. Verified by computation: D5 and D6 in the model, with same-boot
restarts, torn tails, and Track C's replacement windows (D6, "Model evidence"). Every other figure
is the cited code's or document's, arithmetic, or a labeled estimate.

What the re-check should look at first:

1. Durable forgetfulness (D3, D4, D5): the open record's V, durable before its epoch's first turn;
   the mark in every hard state a forgetful voter writes (under `RFTBTCH3`, the new bundle magic,
   and the hard-state and group checkpoint records); and the batch that clears it, against
   `restart3`, `torn3`, and the searches with restarts and torn tails.
2. D6's majority count and its argument for a membership and its successor, against `six`,
   `six_loss`, `grow6`, and the searches from their prefixes; the window stall and `manual`'s
   admission of f + 1.
3. D18 and Q9: the residual as stated, the secret seed, deferral, the deletion check, and digest
   anchoring's mechanism over F21's additive sum.
4. D1's durable head (`index.hh:2188-2252`) and the first-`Create` rule (`create_replay.py`).
5. D5's floor and allowance (DMS-13), D17's bounds (DMS-12), and D7's `synced` from every reply.
6. For the security reviewer: DMS-10 to DMS-13 and C12 at their lines, and D18's secret seed and
   deferral against a leader that floods ingest.
