# Track C, C1 gate report (in progress)

C1 (membership changes and voter roles) as built in `consensus` on `main`; the progress
table in `docs/design/track-c.md` lists the commits. This report collects the gate rows
measured so far.

## Response to the third revision check (`9128a07`)

The review found M1–M7 and added conditions 5 and 6. Each fix is in `consensus` with a
regression in `test/membership_test.cc` that fails without it:

| Finding | Commit | Change |
|---|---|---|
| M1 | `9f7d43e` | Learners and joiners vote; the survivors of a promoted learner's leader elect one |
| M2 | `9f7d43e` | A snapshot names its base membership for anyone outside the founding voters; a joiner refuses one that does not, and cannot checkpoint below the entry that names its membership |
| M3 | `9f7d43e` | The `Worker` accepts snapshots from any authenticated incarnation, serves every peer the core replicates to, and sizes its output by members |
| M4 | `9f7d43e` | A membership is checked against its own limits; `Configuration` entries are bounded by their encoding; a group's limits stay within its node's |
| M5 | `9f7d43e` | A removed member is pruned and its snapshot released (`Removed`); a self-removing leader transfers first |
| M6 | `9f7d43e` | A new member joins only as a learner |
| M7 | `c5b2223` | T2's harness, below |
| Condition 5 | `9f7d43e` | The witness bound is the current leader's report, clamped to its commit |
| Condition 6 | `9f7d43e` | `Timing{}` is R4's default profile; tests run a compact profile |

`can_change_membership()` is now false during a transfer or under a witness leader
(`c5b2223`).

## Response to the fourth revision check (`2e1f3c3`)

| Finding | Commit | Change |
|---|---|---|
| M4's remainder | `b7c9a2e`, `9bf8623` | The host reports each member's node limits (`PeerLimits`, from the hello). A leader admits commands and sizes RPCs within its own node's and every reported member's, and refuses group limits above a reported member's. `Worker::found` checks founding limits, and RFTINIT2 keeps them. The leader's wire limits are cached. |
| Condition 12 | `75ad450`, below | Five forced schedules, counted; D3(a) off recorded as scripted; T2's figures name their commit and command |
| Condition 13 | `6c6e007` | T3's membership rows |
| Condition 14 | `7491ebb` | T4 through `Worker`s, which found that `SnapshotChannel` made channels only for founding voters |
| Condition 15 | `1d78fd0`, below | Measured hours per decoder, sanitizers, coverage, scale |
| Condition 16 | `75ad450` | A joiner's retained install takes the membership it names |
| Condition 17 | strata `06c96c7` | Progress, Still owed, conditions 8 and 9 |

The full budget found one more core bug, fixed in `37786e3`: a node whose limits were
lowered across a restart could not reopen its own log. Recovery now checks history by
the group's limits at each position, or the ceilings where the group set none.

## T1 Compatibility

`consensus` `1822fd7`. Fixtures written by `b7073bb`'s own encoders (Detent's pin) recover
with today's code:

- an initialization record and three storage batches;
- checkpoint bundles v1 and v2;
- an `RFTSNP01` descriptor.

A fixed group's records, bundles, and descriptors re-encode to the same bytes.

`tools/compat.sh` builds `b7073bb` with its pinned dependencies. Its reader refuses each
format Track C added: `RFTINIT2`, `RFTJOIN1`, `RFTBTCH2`, checkpoint bundle v3,
`RFTSNP02`, and a `DTC2` frame. It accepts none.

## T2 Safety

The randomized simulation (`consensus` `test/simulation_test.cc`) has every element of
T2's fault mix and every oracle T2 lists, plus these:

- Crashes and partitions on every seed, and crashed nodes restart over time.
- Linearizable reads.
- Checkpoints and snapshot installs, with oracles that read through the base.
- Limit changes.
- Unequal node limits, which each node reports as a hello would.
- A check that each node's membership in force is the one its durable log implies.

**Forced schedules.** Five run per seed, each counted:

1. A `Configuration` entry in a replaced suffix.
2. A crash between promotion and removal.
3. A removed leader with a snapshot in flight. On five-voter seeds the removal commits
   while a cut-off voter's snapshot is still in flight.
4. A self-removal while the leader reaches one other voter.
5. A delayed duplicate of every `TimeoutNow`.

In schedule 3 a read is started with the removal, but it completes in the same quorum
round that commits the removal, so no read is still in flight at that moment.
`membership_test` scripts the single-server-change bug. T2's text records D3(a) off as
detected by that scripted schedule, not the randomized search.

