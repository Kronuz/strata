# Durability and replication modes, part A: the consistency spectrum and replication

- Status: **designing (revision 7, ready for the design-only re-check)**, 2026-10-19. Part A of two. This file
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
- Read at: strata `main` `3c86484` (merged here at `2f444fc`; every strata `file:line` below is at
  it, re-pinned from `026ee10` by mapping each cited line through the diff,
  `bench/durability/repin_citations.py`, and the seven that moved in substance re-read: DM-R39); consensus `61149a2`; journal
  `cb66f3d` (consensus's pin; journal `main` `dbf753f` changes only `admission.h` and
  `resources.h`); completion `7734544` (strata's pin at `3c86484`; `posix.h` below at it);
  Xapiand `master` `7e8cccb7d`, its MIT own code only (nothing under `src/xapian`, no
  `sortable_serialise`); the storage library `671eaac`; `research/no-stalls` `467b5b3`;
  `perf/small-indexes` `2e9949e` and `45985dc` (SI-R24's claim rule); threadpool `628679a` (strata's
  pin); Kronuz/hashes `7ab0c23` (strata's pin) and Kronuz/random `6487253`; Kronuz/cpp-libraries
  `1104f00`; Detent `f5a8a7a`.
- Reviews folded in: the architect's preliminary review (`docs/reviews/durability-modes-design.md`:
  REJECT for first steps 1, 3, 4, and 5; DM-R1 to DM-R18, C1 to C5) and the security review
  (`docs/reviews/durability-modes-security.md`: APPROVE WITH CONDITIONS; DMS-1 to DMS-9, C1 to C7);
  the owner's answers of 2026-10-19 (Q1 to Q6 as recommended, Q3 with DM-R1's fix, Q8); and
  small-indexes' re-check (`docs/reviews/small-indexes-design.md` on `perf/small-indexes` at
  `2e9949e`, its C3: SI-R19, SI-R20, SI-R24, SI-R25, SI-R26); then both re-checks of revision 1: the
  architect's (`docs/reviews/durability-modes-design.md`, "Re-check (revision 1)": REJECT for first
  steps 1 and 3; DM-R19 to DM-R23, C6 to C14) and the security reviewer's
  (`docs/reviews/durability-modes-security.md`, "Re-check (revision 1)": APPROVE WITH CONDITIONS;
  DMS-10 to DMS-13, C8 to C12), with their probes (`strata-wt/review-durability/.scratch/
  review-durability/r1/`, `strata-wt/review-durability-sec/.scratch/durability-sec-r1/`) rerun on
  this branch's model; then the architect's re-check of revision 2
  (`docs/reviews/durability-modes-design.md`, "Re-check (revision 2)": REJECT for first step 1,
  APPROVE WITH CONDITIONS for step 3; DM-R24 to DM-R28, C15 to C22), with its probes
  (`strata-wt/review-durability/.scratch/review-durability/r2/`) read and its journal probe ported
  onto this branch; then its re-check of revision 3 (`docs/reviews/durability-modes-design.md`,
  "Re-check (revision 3)": APPROVE WITH CONDITIONS for first steps 1 and 3; DM-R29 to DM-R32, C23 to
  C26), with its probes (`strata-wt/review-durability/.scratch/review-durability/r3/`) read, its
  multi-group journal model ported onto this branch (`mark_groups.py`), and its five-voter schedules
  added to both checkers; then its re-check of revision 4 (`docs/reviews/durability-modes-design.md`,
  "Re-check (revision 4)": APPROVE WITH CONDITIONS for first steps 1, 3, and 8, APPROVE for 9;
  DM-R33 to DM-R35, C27 to C29) and the security reviewer's review of D18
  (`docs/reviews/durability-modes-security.md`, "D18 review (revision 4)": APPROVE WITH CONDITIONS;
  DMS-15 to DMS-19, security C13 to C17), with their probes read
  (`strata-wt/review-durability/.scratch/review-durability/r4/`,
  `strata-wt/review-durability-sec/.scratch/durability-sec-r4/`) and their schedules modeled on this
  branch (`check3_model.py`, `vouch_model.py`, `term_value_check.cc`); then its re-check of revision 5
  (`docs/reviews/durability-modes-design.md`, "Re-check (revision 5)": APPROVE WITH CONDITIONS;
  DM-R36 to DM-R38, C30 to C32), with its probes (`strata-wt/review-durability/.scratch/
  review-durability/r5/`: `plant/check35_real.out`, `plant/check35_real.cc`) read, not rerun; then
  the full design reviews of revision 6, the architect's (`docs/reviews/durability-modes-design.md`,
  "Full design review (revision 6)": APPROVE WITH CONDITIONS, step 5's whole-image verifier, mode
  switch, and `verify` switch rejected; DM-R39 to DM-R47, C33 to C40) and the security reviewer's
  (`docs/reviews/durability-modes-security.md`, "Full design review (revision 6)": APPROVE WITH
  CONDITIONS; DMS-20 to DMS-24, security C18 to C22), with their models and probes read and run on
  this branch, unedited (`strata-wt/review-durability/.scratch/review-durability/r6/`:
  `model/switch_model.py`, `model/vouch_model_r6.py`, `plant/alphabet_whole.out`;
  `strata-wt/review-durability-sec/.scratch/durability-sec-r6/`: `switch_model.out`,
  `alphabet_rules.out`). What changed, per finding: "Changes in revision 7", then 6, 5, 4, 3, 2,
  and 1.
- Measured here, as branch evidence for choices, never as gate rows (PLAN, "What waits"): T1's sync
  counts and T2's sizing on the development Mac (`0b407c9`, unchanged). Counted here: the digest
  records of T2's corpus (`bench/durability/records.py`, `results/r4/records.out`). Verified here by
  computation: D5 and D6 in an explicit-state model (`bench/durability/model/`, results in
  `bench/durability/results/model-*.out`, `results/r2/`, and, for revision 3's vote barrier and
  windows, `results/r3/`; for revision 4's five-voter schedules, `results/r4/`), and D3's bytes in
  journal-level models (`mark_model.py`, `results/r3/mark/`; with several groups, `mark_groups.py`,
  `results/r4/`), and D18's check 3 and `Vouch` rule in small exhaustive models and check 5's term
  values in a probe against `format.hh` (`results/r5/`), and revision 7's grounding rules and the
  whole comparison's documents in the vouch and switch models (`results/r7/`). Every other figure
  is the cited code's or
  document's, arithmetic
  (`bench/durability/arith.py`, `results/arith.out`), or a labeled estimate.

## Revision 7

1. **A member is grounded only from content an independent check covered (DM-R40).** Revision 6
   grounded "one it built itself (... D8's catch-up)" and "one it compared whole against its own"
   without asking what the follower's own image rested on: a new member that installed a planted
   image from the leader alone, then caught up and reinstalled the leader's next image whole, found
   them equal, plant and all, and was grounded, vouched, and served (the architect's
   `vouch_model_r6.py`, 5 steps). Now every grounding traces to the log through checked content: an
   image built on an unvouched one, by catch-up or by document mode, stays unvouched, and a whole
   comparison grounds only a follower whose own image was grounded. The widened model, every member
   able to catch up and each image carrying the plants it holds, finds no `yes` for a planted entry
   and no grounded member holding a plant (354,323 states with two entries, every reachable
   one; 1,025,427 with three to depth 11, where the 1.5-million-state cap stops depth 12; the
   architect's model holds to depth 12 with three), and revision 6's rule loses in the
   architect's 5 steps.
2. **A switch to `verify: install` grounds at the switch, its residual counted (DM-R41).** Under
   DM-R40's rule a group whose images were installed under sampling would never ground a member
   again (no reachable state has one), so every new member and repair would install from the
   leader alone for good. Each follower's image now counts as grounded at the switch
   (`grounded_by_switch`): what the samples missed before it stays, as option 2 left it, and no
   member vouches for an entry at or before the switch. Q9 offers the alternatives, (b) keeping
   them unvouched and (c) re-indexing, and recommends (a).
3. **The mode switch, read strictly (DM-R42, DMS-24 item 2).** Revision 6's "the segments it then
   holds" and "its checkpoint ceiling stays at the command" each admit a reading that refuses an
   honest switch. Now the frozen set is the segments a follower holds when it applies the command
   and the output of a merge then in flight; the ceiling is its last refresh before the command;
   the leader expunges every segment holding a document deleted or replaced before the command, a
   straddling merge's output included, whichever leader writes the first `Image`; and D18 states
   how a follower applied past the `Image`'s position computes its image there, and its state after
   the install. The ported `switch_model.py` finds every needed document present and the
   computation exact under the strict readings, and every looser reading and mutant losing.
4. **The security reviewer's full design review (DMS-20 to DMS-24).** One alphabet rule for every
   term dictionary, the received one containing the walked bytes (DMS-20); check 3's and the whole
   comparison's tables bounded by what the follower already holds before they are allocated
   (DMS-21); a pinned checkpoint ceiling released at the group's share by leaving segment mode,
   which runs instead of Track C's forced checkpoint for a pinned group (DMS-22); a floor on mode
   switches, at the leader and at each follower (DMS-23); and the restart flags under the segment's
   alphabet (DMS-24 item 1).
5. **strata `main` `3c86484` (DM-R39).** Every strata citation re-pinned (56 moved, 7 by hand); a
   create costs 4 durability calls at `main`, 8 with its first commit; check 5 now names `main`'s
   skip data (advance entries per group, a codec per block) and the threshold seeds `main` added,
   which a planted seed above the honest one would make pruning skip a document a scan sees.
6. **The rest of the full design review (DM-R43 to DM-R47).** Retention keeps the recorded
   generation until a newer record is synced, `retained_predecessors` named and not relied on
   (DM-R43); the term history bounded at 64 entries with its mechanism, this design's memory per
   idle group totaled at about 80 bytes, every gauge a count per node (DM-R44); T2 gates the option
   the owner picks, and the switch's expunge is in D14 and bounded in T13 (DM-R45); an unknown boot
   identity and a journal directory not known local warn and report (DM-R46); and the stale text
   (DM-R47).

## Changes in revision 7

| Finding | What changed | Where |
|---|---|---|
| DM-R40 (C33; blocking) | Grounded is five rules, each tracing to an independent check of the content (built from the creation or on a grounded image, incremental on a grounded image, a whole comparison while the follower's own image was grounded, a fetch from a grounded member that is not the leader, a `yes`); an image built on an unvouched one stays unvouched; a catch-up reinstall on an unvouched image installs the leader's and stays unvouched; a detector is the independent copy only on a grounded image; `vouch_model.py` widened (every member may catch up, plants carried) with revision 6's rule as `--catchup any`; T4's 5-step schedule with revision 6's list as the mutant | D8, D17, D18, threat model, fault model, summary, Q9, T4, `vouch_model.py` |
| DM-R41 (C33, C36; blocking) | At a switch to `verify: install` each follower's image counts as grounded, `grounded_by_switch` counted, its `Vouch` interval after the switch's entry; the residual stated; Q9's sub-question with (a), (b), (c), recommending (a); options 3 and 4 state it; `vouch_model.py --switch`; T4's missed-plant switch with two mutants | D15, D18, Q9, threat model, fault model, summary, T4 |
| DM-R42 (C33; blocking) | The frozen set (held at the command, and a merge then in flight) and the ceiling (the last refresh before the command); the computation at p and the state after a whole install ((p, a] applied again before the install publishes); items (a) to (d): whichever leader writes the first `Image` owes the expunge, replacements included, one merge per segment holding a pre-command delete or replace, the follower may merge its post-command flushes; `switch_model.py` ported with the computation checked and the two readings as mutants; T4's cases | D8, D15, D18, T4, `switch_model.py` |
| DMS-20 (security C18) | One alphabet rule for every term dictionary: the received alphabet, ascending by termdict's own parse, containing every walked byte, re-encoded byte for byte; the stored-alphabet rules and C14's "from the walked keys" clause withdrawn; T4's widened alphabet passes in incremental installs too | D18, T4 |
| DMS-21 (security C19) | Check 3's table bounded by the inputs' ordinal counts, and the whole comparison's by the follower's own ordinals and its log's documents, before allocation; the walk fails past the claimed count; T4's cases | D18, T4 |
| DMS-22 (security C20) | A follower whose ceiling stays pinned past its group's share leaves segment mode for the group (`segment_mode_left{reason}`), which runs instead of Track C's forced checkpoint; its return through the switch's follower side, deferring what its release dropped (`switch_deferred`); D14's verification row; T4's never-written `Image`; T13 and T14 report the retained log | D8, D14, D18, threat model, `track-c.md` row, T4, T13, T14 |
| DMS-23 (security C21) | `switch_min_interval` (10 minutes): the leader refuses a switch within it, and each follower makes at most one whole comparison per interval by its own clock; `mode_switches` per node; `limits.md` row | D15, threat model, `limits.md` row, T4, T13 |
| DMS-24 (security C22) | The `absolute` and restart flags are the re-encoding builder's under the segment's alphabet (item 1); the expunge covers a merge that straddled the command (item 2), with `switch_model.py --expunge-held-only` as its mutant | D18, D8, T4 |
| DM-R39 | strata `main` `3c86484` merged at `2f444fc`; every strata citation re-pinned (`bench/durability/repin_citations.py`: 29 unchanged, 56 moved, 7 by hand); T1's table and target from `main`'s create (4 calls, 8 with the first commit, `test/directory_test.cc:650-667`); D1's create and `clear_uncreated` citations; completion `7734544`; check 5's skip data in `main`'s layout and its threshold seeds; step 7 measures `main` | Header, summary, "What exists today", D1, D18, T1, first steps |
| DM-R43 (C34) | Retention keeps the recorded generation until a newer checkpoint record's barrier returns; `retained_predecessors` (`index.hh:282`) and the pins named as today's defaults, not relied on; T16's crash in the window with the keep removed and `retained_predecessors` 0 as the mutant | D1, T16 |
| DM-R44 (C35) | The term history keeps the newest `term_history_entries` (64) entries, the horizon a token below which answers `expired`, on disk and read only below the log's base; this design's memory per idle group (about 80 bytes, an estimate) beside small-indexes' 1,964-byte idle index; every new gauge a count per node; T11's expired token; T14 at 10,000 groups per node; `limits.md` row | D10, D15, D16, `limits.md` row, T11, T14 |
| DM-R45 (C36) | T2's gated row follows Q9's answer (option 3's targets 0.70×, 0.62×, 0.40×, and below option 1 at rf = 3; option 2's as before), both reported; the summary gives both options' costs; D14's expunge row; T13 bounds the switch's foreground p99 and reports its duration, bytes, and retained log | Summary, D14, T2, T13, Q9 |
| DM-R46 (C37) | Completion's boot identity returns "unknown" apart from every value; `TurnRecovery` reports why a verdict is unclean; the host reports an unknown identity as state and warns at each open; the journal directory takes the filesystem classification and warns; `getentropy`'s failure is state (`verifier_unavailable`); T9's cases | D3, D18, threat model, T9, first steps 1 and 2 |
| DM-R47 items 1 to 10 | (1) Q9's heading and recommendation restated on revision 7's checks, and the revision-5 re-check's outcome stated as past; (2) DM-R36's choice as ruled (DMS-20) in D18, T4, and here; (3) `results/r4/records.out` cited throughout; (4) the site table's column "This design", and "This design never holds"; (5) step 5's row rewritten; (6) T10 counts barriers instead of timing; (7) T4's segment-mode cases in strata::group with a mutant leader; (8) a source refuses a `Request` for an entry whose files it no longer holds, counted, and a restored follower takes catch-up; (9) a segment-mode follower's replay from the creation, with its cost; (10) the summary and Q9 state DM-R40's and DM-R41's rules | Q9, D18, T4, header, D7's site table, D8, first steps, T10, D17, D1, summary |
| Architect C38, C39 | T6's mutant for an install while forgetful that drops the mark (C38); the done criteria are the lead's (C39), listed in the ledger's next steps | T6, ledger |
| Architect C40 | The lead's calibration rows; no review file is edited here | None |

## Revision 6

Text only: no model, probe, or measurement was run for this revision (the re-check asked for text and
T4 cases, and neither fix changes a rule's argument beyond what `check35_real.out` part A already
shows on strata's own segments).

1. **Check 5's alphabet on a whole image is the received one, when it contains the walked bytes
   (DM-R36).** Revision 5 took a merge output's term alphabet from its inputs' stored alphabets
   (`merge.hh:92-99`), which a follower that installs incrementally holds, and said nothing for a
   whole image, whose segments' inputs are merged away or were never held: the only rule left, the
   walked terms' bytes, refuses an honest merge whose dropped documents took a byte (M1 and M2 of
   `check35_real.out`, part A), so a new member would never install and a reinstall would fall to
   document mode. Now, for a whole image (a new member, a repair, a catch-up or mode-switch
   reinstall), check 5 accepts the received term alphabet when it is strictly ascending and
   contains every walked term's byte, and the re-encoding under it must still equal the received
   bytes; everywhere else revision 5's rule stands (a flush's walked terms; the union of the inputs'
   stored alphabets, each verified at its own install, for a merge). An alphabet only sets codes, and
   a superset reads the same (`term_index_check5.out`), so the choice cannot hide or add a term.
   D18's wording of the union now says "stored alphabets" (DM:2009 in revision 5). It gives up, for
   whole images, security C14's "a function of the walk alone"; the alternative (one rule everywhere:
   the received alphabet containing the walked bytes) is also sound and simpler, and the security
   reviewer confirms this choice at the full design review (the ledger records it). It ruled for
   one rule everywhere (DMS-20), which revision 7 writes.
2. **A mode switch matches every dead ordinal (DM-R37).** Revision 5 let a mode switch's unmatched
   dead ordinals through "counted", but their records are the leader's, unchecked and unbounded per
   document, and the reinstall then grounded the image, so its member vouched for them. Now, from
   the switch command's log position the follower merges none of the segments it then holds (as
   catch-up already does) and keeps its log from the command, and the leader's first `Image` after
   the command carries no dead ordinal of a document deleted before it: the leader first expunges
   those deletes, one merge per segment holding a dead ordinal, at a switch D15 already marks
   heavy. Every dead ordinal of that `Image` then names a document live at the command or written
   after it, which the follower's unmerged segments or its log hold, and an unmatched one fails
   the comparison like any other mismatch. The residual and `reinstall_dead_unmatched` are gone from
   D18, Q9, and T4; T4 gains the mode-switch cases.
3. **Precision (DM-R38).** D8's catch-up bullet says the checkpoint ceiling bars checkpointing the
   follower's own catch-up segments, so its log keeps every write since its last installed image
   (item 1); check 5's "bytes the walk's decoders consumed" now says each decoder reports where it
   ended, with step 5's accessors and a T4 plant in a segment (item 2); and check 3's residual
   "moves IDF and average length for the terms of the deleted documents it keeps" (item 3).

## Changes in revision 6

| Finding | What changed | Where |
|---|---|---|
| DM-R36 (C30; security C14) | Check 5 on a whole image accepts the received term alphabet when it is strictly ascending and contains every walked term's byte, re-encoding byte for byte under it; elsewhere revision 5's rule (flush: walked terms; merge: the union of the inputs' stored alphabets, each verified at its own install); the stored-alphabets wording; why a superset is safe; the security reviewer confirms the choice at the full design review; T4's whole-image honest control and the widened, narrowed, and unordered alphabets | D18, T4, first steps |
| DM-R37 (C30) | From the switch command the follower merges none of the segments it then holds and keeps its log from the command; the leader's first `Image` after it carries no dead ordinal of a document deleted before the command (it expunges them first, one merge per segment holding a dead ordinal); every dead ordinal matches, an unmatched one fails like any mismatch; "counted, residual stated" and `reinstall_dead_unmatched` removed from D18 and Q9, and the T4 text that counted them; the summary, the threat row, and the fault table state the switch as matched; Q9's recommendation states it; T4's mode-switch cases (honest with deletes before and after the command, a phantom ordinal, a pre-command delete, a follower that merges its held segments, a leader that skips the expunge) | D8, D15, D18, summary, threat model, fault model, Q9, T4, first steps |
| DM-R38 item 1 | The catch-up bullet: the checkpoint ceiling bars checkpointing the follower's own catch-up segments, so its log keeps every write since its last installed image; T4's mutant without the ceiling | D8, D18, T4 |
| DM-R38 item 2 | Each decoder reports where it ended (`postings_iterator`, `positions_reader`; step 5 adds the accessors), and T4's "length longer than the walk consumed" plant runs in a segment | D18, T4, first steps |
| DM-R38 item 3 | "For the terms of the deleted documents it keeps" | D18 |
| Architect C30 | Step 5's verifier lands with the whole-image alphabet rule and the mode-switch rule, T4's whole-image honest control and the planted mode-switch mutant; Q9's recommendation states the switch as revision 6 rules it | First steps, Q9, T4 |
| Architect C31, C32 | The lead's: the done criteria (the ledger) and the calibration row; no review file is edited here | None |

## Revision 5

1. **Check 3 covers every ordinal of a merge output (DM-R33).** A merge carries the documents
   deleted while it ran as dead ordinals, and every ranking statistic counts them, while revision
   4's check 3 compared live records only: three planted dead ordinals kept F21's digest and every
   keyed sum equal and moved the best match for "alpha OR beta" out of the top three (the
   architect's `dead_stats_plant.cc`). Every output ordinal now names an input document by docid
   and version, at most once, dead exactly when that document is dead at the `Image`'s position,
   and the keyed sum covers every ordinal. Over every output of up to four ordinals from two input
   documents and three from three, the rule passes no phantom or altered record and no raised
   statistic, and refuses no honest output (`check3_model.py`). What stays the leader's: which
   deleted input documents a merge keeps, as an honest merge's timing chooses, which moves ranking
   statistics, never above the inputs', and hides no document from a match. A whole reinstall
   compares the leader's dead ordinals too.
2. **`Vouch` answers per entry, from a grounded image (DM-R34, DMS-15, DMS-17).** Revision 4 answered
   by position, so a follower that skipped an entry through catch-up vouched for it, and a member
   whose own image was unvouched vouched for others, two such members for each other. A member now
   answers `yes` only for an entry it checked itself from a grounded image, `not yet` while its own
   image is unvouched, and `skipped` for an entry below what it checked; the answer echoes the
   entry; a `no` never routes the re-fetch to its sender; an unvouched member serves no file of the
   group; and an unserved `Request` asks another member. `vouch_model.py` finds no `yes` for a
   planted entry under the rule, and the planted vouch in 4 to 6 steps under each of its three
   controls.
3. **Check 5 rebuilds each term's value from the walk (DMS-16, DM-R35 item 3).** A planted value
   decodes and re-encodes to itself, so its whole-list competitive pairs, which MaxScore takes as the
   term's bound, and its `cf` passed. The verifier now builds each value as the writer does from what
   the walk decoded, names every field and both skip levels, and takes the dictionaries' alphabet as
   the writer derives it (`term_value_check.cc`: every planted field refused).
4. **Hygiene and honest failures (DMS-18, DMS-19).** The keys live in a locked page excluded from
   dumps, a `getentropy` failure fails closed, no sum is logged; failures count per check, and T4's
   honest control covers every part shape.
5. **Precision (DM-R35 items 1 and 2) and the boundary.** D3's "why no slice" stands on the
   accumulation bound, since slices of marked groups only can free a segment sooner; revision 4's
   summary of revision 0's rule is corrected; and Q9's residual says in one clause that every option
   verifies segments against the log, which a compromised leader writes.

## Changes in revision 5

| Finding | What changed | Where |
|---|---|---|
| DM-R33 (C28) | Check 1 sums every ordinal of a flush, its bitmap ignored; check 2's bits for a merge output follow the input document each ordinal names; check 3 covers every ordinal of a merge output: each names an input document by docid and version, at most once, dead exactly when that document is dead at the `Image`'s position, and the keyed sum covers every ordinal's records (8 bytes per carried document while it runs); the residual (which deleted inputs a merge keeps; never above the inputs' statistics; `merge_dead_carried`) stated in D18, Q9, the threat row, the fault table, and the summary; a catch-up or mode-switch reinstall compares the leader's dead ordinals with the follower's image or its log, catch-up merges only the follower's own flushes, and a mode switch counts the unmatched (`reinstall_dead_unmatched`); option 2's sampled merges use check 3; T4's dead-ordinal and carried-document mutants; `check3_model.py` | D8, D18, threat model, fault model, summary, Q9, T2, T4 |
| DM-R34, DMS-15 (C28; security C13) | Grounded and unvouched images defined; `Vouch` answers `yes` only for an entry the member checked itself from a grounded image (one interval per group, 16 bytes), `not yet` while its own image is unvouched, `skipped` below its interval, `no` on a failed check or another term; a `yes` for a later entry clears the `unvouched` gauge but not the earlier entry's count; T4's `vouch_position.py` schedule and the two-member chain, each with its mutant; `vouch_model.py` with the rule and three controls | D16, D17, D18, threat model, fault model, Q9, T4 |
| DMS-17 (security C15) | The answer echoes the entry (185 bytes) and an answer with another entry drops, counted; after a `no` the receiver re-fetches from a grounded member that is neither the leader nor the sender, and counts `no`s per sender; an unvouched member refuses every `Request` of its group; a `Request` unserved for one `recovery_timeout` goes to another member (`request_unserved`); step 8 carries them | D16, D17, D18, threat model, first steps |
| DMS-16, DM-R35 item 3 (security C14) | Check 5 rebuilds each term's dictionary value from the walk (df, cf, the flags, a single posting, the regions' offsets and lengths, the whole-list pairs by `competitive()`), encodes it, and compares bytes; both skip levels' fields named and compared with the walk; the dictionaries' alphabet is the writer's: the walked keys', a flush's walked terms', and for a merge output's terms the inputs' union (`merge.hh:92-99`), where DMS-16's "from the walked keys" would refuse an honest merge; T4's planted pairs, `cf`, flags, and skip fields; `term_value_check.cc` | D18, T2, T4 |
| DMS-18 (security C16) | The two keys in one `mlock`ed page with `MADV_DONTDUMP` (Linux) or `MADV_NOCORE` (FreeBSD), zeroed at exit; a `getentropy` failure leaves the follower in document mode, logged at error level; an `mlock` failure is logged and counted; no sum in any log | D18 |
| DMS-19 (security C17) | `verify_failed{check}` per check; a check-1 failure under an honest leader is a strata bug, gated by T4's honest control over every part shape; the client row states the lever | D18, threat model, T4 |
| DM-R35 items 1 and 2 | D3's "why no slice" rests on the accumulation bound (`own` can free a segment early, the architect's `own_reclaim_forced.py`); revision 4's summary and changes row corrected | D3, "Revision 4" |
| Security's Q9 residual | Option 3 verifies segments against the log, which a compromised leader writes: one clause in Q9, the threat row, and D18 ("The boundary") | D18, threat model, Q9 |
| Architect C27, C28 | Step 1 approved; step 5's verifier lands with DM-R33's and DM-R34's fixes and Q9 restated on them; step 8's `Vouch` answering as step 5's | First steps, Q9 |
| Architect C29, C17 | The lead's: the calibration row and the done criteria; no review file is edited here | None |

## Revision 4

1. **No reclamation slice before the last mark turn (DM-R29).** Revision 3 let reclamation's copies
   ride in the epoch's mark turns, and with more than one forgetful group a slice in the first mark
   turn copied a later-marked group's unmarked hard state into the new epoch: a crash or a clean
   close before that group's mark brought it back not forgetful, without a write it acknowledged (the
   architect's `mark_groups.py`, 7 steps, or 6 with a clean close, no OS crash). The turn log now
   takes no forward slice in an epoch until the last mark turn's write returns. Ported onto this
   branch, the multi-group model holds under that rule to depth 9 with two groups (2,716,835
   states; 2,751,883 with segment switches between the mark turns) and to depth 8 with three
   (2,233,453), and its control, slices in any mark turn, loses in 7, 6, and 6 steps. D3 states what
   the order costs at the open: about 99 bytes written per forgetful group, before the core admits
   anything else.
2. **The count since the restart loses at five voters under every barrier (DM-R30).** Revision 3
   called that open after revision 1's schedules stopped replaying under D4's vote barrier. The
   architect's `lead5` puts the third forgetful episode on the leader that wrote the entry, which
   campaigned before it existed, so no barrier covers it: the owner's rule loses a `memory` write in
   13 steps under every barrier mode in both checkers, and so does revision 0's rule as written
   (`timeout`, `lead5`); its every-voter-heard condition alone loses in 14 (`heard5l`), revision 5's
   correction of this line (DM-R35 item 2). D6, Q10, T3, and T6 cite them; D6's rule blocks both.
