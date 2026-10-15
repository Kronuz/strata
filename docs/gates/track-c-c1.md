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
| Condition 15 | `1d78fd0` through `56641cb`, T8 below | Measured hours per decoder (the transfer frame added), sanitizers, coverage at the targets, scale |
| Condition 16 | `75ad450` | A joiner's retained install takes the membership it names |
| Condition 17 | strata `06c96c7` | Progress, Still owed, conditions 8 and 9 |

The full budget found one more core bug, fixed in `37786e3`: a node whose limits were
lowered across a restart could not reopen its own log. Recovery now checks history by
the group's limits at each position, or the ceilings where the group set none.

## Response to the fifth revision check (`00011c5`)

| Finding | Commit | Change |
|---|---|---|
| M8 | `c8ba286` | A leader sizes each peer's appends by that peer's limits alone; the minimum over every reported member bounds only new commands. A peer below an entry it needs hears paced heartbeats (a successful empty append to it no longer resends at once, which looped without bound) and catches up by install once a checkpoint covers the entry. `membership_test` `per_peer_wire_limits`: the probe's lowered report, and a learner added with smaller limits while a larger entry is uncommitted; both live-locked before |
| M9 | `c8ba286` | Open sessions count against the member limit, and a closed channel of a peer no longer a member is reused. `worker_test`: three times the limit's peers come and go without a refusal, and the channel count stays within the limit; it fails at the old code |
| Condition 18 | `c8ba286` | A `PeerLimits` report that fails `validate_limits` is ignored; `invalid_peer_limits` commits after two `rpc_entries` 0 reports, and fails without the check |
| Condition 19 | `faef7ad` | T3's promotion row cuts the leader's link to the learner first, and fails the leader only once the promotion is committed on the old voters and held by both, not by the learner. The real core passes 100 of 100 seeds per profile; the "learners refuse votes" mutant fails 100 of 100 |
| Condition 20 | `faef7ad` | `read_across_self_removal`: a read admitted before the leader's self-removal is not completed by one remaining voter's acknowledgement plus the leader's own; it completes or is refused once both remaining voters answer. The mutant "a read counts every acknowledgement" joins `tools/mutants.sh` and is caught |
| Rerun | `faef7ad` | The full T2 budget (10,000 seeds × 10,000 steps, the four commands above): ok, with 4,868–4,941 linearizable reads and 29–39 removals committed with a snapshot in flight per 2,500 seeds. T3 at 400 seeds: 0 failures. `ctest` 18 of 18 and 13 of 13 mutants; ASan with UBSan and TSan 19 of 19 |
| Condition 21 | `7046856` | Still owed no longer lists T4 through the Worker; condition 8's corruption case states its cost; `retirement.h` is core code for Goal 5's audit, not a transport header; the channel text follows M9 |

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

**Membership rows** (conditions 13 and 19): `consensus` `faef7ad`, `consensus_liveness_test 400`.
The leader fails in a promotion's window, or in a replacement's (after the promotion
commits, before the removal does), and a new leader commits. Separately, a voter cut off
before learning of its removal returns.

| Profile | Promotion window, p99 | Replacement window, p99 | Removed voter raised the term |
|---|---:|---:|---:|
| Default (macOS x86_64) | 2,175 ms | 3,336 ms | 0 of 400 |
| macOS arm64 APFS | 483 ms | 438 ms | 0 of 400 |
| Linux ext4 | 678 ms | 659 ms | 0 of 400 |
| FreeBSD UFS | 319 ms | 342 ms | 0 of 400 |

The promotion row holds the promotion back from the learner (condition 19): the leader
fails once the promotion is committed on the old voters and held by both, not by the
learner, so a new leader commits only with the learner's vote.

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

At `consensus` `56641cb`. Builds and runs on macOS x86_64 with Homebrew LLVM 23; the M1
is unreachable.

### Fuzzing