**The full budget.** `consensus` `37786e3`, run as `consensus_simulation_test 2500 10000
START` for START in 1, 2501, 5001, and 7501, so 10,000 seeds of 10,000 steps (10^8 steps).
Every oracle passes. Summed over the four parts:

- 22,564 committed memberships.
- 19,623 linearizable reads.
- 46,400 checkpoints and 20,296 installs.
- 9,268 limit changes.
- 3,333 replaced `Configuration` entries.
- 8,663 forced self-removals.
- 4,311 crashes between promotion and removal.
- 135 removals committed with a snapshot in flight.
- 46,397 delayed duplicate `TimeoutNow`s.

Earlier full runs found three failures, now fixed:

- **`aa4b11e`, seed 493:** the harness's snapshot host dropped an unserved request.
- **`b7c9a2e`, seed 9264:** a false positive in the leader-completeness oracle, which
  checked entries committed in later terms.
- **`75ad450`, seed 6294:** a node whose limits were lowered across a restart could not
  reopen its own log. That was a core bug, fixed in `37786e3`: recovery now checks
  history by the group's limits, or the ceilings, not the node's.

**Seeded faults.** `consensus` `9bf8623`, run as `tools/mutants.sh build 400 10000`. The
table shows which suite catches each, and the first seed for the simulation:

| Mutant | Unit | Membership | Simulation |
|---|---|---|---|
| Sends before durability | yes | — | seed 1 |
| Votes and pre-votes without the log check | yes | — | seed 1 |
| Repair with a delivery outstanding | yes | — | seed 6 |
| D3(a) off | yes | yes (scripted: two memberships committed at one index) | no |
| D1's rollback off | yes | — | seed 4 |
| A removed leader counts itself | yes | — | seed 2 |
| A follower checks by its node's limits | yes | — | no |
| A leader grants pre-votes | yes | — | no |
| Learners refuse votes | — | yes | seed 314 |
| Repair below `base_index` | yes | — | seed 1 |
| A leader ignores members' node limits | — | yes | seed 14 |
| A commit of an earlier term by counting | — | — | seed 1 |

Three mutants are not caught by the randomized search on its own:

- **D3(a) off** falls to the scripted schedule.
- **The follower-limits mutant** is equivalent now. A leader keeps every entry within
  every member's node limits, so a follower checking by its own limits refuses nothing
  that a check by position would accept.
- **A leader granting pre-votes** is a liveness fault, which the unit suite and T3
  catch.

The unclamped heartbeat commit joins the list with coalesced heartbeats (D12).

## T3 Liveness, simulated time

`consensus` `test/liveness_test.cc` at `6cc0d88`, 400 seeds (3 and 5 voters alternating);
rerun at `aa4b11e`, after the review's fixes, with identical results.
It is a discrete-event cluster of `Core`s in simulated milliseconds:

- **Barriers:** 99% drawn from [b/10, b] and 1% from [b, 3b], so p99 is b.
- **Messages:** one way takes [r/4, r/2].
- **Busy cores:** messages that reach a core while it persists are held and delivered
  after the persist, as a `Worker`'s caller holds them.

Timing follows R4. The default profile is Detent's authority profile at b = 80 ms. The
others are derived for each platform's estimated b, from the commit-barrier medians in
`docs/perf/commit-barriers.md`. The journal's own barrier p99 per platform is still to
be measured.

| Profile | b | Heartbeat, RPC, elections (ms) | Healed partition commits, p99 / max | Transfer p99 | Deaf leader steps down, max | New leader commits, p99 | Healthy term raised |
|---|---:|---|---:|---:|---:|---:|---:|
| Default (macOS x86_64) | 80 | 100, 500, 1,000–2,000 | 250 / 779 ms | 205 ms | 2,019 ms | 4,233 ms | 0 of 400 |
| macOS arm64 APFS | 20 | 12, 100, 126–252 | 71 / 90 ms | 58 ms | 253 ms | 641 ms | 0 of 400 |
| Linux ext4 | 30 | 18, 128, 186–372 | 100 / 129 ms | 79 ms | 374 ms | 797 ms | 0 of 400 |
| FreeBSD UFS | 15 | 9, 100, 96–192 | 45 / 54 ms | 45 ms | 192 ms | 458 ms | 0 of 400 |

**Membership rows** (condition 13): `consensus` `6c6e007`, `consensus_liveness_test 400`.
The leader fails in a promotion's window, or in a replacement's (after the promotion
commits, before the removal does), and a new leader commits. Separately, a voter cut off
before learning of its removal returns.