3. **The install checks cover what queries read (DM-R31).** The keyed sums covered F21's records, what
   a scan reads, while lookups, ranking, and pruning read the dictionaries' indexes, the key filter,
   norms, the statistics, skip data, and competitive pairs. Option 3 now walks every dictionary from
   its first block, never through its index, re-encodes both dictionaries and compares them byte for
   byte, checks the filter, range, norms, statistics, skip entries, and pairs against the walk, and
   checks the image's metadata against the log; a section the verifier has no check for fails the
   install. Whole images: a mode switch and D8's catch-up reinstall compare against the follower's
   own image; a new member or a repair fetches from, or is vouched for by, a member that is not the
   leader, and the residual when only the leader answers is stated and counted. Q9 is restated with
   the checks' cost and where option 3's cost lead over option 1 ends; D18 goes to the security
   reviewer.
4. **Precision (DM-R32).** Sampling decisions take the `Image` entry's term; `records.py` counts
   SipHash's blocks as its source states (5.3 µs per pass at the 2012 rates, not 5.7); `arith.py`'s
   typical manifest entry is 49 bytes (8 MiB admits about 49,600 segments with a live-docs file
   each); the copy rule (c) reads is the oldest; the open's latency and the flush check's memory are
   stated.

## Changes in revision 4

| Finding | What changed | Where |
|---|---|---|
| DM-R29 (C23) | The turn log takes no forward slice in an epoch until the last mark turn's write returns (`hold_forward`, additive; `forward_slice()` returns nothing while it is set), so a mark turn holds marks only; why not slices of marked groups only (revision 4's reason, that its segment cannot leave before every group in it is marked, was wrong; revision 5 states the bound instead, DM-R35 item 1); D4's mark and D16's API say the same; T6's mutant (a slice in a mark turn) and T9's case (two groups whose marks take two turns, a kill and a clean close between them, and marks across a segment switch); `mark_groups.py` ported with `--switch-marking`, and `runs_r4.sh` | D3 rule 2, D4, D16, T6, T9, D6's model evidence |
| DM-R30 (C24) | D6's "Why the count since the restart is not" cites `lead5` and `heard5l` (losses under every barrier mode, both checkers) and drops "open"; Q10 says the ruled rule loses at rf ≥ 5, so options 2 and 3 confine a rule shown to lose; T3 runs `lead5`, `lead5_loss`, and `heard5l` as forced schedules with no loss under D6; T6's `fplus` mutant is killed by `lead5`; `forgetful_model.py`'s comment on the relabeled schedules corrected (it said the owner's rule still loses `fplus5_vb`); the model's limits state C24's rule for retiring a counterexample; the simulator's schedules gain them (C19) | D6, Q10, T3, T6, the model's limits, `forgetful_model.py`, "Changes to other documents" |
| DM-R31 (C25) | D18 lists what queries read beyond F21's records; option 3 walks dictionaries through termdict's cursor from offset 0, never the index; check 4 (the image's metadata against the log) and check 5 (both dictionaries re-encoded and compared byte for byte, the key range and filter, norms, statistics, document frequencies, skip entries, competitive pairs, and no unchecked section); check 2's lookups sound through check 5; whole images: a mode switch and the catch-up reinstall checked against the follower's own image, a new member or repair fetched from or vouched for by a segment-mode follower that is not the leader (`Vouch`, a transfer control kind), the residual counted (`install_unvouched`); option 2 runs check 5 on its samples; option 1's residual names the structures; the threat row, the fault table, and the summary restated; T2 measures the walk and check 5, T4 carries the planted term index, key filter, structure, and metadata mutants and the whole-image cases; Q9 restated with the checks' cost and its crossover; routed to the security reviewer | D18, D16, D17, threat model, fault model, summary, T2, T4, Q9 |
| DM-R32 items 1 to 7 | (1) D3 states the open's latency and head room after an unclean restart, and T8 measures it at 1,000 and 100,000 forgetful groups per core; (2) the copy rule (c) reads is the oldest readable copy's, `TurnRecovery` reports it, and the newest holds too (`--copy-reading newest`); (3) the sampling input includes the `Image` entry's term; (4) `records.py` counts 124 cycles at 8 bytes as two blocks, so 5.27 µs per pass, not 5.73; (5) `arith.py`'s typical manifest entry is 49 bytes: about 76,900 and 49,600 segments; (6) Q9 states where option 3's cost lead ends (4.5 µs per document and pass at rf = 3, one flush's slow end) and T2 measures the walk; (7) the flush check streams its range, at most 16 bytes per document of the range while it runs | D3, D16, D17, D18, T2, T8, Q9, `records.py`, `arith.py`, `mark_model.py`'s comment, `limits.md`'s row |
| Architect C26 | The architect's own calibration row is the lead's to add; no review file is edited here | None |

## Revision 3

1. **Forgetfulness survives every lifecycle event of the bytes that carry it (DM-R24).** A
   recovery that leaves a group forgetful writes the group's hard state with the mark in the
   epoch's first turns, before any other turn, so no copy of an older record can read as newer
   than the verdict; "newest" is by per-group sequence, never by journal order; and the newest
   epoch's open record, which holds V and the boot identity, is a live record of the journal,
   copied forward like a group's, so a process crash in a long epoch reads as the same boot.
   Revision 2's rule lost a `memory` write in 6 steps with no OS crash (the architect's
   `mark_mechanism.py`). A journal-level model with segments and reclamation (`mark_model.py`)
   finds nothing under the new rules to depth 10 (1,458,689 states; 2,218,798 with three segment
   switches) and to depth 11 (5,111,474), and its controls reproduce the loss and the reclaimed open
   record's two failures.
2. **The verifier compares keyed sums (DM-R25).** F21's digest is unkeyed, so a leader gives a
   planted segment the honest digest in O(1). Every comparison D18 makes now sums SipHash-2-4 over
   F21's records, under a key each follower draws from the operating system at start and never
   sends, modulo the prime 2⁶⁴ − 59: the architect's forged segment sums differently
   (`results/r3/keyed_digest.out`). Format 13 needs no change: the sums are computed from content at
   check time and stored nowhere. Option 3 becomes keyed install checks, which keep no per-document
   state, and Q9 returns to the owner with keyed costs.
3. **`manual` decides only with every voter's answer (DM-R26).** The f + 1 admission, and every
   election a `manual` statement lets run, decide only when every voter of the candidate's
   membership answered, never on a timeout, which readmitted `six`. The stall that remains is
   stated as it is: until every voter is reachable and an operator acts, with no time bound.
4. **The model's candidate syncs at campaign (DM-R28).** With D4's vote barrier in both checkers,
   `grow6` and the shrink window's fence are not enabled (the revision-2 model's artifacts), `six`
   stands, and `grow67`, from rf = 5 to 7, shows the growth failure where it is real; the window
   searches are redone under the barrier. Revision 1's five-voter schedules against the owner's
   rule depend on missing barriers too, which Q10 now says.
5. **Precision (DM-R27; C15 to C22).** A lying voter needs only f honest forgetful voters at any
   membership; the model's bound is the least f of the memberships in force; the argument rules out
   a stale leader two changes back; the memory window's cap counts the batch it answers, so
   DMS-13's floor is one window; an install leaves the durable head to the next checkpoint; the
   file list's limit states what it admits; deferral holds only while the queue and the range do;
   sampling decisions are keyed; T16's oracle is the live state; joint consensus is stated; and
   rf = 3's window stall buys uniformity, not safety.

## Changes in revision 3