`fuzz/fuzz_decoders.cc` covers eight decoders: the control codec, memberships, founding
and join records, storage batches, checkpoint bundles, snapshot descriptors, and, since
`acc1f50`, the snapshot transfer frame (`RFTXFR01`), which had no target before. Each
input must decode or throw, and what decodes must re-encode to the same bytes. Its first
minutes found a non-canonical `RFTINIT2`, which recovery now refuses.

`FUZZ_DECODER` pins every input to one decoder, and `tools/fuzz.sh` gives each its own
CPU-hour, four at a time (condition 15). ASan and UBSan with `-fno-sanitize-recover=all`,
`-max_len=8192`:

| Decoder | Code | CPU-hours | Executions | Crashes |
|---|---|---:|---:|---:|
| Control codec | `1d78fd0` | 1.0 | 123,228,844 | 0 |
| Membership | `1d78fd0` | 1.0 | 149,154,413 | 0 |
| Founding record | `1d78fd0` | 1.0 | 76,627,012 | 0 |
| Join record | `1d78fd0` | 1.0 | 98,265,692 | 0 |
| Storage batch | `1d78fd0` | 1.0 | 72,292,825 | 0 |
| Checkpoint bundle | `1d78fd0` | 1.0 | 123,709,510 | 0 |
| Snapshot descriptor | `1d78fd0` | 1.0 | 171,866,499 | 0 |
| Transfer frame | `acc1f50` | 1.0 | 224,904,031 | 0 |

No decoder changed between `1d78fd0` and `56641cb`. Pinned runs ignore the selector
byte, so each input's first byte is set to its decoder before a libFuzzer `-merge=1`
into `fuzz/corpus/decoders` (`9553c35`, `56641cb`): 358 inputs with 445 new edges. The
replay test runs all 377 inputs. Earlier runs, with the decoder picked by `data[0]`: 444
million executions over six decoders at `dd82c06`, and 471 million over seven at
`9f7d43e`, with no crashes.

### Sanitizers

Every test but the mutants passes at `56641cb` under ASan with UBSan
(`-fsanitize=address,undefined -fno-sanitize-recover=undefined`) and under TSan: 19 of
19 in each. The mutants rebuild the core with its own flags; they run in the plain build
(below).

### Coverage

`tools/coverage.sh` (`755c37d`) runs every test but the mutants in a
`-fprofile-instr-generate -fcoverage-mapping` build, and unions each header's lines and
branch outcomes over the test binaries with strata's `tools/coverage_union.py`
(`--headers`, `d7a4f7d`). A single-byte coverage build confirms any saturated count;
none were. The headers C1 changed:

| Header | Lines | Branches | Target |
|---|---:|---:|---|
| `admission.h` | 98.11% | 82.50% | 90/80 |
| `checkpoint.h` | 100.00% | 88.57% | 90/80 |
| `checkpoint_encoder.h` | 94.96% | 93.24% | 90/80 |
| `checkpoint_format.h` | 100.00% | 100.00% | 90/80 |
| `core.h` | 96.36% | 80.83% | 90/80 |
| `membership.h` | 100.00% | 96.28% | 90/80 |
| `snapshot.h` | 100.00% | 100.00% | 90/80 |
| `snapshot_channel.h` | 97.00% | 80.21% | 90/80 |
| `snapshot_transfer.h` | 100.00% | 96.88% | 90/80 |
| `storage.h` | 97.88% | 84.29% | 90/80 |
| `transport/codec.h` | 94.74% | 87.50% | 90/80 |
| `types.h` | 100.00% | 100.00% | 90/80 |
| `worker.h` | 96.74% | 80.14% | 90/80 |

The decoders, measured over each decode function's lines (target 98/95):

