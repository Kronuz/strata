# Journal binding: the index names the entry it applied last

Status: design (quest strata-journal-behind, after its durability review's F1 and F2), revised
after its design review (CHANGES: D-1 to D-8). The lead approved the binding (D1 to D4, D3's gate)
and Q1 to Q3 (a) on 2026-11-25; the owner confirmed D6's split by refusal and Q1 to Q3 (PLAN D183).
The group's part builds once `consensus:recover-gate` and `strata:applied-term` exist (Contracts);
D5 needs neither and builds first. Line references: strata:31821753, consensus:6d710cb (strata's
pin), journal:9c7e31c.

A replica is a pair: the index its host lends and the group's journal. The pair is sound when the
journal holds the entry the index applied last: replay then delivers the index's own history and
extends it. This design names that entry by its identity, checks it before consensus starts the
group, and makes an install land the index on an entry its journal holds.

## Why positions failed

The open at strata:31821753 compares positions: strata's head P_H against the recovered log's end
E, read once `driver.recover` returns (group.hh:265, 1173). Two failures (review F1, F2):

1. E moves. `recover` returns after Start (driver.h:891-897), and a lone voter campaigns at Start
   (core.h:702-710). Each refused open persists a new term and a NoOp, so E grows by one per retry
   until the check passes over entries strata never applied: job 261125-011921-926bd8 serves e's
   bytes for key c on the second open, and reads past the end of a volume on the third.
2. An install leaves P_H past E on purpose. The leader pins the strata generation G it commits at
   its head, at or past the bundle B (install.hh:344); the learner's install clears its log to B
   (core.h:733) and strata adopts G's head P_G. Every restart before the log reaches P_G is refused: 12 against 7 after a kill, a
   power cut and a clean close (261125-011917-b11e3f); the build-test failure in
   simulation_group_corruption (261125-011923-0ee55a).

Positions order entries; they do not name them. A NoOp at E + 1 sits at a position strata applied,
and it is not the entry strata applied there.

## D1. The index's head is an identity: H = (P_H, T_H)

strata records, with its applied position, the term of the log entry at it. apply takes the
entry's term with its position; the manifest log keeps it beside applied_index, a scalar an edit
carries only when it changes (at a term change); install adopts it from the source generation
(contract `strata:applied-term`).

In a Raft log, (index, term) names an entry and, by log matching, its whole prefix: every log of
the group that holds (i, t) holds the same entries through i. "The journal holds H" so says "the
journal's history through P_H is the history the index applied", whatever the journal did since.

Lost:

- Positions alone: F1 and F2 above.
- A file of term starts beside the index, kept by the group: strata also commits outside the
  group's calls (the catalog's commit chore, catalog.hh:385-388; releases; shutdown), so the file
  can lag the head it describes, and every open reads one more file (millions of indexes per host).
- A metadata key: metadata enters the digest, and replicas checkpoint at different positions.

## D2. The open's rule: the recovered log holds H

`holds(H)`: H equals the log's base boundary, or base.index < P_H <= last and the entry at P_H has
term T_H. It is consensus's own test for an install that keeps its suffix (`retains_suffix`,
core.h:659-661). One comparison covers the three refusals:

- index older: P_H below the base (the log compacted entries the index lacks);
- journal older: P_H past last (the log lost entries the index applied);
- another history: the entry at P_H has another term (a copy taken before a leader replaced it).

What the open binds:

1. A complete install stage whose bundle is the one recovered: the bundle's boundary, which D5
   makes the stage's head; `resume` then activates it.
