# Track C, C1 gate report (in progress)

C1 (membership changes and voter roles) as built in `consensus` on `main`; the progress
table in `docs/design/track-c.md` lists the commits. This report collects the gate rows
measured so far.

## T3 Liveness, simulated time

`consensus` `test/liveness_test.cc` at `6cc0d88`, 400 seeds (3 and 5 voters alternating).
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

Still owed for T4:

- A node that lost its disk rejoins as a new incarnation, end to end through the `Worker`.
- The witness at its retained-log limit with a data voter lagging, and a witness replaced
  while a data voter lags.
- The 10 GiB install at 10 ms of added round trip under 10 MB/s of writes with 1,000
  groups; it needs C2 and C4.

## T8 Robustness

`consensus` `fuzz/fuzz_decoders.cc` (`dd82c06`, `056fcb7`) covers the control codec,
memberships, founding and join records, storage batches, checkpoint bundles, and
snapshot descriptors. Its checked-in corpus replays as a test. Its first minutes found a
non-canonical `RFTINIT2`, which recovery now refuses. A six-CPU-hour run is in progress.