| Finding | What changed | Where |
|---|---|---|
| DM-R24 | "Newest hard state" is the group's record with the highest per-group sequence that holds one; a recovery that leaves a group forgetful, whose newest hard state lacks the mark, writes its marked hard state in the epoch's first turns, before any other turn, every group decided before them, so V covers only the window before them; the newest epoch's open record is a live record of the journal, its fields copied forward by reclamation's slices (a copy record, kind `0x13`), so its boot identity and V stay readable for the epoch's life; an open record recovery cannot read reads as unclean, never as the same boot; the journal reports each kept record's epoch, the verdict, and V; T6's and T9's rows; `mark_model.py` with segments, reclamation, the open record's lifetime, and a precision check | D3, D4, D5, D16, T6, T7, T9, first steps 1 and 3 |
| DM-R25 | Every comparison D18 makes sums SipHash-2-4 over F21's record encodings under a per-process follower key, modulo 2⁶⁴ − 59; sampling decisions are the same PRF of the new segment's place in the log under a second key; option 3 reworked as keyed install checks (every flush against its log range, every live-docs change against the log's deletes, every merge against its inputs), with no per-document state; F21's public digest stays the cross-replica comparison where no member writes another's bytes; T2 prices the keyed hash for options 2 and 3; T4's forging mutant; no format change (D18; D16's format-13 paragraph) | D14, D15, D18, threat model, fault model, Q9, T2, T4 |
| DM-R26 | The f + 1 admission, and every election a `manual` statement lets run, decide only when every voter of the candidate's membership answered, never on a timeout; the stall stated without a time bound, a gauge and a log naming it; T3's `six_heard` and `grow67_heard`; T6's mutant "the admission decides on the timeout" | D6, D9, D15, fault model, threat model, T3, T6 |
| DM-R27 items 1 to 12 | (1) The liar completes the count with f honest forgetful voters at once at any membership; (2) the model's bound is the least f of the memberships in force; (3) the argument rules out a stale W two changes before C; (4) a batch that would take a core past `max_unsynced` is answered after its barrier, so DMS-13's floor is one window; (5) an install updates the current state and leaves the durable head to the next checkpoint; (6) `limits.md`'s `transfer_list_bytes` row states the manifest and file count 8 MiB admits; (7) deferral holds option 2's figure only while the queue holds and the range is retained; (8) option 3 keeps no per-document memory; (9) T16's oracle is the live state under D1's rule; (10) joint consensus needs a majority of each configuration flagged; (11) sampling decisions are keyed; (12) rf = 3's window stall is uniformity's price, not safety's, and Q10 offers the exception | D2, D5, D6, D8, D14, D17, D18, T16, Q10, "Changes to other documents" |
| DM-R28 | Both checkers sync a candidate at campaign, normal elections and rounds alike (`--barrier candidate`, the default), and optionally at every term change and grant (`--barrier all`); the window schedules and searches redone under it; `grow6` and the shrink fence shown to be artifacts, `grow67` and `grow67_loss` in their place; revision 1's `fplus5`, `heard5`, and `fplus5b` checked under the barriers; the C++ checker takes seven nodes and four changes | D6's model evidence, T3, T6, Q10, the simulator's row |
| Architect C15 to C19 | Step 3 may start, with DM-R24's mark, DM-R26's rule, and the cap before its restart, recovery, and rewind code lands; step 1 waits for this re-check; K2 and K3's rows proposed for the lead; step 5's verifier takes the keyed comparison and T4's forging mutant, and its install path item 5; step 6's schedules and outcomes | First steps, ledger |
| Architect C20 | The two calibration rows, on their own commit | `docs/reviews/calibration.md` |
| Architect C21 | Q9 restated with the keyed comparison, each option's cost and residual, honest and planted | Q9 |
| Architect C22 | Joint consensus: D6's count, argument, and model redone before such a change lands; routed to Track C's D3 | D6, "Changes to other documents" |

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
  durably committed generation, batched across indexes. A tiny index's create and first commit
  make 8 durability calls on the acknowledgement path at strata `main` (4 and 4, by `main`'s own
  test; 10 calls and 10 device flushes when T1 measured them at `0b407c9`), and become a share of
  one barrier per turn.
- **A new acknowledgement, `memory`** (D2, D3, D7): a majority of voters wrote the batch to their
  journals, and each syncs within `sync_interval`. The model: at most f voters lose their OS or
  power or are forgetful at once, a forgetful voter counting until it re-learns, across restarts,
  with any number of process crashes; f the least of the memberships in force. Within it nothing
  acknowledged is lost; beyond it, `memory` writes in the capped window (D2) that no recovery quorum
  held. A second cursor, `commit_durable` (a majority synced), bounds checkpoints and `quorum`
  acknowledgements, so no recovery loses what a checkpoint or a `quorum` write holds.
- **Forgetful voters** (D5): a voter that may have lost acknowledged entries (an OS crash or power
  loss, or a torn tail found by a same-boot restart, such as the one a failed barrier forces) keeps
  its identity, acknowledges appends, and votes only after it re-learns from a leader, staying
  forgetful across every restart until then (the open record's verdict epoch, kept readable for the
  epoch's life, and a mark in its hard state, written before anything else of the epoch). When more
  than f are forgetful at once, a recovery election (D6) counts a majority of its membership flagged
  forgetful at its own term and elects a log at least as up to date as every answer's; it never
  loses a durably committed entry. Under `manual`, it decides only with every voter's answer.
- **Segment mode** (D8, D17, D18): followers append the log but install the leader's segments, moved
  by the snapshot transfer's file sets. Estimated from T2's measured parts with SHA-256, TLS and the
  stack, a three-voter group spends 0.38× to 0.43× of document mode's CPU with the sampled rebuild
  (Q9's option 2) and 0.43× to 0.66× with keyed install checks (option 3, which Q9 recommends), and
  a five-voter group 0.25× to 0.32× and 0.31× to 0.58×, at the same refresh cadence (`arith.out`);
  T2 gates whichever the owner picks (DM-R45). A follower whose
  install lags applies the tail itself, so the replay at promotion stays within one election
  timeout, and where the refresh is relaxed for bulk ingest segment mode's cost tends to document
  mode's plus its shipping, hashing, and verification. Its check against a leader-side fault is the
  owner's choice (Q9): keyed install checks on every follower, which catch a corrupt or planted
  flush, merge, deletion, dead ordinal, or structure a query reads at its first install, leaving the
  leader which deleted documents a merge keeps (statistics never above its inputs'), and a new
  member's or a repair's whole image from the leader alone until a member that checked its entry
  vouches for it. An image counts as checked (grounded) only when an independent check covered its
  content, so a catch-up built on an unvouched image grounds nothing (DM-R40), and a group that
  switches to these checks from sampling counts its followers' images grounded at the switch, the
  content before it checked as sampling checked it, counted (DM-R41). A mode switch's and a
  catch-up's reinstall are compared whole against the follower's own image at the `Image`'s
  position, every dead ordinal matched or refused (DM-R37, DM-R42). The alternatives: a sampled
  rebuild that misses one corrupt flush 97 times in 100 at rf = 3; or a document-mode replica. Every comparison sums a PRF under a key the
  leader never sees, since F21's public digest is forgeable by whoever writes the segment.
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
- `index::install` replaces the whole content under fresh local names (`index.hh:766-827`,
  `788-791`), after reserving storage for it (`index.hh:778-782`); strata's apply skips positions at
  or below the applied one (`index.hh:422-425`) and records the position (`index.hh:454`);
  `commit()` makes everything applied durable (`index.hh:513-525`). Track C's D18 retry already
  asks for an install of only the files a receiver lacks (`track-c.md:783-789`); the survey's Q7
  asks for adopting an externally built segment through the log (`rocksdb-survey.md:365`; owner
  decision `:386-388`).
- The snapshot transfer moves one application blob per image (`snapshot.h:35-45`, `RFTSNP02`) in
  chunks of at most 65,536 bytes at contiguous offsets (`snapshot_transfer.h:29-78`;
  `worker.h:312-319`), started by the leader (`core.h:1106-1110`), on the group's bulk-lane window
  at version 2 (`peer-transport.md:840-843`); a receiver cannot ask for one.
- Read copies are learners, up to every node (`placement-strategies.md:111-127`).

**Syncs to create one tiny index and commit it (T1).** T1 measured them at `0b407c9` through a
counting IO over `PosixIO`, with the syscalls counted by an interposed library (macOS, APFS,
development Mac; `results/2026-10-18-t1-macos.txt`): create 6 durability calls (6 `F_FULLFSYNC`s, 4
`fsync`s), each commit 4 (4 and 3), so create plus the first commit 10 durability calls, 10 device
flushes, and 7 plain `fsync`s, the same for each of 10 indexes; small-indexes measured 10 on its
host (`small-indexes.md:19-21`). strata `main` has since cut create to 4 (DM-R39): the owner lock is
the directory's own lock, which writes nothing, and `clear_uncreated` syncs the directory only when
it removed something. At `3c86484`, by `main`'s own test and code (read, not measured here):

| Step | Durability calls | Where |
|---|---:|---|
| `create` | 4 (5 over an interrupted create's residue) | The directory lock, writing nothing (`lock_directory`, `index.hh:2329-2355`); `clear_uncreated`'s directory sync only when it removed a file (`index.hh:2357-2374`); then the commit of generation 1 (`index.hh:372-376`): its metadata file (`sync_files`, `index.hh:2131`), the directory (`2172`), the manifest temporary (`2176`), the directory (`2180`). `test/directory_test.cc:650-667` pins the 4 and the residue's fifth |
| First commit, one document | 4 | The new files synced together in one call (`sync_files`, `index.hh:2131`), the directory (`2172`), the manifest temporary (`2176`), the directory (`2180`) |
| Each later commit | 4 | The same |

Create plus the first commit: 8 durability calls at `main`. On Linux each is an `fsync` per file or
directory by completion's `posix.h:132-140`, `186-197`, and `559-569` (derived, not measured: the
gate VM is a shared measurement host); the device flushes per call on macOS at `main` are step 7's
to measure.

## Decisions

### D1. Commit becomes three steps

- **Durability** is the core's turn-log barrier, shared by every group on the core: one barrier per
  turn today, at most one per `sync_interval` in groups that acknowledge from memory (D3).
- **Visibility** is refresh: memory only, every second by default (ARCHITECTURE.md:105-106), on
  the no-stalls controller's schedule (D9).
- **Checkpoint** is today's commit protocol (`index.hh:2090-2183`: the new files synced together,
  the directory, the manifest under a temporary name, the rename, the directory), moved off the
  acknowledgement path. It runs in the background, batched across indexes through one node-level
  commit group (small-indexes' D3-A in SI-R6's form, below), and is published by the group
  checkpoint record that lets the log truncate (`track-c.md:582-587`; condition 25).
- **A checkpoint records a past generation (DM-R3).** Apply runs at `commit_memory` (D7), so the
  current state is ahead of `commit_durable` under ingest, and a checkpoint of it would hold writes
  D6 can remove; `Repair` would then restore them and skip their replacements as applied
  (`index.hh:422-425`), and the replica would diverge silently. So strata checkpoints **the newest
  refresh generation whose position is at or below `commit_durable`**, the generations an `Image`
  names (D8): its segments (immutable), live-docs files encoded from that refresh's deleted bitmaps
  (which its reader snapshot holds), its metadata, and its position. The cost is disk: the flush
  and merge output written since the checkpointed generation, about one checkpoint interval of it.
  The core's gate compares the capture's `through` with `commit_durable` instead of refusing
  whenever apply runs past the persisted commit (`core.h:562`, D7's site table).
- **Retention keys on the checkpoint's own manifest (DM-R23).** Today one manifest is both the
  durable head and the live state: `commit_files()` sets the in-memory manifest `m_` to what it
  wrote (`index.hh:2181`), and `retain()` keeps the head `m_.generation` with the names of `m_`
  and the current segments `segs_` (`index.hh:2203-2231`, `2263-2267`), queueing every other file
  for `reclaim()` (`index.hh:2234-2259`). A checkpoint of a past generation splits the two: what it
  writes is generation g's snapshot, while `m_` stays the current state, whose applied position
  decides what apply skips (`index.hh:422-425`; were `m_` the snapshot, apply would re-run the
  positions after g). So the index holds both: the **durable head**, the checkpoint's manifest as
  the batch entry snapshotted it (SI-R19, below), and the **current state**, `m_` and `segs_`.
  `retain()` and `manifest_files()` take the head's generation and names from the durable head,
  never from `m_`, and keep them beside the current state's files, so the merge inputs only g
  names and the live-docs and metadata files encoded for g stay until a later checkpoint's head no
  longer names them. Files of later states that no durable manifest names are the open's to
  reclaim after a crash (`index.hh:2431-2436`), and the open requires every file the head names
  (`index.hh:2416-2418`). A standalone `commit()` checkpoints its current generation, so its head
  and `m_` coincide, as today. T16 tests it: a checkpoint of a past generation whose inputs a
  later merge replaced and whose live-docs files it wrote, then the owner's retention and reclaim,
  then a crash and an open.
- **The recorded generation stays until a newer record is synced (DM-R43).** Replay opens at the
  generation the newest durable checkpoint record names (below), while the owner's retention runs
  once its entry completes, before that checkpoint's record has reached the journal's barrier. In
  that window the durable head is the new generation and the journal still names the old one, so a
  crash there with the old generation's files reclaimed would leave replay a generation it cannot
  open. So `retain()` keeps a third set beside the durable head and the current state: the
  **recorded generation**, the one the newest checkpoint record whose barrier returned names, with
  every file its manifest names, released only when a newer record's barrier returns (the
  journal's `synced(turn)`, D3, which the host passes to the index). It is held in memory: after a
  crash the open finds the recorded generation's manifest and files because no retention removed
  them, and replay's `recover(to)` commits it again as the head. Today strata keeps them by a
  default this design does not rest on: `retained_predecessors = 2` (`index.hh:282`), the manifests
  `retain()` keeps behind the head (`index.hh:2212-2218`), which covers the window only while at
  most two checkpoints complete between a record's write and its barrier; and the pins of
  `checkpoint()` and `release()` (`index.hh:740-756`), which cost a commit each. The recorded
  generation names the mechanism instead, and `retained_predecessors` keeps its role as the
  operator's history of older manifests (I14), load-bearing for nothing here. T16 crashes in the
  window, with the keep removed and `retained_predecessors` 0 as the mutant.
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
  (`index.hh:523-524`), never in the batch.
- **A failed checkpoint (SI-R20, SI-R26).** A checkpoint record names only a checkpoint whose every
  phase completed, so the log never truncates past an index's last completed one. A failed
  checkpoint is not a failed journal barrier: the index fences its checkpoints, never retries a
  sync as success, and is restored at the generation its last checkpoint record names
  (`recover(to)`, which commits that generation again as the head, `index.hh:2410-2413`,
  `2428-2430`), never at the highest manifest on disk, through Track C's repair path (`Repair` at
  that boundary and redelivery from the voter's own log, a leader transferring first,
  `track-c.md:432-450`). The journal is intact and holds every entry above that generation, so
  nothing acknowledged is lost and the voter is not forgetful; a failed journal barrier is D5's (a
  process restart, then D3's same-boot rule). Which failures fail one index and which fail every
  index of a batch on a device is small-indexes' D3 (SI-R7). On a segment-mode follower the
  restored image is older than its later `Image` entries, whose files no member may still hold: a
  source refuses such a `Request` (D17), and the follower takes D8's catch-up from the restored
  image, whose log range its ceiling kept (DM-R47 item 8).
- **Creation (SI-R25, DMS-10).** The journal records an index's creation as strata's `Create`
  command (D16), carrying the UUID `create()` already takes (`index.hh:372`; `create_deferred`,
  which writes nothing until the first commit, stores it, `:2291-2306`) and every
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
    removes the residue under strata's names (those `clear_uncreated` removes, `index.hh:2366-2372`)
    and applies from the creation, provided every manifest present carries the record's UUID. A
    duplicate is never taken for the creation record, so a retry never wipes an index. On a
    segment-mode follower that replay removes its installed segments with the residue, applies the
    batches from the creation itself, as D8's catch-up does (its log holds every write since the
    creation), and returns to segment mode at the next `Image` through a catch-up's whole
    reinstall, its image grounded, being built from the creation: document mode's cost over the
    retained log plus one whole transfer, paid only by a follower with no checkpoint record of the
    index (DM-R47 item 9). Read as
    revision 1 wrote it, a retry after a checkpoint record replayed an index of 100 documents at
    generation 7 into 2 documents at generation 0, on each replica that restarts (the security
    re-check's `create_replay.out`).
  - A manifest with another UUID, another incarnation's index at that path, refuses the replay with
    a named error and removes nothing. Only this replay clears past a manifest or a segment; every
    other `create()` and `open()` keeps today's narrow `uncreated()` and its refusal
    (`index.hh:2309-2315`, `2365`).
- **Per tiny index** (derived from T1's counts): the acknowledgement path pays 1/N of a shared
  barrier, N being the groups in the turn; a checkpoint's 4 durability calls run in the background,
  and on macOS 4 device flushes per device per batch once completion has a cross-IO directory
  barrier (SI-R10); create's 4 (DM-R39) fold into the first checkpoint, since the journal records the
  creation (small-indexes' D3-C, journaled; the creation record above). Idle indexes rarely
  checkpoint: the turn log copies their live records forward instead (`track-c.md:523-535`).
- Every preset keeps a synced journal, `resident` included (Part B, DM-R15), so `commit_durable`
  advances in every group and bounds every checkpoint. Standalone (rf = 1) is the same mechanism, a
  group of one.

### D2. A new acknowledgement, `memory`

- **Acknowledged when** a majority of voters, the leader included, wrote the batch to their
  journals (the write returned, so each kernel holds it) and the leader applied it, so the reply
  carries the verdict (D7). Each voter's barrier follows within `sync_interval` (D3).
- **The model** (one statement, used in the summary, T3, T5, and the simulator change): at most
  f voters lose their OS or power or are forgetful at once, a forgetful voter counting until it
  re-learns, across every restart (D5); process crashes, partitions, and clock skew in any number.
  f is ⌊(n − 1)/2⌋ of a membership of n voters, and while two memberships are in force at once,
  the least f of them (DM-R20, DM-R27 item 2): 2f + 1 voters in steady state; 2f + 2 in Track C's
  replacement windows, between two memberships of the same f (six voters and f = 2 at rf = 5,
  four and f = 1 at rf = 3, `track-c.md:270-275`); and where a group grows or shrinks through an
  even membership, whose neighbors differ, the smaller f (five, six, and seven voters have
  f = 2, 2, and 3, so growth from rf = 5 to 7 runs at f = 2, where D6's `grow67` lives).
- **Survives** every failure within the model (D5, D6: no recovery election wins within it).
- **Lost** only beyond it, and only writes no recovery quorum held (D6) among those a majority's
  kernels held unsynced: **the memory window**. D14 caps it per core, on every voter (DM-R13, DM-R22
  item 1): once a core's oldest unsynced written entry is 4 × `sync_interval` old (400 ms) or its
  unsynced bytes reach `max_unsynced` (64 MiB), the core stops answering at `Written`: a follower's
  append replies wait for its barrier, and a leader counts its own match for `commit_memory` at its
  synced index, so every `memory` acknowledgement that rests on that core takes `quorum`'s latency,
  and nothing is refused. The cap counts the batch it answers (DM-R27 item 4): a batch that would
  take the core's unsynced bytes past `max_unsynced` is itself answered after its barrier, so no
  core ever holds more than `max_unsynced` bytes it acknowledged at `Written` and has not synced,
  which D5's rewind floor relies on. The stall DM-R13 named, under the followers, is the follower's
  half of the rule. So a stalled device keeps at most 400 ms or 64 MiB of a core's written entries
  counted toward `commit_memory`, however long it stalls. Every voter that wrote a batch has synced
  it once its barrier returns; from then on it is as safe as `quorum`.
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
  each group recovers as it stopped: forgetful exactly where its newest hard state carries the mark
  or predates the open record's V (below; D4, D5; DM-R19, DM-R24). A voter reports `synced` only for
  turns a barrier of this process covered, recovery's included.
- **The verdict outlives its evidence (DM-R19, DM-R24).** The new epoch's open record is what ends
  the evidence for an unclean or torn verdict: after it, a restart in this boot reads as same-boot,
  and the old epoch's tail is no longer the newest epoch's (`track-c.md:510-516`). So the open
  record carries the verdict, 8 bytes: V, the newest epoch whose recovery found its voters
  forgetful, this epoch when this recovery did, else the previous open record's V. It is durable
  before the epoch's first turn, as every open record is (`track-c.md:476-478`, condition 8), so no
  restart finds the open without its verdict. Three rules keep V and the mark (D4) true through
  every lifecycle event of the records that carry them: copy-forward, supersession, reclamation,
  torn turns, and the clean close (DM-R24).
  1. **Newest by sequence.** A group's newest hard state is the one in its record with the highest
     per-group sequence number that holds a hard state, never the latest in journal order: a record
     copied forward keeps its group and sequence and takes the turn it is written in
     (`track-c.md:526-528`), so by journal order an idle group's pre-verdict record, once copied,
     would read as its newest (`mark_model.py`'s `firstturn_jorder` control loses a write in 7
     steps).
  2. **The marks before anything else of the epoch, reclamation's copies included (DM-R29).** A
     recovery that leaves a `written` group forgetful, and finds that group's newest hard state
     without the mark, writes the group's hard state with the mark in the epoch's first turns,
     before any other turn: the turn driver admits no other record of any group until those are
     written, and **the turn log takes no reclamation slice in the epoch until the last mark turn's
     write has returned**. Today every staged turn takes a forward slice once the log holds more
     than `retain_segments` segments (`turn_log.h:297`, `forward_slice()` at `:357-374`), chosen by
     segment and group, not by which groups are marked yet, so revision 3's "copies may ride in the
     same turns" let the first mark turn copy a later-marked group H's unmarked pre-verdict record
     into epoch V; a process crash or a clean close before H's own mark turn then read H's newest
     hard state by sequence as that copy, in epoch V and unmarked, and H recovered not forgetful
     without a write it acknowledged (the architect's `mark_groups.py`: 7 steps with no OS crash, 6
     with a clean close between the mark turns, 6 with three groups). With the slices held, a mark
     turn holds marks only, and a slice in any later turn copies a group whose mark an earlier turn
     already wrote. So recovery decides every `written` group's forgetfulness before the epoch's
     first turn, idle groups and groups whose replay is deferred included, from the newest hard
     state and configuration among its recovered records: a group decided later could find a copy of
     its pre-verdict record already written in this epoch. Turns land in file order and recovery
     keeps a whole prefix (one write in flight, above), so any later turn that survives a crash
     implies every mark turn did. From then on the group's newest hard state carries the mark through
     every copy (a copy keeps its payload) until re-learning clears it, and V matters only in the
     window between the open record's sync and those turns, when the epoch holds no record of the
     group, so no copy can stand in for a newer hard state. Revision 2's rule alone took a copied
     pre-verdict hard state for one written after V: a failed barrier, a copy-forward, and a clean
     close brought a voter back electable without a write it acknowledged, with no OS crash (the
     architect's 6 steps; `mark_model.py`'s `literal` control reproduces them).
     - **Why no slice, rather than slices of marked groups only (DM-R35 item 1).** The architect
       found both sound: holding every slice until the last mark turn (`--ride none`) and letting a
       mark turn's slice copy only groups whose marks are in that turn or an earlier one
       (`--ride own`) each hold to depth 9 with two groups and depth 8 with three (D6's model
       evidence, revision 4's runs). `own` can free a segment sooner: one whose live records all
       belong to groups already marked leaves before the last mark turn, as the architect's
       `own_reclaim_forced.py` shows (g1 checkpoints into segment 2, g0's write tears, and g0's mark
       turn copies g0's last segment-1 record, so segment 1 leaves while g1's mark is pending; under
       `none` it stays until the marks are done). This design takes `none` anyway, on the bound
       rule 2 already states: while the marks are pending the core admits no other record, so the
       journal never holds more than it held at the restart plus the marks' own bytes (two 64 MiB
       segments for about 1,350,000 groups, within D14's copy-forward slack, below). `own` could
       only shrink it sooner, within the open, which T8 bounds at 1 s at 100,000 forgetful groups per
       core. For that, `none` is one condition in the turn log (`forward_slice()` returns nothing
       while the epoch's marks are pending) instead of a per-group filter inside a slice chosen by
       segment and group. Revision 4 said the filter frees nothing before the last mark; that was
       wrong, and the choice stands on the bound instead. Writing every mark in one turn
       (`--mpt 0`) also holds, but no bound on one turn's size would then hold, and copies keeping
       their first epoch (`orig`) is the format field revision 3 declined.
     - **What it costs: the open's latency and head room (DM-R32 item 1).** One hard-state record
       per forgetful group per recovery that leaves it forgetful with an unmarked newest hard state:
       34 bytes before framing, about 99 bytes written at the 2.92 bytes written per byte appended
       that Track C measured for such records (`track-c.md:550-552`; arithmetic), so about 99 KB at
       1,000 such groups, 9.9 MB at 100,000, and 99 MB at 1,000,000. Until the last of them is
       written the core admits no other turn and reclaims nothing, so the journal grows by the marks'
       bytes: two 64 MiB segments past `retain_segments` hold the marks of about 1,350,000 groups,
       within D14's two segments of copy-forward slack. The decisions read only what recovery
       already reads (each group's newest hard state and configuration among the records it keeps),
       and nothing is admitted on the core until they and the marks are done: that is the open's
       latency after an unclean restart, which T8 measures at 1,000 and 100,000 forgetful groups per
       core (estimate: the marks' bytes at the device's write rate plus the decisions, under 1 s at
       100,000).
     - **Which copy rule (c) reads (DM-R32 item 2).** A record readable through more than one copy
       (an original and its copy whose turn survived but whose barrier had not returned, or two
       copies) reports the epoch of its oldest readable copy's turn, the reading that gives rule (c)
       the older epoch; `TurnRecovery` reports that one, and it is what `mark_model.py` and
       `mark_groups.py` read by default. Read through the newest copy's turn instead, the rules hold
       too (`mark_groups.py --copy-reading newest`, two groups, depth 9), so the choice is for
       conservatism, not safety.
  3. **The open record lives for its epoch.** Reclamation frees the oldest segments once no live
     group record remains in them (`turn_log.h:375-391` at journal `cb66f3d`), and the open record
     is no group's (`turn_log.h:261`: group 0, never indexed as live), so in an epoch long enough to
     reclaim its first segment the boot identity and V go, and every `written` group on the core
     would recover forgetful after any process crash (`mark_model.py`'s `none` control, 4 steps). So
     the newest epoch's open record is a live record of the journal: once its segment is the oldest,
     reclamation's slices (`turn_log.h:355-374`) copy its fields into the head as an open-copy
     record (`RFTW0002`, below), and its segment leaves only after the copy's turn is synced, as any
     original does (DM-R6). Recovery reads the newest epoch's boot identity and V from its open
     record or, once that segment is gone, from the newest whole copy. Pinning the segment instead
     would pin every later one: reclamation frees only the oldest segments so that the turns left
     stay contiguous (`turn_log.h:376-377`). An open record recovery cannot read, whole or through a
     copy, which Track C infers (`track-c.md:483-486`), counts as naming its own epoch, and its boot
     identity as unknown: unclean, never the same boot. Read as the same boot, a power loss in a
     long epoch loses a write (`mark_model.py`'s `same` control, 5 steps).

  A group in a `written` configuration therefore recovers forgetful when this recovery's verdict
  says so, when its newest hard state carries the mark (D4), or when its newest hard state was
  written in an epoch older than V, the epoch of the turn holding its oldest surviving copy (rule
  2's last item): a group that has written nothing since the verdict, idle or not yet replayed,
  stays forgetful across any number of later restarts until it re-learns. The journal reports what
  it reads, and consensus decides: `TurnRecovery` gains each kept record's epoch (its oldest
  readable copy's), the verdict, and V (additive), and each group's replay reads its newest hard
  state's mark and its configuration's `acks`, which live in payloads the journal does not parse.
  `mark_model.py` checks the three rules against the bytes with one group, and `mark_groups.py` with
  two and three whose marks take one turn each (D6's model evidence).
- **Damage inside turns after S** reads as their torn tail, Track C D11's stated limit
  (`track-c.md:560-562`) widened from one turn to those turns. A strict record never sits there
  unsynced for long: it forces a barrier, and the next turn records it synced.
- **Format `RFTW0002`**: the 12-byte end-of-turn payload, a 16-byte kernel boot identity in the open
  record (D5) beside V (above), a clean-close record (kind `0x12`, free at `turn_log.h:87`), an
  open-copy record (kind `0x13`, also free: the epoch, 8 bytes; the boot identity, 16; V, 8), and
  the group checkpoint record's hard state with the forgetful mark (D4). The first-turn marks are
  ordinary hard-state records. The clean-close record is written only after a barrier covering every
  written turn, so every mark is synced before it, is itself synced, and counts only as the newest
  epoch's last record: a later open record cancels it (DMS-7). A clean close never clears a mark. A
  boot identity of zero or one the platform cannot read reads as unknown, which recovery treats as
  unclean.
- **An unknown identity and a remote journal warn and report (DM-R46).** Unclean is the safe reading
  of an unknown boot identity and refuses nothing, but where the platform gives none (FreeBSD before
  r359053, a container that masks `/proc`'s `boot_id`) every process restart then makes every
  `written` group's voters forgetful: each rolling restart costs every group a re-learning, and two
  restarts at once at rf = 3 take D6's recovery election, logged as a correlated loss though nothing
  was lost. The owner's rule for environment risks, which strata `main` follows for filesystems not
  known local (`index_options::nonlocal`, warn by default, `index.hh:325-331`; the index reports it
  as state, `filesystem_warning()`, `index.hh:586`), is to proceed and report. So completion's boot
  identity returns "unknown" as a value apart from every identity (never zero standing for it);
  `TurnRecovery` reports why a verdict is unclean (the identity changed, the identity is unknown,
  or the clean-close record is missing), beside the verdict and V; the host reports an unknown
  identity as state it reads, and logs it at warning level at each open, naming the platform's
  source it tried. The same rule reaches D3's premise: the same-boot rule rests on the page cache
  holding every turn whose write returned, which on a journal directory not known local holds only
  as the network client's cache holds it. So the turn log's directory takes the classification an
  index's does (completion's `directory_filesystem()`, which `lock_directory` reads,
  `index.hh:2329-2355`) and warns and reports as an index does, under the same `nonlocal` policy
  (warn by default). Both are additive to steps 1 and 2 and may land after either (C37), before
  strata::group runs a `written` group outside tests.
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
- **The forgetful mark (DM-R19, DM-R24).** The hard state gains one byte: whether this voter is
  forgetful in this group (D5). A recovery that leaves the voter forgetful, finding its newest hard
  state unmarked, has the turn driver write the hard state with the mark in the epoch's first turns,
  before any other turn and before any reclamation slice (D3's rule 2, DM-R29); from then on every
  hard state the core writes while forgetful
  carries it set, so the newest hard state by sequence says forgetful whatever is copied forward
  around it. The storage batch that completes the voter's re-learning carries it cleared, after the
  re-learned entries, so a cleared mark never survives without the entries it vouches for: turns
  land in file order and recovery keeps a whole prefix (D3). It rides every place the hard state is
  persisted: storage batches, the turn log's hard-state records (`track-c.md:494-496`), checkpoint
  bundles (`checkpoint.h:97-98`), and the group checkpoint record (`track-c.md:582-587`), and a copy
  forward keeps it, so no checkpoint, copy, or truncation drops it. It needs no barrier of its own,
  the first-turn marks included: a power loss before a set or a cleared mark is synced makes the
  voter forgetful again by the unclean rule (D3), and a restart in the same boot reads it from the
  page cache, so the mark is lost only together with a condition that sets it again. It exists only
  in groups whose configuration has `acks: written` (D16).

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

**Forgetfulness is durable (DM-R19, DM-R24).** A forgetful voter stays forgetful until it re-learns,
across every restart in between: a same-boot process restart, a failed barrier's restart, and a
clean close followed by a reboot each find it still forgetful, because the open record, kept
readable for its epoch's life, names the verdict's epoch until the group's first-turn mark is
written (D3), and that mark and every hard state the voter writes while forgetful carry the mark
(D4), which only re-learning clears; a copy forward of an older record never reads as newer, since
"newest" is by sequence. Revision 1 decided each restart from the boot identity, the clean-close
record, and torn turns alone, so a voter that lost entries in a power loss and then restarted its
process before re-learning came back not forgetful and electable without them: the architect's
`restart3` loses a `memory` write in 7 steps at three voters with one OS crash, and `torn3` does the
same with a torn same-boot tail and no OS crash at all (`results/r2/schedules-cc.out`; the searches
in D6's model evidence). D6's argument needs exactly this: an answerer stays forgetful until it
re-learns.

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
   less the voter's largest possible unsynced window, the longest run of entries the leader sent it,
   ending at `matched`, whose bytes fit `max_unsynced` (64 MiB). Both figures bound an honest
   voter's loss: within the model a completed barrier is durable, and D2's cap answers after the
   barrier every batch that would take the voter's core past `max_unsynced`, the crossing batch
   included (DM-R27 item 4), so what the voter acknowledged and has not synced is at most
   `max_unsynced` bytes of its core, and of this group's entries fewer still. Were the crossing
   batch answered at `Written`, it would lie past the window, an honest forgetful voter's rewind
   would stop short of its loss, and it would wait a `recovery_timeout` for the next one (item 2).
   The second figure is the leader's own arithmetic, so a voter that reports `synced` 0 buys one
   window, as an honest one costs, and no more. The leader counts payload bytes, which the journal's
   framing only exceeds, so its window is never shorter than the voter's. A reply below the floor is
   counted (`forgetful_below_synced`) and treated as the floor.
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
window schedules at six and seven voters, with D4's vote barrier (D6).

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
  answered or, under `balanced` and `aggressive`, `recovery_timeout` passed with no leader. Under
  `manual` only the first: the election its statement lets run decides only when every voter of the
  candidate's membership answered (DM-R26, below). A refusal ends the round; the candidate may start
  another at the next term. The start condition stays f + 1: it only lets a round begin.
- **Why the count at the election's own term is exact.** Take the first recovery election ever won,
  by a candidate X at term U whose membership C has n voters, and its flagged answers, a majority of
  C. Each answerer was at term U or below when it answered (one above U refuses, which ends the
  round), and stays forgetful until it re-learns from a leader of term U or later, across any
  restart (D5's mark). No other leader of term U exists: the candidate's grants and its would be two
  majorities of one term. Consider the first leader W of a term above U elected between the first
  flagged answer and the decision. It won a normal election, since no recovery election won before,
  so its voters were not forgetful when they granted: an answerer that had answered was still
  forgetful then, with no leader of term U or later to re-learn from before it, and one that answers
  later was still at a term at or below U, so it had not voted at the later term; X itself voted at
  U and would have left its round to vote at W's term. W counts its own membership C', which is C or
  one change away from it. C is the newest configuration entry in X's log: starting the round
  persists X's term and vote, a barrier that covers every entry X has written (D3, D4). Track C
  admits a change only once the previous one is committed at `commit_durable` (its D3, the rule that
  closes the single-server bug; D7's site table), so a membership two changes from C needs the
  change between them counted durable by a majority, which X's answering majority of C meets in a
  voter holding it; that voter grants X only a log at least as up to date as its own, and such a log
  holds every entry committed at `commit_durable` (D5's argument), so X's newest configuration entry
  would not be C. A W two changes behind C is ruled out from the other side (DM-R27 item 3): C's
  predecessor was committed at `commit_durable` before C was admitted, so a majority of the
  predecessor's membership synced it; W's quorum, of voters that are not forgetful, is a majority of
  the membership before that one, which meets it in a voter that holds the predecessor and grants
  only a log at least as up to date as its own, which holds every entry committed at
  `commit_durable`; so W's newest configuration entry is the predecessor or later. W counts under
  its whole log's newest entry, since its campaign persisted its term and vote first (D4), the same
  barrier as X's. W's quorum avoids X and every flagged answerer in C'. With C' = C, at most
  n − (⌊n/2⌋ + 1) voters remain; with C' = C plus a voter, at most
  (n + 1) − (⌊n/2⌋ + 1) = ⌈n/2⌉ < ⌊(n + 1)/2⌋ + 1; with C' = C less a voter, which may be a flagged
  answerer or X, at most (n − 1) − ⌊n/2⌋ < ⌊(n − 1)/2⌋ + 1. Each is fewer than a majority of C', so
  W does not exist. No answerer re-learns before the decision, so at the decision a majority of C,
  more than f of it, is forgetful at once, beyond the model. Within the model no recovery election
  wins, and every leader is D5's.
- **Why f + 1 is not enough across a membership change (DM-R20).** Revision 1 counted f + 1 flagged
  answers and argued with one membership (2f + 1 − (f + 1) = f). In rf = 5's replacement window a
  candidate X of the six-voter membership needs 3; a leader W of the five-voter membership that
  removes the leaving voter Q needs 3 of 5, and with X and one flagged answerer P excluded, 3
  remain. So W is elected between X's answers, P re-learns from W, and Q loses power after that and
  answers X: three flagged answers from episodes that never overlap, at most two voters forgetful at
  any instant. The architect's `six` decides without an acknowledged `memory` write in 43 steps, and
  `six_loss` (49 steps) then replaces it on every replica that held it. Growth fails the same way
  one size up, where C' = C plus a voter: in the architect's `grow67` (70 steps), from rf = 5 to 7,
  X counts under the six-voter membership while W is elected under the seven-voter one between X's
  answers, and `grow67_loss` (76 steps, built here) then replaces the write on every replica that
  held it, with at most two voters forgetful at once, the least f in force. Revision 2's `grow6`,
  the growth half of rf = 5's window, was the model's artifact: its W counted under the founding
  five while holding the promotion unsynced, which D4's vote barrier rules out (DM-R28). With the
  barrier, a W counting the founding five lacks the promotion, and in `grow6`'s shape the old
  leader, which holds the promotion and the write, is one of the three voters W needs, and refuses a
  log behind its own. The search from `grow6`'s step 42 finds no loss under f + 1 within 5 steps
  (34,366,022 states), short of the 6 that revision 2's artifact needed from there, and the depth-6
  search filled its table without a result, so this half of rf = 5's window rests on that
  arithmetic, not on a search. Under every barrier D4 imposes, `six`, `six_loss`, `grow67`, and
  `grow67_loss` replay with the loss under f + 1, and under the majority count each stops at the
  decision (four flagged answers needed, three exist); the bounded searches near them find nothing
  (model evidence below). At rf = 3's four-voter window f + 1 = 2 would be safe (DM-R27 item 12): a
  forgetful candidate's two flagged answers overlap in time, two voters forgetful at once, beyond
  the model, and for a candidate that is not forgetful no leader fits between its answers (outside
  the candidate and its two flagged answerers, four voters leave one, three at most one, and five
  two, each short of its majority). The majority count asks 3 there too, so rf = 3's window stall
  (below) buys one rule for every n, not safety; Q10 offers the exception.
- **Why the count since the restart is not (DM-R30).** At five voters (f = 2), under every barrier
  D4 imposes: leader 0 writes `v0` and replicates it to voters 1 and 2, a `memory` majority; 1 and 2
  lose power; 2 probes 1 flagged; 2 re-learns `v0` from 0, still leader; 0 loses power, never having
  synced `v0`, since its campaign came before the write and a `memory` write asks no barrier; 0
  probes 1 flagged. Voter 1 has now heard three forgetful voters, itself included, while at most two
  were forgetful at any instant; its `recovery_timeout` passes and it wins term 2 with voters 3 and
  4, which never held `v0`, while 2 still holds it (the architect's `lead5`, 13 steps;
  `lead5_loss`, 14, then replaces `v0` at 2). Revision 0's every-voter-heard rule loses the same way
  with probes from 3 and 4 in place of the timeout (`heard5l`, 14 steps). Each loses under
  `--barrier none`, `candidate`, and `all` in both checkers (`results/r4/schedules-lead5.out`), and
  from `lead5`'s step 9 the C++ search finds the loss within 4 steps under the owner's rule and
  nothing within 6 under D6's (revision 4's runs, below). Revision 1's `fplus5` and `heard5` put the
  third forgetful episode on the leader that let voter 0 re-learn, which had campaigned while
  holding the write, so D4's vote barrier synced it there and they do not replay; revision 3 read
  that as leaving the question open (DM-R30), replaying two schedules instead of searching for the
  shape the barrier leaves. `lead5` is that shape: the third episode is the leader that wrote the
  entry, which campaigned before the entry existed, so no barrier covers it. D6's rule blocks both:
  voter 1 cannot campaign normally while forgetful, and a round at its own term finds at most two
  flagged answers, short of three. The count at the round's own term stands on the argument above;
  a count gathered across episodes has none, and loses. At three voters both rules hold within the
  model, because two forgetful voters at once are already beyond it.
- **Policies** (`unclean_recovery`, a replicated index setting, D15; the administrative privilege,
  DMS-9). `balanced` (default for `replicated` and `available`, Q3): `recovery_timeout` 5 minutes,
  KIP-966's. `aggressive` (`relaxed`'s default): `recovery_timeout` of one election timeout; it now
  loses only beyond the model too. `manual`: no timeout and no recovery election of its own; an
  operator's statement through the peer transport's admin path (signed, audited, `peer-transport.md`
  D17; DMS-9) lets one recovery election run, which decides only when every voter of the candidate's
  membership answered at its term, never on a timeout (DM-R26). The same statement may admit a count
  of f + 1 flagged answers for that one election, which is the operator's admission of loss the
  dissertation's §3.8 describes, for the window stall below. With every voter answering, the voters
  that hold a write refuse a candidate whose log lacks it, so a decision loses only what no voter
  holds: `six` and `grow67` under the admission decide nothing (`six_heard`, `grow67_heard`), and
  the search from `six`'s step 37, where the f + 1 count loses a write within 6 steps through the
  timeout, finds nothing when every voter must answer (65,219,423 states to depth 6). Through the
  timeout, as revision 2 let it decide, the admission readmits `six` (the architect's
  `six_heard.out`).
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
  falsely can complete the decision's count with f honest forgetful voters at once, at any
  membership (DM-R27 item 1): in a window it also votes in the normal election that lets an earlier
  honest answerer re-learn, so the security re-check's liar in `six` (`SIX_LIAR_LOSS`) decides with
  four flagged answers, the majority of six, while at most two honest voters are forgetful at once.
  That is the compromised-voter row of the threat model.
- **Never lost:** a durably committed entry (synced on a majority M), so every `quorum` and `all`
  acknowledgement. Any recovery majority intersects M in a voter whose durable log holds the entry,
  and that voter grants only a log at least as up to date as its own.
- **Lost:** `memory` writes no voter of the recovery majority held (with every voter answering, as
  `manual` requires, only those no voter held at all), and `leader` writes never committed. Readers
  may have seen the `memory` ones, since visibility follows `commit_memory` (D7). A session token
  naming a lost write reports it lost (D10).
- **Availability** (the fault table): at rf = 3, one forgetful voter and one unreachable voter stop
  elections until either returns or the forgetful one re-learns, where `strict` would elect. In a
  replacement window the majority count adds one stall (DM-R20): with exactly f + 1 voters forgetful
  at once and every candidate holding the even membership (six voters at rf = 5, four at rf = 3), no
  normal quorum of voters that are not forgetful exists and a decision needs f + 2 flagged answers,
  unless a candidate holding the window's odd membership can count a majority of it. The window
  lasts two commits after the learner caught up (`track-c.md:267-268`); revision 1's f + 1 would
  have decided there, and lost writes in rf = 5's window doing so. The stall ends only when an
  operator's `manual` statement admits f + 1 and every voter of that membership answers (DM-R26). It
  has no time bound: the window's removal needs a leader, and nothing re-learns without one. With a
  voter of that membership gone for good, the statement cannot decide and nothing in this design
  ends the stall; that is one failure past the f + 1 forgetful voters, and past Track C's window
  tolerance too (`track-c.md:270-275`), where the group needs PLAN's recovery tooling ("Restore from
  elsewhere: a replica", `PLAN.md:678`), outside this design. A gauge (`recovery_stalled`, 1 while a
  voter holds the round's start count and no leader has emerged for a `recovery_timeout`) and an
  error log each `recovery_timeout` name it, under every policy.
- **Joint consensus (DM-R27 item 10).** The count and the argument assume Track C's one voter per
  change (`track-c.md:255-256`). Track C keeps joint consensus as a build its zone-outage simulation
  may call for (`track-c.md:282-284`); under a joint configuration every quorum is a majority of
  each of C_old and C_new, so the decision would need a majority of each flagged, and the argument
  and the model are redone before such a change lands (C22; "Changes to other documents").

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
| 5 | `timeout`, `heard`, `fplus` | `memory` lost: `fplus5` (15 steps; `timeout`, `fplus`) and `heard5` (15 steps; `heard`); fences found by swarm walks (`heard`, 25 steps; `fplus`, `fplus5b`, 26 steps), all without D4's barriers; under every barrier, the architect's `lead5` (13 steps; `timeout`, `fplus`) and `heard5l` (14 steps; `heard`, `fplus_heard`), revision 4's runs below | Not run |
| 5 | `round` (revision 1) | Holds: 67,817,052 states at depth 8 (one write, terms to 4, three OS crashes; voter symmetry, 64-bit keys); 20,000,000 kind-first swarm walks to depth 30 (445,216,259 steps; terms to 5) find nothing; the forced `fplus5`, `fplus5b`, and `heard5` are blocked | No `quorum` or floor loss: 79,154,748 states at depth 8 (64-bit keys; no loss of any kind that shallow), and 20,000,000 walks to depth 30 (443,382,315 steps; four OS crashes), which find 82 `memory` losses, as beyond the model allows |
| 3 | `round`, liveness | Not applicable | `live3`: the one voter that holds the write is not forgetful, starts a recovery election, and wins it; nothing lost |

The swarm walks are weaker than exhaustive search at a given depth, and calibrated as such
(`results/model-swarm.out`): kind-first walks find the `timeout` rule's loss 197 times in 300,000
walks, while 20,000,000 uniform walks missed the `fplus5b` pattern that kind-first walks found once
in about 10,700,000. Walks with two writes and terms to 6 (10,000,000 each, to depth 40) found
nothing under `round` and nothing under `fplus` either, so at that size they are no evidence and
the table does not count them. So the exhaustive depths are the stronger evidence, and the
one-write walks reach where they cannot. These revision-1 runs had no vote barrier either (DM-R28):
their passes at a fixed membership stand, as revision 2's do (below), while their five-voter
counterexamples to the owner's rule do not replay under D4's barriers; `lead5` and `heard5l` do, and
show the same rules losing ("Why the count since the restart is not", above; DM-R30).

**Revision 2's runs** (DM-R19, DM-R20; `results/r2/`, `runs_r2.sh`, reproduced by today's checkers
with `--barrier none`). The model gained, each off unless asked for: same-boot process restarts,
which clear forgetfulness as revision 1 read D3 (`--restart text`) or keep it until re-learning
(`--restart persist`, what D3's V and D4's mark implement); torn same-boot tails, which keep any
prefix from the synced one and make the voter forgetful (`--torn`); retried rounds (`--retry`);
membership in the log, a change admitted only with an entry of the leader's term at or below its
`commit_durable` and no configuration entry above it, and every count taken over the counting node's
membership (`--base`, `--script`); and the decision's count, f + 1 or a majority (`--count`). Python
and C++ agreed on every feature compared (`results/r2/crosscheck.out`). Those runs had no vote
barrier: a node's membership in force was the newest configuration entry in its synced prefix, and a
campaign synced nothing (DM-R28). At a fixed membership the barrier only adds a sync to each
campaign, so every barrier path is a path of that model with one more step per campaign, and its
searches cover the barrier's paths a few steps shorter than their depth; they stand at fixed
membership:

| Run (no barrier) | Scope | Depth | States | Time | Result |
|---|---|---:|---:|---:|---|
| Restarts as revision 1 read them | 3 voters, one write, terms to 2, two OS crashes, one restart | 8 | 1,489,228 | 4 s | `memory` and fence at 7 steps: `restart3` found by search |
| Torn tails as revision 1 read them | 3 voters, one write, terms to 2, no OS crash, two restarts | 8 | 864,657 | 3 s | `memory` and fence at 7 steps, `torn3`'s shape |
| The mark, torn tails, retried rounds | 3 voters, two writes, terms to 4, three OS crashes, two restarts; symmetry, 64-bit keys | 12 | 235,347,895 | 1,641 s | Holds |
| The same | 5 voters, one write, terms to 4, three OS crashes, one restart; symmetry, 64-bit keys | 8 | 143,348,380 | 1,867 s | Holds |

In the windows the missing barrier admitted elections the design does not: `grow6`'s W and the
architect's shrink fence (below). Revision 2's window rows are kept in `results/r2/` and redone
here.

**Revision 3's runs** (DM-R26, DM-R28; `results/r3/`, `bench/durability/model/runs_r3.sh`, each
under `run_bounded.sh`'s cap at `nice -n 19`, one thread; depth, states, and time are each run's
own). Both checkers now sync a candidate when it campaigns, normal elections and recovery rounds
alike, D4's vote barrier, so it counts under the newest configuration entry in its whole log and
campaigns only as a voter of it (`--barrier candidate`, the default). `--barrier all` also syncs
every node that moves to a new term or grants, a follower's first append of a new term included, as
D4 requires of every term change: that is the design, and it removes OS-crash losses the candidate's
barrier leaves. So the searches run under `candidate`, the more adversarial of the two that counts
memberships as the design does, and the counterexamples the design rests on (`six`, `six_loss`,
`grow67`, `grow67_loss`, `restart3`, `torn3`, `dmr1`) replay under `all` too. The C++ checker takes
seven nodes and four configuration changes. Python and C++ agree on the barrier's state counts in
each mode, at fixed and changing membership, seven voters included: twelve configurations, equal
counts (`crosscheck.out`); an ASan and UBSan build runs each new feature with no report and the same
counts (`sanitizers.out`).

| Run (barrier) | Scope | Depth | States | Time | Result |
|---|---|---:|---:|---:|---|
| Every forced schedule, each barrier mode, both checkers | | forced | | seconds | `six`, `six_loss`, `grow67`, `grow67_loss`: f + 1 loses `memory` under `none`, `candidate`, and `all`, at most two voters forgetful at once, and the majority stops at the decision. `grow6`, `grow6_loss`, and the shrink fence: not enabled once the candidate syncs (step 43, W's election; step 37, node 4's). `six_heard`, `grow67_heard` (the f + 1 admission, every voter answering): no decision. `restart3`, `torn3`: lose under revision 1's restart rule, blocked by the mark. `dmr1`: loses under revision 0's timeout. `fplus5`, `heard5`: not enabled at step 11 under the candidate's barrier, nor their relabeled forms at step 15. `fplus5b`: a fence under the owner's rule with the candidate's barrier, not enabled under `all` (step 5) (`schedules.out`) |
| From `six`'s step 38, f + 1 / majority | 6 voters, rf = 5's window: the round started, two answers in (one flagged), the five-voter leader just elected | 5 | 6,288,672 / 6,288,671 | 27 s each | f + 1: `memory` at 5 steps; majority: holds |
| From `six`'s step 37, f + 1 / majority | One step earlier, before that election | 6 | 84,841,967 / 84,841,965 | 452 s each | f + 1: `memory` at 6 steps, re-electing the first leader, 4, under the five-voter membership; majority: holds |
| From `six`'s step 33, both counts | Three OS crashes done, nothing decided | 5 | 13,627,698 each | 51 s (f + 1's 1,462 s include about 23.5 minutes stopped under another session's strict hold) | Both hold; f + 1's loss is 10 steps away |
| From `grow67`'s step 65, f + 1 / majority | 7 voters, rf = 5 to 7: the round's first two answers in, before W's election | 5 | 131,291,764 / 131,291,763 | 1,002 s, 1,009 s | f + 1: `memory` at 5 steps; majority: holds |
| From `grow6`'s step 42, f + 1 / majority | 6 voters, rf = 5's growth half: X's round with two answers in, the step before revision 2's artifact | 5 | 34,366,022 each | 197 s each | Both hold, with equal counts: no f + 1 loss within 5 steps, though revision 2's artifact path needed 6 from here (its W's election, then 5) |
| The same, f + 1 alone | Any f + 1 loss would show here first, since a majority's decisions are f + 1's too | 6 | stopped at 241,591,911 | 1,509 s | No result: a 2²⁸-slot table filled (the first depth-6 pair, stopped under the strict hold after about 25 minutes, is kept as `-stopped`) |
| `manual`'s f + 1 admission from `six`'s step 37 | Every voter's answer required, no timeout (`round_heard`, f + 1), where `round` with f + 1 loses `v0` within 6 steps (above) | 6 | 65,219,423 | 343 s | Holds |
| rf = 5 to 3, f + 1 / majority | 5 nodes, `remove:4,remove:3`, the four-voter window from the architect's prefix (`shrink31`), within the model (FMAX 1) | 7 | 49,541,981 each | 217 s each | Both hold: the fence the architect's search found at 7 steps without the barrier, node 4 winning under the founding five with its removal unsynced, is gone |
| Two replacements at rf = 3, f + 1 / majority | 5 nodes, four changes, from the architect's prefix with memberships two changes apart in force at once (`replace2`'s step 64) | 8 | 61,278,694 each | 220 s each | Both hold, with equal counts: no decision is reachable that shallow, so this is evidence for four changes with memberships two apart in force, not for the count |
| rf = 3's window from its founding state, both counts | 4 voters: 3 founders, node 3 promoted, node 1 removed; one write, terms to 3, two OS crashes; symmetry, 64-bit keys | 10 | 13,072,436 each | 108 s each | Both hold; no recovery decision is reachable that shallow |
| rf = 3's window from `rf3win`'s step 24, both counts | The promotion synced on all four, `v0` at `memory`; terms to 4, two OS crashes | 8 | 48,893,963 each | 177 s each | Both hold, with equal counts: no decision differs within 8 steps |
| The mark, torn tails | 3 voters, one write, terms to 3, no OS crash, two restarts | 10 | 9,793,683 | 31 s | Holds |
| The mark, torn tails, retried rounds | 5 voters, one write, terms to 4, three OS crashes, one restart; symmetry, 64-bit keys | 7 | 17,649,656 | 1,689 s, about 23.5 minutes of it stopped under the strict hold | Holds |
| The same | 3 voters, two writes, terms to 4, three OS crashes, two restarts; symmetry, 64-bit keys | 11 | 54,757,724 | 411 s | Holds |

**The bytes (DM-R24).** `bench/durability/model/mark_model.py` checks what carries forgetfulness
across restarts, where the checkers above take it as a flag. It builds on the architect's
`mark_mechanism.py` (its state, recovery, and checks) and adds segments, reclamation of whole
segments oldest first, which can take the newest epoch's open record, the open record's lifetime
rules (`--open-life`), a step between the open record's sync and the first turn, and a second check.
One core, one group G in a `written` configuration, two or three client entries: G's batches,
hard-state, and checkpoint records with the mark, end-of-turn S, torn turns whose bytes stay,
copy-forward of G's live records from any segment but the current one, the open-copy record, the
clean close, segment switches, and every stop (a process crash, a failed writeback and restart, a
power loss keeping any whole prefix of the unsynced turns, a clean close and a restart or a reboot)
at every point, recovery included. Checks after every recovery: **safety**, G never recovers not
forgetful while it lacks an entry it acknowledged at `Written`; **precision**, after a process crash
or a clean close G is forgetful exactly when it was before the stop (T7's claim). Procedure:
`runs_mark_r3.sh` (`results/r3/mark/`), one thread at `nice -n 19`. The design runs exercise the
copy: by depth 8 the search enters a state whose newest epoch lost its original open record to
reclamation, with a copy holding its fields, 26,487 times, and a process crash follows from such a
state 3,859 times (`mark_reach.py`, `mark-reach.out`).

| Rules | Depth | States | Time | Result |
|---|---:|---:|---:|---|
| Revision 2's rule (c) alone, the open record copied (`literal`) | 6 | 10,133 | <1 s | Safety lost in 6 steps, the architect's trace: a write at `Written`, a failed writeback and restart (forgetful, V = 2), the open record, a copy-forward of G's pre-verdict record into epoch 2, a clean close and restart: not forgetful without the write |
| "Newest" by journal order (`firstturn_jorder`) | 7 | 34,543 | 2 s | Safety lost in 7 steps: the copy, written after the first-turn mark, reads as newest |
| Revision 3's mark, the open record left reclaimable (`none`) | 10 | 598,427 | 33 s | Safety holds; precision lost in 4 steps: G's record copied out of the first segment, which leaves with the open record, and a process crash reads as unclean |
| The same, an unreadable boot identity read as this boot's (`same`) | 5 | 1,668 | <1 s | Safety lost in 5 steps: the open record reclaimed, a write at `Written`, a power loss read as the same boot |
| Revision 3: the first-turn mark, newest by sequence, the open record copied forward | 10 | 1,458,689 | 115 s | Both hold |
| The same, three segment switches | 10 | 2,218,798 | 137 s | Both hold |
| The same, three client entries | 9 | 722,105 | 43 s | Both hold |
| The same, 64-bit state keys | 11 | 5,111,474 | 310 s | Both hold |
| Copies keeping their first epoch (`orig`), the open record copied | 10 | 2,194,189 | 110 s | Both hold |
| The same, 64-bit state keys | 11 | 8,700,510 | 513 s | Both hold |

**Revision 4's runs** (DM-R29, DM-R30; `results/r4/`, `bench/durability/model/runs_r4.sh`, each
under `run_bounded.sh`'s cap at `nice -n 19`, one thread, at most two at once, after `quiet check`;
depth, states, and time are each run's own). `mark_groups.py` is the architect's multi-group journal
model, ported with one option added (`--switch-marking`): two groups, each with an entry synced and
one at `Written`, one mark per turn, a clean close allowed between mark turns where the row says so.
The forced five-voter schedules and the searches from `lead5`'s step 9 use the C++ checker built
from this branch (`-O2 -Wall -Wextra`, no warning) and the Python model. Every count below equals the
architect's for the same configuration but the two `--switch-marking` rows, which are new.

| Run | Scope | Depth | States | Time | Result |
|---|---|---:|---:|---:|---|
| One group, every forgetful group marked (calibration) | `mark_model.py`'s rule | 8 | 122,267 | 5 s | Holds; `mark_model.py`'s count |
| Two groups, slices in any mark turn (revision 3's text) | | 7 | 146,563 | 7 s | Safety lost in 7 steps, no OS crash |
| The same, a clean close between mark turns | | 6 | 32,286 | 1 s | Safety lost in 6 steps |
| Two groups, no slice before the last mark turn (`none`, D3 rule 2) | A clean close between mark turns | 9 | 2,716,835 | 153 s | Holds |
| The same, with segment switches between mark turns | | 9 | 2,751,883 | 153 s | Holds |
| The same, rule (c) reading the newest copy | | 9 | 2,072,008 | 112 s | Holds |
| Two groups, slices of marked groups only (`own`) | | 9 | 2,807,989 | 158 s | Holds |
| Three groups: `any`; `none`; `none` with switches between mark turns; `own` | | 6; 8; 8; 8 | 83,203; 2,233,453; 2,244,878; 2,249,291 | 4 s; 122 s; 122 s; 123 s | Lost in 6 steps; holds; holds; holds |
| `lead5`, `lead5_loss`, `heard5l`, forced, both checkers, `--barrier none`, `candidate`, `all` | 5 voters; `fplus`, `timeout`, `heard`, `fplus_heard`, `round`, `round_heard` | 13 to 14 | | 3 s in all | The owner's rule and revision 0's lose `v0` (and `lead5_loss` fences it) under every barrier, at most two voters forgetful at once; under `round` and `round_heard` the election is not enabled (`schedules-lead5.out`) |
| From `lead5`'s step 9, `fplus` / `round`, the candidate's barrier | MAXOS 3, terms to 4 | 6 | 16,377,089 / 29,609,357 | 56 s / 98 s | `memory` at 4 steps / holds |
| The same, every barrier | | 6 | 14,429,542 / 26,856,533 | 52 s / 85 s | `memory` at 4 steps / holds |

**The model's limits (C14, DM-R28).** Messages are atomic, a request and its effect in one step, and
there is no pre-vote phase beyond the flagged probes, so the model cannot interleave a vote's
delivery with other steps; an append delivers the leader's whole log, so a forgetful voter never
holds entries it has not re-learned; partitions and process crashes are unbounded, but restarts are
bounded by `--maxr`, writes by `--maxp`, terms by `--maxt`, and OS crashes by `--maxos`, as each row
states. A campaign's barrier is one step with the campaign, so no OS crash falls between the barrier
and the first request, which the design's order (the barrier returns, then requests leave) also
forbids. Membership changes follow one script of up to four changes over given founders, on up to
seven nodes: witnesses and learners never promoted are outside it. Forgetful voters are bounded over
all nodes, learners and removed members included, which restricts the adversary slightly. The window
searches start from the schedules' prefixes and reach 5 to 8 steps, so they show that the majority
count blocks every decision f + 1 makes within that reach, not that no other window schedule loses a
write; D6's argument covers the rest, and T3's simulator profiles extend it (C10, C19).
`mark_model.py` models one core and one group, records rather than blocks, and reclamation of whole
segments under a bound on switches; Track C's block manifests and damage rules are outside it.
`mark_groups.py` takes the same per group, two or three groups whose records share segments and
turns, two entries each, one mark per turn: the marks' order across turns, which one group cannot
show (C26's lesson). The C++ runs compact states into 128-bit keys, or 64-bit ones where a run says
so. **Retiring a counterexample (C24).** Before a counterexample is called a model's artifact, the
same loss is searched for from the schedule's prefix under the corrected model, with every holder of
the write that has not synced it since taken as the copy that goes; revision 3 retired `fplus5` and
`heard5` by replaying them alone, and `lead5` is the shape the barrier leaves.

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

| Sites | Today | This design |
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
  once its files are present: searchable at its next refresh, durable as a checkpoint only once the
  entry's index is at or below `commit_durable` (D1). An install is `index::install` made
  incremental: today's writes every segment under a fresh local name (`index.hh:766-827`,
  `788-791`); the incremental form keeps the files the receiver holds and writes the rest. It
  updates the current state, `m_` and `segs_`, and leaves the durable head to the next checkpoint
  (DM-R27 item 5): today's install commits the installed image as the head (`commit_files()`,
  `index.hh:824`, then `retain()`, `826`), which under D8 would make an `Image` durable before its
  entry is at or below `commit_durable`, so the install path is a third reader of DM-R23's split,
  beside `retain()` and `manifest_files()`, for step 5.
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
     already applied (`index.hh:422-425`).
  During the replay, new writes are appended and committed, and their replies wait for their
  verdicts; reads wait for the applied cursor (`core.h:1176`). D9 bounds the replay.
- **Catching up past the replay bound (DM-R10).** A follower whose install lag passes D9's bound
  applies the committed tail itself through the document-mode path it keeps for mixed versions,
  into its own buffer, flushing only when that buffer fills, until an installed `Image` covers its
  applied position; its own segments for that range then give way to the leader's. So the replay
  at promotion stays within `election_min` whatever the leader's uplink, a follower's disk, or a
  mode switch (D15) does, at that follower's own CPU, and the leader never flushes early to hold
  it (D9). At worst a follower does document mode's work and then receives the leader's files: one
  mechanism, the one the mixed-version rule already needs. While it catches up it merges only its
  own flushes, never the segments of the image it last installed, and the checkpoint ceiling above
  bars checkpointing its own catch-up segments, so its log keeps every write since that image. The
  reinstall's comparison can then match every dead ordinal of the leader's image, from the
  follower's segments or from its log (D18, DM-R33, DM-R38); the entries it skips it never vouches
  for (`skipped`, D17). A catch-up builds on the follower's installed image, so its reinstall
  grounds the image only when that installed image was grounded when the catch-up began; a member
  whose installed image was unvouched installs the leader's image when the comparison passes and
  stays unvouched (DM-R40, D18's whole images).
- **The state after a whole install (DM-R42).** A whole reinstall, a catch-up's or a switch's,
  compares at the `Image`'s position p, and the follower has applied further, to a, by the time
  the committed entry and its files reach it (D18 states how it computes its image at p). On a
  pass it installs the leader's image at p, drops its own segments, and applies (p, a] again from
  its own log on top of the installed image, into its buffer, before the install publishes, so its
  visible position never moves back; it is then in catch-up for (p, a], its ceiling at p and
  merging only those flushes, until a later `Image` covers a. Its log holds (p, a], since its
  ceiling was at or below p. The re-application is the install lag's documents, which D9's bound
  caps, since a follower past it is in catch-up already.
- **A switch from document mode (DM-R37, DM-R42).** The followers hold their own segments, which
  they built and merged in document mode, and the leader's first `Image` after the switch command
  replaces them whole. Two things are fixed where a follower applies the command, each read
  strictly (`switch_model.py` shows each looser reading losing an honest switch):
  1. **The frozen set:** the segments the follower holds when it applies the command, and the
     output of a merge in flight then. It merges none of them until its first whole install; the
     segments it flushes after the command it may merge among themselves, as catch-up merges its
     own flushes. Read as "the segments it then holds", the in-flight merge's output, which holds
     documents live at the command, could be merged away (`--inflight-open`).
  2. **The checkpoint ceiling:** the follower's last refresh before it applied the command. It
     checkpoints nothing it refreshed after, so its log keeps every write after that refresh,
     what it held only in its buffer at the command included, until its first whole install. Read
     as "at the command", the ceiling would admit a refresh at the command's own index, which holds
     that buffer in a flush outside the frozen set: a document buffered at the command, replaced
     after it, and dropped by a merge of that flush is then in no segment and no retained log
     (`--inclusive`, 3 operations).

  The leader, before it writes that `Image`, expunges the deletes made before the command,
  replacements' old versions included: it merges every segment holding a document deleted or
  replaced before the command, the output of a merge that straddled the command included (DMS-24
  item 2), so the `Image` carries no dead ordinal of such a document. A merge that starts after the
  command carries only documents deleted while it runs, which were live at the command, and a
  segment whose dead ordinals all name documents deleted after the command needs no merge. The
  expunge is owed by whichever leader writes the first `Image` after the command, a new leader
  after a failover included, which finds the command in its log. It is background work at sustained
  ingest, in D14's table, and T13 bounds it (DM-R45). `switch_model.py` checks the rule exhaustively
  (the architect's, ported to `bench/durability/model/` with the computation at p and the
  straddling merge added; D18's whole images give the counts): for an honest leader and follower,
  every document the comparison needs is on the follower, in a segment, its buffer, or its log; and
  each looser reading, a skipped expunge, an expunge of the held segments only, a follower that
  merges its frozen set, and a lifted ceiling each lose an honest switch.
- **A pinned ceiling is released at the share by leaving segment mode (DMS-22).** Three things pin
  a segment-mode follower's checkpoint ceiling: catch-up, until an installed `Image` covers its
  applied position; a switch, until its first whole install; and steady segment mode, where it
  checkpoints only images whose checks passed (D18), so a leader that proposes no `Image` pins it
  at its last checked image. Through each, the follower acknowledges every delivery, so consensus
  sees no lag and Track C's forced checkpoint for a slow follower never fires
  (`track-c.md:671-673`), while the leader's own retention is unpinned. A follower whose retained
  log for the group passes the group's share (Track C's D17, `track-c.md:712-714`) while its
  ceiling is pinned, whatever pinned it, leaves segment mode for that group as after a failed
  check: it applies the batches itself from its installed position (in a switch, it keeps applying
  as it did), lifts its ceiling, checkpoints its own state, counts `segment_mode_left{reason}`
  (`no_image`, `switch`, `catch_up`; a count per node, the group in the log line), and logs it at
  error level. For a pinned group this release, not Track C's forced checkpoint, is what runs at
  the share: the forced checkpoint would truncate the log range a whole comparison needs (T4's
  lifted-ceiling mutant), and once the follower has left segment mode its checkpoints are of its
  own state, which the forced checkpoint then governs as in document mode. Its image keeps its
  standing: grounded if the image it builds on was, unvouched if not (D18). It returns to segment
  mode as a switch's follower does, freezing what it then holds and pinning its ceiling at its last
  refresh, at a later `Image` whose every dead ordinal it can match. One naming a document it no
  longer holds and whose write its log no longer holds, which its own release may have dropped, it
  neither installs nor counts as a check failure: it counts `switch_deferred` and stays in document
  mode, at document mode's cost, until a later `Image` it can match (the leader's merges drop those
  ordinals) or an operator's new switch, whose expunge covers them (D15's floor applies). Deferring
  never installs, so it grounds nothing. The release bounds every follower's retained log at the
  share; T4 runs a leader that proposes a switch and never writes the `Image` under sustained
  ingest, and T14 reports the retained log per group across a switch.
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
  on each platform before it judges the targets. This design never holds segment mode
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
- After a correlated loss of more than f voters, D6's policy adds its wait: under `balanced`, until
  every voter answers, at most `recovery_timeout`; under `manual`, until an operator's statement and
  every voter's answer, with no time bound (DM-R26).
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
| `session` | A replica whose visible position is at least the client's token and whose term history holds the token's term at its index | The client's own writes and monotonic reads. A token is (term, index, position in batch). Each replica keeps a **term history** per group (the first index of each term with a committed entry, as Kafka's leader-epoch cache keeps), past every checkpoint back to the session horizon below, so a token below the log's base is still checked (DM-R13, DM-R44); a token whose entry D6 lost reports `lost` instead of matching a new entry at the same index, and a token below the horizon reports `expired`, which a client treats as a lost read position (its next read starts a new session). A token above the group's last index known to the replica (its own, or the `leader_last` it received) is refused at once and counted, never waited on (DMS-9) |
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

- **The term history is bounded (DM-R44).** Every election, leadership transfer (Track D's
  placement moves leaders), and rolling restart adds an entry, and nothing removed one: at 16 bytes
  an entry, a group whose leader changes daily would write about 5.8 KB into every manifest it
  checkpoints after a year, 5.8 GB a year at a million groups per host (arithmetic). Kafka's
  leader-epoch cache, the precedent, is truncated with the log's start. So the history keeps the
  newest `term_history_entries` entries (64 by default, a Detent knob, D15; 1 KiB per group), the
  oldest dropped first, and the **session horizon** is the oldest kept entry's index: a token below
  it is `expired`. The mechanism is the oldest read position a client may resume, counted in
  leadership changes rather than time, so no clock enters it: a session that spans more than 64
  leadership changes of its group starts again, which loses its monotonicity across that gap only,
  never a write; at one leadership change a day that is two months. The history lives on disk, in
  the index-level attribute each checkpoint writes (D16), and is read only for a token below the
  log's base; for tokens inside the retained log the log's own entry terms answer (Track C's
  32-byte index entry holds each entry's term, `track-c.md:695-697`), so it costs no memory per
  idle group. Lucene, Tantivy, and Xapian have no session tokens to compare with; `limits.md` gains
  the two rows. T11 presents a token below the horizon.
- **What this design adds per idle group, in memory (DM-R44; estimates from the field sizes).**
  `commit_memory` beside the commit index (8 bytes), the checkpoint ceiling (8), the recorded
  generation (8, D1), the forgetful mark (1), the `Vouch` interval (16, D18), and on a leader each
  peer's reported `synced` and its rewind allowance's time (16 per peer, 32 at rf = 3): about 80
  bytes per idle group with padding at rf = 3, the term history none, 80 MB at a million groups
  per host. Beside small-indexes' idle open index of 1,964 bytes of heap
  (`small-indexes.md:112-113` on `perf/small-indexes` at `39ba962`), that is about 4%, and T14's
  10,000-group run measures it. Every gauge this design adds is a count per node, with the group
  in its log line, never a series per group: `unvouched` counts the node's groups whose image is
  unvouched, `recovery_stalled` its stalled groups, and so for `segment_mode_left`,
  `switch_deferred`, `grounded_by_switch`, and `install_unvouched`.
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
     no byte (`format-13.md:358`, `1340`, `1350-1360`), and its digest adds the HLC to the `D` record
     only in an index configured with it (`format-13.md:1217-1219`). Format 13 is now done with
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
| Barriers (D3) | Unsynced bytes, and the age of the oldest unsynced written entry, per core | `sync_interval` | **The memory window's cap** (DM-R13, DM-R22 item 1): past 4 × `sync_interval` (400 ms) or `max_unsynced` (64 MiB), a core answers at its barrier instead of at `Written`, a follower's append replies and a leader's own match alike, and so does the batch that would cross `max_unsynced` (D2, DM-R27 item 4), so the `memory` acknowledgements resting on it take `quorum`'s latency, refusing nothing. The mechanism: the bytes and time an OS crash can cost a core beyond the interval. Lucene, Tantivy, and Xapian have no counterpart; they acknowledge only after a commit's syncs. A `quorum` write forces a barrier at once, today's cost for every write (DMS-9) |
| `leader` writes not yet committed | Uncommitted entries | `uncommitted_entries` (1,024, `core.h:722`, counted at `commit_memory`) | Refused at the cap today; `leader` acknowledgements slow with the replication lag first |
| Image shipping and installs (D8, D17) | Install lag per follower | D9's replay bound | Shipping never blocks a write; past the bound, a follower applies the tail itself (D8); D17's limits pace the bytes |
| The mode switch's expunge (D8, DM-R45) | The bytes of the leader's segments still holding a document deleted or replaced before the command | None of its own: the followers apply the batches themselves meanwhile (document mode's cost), and their retained logs are bounded by D8's release at the share | Merges on the pool's merge class, reported to the one controller in the merges' unit (bytes × tiers remaining), never a stall and never an admission rule of its own; the leader writes its first `Image` after the command only once they are done, so a slow expunge delays the switch, never a write. T13 bounds the foreground p99 across a switch and reports the expunge's duration, bytes rewritten, and the followers' retained log |
| Segment-mode verification (D18) | Checks or samples queued, and the bytes of the segments they pin | None: under debt a check or a sample waits, never skipped (DMS-11) | Runs at the pool's background class and never delays an install. Keyed install checks: a follower checkpoints only checked images, and one whose unchecked installs pass D9's replay bound applies the tail itself (D8's catch-up) instead of installing unchecked, so no check drops. Sampled rebuild: a sample waits in a queue whose pinned segments are at most `verify_pinned_bytes` (1 GiB per node), and one the queue cannot hold is counted `verify_dropped` and logged, as is one whose log range a checkpoint truncated before it ran (`verify_lost`). Either queue's debt is reported to the controller like install lag. A follower whose checkpoint ceiling stays pinned (catch-up, a switch, or no `Image`) past the group's retained-log share leaves segment mode for the group (D8, DMS-22), so no check's log range is truncated and no follower's retained log outgrows its share |
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
| `recovery_timeout` | Detent (node) | Yes | When a `balanced` or `aggressive` recovery election may decide without every voter's answer; the majority count still gates it, and `manual` never decides on it (D6, DM-R26) |
| `unclean_recovery`, read and ack floors, defaults | Index setting (replicated), administrative privilege | Yes | Every replica decides by the same log position |
| `acks: synced` or `written` | The group's configuration entry, administrative privilege | Yes, through the log, after activation | In force when durable at each node and committed for the leader's replies (D7); the switch to `synced` syncs everything before it (D4) |
| Replication mode | Index setting (replicated), administrative privilege | Yes, through the log; heavy, and at most one switch per index per `switch_min_interval` (DMS-23) | Document to segment: each follower keeps applying, merges nothing of its frozen set (the segments it held when it applied the command and the output of a merge then in flight), and checkpoints nothing it refreshed after its last refresh before the command, until the leader's first `Image` after the command installs whole through D17; that leader first expunges every segment holding a document deleted or replaced before the command, a straddling merge's output included (D8, DM-R37, DM-R42); D8's catch-up holds the replay bound meanwhile, and D8's release bounds the followers' retained logs. Segment to document: followers replay from their installed position, which D9 bounds. **The floor (DMS-23):** a switch to segment mode costs every follower two keyed walks of the whole index and a whole install, and costs a leader whose segments hold no earlier delete nothing, so the leader refuses to propose a switch within `switch_min_interval` (a Detent knob, 10 minutes by default) of the index's last applied one, naming the floor; and each follower, by its own clock, makes at most one whole comparison per group per `switch_min_interval`, so a switch that arrives sooner (a compromised leader's) keeps it in document mode until the interval passes, after which it joins as D8's released follower does. Every switch, applied, refused, or deferred, counts in `mode_switches` (a count per node) and logs at warning level with the index |
| `verify`: `install`, `sample`, or `replica` (D18, Q9) | Index setting (replicated), administrative privilege | Yes, through the log | Detection only; `replica` moves one voter to document mode as the replication-mode row does. Switching to `install` checks from the next `Image` on, and each follower's installed image counts as grounded at the switch, `grounded_by_switch` counted per node and logged: its content before the switch was checked as `sample` or `replica` checked it, which is the residual, and its `Vouch` interval starts after the switch's entry, so it answers `skipped` for every entry at or before it (DM-R41; D18, Q9's alternatives). A group created before the owner answers Q9 reaches `install` this way |
| Verification rate and `verify_pinned_bytes` (D18) | Detent (node) | Yes | Detection only; each follower's sampling decisions are a keyed PRF of each new segment's place in the log under its own key, so no rate change tells a leader which flush is checked |
| Transfer limits and `transfer_list_bytes` (D17) | Detent (node) | Yes | Pacing and refusal only |
| `term_history_entries` (D10) and `switch_min_interval` (above) | Detent (node) | Yes | The first changes only which old session tokens answer `expired`, never a write; the second only how often a follower makes a whole comparison |
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
| 16, 17 `Chunk`, `TransferControl` | `RFTXFR02`: file sets (D17): `Begin` with an `RFTSNP03` descriptor, and three control kinds, `Request`, `Missing`, and `Vouch` (DM-R31) | `Begin` 6,480 → 6,532 (`snapshot.h:53` at 255 members, plus 52); `Request` and a `Vouch` ask 184 each (`Accepted`'s 168 plus an entry's index and term); a `Vouch` answer 185 (the ask's entry echoed, and one byte); `Missing` at most 6,532 | D17's rules; a `Vouch` answer's byte is 0 to 3 (`yes`, `no`, `not yet`, `skipped`; above 3 refused), its echoed index and term must equal the receiver's outstanding ask or the answer drops, counted (DMS-17), and a member answers at most one per (group, receiver) per `recovery_timeout` |
| Host kinds | Revision 0's segment fetch on host kinds 256 to 271 and 512 to 527 is withdrawn (DM-R8) | | |

**Log and storage formats**, each activating only after every node runs the version (the peer
transport's D7 formats rule; Track D8 builds the activation, until then by procedure,
`peer-transport.md:1414-1416`):

| Change | Where | Rule |
|---|---|---|
| `Membership` + optional `acks` (`synced`, `written`) | consensus log format | Absent reads as `synced`, so old configurations and Detent's keep their bytes; applies by position (D7's rule); every replica refuses `written` before activation |
| `EntryKind::Recovery` | consensus log format | Payload exactly 16 bytes (term, last index at election), its last index at or below the entry's own index; only in `written` groups; a batch holding one, or a hard state with the forgetful mark, uses a new batch magic (`RFTBTCH3`, after `storage.h:46-48`'s precedent: entry kinds through `Recovery`, and one byte after a present hard state, the mark), and a checkpoint whose retained suffix holds one, or whose hard state carries the mark, a new bundle magic (after `checkpoint.h:71-72`), which older readers refuse by magic |
| Hard state | consensus storage | One new byte, the forgetful mark (D4, DM-R19), only under `RFTBTCH3` and the new bundle magic, 0 or 1 and refused above; absent, as under every older magic, it reads 0. Its commit index holds `commit_durable` in `written` groups (D4, D7) |
| `RFTW0002` | journal format | End-of-turn payload exactly 12 bytes, its S at most the turn it ends; the open record's 16-byte boot identity; the clean-close record `0x12`; the open record's 8-byte V, the newest epoch whose recovery found voters forgetful (D3); the open-copy record `0x13`, exactly 32 bytes (the epoch, equal to the epoch its turn is written in; the boot identity; V, at most that epoch), a live record of the journal until a newer copy's turn is synced (D3, DM-R24); the group checkpoint record's and the hard-state record's hard state with the mark (`track-c.md:494-496`, `582-587`), the first-turn marks among them; written only after activation (D3); `RFTW0001` still read |
| `Unsupported` | journal's codec (additive) | A `Corruption` subclass for a magic, kind, or version this binary does not read, so N's readers report N + 1's formats as unsupported, never as damage that makes a group rejoin as a new incarnation (DM-R7; F13-R42's lesson); existing handlers still catch it |

**APIs** (additive; absent, today's behavior): consensus's `Written` event, `Repair{promotion}`, the
checkpoint ceiling, `StorageBatch`'s urgency, `PeerRestarted{incarnation}` (D5), the file-set
transfer (D17); journal's `TurnRecovery` with each kept record's epoch, the verdict, and V, and the
turn driver's first turns for the marks, which it takes before it admits any other record, with
the turn log holding its forward slices until the last mark turn's write returns (`hold_forward`,
additive: `forward_slice()` returns nothing while it is set; D3, DM-R24, DM-R29); completion's
kernel boot identity and a
cross-IO directory barrier (SI-R10); Kronuz/hashes' SipHash-2-4 (D18).

**strata** (versioned with strata's command encoding): the `Image` command (each file's identity,
size, CRC-32C, SHA-256, and a new segment's covered log range; file count bounded by the remaining
bytes over the smallest file record before allocation, as `files.hh:174` does; identities strictly
increasing; each size and the sum reserved before the first chunk, D17); the `Create` command (the
index's 16-byte UUID and its settings; only a UUID's first `Create` creates, and a duplicate applies
as `exists` and changes nothing, D1, SI-R25, DMS-10); the `Merge` command (D12); the session token's
term (D10); the term history, an index-level attribute (per replica, `format-13.md:1116-1123`),
required in `written` groups, at most `term_history_entries` entries of 16 bytes (a term and its
first index), the oldest dropped first (D10, DM-R44).

**Format 13**, its freeze-readiness table (`format-13.md:1365-1388`): the physical identity
(F13-R41, done), bit 16 `version_clocks` (done), the per-segment attribute list (F13-R43, done) and
the index-level one (F13-R51, done), whose tags arrive later with their releases: the retention
slot's [min, max] per segment (a cache tag, Part B), the memory tier's "not yet on disk" mark per
segment (a required tag, Part B), the disk tier's position and a shared-memory restart's valid mark
(index-level, Part B), and the term history (index-level, above). Nothing here waits on a format 14.
D18's keyed comparison changes nothing in format 13 (DM-R25): it hashes F21's record encodings,
which a segment's content and the log's prepared batches determine, at check time, under a key that
lives in a follower's memory, and stores nothing; F21's public digest stays as it is. If T2 ever
prices the optional per-ordinal cache of keyed contributions into existence, it stays in memory: a
key that dies with its process cannot vouch for anything written down.

### D17. One byte mover: file sets on the snapshot transfer (DM-R8, DMS-1, DMS-5, DMS-17)

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
  for a large transfer (4 incoming per node, Track C's D13) and the node's `shipping_bytes` pool for
  a small one (below), so lists never hold more than those budgets. The mechanism is the memory a
  receiver holds before the list's SHA-256 can refuse it; Lucene, Tantivy, and Xapian ship no file
  lists to compare with. For an `Image`, whose committed entry the receiver holds, the file count
  and the total bytes must equal the entry's. Once the list is checked, its sizes are summed with an
  overflow check and must equal the descriptor's total bytes, which today's policy already bounds
  (`maximum_application_bytes`, `snapshot.h:49`, `59`). Any disagreement or overflow answers
  `Result{Rejected}`, counted, before the first chunk (T15). 8 MiB admits a list and manifest for
  about 49,600 segments with a live-docs file each at typical manifest entries of 49 bytes, 35,500 at
  the widest (`arith.out`, DM-R27 item 6; revision 3 summed the typical entry as 46 bytes, DM-R32
  item 5); an index with more could not be transferred, repaired, or shipped,
  which `limits.md`'s row states.
- **`Missing`** (receiver to source, control): the receiver answers the list with the ranges of
  files it lacks; a file it holds must match all four of identity, size, CRC-32C, and SHA-256. At
  most 795 ranges fit the control bound (8 bytes each after a 4-byte count); a receiver that needs
  more asks a covering superset. Before it answers, it reserves the missing files' bytes from its
  storage permit (the rule of `index.hh:778-782`, moved before the first chunk); a refused
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
  (`index.hh:2520-2560`, `verify_all` at `2546`). For an `Image`, the list must also equal the
  committed entry's files, so a member serving files cannot pass bytes the leader did not commit.
  OpenSSL 3 is already linked through `consensus::transport` (`consensus/CMakeLists.txt:26`), the
  platform's library, whose version the transport logs at startup (its threat model's dependency
  row, `peer-transport.md:205-228`); Kronuz/sha256 (portable, without SHA instructions, its
  README:68) is not added.
- **`Vouch`** (receiver to member, control; DM-R31, DM-R34, DMS-15, DMS-17): asks whether a member
  checked a committed `Image` entry (its index and term) itself, from a grounded image (D18, whole
  images). The answer echoes the entry and is `yes` (checked it itself: an install with checks 1 to
  5 passing at that entry from a grounded image, or a whole comparison there against its own image
  while that image was grounded, DM-R40), `not yet` (its checks have not reached the
  entry, or its own image is unvouched), `skipped` (below the entries it checked itself, never
  checked: a catch-up skipped it, it joined after it, or a switch to `verify: install` grounded it
  after it, DM-R41), or `no` (its checks failed at or before
  the entry, or its log holds another term at that index); at most one answer per (group, receiver)
  per `recovery_timeout`. A receiver refuses an answer whose echo is not its outstanding ask, and
  after a `no` re-fetches from a grounded member that is neither the leader nor the sender (D18).
- **`Request`** (receiver to source, control): asks a member to stream a committed `Image` entry
  (its index and term) or its latest checkpoint. A source refuses one for an entry not committed in
  its own log, keeps at most one transfer per (group, receiver), and at most 4 per receiver node,
  served FIFO across receiver nodes and work-conserving (SR22's rule, its outgoing form). A member
  whose own image of the group is unvouched refuses every `Request` of that group (D18, DMS-17). A
  follower whose `Request` goes unserved for one `recovery_timeout` (refused, or no stream begun)
  asks another member whose log holds the entry committed, the leader last, and counts it
  (`request_unserved`), so a leader that is a follower's only source cannot choose alone when that
  follower's checks reach an entry (DMS-17). A source refuses a `Request` for a committed entry
  whose files it no longer holds (a later `Image` replaced them and retention reclaimed them) with
  the `Result{Rejected}` it already answers, counted at the source (`request_superseded`), so no
  field changes; a follower whose failed checkpoint restored an older image (D1) and finds that
  entry unserved by every member takes D8's catch-up from its restored image, whose log range its
  ceiling kept (DM-R47 item 8).
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

### D18. Verifying segment mode (DM-R11, DMS-4, DM-R21, DMS-11, DM-R25, DM-R31, DM-R33, DM-R34, DMS-15 to DMS-24, DM-R40 to DM-R42; Q9)

- **What segment mode gives up.** In document mode every voter builds its own segments, and the
  digest comparison catches the one that diverges (REPLICATION.md:64-66). In segment mode followers
  install the leader's bytes, so a leader-side apply bug, nondeterminism, or a bit flip before
  checksumming (REPLICATION.md:19) reaches every replica under valid checksums; T4's digest
  equality holds by construction; and in an all-segment-mode group no member holds an independent
  image to repair from, so a poisoned image outlives its leader.
- **Every comparison is keyed (DM-R25).** F21's digest sums xxHash64 over records modulo 2⁶⁴,
  unkeyed (`index.hh:659-715`; Kronuz/hashes' `xxh64::hash`, seed 0), over records whose bytes a
  segment's author writes (`reader.hh:74-87`, `625-641`). xxHash64 is not preimage-resistant: after
  the last full 32-byte stripe each step is a bijection of the state for known input, so a leader
  that writes a segment solves one 8-byte field of one added record for any digest it wants, in
  O(1). The architect's `digest_forge.py` drops a document's posting and adds a junk posting that
  makes up the difference: the planted segment's digest, and that document's own contribution, equal
  the honest ones. So the verifier never compares F21 digests. Every comparison it makes sums a
  keyed PRF over the same record encodings: SipHash-2-4 (Aumasson and Bernstein, 2012: a PRF with a
  128-bit key and a 64-bit output, built for short inputs; T2's corpus makes 92 records per
  document, 35.8 bytes each on average, `results/r4/records.out`), under a key each follower draws
  from the operating system's generator when its process starts (`getentropy`, on Linux, macOS, and
  FreeBSD alike), never sends, and never stores, as the paper's section 7 proposes for hash tables;
  SipHash takes no secret-dependent branch or address (its section 1), so the key does not leak
  through timing. The sum is taken modulo the prime p = 2⁶⁴ − 59, so a record repeated d < p times
  moves it by d times a uniform value and no multiplicity cancels it (modulo 2⁶⁴, a record repeated
  2^k times would keep only 64 − k random bits of its value). A leader without the key matches an
  honest sum with a planted multiset of records with probability 1/p per comparison, about 2⁻⁶⁴, and
  a mismatch stops that follower's installs, so a forger gets one try per follower. The architect's
  forged segment and its printable variant sum differently under a key while their F21 digests equal
  the honest one (`bench/durability/keyed_digest.py`, `results/r3/keyed_digest.out`, which first
  checks the implementation against the reference vectors). Honest faults are unaffected: a random
  change matches with probability about 2⁻⁶⁴ either way. F21's public digest stays as format 13
  defines it, the comparison across replicas where no member writes another's bytes (document mode)
  and T4's oracle. **Format 13 needs no change:** the keyed sums are computed at check time from
  content, the records F21 already defines, decoded from a segment or built from the log's prepared
  batches; nothing keyed is stored in a segment, a manifest, an attribute, or the log.
  - Rejected: HMAC-SHA-256 through the EVP the transport already links adds no dependency but costs
    two SHA-256 compressions per record, 7.9 to 23.6 µs per pass over a document's records against
    SipHash's 2.0 to 5.3 (estimates, `arith.out`), and option 3 below hashes every record several
    times; keyed BLAKE3 adds a dependency. A linear universal hash would not do at all: a sum of
    linear hashes is the hash of the summed records, which a forger matches without the key.
  - SipHash-2-4 is about a hundred lines of portable code. Kronuz/hashes, which holds strata's
    xxHash64, has no keyed hash ("none of these hashes is cryptographic", its README), so it is
    added there beside `xxh64`, additive, with the reference vectors as its test.
  - **The keys' hygiene (DMS-18, security C16).** "Never stores" covers the design's own writes; a
    core dump, a diagnostic dump of a live process, or a swapped page would store a key outside
    them. So the two keys (the comparison's and the sampling's, 32 bytes) live in one page locked in
    memory with `mlock` and excluded from core dumps (`madvise` with `MADV_DONTDUMP` on Linux,
    `MADV_NOCORE` on FreeBSD; macOS has neither, so `mlock` alone), zeroed at exit. A
    `getentropy` failure refuses to start the verifier: the follower applies every group's batches
    itself (document mode), logs it at error level, and reports it as state the host reads
    (`verifier_unavailable`, DM-R46), never seeding from the clock or a weaker source. An `mlock` or `madvise` failure (a zero `RLIMIT_MEMLOCK`, say) is logged at warning
    level and counted, and the verifier runs: what it exposes is the local-access row. No check
    logs or exposes a sum: a mismatch logs the entry, the segment, and the check that failed
    (below). With a live process's key a leader could forge any
    sum for that follower until it restarts; that needs local access, the peer transport's
    local-file row, outside this design's model.
- **Keyed sampling decisions (DMS-11, DM-R27 item 11, DM-R32 item 3).** A follower's decision to
  sample a flush or a merge is the same PRF, under a second key drawn the same way, of the new
  segment's place in the log: the group, the `Image` entry's index **and term**, and the segment's
  position in its list. It is sampled when the value falls below 2⁶⁴/64. An entry (index, term) is
  never written twice within a group: D6 may replace an `Image` entry above `commit_durable` with a
  new one at the same index, but only at a later term, and with the term in the input the
  replacement draws a fresh decision instead of repeating the replaced entry's. So each decision is a
  fresh draw that no choice of the leader's can steer (the segment's physical identity would not
  do: the leader chooses it, and a planted segment could reuse one it saw go unsampled), and since
  one PRF output reveals nothing of another, a leader that learns which flushes a follower sampled
  (its load while it rebuilds is visible to its peers) predicts none of the others. A seeded
  non-cryptographic generator, Kronuz/random's `std::mt19937_64` included, is predictable from
  enough of its outputs.
- **What a query reads, and what the keyed sums cover (DM-R31).** F21's records are what the scan
  path reads: postings from a walk of the term dictionary (`scan_terms()`, `reader.hh:491-500`),
  lengths from the `lengths` section, keys and ordinals from the keys cursor (`reader.hh:743-750`),
  stored values. A query reads other structures the segment's writer also wrote: it finds a term
  through the dictionary's index, one FST floor over each block's first key (`find_term()`,
  `reader.hh:470-484`; termdict's `find_run`, `termdict.hh:466-478` at strata's pin `eaf83b3`), scores
  with `norms` and the statistics at the head of `term_index` (`reader.hh:390-395`, `425-439`), and
  prunes with competitive pairs from the postings' skip data (`reader.hh:461-467`, `1245-1296`); a key
  lookup, which check 2 below makes for every delete and replace, passes the key range and the key
  filter before the keys' own index (`keys.hh:165-182`). None of these is in F21's records, so no
  keyed sum covers them: the architect's `term_index_plant.cc` keeps the 200 terms a scan sees and
  their keyed sum while a planted index hides 20 of them from `find`. Even the scan is not
  independent of the index: `scan("")` starts at the index's floor of the empty key
  (`termdict.hh:481-483`), which a planted index also sets. So the install checks below walk every
  dictionary with termdict's cursor, which starts at the first block's offset 0 and follows each
  block's length to the next (`termdict.hh:498-520`), never through the index, and check every
  structure a reader consults against that walk.
- **Keyed install checks** (Q9's option 3; revision 2's digest anchoring, reworked: DM-R25, DM-R27
  item 8, DM-R31). Each segment-mode follower checks every install against the log and its own
  checked image, under its key, before that install's position can be checkpointed:
  1. **Every flush against its log range.** For each new flush segment the `Image` names with its
     covered log range (ranges contiguous and covering every applied position, so no document
     escapes), the follower builds from the range's prepared batches the records of the documents
     the range leaves live in the flush (`D`, length, `P`, and stored: the leader's tokens grouped
     per document, no segment built; docids and versions as apply assigns them; documents deleted or
     replaced while buffered dropped, as `flush_active` drops them, `index.hh:1946-1960`), sums
     their keyed hashes, and compares with the keyed sum of the records of every ordinal of the new
     segment, its bitmap ignored, walked once from the cursors above. A flush writes no dead
     ordinal, and the deletes after its range are check 2's, so an ordinal the range does not
     write, live or dead, fails here (DM-R33). A dropped, altered, or planted posting, document, or stored value
     changes one sum and not the other.
  2. **Every live-docs change against the log's deletes.** It applies the range's deletes and
     replacements, by key, to the image it holds and compares the ordinals it expects deleted with
     the `Image`'s bitmaps, bit for bit: an old version left live after a replace, or a live
     document marked deleted, fails. Each lookup's answer is checked against that ordinal's stored
     key, which its segment's own check verified (each docid's `D` record names its key), and a key
     the log holds live that no segment yields is itself a fault. No hash is needed; the follower
     computes the bits itself. Its lookups are sound because check 5 verified every key structure
     they pass through at that segment's install: a planted filter cannot hide an old version from
     them (DM-R31). For a merge output the bits it expects are check 3's: an ordinal is dead exactly
     when the input document it names is dead at the `Image`'s position.
  3. **Every merge against its inputs, every ordinal of its output (DM-R33).** A merge carries the
     documents deleted while it ran into its output as dead ordinals (`install_merge`,
     `index.hh:1401-1429`), and ranking reads every ordinal: the snapshot's document count and
     total length and each term's document frequency count dead ones (`searcher.hh:92-121`), and
     BM25's IDF and average length come from them (`search.hh:76-82`). Revision 4 compared live
     records only, so three planted dead ordinals left F21's digest and every keyed sum equal while
     moving the best match for "alpha OR beta" out of the top three (the architect's
     `dead_stats_plant.cc`). So the check covers every ordinal. The follower walks the output once,
     keeping each dead ordinal's docid in a flat table with slots keyed by its PRF (8 bytes per
     carried document, released at the check's end), then walks the inputs once more, their bitmaps
     as it expects them at the `Image`'s position from the log. The table is bounded by the inputs
     before it is allocated (DMS-21): the output's ordinal count, as its statistics claim it, must
     be at most the sum of its inputs' ordinal counts, which their own installs verified, or check 3
     fails without a walk; the table then takes at most that many slots, and the walk fails at the
     first ordinal past the claimed count, which check 5 also compares with the walk. So a leader's
     output never sizes a follower's memory beyond 8 bytes per ordinal the follower already holds.
     The checks:
     - every ordinal of the output names, by its `D` record's docid and version, a document of the
       inputs: each document live at the position exactly once, live, and each dead there at most
       once, dead; an ordinal naming no input document, or a document twice, fails;
     - the keyed sum of the records of every output ordinal, dead ones included, equals the keyed
       sum of the inputs' records of the documents the output names, the live ones and those in the
       table, the inputs' dead records walked as their live ones are.

     So a planted dead ordinal, a carried document whose postings or length changed, and a live
     document marked dead each fail at the first install (`bench/durability/model/check3_model.py`:
     over every output of up to four ordinals from two input documents, and of up to three from
     three, revision 4's rule passes outputs with phantom or altered records that raise a
     statistic, and this rule passes none and refuses no honest output, `results/r5/check3-*.out`).
     The inputs passed their own checks, so by induction every ordinal of every installed image,
     live or dead, holds the records of a document the log carried.
     - **What it leaves.** Which of the inputs' documents dead at the `Image`'s position the output
       carries is the leader's choice, as an honest merge's start makes it (it carries those deleted
       while it ran). Checking that choice needs the merge's start and the log from there, which a
       follower's checkpoint may have truncated, or each document's deletion position, per-document
       state this design does not keep. Every carried document is one the log wrote and deleted,
       with its own records, so a merge never raises the document count, the total length, or any
       term's document frequency above what its inputs counted, dead documents included; a leader
       can keep chosen deleted documents in the statistics through later merges, as an index that
       never merges them keeps them, which moves IDF and average length for the terms of the
       deleted documents it keeps and hides no document from a match (a dead ordinal never
       matches; it can only raise a pruning bound). Each follower reports the dead ordinals each
       merge output carries beside the inputs' dead count (`merge_dead_carried`), so a leader that
       keeps deleted documents an honest merge would drop shows in the ratio.
  4. **The image's metadata against the log.** The `Image`'s last docid, schema version, and
     metadata entries (F21's `L`, `S`, and `M` records) equal those the follower derives from the
     log it appends: docids as apply assigns them (check 1 tracks them), and the schema and metadata
     commands, which a segment-mode follower applies itself (D8).
  5. **Every structure a reader consults, against the walk (DM-R31).** For each new segment, flush
     or merge output, in the same walk:
     - **Both dictionaries are canonical.** The terms and the keys are re-encoded from the cursor's
       walk with termdict's builder and must equal the received blocks and index byte for byte. For
       the keys the alphabet is the writer's, the bytes the walked keys use (`keys.hh:74-97`),
       everywhere. For the terms there is **one rule for every term dictionary**, a flush's, a merge
       output's, or a whole image's, incremental install or not (DMS-20, security C18): the received
       alphabet, which termdict's parser already requires strictly ascending and at most 255 bytes at
       every open (`alphabet::parse`, `termdict.hh:197-212`, so that clause costs nothing), must
       contain every walked term's byte, and the re-encoding under it must equal the received bytes.
       An alphabet only sets the codes; every read decodes through the same parsed alphabet the walk
       used, and a query byte outside it never matches, as in a dictionary that holds no such byte. So
       a superset reads the same on every path: the architect's `alphabet_whole.out` over strata's own
       segments (every flush, a merge whose dropped document took a byte, a merge of a merge, and a
       planting leader's widened alphabet, each lookup returning its walked value) and the security
       reviewer's `alphabet_rules.out` (`find` over the terms and absent keys inside and outside each
       alphabet, prefix scans, the cursor walk, `find_run`). It cannot hide or add a term; a missing
       byte fails the containment, and a narrowed, unordered, or repeated alphabet fails the parse or
       the comparison. A leader can widen an alphabet, which costs suffix-bit width, at most 8 ÷ the
       canonical width (1.03× the blocks in the security reviewer's probe), persists through later
       merges (`merge.hh:92-99`), and moves where blocks are cut, as an honest writer under that
       alphabet would cut them and the re-encoding reproduces. Revision 6's other rules (a flush's
       walked terms; for a merge, the union of its inputs' stored alphabets) refused only alphabets
       that read the same, tied to `merge.hh`'s exact policy and to which inputs a follower happens
       to hold, so the same bytes passed on a new member and failed an incremental install
       (`check35_real.out`, part A: M1 and M2); they are withdrawn, and with them security C14's "a
       function of the walk alone" clause. The builder is a function of its entries and alphabet: it cuts a
       block at 64 entries or past 1,024 bytes after 32 (`termdict.hh:125`), shares exactly the
       longest common prefix within a restart run (`:102`), and indexes each block's first key at
       that block's offset (`:172`). So the index holds exactly each block's first key at its offset,
       and every lookup behaves as on an honest writer's bytes: a planted index, restart table, or
       prefix length that hides a key from `find`, or makes `find` answer for a key the walk does not
       hold, fails. This also gives the term run's values, which `find_run` replays, the same bytes
       the walk decoded. The keys' rule is the same function (`keys.hh:72-121` builds through it,
       `:97`). The re-encoding follows the rules of the segment's format version, and a change to
       them ships with a new version (Kronuz/termdict's row in "Changes to other documents"), so a
       rolling upgrade never fails it.
     - **The key range and filter.** The range's minimum and maximum equal the walk's first and last
       keys, and the filter contains every walked key's hash (`fusefilter`'s filter8, which a lookup
       consults before the index, `keys.hh:165-168`): a filter's false positives fall through to the
       index, which the walk verified, so only false negatives could hide a key, and those are what
       the check rules out.
     - **Norms and statistics.** Each ordinal's norm byte equals the encoding of its walked length;
       the head of `term_index`, the total length, the least and greatest length, and the most
       positions any posting holds (`reader.hh:390-395`), equals the sums and extremes over the walk
       of every ordinal, dead ones included, as the writer counts them.
     - **Each term's dictionary value, rebuilt from the walk (DMS-16, DM-R35 item 3).** Re-encoding
       a value from its own decoding checks only that it is canonical: a value with planted
       whole-list pairs and a planted `cf` decodes and re-encodes to its own bytes (the security
       reviewer's `impacts_roundtrip.out`). So the verifier builds each term's info as the writer
       does (`writer.hh:304-336`), from what the walk decoded alone: `df`, the postings; `cf`, the
       sum of their wdf; the flags `no_wdf` (every wdf 0), `no_positions` (no position read), and
       `counts_are_wdf` (every position count equal to its wdf); for a single posting
       (`single_posting`, df 1) its ordinal and wdf; the postings' and positions' offsets and
       lengths, the bytes the walk's decoders consumed (each decoder reports where it ended:
       `postings_iterator` bounds its reads at `info.postings_length`, `reader.hh:1147-1149`, and
       `positions_reader` at `info.positions_length`, `reader.hh:1828`, and neither exposes its end
       today, so step 5 adds an accessor to each), each term's regions starting where the
       previous term's end (`term_run`, `format.hh:672-682`); and for a term with skip blocks
       (df ≥ 128) its whole-list competitive pairs, `competitive()` over the walked (wdf, norm)
       pairs (`format.hh:773-790`). It encodes that info with `encode_term_info`
       (`format.hh:689-724`), whose `cf_is_df` follows from those values, and whose `absolute` and
       restart flags are those the re-encoding builder assigns under the segment's alphabet
       (`next_is_restart()`, `termdict.hh:141`), since the alphabet's width moves the block cuts and
       with them the restart points (DMS-24 item 1), and compares the bytes with the received value.
       The whole-list pairs are `term_scorer`'s bound
       (`search.hh:321-323`), which MaxScore orders and skips terms by (`search.hh:756-773`), and the
       bound past a term's last skip block (`search.hh:1283-1287`; `reader.hh:1280-1292`): a pair
       set below the term's best would let pruning pass a document a scan sees. `cf` is read by no
       scorer today (a merge recomputes it, `merge.hh:148`), and is checked so that it stays sound
       when one does. `bench/durability/term_value_check.cc` builds the honest values and plants each
       field: the round trip passes every plant, and the rebuilt value refuses each
       (`results/r5/term_value_check.out`).
     - **Skip data, in `main`'s layout (DMS-16; format 13's F10, `format.hh:427-448`).** For each
       term with skip blocks, every field a reader consults without decoding a block equals what the
       walk decoded, as the writer builds it (`write_postings`, `writer.hh:533-687`): the header's group count, its skip
       flags (the offset width and the codec width), the tail's first position index and block
       offset, and the three regions' lengths (pairs, records, blocks); level 1, per group of 32
       blocks (`load_group`, `reader.hh:1475-1506`), its last ordinal, its frontier pairs
       (`competitive()` over its blocks' walked pairs, `writer.hh:637`), and its pairs, record, blocks,
       first-position, position-block, and count-block offsets; and each group's record
       (`reader.hh:1507-1570`): its advance entries (each block's last ordinal, byte offset, and codec
       code), each block's competitive pairs over that block's walked (wdf, norm) pairs
       (`writer.hh:572`), and the positional deltas. Offsets are compared as values, so either
       offset width a writer may choose (`wide_skip_offsets`, `writer.hh:170`, `642`) passes. Each
       block is decoded through the codec its entry names, as a query decodes it, and the code must
       equal the writer's choice for that block (a bitset where it is smaller than the gaps' FOR,
       `writer.hh:553-573`), so the bytes are canonical; the codec code reserved for Elias-Fano blocks
       is refused, as every reader refuses it (`format.hh:450-454`). A term below 128 postings stores
       no pairs; its bound is computed from its postings (`tail_impacts`, `reader.hh:1810-1815`). A
       planted skip entry or pair would make pruning or `advance()` pass a document a scan sees.
     - **Threshold seeds (format 13's M20, on `main` after `026ee10`; DM-R39).** A term with skip
       blocks and wdfs stores, per k of 10, 100, and 1,000 up to its df, two dominance pairs
       (`format.hh:527-539`), from which a search over a segment without deletes starts its pruning
       threshold (`searcher.hh:198-215`): a seed above the honest one would let pruning pass a
       document a scan sees. The verifier rebuilds them as the writer does (`seeds_of`,
       `writer.hh:751-823`, a function of the walked postings, lengths, and the segment's statistics,
       which the rules above verified) and compares the bytes. strata's own `check()` verifies only
       that each pair dominates its k postings (`check.hh:45-54`), which is sound for pruning but
       admits any weaker seed the leader writes; the rebuild leaves the leader no byte.
     - **No section is unchecked.** The verifier holds a check for every section kind and structure
       bit in format 13's table (`format.hh:166-188`) that segment mode writes; an installed segment
       carrying a section or bit it has no check for fails its install check. The reserved sections
       (values, points, term lists, index sort, pair summaries, the stored dictionary and holes,
       quantized impacts, the key hash) each gain their check in the release that first writes them,
       and a unit test fails when the table holds a kind the verifier does not name.
     A failure in check 5 is a fault as any other: logged at error level, counted, and the follower
     applies the batches itself from its installed position (below). Checks 1 to 5 compare what the
     follower computes from the leader's bytes and the log; they replace nothing, so every file the
     follower keeps stays byte for byte the `Image`'s, as D17's identity rule needs: a follower that
     re-encoded and kept its own structures would hold files no other member could serve or match.

  **Failures, counted per check (DMS-19, security C17).** Every check failure counts in
  `verify_failed{check}`, one series per check (1 to 5, and the whole-image comparison), and logs the
  entry, the segment, and the check that failed, never a sum (DMS-18). Check 1 compares records
  built from the prepared batch (`inverted_plugin::records(docid, part)`, `reader.hh:1986-1996`) with
  records walked from the segment (`reader.hh:1970-1982`), which agree only while the writer's output
  is a function of the part, as F21 states and `validate` protects (`reader.hh:319-371`). A part
  shape where they differ is no plant: one client's document would fail check 1 on every
  segment-mode follower at once, and each would leave segment mode for that group until an operator
  acts. So a check-1 failure under an honest leader is a strata bug, and T4's honest control, a
  corpus over every part shape the plugins admit with zero check failures, is its gate; the
  per-check counter lets an operator tell a shape (check 1 failing on every follower at one entry)
  from a plant.

  **No per-document state.** Revision 2's form kept an expected digest from the log and an 8-byte
  contribution per live document to subtract deletions with: 16 bytes per live document in memory,
  the option's cost with millions of indexes per host, rebuilt by walking every index after a
  restart once the key is per process (DM-R27 item 8). The checks above keep nothing past a check,
  so a restart costs nothing and the key dies with the process. While one runs (DM-R32 item 7): a
  flush check streams its range, since the sum is additive: it adds each document's records as the
  range's batches are read and subtracts them when a later batch of the same range deletes or
  replaces that document, so it keeps, for each keyed document the range added and still holds live,
  its keyed contribution under the SipHash of its key, 16 bytes in a flat table released at the
  check's end: at most 16 bytes per document of one flush's range, 1.6 MB for T2's flush of 100,000
  documents (arithmetic). The table's slots are keyed by the follower's PRF, so a leader cannot pick
  keys that collide in it. A merge check streams its inputs' walk, holding the docids of the
  output's dead ordinals, 8 bytes per carried document in a table keyed the same way (check 3), and
  check 5 compares each
  re-encoded dictionary block against the received bytes at its offset as the builder cuts it, so it
  holds one block and the index's builder, one key per 32 to 64 entries. A per-segment cache of each
  ordinal's keyed contribution (8 bytes per document slot, under a memory budget) would spare a merge
  its inputs' walk; it is an optimization T2 may price, never needed for correctness.

  **What it catches:** any fault in a flush, a merge, a deletion, the image's metadata, or any
  structure a reader consults, honest or planted, in a live ordinal or a dead one, at the first
  install that carries it, before a merge can hide it; for whole images, what the bullet below says.
  The follower that detects it stops installing that group's images and applies the batches itself
  from its installed position, the last image whose checks passed, which its log still holds, and
  becomes the group's independent copy (below) when that image was grounded; on an unvouched image
  it stays unvouched, since what it builds carries whatever that image held (DM-R40). Within a segment it leaves no byte a reader consults
  unchecked (check 5's last rule) and one choice: which of its inputs' deleted documents a merge
  output keeps, which moves ranking statistics, never above the inputs' (check 3's residual).
  **The boundary:** every check compares a segment with the log, and a compromised leader writes the
  log: a document it alters before proposing it is what the log implies, and no check here sees it
  (REPLICATION.md:8-12, "nodes are not Byzantine").

  **Debt:** a check waits under debt, never skipped. A follower checkpoints only images whose checks
  passed (its checkpoint ceiling, D8), so a pending check's log range is never truncated, and one
  whose unchecked installs pass D9's replay bound applies the tail itself through D8's catch-up path
  instead of installing images unchecked: a leader that floods ingest buys a follower document-mode
  work, never a dropped check. The checks' queue is reported to the controller like install lag
  (D14).

  **Cost** (estimates, `arith.out`): per document, one keyed pass over its records built from the
  log, and one over each segment that holds it, once as an install and again as a merge input:
  SipHash-2-4 over 92 records costs 2.0 µs per pass at the paper's section-6 lower bound for a core
  with three 64-bit units and 5.3 µs at its 2012 measurements (`results/r4/records.out`; revision 3's
  5.7 counted one block too many per record, DM-R32 item 4), plus each walk's decode, unmeasured here
  (assumed 0.5 to 2 µs per document), and check 5 on each installed segment, unmeasured here too
  (assumed 0.2 to 1.0 µs per document of the installed segment, once per install, not again as a
  merge input). T2's one-flush run makes 2 passes and 1 install per document; its 16 MiB run, whose
  37 segment files total 2.26 times the final segments' bytes, 4.5 passes and 2.26 installs. So a
  follower spends 5.2 to 15.5 µs per document at one flush and 11.8 to 35.1 µs at 16 MiB, 0.12× to
  0.36× and 0.09× to 0.27× of a document-mode follower's 42.8 and 129.3 µs. Q9 prices the group and
  where it meets option 1; T2 measures the walk, record building included (`digest_record` builds a
  string per record, `reader.hh:74-87`), and check 5 (DM-R32 item 6).
- **Whole images (DM-R31, DM-R33, DM-R34, DM-R40, DM-R41, DMS-15, DMS-17, DMS-21).** An install
  that replaces a follower's whole content has no log range of its own to check. A member's image
  is **grounded** when an independent check covered its content, traced back to the log without
  passing through an image no one checked (DM-R40):
  1. it built the image itself from the log, from the index's creation (document mode) or on top of
     a grounded image (document mode after a failed check or D8's release, or D8's catch-up);
  2. it installed it incrementally on a grounded image, checks 1 to 5 passing;
  3. it compared it whole against its own image at the `Image`'s position while its own was grounded
     (a switch's or a catch-up's reinstall, below);
  4. it fetched it from a grounded member that is not the leader, check 5 passing; or
  5. a member that checked the entry itself answered `yes` for it;

  and once, at a switch to `verify: install`, each follower's image as the switch finds it (DM-R41,
  below). Any other image is **unvouched**, and so is every image built on one: applying the log on
  top of an unvouched image, in D8's catch-up or in document mode after a failed check, a `no`, or
  D8's release, carries whatever that image held, and a whole comparison against it finds a plant
  the leader kept equal. A member whose image is unvouched answers `not yet` to every ask, serves no
  file of the group, and counts in the node's `unvouched` gauge until a `yes` grounds it. Revision 6
  listed "one it built itself (document mode, or D8's catch-up)" and "one it compared whole against
  its own" without asking what the follower's own image rested on, so a new member that installed
  the leader's planted image alone, entered catch-up, and reinstalled the leader's next image whole
  was grounded with the plant kept, and vouched and served: the architect's `vouch_model_r6.py`
  loses in 5 steps, and this branch's `vouch_model.py` with revision 6's catch-up rule in
  5 steps, the architect's schedule (`results/r7/v7-any-e3-d13.out`, `arch-vr6-entry-catchup-e3-d13.out`).
  - **A mode switch's and D8's catch-up reinstall** compare the leader's image at its position p
    with the follower's own image there, and run check 5 on every segment of the leader's. A pass
    grounds the leader's image only under rule 3: in catch-up, when the follower's installed image
    was grounded as its catch-up began; in a switch, when the follower's document-mode image was
    grounded (built from the creation or on a grounded image). A follower whose own image was
    unvouched installs the leader's image on a pass and stays unvouched.
    - **The comparison at p (DM-R42).** The follower has applied past p, to a, when the committed
      entry and its files reach it: during a switch it keeps applying, and in catch-up it applies
      until an `Image` covers its position. Its live set at p is its live set at a, less the
      documents the log writes in (p, a], plus the documents live at p that (p, a] deletes or
      replaces. So it sums the keyed hashes of its live records at a, subtracts those of the
      documents written in (p, a] and live at a, and adds those of the documents live at p and dead
      by a, each found in its segments (a live ordinal of its frozen set, or a dead one a merge
      carried), its buffer, or built from its log's prepared batches while its log holds the write,
      and compares that sum with the keyed sum of the leader's image's live records. The leader's
      dead ordinals are compared too (DM-R33): each names, by docid and version, a document dead at
      p, at most once, and its records equal those of the same document in the follower's segments
      or buffer or, where those no longer hold it, those built from its log while the log holds the
      document's write. After a pass the follower's state is D8's: the leader's image at p, with
      (p, a] applied again on top before the install publishes.
    - **A bound before the table (DMS-21).** The leader's image's ordinal count, live and dead, as
      its statistics claim it, must be at most the follower's own ordinal count, live and dead,
      plus the documents its retained log writes, or the comparison fails without a walk; the dead
      ordinals' table then holds at most that many slots of 8 bytes, and the walk fails at the
      first ordinal past the claimed count. Every dead ordinal must name a document the follower
      holds or its log writes, so the bound refuses nothing honest.
    - **Why every record it needs is there (DM-R37, DM-R38, DM-R42).** In catch-up the follower
      merges only its own flushes, never the segments of the image it last installed, and its
      checkpoint ceiling keeps its log since that image (D8). In a switch it merges nothing of its
      frozen set (the segments it held when it applied the command and the output of a merge then
      in flight), its ceiling is its last refresh before the command, and the leader's first
      `Image` after the command carries no dead ordinal of a document deleted or replaced before the
      command, a straddling merge's output included (D8). So every dead ordinal of the leader's
      image, and every document live at p and dead by a, is on an honest follower in a segment, its
      buffer, or its retained log. `bench/durability/model/switch_model.py` (the architect's, ported,
      with the computation above checked against the true live set at p and the straddling merge)
      finds every needed document present, the computation exact, over every log of up to 6
      operations on 2 keys and 5 on 3, with every command, `Image`, and follower position and every
      schedule of flushes and merges on both replicas: the switch over 16,241,982,
      11,083,575, and, at four segments, 1,769,706 cases; catch-up over
      6,403,536 and 4,319,922 (`results/r7/sw-*.out`). Each looser reading and
      mutant loses an honest comparison: the ceiling read as "at the command" (`--inclusive`) and
      the frozen set without the in-flight merge's output (`--inflight-open`), each the
      architect's losing readings of revision 6's text; a leader that skips the expunge
      (`--no-expunge`) or expunges only the segments it held at the command (`--expunge-held-only`,
      DMS-24 item 2); a follower that merges its frozen set (`--merge-held`); and a lifted ceiling
      (`--no-ceiling`), in both scenarios where they apply (each counterexample within a log's first 3 operations, over 5 operations and 2 keys).

    One that matches nothing fails the comparison like any other mismatch: a fault as any other
    (below), counted in `verify_failed{whole image}`, after which the follower applies the batches
    itself. The one exception is a follower that left segment mode at the share (D8's release),
    whose own release may have dropped what an ordinal names: it defers that `Image`, counted
    `switch_deferred`, and never installs it.
  - **A switch to `verify: install` (DM-R41).** An image installed under `sample`, or beside a
    `replica`, was checked against the log only on its samples, so by the rules above it is
    unvouched, and every group created before the owner answers Q9 reaches `install` by this
    switch. Kept unvouched, no member of such a group is ever grounded again: `vouch_model.py
    --switch unvouched` reaches 0 states with a grounded member, with a missed plant
    and without, so no member would serve a whole image or vouch, and every new member and repair
    would install from the leader alone for good; and the content checked only by samples has no
    independent base to compare with once checkpoints truncated its log range. So at the switch's
    position each segment-mode follower's installed image counts as grounded (`grounded_by_switch`,
    counted per node and logged at warning level), with its residual stated: content installed
    before the switch was checked as `sample` or `replica` checked it, option 2's residual, which
    the group already ran under. Its `Vouch` interval starts after the switch's entry, so it
    answers `skipped` for every entry at or before the switch and `yes` only for entries it checks
    after it, and from the switch on option 3's guarantees hold for every install, the switch's own
    residual aside: a whole fetch from such a member carries that residual onward. In
    `vouch_model.py --switch grounded --plant0`, with a plant the samples missed in the image at
    the switch, no member answers `yes` for an entry at or before the switch, and no grounded
    member holds a plant made after it, over every reachable state with two entries
    (176,827 states) and to depth 12 with three (1,297,001); without the missed plant every
    property holds whole (176,827 states with two entries); and under revision 6's catch-up rule it loses again
    (5 steps: a plant made after the switch, grounded through a catch-up on the leader's image). The alternatives are the owner's, in Q9: keep the images unvouched, which loses
    the whole-image path for good (b), or re-index from the client's source for option 3's full
    guarantee over the content that predates the switch (c).
  - **A new member and a repair** have none. The receiver fetches the whole image from a grounded
    member that is not the leader whenever one is reachable (D17's `Request` names the source): that
    member's latest checkpoint, which holds only images whose checks passed (D8's ceiling). A member
    whose own image is unvouched serves no file of that group, whole or single (DMS-17), so a source
    that is not the leader is always grounded. The receiver runs check 5 on every segment itself
    (with the whole-image alphabet rule above), which needs no log, and its image is grounded.
  - **`Vouch`, per entry (DM-R34, DMS-15, DMS-17).** A receiver whose image came from the leader
    alone asks members other than the leader to vouch for the `Image` entry of its whole install and,
    once it has installed later entries with checks 1 to 5 passing, for its current one, whose files
    it holds byte for byte (D17's identity rule). A member answers, echoing the entry's index and
    term:
    1. **`yes`** only for an entry it checked itself from a grounded image: installed with checks 1
       to 5 passing at that entry, or compared whole against its own image there. It keeps one
       interval of such entries per group (16 bytes), which grows with each checked install and
       restarts at a whole comparison (at that entry) or a whole install or vouch (after it), so an
       entry it checked before a catch-up answers `skipped`, a conservative loss, never a false
       `yes`.
    2. **`not yet`** when its checks have not reached the entry, or its own image is unvouched, so
       two members that each installed the leader's image alone never vouch for each other
       (DM-R34).
    3. **`skipped`** for an entry below its interval that it never checked itself (D8's catch-up
       skipped it, it joined after it, or a switch to `verify: install` grounded it after it),
       which the receiver treats as `not yet` and asks other
       members about (DMS-15): a position past the entry is not a check of it.
    4. **`no`** when its checks failed at or before the entry, or its log holds another term at that
       index.

    The receiver refuses an answer whose echoed entry differs from its outstanding ask. A `yes`
    grounds its image, and it vouches only for entries it checks after that. A `no` is counted per
    sender (`vouch_no{member}`) and logged at error level, and the receiver re-fetches the whole image
    from a grounded member that is neither the leader nor the `no`'s sender, when one holds the entry
    or a later one, or else applies the batches itself where its log reaches, or else stays
    unvouched and keeps asking; it never re-fetches from the sender, so a lying voter's `no` costs
    one re-fetch from a third member per `recovery_timeout`, and a member that answers `no` where
    others answer `yes` stands out in its counter.
  - **The residual, exactly.** While only the leader can serve and no grounded member has checked
    the entry (every other member unreachable, unvouched, past it through catch-up, or applying the
    batches itself after a failed check), the receiver installs the leader's image after check 5,
    counts `install_unvouched` per install, holds the `unvouched` gauge for the group, logs at
    warning level, serves no file of the group, and each `recovery_timeout` asks again and tries a
    whole fetch from a grounded member that is not the leader. A `yes` for a later entry clears the
    gauge and grounds the image as it then is; the counter stays, and the earlier entry stays
    logged as never vouched, since a member vouches only for an entry it checked itself (security
    C13). Until a `yes`, a compromised leader can have planted content (a posting, a document, a
    stored value, a deletion) in the receiver's image, which it serves, and content the leader
    removes again before any grounded member checks that entry is never detected; it stays counted.
    `bench/durability/model/vouch_model.py` checks the rule, widened in revision 7 as the
    architect's `vouch_model_r6.py` widens it: with a planting leader, two followers and two new
    members that may each enter catch-up and reinstall (each member's image carrying the plants it
    holds, so a whole comparison compares content), new members that join from the leader or a
    grounded member and ask each other or the followers, no `yes` is ever given for an entry whose
    image holds a plant and no grounded member holds planted content, over every reachable state
    with two entries (354,323 states) and to depth 11 with three (1,025,427; the 1.5-million-state
    cap stops depth 12); revision
    6's catch-up rule loses (5 steps, the architect's schedule), and so do revision 4's rule by position, its
    per-entry rule without grounding, and grounding without per-entry answers (in 4, 6, and 6 steps;
    `results/r7/v7-*.out`). The architect's model, unedited, agrees (`arch-vr6-*.out`: revision 6's rule loses in 5 steps, and the fix holds over 206,559 states with two entries and 1,333,398 to depth 12 with three). Under options 1
    and 2 a whole image carries whatever its source holds, with check 5 under option 2 only on its
    sampled segments.
- **The sampled rebuild** (Q9's option 2, the lead's call). Each segment-mode follower rebuilds a
  sample of the leader's flushes from its own log: for a new segment the `Image` names with its
  covered log range, it applies that range in document mode into a scratch buffer, flushes it to a
  scratch segment that is never published, and compares the two segments' keyed sums (above). A
  mismatch is corruption: logged at error level, counted, and the follower stops installing that
  group's images and applies the batches itself from its installed position, becoming the group's
  independent copy until an operator acts.
  - **Samples are each follower's secret (DMS-11).** The keyed decisions above sample every flush
    with probability 1/64 whatever the leader computes. A rule the leader could compute (every 64th
    flush) would let it plant a segment it knows no follower checks.
  - **Deletions and structures ride their flush's sample (DM-R21, DM-R31).** For a sampled range the
    follower also runs the live-docs check and check 5 above on the leader's segment, so a
    leader-side fault in deletions or in a structure a reader consults is sampled as its flush is,
    at the cost of the range's key lookups and one walk, without inversion. A sampled merge runs
    check 5 on its output the same way.
  - **Debt defers a sample, never skips it (DMS-11).** Under debt a sample waits in a queue with its
    segment pinned from reclamation, never delaying an install, and the queue's pinned bytes are at
    most `verify_pinned_bytes` (1 GiB per node, D14). A sample the queue cannot hold is counted
    `verify_dropped`, and one whose log range a checkpoint truncated before it ran is counted
    `verify_lost`; both are logged, so a leader that floods ingest to drown its samples is seen.
  - **Merges are checked for merge faults only.** A sampled merge is checked as option 3's check 3
    checks one, every ordinal of its output, dead ones included, against the inputs the follower
    holds (re-running the merge would not do: the leader's output honestly carries the deletions
    that arrived while it ran, DM-R33). The inputs are the leader's segments, so a
    corrupt flush that reaches a merge passes a correct merge, and a missed flush stays missed.
    Rebuilding the merge from the log range its inputs cover (DMS-11's alternative) fails for every
    merge whose inputs predate the last checkpoint, which truncated that range. Option 3 is the
    detection path through merges.
- **Rate and cost.** One in 64 flushes and one in 64 merges per follower by default (live, D15), at
  the pool's background class. Its cost, about a 64th of a document-mode follower's work per
  document plus two keyed walks per sampled flush, is in D8's table (+0.01× of a group's
  document-mode CPU); the deletion check's key lookups are unmeasured and small beside the rebuild
  it rides (an estimate).
- **What option 2 catches, and what it leaves (DM-R21, DM-R25, DM-R31).** Per corrupt flush, honest
  or planted, in its records or in a structure a reader consults, with the keyed comparison and
  check 5: detected with probability 1 − (63/64)^(rf − 1), 3.1% at rf = 3
  and 6.1% at rf = 5, so missed 96.9% and 93.9% of the time (`arith.out`). Without the key a planted
  flush passes every sample, so the figure for one is 0 (DM-R25). A missed flush is missed for good:
  once merged, the merge check passes; the digest comparison is equal by construction in an
  all-segment-mode group; and T4's end-of-run rebuild is a test oracle, not a production check. A
  systematic fault shows within tens to a hundred flushes: after 64 corrupt flushes it is still
  unseen with probability 13.3% at rf = 3 and 1.8% at rf = 5, after 128 with 1.8% and 0.03%. Against
  a leader that drives the debt or computes the rule, revision 1's sample fell to 0.3% at a skip
  rate of 0.9 and to 0 (`d18_residual.out`); the keyed decisions and deferral hold the honest figure
  only while the queue holds the sample and its log range is retained: past `verify_pinned_bytes`,
  or once a checkpoint truncates the range, samples drop, counted and logged (DM-R27 item 7). It
  detects; it does not repair, and strata's plan holds no backup: PLAN's recovery tooling rebuilds
  derived structures from stored records and restores "from elsewhere: a replica, the journal, or a
  checkpoint" (`PLAN.md:663-680`), and in an all-segment-mode group each of those holds the same
  bytes. So repair is re-indexing from the client's source, or the tooling's rebuild from stored
  records when only derived structures (postings, the term dictionary, norms) are wrong.
- **A document-mode member** (Q9's option 1) builds its own segments, so it catches an honest
  leader-side fault in the records the digest covers and is the repair source; it serves correct
  queries itself. A fault in a structure a reader consults (the dictionaries' indexes, the key filter,
  norms, statistics, skip data) reaches the segment-mode followers' queries undetected, since the
  digest does not cover it (DM-R31). Against a leader that forges, it keeps a correct copy but
  detects nothing: the comparison between its F21 digest and a segment-mode follower's is one where
  the leader writes the follower's bytes, so a planted segment matches (DM-R25).
- **Per index, `verify`** (a replicated index setting under the administrative privilege, D15):
  `install` is the keyed install checks, `sample` the sampled rebuild, and `replica` (Q9's option 1)
  keeps one voter in document mode, an independent copy and repair source, through the mixed-version
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
| A compromised member serving files | Serve forged bytes to a receiver | Every file, whatever its source, must match its size, CRC-32C, and SHA-256 in the list, and an `Image`'s list must equal the committed entry's, before the rename (D17) | None for `Image` files; a whole-image install is checked as an install is today (`index.hh:2495-2560`), plus D18's check 5 on every segment and, under option 3, a source or a vouch from a grounded member that is not the leader (DM-R31): a member whose own image is unvouched serves no file of the group and vouches for nothing, and a member vouches only for an entry it checked itself (DM-R34, DMS-15). A false `yes` from a compromised voter vouches for what an honest leader built, so it gains nothing without a compromised leader; a false `no` costs the receiver one re-fetch from a third member per `recovery_timeout`, never from the liar, and counts against the sender (`vouch_no{member}`, DMS-17) |
| A compromised leader | Commit an `Image` naming crafted files; give a planted segment the honest F21 digest (DM-R25); send false `commit_durable`, `sync_through`, `recovery_term` | Followers refuse to replace anything at or below the `commit_durable` they learned, which only grows and is their persisted commit index (D7); the install's decoders bound every length before allocating and are fuzzed with the network as the source (T15); D18's verifier compares keyed sums under each follower's own key, never F21 digests | Every segment-mode follower installs the same bytes, so a crafted file fences them all, and a poisoned image outlives the leader's removal unless a follower detects it. Under Q9's option 3 (keyed install checks) a planted flush, merge, deletion, metadata entry, dead ordinal, or structure a reader consults (a dictionary index hiding or adding a term or key, a key filter hiding a key across a replace, norms, statistics, a term's value with its whole-list pairs and `cf`, skip data, competitive pairs, threshold seeds) is detected at its first install and its detector becomes the group's independent copy when its own image was grounded (DM-R31, DM-R33, DMS-16, DM-R40); the leader still chooses which of a merge's deleted input documents its output keeps, which moves ranking statistics, never above the inputs' and hiding no document from a match (D18's check 3); a whole image installed by a new member or a repair is checked for its structures, and for its content only when a grounded member that is not the leader serves it or vouches for that entry, having checked it itself: until then it installs counted `install_unvouched` and can carry planted content, which the leader may remove again before any member checks it, undetected (DM-R34, DMS-15), and a catch-up on such an image grounds nothing, however its whole comparison ends, since the plant is on both sides (DM-R40); a mode switch's or a catch-up's reinstall is compared whole against the follower's own image at the `Image`'s position, every dead ordinal matched or refused (DM-R37, DM-R42); a group that switches to option 3 from sampling counts its followers' images grounded at the switch, so a plant the samples missed before it stays undetected and counted (`grounded_by_switch`), and no member vouches for an entry at or before the switch (DM-R41). Check 3's and the whole comparison's tables are bounded by what the follower already holds before they are allocated (DMS-21). A switch, or a leader that proposes no `Image`, pins every follower's checkpoint ceiling while ingest continues; past the group's retained-log share each follower leaves segment mode for the group, counted (`segment_mode_left`), so the retained log stays within the share and the node pool every group shares is never at risk (DMS-22); and a switch costs a follower at most one whole comparison per `switch_min_interval` by its own clock, whatever the leader proposes (DMS-23). Every check compares segments with the log, which the leader writes: a document it alters before proposing it is what the log implies, beyond every option (REPLICATION.md:8-12, "nodes are not Byzantine"). Under option 2 a planted flush, its records or its structures, is caught with probability 3.1% at rf = 3 and 6.1% at rf = 5, and never after its first merge, since each follower's samples and comparisons are keyed and debt defers them (DMS-11); with F21's unkeyed digest the figure would be 0. Under option 1 alone the document-mode member keeps a correct copy but detects nothing, its F21 comparison with a segment-mode follower being forgeable, and the segment-mode followers' queries read whatever structures the leader wrote. A leader that floods ingest past the sample queue makes samples drop, counted and logged (`verify_dropped`); under option 3 it buys followers document-mode work instead. Barrier storms through `sync_through` on followers' devices, as a compromised leader can already stall its group |
| A compromised voter lying in replies | Claim `synced` above what it synced; claim `forgetful` | Nothing in consensus: a voter that lies about durability breaks Raft's assumption | A false `synced` can make `commit_durable`, and so a `quorum` acknowledgement, count a copy that is not on disk. A false `forgetful` costs the leader a rewind no deeper than the leader's own floor, one unsynced window, at most once per restart and once per `recovery_timeout` per peer (D5, DMS-13; under `aggressive`, one window per election timeout). In a vote or pre-vote it completes D6's count with f honest forgetful voters at once, at any membership (DM-R27 item 1: in a window it also votes for the leader that lets an earlier honest answerer re-learn, `SIX_LIAR_LOSS`), turning today's fail-closed conflict into a recovery that rolls `memory` writes above `commit_durable` back at honest replicas; D6 makes it loud (logs and counters), not impossible. That loss needs `recovery_timeout` to pass with the write's holders unreachable and never happens when every voter must answer, since the holders refuse (the security re-check's `LIAR`, 10 steps at rf = 3 with one honest OS crash, blocked when every voter must answer), so `manual`, which decides only with every voter's answer (DM-R26), resists a compromised voter at the price of availability (C12). Both are the compromised-voter row (`peer-transport.md:213`) |
| A member offering wire version 2 | Push its sessions back a version | D7's per-session rule: it receives `commit_durable` in `commit`, replies after its barrier, and its grants are never forgetful ones | Its own memory acknowledgements are slower; the group's are not (DMS-6) |
| A client with valid credentials | Session tokens; per-index settings; `quorum` writes on a shared core; the `manual` command | Tokens above the known last index refused at once; settings and `acks` behind the administrative privilege; `manual` only as a signed admin statement (`peer-transport.md` D17) (DMS-9) | A stream of `quorum` writes drives a core's barrier rate, today's cost for every write. A document whose part shape made check 1's two record builds differ would take every segment-mode follower of its group out of segment mode at once, loud and reversible; T4's honest control over every part shape is the gate that no such shape exists, and `verify_failed{check}` tells it from a plant (DMS-19) |
| Local access on a node | Fake a boot identity or a clean-close record | Whoever writes a node's journal is that node (the peer transport's local-file row); an unreadable boot identity, or an open record that reads neither whole nor through a copy, reads as unclean, never as the same boot (D3, DM-R24), and an unknown identity is reported as state and logged at each open (DM-R46) | A false clean close keeps a voter that lost entries in elections, as a deleted journal would. A platform with no boot identity makes every restart unclean, which the host reports instead of hiding it |

## The fault model per preset

The model (D2): at most f voters lose their OS or power or are forgetful at once, a forgetful
voter counting until it re-learns, across restarts; while two memberships are in force, the least
f of them (the same f in a replacement window; the smaller in growth or shrink through an even
membership).

| Failure | `strict` | `replicated` | `relaxed` |
|---|---|---|---|
| Process crashes, any number, kernels alive | Nothing lost | Nothing lost (D3's same-boot rewrite; a torn tail makes the voter forgetful, and a forgetful voter stays so across later restarts until it re-learns, D5's mark; a crash in a long epoch reads as the same boot, its open record copied forward, DM-R24) | Writes on the leader alone, if another voter wins the next election |
| OS crash or power loss with at most f voters forgetful at once | Nothing lost | Nothing lost (D5, D6: no recovery election wins within the model) | The leader's unreplicated writes, if it is among them |
| More than f voters forgetful at once | Nothing lost, but Track C D11's stated limit (damage in the newest turns) | `memory` writes in the capped window (D2) that no recovery majority held (D6); never `quorum` writes | As `replicated`, plus the leader's unreplicated writes; under `aggressive` the recovery runs after one election timeout |
| One forgetful voter and one unreachable voter at rf = 3 | Elects | Elections stop until either returns or the forgetful one re-learns (D6's price) | The same |
| Exactly f + 1 voters forgetful at once in a replacement window, every candidate holding the even membership (six voters at rf = 5, four at rf = 3) | Elects (no voter is forgetful) | Elections stop until an operator's `manual` statement admits f + 1 and every voter of that membership answers: no normal quorum exists, D6's decision needs f + 2 flagged answers there (DM-R20), and the admission never decides on a timeout (DM-R26). No time bound; with a voter of that membership gone for good, nothing in this design ends it (D6). `recovery_stalled` and an error log name it. At rf = 3 the stall is the price of one rule for every n, not of safety (Q10's option 4 trades it back) | The same |
| Disk loss of at most f voters | Nothing lost: a new incarnation re-replicates (`track-c.md:321-323`) | The same | The leader's unreplicated writes, if its disk |
| A partition | The minority side refuses writes; `prefix` and `bounded` reads on any side (`bounded` up to T plus `election_max` stale on a minority side); `linearizable` on the majority side | The same | A quiescent old leader acknowledges until CheckQuorum steps it down, at most `election_max` from the write that wakes it (`track-c.md:357-360`); those writes are truncated at the heal |
| A leadership change with no failure | Nothing lost | Nothing lost | Writes on the old leader alone |
| A leader-side fault (an apply bug, nondeterminism, memory corruption before checksumming; REPLICATION.md:19), or a planted segment | Caught by the digest comparison across independent replicas; a planted segment is not shipped | Reaches every segment-mode follower. Under Q9's option 3, keyed install checks, any such fault in a flush, a merge, a deletion, the image's metadata, a dead ordinal, or a structure a reader consults is detected at its first install, honest or planted, and the detecting follower becomes the repair source; which deleted input documents a merge keeps is the leader's choice and moves ranking statistics only within the inputs' (D18's check 3); a whole image a new member or a repair installs from the leader alone is checked for its structures and counted `install_unvouched` until a grounded member that checked its entry itself vouches for it, and a catch-up on such an image grounds nothing (D18, DM-R31, DM-R33, DM-R34, DM-R40), while a mode switch's or a catch-up's reinstall is compared whole against the follower's own image at the `Image`'s position, every dead ordinal matched or refused (DM-R37, DM-R42); after a switch to option 3 from sampling, a fault the samples missed before it stays, counted (`grounded_by_switch`, DM-R41). Under option 2, the sampled rebuild, one corrupt flush is detected with probability 3.1% at rf = 3 and 6.1% at rf = 5 and never after it is merged, a systematic fault within tens to a hundred flushes. Under option 1, honest faults through the digest comparison, planted ones not at all (DM-R25). With no document-mode member and no detection, repair is re-indexing from the client's source: strata's plan holds no backup (D18) | As `replicated` |

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
| `REPLICATION.md:8-24` | The guarantee gains `memory`'s line and the model statement (D2: the least f of the memberships in force); the outside-the-model list gains "more than f voters forgetful at once" for `memory` writes, and segment mode's leader-side residual under the index's `verify`, with its figures and no backup, and the boundary that its checks verify segments against the log, which the leader writes (D18, DM-R11, DM-R21, DM-R25) |
| `REPLICATION.md:64-66` | The digest comparison is independent only between replicas that apply, and forgeable by a member that writes another's bytes (DM-R25); segment mode's checks are D18's keyed comparisons |
| `REPLICATION.md:68-86` | The write path shows the two cursors (D7) and segment mode beside document mode (D8) |
| `REPLICATION.md:117-149` | The acknowledgement and read tables become D10's, with DM-R13's restatements: `leader`'s condition, `bounded`'s minority-side bound, `session`'s term history and refusal of tokens above the known last index; `local` retires and `nearest` becomes `prefix` (Q6); tokens gain the term |
| `REPLICATION.md:151-158` | Cost: the bounded barrier and the memory window's cap (D3, D14) |
| `REPLICATION.md:160-172` | Xapiand mapping: `DB_SYNCHRONOUS_WAL` is `memory` at rf = 1; the async WAL is `none` at rf = 1; `?commit` is `refresh=true` |
| `REPLICATION.md:183-189` | Track C D11's limit widened to the turns after the last recorded synced turn (D3), and closed for `written` groups by D5 |
| `DISTRIBUTION.md:111` | "Replaced by quorum-acknowledged journals" becomes "by journals acknowledged at the index's level (D10)" |
| `PLAN.md:555-556` | Phase 3R lists document-mode replication; segment mode, `memory`, D17, and D18 join its scope, or follow it (the lead's call) |
| `track-c.md` D3 (`:253-286`) | A recovery election in a replacement window counts a majority of the candidate's membership flagged forgetful, and exactly f + 1 forgetful voters there stall until `manual`, which decides only with every voter's answer (D6, DM-R20, DM-R26); growth and shrink through an even membership run at the least f in force (D2); if joint consensus is built (`:282-284`), D6's decision needs a majority of each configuration flagged, and its argument and model are redone before that change lands (C22) |
| `track-c.md` D5 (`:301-327`) | Forgetful voters keep their incarnation; losing durable hard state or the log still means a new incarnation |
| `track-c.md` D10 (`:419-456`) | `Repair{promotion}` (D8); D6's truncation path and repair; repair's files through D17's `Request` |
| `track-c.md` D11 (`:460-625`), conditions 7 and 24, T9 | One write in flight; effects at the barrier; the end-of-turn S; `RFTW0002`; the clean-close record; same-boot recovery; the verdict's epoch V in the open record and the forgetful mark in the hard-state and group checkpoint records (D3, D4); the newest hard state by per-group sequence, the first-turn marks, and the open record as a live record copied forward (`0x13`), its segment kept until a copy is synced (DM-R24); no forward slice in an epoch until its last mark turn's write returns (DM-R29); recovery's per-record epochs, verdict, and V in `TurnRecovery`; T9's rows |
| `track-c.md` D12 (`:627-667`) | The records' new fields; quiescence on `commit_durable` (D7) |
| `track-c.md` D13 (`:669-674`), D18 (`:775-797`) | File sets (D17); `transfer_slot_bytes`, `shipping_bytes`; the outgoing slot rule per destination node with SR22's C16; a segment-mode follower whose checkpoint ceiling is pinned past its group's share leaves segment mode for the group instead of taking the forced checkpoint, which would truncate a whole comparison's log range (D8, DMS-22) |
| `track-c.md` D15 (`:730-731`) | "`matched` never moves back" gains its exception: a reply flagged `forgetful` lowers it to its floor, once per peer restart (D5) |
| `track-c.md` format table (`:182-200`) | D16's rows |
| `peer-transport.md` D9 (`:587-672`) | Version 3's kinds 1, 2, 3, 4, 6, 9, 10, 16, and 17 at D16's sizes; revision 0's host kind ranges withdrawn |
| `simulator.md` D5, D6, D9 | A process crash that keeps the page cache, apart from an OS crash or power loss that drops unsynced writes, with the boot identity; process restarts that keep the forgetful mark, and torn same-boot tails; restarts after a copy-forward and after the open record's first segment is reclaimed, where its journal fault model reaches them (DM-R24); membership changes through Track C's windows, with the candidate counting under its whole log's membership (D4's barrier, DM-R28); the model statement (D2); D6's recovery election with the majority count, and `manual`'s with every voter's answer; the checker's `memory` mode is definite within the model and unknown beyond it, `leader` unknown, and a compromised voter's rollback (`SIX_LIAR`) counted outside the model; T3's forced schedules, `restart3`, `torn3`, `six`, `six_loss`, `grow67`, `grow67_loss`, `six_heard`, `grow67_heard`, `lead5`, `lead5_loss`, and `heard5l` among them (C10, C19, C24) |
| `limits.md` | Rows for `transfer_list_bytes` (D17) and `verify_pinned_bytes` (D18), each with its mechanism. `limits.md` has no manifest or segment-count row, so the first states what 8 MiB admits: about 49,600 segments with a live-docs file each at typical manifest entries, 76,900 without, and 35,500 at the widest LEB128 encodings (`arith.out`, from `files.hh:256-264`'s encoding; DM-R27 item 6); an index past it cannot be transferred, repaired, or shipped. Rows for `term_history_entries` (64 per group, the mechanism the oldest session token a client may resume, so a cap in leadership changes, D10, DM-R44) and `switch_min_interval` (10 minutes, the mechanism a follower's whole comparisons per flip, D15, DMS-23); none of Lucene, Tantivy, or Xapian has either to compare with |
| `small-indexes.md` D3 | As the section above |
| `format-13.md` | No byte owed: its reservations are done (D16), the tags arrive with their releases, and D18's keyed comparison stores nothing (DM-R25). One rule for its section table (`format.hh:166-188`): a section kind or structure bit that segment mode writes gains its D18 check 5 in the release that first writes it, or an installed segment carrying it fails its install check (DM-R31) |
| Kronuz/termdict | Its README states that the builder's output is a function of its entries and alphabet (the cut rule, `termdict.hh:125`; the exact shared prefix, `:102`; the index of first keys, `:172`), which D18's check 5 relies on: a change to any of them changes every dictionary's bytes, so it ships with a segment format version, never alone, and check 5 re-encodes with the rules of the segment's version (DM-R31) |
| Kronuz/hashes | SipHash-2-4 beside `xxh64`, additive, tested against the reference vectors; its README's "none of these hashes is cryptographic" gains the exception (D18) |
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
   unchanged), completion's boot identity and directory barrier, journal's `Unsupported` (a
   `Corruption` subclass, so existing handlers catch it) and `TurnRecovery`'s per-record epochs,
   verdict, and V (fields Detent may ignore), and Kronuz/hashes' SipHash-2-4.
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
| Aumasson and Bernstein, "SipHash: a fast short-input PRF" (2012, the paper's document of 2012-09-18) | A PRF with a 128-bit key and a 64-bit output built for short inputs: 141 cycles for 16 bytes and 234 for 64 on an AMD FX-8150 (its Table 6.1), 1.875 cycles per byte at best on a core with two 64-bit units (section 6); no secret-dependent branches or addresses; a program "reads a secret SipHash key from the operating system's cryptographic random-number generator" on startup (section 7) | D18's keyed comparisons and sampling decisions, and their cost estimate (`records.py`) |
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
| T1 Syncs per tiny index | Under D1, creating an index and acknowledging its first write issue no sync of their own on the acknowledgement path: the write waits for its turn's barrier, at most one device flush per core per turn whatever the group count; a checkpoint costs at most 4 durability calls per index and, on macOS, at most 4 `F_FULLFSYNC`s per device per batch. Today, at strata `main` `3c86484`: 4 durability calls for a create and 8 with its first commit, by `main`'s own test (`test/directory_test.cc:650-667`; read, not measured here), where T1 measured 10 calls and 10 device flushes on macOS at `0b407c9` (DM-R39); step 7 measures `main` | `bench/durability/count_syncs` with its counting IO at 1, 10, and 1,000 indexes; syscalls by `syncspy` on macOS, `strace -c -f -e trace=fsync,fdatasync,sync_file_range` on Linux, `truss -c` on FreeBSD. Counts, no hold |
| T2 Segment-mode CPU | At 100,000 documents of the 1M corpus's first lines on one thread, at the same cadence in both modes (the default one-second refresh, and T2's two buffer configurations), with SHA-256 through OpenSSL's EVP on the leader and every follower and the bytes over loopback TLS 1.3, and D18's verifier as the owner's answer to Q9 sets the default (DM-R45). Under option 3, keyed install checks on every follower (Q9's recommendation): a three-voter group's CPU per document at most 0.70× of document mode's and below option 1's on the same run, a five-voter group's at most 0.62×, a segment-mode follower's at most 0.40× a document-mode follower's (0.04 above the estimate's top, where option 2's targets sit 0.01 to 0.02 above, since option 3's estimate rests on two unmeasured figures, Q9). Under option 2, the sampled rebuild at its default rate (the lead's call until the owner answers): at most 0.45×, 0.33×, and 0.12×. Both options run and are reported on every trial whichever is gated. Reported beside, not gated: one large index with segment mode at the 16 MiB cadence against document mode at one flush; EVP's rate on each platform, measured before the targets are judged (D8's margin, DM-R22 item 5); SipHash-2-4's cost per record and per pass over a document's records, each segment walk through the dictionaries' cursors with its record building (`digest_record`'s string per record), and D18's check 5 per installed segment (the two dictionaries re-encoded and compared, the filter probes, norms, statistics, each term's value rebuilt from the walk, and both skip levels with their competitive pairs) and check 3 over every ordinal of a merge output, dead ones included, on each platform, so that the walk's cost per document and pass can be set against Q9's crossover (DM-R32 item 6); and Q9's options priced on the same runs, so the owner chooses with their figures (DM-R21, DM-R25, DM-R31): option 1, one document-mode follower, beside the two above. This design's estimates: option 2's 0.38× to 0.43×, 0.25× to 0.32×, 0.05× to 0.11×; option 3's group 0.43× to 0.66× at rf = 3 and 0.31× to 0.58× at rf = 5, option 1's 0.68× to 0.72× and 0.43× to 0.48×; option 3 stays below option 1 at rf = 3 while the walk, decode and check 5 together, costs under 4.5 µs per document and pass at one flush's slow end (`arith.out`) | `bench/durability/sizing`'s phases driven through strata::group's real apply, `Image`, D17's transfer over loopback, and install paths, from the leader and from a peer; EVP on x86_64 with and without SHA extensions and on arm64; SipHash-2-4 from Kronuz/hashes; five interleaved trials under a light hold, as BENCHMARKS' rules state |
| T3 The `memory` window | Within the model (D2): zero `memory`, `quorum`, or `all` writes lost. Beyond it: zero `quorum` or `all` writes lost, and every lost `memory` write in the capped window (D2) before the loss. The beyond profile must report loss, or its oracle is blind. Forced schedules, each with no loss under D6 as revised: DM-R1's trace (`dmr1`); the restarts DM-R19 found, `restart3` and `torn3`, under the persisted mark; the window failures DM-R20 and the re-checks found, `six`, `six_loss`, `grow67`, and `grow67_loss`, under the majority count, and `six_heard` and `grow67_heard` under `manual`'s f + 1 admission with every voter answering; the five-voter losses of the count since the restart under every barrier D4 imposes, `lead5`, `lead5_loss`, and `heard5l` (DM-R30; `bench/durability/model/schedules/`); and `live3`, where the one voter that holds the write is not forgetful, elects it. `fplus5`, `heard5`, `fplus5b`, `grow6`, and the shrink fence are dropped: each needs an OS-crash loss or an election that D4's barriers rule out (DM-R28), and `lead5` and `heard5l` stand in for the first three. The profiles include same-boot process restarts that keep the mark, torn tails, a candidate counting under its whole log's membership, and Track C's windows at rf = 3, rf = 5, and from rf = 5 to 7 (C10, C19) | The simulator, tier C, with D5's fault classes: 10^7 steps over 10,000 seeds per profile, at 3 and 5 voters and through Track C's windows at 4, 6, and 6 to 7 voters, in both modes; the checker's `memory` mode; the forced schedules as scripted runs |
| T4 No divergence | After every schedule, D6's recoveries included, each replica's digest equals the leader's at equal applied positions, and a replica whose applied index passed a divergence point repaired before serving a read; a D6 recovery after checkpoints taken under sustained ingest diverges no replica (DM-R3). In segment mode the oracle is not the leader's bytes: D18's verifier catches a seeded leader-side fault (a mutant flush that drops one posting per segment) within the run; a forging mutant leader, which plants `digest_forge.py`'s compensating posting so the planted segment's F21 digest equals the honest one, is caught by the keyed comparison at the first install under option 3 and within the run under option 2, and an unkeyed comparison, run as a control, misses it (DM-R25); a mutant leader that corrupts one flush in 64 of its choosing under sustained ingest is caught within the run, which carries at least 439 corrupt flushes at rf = 3 and 220 at rf = 5, so every one is missed with probability below 10⁻⁶ under keyed sampling (`arith.out`; DMS-11); a deletion fault, an old version left live after a replace, and a merge fault are caught by option 3 at their first install and by option 2's samples; so are a mutant leader that plants a term index hiding one term from `find` while the scan sees it (the architect's `term_index_plant.cc` in a segment), one that plants a key filter hiding one key across a replace, one that plants a norm, a statistic, a skip entry, or a competitive pair, and one that changes the image's metadata, each at its first install under option 3 (DM-R31); a new member's whole image from a member that is not the leader installs vouched, and from the leader alone counts `install_unvouched` until another member vouches, and a planted record in it is refused at the vouch; under option 3, each at its first install (DM-R33, DMS-16): a mutant leader that adds dead ordinals to a merge output (`dead_stats_plant.cc`'s shape: three planted dead documents moving "alpha OR beta"'s best match out of the top three) and one that alters a carried dead document's postings, while honest merges carrying any subset of their deleted inputs pass; a mutant leader that lowers a term's whole-list competitive pairs (MaxScore then skipping a document a scan sees), plants its `cf`, its `counts_are_wdf` or `no_wdf` flag, a level-1 group's pairs or last ordinal, or a level-0 entry's pairs or offsets (`term_value_check.cc`'s plants in a segment), and a term whose postings or positions length runs past the bytes the walk consumed (DM-R38); a catch-up reinstall and a mode switch whose leader image carries dead ordinals the follower holds, or holds in its log, pass, and a planted one fails (D8, D18, DM-R37, DM-R38): a switch with deletes before the command and deletes of live documents after it, the leader expunging first, passes with no dead ordinal of a document deleted before the command in its first `Image`; a leader whose first `Image` after the switch carries a phantom dead ordinal (a length of 2³² − 1 naming no document, `dead_stats_plant.cc`'s shape) and one carrying a document deleted before the command, which the follower's earlier merges dropped, each fails the comparison, and neither grounds the image nor draws a `yes`; the mutants are a follower that merges the segments it held at the command (an honest switch then fails), a leader that skips the expunge (the same), and a catch-up follower whose checkpoint ceiling is lifted so its log range is truncated (an honest reinstall then fails); the switch's strict readings (DM-R42; `switch_model.py`'s schedules ported, each with its counterexample as the forced case): a document buffered at the command and replaced after it, with the follower's ceiling at its last refresh before the command, passes, and the ceiling read as "at the command" (a refresh at the command's index checkpointed, then that flush merged) fails it, the mutant; a merge in flight on the follower at the command, its output frozen, passes, and its output merged after the command fails it, the mutant; a comparison at an `Image` position p with the follower applied to a > p, deletes and replaces in (p, a], passes, and after the install the follower's visible position never moves back and (p, a] is applied again; a merge on the leader that straddles the command, its output expunged, passes, and a leader that expunges only the segments it held at the command fails every follower, the mutant (DMS-24 item 2); a failover between the command and the first `Image`, the new leader owing the expunge; a catch-up from an unvouched image (DM-R40; `vouch_model_r6.py`'s 5-step schedule: the leader plants entry 1, a new member joins with the leader's image at 1 while both followers are behind it, the leader appends entry 2, the new member enters catch-up and reinstalls entry 2 whole, equal), where the new member stays unvouched, answers `not yet`, serves no file, and stays in the `unvouched` gauge, and revision 6's grounding list, under which it is grounded, vouches, and serves, is the mutant; a switch to `verify: install` with a flush the samples missed planted before it (DM-R41): every follower grounded by the switch, `grounded_by_switch` counted, no `yes` for any entry at or before the switch, and a plant made after the switch refused at its first install; the mutants are a `Vouch` interval that includes the switch's entry and the images kept unvouched (no member is ever grounded); the term alphabet under one rule (DMS-20, security C18; DM-R36): an honest image from a member and one from the leader alone, each holding a merge of a merge whose dropped documents took a byte, passes, a widened alphabet passes and reads the same in a whole image and in an incremental install of a flush and of a merge, and an alphabet missing a walked byte, a narrowed, an unordered, or a repeated one fails; a merge output whose claimed ordinal count exceeds its inputs' sum, and a whole image whose count exceeds the follower's own plus its log's documents, each refused before any table is allocated, with the follower's peak memory for the check bounded by 8 bytes per ordinal it holds (DMS-21); a leader that proposes a switch and never writes the `Image`, and one that stops proposing `Image`s, under sustained ingest: every follower leaves segment mode at its group's share, `segment_mode_left` counted, every retained log within its share at the end, and the group still serving (DMS-22); switches proposed within `switch_min_interval`: refused at an honest leader naming the floor, and at each follower deferred to one whole comparison per interval when a mutant leader writes them anyway, `mode_switches` counted (DMS-23); plants in main's skip data (a block's codec code other than the writer's, an advance entry's last ordinal or offset, a group's frontier pairs) and a threshold seed above the honest one, each refused at its first install (DM-R39); the vouch schedules, each with a mutant (DM-R34, DMS-15, DMS-17): the security reviewer's `vouch_position.py` schedule (a planted flush on a new member while both followers catch up, the delete, the merge, then a later image both followers reinstall whole), where the new member stays `install_unvouched` and its entry is answered `skipped` under D17's rule and `yes` under revision 4's by position (the mutant); two members that each installed the leader's image alone, which neither vouch for each other nor clear `install_unvouched` (the mutant: a member vouching while its own image is unvouched); an answer whose echoed entry differs from the ask, dropped; a lying voter's `no`, after which the re-fetch goes to a third grounded member and never to the sender; an unvouched member refusing every `Request` of its group; and a follower whose `Request` the leader leaves unserved for one `recovery_timeout` fetching from another member (`vouch_model.py`'s three controls as the mutants); the honest control (DMS-19, security C17): a corpus over every part shape the plugins admit (tokens with terms, `split_text`, `no_positions`, wdf 0, the key as a term, empty and maximal stored values, a keyless index) under an honest leader, and as whole images (from a member and from the leader alone, holding a merge of a merge whose dropped documents took a byte, and through a mode switch), with zero failures in every `verify_failed{check}` as the pass condition; `verify_dropped` and `verify_lost` are reported; and an independent rebuild per group at the end matches every member | The simulator and Track C's T2 oracles for the cases that need only document mode and the digest per replica after each step; every segment-mode case (installs, whole images, vouches, plants in segments, the switch, the release, the alphabet) in strata::group's tests with a mutant leader that writes chosen segments, `Image` entries, and log entries, step 5's harness (DM-R47 item 7); the seeded and forging mutants in both |
| T5 Linearizability per level | Histories of `linearizable` reads and `memory` or stronger writes are linearizable within the model; `leader` and `none` outcomes are unknown; `session` and `bounded` reads meet their necessary conditions; with leases on, under restarts of random voters (DM-R14) | The simulator's checker (D9 of its design) with the `memory` and `leader` modes, in both modes |
| T6 Forgetful voters and recovery | A forgetful voter grants no vote or pre-vote outside a recovery election, acts on no `TimeoutNow`, and campaigns only in a recovery election before it re-learns; a recovery candidate leads only with every answer a grant and a majority of its membership flagged forgetful, and under `manual` only with every voter's answer; each mutant is caught within the seed budget, its first seed recorded: a forgetful vote outside a round; re-learning through `commit_memory` instead of `leader_last`; a leader that ignores `forgetful` and never rewinds; a rewind below the leader's floor, or twice in one restart or in one `recovery_timeout` (DMS-2, DMS-13); a crossing batch answered at `Written` past `max_unsynced` (DM-R27 item 4); a D6 truncation at `commit_durable`; a checkpoint above `commit_durable`; D6 without the f + 1 count (DM-R1); the count taken since the restart instead of at the round's term (`fplus`), which `lead5` kills under every barrier D4 imposes, and revision 0's every-voter-heard rule, which `heard5l` kills (DM-R30); forgetfulness cleared by a restart, which `restart3` and `torn3` kill (DM-R19); the mark cleared in a batch before the re-learned entries' (DM-R19); the first-turn mark omitted, written after another turn of the epoch, or decided at a group's deferred replay, and the newest hard state taken by journal order, which a copy-forward then a clean close kill (DM-R24, `mark_model.py`'s controls); a reclamation slice in a mark turn before every group's mark is written, which two groups whose marks take two turns kill with a process crash or a clean close between them (DM-R29, `mark_groups.py --ride any`); f + 1 flagged answers deciding at an even membership, which `six` and `grow67` kill (DM-R20); `manual`'s admission deciding on the timeout, which `six` kills (DM-R26); a candidate counting under its synced prefix's membership instead of its whole log's (DM-R28); a voter that flags `forgetful` on a grant it would refuse, whose rollback the checker counts as a compromised-voter outcome outside the model, never as a pass (DMS-3); an install while forgetful (`InstallPrepared`, `core.h:492-500`) that persists its hard state without the mark, killed by a restart before re-learning, which must then recover forgetful (C38; the install path starts from `durable_`, `core.h:497`, and publishes it, `:454`, so the mark rides it by construction, and `Repair` persists no hard state, `:597-609`) | consensus's `simulation_test` with the forgetful fault class; `tools/mutants.sh` |
| T7 Process restarts | Under one boot identity, 1,000 `SIGKILL`s of random voters at random points under writes, in epochs long enough that the open record's first segment is reclaimed (DM-R24): every write acknowledged at `memory` or stronger survives (DM-R13), and a voter restarts forgetful exactly when its tail was torn after S or it was forgetful when it stopped (DM-R19), which holds only because the open record stays readable for its epoch's life (`mark_model.py`'s precision check); a forgetful voter killed again before it re-learns restarts forgetful every time, and one that re-learned restarts not forgetful | Three processes on one host, the kill loop and a checker of acknowledged writes, a journal small enough (`segment_bytes`, `retain_segments`) that reclamation cycles within the run, replacing `harness/wal_recovery_check.sh` (PLAN's Phase 7 gate) |
| T8 Failover gap | Killing the leader (process and power) at sustained ingest: the next acknowledged write at the new leader within D9's total at p99 over 100 kills per preset (2.3 s for `quorum`, 2.2 s for `memory`, plus 1.0 s in segment mode), the replay within `election_min`, also with the leader's uplink saturated and during a document-to-segment switch; the gap at the client reported beside, with strato's timeout. The open after an unclean restart (DM-R32 item 1): from the open record's sync to the core's first admitted turn, with 1,000 and 100,000 forgetful groups per core whose marks D3's rule 2 writes first, at most 1 s at 100,000, and the journal's growth past `retain_segments` while the slices are held reported beside | A three-process loopback cluster, and the simulator with the measured barrier profiles, in both modes; for the open, one node's journal with N groups per core in `written` configurations, reopened with an injected boot identity |
| T9 Journal with turns between barriers | Power lost at every point with any subset of the unsynced turns' 512-byte sectors persisting, with recycled segments (condition 10): recovery keeps exactly the longest whole prefix after the last recorded S; no synced record is lost but within D3's stated limit; damage at or before S costs only the groups named; `RFTW0001` journals recover unchanged. Same boot: a failed barrier with the pages kept clean, and with them dropped, each followed by a reopen: the first rewrites and syncs the tail and is not made forgetful, the second recovers forgetful; a torn tail recovers forgetful. A power loss after a copy-forward turn's write whose source segment would otherwise be reclaimed loses no synced record (DM-R6). A forgetful voter's same-boot restart, and its clean close followed by a reboot (the boot identity injected), each before it re-learns, recover it forgetful; a kill or a power loss at every point of a recovery, its open record and its first-turn marks included, leaves the next recovery forgetful in every group that was, and a group that writes nothing across two later restarts recovers forgetful at both (V); a cleared mark never survives without its re-learned entries (DM-R19). An idle forgetful group's records copied forward, then a clean close and a restart, then a same-boot crash: forgetful at each, and the same for a group whose replay the host defers past the epoch's first turns; a long epoch whose first segment, with the open record, was reclaimed, then a same-boot crash: not forgetful where nothing was lost, and after a power loss forgetful; an open record readable neither whole nor through a copy reads as unclean (DM-R24). Two groups whose marks take two turns, with the journal past `retain_segments` so that slices would be due, the second group's newest hard state in the oldest segment: no turn before the last mark turn's write holds a slice, and a kill and, separately, a clean close between the two mark turns recover the second group forgetful at both (DM-R29); the same with marks filling a segment, so that the marks cross a segment switch. An unknown boot identity (the platform's source masked): recovery unclean, `TurnRecovery`'s reason `identity_unknown`, the host's state set and the warning logged at each open; and a journal directory not known local warns and reports under `nonlocal`'s default (DM-R46). The per-group journal refuses `acks: written` | journal's T9 harness, extended; `mark_model.py`'s controls as mutants (rule (c) alone on the copy's epoch, newest by journal order, the open record left reclaimable, an unreadable boot identity read as this boot's), and `mark_groups.py`'s `--ride any` as one (a slice in a mark turn) |
| T10 Overrides, floors, and `quorum`'s path | Every combination of preset, requested `ack`, and requested `read`: stronger honored, weaker below the floor refused naming the floor, never silently changed. A `quorum` write in a `replicated` index acknowledged after one barrier and a round trip on its path, against two barriers and a round trip in `strict` (DM-R4), counted, not timed: this is no performance row (DM-R47 item 6) | strata::group tests, the barriers and round trips on each acknowledgement's path counted through a counting IO and transport |
| T11 Bounded and session reads | A `bounded(K, T)` read never serves state more than K entries behind the commit it learned within T, and on a minority side never more than T plus `election_max` stale; a `session` read honors read-your-writes and monotonic reads against a segment-mode follower's lag; a token whose entry D6 lost reports `lost`, also below the log's base; a token below the session horizon, after more than `term_history_entries` leadership changes, reports `expired`, and the history never holds more than that many entries (DM-R44); a token above the known last index is refused at once | The simulator's checker; strata::group tests, in both modes |
| T12 Formats and wire | Every row of D16 has its fixture, its previous-reader rejection test, and its value rules' refusals; the kinds' sizes as D16 states; a `Membership` without `acks` reads as `synced`; `RFTBTCH3` and the new bundle magic refused by older readers and read as unsupported by unknown versions, and their forgetful mark refused above 1 (DM-R19); a version-2 session in a `written` group receives `commit_durable` in `commit` and never a commit above it, replies after its barrier, and is counted for both cursors (DMS-6); Detent builds and passes its suite at each additive step's commit | With Track C's T1 and the peer transport's T3 |
| T13 Live tuning | Under sustained ingest, changing `sync_interval`, the window's caps, refresh, checkpoint thresholds, floors, and the verification rate takes effect within one interval, with no restart and the foreground write p99 within 1.5× of steady state across the change; switching `acks` and the replication mode through the log keeps every acknowledged write and the digest, the replay within `election_min` across a mode switch. A document-to-segment switch under sustained ingest, on an index with deletes and replaces before the command (DM-R45): the foreground write p99 within 1.5× of steady state from the command to the last follower's whole install, with the expunge's duration and bytes rewritten, the switch's duration, and each follower's retained log over it reported, the last within its share (DMS-22); a second switch within `switch_min_interval` refused naming the floor (DMS-23) | Three processes under ingest, Detent's file source, in both modes |
| T14 Background under load | 10 minutes of sustained ingest at 1,000 groups per node, and again at 10,000 groups per node with most of them idle (DM-R44: this design's memory per idle group measured against D10's estimate of about 80 bytes and beside small-indexes' idle-heap row; every new gauge a count per node; the retained log per group reported across a mode switch, DMS-22): retained log per group within its share; install lag within D9's bound; unsynced bytes within `max_unsynced`; an idle group's `commit_durable` reaching its last index before it quiesces; with one peer receiving at its full window, another follower's install within 1.5× of the unslowed run (DMS-1); and the foreground write and query p99 within 1.5× of the same ingest with checkpoints, shipping, and verification deferred, on the same build (DM-R18) | With no-stalls' harness, on the gate VM; the deferred run as the baseline |
| T15 File-set transfers | A transfer sends only the files `Missing` names, skipping every file the receiver holds with equal identity, size, CRC-32C, and SHA-256; a file whose bytes differ, from any source, is refused before its rename; a `Request` for an uncommitted `Image` is refused; a chunk for no outstanding transfer drops, counted, and closes no session; a reservation refused before the first chunk is reported; a `Begin` whose list size exceeds `transfer_list_bytes` or falls below its file count × 60, whose file count or total bytes differ from the committed `Image`'s, or whose sizes overflow or do not sum to its total, is refused before any list byte is buffered, counted (DMS-12); the per-receiver and per-node limits hold; the install's decoders (`load_segment` whole and every reader format 13 adds) fuzzed with the network as the source, one CPU-hour per decoder (TESTING.md's row; DMS-4) | consensus tests with an application whose image is a set of immutable chunks (Track C's T4); strata::group tests; the fuzz targets in Phase 3R's gate |
| T16 Checkpoints and creation (SI-R19, SI-R20, SI-R24, SI-R25, DM-R23, DMS-10) | A crash at every point of a checkpoint batch over 3 to 100 groups leaves each index at its last recorded checkpoint or the batch's, and the log holds everything above it; a checkpoint failed at each phase fences that index's checkpoints and restores it at its recorded generation through `Repair`, with no acknowledged write lost and the voter not forgetful; more owners committing at once than the pool has workers all return; a creation record replays over a first checkpoint's residue, also after that checkpoint's last directory sync failed (in the same boot and after a power loss), and a manifest with another UUID refuses the replay, nothing removed; a duplicate `Create` of the same UUID, a client's retry after a lost reply, after a checkpoint record and before one: replay opens at the checkpoint's generation with every document, or applies from the first `Create`, on every replica, in the same boot and after a power loss, and the duplicate applies as `exists` (DMS-10); in every case the oracle is the live state under D1's rule, what the index held before the stop, as the security re-check's `create_replay_r2.py` uses, not `create_replay.py`'s fixed arm, which opens a first checkpoint's residue instead of replaying from the creation (DM-R27 item 9); a checkpoint of a past generation whose inputs a later merge replaced and whose live-docs files it wrote, then the owner's retention and reclaim, then a crash and an open: every file the manifest names exists, and the digest equals that generation's (DM-R23); an incremental install, then a crash before the next checkpoint: the open finds the durable head, not the install (DM-R27 item 5); a crash after a checkpoint completed and its owner's retention ran but before its checkpoint record's barrier returned, with two and with three later checkpoints completed in between: replay opens the recorded generation with every file its manifest names, the oracle the live state at that generation (DM-R43); the mutant removes the recorded generation's keep and sets `retained_predecessors` to 0, and replay then cannot open | Small-indexes' T9 to T11 harness (faultfs at each phase, lifecycle crash points) and its re-check's probes as tests (`owner_waits_on_pool.cc`, `stale_incarnation.cc`, in `strata-wt/review-small-indexes/.scratch/si-review/`), through strata::group once step 5 exists |

## Open questions for the owner

**Decided on 2026-10-19** (`docs/status/durability-modes.md`): Q1, Xapiand's default preset is
`replicated`; Q2, the default `sync_interval` is 100 ms; Q3, `balanced`, with DM-R1's fix: a
`forgetful` flag on pre-vote requests and D6's grants only after f + 1 forgetful voters are heard
(see Q10); Q4, merges shipped now, revisited with copy merges; Q5, `memory` at rf = 1 offered,
superseding rocksdb-survey's Q3; Q6, one vocabulary, `local` retired and `nearest` renamed `prefix`;
Q7, the split, followed; Q8, the version-2 wire does not wait: this design's fields join version 3
through the N and N − 1 range (D16).

**Q9. Segment mode's check (DMS-4), restated with the keyed comparison, what queries read, and
the checks as revision 7 states them (DM-R21, DMS-11, DM-R25, DM-R31, DM-R32 items 4 to 6, DM-R33,
DM-R34, DMS-15, DMS-16, DM-R40, DM-R41, DM-R45; C21, C25, C28, C36).** The lead called option 2 on
revision 1's statement that "a rare single corrupt segment, found later, is rebuilt from the source
or a backup". Nothing finds it later, and no backup exists, so revision 2 returned the choice with
each option's residual. Revision 3 keyed every comparison, since F21's digest is forgeable in O(1)
by whoever writes the segment (DM-R25). The architect's re-check of revision 3 then showed that the
keyed sums cover F21's records, what a scan reads, but not what a query reads: the dictionaries'
indexes, the key filter and range, norms, the statistics, skip data, and competitive pairs, so a
planted term index hid 20 of 200 terms from lookups while every keyed sum stayed equal (DM-R31).
Revision 4 extends option 3's checks to every structure a reader consults (D18's check 5) and to the
image's metadata (check 4), and states what whole-image installs leave. The re-check of revision 4
and the security review of D18 found three more gaps, which revision 5 closes: a merge output's dead
ordinals, which every ranking statistic counts (DM-R33, check 3 now covers every ordinal); a term's
whole-list pairs and `cf`, which re-encoding a value from its own decoding cannot check (DMS-16,
check 5 rebuilds each value from the walk); and `Vouch` answered by position, so a member that
skipped an entry, or whose own image was unvouched, vouched for it (DMS-15, DM-R34: a member now
vouches only for an entry it checked itself from a grounded image). The full design reviews of
revision 6 found two more, which revision 7 closes: a catch-up built on an unvouched image grounded
it by a whole comparison that the leader's plant passed on both sides (DM-R40: grounding now traces
to an independent check), and a group switching to option 3 had no grounding rule, under which,
with DM-R40's fix, no member would ever be grounded again (DM-R41: the follower images count as
grounded at the switch, with the residual stated, below). Each option, with its cost
against document mode's group CPU (estimates at the same cadence over T2's two configurations and
the revision-1 bracket of hashing and transport rates, `arith.out`; T2 measures) and its residual
against an honest fault and a planted one:
1. **A document-mode member per group by default** (`verify: replica`): one follower applies the log
   itself. Cost: 0.68× to 0.72× of document mode at rf = 3 and 0.43× to 0.48× at rf = 5 (segment mode
   with option 2: 0.38× to 0.44× and 0.25× to 0.32×). Honest: none missed in the records the digest
   covers, and a repair source in every group; a fault in a structure a query reads reaches the
   segment-mode followers' queries undetected. Planted: that member keeps a correct copy and answers
   its own queries correctly, but nothing detects the plant, since its F21 comparison with a
   segment-mode follower is forgeable and nothing compares structures.
2. **The sampled rebuild on every segment-mode follower by default** (`verify: sample`, the lead's
   call), keyed, with check 5 on each sampled segment: +0.01× of document mode's CPU. Honest and
   planted alike, in the records or in the structures: one corrupt flush detected with probability
   3.1% at rf = 3 and 6.1% at rf = 5, missed 96.9% and 93.9% of the time, and never after its first
   merge; a systematic fault within tens to a hundred flushes (after 64 corrupt flushes still unseen
   with probability 13.3% at rf = 3, 1.8% at rf = 5); samples past `verify_pinned_bytes` dropped and
   counted. No repair source and no backup: repair is re-indexing from the client's source.
3. **Keyed install checks on every segment-mode follower by default** (`verify: install`, D18): every
   flush against its log range, every live-docs change against the log's deletes, every merge
   against its inputs, the image's metadata against the log, and every structure a reader consults
   against the segment's own walk, under each follower's key, with no state past a check. Cost: 0.43×
   to 0.66× of document mode at rf = 3 and 0.31× to 0.58× at rf = 5; per follower 5.2 to 15.5 µs per
   document at one flush and 11.8 to 35.1 µs at 16 MiB, against a document-mode follower's 42.8 and
   129.3 (SipHash bracketed by its paper's lower bound and 2012 measurements, 2.0 to 5.3 µs per pass;
   each walk's decode, 0.5 to 2 µs, and check 5, 0.2 to 1.0 µs per installed document, assumed).
   Honest and planted alike: none missed in a segment, its records live or dead and its
   structures, each detected at the first install that carries it, and the detecting follower
   becomes the group's repair source when its own image was grounded. Whole images: a mode
   switch's and D8's catch-up reinstall are checked whole against the follower's own image at the
   `Image`'s position, every dead ordinal matched or refused (a switch freezes what the follower
   holds, its ceiling below the command, and the leader expunges first, D8), and ground only a
   follower whose own image was grounded; a new member's or a repair's is checked for its
   structures, and for its content when a grounded member that is not the leader serves it or,
   having checked that entry itself, vouches for it. **A group that switches to option 3** from
   option 2 or 1 (every group created before the owner answers) counts its followers' images
   grounded at the switch, `grounded_by_switch` counted, so what the samples missed before the
   switch stays, as option 2 left it, and no member vouches for an entry at or before the switch
   (DM-R41). The residuals: which of its inputs' deleted documents a merge output keeps, which
   moves ranking statistics, never above the inputs' and hiding no document from a match; a whole
   image installed while no grounded member that checked its entry answers, counted
   `install_unvouched` and logged, which may carry planted content until one does, and whose
   content the leader removes before then is never detected; and, in a group that switched, the
   content from before the switch, as option 2 checked it. Like every option, it verifies segments
   against the log, which a compromised leader writes.
4. **1 with 3**: 0.71× to 0.83× at rf = 3 and 0.48× to 0.69× at rf = 5. Detection at first install
   for both kinds of fault, and a repair source that never installed the leader's bytes. A group
   that switches to it grounds its segment-mode followers as option 3 does; its document-mode
   member is grounded by construction, built from the log.

Option 1 per index and `strict` stay one setting away under each.

**For a group that switches to option 3 or 4 (DM-R41).** Its images installed under sampling were
checked only on their samples, and once checkpoints truncate their log range nothing independent
remains to compare them with. So the choice for existing content is real:
- (a) each follower's image counts as grounded at the switch, counted (`grounded_by_switch`), its
  residual the sampled one the group already ran under, and option 3's guarantees hold for every
  install after it (revision 7's rule, D15, D18);
- (b) the images stay unvouched, so the group's new members and repairs come from the leader alone,
  counted, until each is vouched, which with DM-R40's rule never happens: the whole-image path is
  lost for good (`vouch_model.py --switch unvouched`: no reachable state has a grounded member);
- (c) option 3's full guarantee for the existing content by re-indexing it from the client's source.

Recommendation: (a), which this revision builds: (b) loses the whole-image path and gains nothing
over (a), whose residual is option 2's, and (c) is what an owner who needs the full guarantee does
anyway, for which (a) is no obstacle.

**Where option 3's cost lead ends (DM-R32 item 6).** Option 3's cost rests on two figures nothing
here measured: the segment walk (decode, record building, and check 5) and SipHash's rate. Holding
the others at each end of the bracket, option 3 costs less than option 1 at rf = 3 while the walk
costs under 4.5 µs per document and pass at one flush's slow end (8.1 at its fast end, 8.2 and 11.8
at 16 MiB), 2.3 times the assumed top of 2 µs at the tightest. At rf = 5 it is the other way round at
the slow end of one flush: option 3 costs more than option 1 at any walk cost (by 0.013× with a
free walk), and elsewhere it costs less only while the walk stays under 1.5 to 4.9 µs (`arith.out`,
Revision 4's lines). So T2 measures the walk and check 5 before the owner's answer is final.

Recommendation: 3, restated on revision 7's checks (C28, C30, C33, C36) and the security reviews' residual. It is
the only option that detects a planted segment, in its records, live or dead, or in what a query
reads, which the threat model's compromised-leader row needs now that the digest is known to be
forgeable and the structures known to be uncovered; and it catches every honest fault in a segment
at its first install, where 2 misses one corrupt flush about 97 times in 100 at rf = 3 and for good,
and 1 misses every fault in a structure. Its residual, in one clause: it verifies segments against
the log, which a compromised leader writes, so a document altered before it is proposed is what the
log implies (the Byzantine boundary, REPLICATION.md:8-12); within that boundary it leaves the
leader which deleted documents a merge keeps, a bounded move of ranking statistics, and a new
member's or a repair's whole image no member that checked it has vouched for, counted and logged
until one does; and, in a group that switches to it from sampling, the content from before the
switch as sampling checked it (DM-R41); a mode switch leaves no residual, since the followers freeze
what they hold and keep their log from below the command, and the leader expunges its earlier
deletes before its first `Image`, so every dead ordinal is matched or refused (DM-R37, DM-R42), at
the cost of a heavy switch whose expunge D14 lists and T13 bounds (DM-R45). If the owner takes it,
T2 gates option 3's targets, not option 2's (DM-R45). At rf = 3 it costs less than 1 across the
estimate (0.43× to 0.66× against 0.68× to 0.72×) as long as T2's walk stays under the crossover
above; at rf = 5 the estimates overlap and at one flush's slow end 3 costs more, so if T2 puts 3
above 1 there, 3 is still the one that detects a plant, and the choice at rf = 5 is that detection
against at most about 0.1× of document mode's CPU. 2 stays available per index (`verify: sample`)
for data its owner can re-derive.
D18's keyed comparison, check 5, and the vouch are the threat model's leader row, which the security
reviewer reviewed (APPROVE WITH CONDITIONS, security C13 to C17); the re-check of revision 5 closed
DMS-15, DMS-17, DMS-18, and DMS-19, and DMS-16 for incremental installs (`8b87d1b`); and both full
design reviews of revision 6 ruled on the rest (DMS-20 to DMS-24, DM-R40 to DM-R42), folded in here,
with DM-R40 and DM-R41 routed to the security reviewer's next re-check (C33). Until the owner answers, the
lead's call stands, now keyed and with check 5 on its samples, and step 5 builds 2 and 3 behind
`verify` so T2 can measure both. Neither blocks a first step.

**Q10. D6's correction to Q3's rule (DM-R1, DM-R20, DM-R27 item 12, DM-R28, DM-R30).** The ruled
rule, f + 1 forgetful voters heard since the restart, loses a `memory` write at five voters under
every barrier D4 imposes: the architect's `lead5` (13 steps), where the third forgetful voter counted
is the leader that wrote the write, never synced it, and campaigned before it existed, and `heard5l`
(14 steps), where revision 0's every-voter-heard rule loses the same way (D6, "Why the count since
the restart is not"; `results/r4/schedules-lead5.out`, both checkers, each barrier mode). Revision
1's `fplus5` and `heard5` showed the same failure in a model without D4's barriers; revision 3 found
they do not replay under them and called the question open, which `lead5` answers. At three voters
the ruled rule holds within the model. Revision 1's correction, f + 1 flagged answers counted again
at the recovery election's own term, holds at every fixed membership the model reached but not
across a membership change: in rf = 5's replacement window (`six`) and growing from rf = 5 to 7
(`grow67`) it loses a `memory` write under every barrier (D6), so revision 2 decides on a majority of
the candidate's membership, which is f + 1 at every odd size.
1. Keep the ruled count as what starts a recovery election, and decide on a majority of the
   candidate's membership flagged among that election's answers at its own term (D6).
2. Keep the ruled rule as written and allow memory acknowledgements only at rf = 3, the one size
   where it is not shown to lose.
3. Keep the ruled rule as written and run only `manual` recovery at rf ≥ 5, so that at the sizes
   where it is shown to lose it never decides without an operator.
4. Option 1, with f + 1 instead of the majority at a four-voter membership, rf = 3's window (DM-R27
   item 12): safe there by D6's arithmetic, so that window does not stall with exactly two voters
   forgetful at once; at the price of a second rule with its own argument and tests, for a stall
   that needs two power losses inside a window two commits long.

Recommendation: 1, which this revision follows (the lead's call, with the count corrected): it keeps
the flag on kind 6 and the f + 1 start, adds one flag to kinds 1 and 2 (D16), and decides as
revision 1 did at every odd membership. It holds in the model at every fixed membership checked,
three and five voters with restarts and torn tails, and near Track C's windows with the majority
count, under D4's vote barrier, and it blocks `lead5` and `heard5l`; with f + 1 it does not hold in
rf = 5's window or from rf = 5 to 7 (D6's model evidence, with each run's depth and scope). Its
price is the window stall (D6, the fault table), which at rf = 3 buys one rule for every n rather
than safety; option 4 trades that back. Options 2 and 3 keep a rule shown to lose a write at
rf ≥ 5, confined by giving up memory acknowledgements there (2) or automatic recovery there (3). It
does not block the first steps that may start now.

## First steps

Every step may start now. The full design review of revision 6 approved steps 1, 2, 3, 4, 6, 7, 8,
and 9 to land under their conditions and outside dependencies, and step 5's checkpoints, `Create`,
apply, `Image`, incremental install, catch-up, and levels under C34 and C35 (both in this revision:
DM-R43's recorded generation, DM-R44's bounded term history and per-node gauges). Step 5's
whole-image verifier, `Vouch` answering, the mode switch, and the `verify` switch were rejected
(DM-R40 to DM-R42) and land only after this revision's design-only re-check (C33), beside the
security reviewer's C18 to C22: one alphabet rule (DMS-20), check 3's and the whole comparison's
bounds before their tables (DMS-21), the release of a pinned ceiling at the share (DMS-22), the
switch floor (DMS-23), and the straddling merge and the restart flags (DMS-24). Steps 1 and 2 gain
DM-R46's reporting, additive, which may land after either and before strata::group runs a
`written` group outside tests (C37); step 3's T6 gains C38's mutant (an install while forgetful
that drops the mark). Step 5 builds on strata `main` as re-pinned here (DM-R39), whose skip data
and threshold seeds check 5 now covers. Step 8 encodes `RFTXFR02`'s controls with the echoed
entry, the `no` routing, the second source for an unserved `Request`, the refusal of an entry whose files a source no longer holds, and
the unvouched member's refusal to serve (DMS-17; security C15).

| Step | Where | Starts | Waits for |
|---|---|---|---|
| 1. `RFTW0002`: one write in flight, effects at the barrier, the end-of-turn S, the boot identity in the open record, the clean-close record, same-boot recovery, the verdict's epoch in the open record and the mark in the hard-state and group checkpoint records (DM-R19), the open record as a live record copied forward (`0x13`) and `TurnRecovery`'s per-record epochs (the oldest readable copy's), verdict, and V (DM-R24), the marks before any other record with forward slices held until the last mark turn's write returns (`hold_forward`, DM-R29), activation, with T9's rows (D3, D4); additive after it, `TurnRecovery`'s reason for an unclean verdict and the journal directory's filesystem classification with its warning (DM-R46, C37) | `Kronuz/journal`, branch `durability-barrier` | Now (C27, approved): its turn driver and turn log land with `hold_forward` and T9's and T6's DM-R29 rows | journal's code review; Detent unaffected (named-file backend); C37's reporting before strata::group runs a `written` group outside tests |
| 2. The kernel boot identity, with "unknown" a value apart from every identity (DM-R46, C37), and a cross-IO directory barrier (D5; SI-R10) | `Kronuz/completion` | Now (C8; approved) | Additive, with Detent's suite at its commit |
| 3. `Written`, the two cursors and the site table, forgetful voters with the durable mark (`RFTBTCH3`'s byte, the bundle's) written in the epoch's first turns and read by per-group sequence (DM-R24), the rewind's floor and allowance (DMS-13) with the cap's crossing batch (DM-R27 item 4), the recovery election with the majority count (DM-R20) and `manual`'s every-voter rule (DM-R26), the `Recovery` entry under its magics, `Repair{promotion}`, the checkpoint ceiling, `Membership::acks`, `PeerRestarted`, `Unsupported`, with T6, C38's mutant included (an install while forgetful that persists its hard state without the mark) (D2, D4 to D7) | `Kronuz/consensus`, branch `durability-modes` | Now (C15, C23) | Steps 1 and 2; its restart, recovery-election, and rewind code lands only with DM-R24's mark, DM-R26's rule, and the cap (C15), and its recovery hands every mark to the driver before the epoch's first turn (C23); the full design review before it lands |
| 4. D16's version-3 wire fields, through the version range | `Kronuz/consensus`, after the peer transport's version 2 lands | Now (C9) | The full design review of D5 to D8 and D17, and the security re-check's conditions, before it lands; Detent's agreement |
| 5. strata::group: D1's checkpoints of a past generation on small-indexes' D3-A commit group with retention on the durable head (SI-R19, SI-R24's claim rule, DM-R23), the failed-checkpoint restore (SI-R20), `Create` and its replay with the first-`Create` rule (SI-R25, DMS-10), apply at `commit_memory`, `Image`, incremental install that leaves the durable head to the next checkpoint (DM-R27 item 5), D8's catch-up (merging only its own flushes, its checkpoint ceiling) and a mode switch's rule (the followers merge nothing they hold, the leader expunges first; DM-R37), D18's keyed comparisons (SipHash-2-4 in Kronuz/hashes first, its keys in a locked page excluded from dumps, DMS-18) with the sampled rebuild and the keyed install checks behind `verify` (Q9), check 3 over every ordinal of a merge output (DM-R33), checks 4 and 5 over every structure a reader consults through the dictionaries' cursors, each term's value rebuilt from the walk and both skip levels (DMS-16), the postings and positions decoders' end accessors (DM-R38), the whole-image alphabet rule (DM-R36), the whole-image rules with `Vouch` per entry and grounded images (DM-R31, DM-R34, DMS-15), `verify_failed{check}` (DMS-19), D10's levels, floors, and term history bounded at `term_history_entries` (DM-R44), the recorded generation kept by retention (DM-R43), grounding traced to an independent check (DM-R40), `grounded_by_switch` at a switch to `verify: install` (DM-R41), the switch's frozen set and ceiling, the comparison at p and the state after it, and the expunge of every pre-command delete (DM-R42, DMS-24), one alphabet rule (DMS-20), check 3's and the whole comparison's bounds (DMS-21), the release at the share (DMS-22), the switch floor (DMS-23), check 5 over `main`'s skip data and threshold seeds (DM-R39), every new gauge a count per node, with T4's forging, planted-structure, dead-ordinal, whole-image, mode-switch, vouch, and honest-control cases, T10, T11, T15, and T16 | strata, Phase 3R's branch, on strata `main` `3c86484` or later, its citations re-pinned at its start (`bench/durability/repin_citations.py`) | Phase 3R's start (C1 to C4); may start now (C11, C18) | Small-indexes' batcher (its step 6, after SI-R24) and its journaled create (SI-R25); checkpoints, `Create`, apply, `Image`, incremental install, catch-up, and levels land under C34 and C35; the whole-image verifier, `Vouch` answering, the mode switch, and the `verify` switch only after this revision's design-only re-check (C33; security C18 to C22) |
| 6. D5's fault classes with restarts that keep the mark and torn tails, restarts after a copy-forward and a reclaimed open record (DM-R24), the model statement, D4's vote barrier, D6's recovery election with the majority count and `manual`'s every-voter rule, membership changes through Track C's windows, the checker's modes with `SIX_LIAR` counted outside the model, and T3's forced schedules, `restart3`, `torn3`, `six`, `six_loss`, `grow67`, `grow67_loss`, `six_heard`, `grow67_heard`, `lead5`, `lead5_loss`, and `heard5l` among them (T3 to T5; C10, C19, C24) | `design/simulator` | Now | Nothing else |
| 7. T1 on Linux, FreeBSD, and macOS at strata `main` (DM-R39) | The lead | Now | Nothing |
| 8. D17's file-set transfer, with the list's bounds and the checked sum (DMS-12), and the control kinds `Request`, `Missing`, and `Vouch` with its four answers and echoed entry, the `no` routing, the second source for an unserved `Request`, the refusal of a superseded entry (DM-R47 item 8), and the unvouched member's refusal (DMS-17) | `Kronuz/consensus`, with step 4 | Now (C12; `Vouch` answering as step 5 rules it) | The peer transport's version 2; security C15 before `RFTXFR02`'s controls are encoded |
| 9. SipHash-2-4 in Kronuz/hashes, with the reference vectors as its test (D18) | `Kronuz/hashes` | Now: additive, no design question open | Its CI on every platform; strata's pin moves with step 5 |

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

Revision 7 complete; nothing is built beyond the probes and the models. Measured on this branch (the
development Mac, as evidence for D1 and D8's choices): T1's counts with no hold, T2's sizing under a
light hold, both at `0b407c9`; strata `main`'s create count is read from its test, not measured.
Counted here: the digest records of T2's corpus (`results/r4/records.out`). Verified by
computation: D5 and D6 in the model, with same-boot restarts, torn tails, D4's vote barrier, and
Track C's windows (D6, "Model evidence"), and the five-voter schedules under every barrier
(`results/r4/`); D3's bytes in the journal-level models, one group and several (D6, "The bytes", and
revision 4's runs); the keyed comparison against the architect's forgery; in revision 5
(`results/r5/`), check 3 over every ordinal of a merge output (`check3_model.py`), the `Vouch` rule
against its three controls, and check 5's rebuilt term values against each planted field
(`term_value_check.cc`); and in revision 7 (`results/r7/`), grounding traced to an independent
check, with every member able to catch up, and the switch to `verify: install` (`vouch_model.py`,
with the architect's `vouch_model_r6.py` beside it), and the whole comparison's documents and its
computation at p under the switch's strict readings and every mutant (`switch_model.py`). Every
other figure is the cited code's or document's, arithmetic, or a labeled estimate.

What the design-only re-check should look at first, among revision 7's changes (the rest is as the
full design reviews of revision 6 left it):

1. DM-R40: D18's grounded list (five rules, each tracing to an independent check; an image built on
   an unvouched one stays unvouched), D8's catch-up bullet, D17's `yes`, the threat row, T4's
   catch-up schedule; `vouch_model.py` and `vouch_model_r6.py` in `results/r7/`.
2. DM-R41: D15's `verify` row, D18's switch bullet (`grounded_by_switch`, the interval after the
   switch's entry), Q9's sub-question with options (a) to (c), T4's missed-plant switch.
3. DM-R42: D8's frozen set and ceiling read strictly, the state after a whole install, items (a) to
   (d) in D8's switch bullet and D15's row, D18's computation at p; `switch_model.py` in
   `results/r7/`.
4. The security reviewer's DMS-20 to DMS-24 (security C18 to C22): check 5's one alphabet rule and
   restart flags, check 3's and the whole comparison's bounds, D8's release at the share and which
   wins over Track C's forced checkpoint, D15's floor, the straddling merge.
5. DM-R39's re-pin to `main` `3c86484` (the T1 table, D1's creation, check 5's skip data in `main`'s
   layout and the threshold seeds `main` added), and DM-R43 to DM-R47.
