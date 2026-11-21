> Status: planning and the root's operations built (quest placement, task `repair`); tested on
> live groups whose learners catch up from the log. A learner behind a truncated log installs
> (group-heal's `strata:group-install`, waiting on Detent's `consensus-driver-install`). Design:
> Xapiand `docs/design/cluster-release.md` D6, D7, D12; contract `strata:placement-repair`.

# Placement repair

The root keeps every shard at its `rf`. A pass at the root leader reads the records
(`placement-root.md`), each shard leader's report and the keepalive table, and plans one step per
group. Planning is a pure function, `include/strata/placement/repair.hh`; a binding runs the steps:

| Step | Runs as |
|---|---|
| `record` | `record_shard`: the shard's members as the group holds them, at the root (loads follow) |
| `change` | `change_membership(next)` at the group's leader; a new learner's node opens it as a joiner first |
| `transfer` | `transfer_leadership(target)` at the group's leader |
| `wait`, `none` | nothing; `why` says what it waits for |

A step runs only when its group's last one has answered, and every pass plans afresh from what is,
so a lost or refused step (a promotion consensus answers `NotReady`) is planned again.

## What a pass reads

- **Records.** Each namespace's shards and `rf`, the nodes' states and loads, and the settings
  record `S` (`replace_after_ms`, default 300 s). The setting is read at every pass, so
  `set_replace_after` takes effect at the next one, with no restart.
- **The keepalive table** (D6): `down_since` per node, volatile at the root leader. A node is
  *lost* once down for `replace_after`, or when its state is not `active` (a drain).
- **A report per group**: the membership in force at its leader, the leader's incarnation, and
  whether the application holds an open multi-entry command (D12's chunked command). No leader, no
  report: an `rf = 1` group whose node is down waits for it, since no leader can add a learner.

## The plan

The shape for `rf` on the usable (not lost) nodes is placement's: `min(rf, usable)` data voters,
and never a lasting two-voter group: two take a witness on a third node, else the second is a
learner. Members on lost nodes are *bad*. In order, the first that applies:

1. **Record.** The record differs from the group: write the group as it is, the preferred leader
   first. The preferred leader stays while it is a healthy voter, unless another healthy voter has
   two fewer leaderships; then that one, which spreads leaders without flapping. Within a pass,
   each record's load changes count before the next shard plans.
2. A **leaving** member: consensus removes it next (D3); wait.
3. A bad **learner** leaves.
4. Too few **data voters**: promote an up learner beside the first leaving choice consensus
   accepts (a bad data voter, a bad witness, a witness past the shape, none); with no learner, add
   one on the up node in no member with the fewest groups.
5. Too few **witnesses**: the same, promoting to witness beside a bad witness or the data voter a
   shrink drops.
6. Too few learners (a short shape): add one.
7. **Removals**, each only if valid: bad witnesses, bad data voters, a witness past the shape, a
   data voter past it (demoted to learner when the short shape wants one), a learner past it.
8. **Leadership** to the preferred leader.

Every change is checked with consensus's `validate_change` and must keep a quorum of up voters,
so the plan never proposes what consensus would refuse or what would stall the group. A change
that removes or retires the current leader moves leadership first, to an up healthy voter.

D7's orders fall out of these rules:

| From | To | Steps |
|---|---|---|
| rf 1 | 2 | learner, promoted (two data voters for the window); learner, promoted to witness |
| rf 2 | 3 | learner, promoted beside the leaving witness (3 + 1), which consensus removes |
| rf 3 | 2 | learner, promoted to witness beside a leaving data voter (3 + 1) |
| rf 2 | 1 | witness removed; data voter removed |
| replace | | learner, promoted beside the lost member (3 + 1, 2 + 2 or n + 1), which consensus removes |

With no free node, a lost voter leaves when the rest keep a quorum and the shard runs short (three
voters on two usable nodes become a voter and a learner); a 2 + witness group with a lost data
voter and no free node waits, as removing either member would break D9's shapes or its quorum.

## Leader balance and multi-entry commands

A chunked command's chunks carry their proposer's term, and the first entry of a higher term
retires every open one (D12): a leadership change mid-upload aborts it. Every transfer the plan
makes, for balance or before retiring a leader, waits while the report says a command is open,
and is made at the first pass after it ends. Writes that begin after a pass planned a transfer
can still be cut; the deferral narrows the window, the client retries.

## The root's operations

`set_replace_after(ms)`, `set_rf(path, rf)` (Xapiand's `number_of_replicas` + 1), `drain(node)`
(state `leaving`; `drained` once its load holds no group, then `remove_node`), and
`record_shard(path, shard)`. The root group itself is planned with no record (`plan(nullptr,
voters, ...)`): a lost root voter is replaced like any member, and the new voter installs the
root's records by catching up from the log, or by install once group-heal binds it.

## Proof

- `test/placement/repair_plan_test.cc` (ctest `placement_repair_plan`): against a model group
  that applies each step as consensus would, D7's 1, 2, 3, 2, 1 with exact memberships, a
  replacement at replace_after and not before (3 voters, 2 + witness's data voter and witness),
  replace_after read from the root's record as it changes, a drain whose transfer precedes the
  change that retires the leader, an `rf = 1` group that waits, a shard with no node to grow on,
  leader balance deferred while a command is open; and 3,000 seeded clusters of 1 to 8 nodes,
  each changed four times (nodes lost or drained, rf from 1 to 5): every change valid with a
  quorum of up voters, and at rest every shard at its shape with the preferred leader leading,
  or waiting with no free node.
- `test/placement/repair_group_test.cc` (ctest `placement_repair_group`), on six nodes with the
  root on three and shard groups under writes: `replace` (no member moves under the default
  300 s; `set_replace_after` takes effect at the next pass; the shards and the root replace a
  lost root voter's members), `drain`, `replicas` (rf 1, 2, 3, 2, 1), `waits` (`rf = 1`),
  `defer` (A4). After each converges: every acknowledged write on every data member, their
  digests equal, the root's records equal on every root member, each shard at its shape and
  recorded as it is, the loads a recount.
- `test/placement/repair_mutants.py`: one mutant per mechanism, each killed by the case it names.