| Decoder | Lines | Branches | Not reached |
|---|---:|---:|---|
| `transport::decode` | 93/97 | 48/48 | Four closing braces after `break`, which emit no code a test can reach |
| `decode_membership` | 34/34 | 21/22 | A `uint64` above `size_t`'s maximum, impossible on 64-bit targets |
| `Recovery::initialization` (founding, join) | 34/34 | 40/40 | — |
| `Recovery::decode` (storage batch) | 32/35 | 35/40 | The overflow guard on the ceilings, compile-time constants that never overflow |
| `decode_checkpoint` | 47/47 | 48/50 | The bundle's 16 GiB bounds, beyond any test's input |
| `decode_snapshot` | 19/19 | 16/16 | — |
| `decode_transfer` | 28/28 | 65/68 | Three field checks the fuzzer's hour did not reach |

The decoders' size bounds sit above the fuzzer's 8 KiB inputs, so a unit test reaches
them (`755c37d`): the control frame and its payload total, the `RFTSNP02` descriptor,
the initialization record, and storage payloads past the RPC ceiling (four entries at
the 64 MiB command ceiling, then one byte). It also builds the empty storage batch the
fuzzer never did.

Reaching the targets took tests of the defensive paths, `b684958` through `8e6a63f`:

- Hostile transfer frames: a frame bound to another session (each of the six envelope
  fields), a Begin naming another boundary, unowned and out-of-sequence Chunks, Results
  with a missing or unexpected term, a bad frame magic or length, and invalid sessions
  close only that session. Frames naming no transfer in progress are ignored.
- The receiving side: a second Begin waits, a conflicting one closes the session, and a
  worker without snapshot reception refuses one.
- The worker's snapshot, checkpoint, and source API on an unopened, closing, and fenced
  worker, with another worker's ids and with stale ids of its own; construction,
  scheduling, and founding guards.
- The checkpoint encoder's suffix checks, which a streamed source reaches, and the
  bundle bound's overflow.

The transport headers C1 did not change, measured by the same run, stay below the
targets: `hello.h` 94.29/68.42, `profile.h` 88.46/65.00, `session.h` 86.17/75.68,
`snapshot_write.h` 88.46/70.00, `tls.h` 92.00/69.57; `rpc_observation.h` 97.56/82.35
passes, and `peers.h` is instantiated by no test. C4 rewrites them (D16), so their tests
come with it. `retirement.h` (40.38/20.83) is core code unchanged since before Track C,
not a transport header; its gap belongs to T8's Goal 5 audit before Phase 3R adopts
`consensus`.

### Scale at 1,000 groups per node

- **Memory per group.** The snapshot channel holds one 65,688-byte input buffer per
  peer it has a session with, created for the founding voters and on demand for later
  members (`7491ebb`). A three-voter group holds two, 131,376 bytes of its 136,168 at
  rest (D17's figure); at 1,000 groups per node that is 131 MB. D17 moves these
  buffers to the transfer pool, at most `snapshot_concurrency` per node.
- **Peer limits.** Each core keeps the limits its members' nodes report, one `Limits`
  (80 bytes) per member in a map: 400 bytes for five members, 0.4 MB at 1,000 groups.
  They are the same for every group a node shares with a peer; DTPROF02's hello carries
  them once per node pair, and the host can then share one table.
- **Wire limits** are computed once per membership or report change (`9bf8623`), not per
  heartbeat: O(members) per change.
- **Files.** C1 adds none: membership, founding, and join records are entries and
  records in the group's existing journal; `RFTSNP02` replaces `RFTSNP01` in place.
- **Work.** A membership change is one log entry. Validation is O(members), at most 255.
  A removed member's snapshot is released (M5). Its snapshot channel, once its session
  closes, is reused by a later peer (`c8ba286`, M9); open sessions count against the
  member limit.

### Mutants

All 12 seeded faults in `tools/mutants.sh` are caught (`ctest` at `c0e8ffb`, 375 s; no
header changed through `56641cb`).
Each suite now runs under a watchdog: a stale run had hung the `ctest` for 7 h 50 min,
and a hang now counts as a catch.
