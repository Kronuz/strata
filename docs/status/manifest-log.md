> Status: CURRENT (2026-11-04) the manifest-log design's ledger; supersedes nothing.

# manifest-log

- Design: `docs/design/manifest-log.md`, revision 1 (draft, ready for the architect's re-check);
  reviews: `docs/reviews/manifest-log-design.md` (preliminary, 2026-11-04, at `strata:057c47e`:
  REJECT for first step 3 until L1 to L3 fold in; steps 1 and 2 may start, step 1 with L1's rule).
  The security reviewer's decoder-safety review covers the log reader before first step 3 lands.
- Branch / worktree: `design/manifest-log` at `~/code/Kronuz/strata-wt/manifest-log`, from `main`
  `2769928` (`main`'s head `9e344d8` changes no code); revision 1 at `strata:1c8ff50`.
- State: designing (ready for the architect's re-check)
- Done criteria (set by the lead; contributors tick, never edit):
  - none yet
- Done:
  - Revision 0: D1 to D11, T1 to T9, Q1 to Q3, first steps 1 to 5 (`strata:3ab5b78`); the floor
    probe `bench/manifest-log/mlog_probe.c` and `results/2026-11-04-floors.txt`.
  - Revision 1 (`strata:1c8ff50`): L1 (D6's per-fragment tail rule, D4's one write
    per append and truncate-back), L2 (D3's edit between two named states, the change list with
    marks), L3 (D8's reads of older logs with `main`'s blocking rule), ML-R4 to ML-R14 folded; D7's
    trigger against a snapshot of the current state in bytes and edits (F 16 KiB, R 128); T3 to T9
    restated; Q1 and Q3 recommendations recorded as the architect agreed, Q2 open; the floors
    replaced by the architect's rerun (`results/2026-11-04-floors-rerun.txt`, copied from its
    scratch). Probes: `bench/manifest-log/tail_model.py`, `t8_model.py`, `open_probe.cc`,
    `run_r1.sh`; output `results/2026-11-04-r1.txt`.
- Doing: nothing; stopped at ready for the architect's re-check, as the prompt asks.
- Next:
  1. The architect's re-check of revision 1 (L1 to L3, and D7's new bounds, which change a
     mechanism the preliminary review saw as 2 S of the log's own snapshot).
  2. The lead: T8 is restated in the log's terms (the bytes the log writes for the table) with the
     4× bound kept, as ML-R6 asked; accept or amend.
  3. The owner: Q2 (what `retained_predecessors` protects; it narrows a user-visible option). Q1
     (the manifest log first) and Q3 (accept the lost-write residual now, close it with K2's
     checkpoint record) carry the architect's agreement for the lead to put to the owner.
  4. First steps 1 and 2 may start now (journal's log component with L1's rule, coordinated with
     Detent's lead; strata's log codec); step 3 waits for the re-check.
- Blocked on: the architect's re-check (the lead launches it); the owner on Q1 to Q3.
- Findings folded in: L1 (ML-R1), L2 (ML-R2), L3 (ML-R3), ML-R4 to ML-R14 (`strata:1c8ff50`).
- Design vs code: nothing built.
- Last verified: `bench/manifest-log/run_r1.sh` under `quiet strict mlog2` (exclusive on macOS),
  2026-11-04 02:27 to 02:28, exit 0: `tail_model.py` checks 0 to 6 (journal's fixtures verify;
  1,020 honest crash images, 0 misread; 219,672 single-bit flips, 0 silent under D6, 18 under
  revision 0's wording; two faults 0 silent; two in flight 33 of 76 damage; short writes 21,846 of
  21,962 sizes fail closed without the truncate-back); `t8_model.py` (2.46× and 3.38×);
  `open_probe.cc` 3 rounds of 3,000 (`main` 79.6 µs; fresh log 0.43×; 128 edits 0.74× and 0.55×).
  Every new `file:line` re-read at `main` `2769928`, journal `bfad97f`, completion `e2c6474`,
  RocksDB 11.11.

## Log

- 2026-11-04: worktree created from `main` `2769928`; reading started; probe written.
- 2026-11-04: probe run under a strict hold (3 rounds; `main`'s commit file work 1,041 µs of CPU
  against the log's 493 µs, an edit alone 137 µs, a rollover 445 µs, a turn-log-like append with
  its barrier 135 µs); a decode-cost proxy probe (`.scratch/mlog/decode_probe.cc`) was written and
  not run, since the host's light slots were full; the design states no replay time and leaves the
  open's replay cost to T7.
- 2026-11-04: the design drafted and set to ready for preliminary review (`strata:3ab5b78`).
- 2026-11-04: the architect's preliminary review landed (`strata:057c47e`): REJECT for step 3
  until L1 to L3; steps 1 and 2 may start; its rerun of the floor probe read 8 to 58% above the
  first run.
- 2026-11-04: revision 1 drafted: `tail_model.py` (from the review's framing model) checks the
  per-fragment tail rule; `t8_model.py` shows T8 needs both the presence mask and the
  current-state bound (either alone 4.19× to 6.03×); `open_probe.cc` measures a small index's open
  against `main`'s whole file work, `main`'s scan of every file included, which the review's
  1.3 to 1.9× estimate left out; replay tracks bytes (about 3 ns per byte) more than records, so R
  caps cheap-to-decode state rather than small indexes. Run under `quiet strict mlog2` twice
  (01:36, and 02:27 after adding the 128-edit and delete-only rows); the second run's output is the
  committed one. Session cut off once by the API spend limit and resumed from `git status`.
  Committed as `strata:1c8ff50`; State set to ready for the architect's re-check.
