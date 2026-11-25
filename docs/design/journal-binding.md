# Journal binding: the index names the entry it applied last

Status: design (quest strata-journal-behind, after its durability review's F1 and F2). The group's
part builds once `consensus:recover-gate` and `strata:applied-term` exist (Contracts); D5 needs
neither and can build first. Line references: strata:31821753, consensus:6d710cb (strata's pin),
journal:9c7e31c.

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

A refusal throws `group_fenced` naming both sides, for example: `strata::group: the recovered log
does not hold strata's head (12, term 3): base (7, term 2), last 9: the journal is older than the
index`. The open reads no file it does not read today: the stage and damage markers `adopt` reads
are read before `recover` instead, and the rule is one comparison.

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
  files and commits strata in one chore item, where today the capture and the commit are two
  (group.hh:1102, 1127). No apply runs between them, so the commit's generation is at P_b, except
  while recovery's replay is still below strata's head (apply skips there, and strata's head stays
  past the bundle).
- While a peer awaits an install, that commit also pins (`index::checkpoint()` and make_pin's file
  lists) in the same item; during that replay it does not pin, and the flight retries.
- At SnapshotNeeded for the latest bundle K with no pin: if strata's applied position is still K,
  it pins at once (`make_pin`, as today). Otherwise it starts a pinning checkpoint; this flight
  finds no pin or a moved base and fails, and consensus retries after rpc_timeout (core.h:840,
  847) at the new bundle, whose pin exists before it publishes.
- The learner's `check_stage` also requires the staged generation's head to equal the bundle's
  boundary before it answers validated: a source that pinned past its bundle fails the flight, and
  consensus never persists that install.

The leader's pins:

| Leader | SnapshotNeeded at the latest bundle K | a checkpoint publishes K' | the flight ends |
|---|---|---|---|
| no pin at K, strata at K | pins K now | - | - |
| no pin at K, strata past K | starts a pinning checkpoint; the flight fails | pins K' in its chore item | released; retried at K' |
| pin at K | the flight holds it | pins K' only while a peer awaits; K's pin stays while a flight holds it | unpinned; released once superseded and free |

Cost: an install that finds its leader applied past the latest bundle waits one rpc_timeout more,
and while a peer awaits an install each checkpoint commits twice (the pin). The fused item does the
chore's work in another order: an apply that arrives between capture and commit now waits for the
commit. C5 measures it against today's split.

Lost:

- Persist the install's B and P_G with the index and accept P_H > E inside (B, P_G] (review F1
  (a)): an index ahead of its log stays legal, so the open needs a second rule, a floor that every
  checkpoint during the catch-up must move, and a clause for a lone voter (it would elect itself
  over the gap).
- Fill the learner's log through P_G inside consensus's install: a protocol change that moves log
  entries through the image channel.

## D6. A replicated member is refused, not repaired from the quorum

A member whose log does not hold H lost entries it held; they may count toward commits it
acknowledged, and its votes went with them. Started, it can vote a candidate missing a committed
entry into leadership, and strata would skip that leader's replacements at or below P_H (review F1
(c)). Refused before Start it stays out of every quorum, so those entries keep their majority. Its
repair is a membership change: remove it, add a new incarnation, which installs. Under D5 no sound
member has a short log, so a lone voter and a replicated member share the rule; only the remedy
differs (a lone voter has no quorum: its own journal must come back).

## D7. What a refusal leaves

