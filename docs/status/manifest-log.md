> Status: CURRENT (2026-11-04) the manifest-log design's ledger; supersedes nothing.

# manifest-log

- Design: `docs/design/manifest-log.md` (draft, ready for preliminary review); reviews: none yet.
  The architect reviews it at max before anything is built (durability format); the security
  reviewer's decoder-safety review covers the log reader before first step 3 lands.
- Branch / worktree: `design/manifest-log` at `~/code/Kronuz/strata-wt/manifest-log`, from
  `main` `2769928`.
- State: designing (ready for preliminary review)
- Done criteria (set by the lead; contributors tick, never edit):
  - none yet
- Done:
  - The design: D1 to D11, gate rows T1 to T9, Q1 to Q3, first steps 1 to 5 (this commit's
    parent chain; see the log).
  - The file-work floor probe and its output: `bench/manifest-log/mlog_probe.c`,
    `bench/manifest-log/results/2026-11-04-floors.txt`.
- Doing: nothing; stopped at ready for preliminary review, as the prompt asks.
- Next:
  1. The architect's preliminary review; fold its findings, citing their IDs.
  2. The owner's answers to Q1 (order: the manifest log first, then K2), Q2 (what
     `retained_predecessors` protects), Q3 (the lost-write residual).
  3. First steps 1 and 2 may start once the review allows (journal's framing component, coordinated
     with Detent's lead; strata's log codec); step 3 is the landing unit.
- Blocked on: the preliminary review (the lead launches it), and the owner on Q1 to Q3. Q1 decides
  whether K2 (the standalone turn log) joins this milestone; Q2 narrows a user-visible option, so it
  is the owner's.
- Findings folded in: none yet.
- Design vs code: nothing built.
- Last verified: `bench/manifest-log/mlog_probe.c`, 3 rounds under `quiet strict mlog`, 2026-11-04,
  exit 0 (medians in the design's "Measured floors"). Every `file:line` in the design re-read at
  `main` `2769928`, journal `bfad97f`, completion `e2c6474`, RocksDB 11.11.

## Log

- 2026-11-04: worktree created from `main` `2769928`; reading started; probe written.
- 2026-11-04: probe run under a strict hold (3 rounds; `main`'s commit file work 1,041 µs of CPU
  against the log's 493 µs, an edit alone 137 µs, a rollover 445 µs, a turn-log-like append with
  its barrier 135 µs); a decode-cost proxy probe (`.scratch/mlog/decode_probe.cc`) was written and
  not run, since the host's light slots were full; the design states no replay time and leaves the
  open's replay cost to T7.
- 2026-11-04: the design drafted and set to ready for preliminary review.