| Profile | Promotion window, p99 | Replacement window, p99 | Removed voter raised the term |
|---|---:|---:|---:|
| Default (macOS x86_64) | 3,123 ms | 3,336 ms | 0 of 400 |
| macOS arm64 APFS | 467 ms | 438 ms | 0 of 400 |
| Linux ext4 | 636 ms | 659 ms | 0 of 400 |
| FreeBSD UFS | 342 ms | 342 ms | 0 of 400 |

None failed, and all stay within 5 s.

Against T3's bounds:

- **Healed partition commits** within 5 s at p99 (10 s worst): pass.
- **A partitioned node never raises the healthy group's term:** pass.
- **A transfer completes** within 500 ms at p99: pass.
- **A deaf leader steps down** within `election_max`, plus one heartbeat of tick
  granularity: pass.
- **A new leader commits** within 5 s: pass. The default profile's 4,233 ms is the
  closest to its bound. It is CheckQuorum's `election_max` (2,000 ms) plus an election
  drawn from 1,000–2,000 ms.

The harness found two core bugs on its first runs, both fixed:

1. **`2f8e6e2`: a leader granted pre-votes.** A node rejoining a 3-voter group with an
   up-to-date log won a pre-vote quorum with the leader and deposed it. That happened in
   1 to 7 of 20 seeds per profile, the default included.
2. **`6cc0d88`: a witness could hand off to a cut-off data voter.** It then waited out
   the transfer's abort.

`TimeoutNow` is now resent each heartbeat during a transfer. Before that, 19 of 80
transfers in the first 20-seed run did not complete, one of them under the default
profile.

Still owed for T3:

- The quiescent-group rows: a quiescent leader's node failing, a restart leaving idle
  groups' leaders unchanged, and 1,000 idle groups for 60 s. They need C2's quiescence
  (D12).
- The journal's measured barrier p99 per platform, replacing the estimates.
- The three-process run on real storage on each platform.

## T4 Learners and witnesses

| Row | Where | Result |
|---|---|---|
| A learner catches up by append and is promoted | `simulation_test` (joiners, 2,000 seeds) | Pass |
| A learner catches up by install, with the base membership, and is promoted in place of a voter | `consensus_test` `learner_install_and_promotion` (`7c60f58`) | Pass |
| A `2 + witness` group is founded as one (`RFTINIT2`) | `consensus_test` `founding_records`, `worker_test` `founding_and_join_records` | Pass |
| A `2 + witness` group survives any single failure | `liveness_test`, 1,200 failures per profile | Pass; recovery p99 2,301 ms (default), 444 (arm64), 611 (ext4), 295 (UFS) |
| A witness leader hands off within `election_max` | `liveness_test`, about 200 hand-offs per profile | Pass; longest 385 ms (default), 115, 159, 71 |
| The witness's truncation bound | `consensus_test` `witnesses` (`91de8ea`) | A checkpoint past the data voters' match is refused |

Added after the review (`aa4b11e`):

| Row | Result |
|---|---|
| A node that lost its disk rejoins as a new incarnation and replaces the old one | Pass (core) |
| A witness at its limit with a data voter down stalls commits and truncates nothing past that voter; it resumes once the voter returns | Pass |
| A witness replaced through the 2 + 2 window while a data voter is down | Pass |

Through `Worker`s (condition 14, `consensus` `7491ebb`, `worker_test`'s
`worker_rejoin_by_install`):

1. Voter 3 loses its disk and returns as joiner 4.
2. The leader adds 4 as a learner.
3. 4 installs the leader's image through real `SnapshotChannel`s and catches up.
4. 4 is promoted in place of 3, which the core removes.
5. 4's store reopens with the membership in force.

The test found that `SnapshotChannel` made channels only for founding voters. A channel
is now created when a new peer's session opens.

Still owed for T4: the 10 GiB install at 10 ms of added round trip, under 10 MB/s of
writes, with 1,000 groups. It needs C2 and C4.

## T8 Robustness

`consensus` `fuzz/fuzz_decoders.cc` (`dd82c06`, `056fcb7`) covers the control codec,
memberships, founding and join records, storage batches, checkpoint bundles, and
snapshot descriptors. Its checked-in corpus replays as a test. Its first minutes found a
non-canonical `RFTINIT2`, which recovery now refuses.

`data[0]` picks the decoder uniformly, so each decoder gets an equal share of the hours:

| Run | Code | Decoders | CPU-hours | Executions | Crashes |
|---|---|---|---:|---:|---:|
| 1 | `dd82c06` | 6 (before `RFTSNP02`) | 6.0 | 444 million | 0 |
| 2 | `9f7d43e` | 7 | 7.0 | 471 million | 0 |

Run 2 gave each decoder about one CPU-hour on the code after the review's fixes, which
T8 asks.