The index and the set's files: byte-equal. The gate runs before the recovery chore item; the
stage, its marker and the quarantine are read, not changed. The journal: the same recovered state
at every attempt (base, last, terms, hard state). Its bytes: equal at every attempt for a group
recovered from the turn log; a closed group's first recovery reopens its table entry and stages
its checkpoint record before any gate can run (journal's journal.h:604-610 at journal:9c7e31c),
then stays equal (Q1).

## States

What the index persists, what the journal holds, what the open compares, and the outcome. P_b is
the bundle's position, B an install's.

| State | Index | Journal | Compares | Outcome |
|---|---|---|---|---|
| clean stop (the close checkpoints through the commit index) | H = (P_b, T_b) | base (P_b, T_b), last >= P_b, closed in the table | H = base | opens; the closed record resumes its term |
| crash with a tail | H, P_H >= P_b | base (P_b, ·), entries through last >= P_H: an entry is durable before it commits, and strata applies only committed ones (core.h:1640-1644) | term at P_H = T_H | opens; replays from P_b |
| power cut, full flush mode | as the crash | as the crash | as the crash | opens |
| power cut, plain mode on macOS | H as last committed | the drive may keep strata's last commit and drop the journal's last turns: last < P_H | holds(H) fails | refused before Start, nothing written; plain mode does not order the two files (any other outcome is the crash row) |
| closed record | as the clean stop | the record matches the log (core.h:320-327) | H = base | opens with no election |
| checkpoint plus a tail | H, P_H >= P_b | base (P_b, ·), entries through last >= P_H | term at P_H = T_H | opens; replays from P_b |
| install: stage complete, consensus persisted it | old H, below B (consensus installs only past its commit index, core.h:767); stage at (B, T_B) | base (B, T_B) | the bundle's boundary = base | `resume` activates; opens at B |
| install activation (`activate_image`) | stage at (B, T_B), checked by `check_stage` | base (B, T_B) | none: the image replaces the index | H = (B, T_B) |
| restart after an activation, before the log reaches P_G | H = (B, T_B) or later: strata applies only what its log delivered (P_G is B under D5) | base (B, T_B), entries through last >= P_H | holds(H) | opens (refused at 31821753: F1) |
| half-activated crash (strata installed, the set's files not moved) | stage and marker for the bundle recovered | base (B, T_B) | the bundle's boundary = base | `resume` finishes; opens |
| member removed and re-added, its directory kept | H, not (0, 0) | the table holds no group | refused before `create` | nothing created (Q2) |
| lone voter | H | its log is the committed history | holds(H) | opens or refused; refused, it needs its own journal back |
| replicated member | H | its log; the leader sends what lies past it | holds(H) | opens or refused; refused, it is replaced (D6) |
| journal put back from an older copy | H | last < P_H, or another term at P_H | holds(H) fails | refused before Start, nothing written |
| copy of a running data root | H | journal copied first: as the older copy; index copied first: holds H, or its base is past P_H | holds(H) | refused, or opens and replays more (a sound pair) |
| refused open retried N times | H | as recovered: each refusal starts nothing (D3) | the same | the same refusal every time |
| index damaged at open | marked; deferred index, H = (0, 0) | any | nothing | opens damaged; reinstalls |
| journal holds no group, fresh index | H = (0, 0) | none | - | creates; opens |
| log damaged (bounded) | any | the turn log lost the group | - | `recover` fails inside consensus, before the gate |
| witness | no index (witness.hh) | its log | nothing | opens |
| index put back older under a newer journal | H | base past P_H, or holds H | holds(H) | refused (index older), or opens and replays more |

## Contracts

`consensus:recover-gate` (consensus provides; strata consumes), in consensus's `docs/DRIVER.md`:

```cpp
// GroupState gains the log's base boundary, beside last.
struct GroupState { Role role; Term term; NodeId leader; Index committed, applied, last; Membership membership; bool resting = false; LogBoundary base; };
// On the loop: the term of the open group's entry at `index` (the base's term at base.index);
// nullopt outside [base.index, last], at 0 with base 0, or for a group not open (Core::term_at).
std::optional<Term> term_at(GroupId, Index) const;
// recover, then `admit()` on the loop once the log is recovered (and `closed` applied), before
// Start; state() and term_at() read the recovered log there. A throw out of admit completes the
// call with it: the Worker never starts (no election, vote, append or checkpoint), and the row's
// close releases the group without a close record, so the next recover finds the same log.
template <class Restore, std::invocable<> Admit>
task::task<void> recover(GroupId, Restore, std::optional<std::string> closed, Admit admit, task::cancel_token = {});
```

Its tests, in consensus: a lone voter refused at admit persists no term and appends no NoOp, and
the next recover finds the same base, last and terms; the turn log and the host's table stay
byte-equal for a group recovered from the turn log; a follower refused at admit grants no vote.

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
  times: each refuses naming H and the same base and last; the index, the set's files and the
  journal byte-equal after each. Mutant `check-after-start` (the check after `recover` returns).
- C2 (F1): the reviewer's install-restart probe as a case: a joiner installs with 0 and 5 writes
  past the bundle, restarted by a kill, a power cut and a clean close: it opens each time, serves
  every acknowledged write, and its digest equals the replay. Mutant `pin-at-head` (make_pin at
  the leader's head). simulation_group_corruption green.
- C3: one case per row of States, by value: the refusal names H, base and last; the files
  byte-equal; a sound row serves every acknowledged write. `missing` checks the table still holds
  no group after the refusal (review N1).
- C4: another history: a replicated member's journal copy holding an entry at P_H of another term
  is refused. Mutant `positions-only`.
- C5: D5's cost: quorum write latency (p50, p99) under periodic checkpoints, fused against split,
  a targeted timing job.
- C6: the reviewer's probes (b11e3f, 926bd8, 0ee55a) rerun at the fixed commit pass.

## Open questions

- Q1. A closed group's first refused recovery changes the journal's bytes (D7) while its state
  stays. (a) Accept: every attempt recovers the same log and refuses the same way. (b) journal
  defers a closed group's reopen until the gate passes (a journal API change). Recommend (a).
- Q2. A member removed and re-added over a directory its host kept (review N4). (a) Refuse: one
  rule, and a new incarnation starts in an empty directory its host provides. (b) Set the index
  aside (`iq.`) and reinstall: repairs a replicated joiner by itself, does nothing for a lone
  voter, keeps the old bytes until an activation. Recommend (a).
- Q3. A journal rolled back to a copy taken after the index's last commit still holds H and opens;
  consensus's own durable state (votes, entries acknowledged past P_H) went back, which the index
  cannot see. (a) State it as consensus's stable-storage assumption in the durability docs. (b) A
  consensus quest for rollback detection (an epoch the host's table keeps). Recommend (a).

## First buildable steps

1. D5 on this quest's branch (needs neither contract): the fused checkpoint, the pin flow,
   `check_stage`'s head check by position; C2, C5 and simulation_group_corruption. It fixes F1
   under today's position check.
2. `consensus:recover-gate` (a consensus quest) and `strata:applied-term` (strata's core, outside
   this quest's file set), in parallel.
3. Then this quest: the binding read on the chore before `recover`, D2 in the gate, D4, the term in
   `check_stage`; the old position check goes; C1, C3, C4, C6; group-open.md rewritten from this
   design (review N3).
