# durability-modes

- Design: docs/design/durability-modes.md (draft); reviews: none yet.
- Branch / worktree: `design/durability-modes` at `~/code/Kronuz/strata-wt/durability`, from
  origin/main `755a918`.
- State: designing (paused at the usage limit, 2026-10-18; not ready for preliminary review).
- Done criteria (set by the lead; contributors tick, never edit): none set yet.
- Done:
  - Read REPLICATION, DISTRIBUTION, PLAN (Phase 3R, Track C, process rules), ARCHITECTURE,
    track-c.md, the system review (SYS-4, SYS-8, SYS-10, SYS-11), placement-strategies,
    workload-profiles, rocksdb-survey (K2, Q3, Q7), small-indexes D1 to D3 at `25deedc`.
  - Verified the Xapiand claims against MIT own code (no GPL read) and recorded corrections:
    effective debouncer defaults come from `opts.cc:114-136`; `DB_SYNCHRONOUS_WAL` and
    `?commit` are not power-loss durable; ELECT_PRIMARY is `discovery.cc:653-801`.
  - Derived today's 10 syncs per tiny index from the code; outlined D1 to D11, the
    small-indexes D3 relation, T1 to T10, Q1 to Q4.
- Doing: nothing (paused).
- Next, in order:
  1. Read `design/peer-transport` and `design/simulator` designs; place D5's and D7's `DTC2`
     fields in the DTC2 table before the coordinated commit (Track C D2).
  2. Check consensus `core.h:581-591` (Repair) against D8's rewind-on-promotion, and the
     track-c review's conditions on D5 and D11.
  3. T1: count syncs with an instrumented IO (no hold). T2: the sizing measurement (light hold
     after `quiet check`; corpus sample from the gate VM or a stated synthetic one), only when
     the lead allows it again.
  4. Fetch and cite the prior art; write each decision in full with its proof sketch, the
     fault model per preset with exact windows, rerunnable procedures for T1 to T10, and the
     first implementable steps with their branches (journal D3 format, consensus D5 to D7,
     strata::group D8, small-indexes D3-A as the checkpoint batcher).
  5. Set State to "designing (ready for preliminary review)".
- Blocked on: the usage limit (the coordinator's pause). SYS-8 (turn log or per-group journal)
  decides which backend D3's format change lands in; the design must state both.
- Findings folded in: none yet.
- Last verified: no build, test, or measurement run (paused before any).
- Scope added by the owner (2026-10-18), for the next revision:
  - "Choose your side": every system on strata (strato, shale and the rest) lets the user pick
    availability or consistency per index, with per-request overrides, on ONE replication
    mechanism (consensus per shard). Precedent: Azure Cosmos DB's five levels on one protocol
    (strong, bounded staleness, session, consistent prefix, eventual). Map the presets and the
    read modes onto such a spectrum, with each level's exact guarantee and loss window.
  - A later preset, designed now so the log and apply contract leave room for it:
    partition-tolerant writes. Replicas on the minority side accept writes into a side log and
    merge on healing by a deterministic rule (last write wins on a hybrid logical clock, or
    conflict-free types for counters and sets). Opt-in per index, its conflict window stated.
    Say what the determinism contract and the digest need for it, and what it costs if never used.
  - Leaderless replication (Dynamo/ScyllaDB style) as a second mechanism is rejected; record
    why (two repair paths, two failure models, two test matrices).
  - The failover gap: bound it (election timeout, lease handover) and state it per preset.