2. Else a replica marked damaged: nothing. Its index is set aside and it reinstalls (repair.hh).
3. Else the index's H. An index that never committed has H = (0, 0), held only by a log whose base
   is 0 (a deferred index's first life, D112).

A refusal throws `group_fenced` naming both sides and the remedy (D6), for example:
`strata::group: the recovered log does not hold strata's head (12, term 3): base (7, term 2), last
9: the journal is older than the index; replace this member, or put its journal back`. A refusal
D6 repairs in place (index older on a replicated member, any refusal on a learner) does not throw:
the open marks the replica damaged, sets its index aside, recovers again with no boundary bound
(case 2) and reinstalls. The open reads no file it does not read today: the stage and damage
markers `adopt` reads are read before `recover` instead, and the rule is one comparison.

## D3. The check runs before Start (`consensus:recover-gate`)

consensus calls the group's check inside `recover`, after the log is recovered and before Start. A
refused journal starts nothing: no election, vote, NoOp, checkpoint or close record. Three reasons:

- A refused journal stays as it was, so every retry refuses the same way (F2's erosion is gone).
- A replicated member never votes with state it lost: between Start and the row's close, a
  refused member can grant a vote.
- (index, term) names one entry only while no term is issued twice. A journal older than its
  index that elects issues terms its lost history already used.

Lost: reading E after `recover` returns (today: Start has run); a term check without the gate (the
refused journal still elects and votes).

## D4. A journal that holds no group

When the host's table holds no group and H is not (0, 0), the open refuses before `create`: nothing
is created, so a retry finds the same table (the created group's erosion in F2 does not arise). A
fresh or deferred index (H = (0, 0)) creates as today. A member removed and re-added over a
directory its host kept is this case (Q2).

## D5. An install lands the index on its bundle

The leader pins the generation strata committed at the bundle itself, so G's head is (B, T_B), and
after activation the learner's head is the boundary consensus installed as its log's base. D2 then
holds in every state, installs included, with no exception.

- A checkpoint captures, aligns strata at P_b (capture_work's empty transaction), syncs the set's
  files and commits strata in one chore item, where the split order made the capture and the
  commit two (group.hh:1102, 1127). No apply runs between them, so the commit's generation is at
  P_b, except while recovery's replay is still below strata's head (apply skips there, and
  strata's head stays past the bundle).
- The leader tracks the peers that await an install across flights: a peer joins at its
  SnapshotNeeded and leaves when a release says it installed, caught up, left the membership, or
  this leader lost its term or fenced (Installed, CaughtUp, Removed, LeadershipLost, Fenced). A
  failed, stale or timed-out flight keeps it.
- While a peer awaits, the checkpoint's commit is also a pin (`make_pin`, whose
  `index::checkpoint()` commits and pins in the same item), unless the replay keeps strata past
  P_b; then it commits unpinned and a later checkpoint pins.
- At SnapshotNeeded for the latest bundle K with no pin: if strata's applied position is K, it
  pins at once (`make_pin`). Otherwise it starts a pinning checkpoint (one at a time); this
  flight finds no pin and fails, and consensus starts the next one after rpc_timeout (core.h:840,
  847).
- consensus refuses a checkpoint past a Sending flight's bundle with Busy (core.h:867), and the
  pinning checkpoint often reaches it inside the failing flight's Sending window (review D-1). A
  publication refused Busy while a flight is in progress waits for the next release and retries
  the same bundle, its pin kept, until consensus accepts it or refuses it otherwise. The
  checkpoint's serializer stays held meanwhile; every flight ends in one release, by
  snapshot_timeout at the latest.
- `make_pin` refuses a bundle strata is not at (P_H != P_b), and the learner's `check_stage`
  requires the staged generation's head to equal the bundle's boundary before it answers
  validated: a source that pinned past its bundle fails the flight, the stage is swept, and
  consensus never persists that install.

The leader's pins:

| Leader | SnapshotNeeded at the latest bundle K | a checkpoint publishes K' | consensus answers Busy | the flight ends |
|---|---|---|---|---|
| no pin at K, strata at K | pins K now | - | - | - |
| no pin at K, strata past K | starts a pinning checkpoint; the flight fails | pins K' in its chore item | waits for the release, then retries K' | released; the next flight finds K' pinned |
| pin at K | the flight holds it | pins K' while a peer awaits; K's pin stays while a flight holds it | as above | unpinned; released once superseded and free |
| no peer awaits | - | commits unpinned | the checkpoint fails Busy, as before D5 | - |

Its timing, by the review's model (job 261125-021119-1b8f9a's `retry` policy): every one of the
54 fixed timings installs within 2 to 3 flights. A pin on every published bundle installs at the
first flight, but every checkpoint of every group then pays make_pin's read of each segment
(install.hh:360), with or without a peer waiting: lost to the retry.

Cost: an install that finds its leader applied past the latest bundle waits one or two flights
more; while a peer awaits, each checkpoint reads and CRCs every segment for its pin on the chore,
and applies wait behind it. The fused item does the chore's work in another order: an apply that
arrives between capture and commit now waits for the commit. C5 measures both.

Lost:

- Persist the install's B and P_G with the index and accept P_H > E inside (B, P_G] (review F1
  (a)): an index ahead of its log stays legal, so the open needs a second rule, a floor that every
  checkpoint during the catch-up must move, and a clause for a lone voter (it would elect itself
  over the gap).
- Fill the learner's log through P_G inside consensus's install: a protocol change that moves log
  entries through the image channel.
- Keep every published bundle pinned (review D-1 (b)): no failed flight, at a full segment read
  per checkpoint in every group.

## D6. Which refusals repair in place

The refusal is one rule (D2); its remedy depends on what was lost. Pending the owner, this design
takes the default the lead set (2026-11-25):

- **Journal older, or another history, on a voter: refused and replaced.** The member lost
  entries it held; they may count toward commits it acknowledged, and its votes went with them.
  Started, it can vote a candidate missing a committed entry into leadership, and strata would
  skip that leader's replacements at or below P_H (review F1 (c)). Refused before Start it stays
  out of every quorum, so those entries keep their majority. Its remedy is a membership change:
  remove it and add a new incarnation, which installs. A lone voter has no quorum: its own
  journal must come back.
- **Index older (P_H below the base), on any replicated member: reinstalls in place.** The
  journal's votes and acknowledgments are intact; only the index lost state (an index directory
  lost or put back older, a plain-mode cut that dropped strata's last commit and kept the
  journal's checkpoint). The replica sets its index aside and reinstalls, as a damaged one does
  (repair.hh). A lone voter has no source: refused, it needs its index back.
- **A learner, by any refusal: reinstalls in place.** A learner neither counts toward commits
  (core.h:1631) nor campaigns (core.h:1468), and candidates ignore its grants, so refusing it buys
  no safety.

The refusal's text and group-open.md name the remedy. The replace operation (remove and re-add)
belongs to the cluster quest: strata::group and Xapiand have none today (Xapiand's shard groups
are lone voters, shard_group.cc:522 at journal-replaces-wal 7b57516). Under D5 no sound member has
a short log, so every replica shares the rule; only the remedy differs.

## D7. What a refusal leaves

The index and the set's files: byte-equal. The gate runs before the recovery chore item; the
stage, its marker and the quarantine are read, not changed. The journal: the same recovered state
at every attempt (base, last, terms, hard state). Three journal writes precede the gate, and each
keeps that state:

- a closed group's first recovery reopens its table entry and stages its checkpoint record
  (journal.h:604-610 at journal:9c7e31c), then stays equal (Q1);
- a bootstrap-only group's typed initialization, queued when the previous process exited
  (worker.h:178; `ready()` waits for it, :183);
- the pre-Start reclaim of a store above its control headroom (worker.h:502).

So the checks compare the recovered state at every attempt, and bytes only for a group recovered
from the turn log without pressure.

## States

What the index persists, what the journal holds, what the open compares, and the outcome. P_b is
the bundle's position, B an install's.

| State | Index | Journal | Compares | Outcome |
|---|---|---|---|---|
| clean stop (the close checkpoints through the commit index) | H = (P_b, T_b) | base (P_b, T_b), last >= P_b, closed in the table | H = base | opens; the closed record resumes its term |
| crash with a tail | H, P_H >= P_b | base (P_b, ·), entries through last >= P_H: an entry is durable before it commits, and strata applies only committed ones (core.h:1640-1644) | term at P_H = T_H | opens; replays from P_b |
| power cut, full flush mode | as the crash | as the crash | as the crash | opens |
| power cut, plain mode on macOS, journal tail lost | H as last committed | the drive may keep strata's last commit and drop the journal's last turns: last < P_H | holds(H) fails | refused before Start, nothing written (journal older, D6). Plain mode orders neither file against the other; for a lone voter every other outcome is the crash row; a replicated member that lost a vote or an acknowledgment above P_H is Q3 |
| power cut, plain mode on macOS, strata's last commit lost | H from the commit before | the journal kept its checkpoint: base past P_H | holds(H) fails | index older (D6): a replicated member reinstalls in place; a lone voter is refused and needs its index back |
| closed record | as the clean stop | the record matches the log (core.h:320-327) | H = base | opens with no election |
| checkpoint plus a tail | H, P_H >= P_b | base (P_b, ·), entries through last >= P_H | term at P_H = T_H | opens; replays from P_b |
| install: stage complete, consensus persisted it | old H, below B, or at or past it for a requested reinstall (core.h:767 admits B at or below the commit index when asked); stage at (B, T_B) | base (B, T_B) | the bundle's boundary = base | `resume` activates; opens at B |
| install activation (`activate_image`) | stage at (B, T_B), checked by `check_stage` | base (B, T_B) | none: the image replaces the index | H = (B, T_B) |
| restart after an activation, before the log reaches P_G | H = (B, T_B) or later: strata applies only what its log delivered (P_G is B under D5) | base (B, T_B), entries through last >= P_H | holds(H) | opens (refused at 31821753: F1) |
| half-activated crash (strata installed, the set's files not moved) | stage and marker for the bundle recovered | base (B, T_B) | the bundle's boundary = base | `resume` finishes; opens |
| member removed and re-added, its directory kept | H, not (0, 0) | the table holds no group | refused before `create` | nothing created (Q2) |
| lone voter | H | its log is the committed history | holds(H) | opens or refused; refused, it needs its own journal (or index) back |
| replicated member | H | its log; the leader sends what lies past it | holds(H) | opens or refused; a voter's journal older or another history is replaced, an index older or a learner reinstalls in place (D6) |
| journal put back from an older copy | H | last < P_H, or another term at P_H | holds(H) fails | refused before Start, nothing written |
| copy of a running data root | H | journal copied first: as the older copy; index copied first: holds H, or its base is past P_H | holds(H) | refused, or opens and replays more (a sound pair) |
| refused open retried N times | H | as recovered: each refusal starts nothing (D3) | the same | the same refusal every time |
| index damaged at open | marked; deferred index, H = (0, 0) | any | nothing | opens damaged; reinstalls |
| journal holds no group, fresh index | H = (0, 0) | none | - | creates; opens |
| log damaged (bounded) | any | the turn log lost the group | - | `recover` fails inside consensus, before the gate |
| witness | no index (witness.hh) | its log | nothing | opens |
| index put back older under a newer journal | H | base past P_H, or holds H | holds(H) | index older (D6), or opens and replays more |

## Contracts

`consensus:recover-gate` (consensus provides; strata consumes), in consensus's `docs/DRIVER.md`. A
value, not a callback (review D-8): every boundary the open accepts is known before `recover`.

```cpp
// recover, gated: once the log is recovered (and `closed` applied) and before Start, a group
// whose log retains none of `accept` (Core::retains_suffix, core.h:659-661) fences with a reason
// naming the log's base boundary and last index, and the call fails with `fenced`. Before Start
// nothing ran that changes the consensus state: no election, vote, append or checkpoint; the
// fenced Worker writes no close record (worker.h:689), so the next recover finds the same log.
// The failure is a `recover_refused` (a `fenced`) carrying what D6 needs to pick a remedy:
// struct recover_refused : fenced { LogBoundary base; Index last; Membership membership; };
// An empty `accept` is no gate (a damaged replica's, a witness's).
template <class Restore>
task::task<void> recover(GroupId, Restore, std::optional<std::string> closed, std::vector<LogBoundary> accept, task::cancel_token = {});
```

It rides the path a store above its headroom already fences on before Start (driver.h:803). The
reason's form, for the host to quote: `consensus: the recovered log retains no accepted boundary:
base (B, term T), last L`. strata passes H, plus a complete stage's bundle boundary (D2 case 1);
none for a replica marked damaged.

Its tests, in consensus: a lone voter refused at the gate persists no term and appends no NoOp,
and the next recover finds the same base, last and terms; the turn log and the host's table stay
byte-equal for a group recovered from the turn log without pressure; a follower refused at the gate
grants no vote; an accepted boundary at the base, inside the log, and an empty `accept` each start.

`strata:applied-term` (strata's core provides; strata::group consumes): `verdict
apply(const transaction&, files::position at, std::uint64_t term)`; `std::uint64_t applied_term()
const noexcept`, the term of the entry at `applied()`; the manifest log's `applied_term` scalar
after `applied_count` (`files::pre_freeze` moves); install adopts it from the source generation;
a standalone or logged index writes 0.

`strata:journal-behind` (this quest provides): D2 at the gate, D4, D5.

## Checks

Each a group test in `test/group/`, run as a job at the fixed commit; each mutant fails only its
named case (`--fails-without`).

- C1 (F2): a lone voter's journal older than its index (P_H = E + 1 and E + 2) opened P_H - E + 1
  times: each refuses naming H and the same base and last; the index and the set's files
  byte-equal and the journal's recovered state equal after each, its bytes too for a turn-log
  group. The case forces the election to persist before the check (926bd8's harness order), so
  mutant `check-after-start` (the check after `recover` returns) dies.
- C2 (F1): the reviewer's install-restart probe as cases: a joiner installs with 0 and 5 writes
  past the bundle, restarted by a kill, a power cut and a clean close: it opens each time, serves
  every acknowledged write, and its digest equals the replay. A `busy` case holds the learner's
  pin fetch until the leader's pinning checkpoint reaches consensus (busy_probe's order: refused
  Busy into the Sending flight) and installs within 2 flights. Mutants `pin-at-head` (make_pin at
  the leader's head) and `no-busy-retry` (D5 without the retry). simulation_group_corruption
  green.
- C3: one case per row of States, by value: the refusal names H, base and last; the files
  byte-equal; a sound row serves every acknowledged write. `missing` checks the table still holds
  no group after the refusal (review N1).
- C4: another history: a replicated member's journal copy holding an entry at P_H of another term
  is refused. Mutant `positions-only`.
- C5: D5's cost, a targeted timing job under `quiet exclusive`: quorum write latency (p50, p99)
  under periodic checkpoints while a learner installs, fused against split. Threshold: p99 within
  10% of the split order's; past it, the job names the honest cost.
- C6: the reviewers' probes rerun at the fixed commit: the install restart (b11e3f) and the
  simulation trace (0ee55a) pass; the erosion probe (926bd8) once the gate lands; the busy probe
  (991b02) finds no Busy surfacing after strata committed; the model (1b8f9a) installs at all 54
  timings under the retry.
- C7 (review D-3): a learner whose source pinned past its bundle: `check_stage` refuses, the
  stage is swept, the learner's own content is unchanged, and a correct pin of the same bundle
  then installs. Mutant `stage-head-unchecked`.

## Open questions

Answered by the lead on 2026-11-25, each (a):

- Q1. A closed group's first refused recovery changes the journal's bytes (D7) while its state
  stays: accepted; every attempt recovers the same log and refuses the same way.
- Q2. A member removed and re-added over a directory its host kept (review N4): refused (D4); a
  new incarnation starts in an empty directory its host provides.
- Q3. A journal rolled back to a copy taken after the index's last commit still holds H and opens;
  consensus's own durable state (votes, entries acknowledged past P_H) went back, which the index
  cannot see: stated as consensus's stable-storage assumption in the durability docs. Plain mode
  on macOS voids that assumption for replicated groups.

## First buildable steps

1. D5 on this quest's branch (needs neither contract): the fused checkpoint, the awaiting peers,
   the pinning checkpoint and its Busy retry, `make_pin` and `check_stage` at the bundle by
   position; C2, C5, C7 and simulation_group_corruption. It fixes F1 under the position check.
2. `consensus:recover-gate` (a consensus quest) and `strata:applied-term` (strata's core, outside
   this quest's file set), in parallel.
3. Then this quest: the binding read on the chore before `recover`, D2 in the gate, D4, the term in
   `check_stage`; the old position check goes; C1, C3, C4, C6; group-open.md rewritten from this
   design (review N3).
